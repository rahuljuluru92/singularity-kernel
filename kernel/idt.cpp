#include "idt.hpp"

#include <cstdint>

extern "C" void* isr_stub_table[32];

namespace {

struct [[gnu::packed]] idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
};

struct [[gnu::packed]] idtr {
    uint16_t limit;
    uint64_t base;
};

constexpr int kExceptionVectors = 32;
constexpr uint16_t kKernelCodeSelector = 0x08;
constexpr uint8_t kInterruptGateFlags = 0x8E; // present, ring 0, 64-bit interrupt gate

idt_entry table[256];

void set_gate(int vector, uint64_t handler) {
    table[vector] = idt_entry{
        .offset_low = static_cast<uint16_t>(handler & 0xFFFF),
        .selector = kKernelCodeSelector,
        .ist = 0, // no IST stack switch — see DECISIONS.md
        .type_attributes = kInterruptGateFlags,
        .offset_mid = static_cast<uint16_t>((handler >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>((handler >> 32) & 0xFFFFFFFF),
        .reserved = 0,
    };
}

} // namespace

namespace idt {

void init() {
    for (int i = 0; i < kExceptionVectors; i++) {
        set_gate(i, reinterpret_cast<uint64_t>(isr_stub_table[i]));
    }

    idtr ptr{
        .limit = sizeof(table) - 1,
        .base = reinterpret_cast<uint64_t>(&table),
    };
    asm volatile("lidt %0" : : "m"(ptr));
}

} // namespace idt
