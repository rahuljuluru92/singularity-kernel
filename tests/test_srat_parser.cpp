#include <catch2/catch_test_macros.hpp>

#include "srat_parser.hpp"

using acpi::SratParseResult;
using acpi::parse_srat_entries;

namespace {

// Little-endian encoders matching how ACPI tables are always laid out.
void put_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

void put_u64(std::uint8_t* p, std::uint64_t v) {
    put_u32(p, static_cast<std::uint32_t>(v));
    put_u32(p + 4, static_cast<std::uint32_t>(v >> 32));
}

// Builds a 16-byte Type 0 (Processor Local APIC/SAPIC Affinity) entry.
void put_processor_affinity(std::uint8_t* e, std::uint32_t proximity_domain,
                             std::uint8_t apic_id, std::uint32_t flags) {
    e[0] = 0;  // type
    e[1] = 16; // length
    e[2] = static_cast<std::uint8_t>(proximity_domain);
    e[3] = apic_id;
    put_u32(e + 4, flags);
    e[8] = 0; // local SAPIC EID, unused
    e[9] = static_cast<std::uint8_t>(proximity_domain >> 8);
    e[10] = static_cast<std::uint8_t>(proximity_domain >> 16);
    e[11] = static_cast<std::uint8_t>(proximity_domain >> 24);
    put_u32(e + 12, 0); // clock domain, unused
}

// Builds a 40-byte Type 1 (Memory Affinity) entry. Caller-provided `e`
// must already be zeroed (this only fills the fields the parser reads;
// the reserved fields are left at their zero-initialized value).
void put_memory_affinity(std::uint8_t* e, std::uint32_t proximity_domain, std::uint64_t base,
                          std::uint64_t length, std::uint32_t flags) {
    e[0] = 1;  // type
    e[1] = 40; // length
    put_u32(e + 2, proximity_domain);
    put_u64(e + 8, base);
    put_u64(e + 16, length);
    put_u32(e + 28, flags);
}

} // namespace

TEST_CASE("empty entry list parses to nothing", "[srat]") {
    SratParseResult result;
    parse_srat_entries(nullptr, 0, result);
    REQUIRE(result.processor_affinity_count == 0);
    REQUIRE(result.memory_affinity_count == 0);
    REQUIRE_FALSE(result.overflowed);
}

TEST_CASE("single enabled Processor Affinity entry, proximity domain within one byte", "[srat]") {
    std::uint8_t bytes[16] = {};
    put_processor_affinity(bytes, /*proximity_domain=*/1, /*apic_id=*/2,
                            acpi::kProcessorAffinityFlagEnabled);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == 1);
    REQUIRE(result.processor_affinities[0].apic_id == 2);
    REQUIRE(result.processor_affinities[0].proximity_domain == 1);
    REQUIRE((result.processor_affinities[0].flags & acpi::kProcessorAffinityFlagEnabled) != 0);
    REQUIRE(result.memory_affinity_count == 0);
}

TEST_CASE("Processor Affinity proximity domain spanning all 4 bytes is reassembled correctly",
          "[srat]") {
    // Domain 0x01020304: low byte (0x04) at offset 2, high 3 bytes
    // (0x01, 0x02, 0x03) at offsets 9-11 — the split ACPI spec layout.
    std::uint8_t bytes[16] = {};
    put_processor_affinity(bytes, 0x01020304, /*apic_id=*/9, acpi::kProcessorAffinityFlagEnabled);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == 1);
    REQUIRE(result.processor_affinities[0].proximity_domain == 0x01020304);
}

TEST_CASE("single Memory Affinity entry, base/length reassembled from split lo/hi fields",
          "[srat]") {
    std::uint8_t bytes[40] = {};
    put_memory_affinity(bytes, /*proximity_domain=*/1, /*base=*/0x100000000ULL,
                         /*length=*/0x8000000ULL, acpi::kMemoryAffinityFlagEnabled);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.memory_affinity_count == 1);
    REQUIRE(result.memory_affinities[0].proximity_domain == 1);
    REQUIRE(result.memory_affinities[0].base_address == 0x100000000ULL);
    REQUIRE(result.memory_affinities[0].length == 0x8000000ULL);
    REQUIRE((result.memory_affinities[0].flags & acpi::kMemoryAffinityFlagEnabled) != 0);
    REQUIRE(result.processor_affinity_count == 0);
}

TEST_CASE("Processor and Memory Affinity entries both parsed, in order", "[srat]") {
    std::uint8_t bytes[16 + 40] = {};
    put_processor_affinity(bytes, /*proximity_domain=*/0, /*apic_id=*/0,
                            acpi::kProcessorAffinityFlagEnabled);
    put_memory_affinity(bytes + 16, /*proximity_domain=*/0, /*base=*/0, /*length=*/0x8000000ULL,
                         acpi::kMemoryAffinityFlagEnabled);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == 1);
    REQUIRE(result.memory_affinity_count == 1);
}

TEST_CASE("unrecognized entry types are skipped via their own length, not misparsed", "[srat]") {
    // A fabricated type 2 (Processor Local x2APIC Affinity) entry, length
    // 24, whose body is never interpreted as anything — followed by a
    // real Processor Affinity entry that must still parse correctly
    // afterward.
    std::uint8_t bytes[24 + 16] = {};
    bytes[0] = 2;
    bytes[1] = 24;
    for (int i = 2; i < 24; i++) {
        bytes[i] = 0xAA;
    }

    put_processor_affinity(bytes + 24, /*proximity_domain=*/3, /*apic_id=*/7,
                            acpi::kProcessorAffinityFlagEnabled);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == 1);
    REQUIRE(result.processor_affinities[0].apic_id == 7);
    REQUIRE(result.memory_affinity_count == 0);
}

TEST_CASE("a disabled Memory Affinity entry is still reported (caller decides what to do with it)",
          "[srat]") {
    std::uint8_t bytes[40] = {};
    put_memory_affinity(bytes, /*proximity_domain=*/0, /*base=*/0, /*length=*/0x1000, /*flags=*/0);

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.memory_affinity_count == 1);
    REQUIRE((result.memory_affinities[0].flags & acpi::kMemoryAffinityFlagEnabled) == 0);
}

TEST_CASE("a malformed entry (length < 2) stops the walk instead of misreading", "[srat]") {
    std::uint8_t bytes[16 + 1] = {};
    put_processor_affinity(bytes, /*proximity_domain=*/0, /*apic_id=*/0,
                            acpi::kProcessorAffinityFlagEnabled);
    // Overwrite what would be a second entry's header with an invalid
    // length (1, which is < the minimum 2-byte type/length header).
    bytes[16] = 1;

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    // The first, well-formed entry is still returned.
    REQUIRE(result.processor_affinity_count == 1);
}

TEST_CASE("an entry whose length would run past the buffer stops the walk, no OOB read",
          "[srat]") {
    std::uint8_t bytes[5] = {0, 16, 0, 0, 0}; // claims length 16 but only 5 bytes exist

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == 0);
}

TEST_CASE("more Processor Affinity entries than the fixed capacity sets overflowed, doesn't crash",
          "[srat]") {
    constexpr int kCount = SratParseResult::kMaxProcessorAffinities + 5;
    std::uint8_t bytes[kCount * 16] = {};
    for (int i = 0; i < kCount; i++) {
        put_processor_affinity(bytes + i * 16, /*proximity_domain=*/0,
                                static_cast<std::uint8_t>(i), acpi::kProcessorAffinityFlagEnabled);
    }

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.processor_affinity_count == SratParseResult::kMaxProcessorAffinities);
    REQUIRE(result.overflowed);
}

TEST_CASE("more Memory Affinity entries than the fixed capacity sets overflowed, doesn't crash",
          "[srat]") {
    constexpr int kCount = SratParseResult::kMaxMemoryAffinities + 3;
    std::uint8_t bytes[kCount * 40] = {};
    for (int i = 0; i < kCount; i++) {
        put_memory_affinity(bytes + i * 40, static_cast<std::uint32_t>(i), 0, 0x1000,
                             acpi::kMemoryAffinityFlagEnabled);
    }

    SratParseResult result;
    parse_srat_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.memory_affinity_count == SratParseResult::kMaxMemoryAffinities);
    REQUIRE(result.overflowed);
}
