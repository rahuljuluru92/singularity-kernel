CXX  := x86_64-elf-g++
LD   := x86_64-elf-ld
NASM := nasm

CXXFLAGS := -std=c++23 -ffreestanding \
            -fno-stack-protector -fno-stack-check \
            -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
            -fno-exceptions -fno-rtti -fno-ident \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -Wall -Wextra \
            -Iinclude -Ikernel -Ipmm

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
            $(BUILD)/paging.o

ASM_OBJS := $(BUILD)/gdt_flush.o \
            $(BUILD)/isr_stubs.o

OBJS := $(CXX_OBJS) $(ASM_OBJS)

.PHONY: all clean run test-boot

all: $(ESP)/EFI/BOOT/BOOTX64.EFI

$(BUILD)/boot.o: boot/boot.cpp include/serial.hpp kernel/efi.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/main.o: kernel/main.cpp kernel/gdt.hpp kernel/idt.hpp kernel/isr.hpp kernel/pmm.hpp kernel/paging.hpp include/serial.hpp
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

$(BUILD)/isr.o: kernel/isr.cpp kernel/isr.hpp include/serial.hpp
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
TEST_CXXFLAGS := -std=c++23 -Wall -Wextra -Ipmm -I$(CATCH2_PREFIX)/include
TEST_LDFLAGS  := -L$(CATCH2_PREFIX)/lib -lCatch2Main -lCatch2
TEST_BIN      := $(BUILD)/host_tests

.PHONY: test
test: $(TEST_BIN)
	$(TEST_BIN)

$(TEST_BIN): tests/test_bitmap_allocator.cpp pmm/bitmap_allocator.cpp pmm/bitmap_allocator.hpp
	@mkdir -p $(BUILD)
	c++ $(TEST_CXXFLAGS) tests/test_bitmap_allocator.cpp pmm/bitmap_allocator.cpp -o $@ $(TEST_LDFLAGS)
