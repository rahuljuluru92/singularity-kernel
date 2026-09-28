bits 64

; CPU exceptions 8, 10, 11, 12, 13, 14, 17 push an error code onto the
; stack automatically; the rest don't. ISR_NOERR pushes a dummy 0 so every
; vector reaches isr_common_stub with the same stack shape (vector on top,
; error code below it) before we push the 15 general-purpose registers.
%macro ISR_NOERR 1
section .text
global isr_stub_%1
isr_stub_%1:
    push qword 0
    push qword %1
    jmp isr_common_stub
%endmacro

%macro ISR_ERR 1
section .text
global isr_stub_%1
isr_stub_%1:
    push qword %1
    jmp isr_common_stub
%endmacro

extern isr_handler

section .text
isr_common_stub:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    call isr_handler

    ; isr_handler returns (in rax) the trap-frame pointer to resume from —
    ; almost always the same one it was called with, but for the timer
    ; vector this is how sched::on_timer_tick actually switches tasks:
    ; a different pointer here means the pops below (and the iretq) act
    ; on a DIFFERENT task's saved context instead of this one's.
    mov rsp, rax
    jmp resume_trap_frame_tail

; extern "C" [[noreturn]] void resume_trap_frame(uint64_t rsp) — an
; entry point into the SAME pop+iretq tail above, callable from normal
; (non-interrupt) C++ code, not just from isr_common_stub. rsp arrives
; in rdi per the SysV ABI's first-argument register. sched::task_exit
; uses this to hand off directly to whatever task/idle-context runs
; next, without ever needing an intermediate "parked" stack at all —
; stage 10 originally used a small per-core scratch stack for this
; instead, which turned out to have a real, hard-to-fully-pin-down
; timing window; removing the intermediate stack removes the window
; category entirely rather than continuing to chase it. See DECISIONS.md.
global resume_trap_frame
resume_trap_frame:
    mov rsp, rdi
resume_trap_frame_tail:
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Vector 32 = IRQ0 (PIT timer), the only hardware interrupt this project
; handles — remapped here from the PIC's power-on default (0-15, which
; collides with CPU exceptions) by pic::init(). Same "no error code"
; shape as any other IRQ.
ISR_NOERR 32

section .rodata
global isr_stub_table
isr_stub_table:
    dq isr_stub_0,  isr_stub_1,  isr_stub_2,  isr_stub_3
    dq isr_stub_4,  isr_stub_5,  isr_stub_6,  isr_stub_7
    dq isr_stub_8,  isr_stub_9,  isr_stub_10, isr_stub_11
    dq isr_stub_12, isr_stub_13, isr_stub_14, isr_stub_15
    dq isr_stub_16, isr_stub_17, isr_stub_18, isr_stub_19
    dq isr_stub_20, isr_stub_21, isr_stub_22, isr_stub_23
    dq isr_stub_24, isr_stub_25, isr_stub_26, isr_stub_27
    dq isr_stub_28, isr_stub_29, isr_stub_30, isr_stub_31
    dq isr_stub_32
