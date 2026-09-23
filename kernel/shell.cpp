#include "shell.hpp"

#include "fat32.hpp"
#include "heap.hpp"
#include "pmm.hpp"
#include "sched.hpp"
#include "serial.hpp"

#include <cstddef>
#include <cstdint>

namespace shell {

namespace {

constexpr std::size_t kLineBufSize = 128;

// Backspace (0x08) and DEL (0x7F) both show up in practice depending on
// the terminal on the other end of the serial line; both are treated as
// "erase the previous character."
constexpr char kBackspace = 0x08;
constexpr char kDel = 0x7F;

void read_line(char* buf, std::size_t max_len) {
    std::size_t len = 0;
    for (;;) {
        char c = serial::getc();
        if (c == '\r' || c == '\n') {
            serial::puts("\r\n");
            buf[len] = '\0';
            return;
        }
        if ((c == kBackspace || c == kDel) && len > 0) {
            len--;
            serial::puts("\b \b"); // move back, erase, move back again
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len + 1 < max_len) {
            buf[len++] = c;
            serial::putc(c); // the remote terminal doesn't echo for us
        }
    }
}

struct Tokens {
    char* cmd;
    char* arg;
};

// Splits into at most two tokens — every command this shell has takes
// zero or one argument, so that's all parsing needs to support.
Tokens tokenize(char* line) {
    Tokens t{nullptr, nullptr};
    std::size_t i = 0;
    while (line[i] == ' ') {
        i++;
    }
    if (line[i] == '\0') {
        return t;
    }
    t.cmd = &line[i];
    while (line[i] != '\0' && line[i] != ' ') {
        i++;
    }
    if (line[i] == ' ') {
        line[i] = '\0';
        i++;
        while (line[i] == ' ') {
            i++;
        }
        if (line[i] != '\0') {
            t.arg = &line[i];
        }
    }
    return t;
}

bool streq(const char* a, const char* b) {
    std::size_t i = 0;
    while (a[i] != '\0' && b[i] != '\0') {
        if (a[i] != b[i]) {
            return false;
        }
        i++;
    }
    return a[i] == b[i];
}

void cmd_help() {
    serial::puts("Commands:\r\n"
                  "  help            this message\r\n"
                  "  ls              list files\r\n"
                  "  cat <file>      print a file's contents\r\n"
                  "  meminfo         physical/heap memory usage\r\n"
                  "  tasks           scheduler task list\r\n");
}

void cmd_ls() {
    fat32::list_root([](const char* name, uint32_t size) {
        serial::puts("  ");
        serial::puts(name);
        serial::puts("  ");
        serial::put_hex(size);
        serial::puts(" bytes\r\n");
    });
}

void cmd_cat(const char* filename) {
    if (filename == nullptr) {
        serial::puts("usage: cat <file>\r\n");
        return;
    }
    static char buf[8192];
    int n = fat32::read_file(filename, buf, sizeof(buf) - 1);
    if (n < 0) {
        serial::puts("file not found: ");
        serial::puts(filename);
        serial::puts("\r\n");
        return;
    }
    buf[n] = '\0';
    serial::puts(buf);
    serial::puts("\r\n");
}

void cmd_meminfo() {
    serial::puts("pmm:  free=");
    serial::put_hex(pmm::free_pages());
    serial::puts(" total=");
    serial::put_hex(pmm::total_pages());
    serial::puts(" pages\r\n");
    serial::puts("heap: free=");
    serial::put_hex(heap::free_bytes());
    serial::puts(" used=");
    serial::put_hex(heap::used_bytes());
    serial::puts(" bytes\r\n");
}

void cmd_tasks() {
    sched::for_each_task([](int id, const char* name, bool active) {
        serial::puts("  ");
        serial::put_hex(static_cast<uint64_t>(id));
        serial::puts(" ");
        serial::puts(name);
        serial::puts(active ? " active\r\n" : " finished\r\n");
    });
}

} // namespace

[[noreturn]] void run() {
    serial::puts("\r\nSingularity Kernel shell. Type 'help' for commands.\r\n");

    char line[kLineBufSize];
    for (;;) {
        serial::puts("> ");
        read_line(line, sizeof(line));

        Tokens t = tokenize(line);
        if (t.cmd == nullptr) {
            continue;
        }

        if (streq(t.cmd, "help")) {
            cmd_help();
        } else if (streq(t.cmd, "ls")) {
            cmd_ls();
        } else if (streq(t.cmd, "cat")) {
            cmd_cat(t.arg);
        } else if (streq(t.cmd, "meminfo")) {
            cmd_meminfo();
        } else if (streq(t.cmd, "tasks")) {
            cmd_tasks();
        } else {
            serial::puts("unknown command: ");
            serial::puts(t.cmd);
            serial::puts(" (try 'help')\r\n");
        }
    }
}

} // namespace shell
