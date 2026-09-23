#include "ioapic.hpp"

#include "acpi.hpp"
#include "paging.hpp"
#include "serial.hpp"

#include <cstdint>

namespace ioapic {

namespace {

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] ioapic: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

// Indirect register access: write the register index to IOREGSEL (MMIO
// offset 0x00), then read/write the 32-bit value through IOWIN (offset
// 0x10). Both are 32-bit registers; the I/O APIC spec requires 32-bit
// accesses only.
constexpr std::uint32_t kRegSelOffset = 0x00;
constexpr std::uint32_t kRegWinOffset = 0x10;

constexpr std::uint32_t kRegId = 0x00;
constexpr std::uint32_t kRegVersion = 0x01;

std::uint32_t read_reg(volatile std::uint8_t* base, std::uint32_t reg) {
    *reinterpret_cast<volatile std::uint32_t*>(base + kRegSelOffset) = reg;
    return *reinterpret_cast<volatile std::uint32_t*>(base + kRegWinOffset);
}

} // namespace

void init() {
    const acpi::MadtParseResult& madt = acpi::madt();
    if (madt.ioapic_count == 0) {
        panic("MADT reported no I/O APIC");
    }
    const acpi::IoApicEntry& entry = madt.ioapics[0];

    paging::map_mmio(entry.address);
    auto* base = reinterpret_cast<volatile std::uint8_t*>(static_cast<std::uintptr_t>(entry.address));

    std::uint32_t id_reg = read_reg(base, kRegId);
    std::uint32_t version_reg = read_reg(base, kRegVersion);
    std::uint32_t version = version_reg & 0xFF;
    std::uint32_t max_redirection_entry = (version_reg >> 16) & 0xFF;

    serial::puts("[OK] ioapic id=");
    serial::put_hex((id_reg >> 24) & 0xF);
    serial::puts(" version=");
    serial::put_hex(version);
    serial::puts(" redirection_entries=");
    serial::put_hex(max_redirection_entry + 1);
    serial::puts("\r\n");
}

} // namespace ioapic
