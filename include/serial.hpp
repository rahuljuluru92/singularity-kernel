#pragma once

#include <cstdint>

namespace serial {

inline void outb(unsigned short port, unsigned char val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

// 16/32-bit port I/O — needed by PCI config space access and virtio's
// legacy I/O-port device registers (kernel/pci.cpp, kernel/virtio_blk.cpp).
// Homed here alongside outb/inb since they're equally generic, not
// serial-specific, and this project already reuses this namespace's
// port I/O elsewhere (e.g. kernel/pic.cpp).
inline void outw(unsigned short port, unsigned short val) {
    asm volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

inline unsigned short inw(unsigned short port) {
    unsigned short ret;
    asm volatile("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

inline void outl(unsigned short port, unsigned int val) {
    asm volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

inline unsigned int inl(unsigned short port) {
    unsigned int ret;
    asm volatile("inl %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

constexpr unsigned short kCom1 = 0x3F8;

// A whole-message spinlock (NOT per-byte) — deliberately coarse. Its
// only job is letting a multi-call diagnostic sequence (several
// puts()/put_hex() calls building one panic line) print as one
// contiguous, human-readable line even if another core panics at nearly
// the same moment, instead of interleaving byte-for-byte into an
// unreadable mess — a real problem hit while debugging stage 9/10's SMP
// work, where two genuinely concurrent panics were impossible to read
// apart. Callers bracket a WHOLE sequence of puts/put_hex calls with
// lock()/unlock(), not each individual call.
inline volatile int g_lock = 0;

inline void lock() {
    while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE) != 0) {
        asm volatile("pause");
    }
}

inline void unlock() { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

// 8N1, no FIFO tricks — just enough to get bytes onto the wire for
// QEMU's -serial to capture. Must run before any putc/puts call.
inline void init() {
    outb(kCom1 + 1, 0x00); // disable interrupts
    outb(kCom1 + 3, 0x80); // enable DLAB to set baud rate divisor
    outb(kCom1 + 0, 0x01); // divisor low byte: 115200 baud
    outb(kCom1 + 1, 0x00); // divisor high byte
    outb(kCom1 + 3, 0x03); // 8 bits, no parity, one stop bit; clear DLAB
    outb(kCom1 + 2, 0xC7); // enable FIFO, clear, 14-byte threshold
    outb(kCom1 + 4, 0x0B); // IRQs disabled, RTS/DSR set
}

inline void putc(char c) {
    while ((inb(kCom1 + 5) & 0x20) == 0) {
        // wait for transmit-holding-register-empty
    }
    outb(kCom1, static_cast<unsigned char>(c));
}

inline void puts(const char* s) {
    while (*s) {
        putc(*s++);
    }
}

// Blocks until a character is available, then returns it. hlt while
// waiting rather than busy-spinning — this project has no UART IRQ
// wired up (only the PIT, stage 5), so this can't sleep-until-exactly-
// ready, but hlt still wakes on the timer tick at worst (every 10 ms at
// the configured 100 Hz), which is a real, if coarse, efficiency win
// over a tight poll loop. Used by kernel/shell.cpp.
inline char getc() {
    while ((inb(kCom1 + 5) & 0x01) == 0) {
        asm volatile("hlt");
    }
    return static_cast<char>(inb(kCom1));
}

inline void put_hex(uint64_t value) {
    putc('0');
    putc('x');
    for (int shift = 60; shift >= 0; shift -= 4) {
        int nibble = (value >> shift) & 0xF;
        putc(nibble < 10 ? static_cast<char>('0' + nibble)
                          : static_cast<char>('a' + nibble - 10));
    }
}

} // namespace serial
