#include "pmm.hpp"

#include "acpi.hpp"
#include "bitmap_allocator.hpp"
#include "numa_map.hpp"
#include "serial.hpp"

#include <new>

namespace pmm {

namespace {

// Caps how much physical memory this allocator can track — see
// ASSUMPTIONS.md, "physical memory manager capped at 4 GiB". Sized
// statically so the bitmap needs no allocator of its own to come into
// existence (128 KiB of BSS, zero cost in the image file).
constexpr std::size_t kMaxManagedBytes = 4ULL * 1024 * 1024 * 1024;
constexpr std::size_t kMaxManagedPages = kMaxManagedBytes / kPageSize;
constexpr std::size_t kBitmapBytes = kMaxManagedPages / 8;

std::uint8_t g_bitmap_storage[kBitmapBytes];

// GetMemoryMap's own buffer. Sized generously (typical QEMU/OVMF maps are
// a few dozen descriptors); GetMemoryMap reports the size actually needed
// if this is too small, so undersizing fails loud rather than corrupting
// anything, but is worth avoiding regardless.
constexpr std::size_t kMemoryMapBufferBytes = 16384;
std::uint8_t g_memory_map_buffer[kMemoryMapBufferBytes];

BitmapAllocator* g_allocator = nullptr;
alignas(BitmapAllocator) std::uint8_t g_allocator_storage[sizeof(BitmapAllocator)];

// NUMA state, all left at "no topology known" until init_numa() runs
// (see pmm.hpp — that's a legitimate, non-error state, not just an
// uninitialized one). Capacity is a generous static bound on distinct
// NUMA node IDs, same "generous static bound" style as
// MadtParseResult::kMaxLapics/SratParseResult::kMax* — comfortably
// beyond anything this project's QEMU `-numa` config will ever use.
constexpr std::size_t kMaxNumaNodes = 8;

NumaMap g_numa_map;
std::size_t g_free_pages_by_node[kMaxNumaNodes] = {};
std::size_t g_numa_node_count = 0;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] pmm::init: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

// Adjusts the given node's free-page counter, if it maps to a node this
// project actually tracks — a no-op for NumaMap::kUnknownNode (a page
// outside every SRAT-described range) or a node ID beyond kMaxNumaNodes.
// Neither is an error: init_numa() is never called at all on hardware/
// QEMU configs with no SRAT, and every alloc_page()/free_page()/etc. call
// site below must stay correct with no NUMA data present, not treat that
// as a special case.
void adjust_node_free_count(std::uint32_t node, int delta) {
    if (node == NumaMap::kUnknownNode || node >= kMaxNumaNodes) {
        return;
    }
    g_free_pages_by_node[node] = static_cast<std::size_t>(
        static_cast<std::ptrdiff_t>(g_free_pages_by_node[node]) + delta);
}

efi::MemoryDescriptor* descriptor_at(std::uint8_t* base, std::size_t index,
                                      std::uintptr_t descriptor_size) {
    return reinterpret_cast<efi::MemoryDescriptor*>(base + index * descriptor_size);
}

} // namespace

void init(efi::Handle image_handle, efi::SystemTable* system_table) {
    if (system_table->hdr.signature != efi::kSystemTableSignature) {
        panic("EFI_SYSTEM_TABLE signature mismatch");
    }
    efi::BootServices* bs = system_table->boot_services;
    if (bs->hdr.signature != efi::kBootServicesSignature) {
        panic("EFI_BOOT_SERVICES signature mismatch");
    }

    std::uintptr_t map_size = kMemoryMapBufferBytes;
    std::uintptr_t map_key = 0;
    std::uintptr_t descriptor_size = 0;
    std::uint32_t descriptor_version = 0;

    efi::Status status = bs->get_memory_map(
        &map_size, reinterpret_cast<efi::MemoryDescriptor*>(g_memory_map_buffer), &map_key,
        &descriptor_size, &descriptor_version);
    if (status != efi::kSuccess) {
        panic("GetMemoryMap failed");
    }
    // Deliberately no cli here: UEFI's own GDT/IDT are still fully active
    // (kernel_main doesn't install ours until after this function
    // returns), so firmware remains free to service its own interrupts
    // internally, exactly as it expects to be able to until
    // ExitBootServices succeeds. See DECISIONS.md, stage 3.
    serial::puts("[OK] memmap\r\n");

    std::size_t descriptor_count = map_size / descriptor_size;

    // Pass 1: find the highest physical address that's actually backed by
    // RAM (efi::is_real_ram — an allow-list, not a deny-list; see its
    // comment for why), to size the allocator/direct-map for what's
    // genuinely installed rather than every address UEFI happens to
    // describe.
    std::uintptr_t highest_address = 0;
    for (std::size_t i = 0; i < descriptor_count; i++) {
        efi::MemoryDescriptor* d = descriptor_at(g_memory_map_buffer, i, descriptor_size);
        if (!efi::is_real_ram(d->type)) {
            continue;
        }
        std::uintptr_t end = d->physical_start + d->number_of_pages * kPageSize;
        if (end > highest_address) {
            highest_address = end;
        }
    }
    std::size_t page_count = highest_address / kPageSize;
    if (page_count > kMaxManagedPages) {
        page_count = kMaxManagedPages; // see ASSUMPTIONS.md — excess memory is left untracked
    }

    g_allocator = new (g_allocator_storage) BitmapAllocator(g_bitmap_storage, page_count);

    // Pass 2: mark free exactly what the firmware says is genuinely free.
    // Everything else (firmware code/data, ACPI, MMIO, our own loaded
    // image) stays used — see DECISIONS.md for why boot-services memory
    // isn't reclaimed.
    for (std::size_t i = 0; i < descriptor_count; i++) {
        efi::MemoryDescriptor* d = descriptor_at(g_memory_map_buffer, i, descriptor_size);
        if (d->type == efi::kEfiConventionalMemory) {
            std::size_t start_page = d->physical_start / kPageSize;
            g_allocator->mark_free_range(start_page, d->number_of_pages);
        }
    }
    // Page 0 is kept reserved regardless of what the map says, so 0
    // unambiguously means "allocation failed" to callers of alloc_page().
    g_allocator->mark_used(0);

    // GetMemoryMap must be the last allocation-affecting call before
    // ExitBootServices; if the map changed between the two (MapKey stale),
    // ExitBootServices fails and the whole dance must be redone.
    while (true) {
        status = bs->exit_boot_services(image_handle, map_key);
        if (status == efi::kSuccess) {
            break;
        }
        status = bs->get_memory_map(&map_size,
                                     reinterpret_cast<efi::MemoryDescriptor*>(g_memory_map_buffer),
                                     &map_key, &descriptor_size, &descriptor_version);
        if (status != efi::kSuccess) {
            panic("GetMemoryMap retry failed");
        }
    }
    serial::puts("[OK] exitbs\r\n");
}

std::uintptr_t alloc_page() {
    std::size_t idx = g_allocator->alloc();
    if (idx == BitmapAllocator::kInvalidPage) {
        return 0;
    }
    adjust_node_free_count(g_numa_map.node_of(idx), -1);
    return static_cast<std::uintptr_t>(idx) * kPageSize;
}

void free_page(std::uintptr_t physical_address) {
    std::size_t idx = static_cast<std::size_t>(physical_address / kPageSize);
    // Read the prior state before freeing: free() (like mark_free) is
    // idempotent, so a double-free must not double-count the node's
    // free-page total either.
    bool was_free = g_allocator->is_free(idx);
    g_allocator->free(idx);
    if (!was_free) {
        adjust_node_free_count(g_numa_map.node_of(idx), 1);
    }
}

void reserve_page(std::uintptr_t physical_address) {
    std::size_t idx = static_cast<std::size_t>(physical_address / kPageSize);
    bool was_free = g_allocator->is_free(idx);
    g_allocator->mark_used(idx);
    if (was_free) {
        adjust_node_free_count(g_numa_map.node_of(idx), -1);
    }
}

std::uintptr_t alloc_contiguous_pages(std::size_t count) {
    std::size_t idx = g_allocator->alloc_contiguous(count);
    if (idx == BitmapAllocator::kInvalidPage) {
        return 0;
    }
    // A contiguous run can in principle straddle a node boundary (even
    // though this project's own `-numa` config never produces one small
    // enough to matter) — charge each page to whatever node it actually
    // belongs to, not just the first page's node.
    for (std::size_t i = 0; i < count; i++) {
        adjust_node_free_count(g_numa_map.node_of(idx + i), -1);
    }
    return static_cast<std::uintptr_t>(idx) * kPageSize;
}

std::size_t total_pages() { return g_allocator->page_count(); }
std::size_t free_pages() { return g_allocator->free_count(); }

void init_numa() {
    if (!acpi::srat_present()) {
        return; // no SRAT — node_count() stays 0, see pmm.hpp
    }

    const acpi::SratParseResult& srat = acpi::srat();
    for (int i = 0; i < srat.memory_affinity_count; i++) {
        const acpi::MemoryAffinityEntry& m = srat.memory_affinities[i];
        // Skip disabled entries (real QEMU SRATs include an all-zero
        // placeholder — see DECISIONS.md, stage 11) and any zero-length
        // range; NumaMap::add_region already drops the latter, but
        // checking the flag here too means a disabled-but-nonzero-length
        // entry (spec-legal, just not something this project's QEMU
        // config produces) is never mistaken for real, allocatable
        // memory.
        if ((m.flags & acpi::kMemoryAffinityFlagEnabled) == 0) {
            continue;
        }
        std::size_t start_page = static_cast<std::size_t>(m.base_address / kPageSize);
        std::size_t page_count_in_region = static_cast<std::size_t>(m.length / kPageSize);
        g_numa_map.add_region(start_page, page_count_in_region, m.proximity_domain);
    }

    g_numa_node_count = g_numa_map.node_count();
    if (g_numa_node_count > kMaxNumaNodes) {
        g_numa_node_count = kMaxNumaNodes; // see kMaxNumaNodes
    }

    // SRAT ranges and the EFI memory map's EfiConventionalMemory ranges
    // (what init()'s Pass 2 actually marked free) are independent
    // partitions of the address space with no guaranteed alignment
    // between them — computing each node's free-page count by scanning
    // what the bitmap now genuinely has marked free, rather than
    // assuming the two agree, is the only way to get a correct answer.
    for (int i = 0; i < g_numa_map.region_count(); i++) {
        const NumaMap::Region& r = g_numa_map.region(i);
        if (r.node >= kMaxNumaNodes) {
            continue;
        }
        for (std::size_t page = r.start_page; page < r.start_page + r.page_count; page++) {
            if (g_allocator->is_free(page)) {
                g_free_pages_by_node[r.node]++;
            }
        }
    }

    serial::puts("[OK] pmm numa nodes=");
    serial::put_hex(static_cast<std::uint64_t>(g_numa_node_count));
    serial::puts("\r\n");
    for (std::size_t node = 0; node < g_numa_node_count; node++) {
        serial::puts("[OK] pmm numa node=");
        serial::put_hex(static_cast<std::uint64_t>(node));
        serial::puts(" free_pages=");
        serial::put_hex(static_cast<std::uint64_t>(g_free_pages_by_node[node]));
        serial::puts("\r\n");
    }
}

std::size_t node_count() { return g_numa_node_count; }

std::size_t free_pages_on_node(std::size_t node) {
    if (node >= kMaxNumaNodes) {
        return 0;
    }
    return g_free_pages_by_node[node];
}

std::uintptr_t alloc_page_on_node(std::size_t node) {
    if (node >= kMaxNumaNodes) {
        return 0;
    }
    for (int i = 0; i < g_numa_map.region_count(); i++) {
        const NumaMap::Region& r = g_numa_map.region(i);
        if (r.node != node) {
            continue;
        }
        std::size_t idx = g_allocator->alloc_in_range(r.start_page, r.page_count);
        if (idx != BitmapAllocator::kInvalidPage) {
            adjust_node_free_count(static_cast<std::uint32_t>(node), -1);
            return static_cast<std::uintptr_t>(idx) * kPageSize;
        }
    }
    return 0;
}

std::size_t node_for_cpu(std::uint32_t apic_id) {
    if (!acpi::srat_present()) {
        return kUnknownNode;
    }
    const acpi::SratParseResult& srat = acpi::srat();
    for (int i = 0; i < srat.processor_affinity_count; i++) {
        const acpi::ProcessorAffinityEntry& p = srat.processor_affinities[i];
        if (p.apic_id != apic_id || (p.flags & acpi::kProcessorAffinityFlagEnabled) == 0) {
            continue;
        }
        return p.proximity_domain < kMaxNumaNodes ? p.proximity_domain : kUnknownNode;
    }
    return kUnknownNode;
}

std::uintptr_t alloc_contiguous_pages_on_node(std::size_t node, std::size_t count) {
    if (node >= kMaxNumaNodes) {
        return 0;
    }
    for (int i = 0; i < g_numa_map.region_count(); i++) {
        const NumaMap::Region& r = g_numa_map.region(i);
        if (r.node != node) {
            continue;
        }
        std::size_t idx = g_allocator->alloc_contiguous_in_range(r.start_page, r.page_count, count);
        if (idx != BitmapAllocator::kInvalidPage) {
            adjust_node_free_count(static_cast<std::uint32_t>(node), -static_cast<int>(count));
            return static_cast<std::uintptr_t>(idx) * kPageSize;
        }
    }
    return 0;
}

} // namespace pmm
