#pragma once

#include <cstdint>

// x2APIC-only Local APIC driver. This project targets QEMU/OVMF
// specifically (see CLAUDE.md's scope note), and x2APIC is MSR-based —
// no MMIO mapping is needed at all, unlike legacy xAPIC's fixed
// 0xFEE00000 physical window, which this kernel's page tables don't map
// (paging::init() only maps physical addresses pmm actually tracks as
// real RAM — see DECISIONS.md, stage 9, for why xAPIC was ruled out
// rather than supported as a fallback).
namespace lapic {

// Checks CPUID leaf 1 ECX bit 21 and panics if x2APIC isn't available —
// there is no xAPIC fallback (see the file comment above). Then enables
// x2APIC mode via IA32_APIC_BASE and software-enables the LAPIC via the
// Spurious Interrupt Vector Register. Must be called on every core
// (BSP and every AP) — IA32_APIC_BASE and the SVR are per-core state,
// not shared.
void enable();

// This core's x2APIC ID, read live from MSR 0x802 — genuine proof of
// which core is executing when called from within an AP's own code path,
// not a value handed to it by the BSP.
std::uint32_t id();

// Sends the universal INIT-SIPI-SIPI startup sequence to bring up the AP
// with the given x2APIC ID, waiting the spec-recommended delays between
// steps. `vector` is the page-aligned physical start address of the AP
// trampoline, divided by 0x1000 (per the SIPI vector's own encoding:
// the target CPU starts executing at physical address vector*0x1000,
// CS = vector<<8, IP = 0). Does not wait for the AP to signal it's
// actually up afterward — that's smp.cpp's job, since only smp.cpp knows
// what "up" means (a flag the AP itself sets once it reaches C++ code).
void send_init_sipi_sipi(std::uint32_t target_apic_id, std::uint8_t vector);

// Must be called at the end of any interrupt handler this core takes
// through the LAPIC.
void eoi();

// Calibrates THIS core's LAPIC timer against the PIT reference clock
// (pit::start_reference_clock() must already be running — see
// kernel/pit.hpp) and arms it in periodic mode at approximately
// `frequency_hz`, targeting IDT vector `vector`. Every core's LAPIC
// timer is independent hardware — this must be called once per core
// (BSP and every AP), never shared. Leaves the timer genuinely counting
// and unmasked; whether it actually delivers an interrupt is controlled
// purely by that core's own EFLAGS.IF (sti/cli), not by this function.
// Returns the measured timer frequency in Hz (for logging/verification —
// an honest measured value, not the requested one) — see DECISIONS.md,
// stage 10, for why calibration is necessary at all (there's no
// documented fixed LAPIC timer frequency to assume).
std::uint32_t calibrate_and_arm_timer(std::uint32_t frequency_hz, std::uint8_t vector);

} // namespace lapic
