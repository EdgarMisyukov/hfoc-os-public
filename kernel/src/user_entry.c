#include "user.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

_Static_assert(sizeof(struct user_iret_frame) == 40U,
               "user iret frame must be 40 bytes");
_Static_assert(offsetof(struct user_iret_frame, rip) == 0U,
               "user iret frame rip must be first");
_Static_assert(offsetof(struct user_iret_frame, cs) == 8U,
               "user iret frame cs must be at offset 8");
_Static_assert(offsetof(struct user_iret_frame, rflags) == 16U,
               "user iret frame rflags must be at offset 16");
_Static_assert(offsetof(struct user_iret_frame, rsp) == 24U,
               "user iret frame rsp must be at offset 24");
_Static_assert(offsetof(struct user_iret_frame, ss) == 32U,
               "user iret frame ss must be at offset 32");

static bool is_canonical(uintptr_t address) {
    const uintptr_t upper = address >> 47U;
    return upper == 0U || upper == UINT64_C(0x1ffff);
}

static bool selector_supported(uint16_t selector) {
    switch (selector) {
    case USER_CS_SELECTOR:
    case USER_SS_SELECTOR:
    case KERNEL_CS_SELECTOR:
    case KERNEL_SS_SELECTOR:
        return true;
    default:
        return false;
    }
}

enum user_entry_status user_iret_frame_init(
    struct user_iret_frame *frame,
    uintptr_t rip,
    uintptr_t rsp,
    uint64_t rflags,
    uint16_t cs,
    uint16_t ss)
{
    if (frame == NULL) {
        return USER_ENTRY_NULL;
    }
    if (rip == 0U || !is_canonical(rip)) {
        return USER_ENTRY_BAD_RIP;
    }
    if (rsp == 0U || !is_canonical(rsp) || (rsp & 7U) != 0U) {
        return USER_ENTRY_BAD_RSP;
    }
    if (!selector_supported(cs) || !selector_supported(ss)) {
        return USER_ENTRY_BAD_SELECTOR;
    }

    frame->rip = (uint64_t)rip;
    frame->cs = cs;
    frame->rflags = rflags;
    frame->rsp = (uint64_t)rsp;
    frame->ss = ss;
    return USER_ENTRY_OK;
}

bool user_iret_frame_valid(const struct user_iret_frame *frame) {
    if (frame == NULL) {
        return false;
    }
    if (frame->rip == 0U ||
        !is_canonical((uintptr_t)frame->rip)) {
        return false;
    }
    if (frame->rsp == 0U ||
        !is_canonical((uintptr_t)frame->rsp) ||
        (frame->rsp & 7U) != 0U) {
        return false;
    }
    return selector_supported((uint16_t)frame->cs) &&
           selector_supported((uint16_t)frame->ss);
}

const char *user_entry_status_string(enum user_entry_status status) {
    switch (status) {
    case USER_ENTRY_OK:
        return "USER_ENTRY_OK";
    case USER_ENTRY_NULL:
        return "USER_ENTRY_NULL";
    case USER_ENTRY_BAD_RIP:
        return "USER_ENTRY_BAD_RIP";
    case USER_ENTRY_BAD_RSP:
        return "USER_ENTRY_BAD_RSP";
    case USER_ENTRY_BAD_SELECTOR:
        return "USER_ENTRY_BAD_SELECTOR";
    default:
        return "USER_ENTRY_UNKNOWN";
    }
}
