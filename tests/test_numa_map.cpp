#include <catch2/catch_test_macros.hpp>

#include "numa_map.hpp"

using pmm::NumaMap;

TEST_CASE("a page outside every region is unknown", "[numa]") {
    NumaMap map;
    map.add_region(0, 100, 0);

    REQUIRE(map.node_of(150) == NumaMap::kUnknownNode);
    REQUIRE(map.region_count() == 1);
    REQUIRE(map.node_count() == 1);
}

TEST_CASE("a page inside a single region resolves to that region's node", "[numa]") {
    NumaMap map;
    map.add_region(100, 50, 3);

    REQUIRE(map.node_of(100) == 3);   // start, inclusive
    REQUIRE(map.node_of(149) == 3);   // end, inclusive
    REQUIRE(map.node_of(150) == NumaMap::kUnknownNode); // end, exclusive
    REQUIRE(map.node_count() == 4); // one past the highest node ID (3)
}

TEST_CASE("two regions belonging to the same node, split around a gap", "[numa]") {
    // Mirrors the real QEMU SRAT layout discovered in stage 11: node 0's
    // RAM split around the legacy ISA hole into two separate regions.
    NumaMap map;
    map.add_region(0, 0xA0, 0);        // [0, 0xA0) node 0
    map.add_region(0x100, 0x1F00, 0);  // [0x100, 0x2000) node 0, after a gap
    map.add_region(0x2000, 0x2000, 1); // [0x2000, 0x4000) node 1

    REQUIRE(map.node_of(0x50) == 0);
    REQUIRE(map.node_of(0xA0) == NumaMap::kUnknownNode); // inside the gap
    REQUIRE(map.node_of(0xFF) == NumaMap::kUnknownNode); // inside the gap
    REQUIRE(map.node_of(0x100) == 0);
    REQUIRE(map.node_of(0x1FFF) == 0);
    REQUIRE(map.node_of(0x2000) == 1);
    REQUIRE(map.node_of(0x3FFF) == 1);
    REQUIRE(map.region_count() == 3);
    REQUIRE(map.node_count() == 2);
}

TEST_CASE("a zero-length region is dropped, not stored as a wasted slot", "[numa]") {
    NumaMap map;
    map.add_region(0, 0, 5); // e.g. the real SRAT's disabled placeholder entry
    map.add_region(10, 5, 0);

    REQUIRE(map.region_count() == 1); // only the real region counted
    REQUIRE(map.node_count() == 1);   // the dropped node-5 region never counted either
    REQUIRE(map.node_of(0) == NumaMap::kUnknownNode);
}

TEST_CASE("more regions than the fixed capacity sets overflowed, doesn't crash", "[numa]") {
    constexpr int kCount = NumaMap::kMaxRegions + 3;
    NumaMap map;
    for (int i = 0; i < kCount; i++) {
        map.add_region(static_cast<std::size_t>(i) * 10, 10, 0);
    }

    REQUIRE(map.region_count() == NumaMap::kMaxRegions);
    REQUIRE(map.overflowed());
}

TEST_CASE("region() exposes exactly what add_region stored, in order", "[numa]") {
    NumaMap map;
    map.add_region(0, 10, 0);
    map.add_region(20, 30, 2);

    REQUIRE(map.region(0).start_page == 0);
    REQUIRE(map.region(0).page_count == 10);
    REQUIRE(map.region(0).node == 0);
    REQUIRE(map.region(1).start_page == 20);
    REQUIRE(map.region(1).page_count == 30);
    REQUIRE(map.region(1).node == 2);
}
