#pragma once

#include <cstdint>

namespace sched {

// Registers the currently-running flow (kernel_main itself) as task 0.
// Must run before creating any other task or enabling interrupts.
void init();

// Creates a new round-robin task running entry() on its own 16 KiB heap-
// allocated stack, and returns its task id. entry returning normally is
// equivalent to calling task_exit() — the task simply won't be scheduled
// again, and its slot (and stack) is reclaimed the next time create_task
// needs a free slot. Must be called before sti (task creation isn't
// synchronized against the timer ISR — see DECISIONS.md).
int create_task(void (*entry)(), const char* name);

// Called only from isr_handler for vector 32 (the timer IRQ). Saves the
// interrupted task's trap-frame pointer, round-robin-picks the next
// ACTIVE task, and returns ITS saved trap-frame pointer — which the
// assembly trampoline switches RSP to before popping registers and
// iretq-ing, actually performing the context switch.
uint64_t on_timer_tick(uint64_t current_rsp);

// How many tasks (of any kind created via create_task, not task 0) have
// run to completion so far — used by kernel_main to know when a batch of
// demo/torture tasks is done.
int tasks_finished();

// Calls callback once per task slot that's ever been used (including
// task 0 and finished-but-not-yet-reused ones), for the shell's `tasks`
// command.
void for_each_task(void (*callback)(int id, const char* name, bool active));

} // namespace sched
