CXX  := x86_64-elf-g++
LD   := x86_64-elf-ld
NASM := nasm

CXXFLAGS := -std=c++23 -ffreestanding \
            -fno-stack-protector -fno-stack-check \
            -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
            -fno-exceptions -fno-rtti -fno-ident \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -Wall -Wextra \
            -Iinclude -Ikernel

NASMFLAGS := -f elf64

LDFLAGS := -m i386pep -T linker/uefi.ld --subsystem 10 \
           --image-base 0x400000 -e efi_main

BUILD := build
ESP   := $(BUILD)/esp

CXX_OBJS := $(BUILD)/boot.o \
            $(BUILD)/main.o \
            $(BUILD)/gdt.o \
            $(BUILD)/idt.o \
            $(BUILD)/isr.o

ASM_OBJS := $(BUILD)/gdt_flush.o \
            $(BUILD)/isr_stubs.o

OBJS := $(CXX_OBJS) $(ASM_OBJS)

.PHONY: all clean run test-boot

all: $(ESP)/EFI/BOOT/BOOTX64.EFI

$(BUILD)/boot.o: boot/boot.cpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/main.o: kernel/main.cpp kernel/gdt.hpp kernel/idt.hpp include/serial.hpp
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
