#include "lapic.hpp"

#include "serial.hpp"

namespace lapic {

namespace {

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] lapic: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

std::uint64_t read_msr(std::uint32_t msr) {
    std::uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<std::uint64_t>(hi) << 32) | lo;
}

void write_msr(std::uint32_t msr, std::uint64_t value) {
    std::uint32_t lo = static_cast<std::uint32_t>(value & 0xFFFFFFFF);
    std::uint32_t hi = static_cast<std::uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

bool x2apic_supported() {
    std::uint32_t eax = 1, ebx = 0, ecx = 0, edx = 0;
    asm volatile("cpuid" : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx) : "a"(1));
    constexpr std::uint32_t kX2ApicBit = 1u << 21;
    return (ecx & kX2ApicBit) != 0;
}

constexpr std::uint32_t kApicBaseMsr = 0x1B;
constexpr std::uint64_t kApicBaseXApicEnable = 1ULL << 11;
constexpr std::uint64_t kApicBaseX2ApicEnable = 1ULL << 10;

// x2APIC registers are MSRs at 0x800 + (legacy xAPIC MMIO byte offset /
// 0x10) — see kernel/lapic.hpp for why this project only ever uses the
// x2APIC form, never the MMIO one.
constexpr std::uint32_t kMsrId = 0x802;
constexpr std::uint32_t kMsrSvr = 0x80F;
constexpr std::uint32_t kMsrEoi = 0x80B;
constexpr std::uint32_t kMsrIcr = 0x830;

constexpr std::uint64_t kSvrApicSoftwareEnable = 1ULL << 8;
constexpr std::uint8_t kSpuriousVector = 0xFF; // conventional choice; unused until an IOAPIC
                                                // redirection ever actually targets it

// ICR (Interrupt Command Register) bit layout, shared by INIT and SIPI:
// bits 0-7 vector, bits 8-10 delivery mode, bit 11 destination mode
// (0 = physical), bit 14 level (1 = assert), bit 15 trigger mode
// (0 = edge). In x2APIC mode the full 32-bit destination APIC ID lives
// in bits 32-63 of this single MSR — no 8-bit shorthand-destination
// limitation the legacy xAPIC MMIO ICR_HI/ICR_LO split has.
constexpr std::uint64_t kDeliveryModeInit = 0x5ULL << 8;
constexpr std::uint64_t kDeliveryModeStartup = 0x6ULL << 8;
constexpr std::uint64_t kLevelAssert = 1ULL << 14;

// No calibrated timer is available this early (the PIT is stage 5's, and
// isn't guaranteed to be safe to depend on for microsecond-scale delays
// during AP bring-up) — these are the same deliberately-uncalibrated
// "long enough in practice under QEMU/TCG" busy-wait style already used
// by kernel/main.cpp's demo-task spin(), scaled to roughly approximate
// the Intel-recommended 10 ms (INIT) / 200 us (between SIPIs) gaps. See
// ASSUMPTIONS.md.
void delay_loop(std::uint32_t iterations) {
    for (volatile std::uint32_t i = 0; i < iterations; i = i + 1) {
    }
}

} // namespace

void enable() {
    if (!x2apic_supported()) {
        panic("x2APIC not supported by this CPU (CPUID.1:ECX.21) — no xAPIC fallback");
    }

    std::uint64_t base = read_msr(kApicBaseMsr);
    write_msr(kApicBaseMsr, base | kApicBaseXApicEnable | kApicBaseX2ApicEnable);

    write_msr(kMsrSvr, kSvrApicSoftwareEnable | kSpuriousVector);
}

std::uint32_t id() { return static_cast<std::uint32_t>(read_msr(kMsrId)); }

void send_init_sipi_sipi(std::uint32_t target_apic_id, std::uint8_t vector) {
    std::uint64_t dest = static_cast<std::uint64_t>(target_apic_id) << 32;

    write_msr(kMsrIcr, dest | kDeliveryModeInit | kLevelAssert);
    delay_loop(20'000'000); // ~10 ms per the Intel MP init algorithm

    write_msr(kMsrIcr, dest | kDeliveryModeStartup | vector);
    delay_loop(400'000); // ~200 us

    write_msr(kMsrIcr, dest | kDeliveryModeStartup | vector);
    delay_loop(400'000);
}

void eoi() { write_msr(kMsrEoi, 0); }

} // namespace lapic
