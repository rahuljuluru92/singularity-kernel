#pragma once

namespace idt {

// Installs handlers for the 32 CPU exception vectors (0-31) and loads the
// IDT. Vectors 32-255 (hardware IRQs) are left not-present — interrupts
// stay masked (see init()'s cli) until a later stage needs them.
void init();

} // namespace idt
