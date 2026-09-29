#pragma once

#include <cstddef>
#include <cstdint>

namespace pmm {

// Maps a physical page index to a NUMA node (ACPI proximity domain), from
// a fixed list of contiguous page ranges — pure logic over plain data, no
// OS or hardware dependency, so it's host-unit-tested
// (tests/test_numa_map.cpp) before ever being built from a real ACPI SRAT
// by kernel/pmm.cpp. Same "pure logic first" pattern as BitmapAllocator
// and the ACPI parsers (acpi/madt_parser.*, acpi/srat_parser.*).
//
// A single NUMA node's memory can be described by more than one region:
// ACPI's SRAT splits a node's RAM around reserved holes (e.g. the legacy
// 0xA0000-0x100000 ISA hole) rather than reporting one contiguous range
// per node — see DECISIONS.md, stage 11's SRAT entry. So this is a flat
// list of regions, not one range per node, and node_of does a linear
// scan over all of them.
class NumaMap {
  public:
    static constexpr int kMaxRegions = 16;
    static constexpr std::uint32_t kUnknownNode = static_cast<std::uint32_t>(-1);

    struct Region {
        std::size_t start_page;
        std::size_t page_count;
        std::uint32_t node;
    };

    NumaMap() = default;

    // Adds one contiguous page range belonging to `node`. Silently
    // dropped (see overflowed()) if capacity is already exhausted, or if
    // page_count is 0 — an empty region carries no information and
    // would otherwise cost a wasted slot (real SRAT data includes
    // exactly this: a disabled, all-zero placeholder entry — see
    // DECISIONS.md, stage 11).
    void add_region(std::size_t start_page, std::size_t page_count, std::uint32_t node);

    // Returns the node the given page belongs to, or kUnknownNode if no
    // added region covers it — legitimate if the page lies outside every
    // described NUMA range (e.g. it's reserved/MMIO, never handed to a
    // node in the first place).
    std::uint32_t node_of(std::size_t page_index) const;

    // One past the highest node ID any added region used, or 0 if no
    // regions were added. Sized this way (not "count of distinct nodes
    // seen") so callers can size a flat per-node array directly, the
    // same convention MadtParseResult/SratParseResult use for their own
    // counts.
    std::uint32_t node_count() const { return node_count_; }

    int region_count() const { return region_count_; }
    const Region& region(int index) const { return regions_[index]; }
    bool overflowed() const { return overflowed_; }

  private:
    Region regions_[kMaxRegions];
    int region_count_ = 0;
    std::uint32_t node_count_ = 0;
    bool overflowed_ = false;
};

} // namespace pmm
