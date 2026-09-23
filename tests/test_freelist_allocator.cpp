#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>

#include "freelist_allocator.hpp"

using heap::FreelistAllocator;

namespace {
// Static, not stack, so a bug that writes out of bounds corrupts
// something detectable (adjacent static storage) rather than silently
// smashing the test runner's own stack frame.
alignas(16) uint8_t g_arena[8192];
} // namespace

TEST_CASE("alloc returns 16-byte aligned, non-null pointers", "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));

    void* p1 = a.alloc(1);
    void* p2 = a.alloc(100);
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(reinterpret_cast<uintptr_t>(p1) % 16 == 0);
    REQUIRE(reinterpret_cast<uintptr_t>(p2) % 16 == 0);
    REQUIRE(p1 != p2);
}

// free_bytes()+used_bytes() is NOT simply constant across arbitrary alloc
// sequences: every split carves a brand-new header out of what was
// previously counted as free payload, and that header's bytes are
// deliberately invisible to both counters (they're overhead, not payload)
// — so the sum legitimately drops by sizeof(header) per split and rises
// by sizeof(header) per coalesce. What IS always true is that it never
// exceeds the region size, and that it's exactly stable across a
// pattern that triggers neither: freeing a block with non-free
// neighbors (no coalesce) and re-allocating the exact same size into the
// same now-free block (no split needed, exact fit).
TEST_CASE("free_bytes + used_bytes is stable across repeated same-size alloc/free",
          "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));
    REQUIRE(a.free_bytes() + a.used_bytes() <= sizeof(g_arena));

    // Two allocations, not one: p1's immediate physical neighbor must be
    // USED (p2), not still-free leftover, or freeing p1 immediately
    // re-coalesces with its own split-off sibling — a real, correct
    // behavior, just not the "nothing changes" scenario this test wants.
    void* p1 = a.alloc(64);
    void* p2 = a.alloc(64);
    (void)p2;
    std::size_t sum = a.free_bytes() + a.used_bytes();

    REQUIRE(a.free(p1)); // p1's neighbor (p2) is used: no coalesce
    REQUIRE(a.free_bytes() + a.used_bytes() == sum);

    void* p1b = a.alloc(64); // exact-size reuse of the same freed block: no split
    REQUIRE(a.free_bytes() + a.used_bytes() == sum);
    REQUIRE(a.free(p1b));
}

TEST_CASE("allocations never overlap: written patterns survive interleaved traffic",
          "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));

    constexpr int kCount = 12;
    void* ptrs[kCount];
    std::size_t sizes[kCount] = {16, 32, 48, 16, 128, 64, 16, 256, 32, 16, 64, 96};

    for (int i = 0; i < kCount; i++) {
        ptrs[i] = a.alloc(sizes[i]);
        REQUIRE(ptrs[i] != nullptr);
        std::memset(ptrs[i], 0x11 * (i + 1), sizes[i]);
    }

    // Free every third one, then allocate more into the gaps, then verify
    // every SURVIVING buffer's pattern is still intact.
    bool freed[kCount] = {};
    for (int i = 0; i < kCount; i += 3) {
        REQUIRE(a.free(ptrs[i]));
        freed[i] = true;
    }
    for (int i = 0; i < 4; i++) {
        void* p = a.alloc(24);
        REQUIRE(p != nullptr);
        std::memset(p, 0xAA, 24);
    }

    for (int i = 0; i < kCount; i++) {
        if (freed[i]) {
            continue;
        }
        auto* bytes = reinterpret_cast<uint8_t*>(ptrs[i]);
        for (std::size_t j = 0; j < sizes[i]; j++) {
            REQUIRE(bytes[j] == static_cast<uint8_t>(0x11 * (i + 1)));
        }
    }
}

TEST_CASE("double free is detected and rejected, not silently corrupting state", "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));

    void* p = a.alloc(64);
    REQUIRE(a.free(p) == true);
    REQUIRE(a.free(p) == false); // magic no longer kMagicUsed — must be rejected
}

TEST_CASE("freeing nullptr is a safe no-op", "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));
    REQUIRE(a.free(nullptr) == true);
}

TEST_CASE("out of memory returns nullptr rather than corrupting anything", "[heap]") {
    FreelistAllocator a;
    a.add_region(g_arena, sizeof(g_arena));
    void* huge = a.alloc(sizeof(g_arena) * 2);
    REQUIRE(huge == nullptr);

    // The allocator must still work normally afterward.
    void* p = a.alloc(64);
    REQUIRE(p != nullptr);
}

// Traces through exact sizes so the outcome is hand-verifiable, not just
// "eventually some allocation succeeded": a region sized for precisely
// three 64-byte allocations with zero slack (336 bytes = 3 * (64 + 48
// header)) is fully consumed by three allocs, freed out of order to
// exercise BOTH forward and backward coalescing, then a single 200-byte
// allocation — bigger than any one original 64-byte block — must succeed,
// which is only possible if the three blocks actually merged into one.
TEST_CASE("freeing adjacent blocks coalesces them, proven by a bigger allocation "
          "than any single block", "[heap]") {
    alignas(16) uint8_t arena[336];
    FreelistAllocator a;
    a.add_region(arena, sizeof(arena));

    void* block_a = a.alloc(64);
    void* block_b = a.alloc(64);
    void* block_c = a.alloc(64);
    REQUIRE(block_a != nullptr);
    REQUIRE(block_b != nullptr);
    REQUIRE(block_c != nullptr);
    REQUIRE(a.free_bytes() == 0); // exact-fit region, nothing left over

    REQUIRE(a.free(block_b)); // middle first: no neighbor is free yet
    REQUIRE(a.free(block_a)); // forward-coalesces with b
    REQUIRE(a.free(block_c)); // backward-coalesces into the (a+b) block

    REQUIRE(a.free_bytes() == 288); // 3*64 + 2*48 reclaimed headers

    void* big = a.alloc(200);
    REQUIRE(big != nullptr);
}
