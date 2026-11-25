#ifndef HFOC_USER_H
#define HFOC_USER_H

#include "vmm.h"
#include <stdbool.h>
#include <stdint.h>

#define USER_CODE_SELECTOR 0x20U
#define USER_DATA_SELECTOR 0x18U
#define USER_CS_SELECTOR (USER_CODE_SELECTOR | 3U)
#define USER_SS_SELECTOR (USER_DATA_SELECTOR | 3U)
#define KERNEL_CS_SELECTOR 0x08U
#define KERNEL_SS_SELECTOR 0x10U

enum user_entry_status {
    USER_ENTRY_OK = 0,
    USER_ENTRY_NULL = 1,
    USER_ENTRY_BAD_RIP = 2,
    USER_ENTRY_BAD_RSP = 3,
    USER_ENTRY_BAD_SELECTOR = 4,
};

struct user_iret_frame {
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
};

enum user_entry_status user_iret_frame_init(
    struct user_iret_frame *frame,
    uintptr_t rip,
    uintptr_t rsp,
    uint64_t rflags,
    uint16_t cs,
    uint16_t ss);
bool user_iret_frame_valid(const struct user_iret_frame *frame);
__attribute__((noreturn)) void user_enter_iretq(
    const struct user_iret_frame *frame);
const char *user_entry_status_string(enum user_entry_status status);

#ifdef HFOC_USER_IRETQ_QEMU_TEST
enum user_entry_status user_iretq_qemu_test_init(uint64_t hhdm_offset);
__attribute__((noreturn)) void user_iretq_qemu_trap_complete(void);
#endif

#endif
