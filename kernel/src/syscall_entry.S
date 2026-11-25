.section .bss
.align 16
/* ITEM-627: points at the running task's entry frame (see scheduler_task). */
.global syscall_frame_current
syscall_frame_current:
    .quad 0
/* Only used before the scheduler publishes a task. */
.global syscall_saved_user_rsp
syscall_saved_user_rsp:
    .quad 0
.global syscall_saved_rip
syscall_saved_rip:
    .quad 0
.global syscall_saved_rflags
syscall_saved_rflags:
    .quad 0
.global syscall_saved_arg0
syscall_saved_arg0:
    .quad 0
.global syscall_saved_arg1
syscall_saved_arg1:
    .quad 0
.global syscall_saved_arg2
syscall_saved_arg2:
    .quad 0
.align 16
.global syscall_kernel_stack
syscall_kernel_stack:
    .skip 16384
.global syscall_kernel_stack_top
syscall_kernel_stack_top:

.section .text
.extern syscall_handle
/* Host ABI proofs link this entry without the scheduler; the weak symbols
   fall back to the pre-scheduler globals below. */
.weak sched_syscall_stack_top
.weak sched_running_task

/*
 * The syscall frame lives in the task record: arg2, arg1, arg0, rflags, rip,
 * user rsp at offsets 0..40.  A cooperative switch inside a blocking syscall
 * therefore cannot overwrite it, and the resumed task reads its own frame.
 * The scheduler publishes the resumed task's page table before it runs.
 */
.global syscall_entry
.type syscall_entry, @function
syscall_entry:
    movq sched_running_task(%rip), %r10
    testq %r10, %r10
    jz 9f
    movq %rdx, 0(%r10)
    movq %rsi, 8(%r10)
    movq %rdi, 16(%r10)
    movq %r11, 24(%r10)
    movq %rcx, 32(%r10)
    movq %rsp, 40(%r10)
    movq %r10, syscall_frame_current(%rip)
    movq sched_syscall_stack_top(%rip), %rsp
    testq %rsp, %rsp
    jnz 1f
    leaq syscall_kernel_stack_top(%rip), %rsp
1:
    movq %rax, %rdi
    movq 16(%r10), %rsi
    movq 8(%r10), %rdx
    movq 0(%r10), %rcx
    call syscall_handle
    movq sched_running_task(%rip), %r10
    testq %r10, %r10
    jz 8f
    cli
    movq 24(%r10), %r11
    movq 32(%r10), %rcx
    movq 16(%r10), %rdi
    movq 8(%r10), %rsi
    movq 0(%r10), %rdx
    movq 40(%r10), %rsp
    sysretq
8:
    leaq syscall_kernel_stack_top(%rip), %rsp
    sysretq
9:
    movq %rsp, syscall_saved_user_rsp(%rip)
    movq %rcx, syscall_saved_rip(%rip)
    movq %r11, syscall_saved_rflags(%rip)
    movq %rdi, syscall_saved_arg0(%rip)
    movq %rsi, syscall_saved_arg1(%rip)
    movq %rdx, syscall_saved_arg2(%rip)
    leaq syscall_kernel_stack_top(%rip), %rsp
    movq %rax, %rdi
    movq syscall_saved_arg0(%rip), %rsi
    movq syscall_saved_arg1(%rip), %rdx
    movq syscall_saved_arg2(%rip), %rcx
    call syscall_handle
    movq syscall_saved_rflags(%rip), %r11
    movq syscall_saved_rip(%rip), %rcx
    movq syscall_saved_arg0(%rip), %rdi
    movq syscall_saved_arg1(%rip), %rsi
    movq syscall_saved_arg2(%rip), %rdx
    movq syscall_saved_user_rsp(%rip), %rsp
    sysretq
.size syscall_entry, .-syscall_entry

.section .note.GNU-stack,"",@progbits
