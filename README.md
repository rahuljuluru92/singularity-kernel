# Singularity Kernel

A from-scratch 64-bit kernel, written in C++23, that boots via UEFI in
QEMU. This is Phase 1 of the project: a deliberately trimmed, honest,
actually-finishable core — not the full original scope (see
[What this deliberately doesn't do yet](#what-this-deliberately-doesnt-do-yet-phase-2)
below).

Every claim in this README is backed by an actual QEMU boot run or host
unit test, not by the code "looking right." Each capability below is
tied to the stage that built it and the specific thing that was checked
to prove it works.

## What it does

Booting the kernel in QEMU takes it through, in order:

1. **Boots as a native UEFI application** — a `BOOTX64.EFI` image loaded
   directly by OVMF firmware, no bootloader chain beyond that.
2. **Hands off from the UEFI entry point to a real kernel proper**
   (`kernel_main`), which then sets up its own GDT and IDT and installs
   handlers for all 32 CPU exception vectors.
3. **Manages physical memory with a real bitmap page allocator**, builds
   its own 4-level page tables with a higher-half direct map, and
   enforces W^X (executable pages are never writable, writable pages are
   never executable).
4. **Runs a heap allocator** (address-ordered freelist, with physical-
   neighbor coalescing) on top of that physical memory, for dynamic
   kernel-side allocation.
5. **Preemptively schedules multiple kernel tasks** on a 100 Hz timer
   tick, round-robin, with real context switching.
6. **Drives a VirtIO block device** over PCI and **reads files off a
   FAT32 filesystem** on it.
7. **Exposes an interactive shell over the serial console** — `help`,
   `ls`, `cat <file>`, `meminfo`, `tasks` — reachable by connecting to
   QEMU's serial console and typing at it.

Every one of stages 1–7 also has a torture test that stresses the
subsystem it introduces (allocate/free cycling, mixed-size heap churn,
many short-lived tasks, repeated disk reads), run as part of the same
boot sequence.

## What was actually verified (stage by stage)

| Stage | Built | Success criterion | Verified by |
|---|---|---|---|
| 1 — UEFI boot | Toolchain, custom PE linker script, minimal boot stub | Boots in QEMU/OVMF, prints a marker | `[OK] boot` in the serial log, reproducibly |
| 2 — Kernel handoff | GDT, IDT, 32 CPU exception handlers | Handoff + a deliberate exception is caught, not a silent triple fault | `[OK] handoff`, `[OK] gdt`, `[OK] idt`, `[OK] exception caught` (divide-by-zero, actually executed) |
| 3 — Memory manager | Bitmap physical allocator, paging, higher-half direct map, W^X | Host tests pass; allocator torture test passes; a non-executable page genuinely faults on execute | `make test` (8 cases / 586 assertions) + `[OK] pmm torture` + `[OK] w^x enforced` (confirmed via the page fault's actual instruction-fetch error-code bit, not just "a fault happened") |
| 4 — Heap allocator | Freelist allocator with coalescing, backed by stage 3's pages | Host tests pass; mixed-size alloc/free torture test passes without corruption | `make test` (+7 cases, 1350 total assertions) + `[OK] heap torture` (2000 iterations, each buffer's contents verified intact before freeing) |
| 5 — Scheduler | Preemptive round-robin, PIT-driven, real context switches | Multiple tasks actually interleave (not run-to-completion); scheduler torture test passes | `[OK] scheduler interleaving` (serial log shows `A1,A2,B1,B2,C1,...` — real interleaving, not `A1..A5` then `B`) + `[OK] scheduler torture` (50 short-lived tasks, exact counter match) |
| 6 — Storage + filesystem | VirtIO block driver, FAT32 reader | Reads a known file's exact contents off a virtual disk; repeated-read torture test passes | `[OK] virtio-blk`, `[OK] fat32`, `[OK] disk read: Hello from the Singularity Kernel test disk!` (byte-exact), `[OK] disk torture` (100 repeated reads) |
| 7 — Shell | Interactive serial shell: `help`, `ls`, `cat`, `meminfo`, `tasks` | A real interactive session (not a scripted test) demonstrates listing and reading a file | A live, turn-by-turn session driving the shell over serial — `help`, `ls` (real directory contents), `cat HELLO.TXT` (exact contents), `meminfo`, `tasks`, a missing-file `cat`, an unknown command, and a mid-command backspace, each confirmed byte-exact against the actual output. See `DECISIONS.md` for the full session. |

Host-side unit tests currently stand at **1365 assertions across 18
Catch2 test cases** (`make test`). The full QEMU boot sequence currently
prints 19 `[OK]` markers with zero `[PANIC]`s, reproducibly.

## What this deliberately doesn't do yet (Phase 2)

The original idea for this project was substantially larger: C++23 and
Rust with a cross-language FFI boundary, SMP/NUMA-aware scheduling, four
filesystems across three storage backends, a hardened allocator with
guard pages, and a hardware-abstraction layer for future ports. Phase 1
scoped that down deliberately, to something that could actually finish.
Specifically, as of Phase 1:

- **Single core only.** No SMP — no ACPI/MADT parsing, no AP bring-up, no
  IOAPIC/x2APIC. The timer is the legacy 8259 PIC + PIT, not the LAPIC
  timer, because LAPIC only pays for itself once there's more than one
  core to route interrupts to.
- **C++23 only.** No Rust, no FFI boundary.
- **No hardening beyond basic W^X.** No ASLR, no stack canaries, no
  guard-page allocator. W^X itself is a 2-tier policy (kernel `.text` is
  R+X; everything else is R+W+NX) — `.rodata` is treated as writable
  data, not given its own read-only tier.
- **The higher-half mapping is dual-mapped, not a true single higher-half
  kernel.** The original low identity-mapped alias is kept permanently
  (never torn down), because the kernel binary is linked at a low
  absolute address and the compiler embedded low-address data references
  throughout. RIP genuinely executes from higher-half addresses after
  the switch, and all physical memory is genuinely reachable through the
  higher-half direct map — but the low alias staying resident means this
  isn't the same guarantee a fully self-relocated higher-half kernel
  makes.
- **Physical memory tracking is capped at 4 GiB** (a static bitmap sized
  for that bound). `EfiBootServicesCode`/`Data` memory (~tens of MiB) is
  never reclaimed after `ExitBootServices`.
- **The kernel heap is a fixed one-time 4 MiB region.** It does not grow
  on demand — if it fills up, allocation just fails.
- **One storage driver, one filesystem.** VirtIO block only (no AHCI, no
  NVMe), and only its legacy I/O-port interface (not modern/1.0 virtio).
  FAT32 only, whole-disk with no MBR/GPT partition table, short (8.3)
  filenames only (no VFAT long-filename support), and all disk I/O is
  polling-based, not interrupt-driven.
- **PCI enumeration is a brute-force full-bus scan** via the legacy
  CONFIG_ADDRESS/CONFIG_DATA ports — no MMCONFIG/ECAM, no bridge-aware
  traversal (there's only ever been one device to find).
- **The shell is intentionally minimal.** A two-token parser (one command
  + one optional argument, no quoting, no pipes), five commands, no
  history or tab completion. `ls` doesn't distinguish files from
  directories.
- **No graphics, audio, or USB.** No GOP framebuffer, no HDA, no xHCI.
- **No hardware-abstraction layer.** Architecture-specific code
  (x86_64 assembly, register access) is not behind any portability
  interface.

All of the above is Phase 2's stated scope, to be built in the same
stage-by-stage, QEMU-proven way as Phase 1 — see `DECISIONS.md` for the
full reasoning behind each simplification above (each one is a
deliberate, documented tradeoff, not an oversight).

## Setup

Everything below installs via Homebrew on macOS (developed on Apple
Silicon; not tested on Intel Macs or other platforms):

```bash
brew install x86_64-elf-gcc nasm qemu catch2
```

- `x86_64-elf-gcc` 16.2.0 (with `x86_64-elf-binutils` 2.47) — cross
  compiler and linker. Ships with no libstdc++ at all, not even
  freestanding-mandatory headers — this project provides its own minimal
  `<cstdint>`/`<cstddef>`/`<new>` shims in `include/`.
- `nasm` 3.02 — for the small number of assembly stubs (GDT reload, ISR
  entry trampolines).
- `qemu` 11.1.1 — emulator; its bundled `edk2-x86_64-code.fd` is used as
  OVMF UEFI firmware (no separate OVMF package needed).
- `catch2` — host-side unit test framework.

No other dependencies. See `DECISIONS.md` for why (e.g. no gnu-efi, no
LLVM/lld-link — the UEFI PE image is linked directly by GNU `ld` using a
hand-written linker script).

## Building and running

```bash
make            # cross-compiles the kernel, produces build/esp/EFI/BOOT/BOOTX64.EFI
make run        # boots it in QEMU with an interactive serial console (Ctrl-C to quit)
make test       # host-side Catch2 unit tests (no QEMU involved)
make test-boot  # headless QEMU boot test — boots, captures serial to a log, greps for markers
```

`scripts/run.sh` and `scripts/test-boot.sh` both auto-create their
build-only dependencies on first run: a writable `OVMF_VARS.fd` (copied
from Homebrew's template) and a 64 MiB FAT32 test disk image
(`scripts/make-disk.sh`, built with macOS-native `hdiutil`/`newfs_msdos`,
containing one test file). Neither is committed to the repo — both are
build artifacts under `build/`, which is gitignored.

`scripts/test-boot.sh` takes an optional list of markers to check for
(defaults to just `[OK] boot`); the full stage 1–7 marker set is:

```bash
./scripts/test-boot.sh \
  "[OK] boot" "[OK] handoff" "[OK] memmap" "[OK] exitbs" "[OK] gdt" "[OK] idt" \
  "[OK] pmm torture" "[OK] paging" "[OK] higher-half" "[OK] exception caught" \
  "[OK] w^x enforced" "[OK] heap" "[OK] heap torture" \
  "[OK] scheduler interleaving" "[OK] scheduler torture" \
  "[OK] virtio-blk" "[OK] fat32" "[OK] disk read:" "[OK] disk torture"
```

To try the shell interactively, `make run` and, once the boot sequence
finishes and `Singularity Kernel shell. Type 'help' for commands.`
appears, type at it directly — QEMU's `-serial stdio` connects the
kernel's serial console straight to your terminal.

## Project layout

```
boot/boot.cpp              UEFI entry point (efi_main)
kernel/main.cpp            kernel_main — the boot sequence + self-tests
kernel/gdt.*, idt.*, isr.* GDT, IDT, exception/interrupt handling
kernel/pmm.*, paging.*     physical memory manager, page tables, W^X
kernel/heap.*              kernel heap (wraps heap/freelist_allocator)
kernel/pic.*, pit.*        legacy interrupt controller + timer
kernel/sched.*             preemptive round-robin scheduler
kernel/pci.*, virtio_blk.* PCI enumeration, VirtIO block driver
kernel/fat32.*             FAT32 filesystem reader
kernel/shell.*             interactive serial shell
kernel/mem.cpp, efi.hpp    freestanding mem*() functions, UEFI struct defs
pmm/bitmap_allocator.*     host-testable physical page allocator core
heap/freelist_allocator.*  host-testable heap allocator core
include/                   project-owned <cstdint>/<cstddef>/<new>/serial.hpp
linker/uefi.ld             custom PE linker script
tests/                     Catch2 host-side unit tests
scripts/                   run.sh, test-boot.sh, make-disk.sh
```

`pmm/bitmap_allocator.*` and `heap/freelist_allocator.*` are split out
from `kernel/` deliberately: their core allocation logic has zero OS or
hardware dependency, which is what makes them host-unit-testable with
Catch2 before ever being linked into the freestanding kernel.

## Notes

- `ASSUMPTIONS.md` and `DECISIONS.md` (untracked, local-only working
  notes) hold the full session-by-session history: every non-trivial
  decision with its alternatives and reasoning, every assumption with
  its risk if wrong, and a session log tying each stage to when it was
  built.
- This README will be revised again at the end of Phase 2, with the same
  standard: every claim tied to an actual verification, hedged claims
  (like the dual-mapped higher-half note above) stated plainly rather
  than rounded up.
