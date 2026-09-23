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

// Registers one additional physical page that init() should map
// read+write+execute — present, writable, AND executable, all three at
// once — instead of the usual read+write/NX default. This is a
// deliberate, narrow W^X exception, unlike literally every other page
// in this kernel (including the kernel's own .text, which is
// read+execute but never writable). Must be called before init() —
// permissions are decided once, per page, while init() builds the
// tables; there's no re-walk afterward. Only ever needed for the fixed
// low-memory AP trampoline page (kernel/smp.hpp) so far: the BSP must
// write (copy + patch) it, and the AP itself must execute from that
// exact same physical page in the instant right after it enables
// paging, before it's reached any other executable memory. See
// DECISIONS.md, stage 9.
void add_executable_page(uint64_t phys);

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

// Maps one additional page at its physical address (identity alias only
// — MMIO windows like the I/O APIC's aren't RAM pmm tracks, so they were
// never covered by init()'s all-tracked-pages loop, and there's no
// meaningful "higher-half alias of a device register" for anything that
// wants one). Present + writable + NX, matching how init() treats every
// non-.text page. Must run after init() — reuses its page tables. phys
// is truncated down to its containing page if not already page-aligned.
void map_mmio(uint64_t phys);

// The physical address of the root (PML4) table init() built and
// switched CR3 to. Valid only after init(). Every core shares this exact
// same table — the AP trampoline loads it into its own CR3 verbatim
// (see kernel/smp.cpp) rather than building a second, redundant set of
// tables, since the BSP's already cover all of physical memory this
// kernel tracks.
uint64_t root_table_physical();

} // namespace paging
