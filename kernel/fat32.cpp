#include "fat32.hpp"

#include "heap.hpp"
#include "serial.hpp"
#include "virtio_blk.hpp"

namespace fat32 {

namespace {

struct [[gnu::packed]] Bpb {
    uint8_t jmp_boot[3];
    uint8_t oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t num_fats;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t fat_size_16;
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;
    uint32_t fat_size_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t reserved[12];
    uint8_t drive_number;
    uint8_t reserved1;
    uint8_t boot_signature;
    uint32_t volume_id;
    uint8_t volume_label[11];
    uint8_t fs_type[8];
};

struct [[gnu::packed]] DirEntry {
    uint8_t name[11];
    uint8_t attr;
    uint8_t reserved;
    uint8_t creation_time_tenth;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
};

constexpr uint8_t kAttrVolumeId = 0x08;
constexpr uint8_t kAttrLongName = 0x0F;
constexpr uint8_t kNameFreeRest = 0xE5;
constexpr uint32_t kEndOfChainMin = 0x0FFFFFF8;
constexpr uint32_t kClusterMask = 0x0FFFFFFF;

Bpb g_bpb;
uint32_t g_first_data_sector;
uint32_t g_bytes_per_cluster;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] fat32: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

void format_8_3(const char* name, uint8_t out[11]) {
    for (int i = 0; i < 11; i++) {
        out[i] = ' ';
    }
    int i = 0;
    int out_i = 0;
    while (name[i] != '\0' && name[i] != '.' && out_i < 8) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out[out_i++] = static_cast<uint8_t>(c);
        i++;
    }
    while (name[i] != '\0' && name[i] != '.') {
        i++;
    }
    if (name[i] == '.') {
        i++;
    }
    int ext_i = 8;
    while (name[i] != '\0' && ext_i < 11) {
        char c = name[i];
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        out[ext_i++] = static_cast<uint8_t>(c);
        i++;
    }
}

// Inverse of format_8_3: "HELLO   TXT" -> "HELLO.TXT" (or just "HELLO"
// if the extension is all spaces). out must have room for 13 bytes
// (8 + '.' + 3 + '\0').
void unformat_8_3(const uint8_t name[11], char* out) {
    int out_i = 0;
    int name_end = 8;
    while (name_end > 0 && name[name_end - 1] == ' ') {
        name_end--;
    }
    for (int i = 0; i < name_end; i++) {
        out[out_i++] = static_cast<char>(name[i]);
    }

    int ext_end = 11;
    while (ext_end > 8 && name[ext_end - 1] == ' ') {
        ext_end--;
    }
    if (ext_end > 8) {
        out[out_i++] = '.';
        for (int i = 8; i < ext_end; i++) {
            out[out_i++] = static_cast<char>(name[i]);
        }
    }
    out[out_i] = '\0';
}

uint32_t cluster_to_lba(uint32_t cluster) {
    return g_first_data_sector + (cluster - 2) * g_bpb.sectors_per_cluster;
}

// Returns the next cluster in the chain, or a value >= kEndOfChainMin at
// the chain's end. One sector's worth of FAT is read per call — no
// caching, simple over fast, matching this stage's actual scope.
uint32_t next_cluster(uint32_t cluster) {
    uint32_t fat_byte_offset = cluster * 4;
    uint32_t fat_sector = g_bpb.reserved_sector_count + (fat_byte_offset / g_bpb.bytes_per_sector);
    uint32_t offset_in_sector = fat_byte_offset % g_bpb.bytes_per_sector;

    auto* sector = static_cast<uint8_t*>(heap::kmalloc(g_bpb.bytes_per_sector));
    if (!virtio_blk::read_sectors(fat_sector, 1, sector)) {
        panic("failed to read FAT sector");
    }
    uint32_t value = static_cast<uint32_t>(sector[offset_in_sector]) |
                      (static_cast<uint32_t>(sector[offset_in_sector + 1]) << 8) |
                      (static_cast<uint32_t>(sector[offset_in_sector + 2]) << 16) |
                      (static_cast<uint32_t>(sector[offset_in_sector + 3]) << 24);
    heap::kfree(sector);
    return value & kClusterMask;
}

// Scans the root directory's cluster chain for an entry matching
// name_8_3. Returns true and fills *out if found.
bool find_in_root(const uint8_t name_8_3[11], DirEntry* out) {
    uint8_t* cluster_buf = static_cast<uint8_t*>(heap::kmalloc(g_bytes_per_cluster));
    uint32_t cluster = g_bpb.root_cluster;

    while (cluster < kEndOfChainMin) {
        if (!virtio_blk::read_sectors(cluster_to_lba(cluster), g_bpb.sectors_per_cluster, cluster_buf)) {
            panic("failed to read root directory cluster");
        }

        auto* entries = reinterpret_cast<DirEntry*>(cluster_buf);
        uint32_t entry_count = g_bytes_per_cluster / sizeof(DirEntry);
        for (uint32_t i = 0; i < entry_count; i++) {
            DirEntry& e = entries[i];
            if (e.name[0] == 0x00) {
                heap::kfree(cluster_buf);
                return false; // end of directory
            }
            if (e.name[0] == kNameFreeRest || e.attr == kAttrLongName || (e.attr & kAttrVolumeId)) {
                continue;
            }
            bool match = true;
            for (int j = 0; j < 11; j++) {
                if (e.name[j] != name_8_3[j]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                *out = e;
                heap::kfree(cluster_buf);
                return true;
            }
        }

        cluster = next_cluster(cluster);
    }

    heap::kfree(cluster_buf);
    return false;
}

} // namespace

void init() {
    auto* sector0 = static_cast<uint8_t*>(heap::kmalloc(512));
    if (!virtio_blk::read_sectors(0, 1, sector0)) {
        panic("failed to read boot sector");
    }
    if (sector0[510] != 0x55 || sector0[511] != 0xAA) {
        panic("boot sector missing 0x55AA signature");
    }

    g_bpb = *reinterpret_cast<Bpb*>(sector0);
    heap::kfree(sector0);

    g_first_data_sector =
        g_bpb.reserved_sector_count + (g_bpb.num_fats * g_bpb.fat_size_32);
    g_bytes_per_cluster =
        static_cast<uint32_t>(g_bpb.bytes_per_sector) * g_bpb.sectors_per_cluster;

    serial::puts("[OK] fat32 bytes_per_sector=");
    serial::put_hex(g_bpb.bytes_per_sector);
    serial::puts(" sectors_per_cluster=");
    serial::put_hex(g_bpb.sectors_per_cluster);
    serial::puts(" root_cluster=");
    serial::put_hex(g_bpb.root_cluster);
    serial::puts("\r\n");
}

int read_file(const char* name, void* buf, std::size_t buf_size) {
    uint8_t name_8_3[11];
    format_8_3(name, name_8_3);

    DirEntry entry;
    if (!find_in_root(name_8_3, &entry)) {
        return -1;
    }

    uint32_t cluster = (static_cast<uint32_t>(entry.first_cluster_high) << 16) | entry.first_cluster_low;
    uint32_t remaining = entry.file_size;
    std::size_t copied = 0;

    uint8_t* cluster_buf = static_cast<uint8_t*>(heap::kmalloc(g_bytes_per_cluster));
    while (cluster < kEndOfChainMin && remaining > 0 && copied < buf_size) {
        if (!virtio_blk::read_sectors(cluster_to_lba(cluster), g_bpb.sectors_per_cluster, cluster_buf)) {
            panic("failed to read file cluster");
        }
        uint32_t chunk = g_bytes_per_cluster;
        if (chunk > remaining) {
            chunk = remaining;
        }
        for (uint32_t i = 0; i < chunk && copied < buf_size; i++) {
            static_cast<uint8_t*>(buf)[copied++] = cluster_buf[i];
        }
        remaining -= chunk;
        cluster = next_cluster(cluster);
    }
    heap::kfree(cluster_buf);

    return static_cast<int>(copied);
}

void list_root(void (*callback)(const char* name, uint32_t size)) {
    uint8_t* cluster_buf = static_cast<uint8_t*>(heap::kmalloc(g_bytes_per_cluster));
    uint32_t cluster = g_bpb.root_cluster;

    while (cluster < kEndOfChainMin) {
        if (!virtio_blk::read_sectors(cluster_to_lba(cluster), g_bpb.sectors_per_cluster, cluster_buf)) {
            panic("failed to read root directory cluster");
        }

        auto* entries = reinterpret_cast<DirEntry*>(cluster_buf);
        uint32_t entry_count = g_bytes_per_cluster / sizeof(DirEntry);
        for (uint32_t i = 0; i < entry_count; i++) {
            DirEntry& e = entries[i];
            if (e.name[0] == 0x00) {
                heap::kfree(cluster_buf);
                return; // end of directory
            }
            if (e.name[0] == kNameFreeRest || e.attr == kAttrLongName || (e.attr & kAttrVolumeId)) {
                continue;
            }
            char name[13];
            unformat_8_3(e.name, name);
            callback(name, e.file_size);
        }

        cluster = next_cluster(cluster);
    }
    heap::kfree(cluster_buf);
}

} // namespace fat32
