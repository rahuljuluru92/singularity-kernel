CXX := x86_64-elf-g++
LD  := x86_64-elf-ld

CXXFLAGS := -std=c++23 -ffreestanding \
            -fno-stack-protector -fno-stack-check \
            -fshort-wchar -mno-red-zone -maccumulate-outgoing-args \
            -fno-exceptions -fno-rtti -fno-ident \
            -fno-asynchronous-unwind-tables -fno-unwind-tables \
            -Wall -Wextra \
            -Iinclude

LDFLAGS := -m i386pep -T linker/uefi.ld --subsystem 10 \
           --image-base 0x400000 -e efi_main

BUILD := build
ESP   := $(BUILD)/esp

.PHONY: all clean run test-boot

all: $(ESP)/EFI/BOOT/BOOTX64.EFI

$(BUILD)/boot.o: boot/boot.cpp include/serial.hpp
	@mkdir -p $(BUILD)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/kernel.efi: $(BUILD)/boot.o linker/uefi.ld
	$(LD) $(LDFLAGS) -o $@ $(BUILD)/boot.o

$(ESP)/EFI/BOOT/BOOTX64.EFI: $(BUILD)/kernel.efi
	@mkdir -p $(ESP)/EFI/BOOT
	cp $< $@

clean:
	rm -rf $(BUILD)

run: all
	./scripts/run.sh

test-boot: all
	./scripts/test-boot.sh
