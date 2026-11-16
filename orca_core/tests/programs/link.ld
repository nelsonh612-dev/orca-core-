/* Test programs: code and data from 0x80003100 (the usual DOL load area).
   Explicit PHDRS keep the ELF/program headers out of the loaded image, so
   nothing is written over the exception vectors at 0x80000000. */
ENTRY(_start)
PHDRS
{
    text PT_LOAD FLAGS(5);   /* R X */
    data PT_LOAD FLAGS(6);   /* R W */
}
SECTIONS
{
    . = 0x80003100;
    .text   : { *(.text.start) *(.text .text.*) } :text
    .rodata : { *(.rodata .rodata.*) } :text
    . = ALIGN(32);
    .data   : { *(.data .data.*) *(.sdata .sdata.*) *(.got2) } :data
    .bss    : { *(.sbss .sbss.*) *(.bss .bss.*) *(COMMON) } :data
    /DISCARD/ : { *(.comment) *(.eh_frame) *(.note*) }
}
