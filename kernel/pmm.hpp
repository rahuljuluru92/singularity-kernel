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

// Builds this allocator's NUMA node map from the ACPI SRAT (if one was
// found) and computes each node's initial free-page count. Must run
// after acpi::init() — it reads acpi::srat_present()/acpi::srat() — and
// after the ordinary init() above, since it classifies pages the bitmap
// already has marked free/used, not the other way around. A no-op
// (node_count() stays 0) if no SRAT was found: real single-node hardware,
// or this kernel run without the stage-11 `-numa` QEMU flags, legitimately
// has no NUMA topology to build.
void init_numa();

// 0 if init_numa() found no SRAT (no NUMA topology known) — every page
// is then equally "unknown node" and alloc_page_on_node/
// free_pages_on_node have nothing meaningful to report. Otherwise one
// past the highest node ID the SRAT described, same convention
// acpi::MadtParseResult/SratParseResult use for their own counts.
std::size_t node_count();

// Free pages currently classified to a specific node. 0 for any node >=
// node_count(), same "out of range is just empty" handling as
// BitmapAllocator's own out-of-range accesses.
std::size_t free_pages_on_node(std::size_t node);

// Allocates a page from the given NUMA node specifically, or returns 0 if
// that node currently has no free page — unlike alloc_page(), this never
// falls back to a different node; callers decide their own fallback
// policy (e.g. "any node" via plain alloc_page(), or panic).
std::uintptr_t alloc_page_on_node(std::size_t node);

} // namespace pmm
