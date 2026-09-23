#pragma once

namespace paging {

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
