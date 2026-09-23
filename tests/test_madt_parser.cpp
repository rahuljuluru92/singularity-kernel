#include <catch2/catch_test_macros.hpp>

#include "madt_parser.hpp"

using acpi::MadtParseResult;
using acpi::parse_madt_entries;

namespace {

// Little-endian encoders matching how ACPI tables are always laid out.
void put_u32(std::uint8_t* p, std::uint32_t v) {
    p[0] = static_cast<std::uint8_t>(v);
    p[1] = static_cast<std::uint8_t>(v >> 8);
    p[2] = static_cast<std::uint8_t>(v >> 16);
    p[3] = static_cast<std::uint8_t>(v >> 24);
}

} // namespace

TEST_CASE("empty entry list parses to nothing", "[madt]") {
    MadtParseResult result;
    parse_madt_entries(nullptr, 0, result);
    REQUIRE(result.lapic_count == 0);
    REQUIRE(result.ioapic_count == 0);
    REQUIRE_FALSE(result.overflowed);
}

TEST_CASE("single enabled Local APIC entry", "[madt]") {
    std::uint8_t bytes[8] = {0, 8, 7, 3, 0, 0, 0, 0}; // type=0 len=8 procid=7 apicid=3
    put_u32(bytes + 4, acpi::kLapicFlagEnabled);

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == 1);
    REQUIRE(result.lapics[0].acpi_processor_id == 7);
    REQUIRE(result.lapics[0].apic_id == 3);
    REQUIRE((result.lapics[0].flags & acpi::kLapicFlagEnabled) != 0);
    REQUIRE(result.ioapic_count == 0);
}

TEST_CASE("Local APIC and I/O APIC entries both parsed, in order", "[madt]") {
    std::uint8_t bytes[8 + 12] = {};
    bytes[0] = 0;
    bytes[1] = 8;
    bytes[2] = 1; // acpi_processor_id
    bytes[3] = 2; // apic_id
    put_u32(bytes + 4, acpi::kLapicFlagEnabled);

    std::uint8_t* io = bytes + 8;
    io[0] = 1;
    io[1] = 12;
    io[2] = 5; // ioapic_id
    io[3] = 0; // reserved
    put_u32(io + 4, 0xFEC00000);
    put_u32(io + 8, 0);

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == 1);
    REQUIRE(result.lapics[0].apic_id == 2);
    REQUIRE(result.ioapic_count == 1);
    REQUIRE(result.ioapics[0].ioapic_id == 5);
    REQUIRE(result.ioapics[0].address == 0xFEC00000);
    REQUIRE(result.ioapics[0].gsi_base == 0);
}

TEST_CASE("unrecognized entry types are skipped via their own length, not misparsed", "[madt]") {
    // A fabricated "type 2" (interrupt source override) entry, length 10,
    // whose body is never interpreted as anything — followed by a real
    // Local APIC entry that must still parse correctly afterward.
    std::uint8_t bytes[10 + 8] = {};
    bytes[0] = 2;
    bytes[1] = 10;
    // bytes[2..9] deliberately left as garbage-looking non-zero filler to
    // prove they're never read as a Local APIC/I/O APIC body.
    for (int i = 2; i < 10; i++) {
        bytes[i] = 0xAA;
    }

    std::uint8_t* lapic = bytes + 10;
    lapic[0] = 0;
    lapic[1] = 8;
    lapic[2] = 0;
    lapic[3] = 9;
    put_u32(lapic + 4, acpi::kLapicFlagEnabled);

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == 1);
    REQUIRE(result.lapics[0].apic_id == 9);
    REQUIRE(result.ioapic_count == 0);
}

TEST_CASE("a disabled Local APIC entry is still reported (caller decides what to do with it)",
          "[madt]") {
    std::uint8_t bytes[8] = {0, 8, 0, 4, 0, 0, 0, 0}; // flags = 0 (not enabled)

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == 1);
    REQUIRE((result.lapics[0].flags & acpi::kLapicFlagEnabled) == 0);
}

TEST_CASE("a malformed entry (length < 2) stops the walk instead of misreading", "[madt]") {
    std::uint8_t bytes[8 + 1] = {0, 8, 0, 0, 0, 0, 0, 0, /* trailing garbage byte */ 0xFF};
    put_u32(bytes + 4, acpi::kLapicFlagEnabled);
    // Overwrite what would be a second entry's header with an invalid
    // length (1, which is < the minimum 2-byte type/length header).
    bytes[8] = 1;

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    // The first, well-formed entry is still returned.
    REQUIRE(result.lapic_count == 1);
}

TEST_CASE("an entry whose length would run past the buffer stops the walk, no OOB read", "[madt]") {
    std::uint8_t bytes[5] = {0, 8, 0, 0, 0}; // claims length 8 but only 5 bytes exist

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == 0);
}

TEST_CASE("more Local APIC entries than the fixed capacity sets overflowed, doesn't crash",
          "[madt]") {
    constexpr int kCount = MadtParseResult::kMaxLapics + 5;
    std::uint8_t bytes[kCount * 8] = {};
    for (int i = 0; i < kCount; i++) {
        std::uint8_t* e = bytes + i * 8;
        e[0] = 0;
        e[1] = 8;
        e[2] = 0;
        e[3] = static_cast<std::uint8_t>(i);
        put_u32(e + 4, acpi::kLapicFlagEnabled);
    }

    MadtParseResult result;
    parse_madt_entries(bytes, sizeof(bytes), result);

    REQUIRE(result.lapic_count == MadtParseResult::kMaxLapics);
    REQUIRE(result.overflowed);
}
