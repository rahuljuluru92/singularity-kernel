#pragma once

#include <cstdint>

// Minimal hand-rolled UEFI type definitions — just enough to call
// GetMemoryMap and ExitBootServices. No gnu-efi/edk2 headers (see
// DECISIONS.md, stage 1, for why this project avoids that dependency).
//
// EFI_BOOT_SERVICES in particular is a long, spec-fixed-order list of
// function pointers; getting a single one wrong silently calls the wrong
// function through the vtable. Every field is listed (mostly as `void*`
// placeholders) rather than computing an offset by hand, so the layout is
// directly checkable against the UEFI spec's own field order. It's also
// runtime-checked: efi::verify() confirms both EFI_SYSTEM_TABLE's and
// EFI_BOOT_SERVICES's Hdr.Signature match the spec's known magic values,
// which only reads correctly if BootServices (and everything before it)
// is laid out at the offset the firmware actually put it at.
namespace efi {

#ifndef EFIAPI
#define EFIAPI __attribute__((ms_abi))
#endif

using Status = std::uintptr_t;
using Handle = void*;

constexpr Status kSuccess = 0;
constexpr Status kErrorBit = static_cast<Status>(1) << 63;
constexpr Status kBufferTooSmall = kErrorBit | 5;

constexpr std::uint64_t kSystemTableSignature = 0x5453595320494249ULL;   // "IBI SYST"
constexpr std::uint64_t kBootServicesSignature = 0x56524553544f4f42ULL; // "BOOTSERV"

struct TableHeader {
    std::uint64_t signature;
    std::uint32_t revision;
    std::uint32_t header_size;
    std::uint32_t crc32;
    std::uint32_t reserved;
};

// Layout, not sizeof(MemoryDescriptor), governs how to step through a
// GetMemoryMap result — the spec allows DescriptorSize to exceed this for
// forward compatibility. See pmm code that walks the map.
struct MemoryDescriptor {
    std::uint32_t type;
    std::uint64_t physical_start;
    std::uint64_t virtual_start;
    std::uint64_t number_of_pages;
    std::uint64_t attribute;
};

constexpr std::uint32_t kEfiLoaderCode = 1;
constexpr std::uint32_t kEfiLoaderData = 2;
constexpr std::uint32_t kEfiBootServicesCode = 3;
constexpr std::uint32_t kEfiBootServicesData = 4;
constexpr std::uint32_t kEfiRuntimeServicesCode = 5;
constexpr std::uint32_t kEfiRuntimeServicesData = 6;
constexpr std::uint32_t kEfiConventionalMemory = 7;
constexpr std::uint32_t kEfiACPIReclaimMemory = 9;
constexpr std::uint32_t kEfiACPIMemoryNVS = 10;

// Whether a descriptor type is backed by real installed RAM at all (as
// opposed to MMIO, reserved address-space windows, etc.) — an allow-list,
// not a deny-list. Verified necessary the hard way: QEMU's q35 chipset
// reports its PCI MMIO windows (both the low ~256 MiB hole and a huge
// 64-bit window near physical address 0xFD00000000) as
// EfiReservedMemoryType (0), not EfiMemoryMappedIO (11) — so a deny-list
// that only excluded the latter still let a bogus ~944 GiB "highest
// address" through. See DECISIONS.md, stage 3.
constexpr bool is_real_ram(std::uint32_t type) {
    switch (type) {
        case kEfiLoaderCode:
        case kEfiLoaderData:
        case kEfiBootServicesCode:
        case kEfiBootServicesData:
        case kEfiRuntimeServicesCode:
        case kEfiRuntimeServicesData:
        case kEfiConventionalMemory:
        case kEfiACPIReclaimMemory:
        case kEfiACPIMemoryNVS:
            return true;
        default:
            return false;
    }
}

using GetMemoryMapFn = EFIAPI Status (*)(std::uintptr_t* memory_map_size,
                                          MemoryDescriptor* memory_map,
                                          std::uintptr_t* map_key,
                                          std::uintptr_t* descriptor_size,
                                          std::uint32_t* descriptor_version);

using ExitBootServicesFn = EFIAPI Status (*)(Handle image_handle, std::uintptr_t map_key);

// Every field before GetMemoryMap/ExitBootServices is listed by its real
// spec name (as void*) rather than folded into a padding array, so this
// stays visually diffable against the UEFI spec's own table.
struct BootServices {
    TableHeader hdr;

    void* raise_tpl;
    void* restore_tpl;

    void* allocate_pages;
    void* free_pages;
    GetMemoryMapFn get_memory_map;
    void* allocate_pool;
    void* free_pool;

    void* create_event;
    void* set_timer;
    void* wait_for_event;
    void* signal_event;
    void* close_event;
    void* check_event;

    void* install_protocol_interface;
    void* reinstall_protocol_interface;
    void* uninstall_protocol_interface;
    void* handle_protocol;
    void* reserved;
    void* register_protocol_notify;
    void* locate_handle;
    void* locate_device_path;
    void* install_configuration_table;

    void* load_image;
    void* start_image;
    void* exit;
    void* unload_image;
    ExitBootServicesFn exit_boot_services;

    void* get_next_monotonic_count;
    void* stall;
    void* set_watchdog_timer;

    void* connect_controller;
    void* disconnect_controller;

    void* open_protocol;
    void* close_protocol;
    void* open_protocol_information;

    void* protocols_per_handle;
    void* locate_handle_buffer;
    void* locate_protocol;
    void* install_multiple_protocol_interfaces;
    void* uninstall_multiple_protocol_interfaces;

    void* calculate_crc32;

    void* copy_mem;
    void* set_mem;

    void* create_event_ex;
};

struct SystemTable {
    TableHeader hdr;
    char16_t* firmware_vendor;
    std::uint32_t firmware_revision;
    Handle console_in_handle;
    void* con_in;
    Handle console_out_handle;
    void* con_out;
    Handle standard_error_handle;
    void* std_err;
    void* runtime_services;
    BootServices* boot_services;
    std::uintptr_t number_of_table_entries;
    void* configuration_table;
};

} // namespace efi
