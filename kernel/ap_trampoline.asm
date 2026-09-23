; AP (application processor) startup trampoline.
;
; Assembled SEPARATELY as a flat binary (`nasm -f bin`, see the Makefile),
; not as part of the main kernel ELF/PE link — an AP starts executing in
; real mode at a fixed low physical address (per the SIPI vector, always
; below 1 MiB, page-aligned: CS = vector<<8, IP = 0, so physical start =
; vector*0x1000), which has nothing to do with where this kernel's own
; image is linked. kernel/smp.cpp memcpy's the resulting flat binary to
; that physical address at runtime and patches the three data slots
; below before triggering each AP's INIT-SIPI-SIPI. See kernel/smp.cpp
; and DECISIONS.md, stage 9.
;
; `org` matches kTrampolineAddr (kernel/smp.hpp) exactly — every label
; here resolves to an ABSOLUTE address equal to where this code will
; actually be copied, which is what makes near jumps/data references
; correct without any manual segment math: DS is zeroed below (base 0,
; matching the AP's guaranteed post-INIT reset state), and a zero-base
; segment means a NASM-computed absolute label value IS the correct
; DS-relative offset.
bits 16
org 0x8000

ap_trampoline_start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax

    lgdt [gdt_ptr]

    mov eax, cr0
    or eax, 1            ; CR0.PE — protected mode, paging still off
    mov cr0, eax

    jmp 0x08:pm_entry     ; far jump: flush the real-mode prefetch queue,
                          ; load CS with the flat 32-bit code selector

bits 32
pm_entry:
    mov ax, 0x10          ; flat 32-bit data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov eax, cr4
    or eax, (1 << 5)       ; CR4.PAE — required before long mode
    mov cr4, eax

    mov eax, [boot_cr3]    ; patched by smp.cpp: this kernel's real PML4,
    mov cr3, eax           ; identical page tables the BSP itself uses

    mov ecx, 0xC0000080     ; IA32_EFER
    rdmsr
    or eax, (1 << 8) | (1 << 11)  ; LME (long mode enable) | NXE (see
    wrmsr                          ; DECISIONS.md — EFER is per-core, W^X
                                    ; silently wouldn't apply on this core
                                    ; otherwise even though the shared page
                                    ; tables already have NX bits set)

    mov eax, cr0
    or eax, 0x80000000     ; CR0.PG — paging on. From the NEXT instruction
    mov cr0, eax           ; fetch onward, this page's own executability
                            ; under the just-loaded page tables is what
                            ; keeps this code running at all — see
                            ; paging::add_executable_page in smp.cpp.

    jmp 0x18:lm_entry       ; far jump into the flat 64-bit code selector

bits 64
lm_entry:
    mov ax, 0x20            ; flat 64-bit data selector
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax

    mov rsp, [abs boot_stack_top] ; patched per-AP by smp.cpp before its SIPI
    mov rax, [abs boot_entry]     ; the linked address of ap_entry() (smp.cpp)
    jmp rax                        ; never returns

; A minimal temporary GDT: null, flat 32-bit code/data (for the real-mode
; -> protected-mode step, before paging exists), flat 64-bit code/data
; (base/limit are architecturally ignored in long mode, so these two
; entries are bit-for-bit identical to kernel/gdt.cpp's own kGdt — no
; coincidence, just the same standard flat descriptor values).
align 8
gdt_start:
    dq 0x0000000000000000 ; null
    dq 0x00CF9A000000FFFF ; 0x08: code32, flat, 4 GiB, G=1, D=1
    dq 0x00CF92000000FFFF ; 0x10: data32, flat, 4 GiB, G=1, D=1
    dq 0x00A09A0000000000 ; 0x18: code64, L=1 (matches kernel/gdt.cpp)
    dq 0x00A0920000000000 ; 0x20: data64            (matches kernel/gdt.cpp)
gdt_end:

gdt_ptr:
    dw gdt_end - gdt_start - 1
    dq gdt_start

; Patch area: fixed, round-number offset from ap_trampoline_start so
; kernel/smp.cpp can compute these addresses (kTrampolineAddr +
; kPatchAreaOffset + 0/8/16) without needing to know this file's exact
; code size — see kernel/smp.hpp's kPatchAreaOffset.
times (0x800 - ($ - $$)) db 0
boot_cr3:       dq 0
boot_stack_top: dq 0
boot_entry:     dq 0

times (0x820 - ($ - $$)) db 0
ap_trampoline_end:
