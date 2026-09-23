#include "pci.hpp"
#include "serial.hpp"

namespace {

constexpr uint16_t kConfigAddress = 0xCF8;
constexpr uint16_t kConfigData = 0xCFC;

uint32_t address_for(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    return (1u << 31) | (static_cast<uint32_t>(bus) << 16) | (static_cast<uint32_t>(slot) << 11) |
           (static_cast<uint32_t>(function) << 8) | (offset & 0xFC);
}

} // namespace

namespace pci {

uint32_t config_read32(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    serial::outl(kConfigAddress, address_for(bus, slot, function, offset));
    return serial::inl(kConfigData);
}

uint16_t config_read16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t dword = config_read32(bus, slot, function, offset);
    return static_cast<uint16_t>((dword >> ((offset & 2) * 8)) & 0xFFFF);
}

uint8_t config_read8(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset) {
    uint32_t dword = config_read32(bus, slot, function, offset);
    return static_cast<uint8_t>((dword >> ((offset & 3) * 8)) & 0xFF);
}

void config_write16(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint16_t value) {
    uint32_t dword = config_read32(bus, slot, function, offset);
    uint32_t shift = (offset & 2) * 8;
    dword = (dword & ~(0xFFFFu << shift)) | (static_cast<uint32_t>(value) << shift);
    serial::outl(kConfigAddress, address_for(bus, slot, function, offset));
    serial::outl(kConfigData, dword);
}

bool find_device(uint16_t vendor_id, uint16_t device_id, Device* out) {
    for (int bus = 0; bus < 256; bus++) {
        for (int slot = 0; slot < 32; slot++) {
            uint16_t vendor = config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), 0, 0x00);
            if (vendor == 0xFFFF) {
                continue; // no device in this slot
            }
            uint8_t header_type = config_read8(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), 0, 0x0E);
            int function_count = (header_type & 0x80) ? 8 : 1;

            for (int fn = 0; fn < function_count; fn++) {
                uint16_t fn_vendor =
                    config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(fn), 0x00);
                if (fn_vendor == 0xFFFF) {
                    continue;
                }
                uint16_t fn_device =
                    config_read16(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot), static_cast<uint8_t>(fn), 0x02);
                if (fn_vendor == vendor_id && fn_device == device_id) {
                    out->bus = static_cast<uint8_t>(bus);
                    out->slot = static_cast<uint8_t>(slot);
                    out->function = static_cast<uint8_t>(fn);
                    out->vendor_id = fn_vendor;
                    out->device_id = fn_device;
                    for (int i = 0; i < 6; i++) {
                        out->bar[i] = config_read32(out->bus, out->slot, out->function,
                                                     static_cast<uint8_t>(0x10 + i * 4));
                    }
                    return true;
                }
            }
        }
    }
    return false;
}

} // namespace pci
