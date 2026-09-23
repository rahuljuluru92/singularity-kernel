/*
 * Minimal PE32+ layout for the i386pep (x86_64 PE) ld emulation.
 *
 * Deliberately NOT ld's built-in default PE script: that script always
 * emits a .idata output section (a null "end of import list" terminator,
 * even with zero real imports), which makes ld populate the PE Import
 * Directory data-directory entry. EDK2's PE/COFF loader has no DLL import
 * resolution and rejects any image with a populated Import Directory with
 * "Load Error" — see DECISIONS.md, 2026-09-23. Omitting .idata entirely
 * keeps that directory entry at RVA=0/Size=0.
 */
OUTPUT_FORMAT(pei-x86-64)
ENTRY(efi_main)

SECTIONS
{
  . = SIZEOF_HEADERS;
  . = ALIGN(__section_alignment__);

  .text __image_base__ + (__section_alignment__ < 0x1000 ? . : __section_alignment__) :
  {
    __text_start = .;
    *(.text)
    *(.text.*)
    . = ALIGN(0x1000); /* page-align so W^X permissions apply per-page cleanly */
    __text_end = .;
  }

  .data BLOCK(__section_alignment__) :
  {
    *(.rdata)
    *(.rdata.*)
    *(.rodata)
    *(.rodata.*)
    *(.data)
    *(.data.*)
  }

  .bss BLOCK(__section_alignment__) :
  {
    *(.bss)
    *(COMMON)
  }

  .reloc BLOCK(__section_alignment__) :
  {
    *(.reloc)
  }

  /DISCARD/ :
  {
    *(.idata*)
    *(.edata)
    *(.pdata*)
    *(.xdata*)
    *(.tls*)
    *(.rsrc*)
    *(.didat*)
    *(.eh_frame*)
    *(.comment)
    *(.note.*)
    *(.ctors)
    *(.dtors)
    *(.drectve)
    *(.CRT*)
  }
}
