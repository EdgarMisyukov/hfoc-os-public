#include "context.h"
#include "scheduler.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SCHEDULER_HOST_TEST

extern void sched_reclaim_deferred(void);

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

#define STRESS_THREADS 3U

static uint8_t stress_memory[KERNEL_THREAD_STACK_SIZE * 8U]
    __attribute__((aligned(16)));
static struct kernel_thread threads[STRESS_THREADS];
static struct scheduler_task *current;
static unsigned int switch_calls;
static uint64_t host_phys;
static uint32_t allocations;
static uint32_t frees;
static uint32_t stack_frees;
static uint32_t freed_pages[STRESS_THREADS];
static uintptr_t freed_phys[STRESS_THREADS];
static bool alloc_fail;

static void stress_thread_entry(void) {
}

uint64_t pmm_alloc_contiguous_pages(uint32_t count) {
    if (alloc_fail || count == 0U) {
        return 0U;
    }
    const uint64_t physical = host_phys;
    host_phys += (uint64_t)count * 4096U;
    ++allocations;
    return physical;
}

bool pmm_free_contiguous_pages(uint64_t physical_address, uint32_t count) {
    if (physical_address == 0U || count == 0U) {
        return false;
    }
    ++frees;
    return true;
}

static enum scheduler_status record_switch(struct context_frame *current_frame,
                                           struct context_frame *next_frame)
{
    if (current_frame == NULL || next_frame == NULL) {
        return SCHEDULER_NULL;
    }
    ++switch_calls;
    return SCHEDULER_OK;
}

static void record_stack_free(uintptr_t physical_address,
                              uint32_t page_count)
{
    freed_phys[stack_frees] = physical_address;
    freed_pages[stack_frees] = page_count;
    ++stack_frees;
}

static void scheduler_ready(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK,
          "scheduler initializes for stress host test");
    memset(threads, 0, sizeof(threads));
    current = NULL;
    switch_calls = 0U;
    host_phys = (uintptr_t)stress_memory;
    allocations = 0U;
    frees = 0U;
    stack_frees = 0U;
    memset(freed_phys, 0, sizeof(freed_phys));
    memset(freed_pages, 0, sizeof(freed_pages));
    alloc_fail = false;
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "stress switch hook installs with no previous hook");
    CHECK(sched_host_test_set_stack_free_hook(record_stack_free) == NULL,
          "stress stack-free hook installs with no previous hook");
    sched_host_test_set_now_ticks(0U);
}

static void test_allocation_failure_rollback(void) {
    scheduler_ready();
    alloc_fail = true;

    CHECK(sched_spawn_kernel_thread(&threads[0], stress_thread_entry, 0U) ==
              SCHEDULER_NO_MEMORY,
          "stress allocation failure returns NO_MEMORY");
    CHECK(allocations == 0U,
          "stress allocation failure performs no allocation");
    CHECK(frees == 0U,
          "stress allocation failure performs no free");
    CHECK(sched_count() == 0U,
          "stress allocation failure leaves queue empty");
    CHECK(threads[0].stack_phys == 0U &&
              threads[0].task.queued == false,
          "stress allocation failure publishes no stack or task");
}

static void test_spawn_yield_sleep_exit_cleanup(void) {
    scheduler_ready();
    CHECK(sched_spawn_kernel_thread(&threads[0], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress thread A spawns");
    CHECK(sched_spawn_kernel_thread(&threads[1], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress thread B spawns");
    CHECK(allocations == 2U && frees == 0U && stack_frees == 0U,
          "stress spawn allocates two stacks and leaks none");

    CHECK(sched_yield() == SCHEDULER_OK,
          "stress idle selects thread A");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[0].task,
          "stress thread A is current");
    CHECK(sched_yield() == SCHEDULER_OK,
          "stress thread A yields to thread B");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[1].task,
          "stress thread B is current");

    CHECK(sched_sleep(3U) == SCHEDULER_OK,
          "stress thread B sleeps with timeout");
    CHECK(threads[1].task.state == TASK_STATE_BLOCKED,
          "stress sleeping thread B is BLOCKED");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[0].task,
          "stress sleeping thread B yields to thread A");

    sched_host_test_set_now_ticks(3U);
    CHECK(sched_on_tick() == SCHEDULER_OK,
          "stress deadline tick wakes and preempts to thread B");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[1].task,
          "stress thread B resumes as current");

    const uintptr_t stack_b = threads[1].stack_phys;
    const uintptr_t stack_a = threads[0].stack_phys;
    CHECK(sched_exit() == SCHEDULER_OK,
          "stress thread B exits");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[0].task,
          "stress thread B handoff selects thread A");
    CHECK(sched_exit() == SCHEDULER_OK,
          "stress thread A exits");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "stress last thread exits to idle");
    CHECK(stack_frees == 0U,
          "stress exit defers both stacks");

    sched_reclaim_deferred();
    CHECK(stack_frees == 2U,
          "stress deferred reclamation frees two stacks");
    CHECK(freed_phys[0] == stack_b &&
              freed_pages[0] == KERNEL_THREAD_STACK_PAGES,
          "stress deferred reclamation frees exact thread B stack");
    CHECK(freed_phys[1] == stack_a &&
              freed_pages[1] == KERNEL_THREAD_STACK_PAGES,
          "stress deferred reclamation frees exact thread A stack");
}

static void test_invalid_successor_exit_rollback(void) {
    scheduler_ready();
    CHECK(sched_spawn_kernel_thread(&threads[0], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress rollback thread A spawns");
    CHECK(sched_spawn_kernel_thread(&threads[1], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress rollback thread B spawns");
    CHECK(sched_yield() == SCHEDULER_OK,
          "stress rollback idle selects thread A");
    threads[1].task.context.rsp = 0U;
    threads[1].task.context.rip = (uintptr_t)0x2000U;

    CHECK(sched_exit() == SCHEDULER_BAD_CONTEXT,
          "stress exit rejects invalid successor context");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &threads[0].task &&
              threads[0].task.state == TASK_STATE_RUNNING,
          "stress invalid successor leaves thread A running");
    CHECK(threads[1].task.state == TASK_STATE_RUNNABLE &&
              threads[1].task.queued,
          "stress invalid successor leaves thread B queued");
    CHECK(sched_count() == 1U,
          "stress invalid successor leaves queue unchanged");
    CHECK(stack_frees == 0U,
          "stress invalid successor defers no stack");
}

static void test_mid_loop_allocation_failure(void) {
    scheduler_ready();
    CHECK(sched_spawn_kernel_thread(&threads[0], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress mid-loop thread A spawns");
    alloc_fail = true;
    CHECK(sched_spawn_kernel_thread(&threads[1], stress_thread_entry, 0U) ==
              SCHEDULER_NO_MEMORY,
          "stress mid-loop thread B allocation fails");
    CHECK(allocations == 1U && frees == 0U && sched_count() == 1U &&
              threads[1].stack_phys == 0U,
          "stress mid-loop failure leaves no leak or published task");
    alloc_fail = false;
    CHECK(sched_spawn_kernel_thread(&threads[2], stress_thread_entry, 0U) ==
              SCHEDULER_OK,
          "stress mid-loop thread C spawns after failure");
    CHECK(allocations == 2U && sched_count() == 2U,
          "stress mid-loop continues after handled failure");

    CHECK(sched_yield() == SCHEDULER_OK &&
              sched_current(&current) == SCHEDULER_OK &&
              current == &threads[0].task,
          "stress mid-loop selects thread A");
    CHECK(sched_exit() == SCHEDULER_OK &&
              sched_current(&current) == SCHEDULER_OK &&
              current == &threads[2].task,
          "stress mid-loop thread A hands off to thread C");
    CHECK(sched_exit() == SCHEDULER_OK &&
              sched_current(&current) == SCHEDULER_OK &&
              current == NULL,
          "stress mid-loop thread C exits to idle");

    sched_reclaim_deferred();
    CHECK(stack_frees == 2U,
          "stress mid-loop reclaims both successful stacks exactly once");
}

int main(void) {
    test_allocation_failure_rollback();
    test_spawn_yield_sleep_exit_cleanup();
    test_invalid_successor_exit_rollback();
    test_mid_loop_allocation_failure();

    if (failures != 0) {
        fprintf(stderr, "Scheduler stress host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Scheduler stress host tests: ALL OK");
    return 0;
}

#endif
