#include "paging.hpp"

#include "bitmap_allocator.hpp" // for pmm::kPageSize
#include "pmm.hpp"
#include "serial.hpp"

#include <cstdint>

extern "C" {
extern char __text_start[];
extern char __text_end[];
}

namespace paging {

namespace {

using Table = uint64_t[512];

constexpr uint64_t kPresent = 1ULL << 0;
constexpr uint64_t kWritable = 1ULL << 1;
constexpr uint64_t kNx = 1ULL << 63;
constexpr uint64_t kAddrMask = 0x000FFFFFFFFFF000ULL;

Table* g_pml4 = nullptr;

// Extra individually-registered executable pages, besides the kernel's
// own .text range — see paging::add_executable_page. Small fixed
// capacity: this is only ever the one AP trampoline page so far, and
// there's no dynamic allocation available this early (init() itself is
// what makes allocation meaningful, via the direct map it builds).
constexpr int kMaxExtraExecutablePages = 4;
uint64_t g_extra_executable_pages[kMaxExtraExecutablePages];
int g_extra_executable_page_count = 0;

[[noreturn]] void panic(const char* msg) {
    serial::puts("[PANIC] paging::init: ");
    serial::puts(msg);
    serial::puts("\r\n");
    for (;;) {
        asm volatile("cli\n\thlt");
    }
}

Table* alloc_table() {
    uintptr_t phys = pmm::alloc_page();
    if (phys == 0) {
        panic("out of physical memory while building page tables");
    }
    // Still running under UEFI's own page tables at this point (this
    // function only runs before the CR3 switch below), which map all
    // installed RAM 1:1 — so the physical address pmm just handed back
    // is directly usable as a pointer. See ASSUMPTIONS.md.
    auto* table = reinterpret_cast<Table*>(phys);
    for (int i = 0; i < 512; i++) {
        (*table)[i] = 0;
    }
    return table;
}

// Intermediate levels (PML4E/PDPTE/PDE) always stay present+writable and
// never set NX: x86 permission checks AND the writable bit and OR the NX
// bit across every level of the walk, so restricting anything above the
// final PTE would make per-page W^X impossible to express at the PTE
// level — the actual enforcement happens only in map_page's pte_flags.
Table* next_level(Table* table, size_t index) {
    uint64_t entry = (*table)[index];
    if (entry & kPresent) {
        return reinterpret_cast<Table*>(entry & kAddrMask);
    }
    Table* child = alloc_table();
    (*table)[index] = reinterpret_cast<uint64_t>(child) | kPresent | kWritable;
    return child;
}

void map_page(uint64_t virt, uint64_t phys, uint64_t pte_flags) {
    size_t pml4_i = (virt >> 39) & 0x1FF;
    size_t pdpt_i = (virt >> 30) & 0x1FF;
    size_t pd_i = (virt >> 21) & 0x1FF;
    size_t pt_i = (virt >> 12) & 0x1FF;

    Table* pdpt = next_level(g_pml4, pml4_i);
    Table* pd = next_level(pdpt, pdpt_i);
    Table* pt = next_level(pd, pd_i);
    (*pt)[pt_i] = (phys & kAddrMask) | kPresent | pte_flags;
}

uint64_t permissions_for(uintptr_t phys) {
    uintptr_t text_start = reinterpret_cast<uintptr_t>(__text_start);
    uintptr_t text_end = reinterpret_cast<uintptr_t>(__text_end);
    if (phys >= text_start && phys < text_end) {
        return kPresent; // read + execute, not writable
    }
    uintptr_t page = phys & kAddrMask;
    for (int i = 0; i < g_extra_executable_page_count; i++) {
        if (page == g_extra_executable_pages[i]) {
            // Present + writable + executable — a deliberate, narrow W^X
            // exception, unlike every other page in this kernel. The AP
            // trampoline is the only page registered this way: the BSP
            // must write (memcpy + patch) it, and the AP itself must
            // execute from that exact same physical page in the instant
            // right after enabling paging, before it's reached any other
            // executable memory. A stricter design (write during copy,
            // then flip to execute-only before the first SIPI) is
            // possible but wasn't judged worth the added complexity for
            // one boot-time-only page never touched again once every AP
            // is up. See DECISIONS.md, stage 9.
            return kPresent | kWritable;
        }
    }
    return kPresent | kWritable | kNx; // read + write, not executable
}

uint64_t read_msr(uint32_t msr) {
    uint32_t lo, hi;
    asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

void write_msr(uint32_t msr, uint64_t value) {
    uint32_t lo = static_cast<uint32_t>(value & 0xFFFFFFFF);
    uint32_t hi = static_cast<uint32_t>(value >> 32);
    asm volatile("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

constexpr uint32_t kEferMsr = 0xC0000080;
constexpr uint64_t kEferNxe = 1ULL << 11;

} // namespace

void add_executable_page(uint64_t phys) {
    if (g_extra_executable_page_count >= kMaxExtraExecutablePages) {
        panic("add_executable_page: out of extra-executable-page slots");
    }
    g_extra_executable_pages[g_extra_executable_page_count++] = phys & kAddrMask;
}

void init() {
    g_pml4 = alloc_table();

    std::size_t pages = pmm::total_pages();
    for (std::size_t i = 0; i < pages; i++) {
        uint64_t phys = static_cast<uint64_t>(i) * pmm::kPageSize;
        uint64_t flags = permissions_for(phys);
        map_page(phys, phys, flags);                      // low identity alias
        map_page(to_higher_half(phys), phys, flags);       // higher-half alias
    }

    write_msr(kEferMsr, read_msr(kEferMsr) | kEferNxe);

    asm volatile("mov %0, %%cr3" : : "r"(reinterpret_cast<uint64_t>(g_pml4)) : "memory");
    serial::puts("[OK] paging\r\n");

    void* target = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&&higher_half_entry) +
                                            kHigherHalfBase);
    goto* target;

higher_half_entry:
    serial::puts("[OK] higher-half\r\n");
}

void map_mmio(uint64_t phys) {
    uint64_t page = phys & kAddrMask;
    map_page(page, page, kWritable | kNx);
}

uint64_t root_table_physical() { return reinterpret_cast<uint64_t>(g_pml4); }

} // namespace paging
