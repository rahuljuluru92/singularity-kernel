#include "sched.hpp"

#include "heap.hpp"
#include "isr.hpp"
#include "lapic.hpp"
#include "serial.hpp"

#include <cstddef>

namespace sched {

namespace {

constexpr int kMaxTasks = 64;
constexpr int kMaxCores = 64; // matches acpi::MadtParseResult::kMaxLapics
constexpr size_t kStackSize = 16384;
constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
constexpr uint64_t kInterruptsEnabledFlag = 0x202; // reserved bit 1 + IF

// A task's lifecycle: kFree (slot never used, or fully reclaimed) ->
// kReady (sitting with a valid saved rsp) -> kRunning (currently
// executing on owner_core) -> kFinished (task_exit() ran; its stack is
// awaiting reclamation) -> back to kFree once reclaimed.
constexpr uint32_t kFree = 0;
constexpr uint32_t kReady = 1;
constexpr uint32_t kRunning = 2;
constexpr uint32_t kFinished = 3;

struct Task {
    uint64_t rsp;
    uint8_t* stack;
    const char* name;
    int32_t owner_core;
    uint32_t state;
};

Task g_tasks[kMaxTasks];
int g_task_count = 0; // high-water mark of slots ever used

// Per-core state -- each array index is a core's own x2APIC id (see
// DECISIONS.md: this project's QEMU target always gives small, dense,
// contiguous APIC ids matching -smp's core count, so using the id
// directly as an array index needs no separate core-id-to-index table).
uint64_t g_idle_rsp[kMaxCores];
int g_current_index[kMaxCores]; // -1 = currently running this core's own idle context
int g_completed_by_core[kMaxCores];
uint64_t g_tick_count[kMaxCores];

volatile bool g_stealing_enabled = false;

// See kernel/isr_stubs.asm — the same pop+iretq tail isr_common_stub
// uses to resume a trap frame, exposed as a callable entry point for
// normal (non-interrupt) code. task_exit() uses this to hand off
// directly to whatever runs next, with no intermediate stack at all.
extern "C" [[noreturn]] void resume_trap_frame(uint64_t rsp);

// EVERY access to g_tasks/g_task_count/g_current_index/g_idle_rsp/
// g_completed_by_core, from ANY core, in ANY context (interrupt or
// normal), goes through this one global spinlock. This replaces an
// earlier lock-free (single-CAS) design that hit a real, persistent,
// intermittent corruption bug under genuine concurrent execution —
// extensively debugged (see ASSUMPTIONS.md's Session 10 entry: ruled
// out work-stealing itself, core count, codegen/CAS correctness, and
// heap aliasing as the cause without finding the actual root cause) —
// and pragmatically replaced with this much simpler, trivially-correct
// design instead of continuing to chase it. The performance cost is
// negligible at this project's scale: a handful of cores, a 100 Hz
// tick, and an O(64) scan held under the lock for at most a few
// microseconds. See DECISIONS.md, stage 10.
//
// Deadlock-safety: every caller that can hold this lock from NORMAL
// (non-interrupt) code — create_task, reap_finished_tasks, task_exit —
// also holds cli across the entire critical section, so this core's own
// timer can never fire (and try to re-enter this same lock) while this
// core already holds it. A different core spinning on this lock during
// that window just waits — its own pending tick is not lost, merely
// delayed until it acquires the lock or its own critical section ends.
volatile int g_sched_lock = 0;

void lock_sched() {
    while (__atomic_exchange_n(&g_sched_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("pause");
    }
}

void unlock_sched() { __atomic_store_n(&g_sched_lock, 0, __ATOMIC_RELEASE); }

// For every caller running in NORMAL (non-interrupt) context with
// interrupts possibly enabled: takes the lock with this core's own
// interrupts held off for the duration. Without this, the core's own
// timer tick can fire while it holds g_sched_lock, and on_timer_tick
// then spins forever on a lock its own core holds — a self-deadlock.
// (Only on_timer_tick itself, already inside an interrupt gate with IF
// cleared, may use plain lock_sched.)
uint64_t lock_sched_irqsave() {
    uint64_t flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(flags)::"memory");
    lock_sched();
    return flags;
}

void unlock_sched_irqrestore(uint64_t flags) {
    unlock_sched();
    asm volatile("push %0\n\tpopfq" : : "r"(flags) : "memory", "cc");
}

// Diagnostic-only (stage 10 debugging, kept deliberately — see
// DECISIONS.md): a task about to be resumed must have a sane, fixed
// selector in its saved trap frame. Called only while g_sched_lock is
// already held. If this ever fires again after the redesign to a single
// global lock, it means the bug wasn't actually a synchronization race
// in the scheduler at all, and points at something else entirely
// (create_task's frame construction, the heap, or memory corruption
// from an unrelated subsystem) — valuable information either way.
void sanity_check_or_panic(int idx, uint32_t claiming_core, uint64_t rsp) {
    auto* r = reinterpret_cast<registers*>(rsp);
    if (r->cs == kKernelCodeSelector && r->ss == kKernelDataSelector) {
        return;
    }
    serial::lock();
    serial::puts("[PANIC] sched: claim sanity check failed core=");
    serial::put_hex(claiming_core);
    serial::puts(" idx=");
    serial::put_hex(static_cast<uint64_t>(idx));
    serial::puts(" rsp=");
    serial::put_hex(rsp);
    serial::puts(" cs=");
    serial::put_hex(r->cs);
    serial::puts(" ss=");
    serial::put_hex(r->ss);
    serial::puts(" rip=");
    serial::put_hex(r->rip);
    serial::puts("\r\n");
    serial::unlock();
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

[[noreturn]] void panic(const char* msg) {
    serial::lock();
    serial::puts("[PANIC] sched: ");
    serial::puts(msg);
    serial::puts("\r\n");
    serial::unlock();
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

// The single source of "what should this core run next" — round-robin
// among tasks it already owns, then (if stealing is enabled) anyone
// else's ready work, then its own idle context. Must be called with
// g_sched_lock already held, and updates g_current_index[core] itself.
// Shared by on_timer_tick (after saving the just-interrupted context)
// and task_exit (which has nothing of its own left to save).
uint64_t pick_next_locked(uint32_t core) {
    int cur = g_current_index[core];
    int start = (cur < 0) ? 0 : cur;

    for (int i = 1; i <= kMaxTasks; i++) {
        int idx = (start + i) % kMaxTasks;
        if (g_tasks[idx].owner_core == static_cast<int32_t>(core) && g_tasks[idx].state == kReady) {
            g_tasks[idx].state = kRunning;
            g_current_index[core] = idx;
            uint64_t rsp = g_tasks[idx].rsp;
            sanity_check_or_panic(idx, core, rsp);
            return rsp;
        }
    }

    if (g_stealing_enabled) {
        for (int idx = 0; idx < kMaxTasks; idx++) {
            if (g_tasks[idx].owner_core == static_cast<int32_t>(core)) {
                continue; // already tried these, above
            }
            if (g_tasks[idx].state == kReady) {
                g_tasks[idx].state = kRunning;
                g_tasks[idx].owner_core = static_cast<int32_t>(core);
                g_current_index[core] = idx;
                uint64_t rsp = g_tasks[idx].rsp;
                sanity_check_or_panic(idx, core, rsp);
                return rsp;
            }
        }
    }

    g_current_index[core] = -1;
    return g_idle_rsp[core];
}

// Runs on whichever core is executing the task at the moment its entry
// function returns. Never returns itself. Hands off DIRECTLY to
// whatever runs next (another ready/stolen task, or this core's own
// idle context) via resume_trap_frame — no intermediate "parked" stack,
// and no waiting for the next hardware tick. An earlier version parked
// on a small per-core scratch stack until the next tick noticed
// kFinished; that turned out to have a real timing window (see
// DECISIONS.md, stage 10) — removing the intermediate step removes the
// whole window, not just this one manifestation of it.
[[noreturn]] static void task_exit() {
    asm volatile("cli");

    uint32_t core = static_cast<uint32_t>(lapic::id());
    lock_sched();
    int idx = g_current_index[core];
    g_tasks[idx].state = kFinished;
    g_completed_by_core[core]++;
    uint64_t next_rsp = pick_next_locked(core);
    unlock_sched();

    resume_trap_frame(next_rsp);
}

} // namespace

void register_this_core_idle() {
    uint32_t core = static_cast<uint32_t>(lapic::id());
    if (core >= kMaxCores) {
        panic("register_this_core_idle: APIC id exceeds kMaxCores");
    }
    uint64_t flags = lock_sched_irqsave();
    g_current_index[core] = -1;
    g_completed_by_core[core] = 0;
    unlock_sched_irqrestore(flags);
}

int reap_finished_tasks() {
    uint64_t saved_flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(saved_flags)::"memory");
    lock_sched();

    int freed = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == kFinished && g_tasks[i].stack != nullptr) {
            heap::kfree(g_tasks[i].stack);
            g_tasks[i].stack = nullptr;
            g_tasks[i].state = kFree;
            freed++;
        }
    }

    unlock_sched();
    asm volatile("push %0\n\tpopfq" : : "r"(saved_flags) : "memory", "cc");
    return freed;
}

int create_task(void (*entry)(), const char* name) {
    uint64_t saved_flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(saved_flags)::"memory");
    lock_sched();

    int id = -1;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == kFree || g_tasks[i].state == kFinished) {
            id = i;
            break;
        }
    }
    if (id == -1) {
        if (g_task_count >= kMaxTasks) {
            panic("create_task: out of task slots");
        }
        id = g_task_count++;
    }

    if (g_tasks[id].state == kFinished && g_tasks[id].stack != nullptr) {
        // Reusing a finished task's slot: safe here specifically because
        // a finished task is never resumed or touched by anyone again.
        heap::kfree(g_tasks[id].stack);
        g_tasks[id].stack = nullptr;
    }

    auto* stack = static_cast<uint8_t*>(heap::kmalloc(kStackSize));
    if (stack == nullptr) {
        // Graceful degradation, not an immediate panic: reclaim whatever
        // finished tasks project-wide haven't been cleaned up yet
        // (safe to call while already holding the lock — reap_finished_
        // tasks acquires it too, but this is the one legitimate nested
        // acquisition in this file, so lock_sched must be re-entrant...
        // it isn't, so inline the sweep here directly instead).
        for (int i = 0; i < g_task_count; i++) {
            if (g_tasks[i].state == kFinished && g_tasks[i].stack != nullptr) {
                heap::kfree(g_tasks[i].stack);
                g_tasks[i].stack = nullptr;
                g_tasks[i].state = kFree;
            }
        }
        stack = static_cast<uint8_t*>(heap::kmalloc(kStackSize));
    }
    if (stack == nullptr) {
        panic("create_task: kmalloc failed even after reaping finished tasks");
    }

    uintptr_t top = reinterpret_cast<uintptr_t>(stack + kStackSize) & ~0xFULL;
    auto* regs = reinterpret_cast<registers*>(top - sizeof(registers));
    *regs = registers{};
    regs->rdi = reinterpret_cast<uint64_t>(entry); // see the trampoline lambda below
    regs->rip = reinterpret_cast<uint64_t>(+[](void (*fn)()) {
        fn();
        task_exit();
    });
    regs->cs = kKernelCodeSelector;
    regs->rflags = kInterruptsEnabledFlag;
    regs->rsp = top;
    regs->ss = kKernelDataSelector;

    g_tasks[id] = Task{
        .rsp = reinterpret_cast<uint64_t>(regs),
        .stack = stack,
        .name = name,
        .owner_core = static_cast<int32_t>(lapic::id()),
        .state = kReady,
    };

    unlock_sched();
    asm volatile("push %0\n\tpopfq" : : "r"(saved_flags) : "memory", "cc");
    return id;
}

void set_stealing_enabled(bool enabled) { g_stealing_enabled = enabled; }

uint64_t on_timer_tick(uint64_t current_rsp) {
    uint32_t core = static_cast<uint32_t>(lapic::id());

    lock_sched();
    g_tick_count[core]++;

    int cur = g_current_index[core];
    if (cur < 0) {
        g_idle_rsp[core] = current_rsp;
    } else if (g_tasks[cur].state == kRunning) {
        // Only save-as-ready if it's still genuinely ours to save --
        // task_exit() may have already moved it straight to kFinished
        // before this tick landed, in which case there's nothing to
        // resume and nothing to save.
        g_tasks[cur].rsp = current_rsp;
        g_tasks[cur].state = kReady;
    }

    uint64_t rsp = pick_next_locked(core);
    unlock_sched();
    return rsp;
}

void debug_dump_tasks() {
    uint64_t flags = lock_sched_irqsave();
    serial::lock();
    serial::puts("[DEBUG] task dump, g_task_count=");
    serial::put_hex(static_cast<uint64_t>(g_task_count));
    serial::puts("\r\n");
    for (int i = 0; i < g_task_count; i++) {
        serial::puts("  idx=");
        serial::put_hex(static_cast<uint64_t>(i));
        serial::puts(" owner=");
        serial::put_hex(static_cast<uint64_t>(static_cast<uint32_t>(g_tasks[i].owner_core)));
        serial::puts(" state=");
        serial::put_hex(static_cast<uint64_t>(g_tasks[i].state));
        serial::puts(" rsp=");
        serial::put_hex(g_tasks[i].rsp);
        serial::puts("\r\n");
    }
    serial::unlock();
    unlock_sched_irqrestore(flags);
}

int tasks_finished() {
    uint64_t flags = lock_sched_irqsave();
    int total = 0;
    for (int i = 0; i < kMaxCores; i++) {
        total += g_completed_by_core[i];
    }
    unlock_sched_irqrestore(flags);
    return total;
}

uint64_t tick_count(uint32_t core_id) {
    if (core_id >= kMaxCores) {
        return 0;
    }
    uint64_t flags = lock_sched_irqsave();
    uint64_t n = g_tick_count[core_id];
    unlock_sched_irqrestore(flags);
    return n;
}

int completed_task_count(uint32_t core_id) {
    if (core_id >= kMaxCores) {
        return 0;
    }
    uint64_t flags = lock_sched_irqsave();
    int n = g_completed_by_core[core_id];
    unlock_sched_irqrestore(flags);
    return n;
}

int owned_task_count(uint32_t core_id) {
    uint64_t flags = lock_sched_irqsave();
    int count = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].owner_core == static_cast<int32_t>(core_id) &&
            (g_tasks[i].state == kReady || g_tasks[i].state == kRunning)) {
            count++;
        }
    }
    unlock_sched_irqrestore(flags);
    return count;
}

void for_each_task(void (*callback)(int id, const char* name, bool active)) {
    uint64_t flags = lock_sched_irqsave();
    int count = g_task_count;
    unlock_sched_irqrestore(flags);
    for (int i = 0; i < count; i++) {
        flags = lock_sched_irqsave();
        bool active = g_tasks[i].state == kReady || g_tasks[i].state == kRunning;
        const char* name = g_tasks[i].name;
        unlock_sched_irqrestore(flags);
        callback(i, name, active);
    }
}

} // namespace sched
