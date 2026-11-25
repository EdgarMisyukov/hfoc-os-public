#ifndef HFOC_CONTEXT_H
#define HFOC_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#define CONTEXT_STACK_ALIGNMENT 16U
#define CONTEXT_FRAME_ALIGNMENT 8U

enum context_status {
    CONTEXT_OK = 0,
    CONTEXT_NULL = 1,
    CONTEXT_BAD_STACK = 2,
    CONTEXT_MISALIGNED = 3,
};

struct context_frame {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rsp;
};

enum context_status context_frame_init(struct context_frame *frame,
                                       uintptr_t stack_top,
                                       void (*entry)(void));
bool context_frame_is_valid(const struct context_frame *frame);
enum context_status context_switch(struct context_frame *current,
                                   struct context_frame *next);
const char *context_status_string(enum context_status status);

#endif
