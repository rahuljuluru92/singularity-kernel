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
// kReady (sitting with a valid saved rsp, safe for its owning core to
// resume OR for another core to steal) -> kRunning (currently executing
// on owner_core, NOT stealable -- the invariant that makes stealing
// safe) -> kFinished (task_exit() ran; owns nothing, resumed by no one,
// its stack awaiting reclamation) -> back to kFree once reclaimed.
constexpr uint32_t kFree = 0;
constexpr uint32_t kReady = 1;
constexpr uint32_t kRunning = 2;
constexpr uint32_t kFinished = 3;

struct Task {
    uint64_t rsp;
    uint8_t* stack;
    const char* name;
    volatile int32_t owner_core;
    volatile uint32_t state;
};

Task g_tasks[kMaxTasks];
int g_task_count = 0; // high-water mark of slots ever used (BSP-only, sequential)

// Per-core state -- each array index is a core's own x2APIC id (see
// DECISIONS.md: this project's QEMU target always gives small, dense,
// contiguous APIC ids matching -smp's core count, so using the id
// directly as an array index needs no separate core-id-to-index table).
uint64_t g_idle_rsp[kMaxCores];
int g_current_index[kMaxCores]; // -1 = currently running this core's own idle context
int g_completed_by_core[kMaxCores]; // only ever written by that one core

volatile bool g_stealing_enabled = false;

// A finished task's own stack becomes unsafe to free the instant it's
// marked kFinished, NOT only once some other core actually reuses the
// slot: with IST=0 (no automatic stack switch on interrupt — see
// kernel/idt.cpp), a core sitting in task_exit()'s halt loop is still
// using that task's stack as its live RSP, and the NEXT hardware
// interrupt on that core would push its trap frame onto it. If the BSP's
// create_task() frees and reuses that same memory for a new task in the
// meantime, the two writes race for real — a genuine, reproduced bug
// (see DECISIONS.md, stage 10) that manifested as iretq faulting on a
// corrupted trap frame. Fixed by having every core immediately switch
// off a finished task's stack onto its own small, dedicated, never-freed
// scratch stack before ever halting — one per core, reused every time
// (never concurrently, since a core only ever runs one task at a time).
constexpr size_t kExitScratchStackSize = 256;
alignas(16) uint8_t g_exit_scratch_stack[kMaxCores][kExitScratchStackSize];

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

// The ONLY safe way for any core to start (or resume) running a Ready
// task, whether it's "resuming its own work" or "stealing someone
// else's" — those are just two different SEARCH ORDERS over candidate
// slots (see on_timer_tick), not two different safety mechanisms. A real
// bug this project hit and fixed (see DECISIONS.md, stage 10): an
// earlier version let a core's own-queue pass claim a Ready slot via a
// plain, non-atomic check-then-set, while only the steal pass used an
// atomic CAS — meaning a core resuming "its own" task and a different
// core stealing that exact same task could BOTH win, both resume the
// same stack concurrently, and corrupt it. Every claim, regardless of
// current ownership, goes through this one atomic compare-and-swap.
bool try_claim(int idx, uint32_t claiming_core) {
    uint32_t expected = kReady;
    if (__atomic_compare_exchange_n(&g_tasks[idx].state, &expected, kRunning, false,
                                     __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
        g_tasks[idx].owner_core = static_cast<int32_t>(claiming_core);

        // Diagnostic-only sanity check (stage 10 debugging): a claimed
        // task's saved trap frame must have sane, fixed selector values.
        // If it doesn't, something corrupted this slot's rsp/stack
        // BEFORE we ever got to iretq — panic here, with the exact
        // claim details, instead of downstream at a confusing #GP.
        auto* r = reinterpret_cast<registers*>(g_tasks[idx].rsp);
        if (r->cs != kKernelCodeSelector || r->ss != kKernelDataSelector) {
            serial::lock();
            serial::puts("[PANIC] sched: claim sanity check failed core=");
            serial::put_hex(claiming_core);
            serial::puts(" idx=");
            serial::put_hex(static_cast<uint64_t>(idx));
            serial::puts(" rsp=");
            serial::put_hex(g_tasks[idx].rsp);
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
        return true;
    }
    return false;
}

// Runs on whichever core is executing the task at the moment its entry
// function returns. Never returns itself. Does NOT halt forever on the
// task's OWN stack the way stage 5's version did (see the
// g_exit_scratch_stack comment above for why that's unsafe here) --
// immediately switches to this core's dedicated scratch stack, with
// interrupts held off across the transition so no interrupt can ever be
// taken while still on the about-to-be-abandoned stack. Once on the
// scratch stack, parks for at most one tick so the very next timer tick
// notices kFinished and reschedules this core onto something else.
[[noreturn]] static void task_exit() {
    asm volatile("cli");

    uint32_t core = static_cast<uint32_t>(lapic::id());
    int idx = g_current_index[core];
    g_tasks[idx].state = kFinished;
    g_completed_by_core[core]++;

    uint64_t scratch_top =
        reinterpret_cast<uint64_t>(&g_exit_scratch_stack[core][kExitScratchStackSize]);
    asm volatile("mov %0, %%rsp\n\t"
                 "1: sti\n\t"
                 "hlt\n\t"
                 "jmp 1b\n\t"
                 :
                 : "r"(scratch_top)
                 : "memory");
    __builtin_unreachable();
}

} // namespace

void register_this_core_idle() {
    uint32_t core = static_cast<uint32_t>(lapic::id());
    if (core >= kMaxCores) {
        panic("register_this_core_idle: APIC id exceeds kMaxCores");
    }
    g_current_index[core] = -1;
    g_completed_by_core[core] = 0;
}

int reap_finished_tasks() {
    // BSP-only, but must still be atomic w.r.t. this core's OWN
    // scheduler: without this, the BSP's own timer tick could preempt it
    // mid-sweep to go run some ready task, and (worse) create_task below
    // relies on calling this safely from inside its own critical
    // section. See create_task's comment for the concrete bug this
    // guards against.
    uint64_t saved_flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(saved_flags)::"memory");

    int freed = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].state == kFinished && g_tasks[i].stack != nullptr) {
            heap::kfree(g_tasks[i].stack);
            g_tasks[i].stack = nullptr;
            g_tasks[i].state = kFree;
            freed++;
        }
    }

    asm volatile("push %0\n\tpopfq" : : "r"(saved_flags) : "memory", "cc");
    return freed;
}

// Must run with interrupts held off for its full duration, not just
// around individual field writes: a real, reproduced bug (see
// DECISIONS.md, stage 10) came from this exact function running on the
// BSP with interrupts enabled — the BSP's own timer tick could fire
// mid-call and switch the BSP away to go run one of its OWN
// already-created, already-ready tasks (pass 1 isn't gated by
// set_stealing_enabled — only pass 2, stealing FROM OTHER cores, is),
// resuming create_task later mid-construction of a DIFFERENT task's fake
// frame. Nothing here is safe to interleave with the scheduler picking
// this same core's own newly-created work back up.
int create_task(void (*entry)(), const char* name) {
    uint64_t saved_flags;
    asm volatile("pushfq\n\tpop %0\n\tcli" : "=r"(saved_flags)::"memory");

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
        // create_task only ever runs on the BSP, and a finished task is
        // never resumed or touched by anyone again once kFinished.
        heap::kfree(g_tasks[id].stack);
        g_tasks[id].stack = nullptr;
    }

    auto* stack = static_cast<uint8_t*>(heap::kmalloc(kStackSize));
    if (stack == nullptr) {
        // Graceful degradation, not an immediate panic: reclaim whatever
        // finished tasks project-wide haven't been cleaned up yet, and
        // try exactly once more. See DECISIONS.md, stage 10.
        reap_finished_tasks();
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

    asm volatile("push %0\n\tpopfq" : : "r"(saved_flags) : "memory", "cc");
    return id;
}

void set_stealing_enabled(bool enabled) { g_stealing_enabled = enabled; }

uint64_t on_timer_tick(uint64_t current_rsp) {
    uint32_t core = static_cast<uint32_t>(lapic::id());
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

    // Pass 1: round-robin among tasks this core already owns, PREFERRING
    // its own work over someone else's -- but the actual claim still
    // goes through try_claim's atomic CAS, exactly like a steal, because
    // "I own this" is only ever a scheduling preference here, never a
    // safety guarantee by itself (see try_claim's comment for the real
    // bug that taught us this).
    int start = (cur < 0) ? 0 : cur;
    for (int i = 1; i <= kMaxTasks; i++) {
        int idx = (start + i) % kMaxTasks;
        if (g_tasks[idx].owner_core == static_cast<int32_t>(core) &&
            g_tasks[idx].state == kReady && try_claim(idx, core)) {
            g_current_index[core] = idx;
            return g_tasks[idx].rsp;
        }
    }

    // Pass 2: nothing of our own claimed -- try to steal exactly one
    // ready task from anyone else. Lock-free: a single atomic
    // compare-and-swap per candidate, no spinlock, no blocking. If we
    // lose a race (another core claimed it first), we just move on to
    // the next candidate rather than retrying the same one. See
    // DECISIONS.md for why this is "lock-free" in the literal sense but
    // not a full wait-free deque.
    if (g_stealing_enabled) {
        for (int idx = 0; idx < kMaxTasks; idx++) {
            if (g_tasks[idx].owner_core == static_cast<int32_t>(core)) {
                continue; // already tried these, in pass 1
            }
            if (g_tasks[idx].state == kReady && try_claim(idx, core)) {
                g_current_index[core] = idx;
                return g_tasks[idx].rsp;
            }
        }
    }

    // Nothing owned, nothing stealable -- park in this core's own idle
    // context until the next tick.
    g_current_index[core] = -1;
    return g_idle_rsp[core];
}

void debug_dump_tasks() {
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
}

int tasks_finished() {
    int total = 0;
    for (int i = 0; i < kMaxCores; i++) {
        total += g_completed_by_core[i];
    }
    return total;
}

int completed_task_count(uint32_t core_id) {
    if (core_id >= kMaxCores) {
        return 0;
    }
    return g_completed_by_core[core_id];
}

int owned_task_count(uint32_t core_id) {
    int count = 0;
    for (int i = 0; i < g_task_count; i++) {
        if (g_tasks[i].owner_core == static_cast<int32_t>(core_id) &&
            (g_tasks[i].state == kReady || g_tasks[i].state == kRunning)) {
            count++;
        }
    }
    return count;
}

void for_each_task(void (*callback)(int id, const char* name, bool active)) {
    for (int i = 0; i < g_task_count; i++) {
        bool active = g_tasks[i].state == kReady || g_tasks[i].state == kRunning;
        callback(i, g_tasks[i].name, active);
    }
}

} // namespace sched
