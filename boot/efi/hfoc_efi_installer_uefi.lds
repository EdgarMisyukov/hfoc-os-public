OUTPUT_FORMAT(pei-x86-64)
ENTRY(efi_main)

SECTIONS
{
    . = 0x1000;

    .text : ALIGN(0x1000) {
        *(.text .text.*)
    }

    .rodata : ALIGN(0x1000) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(0x1000) {
        *(.data .data.*)
    }

    .bss : ALIGN(0x1000) {
        *(.bss .bss.*)
        *(COMMON)
    }

    /DISCARD/ : {
        *(.comment)
        *(.debug*)
        *(.note*)
        *(.eh_frame*)
    }
}
