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

// Marks a specific, already-known physical page permanently used, without
// going through alloc() — for physical addresses something outside this
// allocator's normal alloc/free traffic needs reserved (e.g. the fixed
// low-memory address the AP trampoline is copied to — see
// kernel/smp.hpp). Idempotent, like mark_used itself. Must run before
// anything else has a chance to alloc_page() the same address.
void reserve_page(std::uintptr_t physical_address);

// Returns the physical address of the first of `count` physically
// contiguous pages, or 0 on failure. For hardware (like a virtqueue)
// that's handed one base address and assumes everything after it is
// contiguous.
std::uintptr_t alloc_contiguous_pages(std::size_t count);

std::size_t total_pages();
std::size_t free_pages();

} // namespace pmm
