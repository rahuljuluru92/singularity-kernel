#include "pit.hpp"
#include "serial.hpp"

namespace {

constexpr uint16_t kChannel0Data = 0x40;
constexpr uint16_t kCommand = 0x43;
constexpr uint32_t kBaseFrequencyHz = 1193182; // fixed PIT input clock

// Channel 0, lobyte/hibyte access, mode 3 (square wave generator), binary.
constexpr uint8_t kChannel0Mode3 = 0x36;

} // namespace

namespace pit {

void init(uint32_t frequency_hz) {
    uint32_t divisor = kBaseFrequencyHz / frequency_hz;

    serial::outb(kCommand, kChannel0Mode3);
    serial::outb(kChannel0Data, static_cast<uint8_t>(divisor & 0xFF));
    serial::outb(kChannel0Data, static_cast<uint8_t>((divisor >> 8) & 0xFF));
}

} // namespace pit
