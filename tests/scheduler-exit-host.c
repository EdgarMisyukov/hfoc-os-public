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

static struct context_frame switch_current;
static struct context_frame switch_next;
static struct scheduler_task task_a;
static struct scheduler_task task_b;
static struct scheduler_task *current;
static unsigned int switch_calls;
static uint32_t stack_frees;
static uintptr_t freed_phys;
static uint32_t freed_pages;

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

static void record_stack_free(uintptr_t physical_address,
                              uint32_t page_count)
{
    freed_phys = physical_address;
    freed_pages = page_count;
    ++stack_frees;
}

static void reset_tasks(void) {
    memset(&task_a, 0, sizeof(task_a));
    memset(&task_b, 0, sizeof(task_b));
    task_a.state = TASK_STATE_CREATED;
    task_b.state = TASK_STATE_CREATED;
    memset(&switch_current, 0, sizeof(switch_current));
    memset(&switch_next, 0, sizeof(switch_next));
    current = NULL;
    switch_calls = 0U;
    stack_frees = 0U;
    freed_phys = 0U;
    freed_pages = 0U;
}

static void scheduler_ready(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK,
          "scheduler initializes for exit host test");
    reset_tasks();
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "exit switch hook installs with no previous hook");
    CHECK(sched_host_test_set_stack_free_hook(record_stack_free) == NULL,
          "exit stack-free hook installs with no previous hook");
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "exit running A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "exit running A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects exit running A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "exit running A is published as current");
    switch_calls = 0U;
}

static void start_runnable_b(void) {
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "exit successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "exit successor B enqueues");
}

static void test_exit_validation(void) {
    sched_host_test_reset();
    CHECK(sched_exit() == SCHEDULER_NOT_INITIALIZED,
          "exit before scheduler init returns NOT_INITIALIZED");

    scheduler_ready();
    CHECK(sched_exit() == SCHEDULER_BAD_STATE,
          "idle exit with no current returns BAD_STATE");

    start_running_a();
    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_exit() == SCHEDULER_BAD_STATE,
          "exit during mutation returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "exit during mutation leaves A running");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "exit during mutation leaves current unchanged");
    CHECK(switch_calls == 0U,
          "exit during mutation performs no switch");
    sched_host_test_set_yield_in_progress(false);
}

static void test_exit_successor_handoff(void) {
    scheduler_ready();
    start_running_a();
    task_a.stack_phys = (uintptr_t)0x4000U;
    task_a.stack_pages = KERNEL_THREAD_STACK_PAGES;
    start_runnable_b();

    CHECK(sched_exit() == SCHEDULER_OK,
          "running A exits successfully");
    CHECK(task_a.state == TASK_STATE_EXITED,
          "exiting A publishes TASK_STATE_EXITED");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "exit selects successor B as current");
    CHECK(sched_count() == 0U,
          "exit removes A from run queue");
    CHECK(stack_frees == 0U,
          "exit does not free its own stack immediately");

    CHECK(sched_yield() == SCHEDULER_OK,
          "successor yield performs handoff");
    CHECK(stack_frees == 1U,
          "successor handoff reclaims exited stack once");
    CHECK(freed_phys == (uintptr_t)0x4000U &&
              freed_pages == KERNEL_THREAD_STACK_PAGES,
          "successor handoff frees exact exited stack");
}

static void test_exit_idle_handoff(void) {
    scheduler_ready();
    start_running_a();
    task_a.stack_phys = (uintptr_t)0x5000U;
    task_a.stack_pages = KERNEL_THREAD_STACK_PAGES;

    CHECK(sched_exit() == SCHEDULER_OK,
          "running A exits to idle successfully");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "exit to idle clears current");
    CHECK(stack_frees == 0U,
          "exit to idle defers stack reclamation");

    sched_reclaim_deferred();
    CHECK(stack_frees == 1U,
          "idle handoff reclaims exited stack");
    CHECK(freed_phys == (uintptr_t)0x5000U &&
              freed_pages == KERNEL_THREAD_STACK_PAGES,
          "idle handoff frees exact exited stack");
}

static void test_exit_no_stack(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_exit() == SCHEDULER_OK,
          "running A without owned stack exits");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "no-stack exit clears current");
    sched_reclaim_deferred();
    CHECK(stack_frees == 0U,
          "no-stack exit performs no stack free");
}

static void test_exit_invalid_context_rollback(void) {
    scheduler_ready();
    start_running_a();
    task_a.stack_phys = (uintptr_t)0x6000U;
    task_a.stack_pages = KERNEL_THREAD_STACK_PAGES;
    task_b.context.rsp = 0U;
    task_b.context.rip = (uintptr_t)0x2000U;
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "invalid exit successor B enqueues");
    const unsigned int before_switches = switch_calls;

    CHECK(sched_exit() == SCHEDULER_BAD_CONTEXT,
          "exit rejects invalid successor context");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "invalid exit successor leaves A running");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "invalid exit successor leaves A as current");
    CHECK(sched_count() == 1U,
          "invalid exit successor leaves run queue unchanged");
    CHECK(switch_calls == before_switches,
          "invalid exit successor performs no switch");
    sched_reclaim_deferred();
    CHECK(stack_frees == 0U,
          "invalid exit successor defers no stack");
}

int main(void) {
    test_exit_validation();
    test_exit_successor_handoff();
    test_exit_idle_handoff();
    test_exit_no_stack();
    test_exit_invalid_context_rollback();

    if (failures != 0) {
        fprintf(stderr, "Scheduler exit host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler exit host tests: ALL OK");
    return 0;
}

#endif
