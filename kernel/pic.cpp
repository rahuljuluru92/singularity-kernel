#include "pic.hpp"
#include "serial.hpp"

namespace {

constexpr uint16_t kMasterCommand = 0x20;
constexpr uint16_t kMasterData = 0x21;
constexpr uint16_t kSlaveCommand = 0xA0;
constexpr uint16_t kSlaveData = 0xA1;

constexpr uint8_t kIcw1Init = 0x11;   // start init sequence, expect ICW4
constexpr uint8_t kIcw4_8086 = 0x01;  // 8086/88 mode
constexpr uint8_t kEoi = 0x20;

// serial::outb/inb are declared in the serial namespace but are generic
// port I/O, not serial-specific — reused here rather than duplicated.
using serial::inb;
using serial::outb;

void io_wait() {
    // Writing to an unused port (the classic 0x80 "POST diagnostic" port)
    // takes long enough on real hardware for the PIC to process the
    // previous command — harmless boilerplate on QEMU, but standard
    // practice and cheap to keep.
    outb(0x80, 0);
}

} // namespace

namespace pic {

void init() {
    uint8_t master_mask = inb(kMasterData);
    uint8_t slave_mask = inb(kSlaveData);
    (void)master_mask;
    (void)slave_mask;

    outb(kMasterCommand, kIcw1Init);
    io_wait();
    outb(kSlaveCommand, kIcw1Init);
    io_wait();

    outb(kMasterData, 0x20); // ICW2: master IRQs mapped to vectors 32-39
    io_wait();
    outb(kSlaveData, 0x28); // ICW2: slave IRQs mapped to vectors 40-47
    io_wait();

    outb(kMasterData, 0x04); // ICW3: slave PIC is wired to master's IRQ2
    io_wait();
    outb(kSlaveData, 0x02); // ICW3: this slave's cascade identity is 2
    io_wait();

    outb(kMasterData, kIcw4_8086);
    io_wait();
    outb(kSlaveData, kIcw4_8086);
    io_wait();

    // Mask everything, including IRQ0 — as of stage 10, every core has
    // its own LAPIC timer driving its own scheduler tick (see
    // kernel/lapic.cpp), and nothing in this project takes an interrupt
    // through the legacy 8259 path anymore. This remap+mask-everything
    // call still runs once, from the BSP, purely to put the PIC into a
    // known, safe, non-colliding-with-CPU-exceptions state rather than
    // trusting whatever UEFI happened to leave it in — not because
    // anything still depends on its IRQs. See DECISIONS.md, stage 10.
    outb(kMasterData, 0xFF);
    outb(kSlaveData, 0xFF);
}

void send_eoi(uint8_t irq) {
    if (irq >= 8) {
        outb(kSlaveCommand, kEoi);
    }
    outb(kMasterCommand, kEoi);
}

} // namespace pic
