#include "heap.hpp"

#include "bitmap_allocator.hpp" // for pmm::kPageSize
#include "freelist_allocator.hpp"
#include "paging.hpp"
#include "pmm.hpp"
#include "serial.hpp"

#include <new>

namespace heap {

namespace {

constexpr std::size_t kInitialHeapPages = 1024; // 4 MiB — see ASSUMPTIONS.md

FreelistAllocator* g_allocator = nullptr;
alignas(FreelistAllocator) std::uint8_t g_allocator_storage[sizeof(FreelistAllocator)];

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] heap: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

} // namespace

void init() {
    g_allocator = new (g_allocator_storage) FreelistAllocator();

    // pmm::alloc_page gives no contiguity guarantee, so pages are grouped
    // into contiguous runs as they arrive and handed to the allocator as
    // one region per run, rather than assuming the whole request lands
    // in one block.
    std::size_t added_pages = 0;
    std::uintptr_t run_start = 0;
    std::size_t run_pages = 0;

    auto flush_run = [&]() {
        if (run_pages == 0) {
            return;
        }
        void* region = reinterpret_cast<void*>(paging::to_higher_half(run_start));
        g_allocator->add_region(region, run_pages * pmm::kPageSize);
        run_pages = 0;
    };

    while (added_pages < kInitialHeapPages) {
        std::uintptr_t phys = pmm::alloc_page();
        if (phys == 0) {
            panic("pmm ran out of pages while growing the initial heap");
        }
        if (run_pages > 0 && phys == run_start + run_pages * pmm::kPageSize) {
            run_pages++;
        } else {
            flush_run();
            run_start = phys;
            run_pages = 1;
        }
        added_pages++;
    }
    flush_run();

    serial::puts("[OK] heap\r\n");
}

void* kmalloc(std::size_t size) { return g_allocator->alloc(size); }

void kfree(void* ptr) {
    if (ptr == nullptr) {
        return;
    }
    if (!g_allocator->free(ptr)) {
        panic("kfree: corruption detected (bad magic)");
    }
}

std::size_t free_bytes() { return g_allocator->free_bytes(); }
std::size_t used_bytes() { return g_allocator->used_bytes(); }

} // namespace heap
