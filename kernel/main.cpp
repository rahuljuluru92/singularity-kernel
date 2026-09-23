#include "efi.hpp"
#include "gdt.hpp"
#include "heap.hpp"
#include "idt.hpp"
#include "isr.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "serial.hpp"

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
    serial::puts("[PANIC] ");
    serial::puts(msg);
    serial::puts("\r\n");
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

    divide_by_zero_test();
    wx_enforcement_test();

    heap::init(); // prints [OK] heap
    serial::puts("[OK] heap free_bytes=");
    serial::put_hex(heap::free_bytes());
    serial::puts(" used_bytes=");
    serial::put_hex(heap::used_bytes());
    serial::puts("\r\n");
    heap_torture_test();

    for (;;) {
        asm volatile("cli\n\thlt");
    }
}
