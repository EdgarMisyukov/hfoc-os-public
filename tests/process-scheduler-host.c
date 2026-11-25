#include "context.h"
#include "descriptor.h"
#include "process.h"
#include "scheduler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HFOC_PROCESS_HOST_TEST) && defined(HFOC_SCHEDULER_HOST_TEST)

extern void sched_kernel_thread_trampoline(void);
extern void sched_reclaim_deferred(void);

#define TEST_HHDM        0U
#define TEST_STACK_PHYS  UINT64_C(0x00100000)
#define TEST_AS_PHYS     UINT64_C(0x00200000)
#define PROCESS_POOL_CAP 4U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct kernel_process process_pool[PROCESS_POOL_CAP];
static bool process_pool_used[PROCESS_POOL_CAP];
static uint32_t heap_allocs;
static uint32_t heap_frees;
static bool heap_fail;
static uint8_t process_scheduler_stack[PROCESS_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));

static struct context_frame switch_current;
static struct context_frame switch_next;
static unsigned int switch_calls;

static void test_entry(void) {
}

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

static enum scheduler_status record_switch(struct context_frame *current,
                                           struct context_frame *next)
{
    if (current == NULL || next == NULL) {
        return SCHEDULER_NULL;
    }
    switch_current = *current;
    switch_next = *next;
    ++switch_calls;
    return SCHEDULER_OK;
}

static void reset_all(void) {
    process_host_test_reset();
    sched_host_test_reset();
    memset(process_pool, 0, sizeof(process_pool));
    memset(process_pool_used, 0, sizeof(process_pool_used));
    heap_allocs = 0U;
    heap_frees = 0U;
    heap_fail = false;
    next_stack_phys = (uintptr_t)process_scheduler_stack;
    stack_allocs = 0U;
    stack_frees = 0U;
    stack_fail = false;
    as_allocs = 0U;
    as_frees = 0U;
    as_fail = false;
    memset(&switch_current, 0, sizeof(switch_current));
    memset(&switch_next, 0, sizeof(switch_next));
    switch_calls = 0U;
}

static void test_spawn_validation(void) {
    struct kernel_process *process = NULL;
    struct kernel_process unowned;

    reset_all();
    CHECK(process_spawn_kernel(NULL, TEST_HHDM) == PROCESS_NULL,
          "spawn with null process returns PROCESS_NULL");
    memset(&unowned, 0, sizeof(unowned));
    CHECK(process_spawn_kernel(&unowned, TEST_HHDM) == PROCESS_BAD_STATE,
          "spawn of unpublished process returns BAD_STATE");

    CHECK(process_create_kernel(&process, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "spawn validation creates process");
    CHECK(process_spawn_kernel(process, TEST_HHDM) ==
              PROCESS_SCHEDULER_FAILED,
          "spawn before scheduler init returns SCHEDULER_FAILED");
    CHECK(!process->scheduler_published,
          "failed spawn publishes no scheduler task");
}

static void test_spawn_and_double_spawn(void) {
    struct kernel_process *process = NULL;
    struct scheduler_task *selected = NULL;

    reset_all();
    CHECK(sched_init() == SCHEDULER_OK,
          "process scheduler fixture initializes scheduler");
    CHECK(process_create_kernel(&process, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "process scheduler fixture creates process");
    CHECK(process_spawn_kernel(process, TEST_HHDM) == PROCESS_OK,
          "created process spawns into scheduler");
    CHECK(process->scheduler_published &&
              process->scheduler_task.state == TASK_STATE_RUNNABLE &&
              process->scheduler_task.queued,
          "spawn publishes runnable scheduler task");
    CHECK(process->scheduler_task.process_owner == process,
          "scheduler task records process owner");
    CHECK(process->scheduler_task.stack_phys == 0U &&
              process->scheduler_task.stack_pages == 0U,
          "scheduler task does not own process stack");
    CHECK(process->scheduler_task.context.rip ==
              (uintptr_t)sched_kernel_thread_trampoline,
          "scheduler task uses kernel-thread trampoline");
    CHECK(*(uintptr_t *)process->scheduler_task.context.rsp != 0U &&
              *(uintptr_t *)process->scheduler_task.context.rsp !=
                  (uintptr_t)test_entry,
          "scheduler task stores process entry wrapper");
    CHECK(process->scheduler_task.kernel_stack_top == process->stack_top,
          "process scheduler task publishes exact kernel stack top");
    CHECK(sched_count() == 1U,
          "spawned process is the only runnable task");

    CHECK(process_spawn_kernel(process, TEST_HHDM) == PROCESS_BAD_STATE,
          "double spawn returns BAD_STATE");
    CHECK(process->scheduler_published && sched_count() == 1U,
          "double spawn leaves scheduler state unchanged");
    CHECK(process_destroy_kernel(process, TEST_HHDM) == PROCESS_BAD_STATE,
          "destroy of scheduler-published process returns BAD_STATE");

    CHECK(sched_next(&selected) == SCHEDULER_OK &&
              selected == &process->scheduler_task &&
              selected->state == TASK_STATE_RUNNING,
          "scheduler selects process task as current");
}

static void test_exited_process_not_spawned(void) {
    struct kernel_process *process = NULL;

    reset_all();
    CHECK(sched_init() == SCHEDULER_OK,
          "exited spawn fixture initializes scheduler");
    CHECK(process_create_kernel(&process, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK &&
              process_exit_kernel(process, 0U) == PROCESS_OK,
          "exited spawn fixture creates and exits process");
    CHECK(process_spawn_kernel(process, TEST_HHDM) == PROCESS_BAD_STATE &&
              !process->scheduler_published,
          "exited process cannot be spawned");
}

static void test_idle_recovery(void) {
    struct kernel_process *process = NULL;
    struct scheduler_task *selected = NULL;
    struct scheduler_task *empty = (struct scheduler_task *)(uintptr_t)1U;

    reset_all();
    CHECK(sched_init() == SCHEDULER_OK,
          "idle recovery fixture initializes scheduler");
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "idle recovery installs switch hook");
    CHECK(process_create_kernel(&process, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK &&
              process_spawn_kernel(process, TEST_HHDM) == PROCESS_OK,
          "idle recovery creates and spawns process");
    CHECK(sched_next(&selected) == SCHEDULER_OK &&
              selected == &process->scheduler_task,
          "idle recovery selects process task");

    CHECK(sched_exit() == SCHEDULER_OK,
          "exiting sole process returns scheduler to idle");
    CHECK(process->scheduler_task.state == TASK_STATE_EXITED,
          "exited process task publishes TASK_STATE_EXITED");
    CHECK(sched_current(&selected) == SCHEDULER_OK && selected == NULL,
          "idle recovery clears current task");
    CHECK(sched_count() == 0U,
          "idle recovery leaves empty run queue");
    CHECK(switch_calls == 1U,
          "idle recovery performs one switch to idle");
    CHECK(sched_next(&empty) == SCHEDULER_EMPTY,
          "idle scheduler has no next task");
    sched_reclaim_deferred();
    CHECK(stack_frees == 0U,
          "scheduler does not free process-owned stack");
}

int main(void) {
    test_spawn_validation();
    test_spawn_and_double_spawn();
    test_exited_process_not_spawned();
    test_idle_recovery();

    if (failures != 0) {
        fprintf(stderr, "Process scheduler host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Process scheduler host tests: ALL OK");
    return 0;
}

#endif
