#pragma once

#include <cstddef>
#include <cstdint>

// Per-core preemptive dispatcher with work-stealing — replaces stage 5's
// single global round-robin scheduler entirely (there is no longer one
// global "current task"; every core has its own). See DECISIONS.md,
// stage 10, for the full design and why it replaces Phase 1's scheduler.
namespace sched {

// Every task's stack, exactly this many bytes — fixed-size, not grown on
// demand. Exposed (not just a sched.cpp-local constant) so callers that
// need to reason about a task stack's real physical footprint (e.g.
// kernel/main.cpp's oom_reaper_test, which verifies pmm's per-node
// free-page count drops/recovers by exactly this many pages' worth) don't
// have to duplicate the number.
constexpr std::size_t kTaskStackBytes = 16384;

// Registers the calling core's own current execution context as its
// permanent "idle" fallback — what that core resumes when it owns no
// ready task and stealing finds nothing either. Must be called once per
// core (BSP via kernel_main, each AP via ap_entry), before that core's
// LAPIC timer can safely deliver its first tick.
void register_this_core_idle();

// Creates a new task running entry() on its own kTaskStackBytes stack —
// kTaskStackBytes/pmm::kPageSize physically contiguous pages allocated
// directly from pmm, preferring the NUMA node local to the CALLING core
// (falling back to any node if the local one has no run that long free,
// or if no NUMA topology is known at all — see DECISIONS.md, stage 11).
// The task is initially owned by the CALLING core's run queue, so this
// preference also means "the stack is local to the core that will
// actually run it first." Must only ever be called from the BSP — task
// creation is not made safe for concurrent calls from multiple cores (it
// calls pmm::alloc_contiguous_pages[_on_node], which aren't either — see
// DECISIONS.md). Returns the task's id.
//
// If no allocation attempt succeeds for the new stack, this makes one
// attempt at graceful degradation before giving up:
// reap_finished_tasks() frees any finished-but-not-yet-reclaimed tasks'
// stacks project-wide, and the allocation is retried once. Only panics
// if it still fails after that.
int create_task(void (*entry)(), const char* name);

// Frees the stack of every task slot that has finished but hasn't been
// reclaimed yet (either by a later create_task reusing that specific
// slot, or by a previous call to this function). Safe to call any time,
// from the BSP only (see create_task). Returns how many stacks it freed.
int reap_finished_tasks();

// Called only from isr_handler for vector 32 (the calling core's own
// LAPIC timer). Saves the interrupted task's (or this core's own idle
// context's) trap-frame pointer, then: (1) round-robins among tasks this
// core already owns, if any are ready; else (2) attempts to steal
// exactly one ready task from another core's queue via a single atomic
// compare-and-swap — lock-free in the literal sense (no spinlock, no
// blocking), not a full wait-free deque (see DECISIONS.md for why that
// distinction matters and why the simpler design was chosen); else
// (3) resumes this core's own idle context. Returns the trap-frame
// pointer the assembly trampoline should resume from.
uint64_t on_timer_tick(uint64_t current_rsp);

// Global count of tasks that have run to completion, across every core —
// used by kernel_main to know when a batch of tasks is done. Safe to
// read from the BSP while other cores are running tasks: each core only
// ever increments its own slot of the underlying per-core counters.
int tasks_finished();

// While false (the default), on_timer_tick's step (2) above never
// attempts to steal — every core only ever runs tasks it already owns
// (or idles). kernel_main uses this to take a clean "before" per-core
// snapshot of a deliberately imbalanced task load before letting
// stealing actually redistribute it.
void set_stealing_enabled(bool enabled);

// How many task slots the given core currently owns and hasn't finished
// yet (ready or running). Useful for a "before" snapshot of a freshly
// created, not-yet-run batch of tasks — see kernel/main.cpp's
// smp_scheduler_demo.
int owned_task_count(uint32_t core_id);

// How many tasks the given core has run all the way to completion, ever.
// Unlike owned_task_count (which goes back to 0 once everything
// finishes), this is the real record of which core actually did the
// work — the "after" half of stage 10's per-core task-count proof.
int completed_task_count(uint32_t core_id);

// How many timer ticks the given core has taken through on_timer_tick
// (diagnostic: distinguishes "never stole anything" from "never ticked").
uint64_t tick_count(uint32_t core_id);

// Diagnostic (kept deliberately, stage 10): prints every task slot's owner,
// state, and rsp.
void debug_dump_tasks();

// Calls callback once per task slot that's ever been used (including
// finished-but-not-yet-reclaimed ones), for the shell's `tasks` command.
void for_each_task(void (*callback)(int id, const char* name, bool active));

} // namespace sched
