#pragma once

#include <cstddef>
#include <cstdint>

namespace pmm {

constexpr std::size_t kPageSize = 4096;

// A bitmap-backed physical page allocator, with no OS/hardware dependency
// — it only ever touches the buffer it's given. This is what makes it
// host-testable: the same translation unit is compiled once for the
// host-side unit tests and once into the freestanding kernel.
//
// One bit per page: 1 = free, 0 = used. The caller owns the bitmap's
// storage and must size it for at least ceil(page_count / 8) bytes.
class BitmapAllocator {
  public:
    static constexpr std::size_t kInvalidPage = static_cast<std::size_t>(-1);

    // Every page starts marked used; the caller marks known-free ranges
    // (e.g. from a UEFI memory map) explicitly via mark_free_range.
    BitmapAllocator(std::uint8_t* bitmap, std::size_t page_count);

    void mark_free(std::size_t page_index);
    void mark_used(std::size_t page_index);
    void mark_free_range(std::size_t start_page, std::size_t count);
    void mark_used_range(std::size_t start_page, std::size_t count);

    bool is_free(std::size_t page_index) const;

    // Returns the index of a newly-allocated page (now marked used), or
    // kInvalidPage if none are free.
    std::size_t alloc();

    // Returns the index of the first page of `count` newly-allocated,
    // physically contiguous pages, or kInvalidPage if no run that long
    // is free. Needed by anything (like a virtqueue) that hands a device
    // a single base physical address and relies on everything after it
    // being contiguous — plain alloc() gives no such guarantee between
    // separate calls.
    std::size_t alloc_contiguous(std::size_t count);

    // Returns the index of a newly-allocated page (now marked used) from
    // within [start_page, start_page + page_count), or kInvalidPage if
    // none in that sub-range are free. Indices at or past page_count()
    // are silently excluded from the search, same "ignore, don't crash"
    // handling as every other out-of-range access here. Doesn't touch or
    // consult next_search_hint_ — this is a secondary, range-targeted
    // path (e.g. "allocate on this NUMA node specifically"), not the
    // general-purpose path alloc() optimizes for.
    std::size_t alloc_in_range(std::size_t start_page, std::size_t page_count);

    // Idempotent, like mark_free — freeing an already-free (or
    // already-double-freed) page is a no-op, not an error. See
    // DECISIONS.md for why: no panic/error-reporting path exists at this
    // layer, and silently-safe was judged better than undefined behavior.
    void free(std::size_t page_index);

    std::size_t page_count() const { return page_count_; }
    std::size_t free_count() const { return free_count_; }

  private:
    std::uint8_t* bitmap_;
    std::size_t page_count_;
    std::size_t free_count_;
    std::size_t next_search_hint_;
};

} // namespace pmm
