/* Apploader: a flat image loaded at 0x81200000 (entry first). BSS is placed in
   the data output section so the raw binary already contains its zero bytes. */
ENTRY(apploader_entry)
SECTIONS
{
    . = 0x81200000;
    .text : { *(.text.start) *(.text .text.*) *(.rodata .rodata.*) }
    .data : { *(.data .data.*) *(.sdata .sdata.*) *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) . = ALIGN(32); }
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
