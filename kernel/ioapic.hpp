#pragma once

// I/O APIC discovery: maps the MMIO register window the MADT reported
// and reads back its ID/version/redirection-entry-count registers, as
// real proof the device is there and responding — not just that an
// address was found in a table. Stops there: this stage does not
// reprogram any redirection-table entry to actually route a device
// interrupt through it. See DECISIONS.md, stage 9, for why — in short,
// there's no per-core interrupt-routing policy yet to route TO (that's
// stage 10's per-core dispatcher), so the existing legacy-PIC-routed PIT
// timer stage 5 built is left exactly as-is.
namespace ioapic {

// Reads acpi::madt()'s first I/O APIC entry, maps its MMIO page via
// paging::map_mmio, and reads back IOAPICID/IOAPICVER. Panics if the
// MADT reported no I/O APIC at all — every machine this project targets
// (QEMU/OVMF q35) always has exactly one. Must run after acpi::init()
// and paging::init().
void init();

} // namespace ioapic
