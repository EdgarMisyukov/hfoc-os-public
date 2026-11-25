/*
 * ITEM-626: the native user entry point.
 *
 * At entry the kernel has published the SysV-style initial stack:
 *   argc, argv[0..argc-1], NULL, envp[0..envc-1], NULL
 * There is no libc and no dynamic loader; the runtime below is all a program
 * gets.  main() returning is the same as exit(main()).
 */
.section .text.startup
.global _start
.type _start, @function
_start:
    movq (%rsp), %rdi
    leaq 8(%rsp), %rsi
    leaq 16(%rsp,%rdi,8), %rdx
    movq %rdx, environ(%rip)
    movq %rdi, hfoc_argc(%rip)
    movq %rsi, hfoc_argv(%rip)
    movq %rdx, hfoc_envp(%rip)
    andq $-16, %rsp
    call main
    movl %eax, %edi
    call hfoc_exit
1:
    hlt
    jmp 1b
.size _start, .-_start
