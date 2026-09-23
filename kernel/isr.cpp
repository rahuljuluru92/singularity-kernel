#include "isr.hpp"
#include "serial.hpp"

namespace {

constexpr uint64_t kNoExpectation = static_cast<uint64_t>(-1);

uint64_t g_expect_vector = kNoExpectation;
uint64_t g_resume_rip = 0;

} // namespace

namespace isr {

uint64_t last_error_code = 0;

void expect_fault(uint64_t vector, uint64_t resume_rip) {
    g_expect_vector = vector;
    g_resume_rip = resume_rip;
}

} // namespace isr

// Stage 2/3 have no general recovery path for any exception — real
// recovery (e.g. resuming after a page fault to grow a stack) is
// later-stage work. Every vector ends up here; an unmatched one halts.
// A vector matching a registered isr::expect_fault() resumes instead —
// see isr.hpp for why that's safe and deliberately narrow.
extern "C" void isr_handler(registers* regs) {
    if (regs->vector == g_expect_vector) {
        isr::last_error_code = regs->error_code;
        g_expect_vector = kNoExpectation;
        regs->rip = g_resume_rip;
        return;
    }

    serial::puts("[PANIC] unexpected exception vector=");
    serial::put_hex(regs->vector);
    serial::puts(" error_code=");
    serial::put_hex(regs->error_code);
    serial::puts(" rip=");
    serial::put_hex(regs->rip);
    serial::puts("\r\n");

    for (;;) {
        asm volatile("cli\n\thlt");
    }
}
