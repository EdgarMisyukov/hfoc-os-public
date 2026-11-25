#ifndef HFOC_USER_INIT_H
#define HFOC_USER_INIT_H

#include "vmm.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define USER_INIT_IMAGE_MAX_SIZE   4096U
#define USER_INIT_STACK_PAGE_COUNT 1U
#define USER_INIT_STACK_TOP \
    (VMM_USER_STACK_BASE + (USER_INIT_STACK_PAGE_COUNT * 4096U))

#define USER_INIT_MAGIC UINT32_C(0x1E571E57)

#define USER_INIT_CODE_FLAGS \
    (VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER)
#define USER_INIT_STACK_FLAGS \
    (USER_INIT_CODE_FLAGS | VMM_FLAG_NX)

enum user_init_status {
    USER_INIT_OK = 0,
    USER_INIT_NULL = 1,
    USER_INIT_IMAGE_TOO_LARGE = 2,
};

size_t user_init_image_size(void);
bool user_init_image_copy(uint8_t *dest, size_t capacity);
const char *user_init_status_string(enum user_init_status status);

#ifdef HFOC_USER_INIT_QEMU_TEST
enum user_init_status user_init_qemu_test_init(uint64_t hhdm_offset);
#endif

#endif
