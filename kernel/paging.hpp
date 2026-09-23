#pragma once

#include <cstdint>

namespace paging {

// Start of the canonical higher half for 4-level paging (bit 47 set,
// sign-extended) — the fixed offset init() maps every tracked physical
// page to, in addition to its original low identity address. Exposed so
// other kernel code (e.g. the heap) can translate a physical address pmm
// hands out into its higher-half virtual alias.
constexpr uint64_t kHigherHalfBase = 0xFFFF800000000000ULL;

inline uint64_t to_higher_half(uint64_t phys) { return phys + kHigherHalfBase; }

// Inverse of to_higher_half — but for ANY address currently valid in this
// kernel, not just ones already known to be higher-half. init() maps
// every physical page pmm tracks at BOTH aliases, so a virtual address
// below kHigherHalfBase is, by construction, identity-mapped (VA == PA);
// one at or above it is a higher-half alias (VA == PA + offset). This
// matters for DMA: a buffer's physical address is needed regardless of
// which alias the caller happened to have a pointer through — a kernel
// .bss/.data static (low alias) and a heap allocation (higher-half
// alias, since heap::init() backs it that way) both need to work.
inline uint64_t to_physical(uint64_t virt) {
    if (virt >= kHigherHalfBase) {
        return virt - kHigherHalfBase;
    }
    return virt;
}

// Builds a fresh set of page tables: a direct map of all physical memory
// pmm tracks, at both its original low (identity) address and a
// higher-half offset, with W^X enforced per page (the kernel's own .text
// range gets read+execute/non-writable; everything else gets
// read+write/non-executable). Switches CR3 to it, then transfers
// execution to the higher-half alias of the current code.
//
// Must run after pmm::init() — it allocates page-table frames from the
// physical allocator. See DECISIONS.md for why the low alias is kept
// permanently rather than torn down after the switch.
void init();

} // namespace paging
