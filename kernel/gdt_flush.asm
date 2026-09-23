bits 64

section .text

; void gdt_flush(gdtr* ptr) — ptr in rdi per SysV ABI.
; CS can't be reloaded with a plain mov, so we push the new selector and a
; target address, then far-return into it — the standard long-mode trick.
global gdt_flush
gdt_flush:
    lgdt [rdi]

    mov ax, 0x10        ; kernel data selector
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax

    push qword 0x08      ; kernel code selector
    lea rax, [rel .reload_cs]
    push rax
    retfq
.reload_cs:
    ret
