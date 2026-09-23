#pragma once

#include <cstdint>

namespace pci {

struct Device {
    uint8_t bus, slot, function;
    uint16_t vendor_id, device_id;
    uint32_t bar[6];
};

uint8_t config_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint16_t config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
uint32_t config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset);
void config_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value);

// Brute-force scans all 256 buses for the first device matching
// vendor_id/device_id (all functions, including multi-function devices).
// Fills *out and returns true if found.
bool find_device(uint16_t vendor_id, uint16_t device_id, Device* out);

} // namespace pci
