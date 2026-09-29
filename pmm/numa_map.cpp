#include "numa_map.hpp"

namespace pmm {

void NumaMap::add_region(std::size_t start_page, std::size_t page_count, std::uint32_t node) {
    if (page_count == 0) {
        return;
    }
    if (region_count_ >= kMaxRegions) {
        overflowed_ = true;
        return;
    }
    regions_[region_count_++] = Region{start_page, page_count, node};
    if (node + 1 > node_count_) {
        node_count_ = node + 1;
    }
}

std::uint32_t NumaMap::node_of(std::size_t page_index) const {
    for (int i = 0; i < region_count_; i++) {
        const Region& r = regions_[i];
        if (page_index >= r.start_page && page_index < r.start_page + r.page_count) {
            return r.node;
        }
    }
    return kUnknownNode;
}

} // namespace pmm
