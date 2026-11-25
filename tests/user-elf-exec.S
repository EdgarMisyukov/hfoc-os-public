/* ITEM-625: exec a different native ELF in place, preserving the process. */
.text
.global _start
_start:
    subq $32, %rsp
    leaq arg0(%rip), %rax
    movq %rax, 0(%rsp)
    leaq arg1(%rip), %rax
    movq %rax, 8(%rsp)
    movq $0, 16(%rsp)
    leaq path(%rip), %rdi
    movq %rsp, %rsi
    xorl %edx, %edx
    movl $15, %eax
    syscall
    /* exec failed: report it and exit nonzero */
    movl $0, %eax
    movl $1, %edi
    leaq fail_message(%rip), %rsi
    movl $10, %edx
    syscall
    movl $2, %eax
    movl $1, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
1:
    hlt
    jmp 1b
path:
    .asciz "/bin/hello"
arg0:
    .asciz "hello"
arg1:
    .asciz "arg"
fail_message:
    .ascii "EXEC-FAIL\n"
