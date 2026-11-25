#include "capability.h"
#include "process.h"
#include "syscall.h"
#include "user.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HFOC_SYSCALL_HOST_TEST) && defined(HFOC_SYSCALL_GATE_HOST_TEST)

static int failures;
static struct process_control_block denied;
static struct process_control_block partial;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static char write_buffer[8] = "abc";

uint64_t host_rdmsr(uint32_t msr) {
    (void)msr;
    return 0U;
}

void host_wrmsr(uint32_t msr, uint64_t value) {
    (void)msr;
    (void)value;
}

static void reset_user_pcb(struct process_control_block *pcb) {
    memset(pcb, 0, sizeof(*pcb));
    pcb->published = true;
    pcb->security_domain = PROCESS_SECURITY_DOMAIN_USER;
    pcb->security_generation = 0U;
    (void)capability_table_init(&pcb->capability_table);
}

static bool pcb_state_unchanged(const struct process_control_block *pcb,
                                bool published,
                                enum process_security_domain domain,
                                uint32_t generation)
{
    return pcb->published == published &&
           pcb->security_domain == domain &&
           pcb->security_generation == generation;
}

static void test_denied_without_capability(void) {
    struct syscall_origin origin;

    reset_user_pcb(&denied);
    CHECK(syscall_origin_capture(&denied, &origin) == SYSCALL_ORIGIN_OK,
          "capability-free fixture captures a valid origin");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)write_buffer, 3U) ==
              SYSCALL_RET_EPERM,
          "write is denied without write capability");
    CHECK(syscall_handle_checked(&origin, SYSCALL_YIELD, 0U, 0U, 0U) ==
              SYSCALL_RET_EPERM,
          "yield is denied without yield capability");
    CHECK(syscall_handle_checked(&origin, SYSCALL_EXIT, 0U, 0U, 0U) ==
              SYSCALL_RET_EPERM,
          "exit is denied without exit capability");
    CHECK(capability_table_open_count(&denied.capability_table) == 0U &&
              !capability_table_has(&denied.capability_table,
                                    CAPABILITY_TYPE_SYSCALL_WRITE) &&
              !capability_table_has(&denied.capability_table,
                                    CAPABILITY_TYPE_SYSCALL_YIELD) &&
              !capability_table_has(&denied.capability_table,
                                    CAPABILITY_TYPE_SYSCALL_EXIT) &&
              pcb_state_unchanged(&denied, true,
                                  PROCESS_SECURITY_DOMAIN_USER, 0U),
          "denied calls leave PCB and capability table unchanged");
}

static void test_write_capability_allows_write_only(void) {
    struct syscall_origin origin;
    struct capability capability = {
        CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0x1000U
    };
    uint64_t handle = 0U;

    reset_user_pcb(&partial);
    CHECK(capability_table_add(&partial.capability_table, &capability,
                               &handle) == CAPABILITY_OK,
          "partial fixture publishes write capability");
    CHECK(syscall_origin_capture(&partial, &origin) == SYSCALL_ORIGIN_OK,
          "partial fixture captures a valid origin");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)write_buffer, 3U) ==
              3U,
          "write capability preserves write ABI result");
    CHECK(syscall_handle_checked(&origin, SYSCALL_YIELD, 0U, 0U, 0U) ==
              SYSCALL_RET_EPERM,
          "yield remains denied without yield capability");
    CHECK(syscall_handle_checked(&origin, SYSCALL_EXIT, 0U, 0U, 0U) ==
              SYSCALL_RET_EPERM,
          "exit remains denied without exit capability");
    CHECK(capability_table_open_count(&partial.capability_table) == 1U &&
              capability_table_has(&partial.capability_table,
                                   CAPABILITY_TYPE_SYSCALL_WRITE),
          "permitted and denied calls preserve published capability");
}

static void test_all_capabilities_retain_abi(void) {
    struct syscall_origin origin;
    struct capability write_cap = {
        CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0x1000U
    };
    struct capability yield_cap = {
        CAPABILITY_TYPE_SYSCALL_YIELD, 0U, 0x2000U
    };
    struct capability exit_cap = {
        CAPABILITY_TYPE_SYSCALL_EXIT, 0U, 0x3000U
    };
    uint64_t handle = 0U;

    reset_user_pcb(&partial);
    CHECK(capability_table_add(&partial.capability_table, &write_cap,
                               &handle) == CAPABILITY_OK &&
              capability_table_add(&partial.capability_table, &yield_cap,
                                   &handle) == CAPABILITY_OK &&
              capability_table_add(&partial.capability_table, &exit_cap,
                                   &handle) == CAPABILITY_OK,
          "full fixture publishes all baseline syscall capabilities");
    CHECK(capability_table_open_count(&partial.capability_table) == 3U,
          "full fixture publishes exactly three capabilities");
    CHECK(syscall_origin_capture(&partial, &origin) == SYSCALL_ORIGIN_OK,
          "full fixture captures a valid origin");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)write_buffer, 3U) ==
              3U &&
              syscall_handle_checked(&origin, SYSCALL_YIELD,
                                     0U, 0U, 0U) == SYSCALL_RET_OK &&
              syscall_handle_checked(&origin, SYSCALL_EXIT,
                                     0U, 0U, 0U) == SYSCALL_RET_OK,
          "all baseline capabilities retain existing ABI behavior");
}

static void test_revoked_capability_denies(void) {
    struct syscall_origin origin;
    struct capability capability = {
        CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0x1000U
    };
    uint64_t handle = 0U;

    reset_user_pcb(&denied);
    CHECK(capability_table_add(&denied.capability_table, &capability,
                               &handle) == CAPABILITY_OK,
          "revoke fixture publishes write capability");
    CHECK(syscall_origin_capture(&denied, &origin) == SYSCALL_ORIGIN_OK,
          "revoke fixture captures a valid origin");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)write_buffer, 3U) ==
              3U,
          "revoke fixture permits write before revoke");
    CHECK(capability_table_revoke(&denied.capability_table, handle) ==
              CAPABILITY_OK,
          "revoke removes write capability");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)write_buffer, 3U) ==
              SYSCALL_RET_EPERM,
          "write is denied after capability revoke");
    CHECK(capability_table_open_count(&denied.capability_table) == 0U &&
              !capability_table_has(&denied.capability_table,
                                    CAPABILITY_TYPE_SYSCALL_WRITE),
          "revoked capability is not present after denial");
}

static void test_unknown_syscall_does_not_require_capability(void) {
    struct syscall_origin origin;

    reset_user_pcb(&denied);
    CHECK(syscall_origin_capture(&denied, &origin) == SYSCALL_ORIGIN_OK,
          "unknown syscall fixture captures a valid origin");
    CHECK(syscall_handle_checked(&origin, 99U, 0U, 0U, 0U) ==
              SYSCALL_RET_ENOSYS,
          "unknown syscall returns ENOSYS without a capability");
}

int main(void) {
    test_denied_without_capability();
    test_write_capability_allows_write_only();
    test_all_capabilities_retain_abi();
    test_revoked_capability_denies();
    test_unknown_syscall_does_not_require_capability();

    if (failures != 0) {
        fprintf(stderr, "Syscall gate host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Syscall gate host tests: ALL OK");
    return 0;
}

#endif
