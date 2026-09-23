#!/usr/bin/env bash
# Builds build/disk.img: a 64 MiB raw disk image containing a single
# whole-device FAT32 filesystem (no partition table — a "superfloppy"
# layout, chosen so the kernel's FAT32 reader only ever has to parse a
# boot sector at LBA 0, not MBR/GPT partitioning too — see DECISIONS.md)
# with one known test file on it, for stage 6's storage/filesystem work.
#
# macOS-native tooling only (hdiutil, newfs_msdos) — nothing new to
# install. Safe to re-run: always starts from a fresh image.
set -euo pipefail

cd "$(dirname "$0")/.."

IMG="build/disk.img"
SIZE_MB=64
MOUNT_POINT="build/disk-mount"
TEST_FILE_NAME="HELLO.TXT"
TEST_FILE_CONTENT="Hello from the Singularity Kernel test disk!"

mkdir -p build
rm -f "$IMG"
truncate -s "${SIZE_MB}m" "$IMG"

DEVICE=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$IMG" | awk '{print $1}' | head -1)
echo "Attached as $DEVICE"

cleanup() {
    if mount | grep -q "$MOUNT_POINT"; then
        umount "$MOUNT_POINT" || true
    fi
    hdiutil detach "$DEVICE" >/dev/null 2>&1 || true
}
trap cleanup EXIT

newfs_msdos -F 32 -v SINGULARITY "$DEVICE"

mkdir -p "$MOUNT_POINT"
mount -t msdos "$DEVICE" "$MOUNT_POINT"
# macOS's msdosfs kext writes its own AppleDouble (._HELLO.TXT) metadata
# file alongside anything created on a FAT volume, regardless of
# COPYFILE_DISABLE (that only suppresses copyfile()-driven AppleDouble
# creation, not the kext's own). Left as-is deliberately: the FAT32
# reader needs to skip directory entries that don't match what it's
# looking for anyway, so this is realistic filesystem noise to read
# past correctly, not something worth fighting further.
printf '%s' "$TEST_FILE_CONTENT" > "$MOUNT_POINT/$TEST_FILE_NAME"
umount "$MOUNT_POINT"
rmdir "$MOUNT_POINT"

hdiutil detach "$DEVICE"
trap - EXIT

echo "Wrote $IMG ($SIZE_MB MiB, FAT32, containing $TEST_FILE_NAME)"
