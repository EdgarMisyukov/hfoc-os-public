#include "context.h"
#include "descriptor.h"
#include "process.h"
#include "scheduler.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HFOC_PROCESS_HOST_TEST) && \
    defined(HFOC_PROCESS_SCHEDULER_HOST_TEST) && \
    defined(HFOC_SCHEDULER_HOST_TEST)

extern void sched_kernel_thread_trampoline(void);
extern void sched_reclaim_deferred(void);

#define TEST_HHDM        0U
#define TEST_STACK_PHYS  UINT64_C(0x00100000)
#define TEST_AS_PHYS     UINT64_C(0x00200000)
#define PROCESS_POOL_CAP 4U
#define CPU_LIMIT_TICKS  2U

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
static uint8_t process_limit_stack[
    PROCESS_POOL_CAP * PROCESS_KERNEL_STACK_SIZE]
    __attribute__((aligned(16)));

static struct context_frame switch_current;
static struct context_frame switch_next;
static unsigned int switch_calls;
static struct scheduler_task *current;

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

static enum scheduler_status record_switch(struct context_frame *current_frame,
                                           struct context_frame *next_frame)
{
    if (current_frame == NULL || next_frame == NULL) {
        return SCHEDULER_NULL;
    }
    switch_current = *current_frame;
    switch_next = *next_frame;
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
    next_stack_phys = (uintptr_t)process_limit_stack;
    stack_allocs = 0U;
    stack_frees = 0U;
    stack_fail = false;
    as_allocs = 0U;
    as_frees = 0U;
    as_fail = false;
    memset(&switch_current, 0, sizeof(switch_current));
    memset(&switch_next, 0, sizeof(switch_next));
    switch_calls = 0U;
    current = NULL;
}

static void scheduler_ready(void) {
    CHECK(sched_init() == SCHEDULER_OK,
          "process limit fixture initializes scheduler");
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "process limit fixture installs switch hook");
}

static void test_limit_validation_and_memory_charge(void) {
    struct kernel_process *probe = NULL;
    struct kernel_process unowned;

    reset_all();
    CHECK(process_limit_set_cpu_ticks(NULL, 1U) == PROCESS_NULL,
          "CPU limit setter rejects null process");
    CHECK(process_limit_set_user_memory_pages(NULL, 1U) == PROCESS_NULL,
          "user-memory limit setter rejects null process");
    CHECK(process_limit_charge_user_memory(NULL, 1U) == PROCESS_NULL,
          "user-memory charge rejects null process");

    memset(&unowned, 0, sizeof(unowned));
    CHECK(process_limit_set_cpu_ticks(&unowned, 1U) == PROCESS_BAD_STATE,
          "CPU limit setter rejects unpublished process");
    CHECK(process_limit_set_user_memory_pages(&unowned, 1U) ==
              PROCESS_BAD_STATE,
          "user-memory limit setter rejects unpublished process");
    CHECK(process_limit_charge_user_memory(&unowned, 1U) == PROCESS_BAD_STATE,
          "user-memory charge rejects unpublished process");

    CHECK(process_create_kernel(&probe, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "memory charge fixture creates process");
    CHECK(process_limit_cpu_ticks(probe) ==
              PROCESS_CPU_TICK_LIMIT_UNLIMITED &&
              process_limit_user_memory_limit_pages(probe) ==
                  PROCESS_USER_MEMORY_LIMIT_UNLIMITED &&
              process_limit_user_memory_pages(probe) == 0U,
          "new process publishes unlimited limits and zero memory");

    CHECK(process_limit_set_user_memory_pages(probe, 1U) == PROCESS_OK &&
              process_limit_user_memory_limit_pages(probe) == 1U,
          "one-page user-memory limit publishes");
    CHECK(process_limit_charge_user_memory(probe, 2U) ==
              PROCESS_USER_MEMORY_LIMIT_EXCEEDED &&
              process_limit_user_memory_pages(probe) == 0U,
          "two-page charge against one-page limit fails without mutation");

    CHECK(process_limit_set_user_memory_pages(probe, 2U) == PROCESS_OK &&
              process_limit_charge_user_memory(probe, 1U) == PROCESS_OK &&
              process_limit_user_memory_pages(probe) == 1U,
          "first single-page charge under two-page limit succeeds");
    CHECK(process_limit_charge_user_memory(probe, 1U) == PROCESS_OK &&
              process_limit_user_memory_pages(probe) == 2U,
          "second single-page charge reaches exact limit");
    CHECK(process_limit_charge_user_memory(probe, 1U) ==
              PROCESS_USER_MEMORY_LIMIT_EXCEEDED &&
              process_limit_user_memory_pages(probe) == 2U,
          "charge beyond exact limit fails without mutation");

    CHECK(process_limit_set_cpu_ticks(probe, 1U) == PROCESS_OK &&
              process_limit_cpu_ticks(probe) == 1U,
          "CPU limit setter accepts pre-spawn process");
    CHECK(process_destroy_kernel(probe, TEST_HHDM) == PROCESS_OK,
          "memory charge fixture destroys cleanly");
    CHECK(stack_frees == 1U && as_frees == 1U && heap_frees == 1U,
          "memory charge fixture releases process resources exactly once");
}

static void test_cpu_limit_termination_and_independent_continuation(void) {
    struct kernel_process *parent = NULL;
    struct kernel_process *limited = NULL;
    struct kernel_process *independent = NULL;
    uint32_t pid = 0U;
    uint32_t status = 0U;
    uint32_t limited_pid = 0U;
    uint32_t independent_pid = 0U;
    uint32_t reaped = 0U;

    reset_all();
    scheduler_ready();
    CHECK(process_create_kernel(&parent, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "CPU limit fixture creates parent");
    CHECK(process_create_kernel(&limited, &parent->pcb, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "CPU limit fixture creates limited child");
    limited_pid = limited->pcb.pid;
    CHECK(process_limit_set_cpu_ticks(limited, CPU_LIMIT_TICKS) == PROCESS_OK &&
              process_limit_cpu_ticks(limited) == CPU_LIMIT_TICKS,
          "limited child publishes CPU tick limit before spawn");
    CHECK(process_create_kernel(&independent, &parent->pcb, test_entry,
                                TEST_HHDM) == PROCESS_OK,
          "CPU limit fixture creates independent child");
    independent_pid = independent->pcb.pid;

    CHECK(process_spawn_kernel(limited, TEST_HHDM) == PROCESS_OK &&
              limited->scheduler_task.cpu_tick_limit == CPU_LIMIT_TICKS,
          "limited child spawn publishes exact scheduler tick limit");
    CHECK(process_spawn_kernel(independent, TEST_HHDM) == PROCESS_OK &&
              independent->scheduler_task.cpu_tick_limit ==
                  PROCESS_CPU_TICK_LIMIT_UNLIMITED,
          "independent child spawn remains unlimited");

    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects limited child first");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &limited->scheduler_task,
          "limited child is current before first tick");

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "first tick preempts limited child");
    CHECK(limited->scheduler_task.accounting.tick_count == 1U &&
              limited->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
              limited->scheduler_task.state == TASK_STATE_RUNNABLE,
          "first tick stays under limit and preempts limited child");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &independent->scheduler_task,
          "independent child becomes current after first tick");

    CHECK(sched_yield() == SCHEDULER_OK,
          "independent child yields back to limited child");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &limited->scheduler_task,
          "limited child becomes current for second tick");

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "second tick terminates limited child safely");
    CHECK(limited->lifecycle == PROCESS_LIFECYCLE_EXITED &&
              limited->exit_status == PROCESS_EXIT_LIMIT_EXCEEDED &&
              limited->scheduler_task.state == TASK_STATE_EXITED,
          "limited child publishes limit-exit state");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &independent->scheduler_task &&
              independent->lifecycle == PROCESS_LIFECYCLE_RUNNING,
          "independent child continues after limited termination");

    CHECK(process_wait_kernel(&parent->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_OK &&
              pid == limited_pid &&
              status == PROCESS_EXIT_LIMIT_EXCEEDED,
          "parent reaps limited child with exact limit status");
    ++reaped;

    CHECK(process_exit_kernel(independent, 0U) == PROCESS_OK &&
              sched_exit() == SCHEDULER_OK,
          "independent child exits and scheduler returns to idle");
    CHECK(process_wait_kernel(&parent->pcb, &pid, &status,
                              TEST_HHDM) == PROCESS_OK &&
              pid == independent_pid && status == 0U,
          "parent reaps independent child after normal exit");
    ++reaped;

    CHECK(reaped == 2U &&
              process_destroy_kernel(parent, TEST_HHDM) == PROCESS_OK,
          "parent destroys after reaping both children");
}

static void test_limit_invalid_successor_does_not_exit(void) {
    struct kernel_process *parent = NULL;
    struct kernel_process *limited = NULL;
    struct kernel_process *independent = NULL;

    reset_all();
    scheduler_ready();
    CHECK(process_create_kernel(&parent, NULL, test_entry,
                                TEST_HHDM) == PROCESS_OK &&
              process_create_kernel(&limited, &parent->pcb, test_entry,
                                    TEST_HHDM) == PROCESS_OK &&
              process_limit_set_cpu_ticks(limited, CPU_LIMIT_TICKS) ==
                  PROCESS_OK &&
              process_create_kernel(&independent, &parent->pcb, test_entry,
                                    TEST_HHDM) == PROCESS_OK,
          "invalid-successor fixture creates processes and limit");
    CHECK(process_spawn_kernel(limited, TEST_HHDM) == PROCESS_OK &&
              process_spawn_kernel(independent, TEST_HHDM) == PROCESS_OK,
          "invalid-successor fixture spawns both children");

    CHECK(sched_yield() == SCHEDULER_OK &&
              sched_on_tick() == SCHEDULER_OK,
          "invalid-successor fixture reaches independent as current");
    independent->scheduler_task.context.rsp = 0U;
    CHECK(sched_yield() == SCHEDULER_OK &&
              sched_current(&current) == SCHEDULER_OK &&
              current == &limited->scheduler_task,
          "invalid-successor fixture returns limited to current");

    CHECK(sched_on_tick() == SCHEDULER_BAD_CONTEXT,
          "invalid successor at limit returns BAD_CONTEXT");
    CHECK(limited->lifecycle == PROCESS_LIFECYCLE_RUNNING &&
              limited->scheduler_task.state == TASK_STATE_RUNNING &&
              !limited->scheduler_task.queued,
          "invalid successor at limit leaves limited process running");
    CHECK(independent->scheduler_task.state == TASK_STATE_RUNNABLE &&
              independent->scheduler_task.queued,
          "invalid successor at limit leaves independent runnable");
}

int main(void) {
    test_limit_validation_and_memory_charge();
    test_cpu_limit_termination_and_independent_continuation();
    test_limit_invalid_successor_does_not_exit();

    if (failures != 0) {
        fprintf(stderr, "Process limit host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Process limit host tests: ALL OK");
    return 0;
}

#endif
