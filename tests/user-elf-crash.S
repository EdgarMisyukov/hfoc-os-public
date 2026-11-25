/* ITEM-625: a user process that faults at a null address. */
.text
.global _start
_start:
    xorl %eax, %eax
    movq (%rax), %rax
1:
    hlt
    jmp 1b
