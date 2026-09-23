#include "isr.hpp"
#include "serial.hpp"

// Stage 2 has no recovery path for any exception — real recovery (e.g.
// resuming after a handled page fault) is later-stage work. Every vector
// ends up here and halts; vector 0 is the deliberate boot-time self-test.
extern "C" void isr_handler(registers* regs) {
    if (regs->vector == 0) {
        serial::puts("[OK] exception caught\r\n");
    } else {
        serial::puts("[PANIC] unexpected exception vector=");
        serial::put_hex(regs->vector);
        serial::puts(" error_code=");
        serial::put_hex(regs->error_code);
        serial::puts(" rip=");
        serial::put_hex(regs->rip);
        serial::puts("\r\n");
    }

    for (;;) {
        asm volatile("cli\n\thlt");
    }
}
