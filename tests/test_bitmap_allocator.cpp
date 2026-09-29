#include <catch2/catch_test_macros.hpp>

#include "bitmap_allocator.hpp"

using pmm::BitmapAllocator;

TEST_CASE("starts with everything used", "[bitmap]") {
    std::uint8_t bitmap[4] = {0xFF, 0xFF, 0xFF, 0xFF}; // deliberately non-zero
    BitmapAllocator alloc(bitmap, 32);

    REQUIRE(alloc.page_count() == 32);
    REQUIRE(alloc.free_count() == 0);
    for (std::size_t i = 0; i < 32; i++) {
        REQUIRE_FALSE(alloc.is_free(i));
    }
}

TEST_CASE("mark_free then alloc returns that page", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);

    alloc.mark_free(5);
    REQUIRE(alloc.is_free(5));
    REQUIRE(alloc.free_count() == 1);

    std::size_t got = alloc.alloc();
    REQUIRE(got == 5);
    REQUIRE_FALSE(alloc.is_free(5));
    REQUIRE(alloc.free_count() == 0);
}

TEST_CASE("alloc skips used pages and never double-allocates", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t seen[32];
    for (int i = 0; i < 32; i++) {
        seen[i] = alloc.alloc();
        REQUIRE(seen[i] != BitmapAllocator::kInvalidPage);
    }
    REQUIRE(alloc.free_count() == 0);
    REQUIRE(alloc.alloc() == BitmapAllocator::kInvalidPage);

    for (int i = 0; i < 32; i++) {
        for (int j = i + 1; j < 32; j++) {
            REQUIRE(seen[i] != seen[j]);
        }
    }
}

TEST_CASE("free makes a page available again", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t p = alloc.alloc();
    REQUIRE(alloc.free_count() == 31);
    alloc.free(p);
    REQUIRE(alloc.free_count() == 32);
    REQUIRE(alloc.is_free(p));
}

TEST_CASE("double free is a safe no-op, not corruption", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t p = alloc.alloc();
    alloc.free(p);
    alloc.free(p); // double free
    alloc.free(p); // triple, for good measure
    REQUIRE(alloc.free_count() == 32);
}

TEST_CASE("mark_used_range reserves a range regardless of prior state", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);
    alloc.mark_used_range(10, 5); // reserve pages 10..14

    REQUIRE(alloc.free_count() == 27);
    for (std::size_t i = 10; i < 15; i++) {
        REQUIRE_FALSE(alloc.is_free(i));
    }
    REQUIRE(alloc.is_free(9));
    REQUIRE(alloc.is_free(15));
}

TEST_CASE("out-of-range indices are ignored, not undefined behavior", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);

    alloc.mark_free(1000);   // must not write out of bounds
    alloc.mark_used(1000);
    REQUIRE_FALSE(alloc.is_free(1000));
    REQUIRE(alloc.free_count() == 0);
}

TEST_CASE("alloc_contiguous returns a run of physically adjacent pages", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t start = alloc.alloc_contiguous(5);
    REQUIRE(start != BitmapAllocator::kInvalidPage);
    for (std::size_t i = 0; i < 5; i++) {
        REQUIRE_FALSE(alloc.is_free(start + i));
    }
    REQUIRE(alloc.free_count() == 27);
}

TEST_CASE("alloc_contiguous skips a used page in the middle of an otherwise-free range",
          "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);
    alloc.mark_used(3); // breaks the only run long enough to start at 0

    std::size_t start = alloc.alloc_contiguous(4);
    REQUIRE(start != BitmapAllocator::kInvalidPage);
    REQUIRE(start != 0); // can't start at 0: page 3 would be included and isn't free
    for (std::size_t i = 0; i < 4; i++) {
        REQUIRE(alloc.is_free(start + i) == false);
    }
}

TEST_CASE("alloc_contiguous fails without corrupting state when no run is long enough",
          "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    // Free only alternating pages — no run of 2 exists anywhere.
    for (std::size_t i = 0; i < 32; i += 2) {
        alloc.mark_free(i);
    }
    std::size_t before = alloc.free_count();

    REQUIRE(alloc.alloc_contiguous(2) == BitmapAllocator::kInvalidPage);
    REQUIRE(alloc.free_count() == before); // failed attempt must not partially allocate
}

TEST_CASE("alloc_in_range only allocates from within the given sub-range", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t got = alloc.alloc_in_range(16, 8); // pages [16, 24)
    REQUIRE(got >= 16);
    REQUIRE(got < 24);
    REQUIRE_FALSE(alloc.is_free(got));
    REQUIRE(alloc.free_count() == 31);
}

TEST_CASE("alloc_in_range fails without corrupting state when the sub-range is fully used",
          "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);
    alloc.mark_used_range(16, 8); // pages [16, 24) all used

    std::size_t before = alloc.free_count();
    REQUIRE(alloc.alloc_in_range(16, 8) == BitmapAllocator::kInvalidPage);
    REQUIRE(alloc.free_count() == before);
}

TEST_CASE("alloc_in_range never returns a page outside the requested sub-range", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);
    alloc.mark_used_range(16, 8); // only pages outside [16,24) are free

    std::size_t got = alloc.alloc_in_range(16, 8);
    REQUIRE(got == BitmapAllocator::kInvalidPage); // must not "spill over" to pages 0-15/24-31
}

TEST_CASE("alloc_in_range clamps to page_count() instead of reading out of bounds", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    std::size_t got = alloc.alloc_in_range(30, 100); // range runs past page_count()
    REQUIRE(got >= 30);
    REQUIRE(got < 32);
}

TEST_CASE("alloc search wraps around the hint instead of only scanning forward", "[bitmap]") {
    std::uint8_t bitmap[4] = {0};
    BitmapAllocator alloc(bitmap, 32);
    alloc.mark_free_range(0, 32);

    // Exhaust everything except page 3, by allocating in order and
    // freeing everything but page 3, then confirm alloc() still finds it
    // even though the search hint has moved past it.
    std::size_t pages[32];
    for (int i = 0; i < 32; i++) {
        pages[i] = alloc.alloc();
    }
    for (int i = 0; i < 32; i++) {
        if (pages[i] != 3) {
            alloc.free(pages[i]);
        }
    }
    REQUIRE(alloc.free_count() == 31);

    std::size_t got = alloc.alloc();
    REQUIRE(got != 3);
    REQUIRE(got != BitmapAllocator::kInvalidPage);
}
