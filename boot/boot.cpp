#include "serial.hpp"

// UEFI always calls the application entry point using the Microsoft x64
// calling convention, regardless of what ABI the host toolchain defaults
// to — ms_abi forces GCC to honor that at this boundary.
#define EFIAPI __attribute__((ms_abi))

extern "C" EFIAPI void* efi_main(void* image_handle, void* system_table) {
    (void)image_handle;
    (void)system_table;

    serial::init();
    serial::puts("[OK] boot\r\n");

    for (;;) {
        asm volatile("hlt");
    }
}
