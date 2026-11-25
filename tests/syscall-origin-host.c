#include "process.h"
#include "syscall.h"
#include "user.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HFOC_PROCESS_HOST_TEST) && defined(HFOC_SYSCALL_ORIGIN_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* Process creation is not exercised here, but process.c requires these
   ownership-entry symbols to link. */
void *kmalloc(size_t size) {
    (void)size;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    return false;
}

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    (void)count;
    return 0U;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    (void)physical_address;
    (void)count;
    return false;
}

enum vmm_status vmm_create_address_space(struct address_space *out,
                                         uint64_t hhdm_offset)
{
    (void)out;
    (void)hhdm_offset;
    return VMM_BAD_ADDRESS_SPACE;
}

enum vmm_status vmm_destroy_address_space(struct address_space *as,
                                          uint64_t hhdm_offset)
{
    (void)as;
    (void)hhdm_offset;
    return VMM_BAD_ADDRESS_SPACE;
}

enum context_status context_frame_init(struct context_frame *frame,
                                       uintptr_t stack_top,
                                       void (*entry)(void))
{
    (void)frame;
    (void)stack_top;
    (void)entry;
    return CONTEXT_NULL;
}

static struct process_control_block root;
static struct process_control_block user_pcb;
static struct process_control_block invalid_pcb;
static struct process_control_block stale_pcb;
static struct kernel_process stale_process;

static void reset_all(void) {
    process_host_test_reset();
    memset(&root, 0, sizeof(root));
    memset(&user_pcb, 0, sizeof(user_pcb));
    memset(&invalid_pcb, 0, sizeof(invalid_pcb));
    memset(&stale_pcb, 0, sizeof(stale_pcb));
    memset(&stale_process, 0, sizeof(stale_process));
}

static bool pcb_snapshot_unchanged(const struct process_control_block *pcb,
                                   uint32_t pid,
                                   bool published,
                                   enum process_security_domain domain,
                                   uint32_t generation,
                                   uint32_t child_count,
                                   const struct process_control_block *parent)
{
    return pcb->pid == pid &&
           pcb->published == published &&
           pcb->security_domain == domain &&
           pcb->security_generation == generation &&
           pcb->child_count == child_count &&
           pcb->parent == parent;
}

static void test_domain_publication_and_immutability(void) {
    uint32_t root_pid = 0U;
    uint32_t user_pid = 0U;
    uint32_t repeated_pid = 0U;
    uint32_t invalid_pid = 0U;
    uint32_t reused = 0U;

    reset_all();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              root.pid == 1U &&
              root.security_domain == PROCESS_SECURITY_DOMAIN_KERNEL &&
              root.security_generation == 0U,
          "default PCB publishes kernel security domain");
    CHECK(process_pcb_init_domain(&user_pcb, &root, &user_pid,
                                  PROCESS_SECURITY_DOMAIN_USER) ==
              PROCESS_OK &&
              user_pcb.pid == 2U &&
              user_pcb.security_domain == PROCESS_SECURITY_DOMAIN_USER &&
              user_pcb.security_generation == 0U,
          "domain-aware PCB publishes exact user security domain");
    CHECK(process_pcb_security_domain(&user_pcb) ==
              PROCESS_SECURITY_DOMAIN_USER,
          "domain accessor returns user security domain");
    CHECK(process_pcb_security_generation(&user_pcb) == 0U,
          "generation accessor returns initial generation");

    CHECK(process_pcb_init_domain(&user_pcb, &root, &repeated_pid,
                                  PROCESS_SECURITY_DOMAIN_KERNEL) ==
              PROCESS_BAD_STATE &&
              user_pcb.security_domain == PROCESS_SECURITY_DOMAIN_USER &&
              user_pcb.security_generation == 0U &&
              root.child_count == 1U,
          "published security domain cannot be reinitialized");

    CHECK(process_pcb_init_domain(&invalid_pcb, &root, &invalid_pid,
                                  PROCESS_SECURITY_DOMAIN_COUNT) ==
              PROCESS_BAD_DOMAIN &&
              !invalid_pcb.published && invalid_pcb.pid == 0U,
          "invalid security domain leaves PCB unpublished");
    CHECK(process_pid_alloc(&reused) == PROCESS_OK && reused == 3U,
          "invalid domain does not consume a PID");
    CHECK(process_status_string(PROCESS_BAD_DOMAIN) != NULL,
          "process status string covers BAD_DOMAIN");

    CHECK(process_pcb_destroy(&user_pcb) == PROCESS_OK &&
              process_pcb_destroy(&root) == PROCESS_OK,
          "domain fixture destroys published PCBs");
}

static void test_origin_capture_and_valid_dispatch(void) {
    struct syscall_origin origin;
    struct capability write_capability = {
        CAPABILITY_TYPE_SYSCALL_WRITE, 0U, 0x1000U
    };
    uint32_t root_pid = 0U;
    uint32_t user_pid = 0U;
    uint64_t handle = 0U;
    char buffer[8] = "abc";

    reset_all();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              process_pcb_init_domain(&user_pcb, &root, &user_pid,
                  PROCESS_SECURITY_DOMAIN_USER) ==
                  PROCESS_OK,
          "valid origin fixture publishes user PCB");
    CHECK(capability_table_add(&user_pcb.capability_table,
                               &write_capability, &handle) ==
              CAPABILITY_OK,
          "valid origin fixture publishes write capability");

    CHECK(syscall_origin_capture(&user_pcb, &origin) == SYSCALL_ORIGIN_OK &&
              origin.pcb == &user_pcb &&
              origin.domain == PROCESS_SECURITY_DOMAIN_USER &&
              origin.generation == 0U,
          "origin capture records exact user PCB identity");
    CHECK(syscall_origin_validate(&origin) == SYSCALL_ORIGIN_OK,
          "captured origin validates");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)buffer, 3U) == 3U,
          "valid checked origin dispatches write");
    CHECK(pcb_snapshot_unchanged(&user_pcb, user_pid, true,
                                 PROCESS_SECURITY_DOMAIN_USER, 0U, 0U,
                                 &root),
          "valid dispatch leaves user PCB state unchanged");
    CHECK(capability_table_open_count(&user_pcb.capability_table) == 1U &&
              capability_table_has(&user_pcb.capability_table,
                                   CAPABILITY_TYPE_SYSCALL_WRITE),
          "valid dispatch leaves capability table state unchanged");

    CHECK(syscall_origin_capture(NULL, &origin) == SYSCALL_ORIGIN_NULL,
          "origin capture rejects null PCB");
    CHECK(syscall_origin_capture(&user_pcb, NULL) == SYSCALL_ORIGIN_NULL,
          "origin capture rejects null output");
    CHECK(syscall_origin_validate(NULL) == SYSCALL_ORIGIN_NULL,
          "origin validate rejects null origin");
    origin.pcb = NULL;
    CHECK(syscall_origin_validate(&origin) == SYSCALL_ORIGIN_BAD_PCB,
          "origin validate rejects null PCB");
    CHECK(syscall_origin_validate(&origin) != SYSCALL_ORIGIN_OK &&
              syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                     (uint64_t)(uintptr_t)buffer, 3U) ==
                  SYSCALL_RET_EPERM,
          "null PCB origin is rejected before dispatch");
    memset(&invalid_pcb, 0, sizeof(invalid_pcb));
    origin.pcb = &invalid_pcb;
    origin.domain = PROCESS_SECURITY_DOMAIN_USER;
    origin.generation = 0U;
    CHECK(syscall_origin_validate(&origin) == SYSCALL_ORIGIN_BAD_PCB,
          "unpublished PCB origin is rejected");
    CHECK(syscall_origin_status_string(SYSCALL_ORIGIN_DOMAIN_MISMATCH) !=
              NULL,
          "origin status string covers all statuses");

    CHECK(process_pcb_destroy(&user_pcb) == PROCESS_OK &&
              process_pcb_destroy(&root) == PROCESS_OK,
          "valid origin fixture destroys PCBs");
}

static void test_cross_domain_rejection_no_state_change(void) {
    struct syscall_origin origin;
    uint32_t root_pid = 0U;
    uint32_t user_pid = 0U;
    char buffer[8] = "abc";

    reset_all();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              process_pcb_init_domain(&user_pcb, &root, &user_pid,
                                      PROCESS_SECURITY_DOMAIN_USER) ==
                  PROCESS_OK,
          "cross-domain fixture publishes user PCB");
    CHECK(syscall_origin_capture(&user_pcb, &origin) == SYSCALL_ORIGIN_OK,
          "cross-domain fixture captures user origin");

    origin.domain = PROCESS_SECURITY_DOMAIN_KERNEL;
    CHECK(syscall_origin_validate(&origin) ==
              SYSCALL_ORIGIN_DOMAIN_MISMATCH,
          "cross-domain origin is rejected");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U,
                                 (uint64_t)(uintptr_t)buffer, 3U) ==
              SYSCALL_RET_EPERM,
          "cross-domain checked syscall returns EPERM");
    CHECK(pcb_snapshot_unchanged(&user_pcb, user_pid, true,
                                 PROCESS_SECURITY_DOMAIN_USER, 0U, 0U,
                                 &root) &&
              root.child_count == 1U && root.children_head == &user_pcb,
          "cross-domain rejection leaves process graph unchanged");

    CHECK(process_pcb_destroy(&user_pcb) == PROCESS_OK &&
              process_pcb_destroy(&root) == PROCESS_OK,
          "cross-domain fixture destroys PCBs");
}

static void test_stale_origin_rejection_no_state_change(void) {
    struct syscall_origin origin;
    uint32_t root_pid = 0U;
    uint32_t stale_pid = 0U;
    uint32_t reused = 0U;

    reset_all();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              process_pcb_init_domain(&stale_process.pcb, &root, &stale_pid,
                                      PROCESS_SECURITY_DOMAIN_USER) ==
                  PROCESS_OK,
          "stale fixture publishes user PCB");
    CHECK(syscall_origin_capture(&stale_process.pcb, &origin) ==
              SYSCALL_ORIGIN_OK,
          "stale fixture captures pre-exit origin");
    CHECK(process_exit_kernel(&stale_process, 7U) == PROCESS_OK,
          "stale fixture exits user process");
    CHECK(stale_process.pcb.security_generation == 1U &&
              process_pcb_security_generation(&stale_process.pcb) == 1U,
          "process exit invalidates previous generation");
    CHECK(syscall_origin_validate(&origin) == SYSCALL_ORIGIN_STALE,
          "pre-exit origin is stale after exit");
    CHECK(syscall_handle_checked(&origin, SYSCALL_WRITE, 1U, 1U, 1U) ==
              SYSCALL_RET_EPERM,
          "stale checked syscall returns EPERM");
    CHECK(stale_process.pcb.pid == stale_pid &&
              stale_process.pcb.published &&
              stale_process.pcb.security_domain ==
                  PROCESS_SECURITY_DOMAIN_USER &&
              stale_process.pcb.security_generation == 1U &&
              root.child_count == 1U && root.children_head == &stale_process.pcb,
          "stale rejection leaves exited PCB state unchanged");

    CHECK(process_pcb_destroy(&stale_process.pcb) == PROCESS_OK &&
              root.child_count == 0U,
          "stale fixture destroys exited PCB");
    CHECK(process_pid_alloc(&reused) == PROCESS_OK && reused == stale_pid,
          "stale fixture releases PID after destroy");
    CHECK(process_pcb_destroy(&root) == PROCESS_OK,
          "stale fixture destroys root PCB");
}

int main(void) {
    test_domain_publication_and_immutability();
    test_origin_capture_and_valid_dispatch();
    test_cross_domain_rejection_no_state_change();
    test_stale_origin_rejection_no_state_change();

    if (failures != 0) {
        fprintf(stderr, "Syscall origin host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Syscall origin host tests: ALL OK");
    return 0;
}

#endif
