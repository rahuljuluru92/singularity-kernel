#include "gdt.hpp"
#include "idt.hpp"
#include "serial.hpp"

extern "C" void kernel_main() {
    serial::puts("[OK] handoff\r\n");

    gdt::init();
    serial::puts("[OK] gdt\r\n");

    idt::init();
    serial::puts("[OK] idt\r\n");

    // Deliberate divide-by-zero to prove the exception path works. volatile
    // forces a real runtime idiv — without it the compiler could treat
    // division-by-a-known-zero as UB and discard it entirely.
    volatile int zero = 0;
    volatile int result = 10 / zero;
    (void)result;

    // Unreachable: isr_handler halts instead of returning. Kept only so
    // this function has a defined fallback if that ever changes.
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}
