#pragma once

#include <cstdint>

// Layout matches exactly what isr_stubs.asm pushes, in order (lowest
// address first): 15 general-purpose registers, then vector/error_code,
// then the 5 fields the CPU itself always pushes in long mode (RIP, CS,
// RFLAGS, RSP, SS — pushed unconditionally in 64-bit mode, unlike legacy
// 32-bit protected mode where SS:ESP is only pushed on a privilege change).
struct [[gnu::packed]] registers {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t vector, error_code;
    uint64_t rip, cs, rflags, rsp, ss;
};

extern "C" void isr_handler(registers* regs);
