#pragma once

#include "efi.hpp"
#include "madt_parser.hpp"
#include "srat_parser.hpp"

// Finds the ACPI RSDP via UEFI's configuration table, walks the
// XSDT/MADT/SRAT, and exposes the discovered Local APIC / I/O APIC /
// NUMA affinity entries. Everything OS/hardware-specific (physical-memory
// table layout, checksums, UEFI struct access) lives here; the actual
// MADT/SRAT entry-by-entry walks are acpi::parse_madt_entries and
// acpi::parse_srat_entries (acpi/madt_parser.*, acpi/srat_parser.*),
// which are pure logic and host-unit-tested separately.
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
//
// Also looks for the SRAT (System Resource Affinity Table) and parses it
// if present — but does NOT panic if it's missing, unlike the MADT.
// Unlike SMP bring-up, nothing before stage 11 depends on NUMA topology
// existing at all, and real single-node hardware (or a QEMU invocation
// with no `-numa` flags) legitimately has no SRAT. Callers check
// srat_present() before trusting srat().
void init(efi::SystemTable* system_table);

// Valid only after init(). Read-only access to what the MADT actually
// contained, for smp.cpp/ioapic.cpp to consume.
const MadtParseResult& madt();

// Valid only after init(). True if a checksum-valid SRAT was found in
// the XSDT.
bool srat_present();

// Valid only after init() and only meaningful if srat_present() is true;
// an empty result otherwise.
const SratParseResult& srat();

} // namespace acpi
