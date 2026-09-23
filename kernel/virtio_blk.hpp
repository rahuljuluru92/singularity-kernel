#pragma once

#include <cstddef>
#include <cstdint>

namespace virtio_blk {

// Finds the virtio-blk PCI device (legacy I/O-port interface — see
// DECISIONS.md), negotiates zero optional features, and sets up its
// single request virtqueue. Panics if the device isn't found.
void init();

// Reads `count` 512-byte sectors starting at `sector` into buf (must be
// at least count*512 bytes). Polls for completion — there's no IRQ
// handling for this device yet, see DECISIONS.md. Returns true on
// success (the device reported VIRTIO_BLK_S_OK).
bool read_sectors(uint64_t sector, uint32_t count, void* buf);

} // namespace virtio_blk
