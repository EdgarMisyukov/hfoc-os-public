.section .text

.global context_switch_asm
.type context_switch_asm, @function
context_switch_asm:
    movq %r15, 0(%rdi)
    movq %r14, 8(%rdi)
    movq %r13, 16(%rdi)
    movq %r12, 24(%rdi)
    movq %rbx, 32(%rdi)
    movq %rbp, 40(%rdi)
    movq (%rsp), %rax
    movq %rax, 48(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 56(%rdi)

    movq 56(%rsi), %rsp
    subq $8, %rsp
    movq 48(%rsi), %rax
    movq %rax, (%rsp)
    movq 0(%rsi), %r15
    movq 8(%rsi), %r14
    movq 16(%rsi), %r13
    movq 24(%rsi), %r12
    movq 32(%rsi), %rbx
    movq 40(%rsi), %rbp
    ret
.size context_switch_asm, .-context_switch_asm

.section .note.GNU-stack,"",@progbits
