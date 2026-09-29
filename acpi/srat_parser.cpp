#include "srat_parser.hpp"

namespace acpi {

namespace {

constexpr std::uint8_t kTypeProcessorAffinity = 0;
constexpr std::uint8_t kTypeMemoryAffinity = 1;

std::uint8_t read_u8(const std::uint8_t* p) { return p[0]; }

std::uint32_t read_u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t read_u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(read_u32(p)) |
           (static_cast<std::uint64_t>(read_u32(p + 4)) << 32);
}

} // namespace

void parse_srat_entries(const std::uint8_t* entries, std::size_t length, SratParseResult& out) {
    out = SratParseResult{};

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

        if (type == kTypeProcessorAffinity && entry_length >= 16) {
            const std::uint8_t* e = entries + offset;
            // Proximity domain is split: low byte at offset 2, high 3
            // bytes at offset 9-11 (around the APIC ID/flags/SAPIC EID
            // fields) — an ACPI spec quirk, not this project's choice.
            std::uint32_t domain_low = read_u8(e + 2);
            std::uint32_t domain_high = static_cast<std::uint32_t>(read_u8(e + 9)) |
                                         (static_cast<std::uint32_t>(read_u8(e + 10)) << 8) |
                                         (static_cast<std::uint32_t>(read_u8(e + 11)) << 16);
            if (out.processor_affinity_count < SratParseResult::kMaxProcessorAffinities) {
                out.processor_affinities[out.processor_affinity_count++] = ProcessorAffinityEntry{
                    .apic_id = read_u8(e + 3),
                    .proximity_domain = domain_low | (domain_high << 8),
                    .flags = read_u32(e + 4),
                };
            } else {
                out.overflowed = true;
            }
        } else if (type == kTypeMemoryAffinity && entry_length >= 40) {
            const std::uint8_t* e = entries + offset;
            // Base address and length are each split into a low/high u32
            // pair (offsets 8/12 and 16/20) rather than a single u64 field
            // — again an ACPI spec layout, kept split here to mirror the
            // table bytes exactly before combining.
            if (out.memory_affinity_count < SratParseResult::kMaxMemoryAffinities) {
                out.memory_affinities[out.memory_affinity_count++] = MemoryAffinityEntry{
                    .proximity_domain = read_u32(e + 2),
                    .base_address = read_u64(e + 8),
                    .length = read_u64(e + 16),
                    .flags = read_u32(e + 28),
                };
            } else {
                out.overflowed = true;
            }
        }
        // Every other type (Local x2APIC affinity, GICC, GIC ITS, Generic
        // Initiator, etc.) is intentionally skipped, not parsed — see
        // srat_parser.hpp.

        offset += entry_length;
    }
}

} // namespace acpi
