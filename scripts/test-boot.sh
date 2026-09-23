#!/usr/bin/env bash
# Automated QEMU boot test: boots headlessly, captures serial to a log file,
# and greps it for expected markers. macOS ships no `timeout` binary, so we
# background QEMU and kill it after a fixed grace period instead.
#
# Usage: scripts/test-boot.sh [marker ...]
# Defaults to the stage 1 marker if none given.
set -uo pipefail

cd "$(dirname "$0")/.."

MARKERS=("$@")
if [ "${#MARKERS[@]}" -eq 0 ]; then
    MARKERS=("[OK] boot")
fi

BOOT_SECONDS=20 # stage 5's scheduler demos + stage 6's 100-iteration disk-I/O torture test

OVMF_DIR="$(brew --prefix qemu)/share/qemu"
OVMF_CODE="$OVMF_DIR/edk2-x86_64-code.fd"
VARS_TEMPLATE="$OVMF_DIR/edk2-i386-vars.fd" # see ASSUMPTIONS.md, 2026-09-23
VARS="build/OVMF_VARS.fd"
LOG="build/serial.log"

mkdir -p build
if [ ! -f "$VARS" ]; then
    cp "$VARS_TEMPLATE" "$VARS"
fi
if [ ! -f build/disk.img ]; then
    ./scripts/make-disk.sh
fi
rm -f "$LOG"

qemu-system-x86_64 \
    -machine q35 \
    -m 256M \
    -drive if=pflash,format=raw,unit=0,file="$OVMF_CODE",readonly=on \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive format=raw,file=fat:rw:build/esp \
    -drive file=build/disk.img,format=raw,if=none,id=disk0 \
    -device virtio-blk-pci,drive=disk0 \
    -serial file:"$LOG" \
    -display none \
    -no-reboot &
QEMU_PID=$!

sleep "$BOOT_SECONDS"
kill "$QEMU_PID" 2>/dev/null
wait "$QEMU_PID" 2>/dev/null

STATUS=0
for marker in "${MARKERS[@]}"; do
    if grep -qF "$marker" "$LOG"; then
        echo "[PASS] found marker: $marker"
    else
        echo "[FAIL] missing marker: $marker"
        STATUS=1
    fi
done

if grep -qF "[PANIC]" "$LOG"; then
    echo "[FAIL] kernel panic marker found in serial log"
    STATUS=1
fi

if [ "$STATUS" -ne 0 ]; then
    echo "--- serial log ($LOG) ---"
    cat "$LOG"
fi

exit "$STATUS"
