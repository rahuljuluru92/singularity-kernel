#include "virtio_blk.hpp"

#include "paging.hpp"
#include "pci.hpp"
#include "pmm.hpp"
#include "serial.hpp"

namespace virtio_blk {

namespace {

constexpr uint16_t kVendorId = 0x1AF4;
constexpr uint16_t kDeviceId = 0x1001; // legacy/transitional virtio-blk

// Legacy virtio-pci I/O-port register offsets from BAR0.
constexpr uint16_t kRegDeviceFeatures = 0x00;
constexpr uint16_t kRegGuestFeatures = 0x04;
constexpr uint16_t kRegQueueAddress = 0x08;
constexpr uint16_t kRegQueueSize = 0x0C;
constexpr uint16_t kRegQueueSelect = 0x0E;
constexpr uint16_t kRegQueueNotify = 0x10;
constexpr uint16_t kRegStatus = 0x12;

constexpr uint8_t kStatusAcknowledge = 1;
constexpr uint8_t kStatusDriver = 2;
constexpr uint8_t kStatusDriverOk = 4;

constexpr uint16_t kDescFNext = 1;
constexpr uint16_t kDescFWrite = 2;

constexpr uint32_t kReqTypeIn = 0; // read
constexpr uint8_t kBlkStatusOk = 0;

struct [[gnu::packed]] VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct [[gnu::packed]] BlkReqHeader {
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

uint16_t g_io_base = 0;
uint16_t g_queue_size = 0;

VirtqDesc* g_desc = nullptr;
volatile uint16_t* g_avail_flags = nullptr;
volatile uint16_t* g_avail_idx = nullptr;
volatile uint16_t* g_avail_ring = nullptr;
volatile uint16_t* g_used_flags = nullptr;
volatile uint16_t* g_used_idx = nullptr;

struct [[gnu::packed]] UsedElem {
    uint32_t id;
    uint32_t len;
};
volatile UsedElem* g_used_ring = nullptr;

uint16_t g_last_seen_used_idx = 0;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] virtio_blk: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

uint16_t reg(uint16_t offset) { return static_cast<uint16_t>(g_io_base + offset); }

std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

void init() {
    pci::Device dev;
    if (!pci::find_device(kVendorId, kDeviceId, &dev)) {
        panic("device not found on the PCI bus");
    }

    // Command register: enable I/O space decoding (bit 0) and bus
    // mastering (bit 2) — the latter is what lets the device actually
    // perform DMA into/out of the buffers this driver gives it.
    uint16_t command = pci::config_read16(dev.bus, dev.slot, dev.function, 0x04);
    command |= 0x1 | 0x4;
    pci::config_write16(dev.bus, dev.slot, dev.function, 0x04, command);

    if ((dev.bar[0] & 0x1) == 0) {
        panic("BAR0 is not an I/O-space BAR");
    }
    g_io_base = static_cast<uint16_t>(dev.bar[0] & ~0x3u);

    serial::outb(reg(kRegStatus), 0); // reset
    serial::outb(reg(kRegStatus), kStatusAcknowledge);
    serial::outb(reg(kRegStatus), kStatusAcknowledge | kStatusDriver);

    // No optional features negotiated — plain sector read/write needs
    // none of them. See DECISIONS.md.
    serial::outl(reg(kRegGuestFeatures), 0);

    serial::outw(reg(kRegQueueSelect), 0);
    g_queue_size = serial::inw(reg(kRegQueueSize));
    if (g_queue_size == 0) {
        panic("queue 0 reported size 0");
    }

    // Layout is fixed by the virtio spec, not something the driver
    // chooses: descriptor table, then avail ring, immediately followed
    // (no gap) by the used ring at the next 4 KiB boundary — the device
    // computes these same offsets internally from queue_size and the one
    // physical page number handed to it below.
    std::size_t desc_bytes = static_cast<std::size_t>(g_queue_size) * sizeof(VirtqDesc);
    std::size_t avail_bytes = 4 + static_cast<std::size_t>(g_queue_size) * 2;
    std::size_t used_offset = align_up(desc_bytes + avail_bytes, 4096);
    std::size_t used_bytes = 4 + static_cast<std::size_t>(g_queue_size) * sizeof(UsedElem);
    std::size_t total_bytes = used_offset + used_bytes;
    std::size_t pages = (total_bytes + 4095) / 4096;

    uintptr_t phys = pmm::alloc_contiguous_pages(pages);
    if (phys == 0) {
        panic("out of contiguous physical memory for the virtqueue");
    }
    auto* base = reinterpret_cast<uint8_t*>(paging::to_higher_half(phys));
    for (std::size_t i = 0; i < pages * 4096; i++) {
        base[i] = 0;
    }

    g_desc = reinterpret_cast<VirtqDesc*>(base);
    g_avail_flags = reinterpret_cast<volatile uint16_t*>(base + desc_bytes);
    g_avail_idx = g_avail_flags + 1;
    g_avail_ring = g_avail_flags + 2;
    g_used_flags = reinterpret_cast<volatile uint16_t*>(base + used_offset);
    g_used_idx = g_used_flags + 1;
    g_used_ring = reinterpret_cast<volatile UsedElem*>(const_cast<uint16_t*>(g_used_flags) + 2);

    serial::outl(reg(kRegQueueAddress), static_cast<uint32_t>(phys / 4096));

    serial::outb(reg(kRegStatus), kStatusAcknowledge | kStatusDriver | kStatusDriverOk);

    serial::puts("[OK] virtio-blk queue_size=");
    serial::put_hex(g_queue_size);
    serial::puts("\r\n");
}

bool read_sectors(uint64_t sector, uint32_t count, void* buf) {
    BlkReqHeader header{.type = kReqTypeIn, .reserved = 0, .sector = sector};
    volatile uint8_t status = 0xFF; // sentinel; the device overwrites this

    uint64_t header_phys = paging::to_physical(reinterpret_cast<uint64_t>(&header));
    uint64_t buf_phys = paging::to_physical(reinterpret_cast<uint64_t>(buf));
    uint64_t status_phys = paging::to_physical(reinterpret_cast<uint64_t>(const_cast<uint8_t*>(&status)));

    constexpr uint16_t kHeaderDesc = 0, kDataDesc = 1, kStatusDesc = 2;
    g_desc[kHeaderDesc] = VirtqDesc{header_phys, sizeof(BlkReqHeader), kDescFNext, kDataDesc};
    g_desc[kDataDesc] =
        VirtqDesc{buf_phys, count * 512u, kDescFNext | kDescFWrite, kStatusDesc};
    g_desc[kStatusDesc] = VirtqDesc{status_phys, 1, kDescFWrite, 0};

    uint16_t slot = *g_avail_idx % g_queue_size;
    g_avail_ring[slot] = kHeaderDesc;
    asm volatile("" ::: "memory"); // publish the ring entry before the index
    *g_avail_idx = static_cast<uint16_t>(*g_avail_idx + 1);
    asm volatile("" ::: "memory"); // publish the index before notifying

    serial::outw(reg(kRegQueueNotify), 0);

    while (*g_used_idx == g_last_seen_used_idx) {
        // Polling, not IRQ-driven — see DECISIONS.md.
    }
    g_last_seen_used_idx = *g_used_idx;

    return status == kBlkStatusOk;
}

} // namespace virtio_blk
