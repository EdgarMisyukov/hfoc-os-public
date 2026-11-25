#include "process.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PROCESS_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* PCB host harness does not exercise kernel-process creation, but the shared
   process.c object requires these ownership-entry symbols to link. */
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
static struct process_control_block child_a;
static struct process_control_block child_b;
static struct process_control_block orphan;
static struct process_control_block self_pcb;
static struct process_control_block failed;
static struct kernel_process capability_process;

static void process_ready(void) {
    process_host_test_reset();
    memset(&root, 0, sizeof(root));
    memset(&child_a, 0, sizeof(child_a));
    memset(&child_b, 0, sizeof(child_b));
    memset(&orphan, 0, sizeof(orphan));
    memset(&self_pcb, 0, sizeof(self_pcb));
    memset(&failed, 0, sizeof(failed));
    memset(&capability_process, 0, sizeof(capability_process));
}

static void test_pid_allocation_release_reuse(void) {
    uint32_t pid_a = 0U;
    uint32_t pid_b = 0U;
    uint32_t pid_c = 0U;
    uint32_t reused = 0U;

    process_ready();
    CHECK(process_pid_alloc(NULL) == PROCESS_NULL,
          "PID alloc with null output returns NULL");
    CHECK(process_pid_alloc(&pid_a) == PROCESS_OK && pid_a == 1U,
          "first PID allocation returns PID 1");
    CHECK(process_pid_alloc(&pid_b) == PROCESS_OK && pid_b == 2U,
          "second PID allocation returns PID 2");
    CHECK(process_pid_alloc(&pid_c) == PROCESS_OK && pid_c == 3U,
          "third PID allocation returns PID 3");

    CHECK(process_pid_free(0U) == PROCESS_BAD_PID,
          "PID 0 release returns BAD_PID");
    CHECK(process_pid_free(PROCESS_MAX_PIDS + 1U) == PROCESS_BAD_PID,
          "out-of-range PID release returns BAD_PID");
    CHECK(process_pid_free(10U) == PROCESS_BAD_PID,
          "unallocated PID release returns BAD_PID");

    CHECK(process_pid_free(pid_b) == PROCESS_OK,
          "allocated PID release succeeds");
    CHECK(process_pid_alloc(&reused) == PROCESS_OK && reused == pid_b,
          "released PID is immediately reused");
    CHECK(process_pid_free(pid_a) == PROCESS_OK &&
              process_pid_free(pid_c) == PROCESS_OK &&
              process_pid_free(reused) == PROCESS_OK,
          "remaining PIDs release successfully");
}

static void test_pid_exhaustion_and_recovery(void) {
    uint32_t pids[PROCESS_MAX_PIDS];
    uint32_t extra = 0U;
    uint32_t recovered = 0U;
    unsigned int index;

    process_ready();
    for (index = 0U; index < PROCESS_MAX_PIDS; ++index) {
        CHECK(process_pid_alloc(&pids[index]) == PROCESS_OK,
              "PID exhaustion allocation succeeds");
    }
    CHECK(process_pid_alloc(&extra) == PROCESS_NO_PIDS,
          "PID exhaustion returns NO_PIDS");

    CHECK(process_pid_free(pids[7U]) == PROCESS_OK,
          "exhausted PID release succeeds");
    CHECK(process_pid_alloc(&recovered) == PROCESS_OK &&
              recovered == pids[7U],
          "released exhausted PID is recovered");
    CHECK(process_pid_free(recovered) == PROCESS_OK,
          "recovered PID releases");
}

static void test_pcb_root_and_children(void) {
    uint32_t root_pid = 0U;
    uint32_t pid_a = 0U;
    uint32_t pid_b = 0U;

    process_ready();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              root.pid == 1U && root.published,
          "root PCB publishes with PID 1");
    CHECK(root.parent == NULL && root.children_head == NULL &&
              root.children_tail == NULL && root.child_count == 0U,
          "root PCB has no parent or children");

    CHECK(process_pcb_init(&child_a, &root, &pid_a) == PROCESS_OK &&
              child_a.published && child_a.pid == 2U,
          "first child PCB publishes under root");
    CHECK(process_pcb_init(&child_b, &root, &pid_b) == PROCESS_OK &&
              child_b.published && child_b.pid == 3U,
          "second child PCB publishes under root");

    CHECK(root.children_head == &child_a &&
              root.children_tail == &child_b &&
              root.child_count == 2U,
          "root child list links both children");
    CHECK(child_a.parent == &root && child_a.sibling_next == &child_b &&
              child_a.sibling_prev == NULL,
          "first child has exact parent and next sibling");
    CHECK(child_b.parent == &root && child_b.sibling_next == NULL &&
              child_b.sibling_prev == &child_a,
          "second child has exact parent and previous sibling");
    CHECK(process_pcb_child_count(&root) == 2U,
          "root child count accessor returns 2");
}

static void test_pcb_destroy_and_pid_reuse(void) {
    uint32_t root_pid = 0U;
    uint32_t pid_a = 0U;
    uint32_t pid_b = 0U;
    uint32_t reused = 0U;

    process_ready();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              process_pcb_init(&child_a, &root, &pid_a) == PROCESS_OK &&
              process_pcb_init(&child_b, &root, &pid_b) == PROCESS_OK,
          "destroy fixture publishes root and two children");

    CHECK(process_pcb_destroy(&root) == PROCESS_HAS_CHILDREN &&
              root.published && root.child_count == 2U,
          "parent with children cannot be destroyed");
    CHECK(process_pcb_destroy(&child_a) == PROCESS_OK,
          "child A destroys");
    CHECK(root.children_head == &child_b &&
              root.children_tail == &child_b &&
              root.child_count == 1U,
          "destroyed child is unlinked from parent");
    CHECK(child_b.sibling_prev == NULL && child_b.sibling_next == NULL,
          "remaining child has no stale siblings");
    CHECK(process_pid_alloc(&reused) == PROCESS_OK && reused == pid_a,
          "destroyed child PID is reused");

    CHECK(process_pcb_destroy(&child_b) == PROCESS_OK &&
              root.children_head == NULL &&
              root.children_tail == NULL && root.child_count == 0U,
          "last child destroys and leaves parent childless");
    CHECK(process_pcb_destroy(&root) == PROCESS_OK && !root.published,
          "childless parent destroys");
}

static void test_pcb_validation_and_publication_rollback(void) {
    uint32_t root_pid = 0U;
    uint32_t self_pid = 0U;
    uint32_t failed_pid = 0U;
    uint32_t reused = 0U;
    uint32_t ok_pid = 0U;

    process_ready();
    CHECK(process_pcb_init(NULL, NULL, &root_pid) == PROCESS_NULL,
          "null PCB init returns NULL");
    CHECK(process_pcb_init(&root, NULL, NULL) == PROCESS_NULL,
          "null PID output returns NULL");

    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK,
          "validation root publishes");
    CHECK(process_pcb_init(&child_a, &orphan, &ok_pid) == PROCESS_BAD_STATE,
          "non-published parent returns BAD_STATE");
    CHECK(process_pcb_init(&self_pcb, &self_pcb, &self_pid) ==
              PROCESS_BAD_STATE,
          "self-parenting returns BAD_STATE");
    CHECK(process_pcb_init(&child_a, &root, &ok_pid) == PROCESS_OK &&
              process_pcb_init(&child_a, &root, &ok_pid) == PROCESS_BAD_STATE,
          "double publication returns BAD_STATE");

    process_host_test_set_link_failure(true);
    CHECK(process_pcb_init(&failed, &root, &failed_pid) ==
              PROCESS_LINK_FAILED,
          "injected link failure returns LINK_FAILED");
    CHECK(!failed.published && failed.pid == 0U &&
              failed.parent == NULL && failed.sibling_next == NULL,
          "link failure publishes no PCB fields");
    CHECK(root.child_count == 1U && root.children_head == &child_a,
          "link failure leaves parent graph unchanged");
    CHECK(process_pid_alloc(&reused) == PROCESS_OK && reused == failed_pid,
          "link failure releases the allocated PID");
    process_host_test_set_link_failure(false);

    CHECK(process_pcb_init(&child_b, &root, &ok_pid) == PROCESS_OK &&
              root.child_count == 2U,
          "PCB publication succeeds after rollback");
    CHECK(process_status_string(PROCESS_LINK_FAILED) != NULL,
          "process status string covers all statuses");
}

static void test_capability_table_process_ownership(void) {
    struct capability capability = {5U, 0U, 0x50U};
    struct capability output;
    uint32_t root_pid = 0U;
    uint32_t child_pid = 0U;
    uint64_t handle = 0U;

    process_ready();
    CHECK(process_pcb_init(&root, NULL, &root_pid) == PROCESS_OK &&
              capability_table_is_published(&root.capability_table) &&
              capability_table_open_count(&root.capability_table) == 0U,
          "PCB init publishes an empty capability table");
    CHECK(process_pcb_init_domain(&capability_process.pcb, &root, &child_pid,
                                  PROCESS_SECURITY_DOMAIN_USER) ==
              PROCESS_OK &&
              capability_table_is_published(
                  &capability_process.pcb.capability_table),
          "user PCB publishes its capability table");
    CHECK(capability_table_add(&capability_process.pcb.capability_table,
                               &capability, &handle) == CAPABILITY_OK &&
              capability_table_open_count(
                  &capability_process.pcb.capability_table) == 1U,
          "process-owned capability table accepts a capability");
    CHECK(capability_table_get(&capability_process.pcb.capability_table,
                               handle, &output) == CAPABILITY_OK &&
              output.object == capability.object,
          "process-owned capability is readable before exit");

    CHECK(process_exit_kernel(&capability_process, 0U) == PROCESS_OK &&
              !capability_table_is_published(
                  &capability_process.pcb.capability_table),
          "process exit revokes the process capability table");
    CHECK(capability_table_get(&capability_process.pcb.capability_table,
                               handle, &output) == CAPABILITY_BAD_STATE,
          "exited process capability handle is invalid");
    CHECK(process_pcb_destroy(&capability_process.pcb) == PROCESS_OK &&
              process_pcb_destroy(&root) == PROCESS_OK,
          "exited capability process and root destroy cleanly");
}

int main(void) {
    test_pid_allocation_release_reuse();
    test_pid_exhaustion_and_recovery();
    test_pcb_root_and_children();
    test_pcb_destroy_and_pid_reuse();
    test_pcb_validation_and_publication_rollback();
    test_capability_table_process_ownership();

    if (failures != 0) {
        fprintf(stderr, "Process host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Process host tests: ALL OK");
    return 0;
}

#endif
