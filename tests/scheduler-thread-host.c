#include "scheduler.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SCHEDULER_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

extern void sched_kernel_thread_trampoline(void);

static uint8_t thread_stack[KERNEL_THREAD_STACK_SIZE]
    __attribute__((aligned(16)));
static struct kernel_thread thread;
static uint64_t host_phys;
static uint32_t allocations;
static uint32_t frees;
static bool alloc_fail;

static void test_thread_entry(void) {
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

static void reset_all(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK, "scheduler initializes for threads");
    memset(&thread, 0, sizeof(thread));
    host_phys = (uintptr_t)thread_stack;
    allocations = 0U;
    frees = 0U;
    alloc_fail = false;
}

static void test_spawn_invalid_arguments(void) {
    sched_host_test_reset();
    CHECK(sched_spawn_kernel_thread(&thread, test_thread_entry, 0U) ==
              SCHEDULER_NOT_INITIALIZED,
          "thread spawn before init returns NOT_INITIALIZED");

    reset_all();
    CHECK(sched_spawn_kernel_thread(NULL, test_thread_entry, 0U) ==
              SCHEDULER_NULL,
          "null thread returns SCHEDULER_NULL");
    CHECK(sched_spawn_kernel_thread(&thread, NULL, 0U) == SCHEDULER_NULL,
          "null entry returns SCHEDULER_NULL");
}

static void test_spawn_allocation_failure(void) {
    reset_all();
    alloc_fail = true;
    CHECK(sched_spawn_kernel_thread(&thread, test_thread_entry, 0U) ==
              SCHEDULER_NO_MEMORY,
          "PMM allocation failure returns NO_MEMORY");
    CHECK(allocations == 0U, "PMM failure performs no allocation");
    CHECK(frees == 0U, "PMM failure performs no free");
    CHECK(sched_count() == 0U, "PMM failure leaves queue empty");
    CHECK(thread.stack_phys == 0U, "PMM failure publishes no stack");
    CHECK(thread.task.queued == false,
          "PMM failure publishes no queued task");
}

static void test_spawn_overflow_rollback(void) {
    reset_all();
    const uint64_t hhdm = UINT64_MAX - 0x1000ULL;
    CHECK(sched_spawn_kernel_thread(&thread, test_thread_entry, hhdm) ==
              SCHEDULER_NO_MEMORY,
          "HHDM overflow returns NO_MEMORY");
    CHECK(allocations == 1U, "overflow path allocates stack once");
    CHECK(frees == 1U, "overflow path frees allocated stack");
    CHECK(sched_count() == 0U, "overflow path leaves queue empty");
    CHECK(thread.stack_phys == 0U, "overflow path publishes no stack");
    CHECK(thread.task.queued == false,
          "overflow path publishes no queued task");
}

static void test_spawn_success(void) {
    reset_all();
    CHECK(sched_spawn_kernel_thread(&thread, test_thread_entry, 0U) ==
              SCHEDULER_OK,
          "kernel thread spawn succeeds");
    CHECK(allocations == 1U, "kernel thread spawn allocates one stack");
    CHECK(frees == 0U, "successful spawn does not free stack");
    CHECK(sched_count() == 1U, "successful spawn enqueues one task");
    CHECK(thread.task.state == TASK_STATE_RUNNABLE,
          "spawned task is RUNNABLE");
    CHECK(thread.task.queued, "spawned task is queued");
    CHECK(thread.entry == test_thread_entry,
          "spawned thread stores entry pointer");
    CHECK(thread.stack_phys == (uintptr_t)thread_stack,
          "spawned thread stores physical stack base");
    CHECK(thread.stack_top ==
              (uintptr_t)thread_stack + KERNEL_THREAD_STACK_SIZE,
          "spawned thread stores exact stack top");
    CHECK(thread.task.kernel_stack_top == thread.stack_top,
          "spawned scheduler task publishes exact kernel stack top");
    CHECK(thread.task.context.rsp == thread.stack_top - 8U,
          "spawned context rsp is stack top minus 8");
    CHECK(thread.task.context.rip ==
              (uintptr_t)sched_kernel_thread_trampoline,
          "spawned context rip is first-entry trampoline");
    if (thread.stack_top != 0U) {
        CHECK(*(uintptr_t *)(thread.stack_top - 8U) ==
                  (uintptr_t)test_thread_entry,
              "entry pointer is stored for trampoline pop");
    } else {
        CHECK(0, "entry pointer storage requires valid stack");
    }
}

static void test_status_string(void) {
    CHECK(strcmp(scheduler_status_string(SCHEDULER_NO_MEMORY),
                 "SCHEDULER_NO_MEMORY") == 0,
          "SCHEDULER_NO_MEMORY status string is exact");
}

int main(void) {
    test_spawn_invalid_arguments();
    test_spawn_allocation_failure();
    test_spawn_overflow_rollback();
    test_spawn_success();
    test_status_string();

    if (failures != 0) {
        fprintf(stderr, "Scheduler thread host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Scheduler thread host tests: ALL OK");
    return 0;
}

#endif
