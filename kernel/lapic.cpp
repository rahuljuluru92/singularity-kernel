#include "lapic.hpp"

#include "pit.hpp"
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
constexpr std::uint32_t kMsrLvtTimer = 0x832;
constexpr std::uint32_t kMsrTimerInitialCount = 0x838;
constexpr std::uint32_t kMsrTimerCurrentCount = 0x839;
constexpr std::uint32_t kMsrTimerDivideConfig = 0x83E;

constexpr std::uint64_t kTimerDivideBy16 = 0x3;
constexpr std::uint64_t kLvtTimerMasked = 1ULL << 16;
constexpr std::uint64_t kLvtTimerPeriodic = 1ULL << 17;

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

std::uint32_t id() {
    std::uint64_t raw = read_msr(kMsrId);
    std::uint32_t value = static_cast<std::uint32_t>(raw);
    // Sanity bound (kept deliberately after stage 10 debugging): this project never
    // configures more than a handful of cores. A value outside a very
    // generous bound almost certainly means the MSR read itself didn't
    // land where expected, not a real 64+-core machine.
    if (value >= 64 || (raw >> 32) != 0) {
        serial::lock();
        serial::puts("[PANIC] lapic: id() read implausible value raw=");
        serial::put_hex(raw);
        serial::puts("\r\n");
        serial::unlock();
        for (;;) {
            asm volatile("cli\n\thlt");
        }
    }
    return value;
}

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

std::uint32_t calibrate_and_arm_timer(std::uint32_t frequency_hz, std::uint8_t vector) {
    write_msr(kMsrTimerDivideConfig, kTimerDivideBy16);

    // One-shot, masked (so no interrupt is actually delivered from this
    // measurement run), counting down from the maximum possible value —
    // whatever's left when the PIT-measured window ends tells us how
    // fast this core's timer actually ticks.
    write_msr(kMsrLvtTimer, kLvtTimerMasked | vector);
    write_msr(kMsrTimerInitialCount, 0xFFFFFFFFULL);

    // Wait for a fixed, known slice of real time — 10 ms — measured
    // against the PIT's exact, documented clock rate, not assumed.
    // pit::read_counter() counts DOWN from 0xFFFF and wraps; a plain
    // modular difference correctly handles the single wrap this ~10 ms
    // window (well under the PIT's ~54.9 ms full-period at max reload)
    // could ever cross.
    constexpr std::uint32_t kCalibrationMillis = 10;
    std::uint32_t target_pit_ticks = (pit::kBaseFrequencyHz / 1000) * kCalibrationMillis;

    std::uint16_t pit_start = pit::read_counter();
    std::uint32_t elapsed_pit_ticks = 0;
    std::uint32_t spins = 0;
    constexpr std::uint32_t kCalibrationSpinLimit = 100'000'000;
    while (elapsed_pit_ticks < target_pit_ticks) {
        std::uint16_t pit_now = pit::read_counter();
        elapsed_pit_ticks = (static_cast<std::uint32_t>(pit_start) -
                              static_cast<std::uint32_t>(pit_now)) &
                             0xFFFFu;
        spins++;
        if (spins > kCalibrationSpinLimit) {
            panic("LAPIC timer calibration: PIT reference clock never advanced");
        }
    }

    std::uint32_t remaining = static_cast<std::uint32_t>(read_msr(kMsrTimerCurrentCount));
    std::uint32_t elapsed_lapic_ticks = 0xFFFFFFFFu - remaining;
    if (elapsed_lapic_ticks == 0) {
        panic("LAPIC timer calibration: timer did not advance during the measurement window");
    }

    std::uint32_t measured_frequency_hz = elapsed_lapic_ticks * (1000 / kCalibrationMillis);
    std::uint32_t periodic_initial_count = measured_frequency_hz / frequency_hz;
    if (periodic_initial_count == 0) {
        periodic_initial_count = 1; // degenerate only if frequency_hz is absurdly high
    }

    // Stop the one-shot countdown before reprogramming for periodic mode.
    write_msr(kMsrTimerInitialCount, 0);
    write_msr(kMsrLvtTimer, kLvtTimerPeriodic | vector); // unmasked
    write_msr(kMsrTimerInitialCount, periodic_initial_count);

    return measured_frequency_hz;
}

} // namespace lapic
