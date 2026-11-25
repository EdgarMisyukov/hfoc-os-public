/* ITEM-625: a runnable child the parent cancels before it exits. */
.text
.global _start
_start:
    movl $0, %eax
    movl $1, %edi
    leaq message(%rip), %rsi
    movl $9, %edx
    syscall
    movl $1, %eax
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
1:
    pause
    jmp 1b
message:
    .ascii "SPIN-RUN\n"
