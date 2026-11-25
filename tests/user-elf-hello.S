/* ITEM-625: a minimal static ELF64 program the QEMU proof executes. */
.text
.global _start
_start:
    cmpq $2, (%rsp)
    jne bad_args
    cmpq $0, 8(%rsp)
    je bad_args
    cmpq $0, 16(%rsp)
    je bad_args
    movl $0, %eax
    movl $1, %edi
    leaq message(%rip), %rsi
    movl $11, %edx
    syscall
    jmp exit_ok
bad_args:
    movl $0, %eax
    movl $1, %edi
    leaq bad_message(%rip), %rsi
    movl $8, %edx
    syscall
exit_ok:
    movl $2, %eax
    xorl %edi, %edi
    xorl %esi, %esi
    xorl %edx, %edx
    syscall
1:
    hlt
    jmp 1b
message:
    .ascii "HELLO-ARG!\n"
bad_message:
    .ascii "BAD-ARG\n"
