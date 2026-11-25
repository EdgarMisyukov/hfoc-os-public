#include "context.h"
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

static struct context_frame switch_current;
static struct context_frame switch_next;
static struct scheduler_task task_a;
static struct scheduler_task task_b;
static struct scheduler_task *current;
static unsigned int switch_calls;

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

static void reset_tasks(void) {
    memset(&task_a, 0, sizeof(task_a));
    memset(&task_b, 0, sizeof(task_b));
    task_a.state = TASK_STATE_CREATED;
    task_b.state = TASK_STATE_CREATED;
    memset(&switch_current, 0, sizeof(switch_current));
    memset(&switch_next, 0, sizeof(switch_next));
    current = NULL;
    switch_calls = 0U;
}

static void scheduler_ready(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK, "scheduler initializes for yield");
    reset_tasks();
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "switch hook installs with no previous hook");
}

static void test_idle_empty_queue(void) {
    scheduler_ready();
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle yield with empty queue returns OK");
    CHECK(switch_calls == 0U, "idle empty yield performs no switch");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "idle context has no current task");
}

static void test_invalid_context_no_mutation(void) {
    scheduler_ready();
    task_b.context.rsp = 0U;
    task_b.context.rip = (uintptr_t)0x1000U;
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "invalid-context B task enqueues");
    CHECK(sched_yield() == SCHEDULER_BAD_CONTEXT,
          "invalid next context returns BAD_CONTEXT");
    CHECK(switch_calls == 0U, "invalid context performs no switch");
    CHECK(sched_count() == 1U, "invalid context leaves queue unchanged");
    CHECK(task_b.state == TASK_STATE_RUNNABLE,
          "invalid context leaves B RUNNABLE");
    CHECK(task_b.queued, "invalid context leaves B queued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "invalid context does not publish a running task");
}

static void test_thread_to_thread(void) {
    scheduler_ready();
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "thread A context initializes");
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "thread B context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK, "thread A enqueues");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK, "thread B enqueues");

    CHECK(sched_yield() == SCHEDULER_OK,
          "idle yield to thread A returns OK");
    CHECK(switch_calls == 1U, "idle-to-thread switch recorded once");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "thread A is published as running");

    CHECK(sched_yield() == SCHEDULER_OK,
          "thread A yield to thread B returns OK");
    CHECK(switch_calls == 2U, "thread-to-thread switch recorded once");
    CHECK(task_a.state == TASK_STATE_RUNNABLE &&
              task_a.queued,
          "yielding task A is RUNNABLE and queued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "thread B is published as running");
    CHECK(switch_next.rsp == task_b.context.rsp &&
              switch_next.rip == task_b.context.rip,
          "thread-to-thread switch restores thread B context");
}

static void test_thread_to_idle(void) {
    scheduler_ready();
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "idle-selection A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "idle-selection A task enqueues");

    CHECK(sched_yield() == SCHEDULER_OK,
          "idle yield to thread A returns OK");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "idle-selection A is running before yield");

    CHECK(sched_yield() == SCHEDULER_OK,
          "thread A yield to idle returns OK");
    CHECK(switch_calls == 2U, "thread-to-idle switch recorded once");
    CHECK(task_a.state == TASK_STATE_RUNNABLE &&
              task_a.queued,
          "thread-to-idle leaves A RUNNABLE and queued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "idle context has no current task after thread yield");
    CHECK(switch_next.rsp == 0U && switch_next.rip == 0U,
          "thread-to-idle records the scheduler idle context");
}

int main(void) {
    test_idle_empty_queue();
    test_invalid_context_no_mutation();
    test_thread_to_thread();
    test_thread_to_idle();

    if (failures != 0) {
        fprintf(stderr, "Scheduler yield host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler yield host tests: ALL OK");
    return 0;
}

#endif
