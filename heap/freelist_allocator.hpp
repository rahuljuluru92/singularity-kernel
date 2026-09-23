#pragma once

#include <cstddef>
#include <cstdint>

namespace heap {

// A first-fit, address-ordered free-list allocator with physical-neighbor
// coalescing. Like pmm::BitmapAllocator, this has no OS/hardware
// dependency — it only ever touches memory regions it's explicitly given
// via add_region — which is what makes it host-testable.
//
// Blocks (free or used) are singly-typed headers embedded directly before
// each region they describe, chained two ways: prev_physical/next_physical
// reflect actual memory adjacency within one region (used for coalescing),
// while prev_free/next_free thread only the free blocks across all regions
// (used for allocation search). Regions added via add_region need not be
// contiguous with each other; each keeps its own physical chain, capped
// with nullptr at both ends.
class FreelistAllocator {
  public:
    FreelistAllocator() = default;

    // Adds [base, base + size) as backing memory, as one large free block.
    // size must be large enough to hold at least one header plus a
    // minimal payload; smaller regions are rejected (see kMinUsefulRegion).
    void add_region(void* base, std::size_t size);

    // Returns a pointer to at least `size` usable, suitably-aligned bytes,
    // or nullptr if no free block is large enough.
    void* alloc(std::size_t size);

    // Returns false — and does NOT free the block — if the pointer's
    // header fails its corruption-canary check (a double-free, a foreign
    // pointer, or a buffer overrun from an adjacent allocation having
    // clobbered it). Callers should treat false as a serious bug, not
    // something to retry.
    bool free(void* ptr);

    // free_bytes() + used_bytes() is constant except when add_region
    // grows it — a useful invariant for corruption-detecting tests.
    std::size_t free_bytes() const { return free_bytes_; }
    std::size_t used_bytes() const { return used_bytes_; }

  private:
    struct BlockHeader {
        std::size_t size; // usable payload size, not including this header
        BlockHeader* prev_physical;
        BlockHeader* next_physical;
        BlockHeader* prev_free;
        BlockHeader* next_free;
        std::uint32_t magic;
        bool is_free;
    };

    static constexpr std::uint32_t kMagicUsed = 0xA110C000;
    static constexpr std::uint32_t kMagicFree = 0xF4EE0000;
    static constexpr std::size_t kAlignment = 16;
    static constexpr std::size_t kMinSplitPayload = 16;
    static constexpr std::size_t kMinUsefulRegion = sizeof(BlockHeader) + kMinSplitPayload;

    BlockHeader* free_list_head_ = nullptr;
    std::size_t free_bytes_ = 0;
    std::size_t used_bytes_ = 0;

    void freelist_push_front(BlockHeader* b);
    void freelist_remove(BlockHeader* b);
    void coalesce_forward(BlockHeader* b);
};

} // namespace heap
