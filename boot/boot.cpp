#include "efi.hpp"
#include "serial.hpp"

extern "C" void kernel_main(efi::Handle image_handle, efi::SystemTable* system_table);

extern "C" EFIAPI void* efi_main(efi::Handle image_handle, efi::SystemTable* system_table) {
    serial::init();
    serial::puts("[OK] boot\r\n");

    kernel_main(image_handle, system_table);

    for (;;) {
        asm volatile("hlt");
    }
}
