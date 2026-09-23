#include "pmm.hpp"

#include "bitmap_allocator.hpp"
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

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] pmm::init: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
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
    return static_cast<std::uintptr_t>(idx) * kPageSize;
}

void free_page(std::uintptr_t physical_address) {
    g_allocator->free(static_cast<std::size_t>(physical_address / kPageSize));
}

std::uintptr_t alloc_contiguous_pages(std::size_t count) {
    std::size_t idx = g_allocator->alloc_contiguous(count);
    if (idx == BitmapAllocator::kInvalidPage) {
        return 0;
    }
    return static_cast<std::uintptr_t>(idx) * kPageSize;
}

std::size_t total_pages() { return g_allocator->page_count(); }
std::size_t free_pages() { return g_allocator->free_count(); }

} // namespace pmm
