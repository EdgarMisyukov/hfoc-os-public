#include "syscall.h"
#include "user.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SYSCALL_HOST_TEST

enum host_msr_index {
    HOST_MSR_EFER = 0,
    HOST_MSR_STAR = 1,
    HOST_MSR_LSTAR = 2,
    HOST_MSR_FMASK = 3,
    HOST_MSR_COUNT = 4,
};

static uint64_t host_msr_values[HOST_MSR_COUNT];
static uint32_t last_wrmsr_msr;
static uint64_t last_wrmsr_value;
static uint32_t force_prepare_mismatch_msr;
static bool force_prepare_mismatch_active;
static uint32_t prepare_mismatch_write_count;
static uint32_t force_restore_mismatch_msr;
static bool force_restore_mismatch_active;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static int host_msr_index(uint32_t msr) {
    switch (msr) {
    case IA32_EFER_MSR:
        return HOST_MSR_EFER;
    case IA32_STAR_MSR:
        return HOST_MSR_STAR;
    case IA32_LSTAR_MSR:
        return HOST_MSR_LSTAR;
    case IA32_FMASK_MSR:
        return HOST_MSR_FMASK;
    default:
        return -1;
    }
}

uint64_t host_rdmsr(uint32_t msr) {
    const int index = host_msr_index(msr);
    uint64_t value;

    if (index < 0) {
        return 0U;
    }
    value = host_msr_values[index];
    if (force_prepare_mismatch_active &&
        force_prepare_mismatch_msr == msr &&
        prepare_mismatch_write_count > 0U) {
        return value ^ IA32_EFER_SCE;
    }
    if (force_restore_mismatch_active &&
        force_restore_mismatch_msr == msr) {
        return value ^ UINT64_C(1);
    }
    return value;
}

void host_wrmsr(uint32_t msr, uint64_t value) {
    const int index = host_msr_index(msr);

    last_wrmsr_msr = msr;
    last_wrmsr_value = value;
    if (index >= 0) {
        host_msr_values[index] = value;
    }
    if (force_prepare_mismatch_msr == msr) {
        ++prepare_mismatch_write_count;
        if (prepare_mismatch_write_count >= 2U) {
            force_prepare_mismatch_active = false;
        }
    }
    if (force_restore_mismatch_msr == msr) {
        force_restore_mismatch_active = true;
    }
}

static void host_msr_reset(void) {
    memset(host_msr_values, 0, sizeof(host_msr_values));
    last_wrmsr_msr = 0U;
    last_wrmsr_value = 0U;
    force_prepare_mismatch_msr = 0U;
    force_prepare_mismatch_active = false;
    prepare_mismatch_write_count = 0U;
    force_restore_mismatch_msr = 0U;
    force_restore_mismatch_active = false;
}

static void test_selectors_and_star(void) {
    CHECK(USER_CS_SELECTOR == 0x23U,
          "user code selector is 0x23");
    CHECK(USER_SS_SELECTOR == 0x1BU,
          "user data selector is 0x1B");
    CHECK(SYSCALL_STAR_SYSCALL == KERNEL_CS_SELECTOR,
          "syscall STAR field equals kernel CS");
    CHECK((SYSCALL_STAR_SYSCALL + 8U) == KERNEL_SS_SELECTOR,
          "syscall STAR low derives kernel SS");
    CHECK((SYSCALL_STAR_SYSRET + 16U) == USER_CODE_SELECTOR,
          "sysret STAR field derives user CS");
    CHECK((SYSCALL_STAR_SYSRET + 8U) == USER_DATA_SELECTOR,
          "sysret STAR field derives user SS");
    CHECK(SYSCALL_STAR_VALUE ==
              ((UINT64_C(0x10) << 48U) | (UINT64_C(0x08) << 32U)),
          "exact STAR value is 0x0010000800000000");
    CHECK(SYSCALL_FMASK == (UINT64_C(1) << 9U),
          "FMASK clears IF");
}

static void test_syscall_numbers(void) {
    CHECK(SYSCALL_WRITE == 0U, "write syscall number is 0");
    CHECK(SYSCALL_YIELD == 1U, "yield syscall number is 1");
    CHECK(SYSCALL_EXIT == 2U, "exit syscall number is 2");
    CHECK(SYSCALL_COUNT == 26U, "syscall count is 26");
    CHECK(SYSCALL_READ == 3U, "read syscall number is 3");
    CHECK(SYSCALL_OPEN == 4U, "open syscall number is 4");
    CHECK(SYSCALL_RENAME == 13U, "rename syscall number is 13");
    CHECK(SYSCALL_FILE_WRITE == 14U, "file write syscall number is 14");
    CHECK(SYSCALL_EXEC == 15U, "exec syscall number is 15");
    CHECK(SYSCALL_RESULT == 16U, "result syscall number is 16");
    CHECK(SYSCALL_PIPE == 17U, "pipe syscall number is 17");
    CHECK(SYSCALL_DUP2 == 18U, "dup2 syscall number is 18");
    CHECK(SYSCALL_SPAWN == 19U, "spawn syscall number is 19");
    CHECK(SYSCALL_WAIT == 20U, "wait syscall number is 20");
}

static void test_dispatch(void) {
    char buffer[8] = "abc";

    CHECK(syscall_handle(SYSCALL_WRITE, 1U,
                         (uint64_t)(uintptr_t)buffer, 3U) == 3U,
          "write with fd 1 returns validated length");
    CHECK(syscall_handle(SYSCALL_WRITE, 0U,
                         (uint64_t)(uintptr_t)buffer, 3U) ==
              SYSCALL_RET_EINVAL,
          "write rejects non-stdout fd");
    CHECK(syscall_handle(SYSCALL_WRITE, 1U,
                         (uint64_t)(uintptr_t)buffer, 0U) ==
              SYSCALL_RET_EINVAL,
          "write rejects zero length");
    CHECK(syscall_handle(SYSCALL_WRITE, 1U,
                         (uint64_t)(uintptr_t)buffer, 65U) ==
              SYSCALL_RET_EINVAL,
          "write rejects oversized length");
    CHECK(syscall_handle(SYSCALL_WRITE, 1U, 0U, 1U) ==
              SYSCALL_RET_EFAULT,
          "write rejects null user range");
    CHECK(syscall_handle(SYSCALL_WRITE, 1U,
                         (uint64_t)VMM_USER_STACK_TOP - 3U, 4U) ==
              SYSCALL_RET_EFAULT,
          "write rejects user range overflow");
    CHECK(syscall_handle(SYSCALL_YIELD, 0U, 0U, 0U) ==
              SYSCALL_RET_OK,
          "yield with no args succeeds");
    CHECK(syscall_handle(SYSCALL_YIELD, 1U, 0U, 0U) ==
              SYSCALL_RET_EINVAL,
          "yield rejects args");
    CHECK(syscall_handle(SYSCALL_EXIT, 0U, 0U, 0U) ==
              SYSCALL_RET_OK,
          "exit with no args succeeds");
    CHECK(syscall_handle(SYSCALL_EXIT, 0U, 1U, 0U) ==
              SYSCALL_RET_EINVAL,
          "exit rejects args");
    CHECK(syscall_handle(99U, 0U, 0U, 0U) ==
              SYSCALL_RET_ENOSYS,
          "unknown syscall returns ENOSYS");
}

static void test_msr_prepare_success(void) {
    host_msr_reset();
    CHECK(syscall_msr_prepare() == SYSCALL_ABI_OK,
          "MSR prepare succeeds");
    CHECK(syscall_msr_published(), "MSR prepare publishes ABI");
    CHECK((host_msr_values[HOST_MSR_EFER] & IA32_EFER_SCE) != 0U,
          "EFER SCE is set");
    CHECK(host_msr_values[HOST_MSR_STAR] == SYSCALL_STAR_VALUE,
          "STAR readback is exact");
    CHECK(host_msr_values[HOST_MSR_LSTAR] ==
              (uint64_t)(uintptr_t)&syscall_entry,
          "LSTAR readback points at syscall entry");
    CHECK(host_msr_values[HOST_MSR_FMASK] == SYSCALL_FMASK,
          "FMASK readback is exact");
    CHECK(syscall_msr_verify() == SYSCALL_ABI_OK,
          "MSR verify succeeds after prepare");
    CHECK(syscall_msr_restore() == SYSCALL_ABI_OK,
          "MSR restore succeeds");
    CHECK(!syscall_msr_published(), "MSR restore clears publication");
    CHECK(host_msr_values[HOST_MSR_EFER] == 0U,
          "MSR restore returns previous EFER");
    CHECK(host_msr_values[HOST_MSR_STAR] == 0U,
          "MSR restore returns previous STAR");
    CHECK(host_msr_values[HOST_MSR_LSTAR] == 0U,
          "MSR restore returns previous LSTAR");
    CHECK(host_msr_values[HOST_MSR_FMASK] == 0U,
          "MSR restore returns previous FMASK");
}

static void test_msr_prepare_mismatch(uint32_t mismatch_msr,
                                      const char *label) {
    host_msr_reset();
    force_prepare_mismatch_msr = mismatch_msr;
    force_prepare_mismatch_active = true;
    CHECK(syscall_msr_prepare() == SYSCALL_ABI_READBACK_MISMATCH,
          label);
    CHECK(!syscall_msr_published(), "mismatch leaves ABI unpublished");
    CHECK(host_msr_values[HOST_MSR_EFER] == 0U &&
              host_msr_values[HOST_MSR_STAR] == 0U &&
              host_msr_values[HOST_MSR_LSTAR] == 0U &&
              host_msr_values[HOST_MSR_FMASK] == 0U,
          "mismatch restores previous MSRs");
}

static void test_msr_mismatches(void) {
    test_msr_prepare_mismatch(IA32_EFER_MSR,
                              "EFER readback mismatch fails and rolls back");
    test_msr_prepare_mismatch(IA32_STAR_MSR,
                              "STAR readback mismatch fails and rolls back");
    test_msr_prepare_mismatch(IA32_LSTAR_MSR,
                              "LSTAR readback mismatch fails and rolls back");
    test_msr_prepare_mismatch(IA32_FMASK_MSR,
                              "FMASK readback mismatch fails and rolls back");
}

static void test_msr_restore_failure(void) {
    host_msr_reset();
    CHECK(syscall_msr_prepare() == SYSCALL_ABI_OK,
          "restore failure fixture prepares");
    force_restore_mismatch_msr = IA32_STAR_MSR;
    force_restore_mismatch_active = true;
    CHECK(syscall_msr_restore() == SYSCALL_ABI_RESTORE_FAILED,
          "restore readback mismatch reports failure");
    CHECK(!syscall_msr_published(), "failed restore leaves ABI unpublished");
}

int main(void) {
    test_selectors_and_star();
    test_syscall_numbers();
    test_dispatch();
    test_msr_prepare_success();
    test_msr_mismatches();
    test_msr_restore_failure();

    if (failures != 0) {
        fprintf(stderr, "Syscall ABI host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Syscall ABI host tests: ALL OK");
    return 0;
}

#endif
