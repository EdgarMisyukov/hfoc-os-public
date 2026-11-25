.section .text

.global sched_kernel_thread_trampoline
.type sched_kernel_thread_trampoline, @function
sched_kernel_thread_trampoline:
    call sched_reclaim_deferred
    popq %rax
    sti
    call *%rax
    call sched_exit
    cli
1:
    hlt
    jmp 1b
.size sched_kernel_thread_trampoline, .-sched_kernel_thread_trampoline

.section .note.GNU-stack,"",@progbits
