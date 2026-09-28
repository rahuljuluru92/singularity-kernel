#include "acpi.hpp"
#include "efi.hpp"
#include "ioapic.hpp"
#include "lapic.hpp"
#include "smp.hpp"
#include "fat32.hpp"
#include "gdt.hpp"
#include "heap.hpp"
#include "idt.hpp"
#include "isr.hpp"
#include "paging.hpp"
#include "pic.hpp"
#include "pit.hpp"
#include "pmm.hpp"
#include "sched.hpp"
#include "serial.hpp"
#include "shell.hpp"
#include "virtio_blk.hpp"

namespace {

// Deterministic, not truly random — no <random> here, and a fixed seed
// keeps the boot-test outcome reproducible run to run rather than flaky.
struct Xorshift32 {
    uint32_t state;
    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
};

[[noreturn]] void panic(const char* msg) {
    serial::lock();
    serial::puts("[PANIC] ");
    serial::puts(msg);
    serial::puts("\r\n");
    serial::unlock();
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

// Allocate/free torture test for the physical page allocator: several
// cycles of grabbing a batch of pages and releasing them, checking the
// free count returns to the same baseline every time (no leak, no
// double-count) and that every page handed out in a batch is distinct
// (no double-allocation).
void pmm_torture_test() {
    constexpr int kBatch = 200;
    constexpr int kCycles = 50;

    std::size_t baseline = pmm::free_pages();
    for (int cycle = 0; cycle < kCycles; cycle++) {
        std::uintptr_t pages[kBatch];
        for (int i = 0; i < kBatch; i++) {
            pages[i] = pmm::alloc_page();
            if (pages[i] == 0) {
                panic("pmm torture: alloc_page failed");
            }
            for (int j = 0; j < i; j++) {
                if (pages[j] == pages[i]) {
                    panic("pmm torture: alloc_page returned a duplicate");
                }
            }
        }
        for (int i = 0; i < kBatch; i++) {
            pmm::free_page(pages[i]);
        }
        if (pmm::free_pages() != baseline) {
            panic("pmm torture: free_pages count drifted after a cycle");
        }
    }
    serial::puts("[OK] pmm torture\r\n");
}

// Deliberate divide-by-zero (stage 2's original self-test, now using
// isr::expect_fault so it can resume instead of halting — stage 3 has
// more to do afterward). volatile forces a real runtime idiv; without it
// the compiler could treat division-by-a-known-zero as UB and discard it.
void divide_by_zero_test() {
    isr::expect_fault(0, reinterpret_cast<uint64_t>(&&resume));
    volatile int zero = 0;
    volatile int result = 10 / zero;
    (void)result;
resume:
    serial::puts("[OK] exception caught\r\n");
}

// Attempt to execute a non-executable (NX) page and confirm it faults —
// the actual proof W^X is enforced, not just configured. A `jmp` (not a
// call) into the bad page: a call would push a return address before the
// instruction-fetch fault fires, leaving the stack unbalanced relative to
// what resuming at `resume:` expects; jmp has no such stack effect.
void wx_enforcement_test() {
    static const uint8_t harmless_but_forbidden_code[] = {0xC3}; // `ret`

    isr::expect_fault(14, reinterpret_cast<uint64_t>(&&resume));
    uintptr_t target = reinterpret_cast<uintptr_t>(harmless_but_forbidden_code);
    asm volatile("jmp *%0" : : "r"(target));
resume:
    constexpr uint64_t kInstructionFetch = 1u << 4; // #PF error code bit 4 (I/D)
    if ((isr::last_error_code & kInstructionFetch) == 0) {
        panic("W^X test: fault happened but wasn't an instruction fetch");
    }
    serial::puts("[OK] w^x enforced\r\n");
}

// Mixed-size, interleaved-free heap torture test: up to kSlots concurrent
// live allocations, each iteration either allocates a new one (varied
// size, written with a unique byte pattern) into a free slot or frees an
// occupied one — after first re-checking its pattern is still intact, the
// actual proof of no corruption, not just "the allocator didn't crash".
void heap_torture_test() {
    constexpr int kSlots = 64;
    constexpr int kIterations = 2000;

    void* ptrs[kSlots] = {};
    size_t sizes[kSlots] = {};
    uint8_t patterns[kSlots] = {};

    auto verify_and_clear = [&](int slot) {
        auto* bytes = reinterpret_cast<uint8_t*>(ptrs[slot]);
        for (size_t i = 0; i < sizes[slot]; i++) {
            if (bytes[i] != static_cast<uint8_t>(patterns[slot] + i)) {
                panic("heap torture: data corruption detected");
            }
        }
        heap::kfree(ptrs[slot]);
        ptrs[slot] = nullptr;
    };

    Xorshift32 rng{12345};
    for (int iter = 0; iter < kIterations; iter++) {
        int slot = static_cast<int>(rng.next() % kSlots);
        if (ptrs[slot] == nullptr) {
            size_t size = 16 + (rng.next() % 1009);
            void* p = heap::kmalloc(size);
            if (p == nullptr) {
                panic("heap torture: kmalloc failed");
            }
            uint8_t pattern = static_cast<uint8_t>(rng.next());
            auto* bytes = reinterpret_cast<uint8_t*>(p);
            for (size_t i = 0; i < size; i++) {
                bytes[i] = static_cast<uint8_t>(pattern + i);
            }
            ptrs[slot] = p;
            sizes[slot] = size;
            patterns[slot] = pattern;
        } else {
            verify_and_clear(slot);
        }
    }

    for (int i = 0; i < kSlots; i++) {
        if (ptrs[i] != nullptr) {
            verify_and_clear(i);
        }
    }

    serial::puts("[OK] heap torture\r\n");
}

// A few million empty iterations is enough wall-clock time in QEMU TCG
// to span several 100 Hz (10 ms) timer ticks — deliberately long enough
// that a demo task gets genuinely preempted mid-task by another one,
// rather than finishing all its work inside a single tick (which would
// make the serial log look sequential even though the scheduler really
// is round-robining).
void spin() {
    for (volatile int i = 0; i < 3000000; i = i + 1) {
    }
}

volatile int g_smp_demo_counter = 0;
volatile int g_smp_demo_started = 0; // TEMP DIAGNOSTIC

// Short-lived on purpose: a small amount of busy work, one atomic
// increment of a shared counter, then returns (task_exit via the
// sched.cpp trampoline). Unlike stage 5's single-core version of this
// same idea, `lock` genuinely matters here now: this counter is
// incremented by whichever core happens to be running this task at the
// time, and a plain (unlocked) `incl` only guarantees atomicity against
// same-core preemption, not against a DIFFERENT core's concurrent
// read-modify-write to the same cache line. `lock incl` is a real
// cross-core interlocked operation.
void smp_demo_task() {
    asm volatile("lock incl %0" : "+m"(g_smp_demo_started)); // TEMP DIAGNOSTIC
    spin();
    asm volatile("lock incl %0" : "+m"(g_smp_demo_counter));
}

// Stage 10's actual success criterion, in one function: creates a
// deliberately imbalanced load (every task initially owned by the BSP),
// snapshots per-core ownership before any stealing is allowed, then lets
// idle cores actually redistribute the work and snapshots per-core
// COMPLETION counts afterward — panicking (not just printing and hoping
// a human notices) if the "after" picture doesn't show real migration,
// or if the shared counter doesn't land on exactly the expected total
// despite genuinely concurrent, cross-core execution.
void smp_scheduler_demo() {
    constexpr int kTaskCount = 40;
    int core_count = static_cast<int>(acpi::madt().lapic_count);
    g_smp_demo_counter = 0;

    int start = sched::tasks_finished();
    // The whole batch, not just each individual create_task call, must
    // run with the BSP's own interrupts held off: create_task's own
    // internal critical section only stops the BSP from being preempted
    // MID-CALL, but with the BSP's timer left enabled BETWEEN calls, a
    // real bug (see DECISIONS.md, stage 10) let the BSP's own pass-1 run
    // an already-created task to completion partway through this exact
    // loop, making create_task's LATER calls reuse that slot's stack
    // while stealing was already quietly happening in the gaps — never
    // the clean, fully-imbalanced starting point this demo means to
    // create. Holding cli for the whole batch guarantees all kTaskCount
    // tasks exist, fresh, untouched, before anything can run any of them.
    asm volatile("cli");
    for (int i = 0; i < kTaskCount; i++) {
        sched::create_task(smp_demo_task, "smp-demo");
    }
    asm volatile("sti");

    serial::puts("[OK] smp before:");
    for (int core = 0; core < core_count; core++) {
        serial::puts(" core");
        serial::put_hex(static_cast<uint64_t>(core));
        serial::puts("=");
        serial::put_hex(static_cast<uint64_t>(sched::owned_task_count(static_cast<uint32_t>(core))));
    }
    serial::puts("\r\n");

    sched::set_stealing_enabled(true);

    while (sched::tasks_finished() < start + kTaskCount) {
        asm volatile("hlt");
    }

    if (g_smp_demo_counter != kTaskCount) {
        serial::puts("[DEBUG] started=");
        serial::put_hex(static_cast<uint64_t>(g_smp_demo_started));
        serial::puts(" counter=");
        serial::put_hex(static_cast<uint64_t>(g_smp_demo_counter));
        serial::puts(" expected=");
        serial::put_hex(static_cast<uint64_t>(kTaskCount));
        serial::puts("\r\n");
        sched::debug_dump_tasks();
        panic("smp scheduler: counter mismatch — lost or duplicated a task across cores");
    }

    serial::puts("[OK] smp after:");
    int cores_with_completions = 0;
    for (int core = 0; core < core_count; core++) {
        int completed = sched::completed_task_count(static_cast<uint32_t>(core));
        if (completed > 0) {
            cores_with_completions++;
        }
        serial::puts(" core");
        serial::put_hex(static_cast<uint64_t>(core));
        serial::puts("=");
        serial::put_hex(static_cast<uint64_t>(completed));
    }
    serial::puts("\r\n");

    if (cores_with_completions < 2) {
        panic("smp scheduler: no work migrated off the BSP onto any other core");
    }
    serial::puts("[OK] smp scheduler demo\r\n");
}

// Directly exhausts the heap (bypassing the task abstraction, for full
// control), confirms kmalloc genuinely fails, then proves
// sched::reap_finished_tasks() is what turns that failure into success:
// a batch of short-lived tasks is created and run to completion first
// (their stacks deliberately left unreclaimed — nothing has called
// create_task again to trigger the ordinary per-slot reuse path), then a
// single large allocation is sized (from a live heap::free_bytes()
// reading, not a hardcoded guess) to require reclaiming that leftover
// space. Confirms the exact same allocation fails before reaping and
// succeeds after, using the real numbers rather than an assumption.
void oom_reaper_test() {
    // Clear out anything already finished-but-unreclaimed from earlier
    // in the boot sequence (smp_scheduler_demo's 40 tasks, in
    // particular) first, so the counts below are cleanly attributable
    // to just this test's own batch, not a mix of old and new garbage.
    sched::reap_finished_tasks();

    constexpr int kGarbageTasks = 20;
    int start = sched::tasks_finished();
    // See smp_scheduler_demo's identical comment: the whole batch must
    // be created with the BSP's own interrupts held off, not just each
    // individual create_task call.
    asm volatile("cli");
    for (int i = 0; i < kGarbageTasks; i++) {
        sched::create_task(smp_demo_task, "oom-garbage");
    }
    asm volatile("sti");
    while (sched::tasks_finished() < start + kGarbageTasks) {
        asm volatile("hlt");
    }
    // Every one of those kGarbageTasks slots is now kFinished, holding a
    // 16 KiB stack apiece, entirely unreclaimed — nothing has called
    // create_task since, so the ordinary per-call reuse path never ran.

    std::size_t free_before = heap::free_bytes();
    std::size_t oversized_request = free_before + 32768; // more than is currently free, on purpose

    void* should_fail = heap::kmalloc(oversized_request);
    if (should_fail != nullptr) {
        panic("oom reaper test: allocation unexpectedly succeeded before reaping");
    }

    int reaped = sched::reap_finished_tasks();
    if (reaped != kGarbageTasks) {
        panic("oom reaper test: reaped count didn't match the garbage batch exactly");
    }

    void* should_succeed = heap::kmalloc(oversized_request);
    if (should_succeed == nullptr) {
        panic("oom reaper test: the same allocation still failed after reaping");
    }
    heap::kfree(should_succeed);

    serial::puts("[OK] oom reaper reaped=");
    serial::put_hex(static_cast<uint64_t>(reaped));
    serial::puts("\r\n");
}

constexpr const char* kTestFileName = "HELLO.TXT";
constexpr const char* kExpectedContent = "Hello from the Singularity Kernel test disk!";

constexpr std::size_t const_strlen(const char* s) {
    std::size_t n = 0;
    while (s[n] != '\0') {
        n++;
    }
    return n;
}

void disk_io_test() {
    char buf[128] = {};
    int n = fat32::read_file(kTestFileName, buf, sizeof(buf) - 1);
    if (n < 0) {
        panic("disk I/O: test file not found");
    }
    if (static_cast<std::size_t>(n) != const_strlen(kExpectedContent)) {
        panic("disk I/O: file size mismatch");
    }
    for (int i = 0; i < n; i++) {
        if (buf[i] != kExpectedContent[i]) {
            panic("disk I/O: file content mismatch");
        }
    }
    buf[n] = '\0';
    serial::puts("[OK] disk read: ");
    serial::puts(buf);
    serial::puts("\r\n");
}

// Repeatedly reads the same file end-to-end (virtio-blk request/poll
// cycle, FAT cluster-chain walk, byte copy) and verifies byte-for-byte
// correctness every time — proof the driver and filesystem reader are
// stable under repeated use, not just correct once.
void disk_io_torture_test() {
    constexpr int kIterations = 100;
    std::size_t expected_len = const_strlen(kExpectedContent);

    for (int iter = 0; iter < kIterations; iter++) {
        char buf[128] = {};
        int n = fat32::read_file(kTestFileName, buf, sizeof(buf) - 1);
        if (n < 0 || static_cast<std::size_t>(n) != expected_len) {
            panic("disk I/O torture: size mismatch on a repeated read");
        }
        for (int i = 0; i < n; i++) {
            if (buf[i] != kExpectedContent[i]) {
                panic("disk I/O torture: content mismatch on a repeated read");
            }
        }
    }
    serial::puts("[OK] disk torture\r\n");
}

} // namespace

extern "C" void kernel_main(efi::Handle image_handle, efi::SystemTable* system_table) {
    serial::puts("[OK] handoff\r\n");

    // pmm::init (GetMemoryMap + ExitBootServices) must run before we touch
    // the GDT/IDT at all, not after. UEFI's firmware code may still be
    // using its own timer/interrupts internally during those calls, and
    // once ExitBootServices succeeds it guarantees it won't anymore —
    // that guarantee is exactly what makes it safe for us to replace the
    // GDT/IDT afterward. Getting this backwards (as an earlier version of
    // this function did) means a timer interrupt firing while firmware's
    // own code is still running gets routed through OUR already-installed,
    // deliberately-incomplete IDT (vectors 32+ not present) instead of
    // UEFI's own — observed directly as a #GP(0x102) (take an interrupt
    // on a not-present vector-32 gate) with an RIP pointing into firmware
    // code, not ours. See DECISIONS.md, stage 3.
    pmm::init(image_handle, system_table); // prints [OK] memmap, [OK] exitbs

    // Only now do we own the machine outright.
    asm volatile("cli");

    // Must happen before paging::init() below — it decides, once, which
    // physical pages get read+execute permissions, and the AP trampoline
    // page needs to be one of them. See DECISIONS.md, stage 9.
    smp::reserve_trampoline_page();

    gdt::init();
    serial::puts("[OK] gdt\r\n");

    idt::init();
    serial::puts("[OK] idt\r\n");

    serial::puts("[OK] pmm total_pages=");
    serial::put_hex(pmm::total_pages());
    serial::puts(" free_pages=");
    serial::put_hex(pmm::free_pages());
    serial::puts("\r\n");
    pmm_torture_test();

    paging::init(); // prints [OK] paging, [OK] higher-half

    // ACPI's configuration-table pointer and everything it leads to
    // (XSDT, MADT) live in ordinary physical memory this kernel's own
    // identity map now covers — safe to walk from here on, same as any
    // other raw-physical-pointer access in this codebase.
    acpi::init(system_table); // prints [OK] acpi madt lapics=... ioapics=...

    // Free-running reference clock the LAPIC timer calibrates itself
    // against below — must be running before any core (BSP or AP) calls
    // lapic::calibrate_and_arm_timer. A single shared physical PIT, only
    // ever started once, from the BSP. See DECISIONS.md, stage 10.
    pit::start_reference_clock();

    lapic::enable();
    serial::puts("[OK] lapic id=");
    serial::put_hex(lapic::id());
    serial::puts("\r\n");

    constexpr uint8_t kTimerVector = 32;
    uint32_t measured_hz = lapic::calibrate_and_arm_timer(100, kTimerVector);
    serial::puts("[OK] lapic timer measured_hz=");
    serial::put_hex(measured_hz);
    serial::puts("\r\n");

    ioapic::init();

    divide_by_zero_test();
    wx_enforcement_test();

    heap::init(); // prints [OK] heap
    serial::puts("[OK] heap free_bytes=");
    serial::put_hex(heap::free_bytes());
    serial::puts(" used_bytes=");
    serial::put_hex(heap::used_bytes());
    serial::puts("\r\n");
    heap_torture_test();

    pic::init(); // remap + mask everything; see DECISIONS.md, stage 10
    sched::register_this_core_idle();

    // Every other enabled core the MADT reported, brought up AND now
    // genuinely participating in scheduling (each AP calibrates its own
    // LAPIC timer, registers its own idle context, and calls sti as the
    // last thing ap_entry does) — the actual proof of a real per-core
    // dispatcher, not just cores that exist and print an id. See
    // DECISIONS.md, stage 10 (this replaces stage 9's "parks forever,
    // interrupts masked" ending).
    smp::bring_up_all_aps();

    asm volatile("sti"); // BSP's own first-ever interrupts-enabled moment

    smp_scheduler_demo();

    virtio_blk::init(); // prints [OK] virtio-blk queue_size=...
    fat32::init();       // prints [OK] fat32 bytes_per_sector=...

    disk_io_test();
    disk_io_torture_test();

    oom_reaper_test();

    shell::run(); // never returns
}
