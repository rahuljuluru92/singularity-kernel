#pragma once

#include <cstdint>

// Stage 10 repurposes the PIT from "the scheduling tick source" (stage
// 5) to "a known-frequency reference clock the LAPIC timer calibrates
// itself against" — every core now has its own hardware timer (the
// LAPIC's), and the PIT's own IRQ0 is never delivered as a CPU interrupt
// at all anymore (the 8259 PIC stays fully masked — see kernel/pic.cpp).
// See DECISIONS.md, stage 10.
namespace pit {

// The 8253/8254's fixed input clock — exact, not measured.
constexpr uint32_t kBaseFrequencyHz = 1193182;

// Configures channel 0 as a free-running rate generator (mode 2) with
// the maximum 16-bit reload value, so it counts down and auto-reloads
// continuously at kBaseFrequencyHz. Must run once, from any single core
// (this project always calls it from the BSP), before any core attempts
// LAPIC timer calibration.
void start_reference_clock();

// Latches and reads channel 0's live countdown value. Safe to call from
// any core, but never concurrently from more than one — see
// DECISIONS.md (AP bring-up, and therefore each AP's own calibration
// call, is strictly sequential).
uint16_t read_counter();

} // namespace pit
