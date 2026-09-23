#pragma once

#include <cstddef>
#include <cstdint>

// Walks the variable-length entry list of an ACPI MADT (Multiple APIC
// Description Table) — pure logic over a byte buffer, with no OS or
// hardware dependency, so it's host-unit-tested (Catch2,
// tests/test_madt_parser.cpp) before ever being pointed at a real table
// read out of physical memory by kernel/acpi.cpp. Same pattern this
// project already uses for pmm/bitmap_allocator and
// heap/freelist_allocator. See DECISIONS.md, stage 9.
namespace acpi {

constexpr std::uint32_t kLapicFlagEnabled = 1u << 0;
constexpr std::uint32_t kLapicFlagOnlineCapable = 1u << 1;

struct LocalApicEntry {
    std::uint8_t acpi_processor_id;
    std::uint8_t apic_id;
    std::uint32_t flags;
};

struct IoApicEntry {
    std::uint8_t ioapic_id;
    std::uint32_t address;
    std::uint32_t gsi_base;
};

// Fixed-capacity, no dynamic allocation — this runs during early boot,
// before the heap exists. Capacities are generous relative to any
// realistic QEMU `-smp` configuration or IOAPIC count this project will
// ever be run with, not sized against a specific config.
struct MadtParseResult {
    static constexpr int kMaxLapics = 64;
    static constexpr int kMaxIoapics = 8;

    LocalApicEntry lapics[kMaxLapics];
    int lapic_count = 0;
    IoApicEntry ioapics[kMaxIoapics];
    int ioapic_count = 0;
    // True if a Local APIC (type 0) or I/O APIC (type 1) entry was seen
    // but its array was already full — never silently drops data without
    // saying so.
    bool overflowed = false;
};

// `entries`/`length` is exactly the byte range following the MADT's fixed
// 44-byte header (local_apic_address + flags) — i.e. the table's
// variable-length entry list, not the whole MADT. Recognizes type 0
// (Processor Local APIC) and type 1 (I/O APIC); every other type
// (interrupt source overrides, NMI entries, type 9 Local x2APIC for APIC
// IDs >= 255, etc.) is skipped via its own length field rather than
// parsed — see DECISIONS.md for why type 9 specifically is out of scope
// here. A malformed entry (length < 2, or a length that would run past
// the end of the buffer) stops the walk rather than reading out of
// bounds; whatever was parsed before that point is still returned.
void parse_madt_entries(const std::uint8_t* entries, std::size_t length, MadtParseResult& out);

} // namespace acpi
