#pragma once

#include <cstdint>

namespace pic {

// Remaps the legacy 8259 PIC's IRQ0-15 from their power-on default of
// vectors 0-15 (which collide with CPU exceptions) to 32-47, then masks
// everything except IRQ0 (the PIT timer) — the only hardware interrupt
// this project handles so far.
void init();

// Must be sent after handling any IRQ from this PIC, or it stops
// delivering further interrupts at that priority level or lower. irq is
// 0-15; irq >= 8 additionally EOIs the slave PIC (cascaded through
// master IRQ2), per the 8259's cascade wiring.
void send_eoi(uint8_t irq);

} // namespace pic
