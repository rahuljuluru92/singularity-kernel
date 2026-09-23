#include "gdt.hpp"

#include <cstdint>

extern "C" void gdt_flush(void* gdtr_ptr);

namespace {

struct [[gnu::packed]] gdtr {
    uint16_t limit;
    uint64_t base;
};

// Base/limit are ignored by the CPU for code/data segments in long mode,
// so these are the standard flat descriptors: null, ring-0 code (L=1,
// marking it a 64-bit code segment), ring-0 data.
constexpr uint64_t kGdt[3] = {
    0x0000000000000000,
    0x00A09A0000000000,
    0x00A0920000000000,
};

} // namespace

namespace gdt {

void init() {
    gdtr ptr{
        .limit = sizeof(kGdt) - 1,
        .base = reinterpret_cast<uint64_t>(&kGdt),
    };
    gdt_flush(&ptr);
}

} // namespace gdt
