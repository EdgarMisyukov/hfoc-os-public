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
          "scheduler initializes for discipline host test");
    reset_tasks();
    CHECK(sched_wait_queue_init(&wait_queue) == SCHEDULER_OK,
          "wait queue initializes for discipline host test");
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "discipline switch hook installs with no previous hook");
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "discipline running A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "discipline running A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects discipline running A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "discipline running A is published as current");
    switch_calls = 0U;
}

static void start_running_b(void) {
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "discipline running B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "discipline running B enqueues before idle selection");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects discipline running B");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "discipline running B is published as current");
    switch_calls = 0U;
}

static void block_running_a(void) {
    struct scheduler_task *published = NULL;
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "discipline running A blocks on wait queue");
    CHECK(sched_current(&published) == SCHEDULER_OK &&
              published == NULL && wait_queue.head == &task_a,
          "discipline blocked A is current-cleared and queued");
    switch_calls = 0U;
}

static void block_running_b(void) {
    struct scheduler_task *published = NULL;
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "discipline running B blocks on wait queue");
    CHECK(sched_current(&published) == SCHEDULER_OK &&
              published == NULL && wait_queue.head == &task_a,
          "discipline blocked B links after A");
    CHECK(wait_queue.count == 2U,
          "discipline wait queue contains A and B");
    switch_calls = 0U;
}

static void test_enqueue_reentry(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "re-entry successor B context initializes");
    const uint32_t before_count = sched_count();

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_enqueue(&task_b) == SCHEDULER_BAD_STATE,
          "enqueue during mutation returns BAD_STATE");
    CHECK(task_b.state == TASK_STATE_CREATED && !task_b.queued,
          "enqueue during mutation leaves B unpublished");
    CHECK(sched_count() == before_count,
          "enqueue during mutation leaves run-queue count unchanged");
    sched_host_test_set_yield_in_progress(false);

    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "enqueue after mutation release succeeds");
}

static void test_next_reentry(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "next re-entry successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "next re-entry successor B enqueues");
    const uint32_t before_count = sched_count();

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_next(&current) == SCHEDULER_BAD_STATE,
          "next during mutation returns BAD_STATE");
    CHECK(sched_count() == before_count,
          "next during mutation leaves run-queue count unchanged");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "next during mutation leaves B RUNNABLE and queued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "next during mutation leaves A as current");
    sched_host_test_set_yield_in_progress(false);
}

static void test_transition_reentry(void) {
    scheduler_ready();
    start_running_a();

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_transition(&task_a, TASK_STATE_RUNNABLE) ==
              SCHEDULER_BAD_STATE,
          "transition during mutation returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_RUNNING,
          "transition during mutation leaves state unchanged");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "transition during mutation leaves current unchanged");
    sched_host_test_set_yield_in_progress(false);

    CHECK(sched_transition(&task_a, TASK_STATE_RUNNABLE) == SCHEDULER_OK,
          "transition after mutation release succeeds");
}

static void test_wait_reentry(void) {
    scheduler_ready();
    start_running_a();
    const unsigned int before_switches = switch_calls;

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_BAD_STATE,
          "wait during mutation returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "wait during mutation leaves A RUNNING and unqueued");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "wait during mutation leaves current unchanged");
    CHECK(wait_queue.head == NULL && wait_queue.count == 0U,
          "wait during mutation leaves wait queue empty");
    CHECK(switch_calls == before_switches,
          "wait during mutation performs no switch");
    sched_host_test_set_yield_in_progress(false);
}

static void test_wake_reentry(void) {
    scheduler_ready();
    start_running_a();
    block_running_a();
    start_running_b();
    block_running_b();

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_wake_one(&wait_queue) == SCHEDULER_BAD_STATE,
          "wake_one during mutation returns BAD_STATE");
    CHECK(wait_queue.head == &task_a && wait_queue.count == 2U,
          "wake_one during mutation leaves wait queue unchanged");
    CHECK(task_a.state == TASK_STATE_BLOCKED && !task_a.queued,
          "wake_one during mutation leaves A blocked");
    CHECK(task_b.state == TASK_STATE_BLOCKED && !task_b.queued,
          "wake_one during mutation leaves B blocked");
    CHECK(sched_count() == 0U,
          "wake_one during mutation leaves run queue empty");
    sched_host_test_set_yield_in_progress(false);

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_wake_all(&wait_queue) == SCHEDULER_BAD_STATE,
          "wake_all during mutation returns BAD_STATE");
    CHECK(wait_queue.head == &task_a && wait_queue.count == 2U,
          "wake_all during mutation leaves wait queue unchanged");
    CHECK(task_a.state == TASK_STATE_BLOCKED && !task_a.queued,
          "wake_all during mutation leaves A blocked");
    CHECK(task_b.state == TASK_STATE_BLOCKED && !task_b.queued,
          "wake_all during mutation leaves B blocked");
    CHECK(sched_count() == 0U,
          "wake_all during mutation leaves run queue empty");
    sched_host_test_set_yield_in_progress(false);
}

static void test_tick_reentry_ignored(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "tick re-entry successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "tick re-entry successor B enqueues");

    sched_host_test_set_yield_in_progress(true);
    CHECK(sched_on_tick() == SCHEDULER_OK,
          "tick during mutation returns OK without preemption");
    CHECK(switch_calls == 0U,
          "tick during mutation performs no switch");
    CHECK(task_a.state == TASK_STATE_RUNNING && !task_a.queued,
          "tick during mutation leaves A running");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "tick during mutation leaves B runnable");
    sched_host_test_set_yield_in_progress(false);
}

int main(void) {
    test_enqueue_reentry();
    test_next_reentry();
    test_transition_reentry();
    test_wait_reentry();
    test_wake_reentry();
    test_tick_reentry_ignored();

    if (failures != 0) {
        fprintf(stderr,
                "Scheduler queue mutation discipline host tests: "
                "%d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler queue mutation discipline host tests: ALL OK");
    return 0;
}

#endif
