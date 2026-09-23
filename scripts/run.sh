#!/usr/bin/env bash
# Interactive QEMU launch: serial console on stdio, no graphical window.
# Kernel output only ever goes to serial — there's no VGA driver planned.
set -euo pipefail

cd "$(dirname "$0")/.."

OVMF_DIR="$(brew --prefix qemu)/share/qemu"
OVMF_CODE="$OVMF_DIR/edk2-x86_64-code.fd"
VARS_TEMPLATE="$OVMF_DIR/edk2-i386-vars.fd" # see ASSUMPTIONS.md, 2026-09-23
VARS="build/OVMF_VARS.fd"

mkdir -p build
if [ ! -f "$VARS" ]; then
    cp "$VARS_TEMPLATE" "$VARS"
fi

exec qemu-system-x86_64 \
    -machine q35 \
    -m 256M \
    -drive if=pflash,format=raw,unit=0,file="$OVMF_CODE",readonly=on \
    -drive if=pflash,format=raw,unit=1,file="$VARS" \
    -drive format=raw,file=fat:rw:build/esp \
    -serial stdio \
    -display none \
    -no-reboot
