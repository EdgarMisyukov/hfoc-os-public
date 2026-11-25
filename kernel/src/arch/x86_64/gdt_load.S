.section .text

.global gdt_load_table
.type gdt_load_table, @function
gdt_load_table:
    lgdt (%rdi)
    ret
.size gdt_load_table, .-gdt_load_table

.global gdt_reload_segments_asm
.type gdt_reload_segments_asm, @function
gdt_reload_segments_asm:
    pushq $0x08
    leaq .Lreload_cs(%rip), %rax
    pushq %rax
    lretq
.Lreload_cs:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    xorw %ax, %ax
    movw %ax, %fs
    movw %ax, %gs
    ret
.size gdt_reload_segments_asm, .-gdt_reload_segments_asm

.section .note.GNU-stack,"",@progbits
