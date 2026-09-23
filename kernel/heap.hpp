#pragma once

#include <cstddef>

namespace heap {

// Grows the kernel heap by requesting pages from pmm (grouping physically
// contiguous runs into single regions, but not requiring contiguity —
// pmm's allocator gives no such guarantee) and handing them to a
// FreelistAllocator as their higher-half virtual alias. Must run after
// paging::init() (needs the higher-half mapping to exist) and pmm::init().
void init();

void* kmalloc(std::size_t size);
void kfree(void* ptr);

std::size_t free_bytes();
std::size_t used_bytes();

} // namespace heap
