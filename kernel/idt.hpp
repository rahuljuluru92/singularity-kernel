#pragma once

namespace idt {

// Builds the (shared, global) IDT — handlers for the 32 CPU exception
// vectors (0-31) plus vector 32 (each core's own LAPIC timer — see
// kernel/lapic.hpp) — and loads it on the calling core. Vectors 33-255
// are left not-present. Must be called exactly ONCE, by the BSP: the
// table it builds is shared, identical content for every core, and
// stage 10 needs every core able to safely load (not rebuild) it at any
// time relative to any other core's ongoing execution — see load()
// below and DECISIONS.md, stage 10, for the real bug this split fixes.
void init();

// Loads the ALREADY-BUILT shared IDT on the calling core — just the
// `lidt` instruction, no writes to the table at all. Safe to call from
// any core at any time, including while other cores are actively taking
// interrupts through that same table (unlike init(), which mutates it).
// Every AP calls this instead of init() — see kernel/smp.cpp.
void load();

} // namespace idt
