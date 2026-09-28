#include "pit.hpp"
#include "serial.hpp"

namespace {

constexpr uint16_t kChannel0Data = 0x40;
constexpr uint16_t kCommand = 0x43;

// Channel 0, lobyte/hibyte access, mode 2 (rate generator), binary — a
// plain linear countdown that auto-reloads, the simplest shape to poll
// for calibration purposes (unlike mode 3's square-wave halving
// behavior, which complicates reading an exact elapsed-tick count).
constexpr uint8_t kChannel0Mode2 = 0x34;

// Latches channel 0's count for a following two-byte read, without
// altering the count itself.
constexpr uint8_t kLatchChannel0 = 0x00;

} // namespace

namespace pit {

void start_reference_clock() {
    serial::outb(kCommand, kChannel0Mode2);
    serial::outb(kChannel0Data, 0xFF);
    serial::outb(kChannel0Data, 0xFF);
}

uint16_t read_counter() {
    serial::outb(kCommand, kLatchChannel0);
    uint8_t lo = serial::inb(kChannel0Data);
    uint8_t hi = serial::inb(kChannel0Data);
    return static_cast<uint16_t>(lo | (static_cast<uint16_t>(hi) << 8));
}

} // namespace pit
