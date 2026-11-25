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
    CHECK(sched_init() == SCHEDULER_OK,
          "scheduler initializes for tick host test");
    reset_tasks();
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "tick switch hook installs with no previous hook");
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "tick running task A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "tick running task A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects tick running task A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "tick running task A is published as running");
    CHECK(switch_calls == 1U, "idle-to-tick-A switch recorded once");
}

static void test_uninitialized_tick(void) {
    sched_host_test_reset();
    CHECK(sched_on_tick() == SCHEDULER_NOT_INITIALIZED,
          "tick before scheduler init returns NOT_INITIALIZED");
}

static void test_tick_preemption_selection(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "tick successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "tick successor B enqueues after A");

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "tick preempts running A to FIFO successor B");
    CHECK(switch_calls == 2U, "tick preemption switch recorded once");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "tick preempted A is RUNNABLE and queued");
    CHECK(task_b.state == TASK_STATE_RUNNING && !task_b.queued,
          "tick successor B is RUNNING and dequeued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "tick preemption publishes B as current");
    CHECK(switch_current.rsp == task_a.context.rsp &&
              switch_current.rip == task_a.context.rip,
          "tick preemption saves running A context");
    CHECK(switch_next.rsp == task_b.context.rsp &&
              switch_next.rip == task_b.context.rip,
          "tick preemption restores successor B context");
}

static void test_invalid_context_no_mutation(void) {
    scheduler_ready();
    start_running_a();
    task_b.context.rsp = 0U;
    task_b.context.rip = (uintptr_t)0x2000U;
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "invalid tick successor B enqueues after A");
    const unsigned int before = switch_calls;

    CHECK(sched_on_tick() == SCHEDULER_BAD_CONTEXT,
          "tick rejects invalid successor context");
    CHECK(switch_calls == before, "invalid tick successor performs no switch");
    CHECK(sched_count() == 1U, "invalid tick successor leaves queue unchanged");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "invalid tick successor stays RUNNABLE and queued");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "invalid tick successor leaves A RUNNING");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "invalid tick successor leaves A as current");
}

static void test_idle_tick_no_switch(void) {
    scheduler_ready();
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick with no current returns BAD_STATE");
    CHECK(switch_calls == 0U, "idle tick performs no switch");
}

static void test_empty_queue_tick_no_switch(void) {
    scheduler_ready();
    start_running_a();
    CHECK(sched_on_tick() == SCHEDULER_OK,
          "tick with no FIFO successor returns OK");
    CHECK(switch_calls == 1U, "tick with no successor performs no switch");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "tick with no successor leaves A RUNNING");
}

static void test_tick_during_yield_ignored(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "yield-reentrancy successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "yield-reentrancy successor B enqueues after A");
    sched_host_test_set_yield_in_progress(true);

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "tick during cooperative yield is ignored");
    CHECK(switch_calls == 1U, "tick during yield performs no switch");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "tick during yield leaves A RUNNING");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "tick during yield leaves B RUNNABLE");
    sched_host_test_set_yield_in_progress(false);
}

int main(void) {
    test_uninitialized_tick();
    test_tick_preemption_selection();
    test_invalid_context_no_mutation();
    test_idle_tick_no_switch();
    test_empty_queue_tick_no_switch();
    test_tick_during_yield_ignored();

    if (failures != 0) {
        fprintf(stderr, "Scheduler tick host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler tick host tests: ALL OK");
    return 0;
}

#endif
