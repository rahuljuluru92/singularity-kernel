#include "bitmap_allocator.hpp"

namespace pmm {

namespace {

inline std::size_t byte_of(std::size_t page_index) { return page_index / 8; }
inline std::uint8_t bit_of(std::size_t page_index) {
    return static_cast<std::uint8_t>(1u << (page_index % 8));
}

} // namespace

BitmapAllocator::BitmapAllocator(std::uint8_t* bitmap, std::size_t page_count)
    : bitmap_(bitmap), page_count_(page_count), free_count_(0), next_search_hint_(0) {
    for (std::size_t i = 0; i < (page_count_ + 7) / 8; i++) {
        bitmap_[i] = 0; // everything starts used
    }
}

bool BitmapAllocator::is_free(std::size_t page_index) const {
    if (page_index >= page_count_) {
        return false;
    }
    return (bitmap_[byte_of(page_index)] & bit_of(page_index)) != 0;
}

void BitmapAllocator::mark_free(std::size_t page_index) {
    if (page_index >= page_count_) {
        return;
    }
    if (!is_free(page_index)) {
        bitmap_[byte_of(page_index)] |= bit_of(page_index);
        free_count_++;
    }
}

void BitmapAllocator::mark_used(std::size_t page_index) {
    if (page_index >= page_count_) {
        return;
    }
    if (is_free(page_index)) {
        bitmap_[byte_of(page_index)] &= static_cast<std::uint8_t>(~bit_of(page_index));
        free_count_--;
    }
}

void BitmapAllocator::mark_free_range(std::size_t start_page, std::size_t count) {
    for (std::size_t i = 0; i < count; i++) {
        mark_free(start_page + i);
    }
}

void BitmapAllocator::mark_used_range(std::size_t start_page, std::size_t count) {
    for (std::size_t i = 0; i < count; i++) {
        mark_used(start_page + i);
    }
}

std::size_t BitmapAllocator::alloc() {
    if (free_count_ == 0) {
        return kInvalidPage;
    }
    for (std::size_t offset = 0; offset < page_count_; offset++) {
        std::size_t idx = (next_search_hint_ + offset) % page_count_;
        if (is_free(idx)) {
            mark_used(idx);
            next_search_hint_ = idx + 1;
            return idx;
        }
    }
    return kInvalidPage; // unreachable if free_count_ bookkeeping is correct
}

void BitmapAllocator::free(std::size_t page_index) { mark_free(page_index); }

} // namespace pmm
