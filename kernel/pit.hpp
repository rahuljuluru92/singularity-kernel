#pragma once

#include <cstdint>

namespace pit {

// Configures PIT channel 0 to fire IRQ0 at the given frequency (Hz).
void init(uint32_t frequency_hz);

} // namespace pit
