#pragma once

namespace gdt {

// Replaces UEFI's GDT with our own flat, ring-0-only long-mode GDT and
// reloads every segment register (including CS, which needs the far-return
// trick — see gdt_flush.asm).
void init();

} // namespace gdt
