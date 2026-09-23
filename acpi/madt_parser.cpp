#include "madt_parser.hpp"

namespace acpi {

namespace {

constexpr std::uint8_t kTypeLocalApic = 0;
constexpr std::uint8_t kTypeIoApic = 1;

std::uint8_t read_u8(const std::uint8_t* p) { return p[0]; }

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

} // namespace

void parse_madt_entries(const std::uint8_t* entries, std::size_t length, MadtParseResult& out) {
    out = MadtParseResult{};

    std::size_t offset = 0;
    while (offset + 2 <= length) {
        std::uint8_t type = entries[offset];
        std::uint8_t entry_length = entries[offset + 1];

        // A well-formed entry is at least the 2-byte type/length header;
        // anything smaller (or one that would read past the buffer) is
        // corrupt — stop rather than risk an out-of-bounds read.
        if (entry_length < 2 || offset + entry_length > length) {
            break;
        }

        if (type == kTypeLocalApic && entry_length >= 8) {
            if (out.lapic_count < MadtParseResult::kMaxLapics) {
                out.lapics[out.lapic_count++] = LocalApicEntry{
                    .acpi_processor_id = read_u8(entries + offset + 2),
                    .apic_id = read_u8(entries + offset + 3),
                    .flags = read_u32(entries + offset + 4),
                };
            } else {
                out.overflowed = true;
            }
        } else if (type == kTypeIoApic && entry_length >= 12) {
            if (out.ioapic_count < MadtParseResult::kMaxIoapics) {
                out.ioapics[out.ioapic_count++] = IoApicEntry{
                    .ioapic_id = read_u8(entries + offset + 2),
                    .address = read_u32(entries + offset + 4),
                    .gsi_base = read_u32(entries + offset + 8),
                };
            } else {
                out.overflowed = true;
            }
        }
        // Every other type (interrupt source override, NMI, local x2APIC,
        // etc.) is intentionally skipped, not parsed — see madt_parser.hpp.

        offset += entry_length;
    }
}

} // namespace acpi
