#include "process.h"
#include "user_init.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HFOC_PROCESS_HOST_TEST) && defined(HFOC_USER_INIT_HOST_TEST)

#define PROCESS_POOL_CAP 4U

static int failures;

static uint8_t user_page_storage[0x4000U]
    __attribute__((aligned(16)));

#define TEST_HHDM        ((uint64_t)(uintptr_t)user_page_storage - 0x1000U)
#define TEST_STACK_PHYS  UINT64_C(0x3000)
#define TEST_AS_PHYS     UINT64_C(0x4000)
#define USER_CODE_PHYS   UINT64_C(0x1000)
#define USER_STACK_PHYS  UINT64_C(0x2000)

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_entry(void) {
}

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

static uint64_t next_user_page_phys;
static uint32_t user_page_allocs;
static uint32_t user_page_frees;
static bool user_page_fail;

uint64_t pmm_alloc_page(void) {
    uint64_t physical;

    if (user_page_fail) {
        return 0U;
    }
    physical = next_user_page_phys;
    next_user_page_phys += 4096U;
    ++user_page_allocs;
    return physical;
}

bool pmm_free_page(uint64_t physical_address) {
    if (physical_address == 0U) {
        return false;
    }
    ++user_page_frees;
    return true;
}

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

struct map_call {
    uint64_t virtual_address;
    uint64_t physical_address;
    uint64_t flags;
    uint64_t page_count;
};

#define MAP_CALL_CAP 2U

static struct map_call map_calls[MAP_CALL_CAP];
static uint32_t map_count;
static bool map_fail;

enum vmm_status vmm_map_user_pages_in(
    const struct address_space *as,
    uint64_t virtual_address,
    const uint64_t *physical_pages,
    uint64_t page_count,
    uint64_t user_flags,
    uint64_t hhdm_offset)
{
    (void)as;
    (void)hhdm_offset;
    if (map_fail) {
        return VMM_PMM_ALLOC_FAILED;
    }
    if (physical_pages == NULL || map_count >= MAP_CALL_CAP) {
        return VMM_BAD_OUTPUT_POINTER;
    }
    map_calls[map_count].virtual_address = virtual_address;
    map_calls[map_count].physical_address = physical_pages[0];
    map_calls[map_count].flags = user_flags;
    map_calls[map_count].page_count = page_count;
    ++map_count;
    return VMM_OK;
}

static void reset_all(void) {
    process_host_test_reset();
    memset(process_pool, 0, sizeof(process_pool));
    memset(process_pool_used, 0, sizeof(process_pool_used));
    heap_allocs = 0U;
    heap_frees = 0U;
    heap_fail = false;
    next_stack_phys = TEST_STACK_PHYS;
    stack_allocs = 0U;
    stack_frees = 0U;
    stack_fail = false;
    next_user_page_phys = USER_CODE_PHYS;
    user_page_allocs = 0U;
    user_page_frees = 0U;
    user_page_fail = false;
    as_allocs = 0U;
    as_frees = 0U;
    as_fail = false;
    memset(map_calls, 0, sizeof(map_calls));
    map_count = 0U;
    map_fail = false;
}

static void test_image_contract(void) {
    uint8_t image[USER_INIT_IMAGE_MAX_SIZE];

    reset_all();
    CHECK(user_init_image_size() > 0U &&
              user_init_image_size() <= USER_INIT_IMAGE_MAX_SIZE,
          "embedded user init image fits the bounded image page");
    CHECK(!user_init_image_copy(NULL, USER_INIT_IMAGE_MAX_SIZE),
          "image copy rejects null destination");
    CHECK(!user_init_image_copy(image, 0U),
          "image copy rejects zero capacity");
    CHECK(user_init_image_copy(image, USER_INIT_IMAGE_MAX_SIZE),
          "image copy succeeds into bounded destination");
}

static void test_success_create_destroy(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    CHECK(process_create_user_init(&process, NULL, test_entry,
                                   TEST_HHDM) == PROCESS_OK,
          "user init process creation succeeds");
    CHECK(process != NULL && process->pcb.published &&
              process->pcb.pid == 1U &&
              process->pcb.security_domain == PROCESS_SECURITY_DOMAIN_USER,
          "user init process publishes user PCB with PID 1");
    CHECK(process->user_pages_owned &&
              process->user_code_phys == USER_CODE_PHYS &&
              process->user_stack_phys == USER_STACK_PHYS,
          "user init process owns exact user code and stack pages");
    CHECK(process_limit_user_memory_limit_pages(process) ==
              PROCESS_USER_MEMORY_LIMIT_UNLIMITED &&
              process_limit_user_memory_pages(process) == 2U,
          "user init process accounts both fixed user pages within default limit");
    CHECK(capability_table_open_count(
              &process->pcb.capability_table) == 4U &&
              capability_table_has(
                  &process->pcb.capability_table,
                  CAPABILITY_TYPE_SYSCALL_WRITE) &&
              capability_table_has(
                  &process->pcb.capability_table,
                  CAPABILITY_TYPE_SYSCALL_YIELD) &&
              capability_table_has(
                  &process->pcb.capability_table,
                  CAPABILITY_TYPE_SYSCALL_EXIT) &&
              capability_table_has(
                  &process->pcb.capability_table,
                  CAPABILITY_TYPE_SYSCALL_FILE),
          "user init process publishes all baseline syscall capabilities");
    CHECK(process->stack_phys == TEST_STACK_PHYS &&
              process->address_space.pml4_phys == TEST_AS_PHYS,
          "user init process retains exact kernel stack and address space");
    CHECK(map_count == 2U &&
              map_calls[0].virtual_address == VMM_USER_IMAGE_BASE &&
              map_calls[0].physical_address == USER_CODE_PHYS &&
              map_calls[0].flags == USER_INIT_CODE_FLAGS &&
              map_calls[0].page_count == 1U,
          "code page maps at exact image base with code flags");
    CHECK(map_calls[1].virtual_address == VMM_USER_STACK_BASE &&
              map_calls[1].physical_address == USER_STACK_PHYS &&
              map_calls[1].flags == USER_INIT_STACK_FLAGS &&
              map_calls[1].page_count == 1U,
          "stack page maps at exact stack base with NX flags");

    CHECK(process_destroy_kernel(process, TEST_HHDM) == PROCESS_OK,
          "user init process destroys");
    CHECK(user_page_frees == 2U && stack_frees == 1U &&
              as_frees == 1U && heap_frees == 1U,
          "destroy frees user pages, kernel stack, address space, and heap");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "destroyed user init PID is reusable");
}

static void test_user_page_alloc_failure(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    user_page_fail = true;
    CHECK(process_create_user_init(&process, NULL, test_entry,
                                   TEST_HHDM) == PROCESS_USER_PAGE_FAILED,
          "user page allocation failure returns USER_PAGE_FAILED");
    CHECK(process == NULL,
          "user page allocation failure publishes no process");
    CHECK(user_page_allocs == 0U && user_page_frees == 0U,
          "user page allocation failure performs no user page transaction");
    CHECK(stack_allocs == 1U && stack_frees == 1U &&
              as_allocs == 1U && as_frees == 1U &&
              heap_allocs == 1U && heap_frees == 1U,
          "user page allocation failure rolls back process resources");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "user page allocation failure releases PID");
}

static void test_mapping_failure(void) {
    struct kernel_process *process = NULL;
    uint32_t pid = 0U;

    reset_all();
    map_fail = true;
    CHECK(process_create_user_init(&process, NULL, test_entry,
                                   TEST_HHDM) == PROCESS_USER_PAGE_FAILED,
          "user mapping failure returns USER_PAGE_FAILED");
    CHECK(process == NULL,
          "user mapping failure publishes no process");
    CHECK(user_page_allocs == 2U && user_page_frees == 2U,
          "user mapping failure frees both user pages");
    CHECK(stack_frees == 1U && as_frees == 1U && heap_frees == 1U,
          "user mapping failure rolls back process resources");
    CHECK(process_pid_alloc(&pid) == PROCESS_OK && pid == 1U,
          "user mapping failure releases PID");
}

static void test_exit_and_reap(void) {
    struct kernel_process *root = NULL;
    struct kernel_process *child = NULL;
    uint32_t pid = 0U;
    uint32_t expected_pid = 0U;
    uint32_t status = 0U;

    reset_all();
    CHECK(process_create_kernel(&root, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "reap fixture creates root process");
    CHECK(process_create_user_init(&child, &root->pcb, test_entry,
                                   TEST_HHDM) == PROCESS_OK,
          "reap fixture creates user init child");
    CHECK(process_exit_kernel(child, 7U) == PROCESS_OK,
          "user init child exits with exact status");
    expected_pid = child->pcb.pid;
    CHECK(process_wait_kernel(&root->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_OK,
          "parent reaps exited user init child");
    CHECK(pid == expected_pid && status == 7U,
          "reap returns exact child PID and exit status");
    CHECK(user_page_frees == 2U && stack_frees == 1U &&
              as_frees == 1U && heap_frees == 1U,
          "reap frees user pages and child process resources");
    CHECK(process_destroy_kernel(root, TEST_HHDM) == PROCESS_OK,
          "root destroys after reaping child");
    CHECK(user_page_frees == 2U && stack_frees == 2U &&
              as_frees == 2U && heap_frees == 2U,
          "root destroy completes all process cleanup");
}

int main(void) {
    test_image_contract();
    test_success_create_destroy();
    test_user_page_alloc_failure();
    test_mapping_failure();
    test_exit_and_reap();

    if (failures != 0U) {
        fprintf(stderr, "User init process host tests: %u FAILURES\n",
                failures);
        return 1;
    }
    puts("User init process host tests: ALL OK");
    return 0;
}

#endif
