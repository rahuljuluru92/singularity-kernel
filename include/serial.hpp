#pragma once

namespace serial {

inline void outb(unsigned short port, unsigned char val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

inline unsigned char inb(unsigned short port) {
    unsigned char ret;
    asm volatile("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

constexpr unsigned short kCom1 = 0x3F8;

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

} // namespace serial
