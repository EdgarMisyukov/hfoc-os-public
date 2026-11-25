/*
 * ITEM-626: native user image layout.
 *
 * The loader accepts page-aligned static ET_EXEC segments with no W+X range.
 * Text and read-only data are one RX segment, writable data and bss are a
 * separate RW/NX segment, and both start on a page boundary at 0x400000.
 */
ENTRY(_start)

PHDRS
{
    text PT_LOAD FLAGS(5);
    data PT_LOAD FLAGS(6);
}

SECTIONS
{
    . = 0x400000;
    .text : {
        *(.text.startup)
        *(.text*)
    } :text

    . = ALIGN(0x1000);
    .rodata : {
        *(.rodata*)
    } :text

    . = ALIGN(0x1000);
    .data : {
        *(.data*)
    } :data

    . = ALIGN(0x1000);
    .bss : {
        *(COMMON)
        *(.bss*)
    } :data

    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
