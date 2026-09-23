#include "smp.hpp"

#include "acpi.hpp"
#include "bitmap_allocator.hpp" // for pmm::kPageSize
#include "gdt.hpp"
#include "heap.hpp"
#include "idt.hpp"
#include "lapic.hpp"
#include "paging.hpp"
#include "pmm.hpp"
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

    serial::puts("[OK] smp cpus_up=");
    serial::put_hex(static_cast<std::uint64_t>(g_cpus_up));
    serial::puts("\r\n");
}

extern "C" [[noreturn]] void ap_entry() {
    // Every core owns its own GDT/IDT registers (GDTR/IDTR), even though
    // the content they point to is shared — see kernel/gdt.cpp,
    // kernel/idt.cpp. Calling the exact same init() the BSP used, rather
    // than a separate "AP variant," means there's only one place either
    // is ever defined.
    gdt::init();
    idt::init();
    // IA32_APIC_BASE and the Spurious Interrupt Vector Register are
    // per-core MSRs — every core must enable its own x2APIC.
    lapic::enable();

    std::uint32_t id = lapic::id();
    asm volatile("lock incl %0" : "+m"(g_cpus_up));
    serial::puts("[OK] cpu ");
    serial::put_hex(id);
    serial::puts(" up\r\n");

    // No per-core scheduling exists yet (stage 10) — idle forever with
    // interrupts masked, exactly like kernel/sched.cpp's task_exit()
    // idles a finished task, but never enabling interrupts at all: an
    // unhandled hardware IRQ landing on a core with no scheduler to
    // route it to would be a bug with no good outcome yet.
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

} // namespace smp
