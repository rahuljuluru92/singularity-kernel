CXX  := x86_64-elf-g++
LD   := x86_64-elf-ld
NASM := nasm

CXXFLAGS := -std=c++23 -ffreestanding \
            -fno-stack-protector -fno-stack-check \
            -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
            -fno-exceptions -fno-rtti -fno-ident \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -Wall -Wextra \
            -Iinclude -Ikernel -Ipmm -Iheap

NASMFLAGS := -f elf64

LDFLAGS := -m i386pep -T linker/uefi.ld --subsystem 10 \
           --image-base 0x400000 -e efi_main

BUILD := build
ESP   := $(BUILD)/esp

CXX_OBJS := $(BUILD)/boot.o \
            $(BUILD)/main.o \
            $(BUILD)/gdt.o \
            $(BUILD)/idt.o \
            $(BUILD)/isr.o \
            $(BUILD)/pmm.o \
            $(BUILD)/bitmap_allocator.o \
            $(BUILD)/paging.o \
            $(BUILD)/heap.o \
            $(BUILD)/freelist_allocator.o \
            $(BUILD)/mem.o \
            $(BUILD)/pic.o \
            $(BUILD)/pit.o \
            $(BUILD)/sched.o \
            $(BUILD)/pci.o \
            $(BUILD)/virtio_blk.o \
            $(BUILD)/fat32.o

ASM_OBJS := $(BUILD)/gdt_flush.o \
            $(BUILD)/isr_stubs.o

OBJS := $(CXX_OBJS) $(ASM_OBJS)

.PHONY: all clean run test-boot

all: $(ESP)/EFI/BOOT/BOOTX64.EFI

$(BUILD)/boot.o: boot/boot.cpp include/serial.hpp kernel/efi.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/main.o: kernel/main.cpp kernel/gdt.hpp kernel/idt.hpp kernel/isr.hpp kernel/pmm.hpp kernel/paging.hpp kernel/heap.hpp kernel/pic.hpp kernel/pit.hpp kernel/sched.hpp kernel/virtio_blk.hpp kernel/fat32.hpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/heap.o: kernel/heap.cpp kernel/heap.hpp kernel/paging.hpp kernel/pmm.hpp heap/freelist_allocator.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/freelist_allocator.o: heap/freelist_allocator.cpp heap/freelist_allocator.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/mem.o: kernel/mem.cpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/pic.o: kernel/pic.cpp kernel/pic.hpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/pit.o: kernel/pit.cpp kernel/pit.hpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/sched.o: kernel/sched.cpp kernel/sched.hpp kernel/heap.hpp kernel/isr.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/pci.o: kernel/pci.cpp kernel/pci.hpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/virtio_blk.o: kernel/virtio_blk.cpp kernel/virtio_blk.hpp kernel/pci.hpp kernel/pmm.hpp kernel/paging.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/fat32.o: kernel/fat32.cpp kernel/fat32.hpp kernel/virtio_blk.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/pmm.o: kernel/pmm.cpp kernel/pmm.hpp kernel/efi.hpp pmm/bitmap_allocator.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/paging.o: kernel/paging.cpp kernel/paging.hpp kernel/pmm.hpp pmm/bitmap_allocator.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/bitmap_allocator.o: pmm/bitmap_allocator.cpp pmm/bitmap_allocator.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/gdt.o: kernel/gdt.cpp kernel/gdt.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/idt.o: kernel/idt.cpp kernel/idt.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/isr.o: kernel/isr.cpp kernel/isr.hpp kernel/pic.hpp kernel/sched.hpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/gdt_flush.o: kernel/gdt_flush.asm
	@mkdir -p $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/isr_stubs.o: kernel/isr_stubs.asm
	@mkdir -p $(BUILD)
	$(NASM) $(NASMFLAGS) $< -o $@

$(BUILD)/kernel.efi: $(OBJS) linker/uefi.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(ESP)/EFI/BOOT/BOOTX64.EFI: $(BUILD)/kernel.efi
	@mkdir -p $(ESP)/EFI/BOOT
	cp $< $@

clean:
	rm -rf $(BUILD)

run: all
	./scripts/run.sh

test-boot: all
	./scripts/test-boot.sh

# Host-side unit tests for OS-independent logic (pmm/), compiled with the
# host's own compiler — not the freestanding cross-compiler — and run
# directly, no QEMU involved. See CLAUDE.md's Testing section.
CATCH2_PREFIX := $(shell brew --prefix catch2)
TEST_CXXFLAGS := -std=c++23 -Wall -Wextra -Ipmm -Iheap -I$(CATCH2_PREFIX)/include
TEST_LDFLAGS  := -L$(CATCH2_PREFIX)/lib -lCatch2Main -lCatch2
TEST_BIN      := $(BUILD)/host_tests
TEST_SRCS     := tests/test_bitmap_allocator.cpp pmm/bitmap_allocator.cpp \
                  tests/test_freelist_allocator.cpp heap/freelist_allocator.cpp

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): $(TEST_SRCS) pmm/bitmap_allocator.hpp heap/freelist_allocator.hpp
	@mkdir -p $(BUILD)
	c++ $(TEST_CXXFLAGS) $(TEST_SRCS) -o $@ $(TEST_LDFLAGS)
