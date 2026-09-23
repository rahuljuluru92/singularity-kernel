#pragma once

#include <cstdint>

// SMP bring-up: copies the AP trampoline (kernel/ap_trampoline.asm) to a
// fixed low physical address and, sequentially, sends every other
// enabled MADT Local APIC entry the universal INIT-SIPI-SIPI sequence,
// waiting for it to signal it's alive before moving to the next. See
// DECISIONS.md, stage 9, for the full design (why sequential, why x2APIC-
// only, why the trampoline page needs to be marked executable).
namespace smp {

// Fixed physical address the trampoline is copied to and the SIPI vector
// is derived from (vector = kTrampolineAddr / 0x1000). Low, page-aligned,
// below 1 MiB — required by the SIPI startup-vector encoding itself, not
// a free choice. MUST match kernel/ap_trampoline.asm's `org` exactly.
constexpr uint64_t kTrampolineAddr = 0x8000;

// Byte offset, from kTrampolineAddr, of the three 8-byte "patch slots"
// (boot_cr3, boot_stack_top, boot_entry, in that order) the trampoline
// reads before entering long mode. A fixed, round-number offset chosen
// to be comfortably past the trampoline's actual code+GDT size, so this
// constant doesn't need to track that size exactly — see
// kernel/ap_trampoline.asm.
constexpr uint64_t kPatchAreaOffset = 0x800;

// Reserves the trampoline's physical page (pmm::reserve_page) and marks
// it executable under the shared page tables (paging::add_executable_page).
// Must run after pmm::init() and strictly BEFORE paging::init() — permissions
// are decided once, when init() builds the tables. See DECISIONS.md.
void reserve_trampoline_page();

// Brings up every enabled MADT Local APIC entry other than this core.
// Must run after acpi::init(), lapic::enable(), paging::init(), and
// heap::init() (each AP's stack is heap-allocated). Prints one
// "[OK] cpu <id> up" per AP — genuinely printed from that AP's own
// execution, reading its own x2APIC ID — then a final
// "[OK] smp cpus_up=<n>" once every AP has either checked in or a
// bounded wait has timed out (which panics, per this project's
// never-round-up-a-failure policy — see DECISIONS.md).
void bring_up_all_aps();

} // namespace smp

// Where every AP's trampoline hands off, in 64-bit long mode. Declared
// here at file scope (not inside namespace smp) but DEFINED inside
// namespace smp in smp.cpp — legal and unambiguous because extern "C"
// linkage is keyed on the unmangled name alone, not the enclosing C++
// namespace; see smp.cpp. Never returns: idles forever with interrupts
// masked — this stage brings cores up and proves they execute
// independently, nothing schedules work onto them yet (that's stage 10).
extern "C" [[noreturn]] void ap_entry();
