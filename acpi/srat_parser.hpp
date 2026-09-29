#pragma once

#include <cstddef>
#include <cstdint>

// Walks the variable-length entry list of an ACPI SRAT (System Resource
// Affinity Table) — pure logic over a byte buffer, with no OS or hardware
// dependency, so it's host-unit-tested (Catch2, tests/test_srat_parser.cpp)
// before ever being pointed at a real table read out of physical memory by
// kernel/acpi.cpp. Same pattern this project already uses for the MADT
// (acpi/madt_parser.*, stage 9). See DECISIONS.md, stage 11.
namespace acpi {

constexpr std::uint32_t kProcessorAffinityFlagEnabled = 1u << 0;
constexpr std::uint32_t kMemoryAffinityFlagEnabled = 1u << 0;
constexpr std::uint32_t kMemoryAffinityFlagHotPluggable = 1u << 1;
constexpr std::uint32_t kMemoryAffinityFlagNonVolatile = 1u << 2;

// Type 0: Processor Local APIC/SAPIC Affinity Structure. Ties a Local
// APIC ID — the same 8-bit ID as MadtParseResult::LocalApicEntry::apic_id
// — to a proximity domain (NUMA node). Only valid for APIC IDs < 255;
// a CPU identified via Local x2APIC (type 2, skipped — see below) would
// need a separate lookup, but this project's `-smp 4` never has an APIC
// ID anywhere near that boundary, same reasoning stage 9 already applied
// to the MADT's own type 9 x2APIC entries.
struct ProcessorAffinityEntry {
    std::uint8_t apic_id;
    std::uint32_t proximity_domain;
    std::uint32_t flags;
};

// Type 1: Memory Affinity Structure. Ties a physical memory range to a
// proximity domain.
struct MemoryAffinityEntry {
    std::uint32_t proximity_domain;
    std::uint64_t base_address;
    std::uint64_t length;
    std::uint32_t flags;
};

// Fixed-capacity, no dynamic allocation — this runs during early boot,
// before the heap exists. Capacities are generous relative to any
// realistic QEMU `-numa` configuration this project will ever be run
// with, not sized against a specific config (mirrors MadtParseResult's
// kMaxLapics/kMaxIoapics choice).
struct SratParseResult {
    static constexpr int kMaxProcessorAffinities = 64;
    static constexpr int kMaxMemoryAffinities = 16;

    ProcessorAffinityEntry processor_affinities[kMaxProcessorAffinities];
    int processor_affinity_count = 0;
    MemoryAffinityEntry memory_affinities[kMaxMemoryAffinities];
    int memory_affinity_count = 0;
    // True if a Processor Affinity (type 0) or Memory Affinity (type 1)
    // entry was seen but its array was already full — never silently
    // drops data without saying so.
    bool overflowed = false;
};

// `entries`/`length` is exactly the byte range following the SRAT's fixed
// 12-byte header (a 4-byte reserved field followed by an 8-byte reserved
// field, per spec, after the common 36-byte SdtHeader) — i.e. the table's
// variable-length entry list, not the whole SRAT. Recognizes type 0
// (Processor Local APIC/SAPIC Affinity) and type 1 (Memory Affinity);
// type 2 (Processor Local x2APIC Affinity) and every other type (GICC,
// GIC ITS, Generic Initiator, etc. — all ARM/accelerator-specific, none
// of which this x86_64-only project can ever exercise) are skipped via
// their own length field rather than parsed. A malformed entry (length <
// 2, or a length that would run past the end of the buffer) stops the
// walk rather than reading out of bounds; whatever was parsed before
// that point is still returned.
void parse_srat_entries(const std::uint8_t* entries, std::size_t length, SratParseResult& out);

} // namespace acpi
