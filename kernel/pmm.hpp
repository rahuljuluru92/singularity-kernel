#pragma once

#include "efi.hpp"

#include <cstddef>
#include <cstdint>

namespace pmm {

// Gets the UEFI memory map, calls ExitBootServices, and builds the global
// physical page allocator from the map's EfiConventionalMemory ranges.
// After this returns, UEFI Boot Services are gone — this is the actual
// "leave UEFI behind" moment deferred from stage 2. Must run before any
// other pmm:: call.
void init(efi::Handle image_handle, efi::SystemTable* system_table);

// Returns a physical address (page-aligned), or 0 on failure. 0 is never
// itself a valid allocatable address — see pmm.cpp, page 0 is always kept
// reserved.
std::uintptr_t alloc_page();
void free_page(std::uintptr_t physical_address);

std::size_t total_pages();
std::size_t free_pages();

} // namespace pmm
