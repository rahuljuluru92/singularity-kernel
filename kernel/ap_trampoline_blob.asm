; Embeds the pre-assembled flat-binary AP trampoline (build/ap_trampoline.bin
; — see kernel/ap_trampoline.asm and the Makefile) into the normal ELF64
; kernel link, as a plain byte array smp.cpp can memcpy from. No objcopy
; needed: incbin, assembled with the same NASM this project already uses
; for everything else.
bits 64

section .rodata

global ap_trampoline_blob_start
global ap_trampoline_blob_end

ap_trampoline_blob_start:
    incbin "build/ap_trampoline.bin"
ap_trampoline_blob_end:
