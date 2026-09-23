#pragma once

#include <cstddef>
#include <cstdint>

namespace fat32 {

// Reads and parses the boot sector (LBA 0 — this project's disk images
// are a whole-device FAT32 filesystem with no partition table, see
// DECISIONS.md) via virtio_blk. Must run after virtio_blk::init().
void init();

// Reads a file by name, given in ordinary "NAME.EXT" form (converted
// internally to FAT's fixed 11-byte 8.3 directory-entry format — long
// filenames aren't supported, matching CLAUDE.md's "FAT32 for read
// support" scope). Copies up to buf_size bytes into buf. Returns the
// number of bytes actually copied (the file's real size, clamped to
// buf_size), or -1 if no matching file was found in the root directory.
int read_file(const char* name, void* buf, std::size_t buf_size);

// Calls callback once per real (non-deleted, non-volume-label,
// non-long-filename) root directory entry, with its name converted back
// to ordinary "NAME.EXT" form (or just "NAME" with no dot if the
// extension is empty) and its size in bytes.
void list_root(void (*callback)(const char* name, uint32_t size));

} // namespace fat32
