#include "process.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PROCESS_HOST_TEST

#define TEST_HHDM        UINT64_C(0xFFFF800000000000)
#define TEST_STACK_PHYS  UINT64_C(0x00100000)
#define TEST_AS_PHYS     UINT64_C(0x00200000)
#define PROCESS_POOL_CAP 8U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_entry_a(void) {
}

static void test_entry_b(void) {
}

/* Heap mock: a small fixed pool of process objects. */
static struct kernel_process process_pool[PROCESS_POOL_CAP];
static bool process_pool_used[PROCESS_POOL_CAP];
static uint32_t heap_allocs;
static uint32_t heap_frees;
static bool heap_fail;

void *kmalloc(size_t size) {
    uint32_t index;

    if (heap_fail || size != sizeof(struct kernel_process)) {
        return NULL;
    }
    for (index = 0U; index < PROCESS_POOL_CAP; ++index) {
        if (!process_pool_used[index]) {
            process_pool_used[index] = true;
            memset(&process_pool[index], 0, sizeof(process_pool[index]));
            ++heap_allocs;
            return &process_pool[index];
        }
    }
    return NULL;
}

bool kfree(void *ptr) {
    uint32_t index;

    for (index = 0U; index < PROCESS_POOL_CAP; ++index) {
        if (&process_pool[index] == ptr) {
            process_pool_used[index] = false;
            ++heap_frees;
            return true;
        }
    }
    return false;
}

/* PMM stack mock. */
static uint64_t next_stack_phys;
static uint32_t stack_allocs;
static uint32_t stack_frees;
static bool stack_fail;

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    uint64_t physical;

    if (stack_fail || count != PROCESS_KERNEL_STACK_PAGES) {
        return 0U;
    }
    physical = next_stack_phys;
    next_stack_phys += (uint64_t)count * 4096U;
    ++stack_allocs;
    return physical;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    if (physical_address == 0U || count == 0U) {
        return false;
    }
    ++stack_frees;
    return true;
}

/* VMM mock for process-owned address spaces. */
static uint32_t as_allocs;
static uint32_t as_frees;
static bool as_fail;

enum vmm_status vmm_create_address_space(struct address_space *out,
                                         uint64_t hhdm_offset)
{
    (void)hhdm_offset;
    if (out == NULL) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    if (as_fail) {
        return VMM_PMM_ALLOC_FAILED;
    }
    out->pml4_phys = TEST_AS_PHYS + (uint64_t)as_allocs * 4096U;
    ++as_allocs;
    return VMM_OK;
}

enum vmm_status vmm_destroy_address_space(struct address_space *as,
                                          uint64_t hhdm_offset)
{
    (void)hhdm_offset;
    if (as == NULL || as->pml4_phys == 0U) {
        return VMM_BAD_ADDRESS_SPACE;
    }
    as->pml4_phys = 0U;
    ++as_frees;
    return VMM_OK;
}

static struct process_control_block root;
static struct process_control_block invalid_parent;
static uint32_t root_pid;

static void reset_all(void) {
    process_host_test_reset();
    memset(&root, 0, sizeof(root));
    memset(&invalid_parent, 0, sizeof(invalid_parent));
    memset(process_pool, 0, sizeof(process_pool));
    memset(process_pool_used, 0, sizeof(process_pool_used));
    heap_allocs = 0U;
    heap_frees = 0U;
    heap_fail = false;
    next_stack_phys = TEST_STACK_PHYS;
    stack_allocs = 0U;
    stack_frees = 0U;
    stack_fail = false;
    as_allocs = 0U;
    as_frees = 0U;
    as_fail = false;
    root_pid = 0U;
}

static enum process_status publish_root(void) {
    return process_pcb_init(&root, NULL, &root_pid);
}

static void test_invalid_arguments(void) {
    struct kernel_process *process = NULL;
    struct kernel_process unowned;

    reset_all();
    CHECK(process_create_kernel(NULL, NULL, test_entry_a, TEST_HHDM) ==
              PROCESS_NULL,
          "null output pointer returns PROCESS_NULL");
    CHECK(process_create_kernel(&process, NULL, NULL, TEST_HHDM) ==
              PROCESS_NULL && process == NULL,
          "null entry returns PROCESS_NULL and no output");
    CHECK(process_destroy_kernel(NULL, TEST_HHDM) == PROCESS_NULL,
          "null destroy returns PROCESS_NULL");

    memset(&unowned, 0, sizeof(unowned));
    CHECK(process_destroy_kernel(&unowned, TEST_HHDM) == PROCESS_BAD_STATE,
          "unpublished process destroy returns PROCESS_BAD_STATE");
}

static void test_heap_failure_rollback(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    heap_fail = true;
    CHECK(process_create_kernel(&process, NULL, test_entry_a, TEST_HHDM) ==
              PROCESS_HEAP_FAILED,
          "heap failure returns PROCESS_HEAP_FAILED");
    CHECK(process == NULL, "heap failure publishes no process");
    CHECK(heap_allocs == 0U && heap_frees == 0U,
          "heap failure performs no heap transaction");
    CHECK(as_allocs == 0U && stack_allocs == 0U,
          "heap failure performs no address-space or stack allocation");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "heap failure leaves PID 1 available");
}

static void test_address_space_failure_rollback(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    as_fail = true;
    CHECK(process_create_kernel(&process, NULL, test_entry_a, TEST_HHDM) ==
              PROCESS_ADDRESS_SPACE_FAILED,
          "address-space failure returns PROCESS_ADDRESS_SPACE_FAILED");
    CHECK(process == NULL, "address-space failure publishes no process");
    CHECK(heap_allocs == 1U && heap_frees == 1U,
          "address-space failure returns heap object");
    CHECK(as_allocs == 0U && as_frees == 0U,
          "failed address-space allocation is not destroyed");
    CHECK(stack_allocs == 0U, "address-space failure allocates no stack");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "address-space failure leaves PID 1 available");
}

static void test_stack_failure_rollback(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    stack_fail = true;
    CHECK(process_create_kernel(&process, NULL, test_entry_a, TEST_HHDM) ==
              PROCESS_STACK_FAILED,
          "stack failure returns PROCESS_STACK_FAILED");
    CHECK(process == NULL, "stack failure publishes no process");
    CHECK(heap_allocs == 1U && heap_frees == 1U,
          "stack failure returns heap object");
    CHECK(as_allocs == 1U && as_frees == 1U,
          "stack failure destroys address space");
    CHECK(stack_allocs == 0U && stack_frees == 0U,
          "failed stack allocation performs no stack transaction");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "stack failure leaves PID 1 available");
}

static void test_hhdm_overflow_rollback(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    CHECK(process_create_kernel(&process, NULL, test_entry_a, UINT64_MAX) ==
              PROCESS_STACK_FAILED,
          "HHDM overflow returns PROCESS_STACK_FAILED");
    CHECK(process == NULL, "HHDM overflow publishes no process");
    CHECK(stack_allocs == 1U && stack_frees == 1U,
          "HHDM overflow frees the allocated stack");
    CHECK(as_allocs == 1U && as_frees == 1U,
          "HHDM overflow destroys address space");
    CHECK(heap_allocs == 1U && heap_frees == 1U,
          "HHDM overflow returns heap object");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "HHDM overflow leaves PID 1 available");
}

static void test_success_and_destroy(void) {
    struct kernel_process *root_process = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;

    reset_all();
    CHECK(process_create_kernel(&root_process, NULL, test_entry_a, TEST_HHDM) ==
              PROCESS_OK,
          "root kernel process creation succeeds");
    CHECK(root_process != NULL && root_process->pcb.published &&
              root_process->pcb.pid == 1U &&
              root_process->pcb.security_domain ==
                  PROCESS_SECURITY_DOMAIN_KERNEL,
          "created process publishes kernel PCB with PID 1");
    CHECK(root_process->pcb.parent == NULL &&
              root_process->pcb.child_count == 0U,
          "created root process has no parent or children");
    CHECK(root_process->address_space.pml4_phys == TEST_AS_PHYS,
          "created process owns its address space");
    CHECK(root_process->stack_phys == TEST_STACK_PHYS &&
              root_process->stack_pages == PROCESS_KERNEL_STACK_PAGES,
          "created process owns the exact kernel stack");
    CHECK(root_process->stack_top ==
              TEST_HHDM + TEST_STACK_PHYS + PROCESS_KERNEL_STACK_SIZE,
          "created process stack top is exact");
    CHECK(root_process->context.rip == (uintptr_t)test_entry_a &&
              root_process->context.rsp == root_process->stack_top - 8U,
          "created process context points at entry on its stack");

    CHECK(process_create_kernel(&child, &root_process->pcb,
                                test_entry_b, TEST_HHDM) == PROCESS_OK,
          "child kernel process creation succeeds");
    CHECK(child != NULL && child->pcb.pid == 2U,
          "child process publishes PID 2");
    CHECK(root_process->pcb.child_count == 1U &&
              root_process->pcb.children_head == &child->pcb,
          "parent graph links the created child");

    CHECK(process_destroy_kernel(root_process, TEST_HHDM) ==
              PROCESS_HAS_CHILDREN,
          "parent with children cannot be destroyed");
    CHECK(root_process->pcb.published && root_process->pcb.child_count == 1U,
          "rejected parent destruction leaves graph intact");
    CHECK(stack_frees == 0U && as_frees == 0U && heap_frees == 0U,
          "rejected parent destruction releases nothing");

    CHECK(process_destroy_kernel(child, TEST_HHDM) == PROCESS_OK,
          "child process destroys");
    CHECK(!child->pcb.published &&
              root_process->pcb.child_count == 0U,
          "destroyed child is unlinked from parent");
    CHECK(stack_frees == 1U && as_frees == 1U && heap_frees == 1U,
          "child destruction frees stack, address space, and heap object");

    CHECK(process_destroy_kernel(root_process, TEST_HHDM) == PROCESS_OK,
          "childless root process destroys");
    CHECK(stack_frees == 2U && as_frees == 2U && heap_frees == 2U,
          "root destruction frees remaining owned resources");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "destroyed process PID is immediately reusable");
}

static void test_publication_rollback(void) {
    struct kernel_process *bad = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;

    reset_all();
    CHECK(publish_root() == PROCESS_OK, "rollback fixture publishes root");

    CHECK(process_create_kernel(&bad, &invalid_parent,
                                test_entry_a, TEST_HHDM) ==
              PROCESS_BAD_STATE,
          "non-published parent returns PROCESS_BAD_STATE");
    CHECK(bad == NULL, "invalid parent publishes no process");
    CHECK(heap_allocs == 1U && heap_frees == 1U,
          "invalid parent returns heap object");
    CHECK(as_allocs == 1U && as_frees == 1U,
          "invalid parent destroys address space");
    CHECK(stack_allocs == 1U && stack_frees == 1U,
          "invalid parent frees kernel stack");
    CHECK(root.child_count == 0U,
          "invalid parent leaves parent graph unchanged");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 2U,
          "invalid parent consumes no PID beyond root");
    CHECK(process_pid_free(pid) == PROCESS_OK,
          "verification PID is released before link failure fixture");

    process_host_test_set_link_failure(true);
    CHECK(process_create_kernel(&child, &root,
                                test_entry_b, TEST_HHDM) ==
              PROCESS_LINK_FAILED,
          "injected link failure returns PROCESS_LINK_FAILED");
    process_host_test_set_link_failure(false);
    CHECK(child == NULL, "link failure publishes no process");
    CHECK(stack_allocs == 2U && stack_frees == 2U,
          "link failure frees the new kernel stack");
    CHECK(as_allocs == 2U && as_frees == 2U,
          "link failure destroys the new address space");
    CHECK(root.child_count == 0U,
          "link failure leaves parent graph unchanged");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 2U,
          "link failure releases the allocated PID");
}

static void test_exit_and_reap(void) {
    struct kernel_process *root_process = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;
    uint32_t status = 0U;

    reset_all();
    CHECK(process_create_kernel(&root_process, NULL, test_entry_a,
                                TEST_HHDM) == PROCESS_OK,
          "reap fixture creates root kernel process");
    CHECK(process_create_kernel(&child, &root_process->pcb, test_entry_b,
                                TEST_HHDM) == PROCESS_OK,
          "reap fixture creates child kernel process");

    CHECK(process_exit_kernel(NULL, 1U) == PROCESS_NULL,
          "null exit returns PROCESS_NULL");
    CHECK(process_exit_kernel(child, 42U) == PROCESS_OK,
          "running child exits with exact status");
    CHECK(child->pcb.published && child->pcb.pid == 2U &&
              child->pcb.parent == &root_process->pcb,
          "exited child remains published with parent link");
    CHECK(root_process->pcb.child_count == 1U,
          "exited child remains linked to parent");
    CHECK(child->address_space.pml4_phys == TEST_AS_PHYS + 4096U,
          "exited child retains its address space");
    CHECK(child->stack_phys == TEST_STACK_PHYS + PROCESS_KERNEL_STACK_SIZE,
          "exited child retains its kernel stack");
    CHECK(stack_allocs == 2U && as_allocs == 2U && heap_allocs == 2U,
          "exit publishes no allocation changes");

    CHECK(process_destroy_kernel(child, TEST_HHDM) ==
              PROCESS_ALREADY_EXITED,
          "destroying an exited process is rejected");
    CHECK(process_destroy_kernel(root_process, TEST_HHDM) ==
              PROCESS_HAS_CHILDREN,
          "parent with unreaped exited child cannot be destroyed");
    CHECK(process_exit_kernel(child, 43U) == PROCESS_ALREADY_EXITED &&
              child->pcb.published,
          "double exit is rejected without changing publication");

    CHECK(process_wait_kernel(&root_process->pcb, NULL, &status,
                              TEST_HHDM) == PROCESS_NULL,
          "wait with null PID output returns PROCESS_NULL");
    CHECK(process_wait_kernel(&root_process->pcb, &pid, NULL,
                              TEST_HHDM) == PROCESS_NULL,
          "wait with null status output returns PROCESS_NULL");
    CHECK(process_wait_kernel(&root_process->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_OK,
          "parent wait reaps exited child");
    CHECK(pid == 2U && status == 42U,
          "wait returns exact child PID and exit status");
    CHECK(root_process->pcb.child_count == 0U && !child->pcb.published,
          "reap unlinks and unpublishes the child");
    CHECK(stack_frees == 1U && as_frees == 1U && heap_frees == 1U,
          "reap frees child stack, address space, and heap object");
    CHECK(process_wait_kernel(&root_process->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_NO_CHILDREN,
          "second wait on empty parent returns PROCESS_NO_CHILDREN");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 2U,
          "reaped child PID is immediately reusable");
    CHECK(process_pid_free(pid) == PROCESS_OK,
          "reusable PID is returned for next fixture");
}

static void test_wait_errors_and_empty(void) {
    struct kernel_process *root_process = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;
    uint32_t status = 0U;

    reset_all();
    CHECK(process_create_kernel(&root_process, NULL, test_entry_a,
                                TEST_HHDM) == PROCESS_OK &&
              process_create_kernel(&child, &root_process->pcb,
                                    test_entry_b, TEST_HHDM) == PROCESS_OK,
          "wait error fixture creates root and child");
    CHECK(process_wait_kernel(&root_process->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_NOT_EXITED,
          "wait on running child returns PROCESS_NOT_EXITED");
    CHECK(root_process->pcb.child_count == 1U && child->pcb.published,
          "rejected wait leaves child and parent graph unchanged");

    CHECK(process_destroy_kernel(child, TEST_HHDM) == PROCESS_OK,
          "running child destroys before empty-wait fixture");
    CHECK(process_wait_kernel(&root_process->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_NO_CHILDREN,
          "wait with no children returns PROCESS_NO_CHILDREN");
    CHECK(process_destroy_kernel(root_process, TEST_HHDM) == PROCESS_OK,
          "empty childless parent destroys");
}

static void test_orphan_and_root_reap(void) {
    struct kernel_process *root_process = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;
    uint32_t status = 0U;

    reset_all();
    CHECK(process_create_kernel(&root_process, NULL, test_entry_a,
                                TEST_HHDM) == PROCESS_OK &&
              process_create_kernel(&child, &root_process->pcb,
                                    test_entry_b, TEST_HHDM) == PROCESS_OK,
          "orphan fixture creates root and child");

    CHECK(process_exit_kernel(child, 42U) == PROCESS_OK,
          "child exits before parent");
    CHECK(process_exit_kernel(root_process, 7U) == PROCESS_OK,
          "parent exits and orphans child");
    CHECK(root_process->pcb.child_count == 0U &&
              child->pcb.parent == NULL,
          "exiting parent orphans children before becoming exited");
    CHECK(child->pcb.published && root_process->pcb.published,
          "exiting parent and child remain published zombies");
    CHECK(stack_frees == 0U && as_frees == 0U && heap_frees == 0U,
          "exit and orphan transfer release no resources");

    CHECK(process_wait_kernel(NULL, &pid, &status, TEST_HHDM) ==
              PROCESS_OK,
          "kernel wait reaps exited orphan");
    CHECK(pid == 2U && status == 42U,
          "kernel wait returns orphan child PID and status");
    CHECK(heap_frees == 1U,
          "orphan reap frees first heap object");
    CHECK(process_wait_kernel(NULL, &pid, &status, TEST_HHDM) ==
              PROCESS_OK,
          "kernel wait reaps exited root orphan");
    CHECK(pid == 1U && status == 7U,
          "kernel wait returns root orphan PID and status");
    CHECK(heap_frees == 2U,
          "root orphan reap frees second heap object");
    CHECK(process_wait_kernel(NULL, &pid, &status, TEST_HHDM) ==
              PROCESS_NO_CHILDREN,
          "kernel wait with empty orphan queue returns PROCESS_NO_CHILDREN");
}

static void test_status_strings(void) {
    CHECK(process_status_string(PROCESS_HEAP_FAILED) != NULL &&
              process_status_string(PROCESS_STACK_FAILED) != NULL,
          "process creation status strings are present");
}

static void test_process_descriptor_lifetime(void) {
    struct kernel_process *root_process = NULL;
    struct kernel_process *child = NULL;
    uint32_t descriptor = 0U;

    reset_all();
    CHECK(process_create_kernel(&root_process, NULL, test_entry_a,
                                TEST_HHDM) == PROCESS_OK &&
              descriptor_table_is_published(
                  &root_process->descriptor_table) &&
              descriptor_table_open_count(
                  &root_process->descriptor_table) == 0U,
          "created root process publishes empty descriptor table");
    CHECK(descriptor_table_reserve(&root_process->descriptor_table,
                                   &descriptor) == DESCRIPTOR_OK &&
              descriptor == 0U &&
              descriptor_table_open_count(
                  &root_process->descriptor_table) == 1U,
          "root process descriptor reserve succeeds");

    CHECK(process_create_kernel(&child, &root_process->pcb, test_entry_b,
                                TEST_HHDM) == PROCESS_OK &&
              descriptor_table_is_published(
                  &child->descriptor_table) &&
              descriptor_table_reserve(&child->descriptor_table,
                                       &descriptor) == DESCRIPTOR_OK &&
              descriptor == 0U,
          "child process owns independent descriptor table");
    CHECK(descriptor_table_open_count(
              &child->descriptor_table) == 1U &&
              descriptor_table_open_count(
                  &root_process->descriptor_table) == 1U,
          "process descriptor tables do not share reservations");

    CHECK(process_destroy_kernel(child, TEST_HHDM) == PROCESS_OK &&
              !descriptor_table_is_published(
                  &child->descriptor_table) &&
              descriptor_table_open_count(
                  &child->descriptor_table) == 0U,
          "process destruction tears down child descriptor table");
    CHECK(process_destroy_kernel(root_process, TEST_HHDM) == PROCESS_OK &&
              !descriptor_table_is_published(
                  &root_process->descriptor_table) &&
              descriptor_table_open_count(
                  &root_process->descriptor_table) == 0U,
          "process destruction tears down root descriptor table");
}

int main(void) {
    test_invalid_arguments();
    test_heap_failure_rollback();
    test_address_space_failure_rollback();
    test_stack_failure_rollback();
    test_hhdm_overflow_rollback();
    test_success_and_destroy();
    test_publication_rollback();
    test_exit_and_reap();
    test_wait_errors_and_empty();
    test_orphan_and_root_reap();
    test_status_strings();
    test_process_descriptor_lifetime();

    if (failures != 0) {
        fprintf(stderr, "Process creation host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Process creation host tests: ALL OK");
    return 0;
}

#endif
