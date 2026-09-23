#include "sched.hpp"

#include "heap.hpp"
#include "isr.hpp"
#include "serial.hpp"

#include <cstddef>

namespace sched {

namespace {

constexpr int kMaxTasks = 64;
constexpr size_t kStackSize = 16384;
constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint16_t kKernelDataSelector = 0x10;
constexpr uint64_t kInterruptsEnabledFlag = 0x202; // reserved bit 1 + IF

struct Task {
    uint64_t rsp;
    uint8_t* stack;
    bool active;
    const char* name;
};

Task g_tasks[kMaxTasks];
int g_task_count = 0; // high-water mark of slots ever used (1..kMaxTasks)
int g_current = 0;
int g_tasks_finished = 0;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] sched: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

} // namespace

void init() {
    g_tasks[0] = Task{0, nullptr, true, "main"};
    g_task_count = 1;
    g_current = 0;
}

// Called when a task's entry function returns. Never returns itself —
// marks the slot reclaimable and idles (with interrupts enabled) until
// the next timer tick switches away from it for good; task_count's
// round-robin search skips inactive slots from then on.
[[noreturn]] static void task_exit() {
    g_tasks[g_current].active = false;
    g_tasks_finished++;
    for (;;) {
        asm volatile("sti\n\thlt");
    }
}

int create_task(void (*entry)(), const char* name) {
    int id = -1;
    for (int i = 1; i < g_task_count; i++) {
        if (!g_tasks[i].active) {
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

    if (g_tasks[id].stack != nullptr) {
        // Reusing a finished task's slot: safe to free its old stack
        // here specifically because we're running as whatever called
        // create_task (kernel_main), never as the finished task itself
        // — it stopped being scheduled the moment it set active=false.
        heap::kfree(g_tasks[id].stack);
    }

    auto* stack = static_cast<uint8_t*>(heap::kmalloc(kStackSize));
    if (stack == nullptr) {
        panic("create_task: kmalloc failed");
    }

    uintptr_t top = reinterpret_cast<uintptr_t>(stack + kStackSize) & ~0xFULL;
    auto* regs = reinterpret_cast<registers*>(top - sizeof(registers));
    *regs = registers{};
    regs->rdi = reinterpret_cast<uint64_t>(entry); // see task_trampoline
    regs->rip = reinterpret_cast<uint64_t>(+[](void (*fn)()) {
        fn();
        task_exit();
    });
    regs->cs = kKernelCodeSelector;
    regs->rflags = kInterruptsEnabledFlag;
    regs->rsp = top;
    regs->ss = kKernelDataSelector;

    g_tasks[id] = Task{reinterpret_cast<uint64_t>(regs), stack, true, name};
    return id;
}

uint64_t on_timer_tick(uint64_t current_rsp) {
    g_tasks[g_current].rsp = current_rsp;

    int next = g_current;
    for (int i = 0; i < g_task_count; i++) {
        next = (next + 1) % g_task_count;
        if (g_tasks[next].active) {
            break;
        }
    }
    g_current = next;
    return g_tasks[g_current].rsp;
}

int tasks_finished() { return g_tasks_finished; }

} // namespace sched
