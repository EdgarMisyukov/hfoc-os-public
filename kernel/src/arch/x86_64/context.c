#include "context.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(struct context_frame) == 64U,
               "context frame must be 64 bytes");
_Static_assert(_Alignof(struct context_frame) <= CONTEXT_FRAME_ALIGNMENT,
               "context frame must fit in 8-byte alignment");

void context_switch_asm(struct context_frame *current,
                        struct context_frame *next);

static bool is_canonical(uintptr_t address) {
    const uintptr_t upper = address >> 47U;
    return upper == 0U || upper == UINT64_C(0x1ffff);
}

enum context_status context_frame_init(struct context_frame *frame,
                                       uintptr_t stack_top,
                                       void (*entry)(void))
{
    if (frame == NULL || entry == NULL) {
        return CONTEXT_NULL;
    }
    if (stack_top < 16U ||
        (stack_top & (CONTEXT_STACK_ALIGNMENT - 1U)) != 0U ||
        !is_canonical(stack_top)) {
        return CONTEXT_BAD_STACK;
    }

    frame->r15 = 0U;
    frame->r14 = 0U;
    frame->r13 = 0U;
    frame->r12 = 0U;
    frame->rbx = 0U;
    frame->rbp = 0U;
    frame->rip = (uintptr_t)entry;
    frame->rsp = stack_top - 8U;
    return CONTEXT_OK;
}

bool context_frame_is_valid(const struct context_frame *frame) {
    if (frame == NULL) {
        return false;
    }
    if (frame->rsp == 0U || !is_canonical(frame->rsp)) {
        return false;
    }
    return (frame->rsp & (CONTEXT_FRAME_ALIGNMENT - 1U)) == 0U;
}

enum context_status context_switch(struct context_frame *current,
                                   struct context_frame *next)
{
    if (current == NULL || next == NULL) {
        return CONTEXT_NULL;
    }
    if (next->rsp == 0U || !is_canonical(next->rsp)) {
        return CONTEXT_BAD_STACK;
    }
    if ((next->rsp & (CONTEXT_FRAME_ALIGNMENT - 1U)) != 0U) {
        return CONTEXT_MISALIGNED;
    }

    context_switch_asm(current, next);
    return CONTEXT_OK;
}

const char *context_status_string(enum context_status status) {
    switch (status) {
    case CONTEXT_OK:
        return "CONTEXT_OK";
    case CONTEXT_NULL:
        return "CONTEXT_NULL";
    case CONTEXT_BAD_STACK:
        return "CONTEXT_BAD_STACK";
    case CONTEXT_MISALIGNED:
        return "CONTEXT_MISALIGNED";
    }
    return "CONTEXT_UNKNOWN";
}
