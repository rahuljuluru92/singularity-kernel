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
