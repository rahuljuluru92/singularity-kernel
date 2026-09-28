#include "smp.hpp"

#include "acpi.hpp"
#include "bitmap_allocator.hpp" // for pmm::kPageSize
#include "gdt.hpp"
#include "heap.hpp"
#include "idt.hpp"
#include "lapic.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "sched.hpp"
#include "serial.hpp"

#include <cstddef>

extern "C" {
extern const std::uint8_t ap_trampoline_blob_start[];
extern const std::uint8_t ap_trampoline_blob_end[];
}

namespace smp {

namespace {

constexpr std::size_t kApStackSize = 16384; // matches kernel/sched.cpp's own task stacks

// No calibrated timer available to bound this by wall-clock time (see
// lapic.cpp's own delay_loop comment) — a generously large busy-wait
// iteration count instead. If a real AP were ever slower than this to
// reach ap_entry() and print its marker, that's a genuine bring-up
// failure worth panicking over, not a value to silently raise — see
// ASSUMPTIONS.md.
constexpr std::uint32_t kBringupTimeoutIterations = 200'000'000;

constexpr int kMaxCpus = acpi::MadtParseResult::kMaxLapics;

struct CpuInfo {
    std::uint32_t apic_id;
    bool up;
};

CpuInfo g_cpus[kMaxCpus];
int g_cpu_count = 0;

// Starts at 1 (the BSP) — every AP that reaches ap_entry() increments
// this itself, from its own execution context, via a single `lock incl`
// (see the torture-test counter in kernel/main.cpp for the exact same
// single-instruction-atomicity reasoning, here needed for real —
// multiple physical cores, not just preemption on one).
volatile int g_cpus_up = 1;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] smp: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

void write_patch_slot(std::uint64_t offset, std::uint64_t value) {
    auto* slot = reinterpret_cast<volatile std::uint64_t*>(kTrampolineAddr + kPatchAreaOffset +
                                                             offset);
    *slot = value;
}

} // namespace

void reserve_trampoline_page() {
    pmm::reserve_page(kTrampolineAddr);
    paging::add_executable_page(kTrampolineAddr);
}

void bring_up_all_aps() {
    std::size_t blob_size =
        static_cast<std::size_t>(ap_trampoline_blob_end - ap_trampoline_blob_start);
    if (blob_size > pmm::kPageSize) {
        panic("AP trampoline blob is larger than the one reserved page");
    }
    auto* dest = reinterpret_cast<std::uint8_t*>(kTrampolineAddr);
    for (std::size_t i = 0; i < blob_size; i++) {
        dest[i] = ap_trampoline_blob_start[i];
    }

    // Shared across every AP: all cores use the exact same page tables
    // and land at the exact same C++ entry point.
    write_patch_slot(0, paging::root_table_physical());
    write_patch_slot(16, reinterpret_cast<std::uint64_t>(&ap_entry));

    std::uint32_t bsp_apic_id = lapic::id();
    std::uint8_t sipi_vector = static_cast<std::uint8_t>(kTrampolineAddr / 0x1000);

    const acpi::MadtParseResult& madt = acpi::madt();
    for (int i = 0; i < madt.lapic_count; i++) {
        const acpi::LocalApicEntry& entry = madt.lapics[i];
        if ((entry.flags & acpi::kLapicFlagEnabled) == 0) {
            continue; // not a real, bring-up-able core
        }
        if (entry.apic_id == bsp_apic_id) {
            continue; // that's us
        }
        if (g_cpu_count >= kMaxCpus) {
            panic("out of per-CPU tracking slots");
        }

        int slot = g_cpu_count++;
        g_cpus[slot] = CpuInfo{entry.apic_id, false};

        void* stack = heap::kmalloc(kApStackSize);
        if (stack == nullptr) {
            panic("kmalloc failed for an AP's stack");
        }
        std::uint64_t stack_top =
            (reinterpret_cast<std::uint64_t>(stack) + kApStackSize) & ~0xFULL;
        write_patch_slot(8, stack_top);

        // Bring-up is deliberately sequential, one AP at a time: the
        // patch slots above are a single shared area, safely reusable
        // across APs only because no two APs are ever mid-startup
        // concurrently — the same reasoning kernel/sched.cpp's
        // create_task already relies on for boot-time-only task
        // creation. See DECISIONS.md.
        int before = g_cpus_up;
        lapic::send_init_sipi_sipi(entry.apic_id, sipi_vector);

        std::uint32_t waited = 0;
        while (g_cpus_up == before && waited < kBringupTimeoutIterations) {
            asm volatile("pause");
            waited++;
        }
        if (g_cpus_up == before) {
            serial::puts("[PANIC] smp: apic_id=");
            serial::put_hex(entry.apic_id);
            serial::puts(" did not signal up within the bring-up timeout\r\n");
            panic("AP bring-up timed out");
        }
        g_cpus[slot].up = true;
    }

    serial::lock();
    serial::puts("[OK] smp cpus_up=");
    serial::put_hex(static_cast<std::uint64_t>(g_cpus_up));
    serial::puts("\r\n");
    serial::unlock();
}

extern "C" [[noreturn]] void ap_entry() {
    // Every core owns its own GDT/IDT registers (GDTR/IDTR), even though
    // the content they point to is shared — see kernel/gdt.cpp,
    // kernel/idt.cpp. Calling the exact same init() the BSP used, rather
    // than a separate "AP variant," means there's only one place either
    // is ever defined.
    // gdt::init() is safe to call redundantly per-core: kGdt is a
    // compile-time constant, never mutated, so every core just does its
    // own independent lgdt against the same immutable content. The IDT
    // is NOT: idt::init() rewrites a shared, mutable table, which is
    // unsafe once any earlier AP is already ticking (a real, reproduced
    // bug — see DECISIONS.md, stage 10) — every AP calls idt::load()
    // instead, which only loads the table the BSP already built once.
    gdt::init();
    idt::load();
    // IA32_APIC_BASE and the Spurious Interrupt Vector Register are
    // per-core MSRs — every core must enable its own x2APIC.
    lapic::enable();

    // Every core calibrates its OWN LAPIC timer against the shared PIT
    // reference clock — safe here specifically because AP bring-up is
    // strictly sequential (see bring_up_all_aps above): no other AP is
    // ever mid-calibration (reading the same PIT) at the same time.
    constexpr std::uint8_t kTimerVector = 32;
    lapic::calibrate_and_arm_timer(100, kTimerVector);

    sched::register_this_core_idle();

    std::uint32_t id = lapic::id();
    asm volatile("lock incl %0" : "+m"(g_cpus_up));
    serial::lock();
    serial::puts("[OK] cpu ");
    serial::put_hex(id);
    serial::puts(" up\r\n");
    serial::unlock();

    // This core now genuinely participates in scheduling: its own timer
    // will fire, and sched::on_timer_tick will look for owned or
    // stealable work every tick. Idling here just means "nothing to do
    // yet, or right now" — not "parked forever" the way stage 9 left it.
    asm volatile("sti");
    for (;;) {
        asm volatile("hlt");
    }
}

} // namespace smp
