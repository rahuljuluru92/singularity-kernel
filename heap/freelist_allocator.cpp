#include "freelist_allocator.hpp"

namespace heap {

void FreelistAllocator::freelist_push_front(BlockHeader* b) {
    b->prev_free = nullptr;
    b->next_free = free_list_head_;
    if (free_list_head_ != nullptr) {
        free_list_head_->prev_free = b;
    }
    free_list_head_ = b;
}

void FreelistAllocator::freelist_remove(BlockHeader* b) {
    if (b->prev_free != nullptr) {
        b->prev_free->next_free = b->next_free;
    } else {
        free_list_head_ = b->next_free;
    }
    if (b->next_free != nullptr) {
        b->next_free->prev_free = b->prev_free;
    }
}

// Merges b with its physical successor, if that successor exists and is
// free. Used for both directions of coalescing in free(): forward
// directly on the freed block, and backward by calling this on
// block->prev_physical (which then absorbs the freed block as ITS
// successor) — same bookkeeping either way.
void FreelistAllocator::coalesce_forward(BlockHeader* b) {
    BlockHeader* next = b->next_physical;
    if (next == nullptr || !next->is_free) {
        return;
    }
    free_bytes_ -= next->size;
    freelist_remove(next);

    std::size_t reclaimed = sizeof(BlockHeader) + next->size;
    b->size += reclaimed;
    b->next_physical = next->next_physical;
    if (b->next_physical != nullptr) {
        b->next_physical->prev_physical = b;
    }
    free_bytes_ += reclaimed;
}

void FreelistAllocator::add_region(void* base, std::size_t size) {
    if (size < kMinUsefulRegion) {
        return;
    }
    auto* block = reinterpret_cast<BlockHeader*>(base);
    block->size = size - sizeof(BlockHeader);
    block->is_free = true;
    block->magic = kMagicFree;
    block->prev_physical = nullptr;
    block->next_physical = nullptr;

    freelist_push_front(block);
    free_bytes_ += block->size;
}

void* FreelistAllocator::alloc(std::size_t size) {
    if (size < kAlignment) {
        size = kAlignment;
    }
    size = (size + (kAlignment - 1)) & ~(kAlignment - 1);

    BlockHeader* block = free_list_head_;
    while (block != nullptr && block->size < size) {
        block = block->next_free;
    }
    if (block == nullptr) {
        return nullptr;
    }

    std::size_t original_size = block->size;
    free_bytes_ -= original_size;
    freelist_remove(block);

    if (original_size >= size + sizeof(BlockHeader) + kMinSplitPayload) {
        std::size_t leftover_size = original_size - size - sizeof(BlockHeader);
        auto* leftover = reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(block) +
                                                          sizeof(BlockHeader) + size);
        leftover->size = leftover_size;
        leftover->is_free = true;
        leftover->magic = kMagicFree;
        leftover->next_physical = block->next_physical;
        leftover->prev_physical = block;
        if (leftover->next_physical != nullptr) {
            leftover->next_physical->prev_physical = leftover;
        }
        block->next_physical = leftover;
        block->size = size;

        freelist_push_front(leftover);
        free_bytes_ += leftover_size;
    }

    block->is_free = false;
    block->magic = kMagicUsed;
    used_bytes_ += block->size;

    return reinterpret_cast<std::uint8_t*>(block) + sizeof(BlockHeader);
}

bool FreelistAllocator::free(void* ptr) {
    if (ptr == nullptr) {
        return true; // matches the usual free(nullptr)-is-a-no-op contract
    }
    auto* block =
        reinterpret_cast<BlockHeader*>(reinterpret_cast<std::uint8_t*>(ptr) - sizeof(BlockHeader));
    if (block->magic != kMagicUsed) {
        return false;
    }

    used_bytes_ -= block->size;
    block->is_free = true;
    block->magic = kMagicFree;
    freelist_push_front(block);
    free_bytes_ += block->size;

    coalesce_forward(block);
    if (block->prev_physical != nullptr && block->prev_physical->is_free) {
        coalesce_forward(block->prev_physical);
    }
    return true;
}

} // namespace heap
