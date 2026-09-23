#pragma once

namespace idt {

// Installs handlers for the 32 CPU exception vectors (0-31), plus vector
// 32 (the PIT timer IRQ, remapped there by pic::init() — see stage 5's
// DECISIONS.md entry), and loads the IDT. Vectors 33-255 are left
// not-present; nothing else generates interrupts in this project yet.
void init();

} // namespace idt
