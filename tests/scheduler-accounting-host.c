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
static struct scheduler_wait_queue wait_queue;
static struct scheduler_task *current;
static struct scheduler_snapshot snapshot;
static struct scheduler_task_accounting accounting;
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
          "scheduler initializes for accounting host test");
    reset_tasks();
    CHECK(sched_wait_queue_init(&wait_queue) == SCHEDULER_OK,
          "accounting wait queue initializes");
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "accounting switch hook installs with no previous hook");
    sched_host_test_set_now_ticks(0U);
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "accounting running A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "accounting running A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects accounting running A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "accounting A is published as current");
    switch_calls = 0U;
}

static void test_validation(void) {
    sched_host_test_reset();
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_NOT_INITIALIZED,
          "snapshot before init returns NOT_INITIALIZED");
    CHECK(sched_task_accounting(&task_a, &accounting) ==
              SCHEDULER_NOT_INITIALIZED,
          "task accounting before init returns NOT_INITIALIZED");

    scheduler_ready();
    CHECK(sched_snapshot(NULL) == SCHEDULER_NULL,
          "snapshot with null output returns NULL");
    CHECK(sched_task_accounting(NULL, &accounting) == SCHEDULER_NULL,
          "task accounting with null task returns NULL");
    CHECK(sched_task_accounting(&task_a, NULL) == SCHEDULER_NULL,
          "task accounting with null output returns NULL");
}

static void test_snapshot_after_selection(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK,
          "snapshot publishes after idle selection");
    CHECK(snapshot.ticks == 0U, "snapshot reports host now ticks");
    CHECK(snapshot.switch_count == 1U,
          "snapshot records idle-to-task switch");
    CHECK(snapshot.runnable_count == 0U,
          "snapshot reports no runnable tasks");
    CHECK(snapshot.blocked_count == 0U,
          "snapshot reports no blocked tasks");
    CHECK(snapshot.timeout_count == 0U,
          "snapshot reports no timeout tasks");
    CHECK(snapshot.current == &task_a,
          "snapshot reports A as current");

    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK,
          "task accounting publishes after selection");
    CHECK(accounting.run_count == 1U,
          "selection charges one run");
    CHECK(accounting.tick_count == 0U,
          "selection does not charge a tick");
    CHECK(accounting.last_run_tick == 0U,
          "selection stores current tick");
}

static void test_tick_charging(void) {
    scheduler_ready();
    start_running_a();
    sched_host_test_set_now_ticks(7U);

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "running A tick succeeds");
    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK,
          "tick task accounting publishes");
    CHECK(accounting.tick_count == 1U,
          "running tick charges current task once");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK,
          "snapshot after tick publishes");
    CHECK(snapshot.ticks == 7U, "snapshot after tick reports now");
}

static void test_tick_preemption_accounting(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "accounting tick successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "accounting tick successor B enqueues");
    sched_host_test_set_now_ticks(9U);

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "tick preempts accounting A to B");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "tick publishes B as current");
    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK &&
              accounting.tick_count == 1U,
          "preempted A is charged for the running tick");
    CHECK(sched_task_accounting(&task_b, &accounting) == SCHEDULER_OK &&
              accounting.run_count == 1U &&
              accounting.last_run_tick == 9U,
          "fresh successor B records one run at current tick");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.switch_count == 2U &&
              snapshot.runnable_count == 1U,
          "tick preemption updates switch and runnable snapshot");
}

static void test_wait_wake_accounting(void) {
    scheduler_ready();
    start_running_a();
    sched_host_test_set_now_ticks(10U);

    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "accounting A waits");
    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK &&
              accounting.block_count == 1U,
          "wait charges one block");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.blocked_count == 1U &&
              snapshot.runnable_count == 0U,
          "wait updates blocked snapshot");

    CHECK(sched_wake_one(&wait_queue) == SCHEDULER_OK,
          "accounting wake_one succeeds");
    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK &&
              accounting.wake_count == 1U,
          "wake charges one wake");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.blocked_count == 0U &&
              snapshot.runnable_count == 1U &&
              snapshot.wake_count == 1U,
          "wake restores blocked count and publishes runnable");
}

static void test_timeout_accounting(void) {
    scheduler_ready();
    start_running_a();
    sched_host_test_set_now_ticks(10U);

    CHECK(sched_wait(&wait_queue, 5U) == SCHEDULER_OK,
          "accounting A waits with timeout");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.blocked_count == 1U &&
              snapshot.timeout_count == 1U,
          "timed wait updates blocked and timeout snapshot");

    sched_host_test_set_now_ticks(14U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick before deadline returns BAD_STATE");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.blocked_count == 1U &&
              snapshot.timeout_count == 1U,
          "pre-deadline tick leaves blocked accounting");

    sched_host_test_set_now_ticks(15U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick at deadline returns BAD_STATE");
    CHECK(sched_task_accounting(&task_a, &accounting) == SCHEDULER_OK &&
              accounting.wake_count == 1U,
          "timeout charges one wake");
    CHECK(sched_snapshot(&snapshot) == SCHEDULER_OK &&
              snapshot.blocked_count == 0U &&
              snapshot.timeout_count == 0U &&
              snapshot.runnable_count == 1U &&
              snapshot.wake_count == 1U,
          "timeout restores blocked count and clears timeout queue");
}

int main(void) {
    test_validation();
    test_snapshot_after_selection();
    test_tick_charging();
    test_tick_preemption_accounting();
    test_wait_wake_accounting();
    test_timeout_accounting();

    if (failures != 0) {
        fprintf(stderr, "Scheduler accounting host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Scheduler accounting host tests: ALL OK");
    return 0;
}

#endif
