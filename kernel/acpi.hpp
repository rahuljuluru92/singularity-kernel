#pragma once

#include "efi.hpp"
#include "madt_parser.hpp"

// Finds the ACPI RSDP via UEFI's configuration table, walks the
// XSDT/MADT, and exposes the discovered Local APIC / I/O APIC entries.
// Everything OS/hardware-specific (physical-memory table layout,
// checksums, UEFI struct access) lives here; the actual MADT
// entry-by-entry walk is acpi::parse_madt_entries (acpi/madt_parser.*),
// which is pure logic and host-unit-tested separately.
namespace acpi {

// Finds the RSDP, walks the XSDT to the MADT, and parses it. Must run
// after ExitBootServices (pmm::init) and after paging::init() — it
// dereferences physical addresses directly, which only works once this
// kernel's own permanent identity-mapped page tables are active (see
// DECISIONS.md, stage 9). Panics (matching every other subsystem's
// verify-or-die style) if the RSDP, XSDT, or MADT can't be found or
// fails its checksum — there is no fallback path, and continuing with
// unverified ACPI data would make every later result about CPU topology
// unreliable in a way that's hard to diagnose after the fact.
void init(efi::SystemTable* system_table);

// Valid only after init(). Read-only access to what the MADT actually
// contained, for smp.cpp/ioapic.cpp to consume.
const MadtParseResult& madt();

} // namespace acpi
