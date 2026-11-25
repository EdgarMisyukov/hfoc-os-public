#include "user.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef HFOC_USER_IRETQ_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_selectors_and_layout(void) {
    CHECK(USER_CS_SELECTOR == 0x23U,
          "user code selector is 0x23");
    CHECK(USER_SS_SELECTOR == 0x1BU,
          "user data selector is 0x1B");
    CHECK(KERNEL_CS_SELECTOR == 0x08U,
          "kernel code selector is 0x08");
    CHECK(KERNEL_SS_SELECTOR == 0x10U,
          "kernel data selector is 0x10");
    CHECK(offsetof(struct user_iret_frame, rip) == 0U,
          "iret frame rip is at offset 0");
    CHECK(offsetof(struct user_iret_frame, cs) == 8U,
          "iret frame cs is at offset 8");
    CHECK(offsetof(struct user_iret_frame, rflags) == 16U,
          "iret frame rflags is at offset 16");
    CHECK(offsetof(struct user_iret_frame, rsp) == 24U,
          "iret frame rsp is at offset 24");
    CHECK(offsetof(struct user_iret_frame, ss) == 32U,
          "iret frame ss is at offset 32");
    CHECK(sizeof(struct user_iret_frame) == 40U,
          "iret frame is 40 bytes");
}

static void test_init_validation(void) {
    struct user_iret_frame frame;

    CHECK(user_iret_frame_init(NULL, VMM_USER_IMAGE_BASE,
                               VMM_USER_STACK_TOP - 16U, 0x2U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_NULL,
          "null frame returns USER_ENTRY_NULL");
    CHECK(user_iret_frame_init(&frame, 0U, VMM_USER_STACK_TOP - 16U, 0x2U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_BAD_RIP,
          "zero rip returns USER_ENTRY_BAD_RIP");
    CHECK(user_iret_frame_init(&frame, UINT64_C(0x0000800000001000),
                               VMM_USER_STACK_TOP - 16U, 0x2U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_BAD_RIP,
          "noncanonical rip returns USER_ENTRY_BAD_RIP");
    CHECK(user_iret_frame_init(&frame, VMM_USER_IMAGE_BASE, 0U, 0x2U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_BAD_RSP,
          "zero rsp returns USER_ENTRY_BAD_RSP");
    CHECK(user_iret_frame_init(&frame, VMM_USER_IMAGE_BASE,
                               UINT64_C(0x0000800000001000), 0x2U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_BAD_RSP,
          "noncanonical rsp returns USER_ENTRY_BAD_RSP");
    CHECK(user_iret_frame_init(&frame, VMM_USER_IMAGE_BASE,
                               VMM_USER_STACK_TOP - 16U, 0x2U,
                               0x33U, USER_SS_SELECTOR) ==
              USER_ENTRY_BAD_SELECTOR,
          "unknown code selector returns USER_ENTRY_BAD_SELECTOR");
    CHECK(user_iret_frame_init(&frame, VMM_USER_IMAGE_BASE,
                               VMM_USER_STACK_TOP - 16U, 0x2U,
                               USER_CS_SELECTOR, 0x3BU) ==
              USER_ENTRY_BAD_SELECTOR,
          "unknown stack selector returns USER_ENTRY_BAD_SELECTOR");
}

static void test_valid_frames(void) {
    struct user_iret_frame user_frame;
    struct user_iret_frame kernel_frame;

    CHECK(user_iret_frame_init(&user_frame, VMM_USER_IMAGE_BASE,
                               VMM_USER_STACK_TOP - 16U, 0x202U,
                               USER_CS_SELECTOR, USER_SS_SELECTOR) ==
              USER_ENTRY_OK,
          "user frame initializes");
    CHECK(user_frame.rip == VMM_USER_IMAGE_BASE,
          "user frame stores rip");
    CHECK(user_frame.cs == USER_CS_SELECTOR,
          "user frame stores user code selector");
    CHECK(user_frame.rflags == 0x202U,
          "user frame stores rflags");
    CHECK(user_frame.rsp == VMM_USER_STACK_TOP - 16U,
          "user frame stores rsp");
    CHECK(user_frame.ss == USER_SS_SELECTOR,
          "user frame stores user stack selector");
    CHECK(user_iret_frame_valid(&user_frame),
          "valid user frame is accepted");

    CHECK(user_iret_frame_init(&kernel_frame,
                               UINT64_C(0xFFFFFFFF80001000),
                               UINT64_C(0xFFFFFFFF80100000),
                               0x202U,
                               KERNEL_CS_SELECTOR, KERNEL_SS_SELECTOR) ==
              USER_ENTRY_OK,
          "kernel return frame initializes");
    CHECK(user_iret_frame_valid(&kernel_frame),
          "valid kernel return frame is accepted");

    struct user_iret_frame invalid = user_frame;
    invalid.rip = UINT64_C(0x0000800000001000);
    CHECK(!user_iret_frame_valid(&invalid),
          "noncanonical rip frame is rejected");
    invalid = user_frame;
    invalid.rsp = 0U;
    CHECK(!user_iret_frame_valid(&invalid),
          "zero rsp frame is rejected");
    invalid = user_frame;
    invalid.cs = 0U;
    CHECK(!user_iret_frame_valid(&invalid),
          "null code selector frame is rejected");
    invalid = user_frame;
    invalid.ss = 0U;
    CHECK(!user_iret_frame_valid(&invalid),
          "null stack selector frame is rejected");
    CHECK(!user_iret_frame_valid(NULL),
          "null frame is rejected");
}

static void test_status_strings(void) {
    for (int status = USER_ENTRY_OK; status <= USER_ENTRY_BAD_SELECTOR;
         ++status) {
        const char *text =
            user_entry_status_string((enum user_entry_status)status);
        CHECK(text != NULL && text[0] != '\0',
              "status string is non-empty");
    }
}

int main(void) {
    test_selectors_and_layout();
    test_init_validation();
    test_valid_frames();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "User iretq entry host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("User iretq entry host tests: ALL OK");
    return 0;
}

#endif
