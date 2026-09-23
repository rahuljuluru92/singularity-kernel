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

namespace isr {

// Registers a one-shot "this exact fault is expected" self-test: if the
// next exception's vector matches, isr_handler stashes its error_code
// into last_error_code, moves the saved RIP to resume_rip, and returns
// (resuming execution there via iretq) instead of halting. Consumed
// after one match — any other fault, or a mismatched vector, still
// halts. The caller is responsible for verifying last_error_code/
// printing its own success marker after resuming; this is deliberately
// narrow (used only by the divide-by-zero and W^X self-tests), not a
// general recovery mechanism.
void expect_fault(uint64_t vector, uint64_t resume_rip);

extern uint64_t last_error_code;

} // namespace isr
