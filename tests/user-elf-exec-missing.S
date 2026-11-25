/* ITEM-625: a failed exec must leave the caller running its old image. */
.text
.global _start
_start:
    leaq path(%rip), %rdi
    xorl %esi, %esi
    xorl %edx, %edx
    movl $15, %eax
    syscall
    cmpl $5, %eax
    jne bad
    movl $0, %eax
    movl $1, %edi
    leaq kept_message(%rip), %rsi
    movl $10, %edx
    syscall
    xorl %edi, %edi
    jmp exit
bad:
    movl $0, %eax
    movl $1, %edi
    leaq bad_message(%rip), %rsi
    movl $9, %edx
    syscall
    movl $3, %edi
exit:
    movl $2, %eax
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
1:
    hlt
    jmp 1b
path:
    .asciz "/bin/missing"
kept_message:
    .ascii "EXEC-KEPT\n"
bad_message:
    .ascii "EXEC-BAD\n"
