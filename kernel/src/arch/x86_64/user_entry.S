.section .text

.global user_enter_iretq
.type user_enter_iretq, @function
user_enter_iretq:
    movq %rdi, %rsp
    iretq
.size user_enter_iretq, .-user_enter_iretq

.section .note.GNU-stack,"",@progbits
