#include "acpi.hpp"

#include "serial.hpp"

namespace acpi {

namespace {

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] acpi: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

// ACPI 2.0+ RSDP (Root System Description Pointer) — the structure
// SystemTable::configuration_table's ACPI 2.0 GUID entry points at
// directly (not a pointer to a pointer).
struct [[gnu::packed]] Rsdp {
    char signature[8]; // "RSD PTR " (note the trailing space)
    std::uint8_t checksum;
    char oem_id[6];
    std::uint8_t revision;
    std::uint32_t rsdt_address;
    std::uint32_t length;
    std::uint64_t xsdt_address;
    std::uint8_t extended_checksum;
    std::uint8_t reserved[3];
};

// Common ACPI SDT (System Description Table) header — every ACPI table
// (XSDT, MADT, ...) starts with exactly this, per the ACPI spec.
struct [[gnu::packed]] SdtHeader {
    char signature[4];
    std::uint32_t length; // includes this header
    std::uint8_t revision;
    std::uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    std::uint32_t oem_revision;
    std::uint32_t creator_id;
    std::uint32_t creator_revision;
};

static_assert(sizeof(SdtHeader) == 36, "ACPI SDT header must be exactly 36 bytes");

bool signature_matches(const char* actual, const char* expected, std::size_t len) {
    for (std::size_t i = 0; i < len; i++) {
        if (actual[i] != expected[i]) {
            return false;
        }
    }
    return true;
}

// Every ACPI table (RSDP included) is self-checksummed: sum every byte
// in the table over its own stated length, and the low 8 bits must be 0.
// This is the spec's own integrity check, not something this project
// invented — verified here the same way pmm::init() verifies the
// EFI_SYSTEM_TABLE/EFI_BOOT_SERVICES signatures before trusting them.
bool checksum_ok(const void* data, std::size_t length) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    std::uint8_t sum = 0;
    for (std::size_t i = 0; i < length; i++) {
        sum = static_cast<std::uint8_t>(sum + bytes[i]);
    }
    return sum == 0;
}

const Rsdp* find_rsdp(efi::SystemTable* system_table) {
    for (std::uintptr_t i = 0; i < system_table->number_of_table_entries; i++) {
        const efi::ConfigurationTable& entry = system_table->configuration_table[i];
        if (entry.vendor_guid == efi::kAcpi20TableGuid) {
            return reinterpret_cast<const Rsdp*>(entry.vendor_table);
        }
    }
    return nullptr;
}

MadtParseResult g_madt_result;

} // namespace

void init(efi::SystemTable* system_table) {
    const Rsdp* rsdp = find_rsdp(system_table);
    if (rsdp == nullptr) {
        panic("ACPI 2.0 RSDP not found in UEFI configuration table");
    }
    if (!signature_matches(rsdp->signature, "RSD PTR ", 8)) {
        panic("RSDP signature mismatch");
    }
    // Two separate checksums, per spec: the first 20 bytes (the ACPI 1.0
    // portion) must independently sum to 0, and the full `length`-byte
    // 2.0 structure must also sum to 0 — a 1.0-only OS only ever checks
    // the first; this project requires both since it only supports 2.0+.
    if (!checksum_ok(rsdp, 20)) {
        panic("RSDP (ACPI 1.0 region) checksum mismatch");
    }
    if (rsdp->revision < 2) {
        panic("RSDP revision < 2 (ACPI 1.0-only firmware not supported)");
    }
    if (!checksum_ok(rsdp, rsdp->length)) {
        panic("RSDP (full ACPI 2.0 structure) checksum mismatch");
    }

    // Identity mapping (see paging.cpp) makes this physical address
    // directly dereferenceable, same as every other raw-physical-pointer
    // access elsewhere in this codebase (e.g. pmm's memory map descriptors).
    const auto* xsdt = reinterpret_cast<const SdtHeader*>(rsdp->xsdt_address);
    if (!signature_matches(xsdt->signature, "XSDT", 4)) {
        panic("XSDT signature mismatch");
    }
    if (!checksum_ok(xsdt, xsdt->length)) {
        panic("XSDT checksum mismatch");
    }

    const auto* entries =
        reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(xsdt) +
                                                 sizeof(SdtHeader));
    std::size_t entry_count = (xsdt->length - sizeof(SdtHeader)) / sizeof(std::uint64_t);

    const SdtHeader* madt = nullptr;
    for (std::size_t i = 0; i < entry_count; i++) {
        const auto* candidate = reinterpret_cast<const SdtHeader*>(entries[i]);
        if (signature_matches(candidate->signature, "APIC", 4)) {
            madt = candidate;
            break;
        }
    }
    if (madt == nullptr) {
        panic("MADT (APIC table) not found in XSDT");
    }
    if (!checksum_ok(madt, madt->length)) {
        panic("MADT checksum mismatch");
    }

    // Fixed MADT header: SdtHeader (36 bytes) + local_apic_address (u32)
    // + flags (u32) = 44 bytes, then the variable-length entry list.
    constexpr std::size_t kMadtFixedHeaderSize = sizeof(SdtHeader) + 8;
    const auto* madt_bytes = reinterpret_cast<const std::uint8_t*>(madt);
    parse_madt_entries(madt_bytes + kMadtFixedHeaderSize, madt->length - kMadtFixedHeaderSize,
                        g_madt_result);

    serial::puts("[OK] acpi madt lapics=");
    serial::put_hex(static_cast<std::uint64_t>(g_madt_result.lapic_count));
    serial::puts(" ioapics=");
    serial::put_hex(static_cast<std::uint64_t>(g_madt_result.ioapic_count));
    serial::puts("\r\n");
}

const MadtParseResult& madt() { return g_madt_result; }

} // namespace acpi
