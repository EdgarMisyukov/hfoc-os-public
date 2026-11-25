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
          "scheduler initializes for wait host test");
    reset_tasks();
    CHECK(sched_wait_queue_init(&wait_queue) == SCHEDULER_OK,
          "wait queue initializes for wait host test");
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "wait switch hook installs with no previous hook");
    sched_host_test_set_now_ticks(0U);
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "wait running A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "wait running A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects wait running A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "wait running A is published as current");
    switch_calls = 0U;
}

static void start_running_b(void) {
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "wait running B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "wait running B enqueues before idle selection");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects wait running B");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "wait running B is published as current");
    switch_calls = 0U;
}

static void test_queue_init_and_empty_wake(void) {
    scheduler_ready();
    CHECK(sched_wait_queue_init(NULL) == SCHEDULER_NULL,
          "null wait queue init returns NULL");
    CHECK(wait_queue.head == NULL && wait_queue.tail == NULL &&
              wait_queue.count == 0U,
          "initialized wait queue is empty");
    CHECK(sched_wake_one(&wait_queue) == SCHEDULER_EMPTY,
          "empty wake_one returns EMPTY");
    CHECK(sched_wake_all(&wait_queue) == SCHEDULER_EMPTY,
          "empty wake_all returns EMPTY");
}

static void test_invalid_wait(void) {
    sched_host_test_reset();
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_NOT_INITIALIZED,
          "wait before init returns NOT_INITIALIZED");

    scheduler_ready();
    CHECK(sched_wait(NULL, 0U) == SCHEDULER_NULL,
          "wait with null queue returns NULL");
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_BAD_STATE,
          "idle wait with no current returns BAD_STATE");
}

static void test_wait_blocks_and_wake_one(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "running A blocks on wait queue");
    CHECK(task_a.state == TASK_STATE_BLOCKED,
          "blocked A publishes TASK_STATE_BLOCKED");
    CHECK(task_a.wait_queue == &wait_queue,
          "blocked A links to wait queue");
    CHECK(wait_queue.head == &task_a && wait_queue.count == 1U,
          "wait queue contains A once");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "blocking clears current task");
    CHECK(switch_calls == 1U, "wait performs one switch to idle");
    CHECK(sched_count() == 0U, "blocked A leaves run queue empty");

    CHECK(sched_wake_one(&wait_queue) == SCHEDULER_OK,
          "wake_one returns OK");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "wake_one publishes A as RUNNABLE and queued");
    CHECK(task_a.wait_queue == NULL,
          "wake_one unlinks A from wait queue");
    CHECK(wait_queue.head == NULL && wait_queue.count == 0U,
          "wake_one empties wait queue");
    CHECK(sched_count() == 1U, "wake_one enqueues A into run queue");
}

static void test_wait_wake_all(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "running A blocks on wake_all queue");
    start_running_b();
    CHECK(sched_wait(&wait_queue, 0U) == SCHEDULER_OK,
          "running B blocks on wake_all queue");
    CHECK(wait_queue.count == 2U, "wake_all queue contains two tasks");

    CHECK(sched_wake_all(&wait_queue) == SCHEDULER_OK,
          "wake_all returns OK");
    CHECK(wait_queue.count == 0U, "wake_all empties wait queue");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "wake_all makes A RUNNABLE");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "wake_all makes B RUNNABLE");
    CHECK(sched_count() == 2U, "wake_all enqueues both tasks");
}

static void test_timeout_expiry(void) {
    scheduler_ready();
    start_running_a();
    sched_host_test_set_now_ticks(10U);

    CHECK(sched_wait(&wait_queue, 5U) == SCHEDULER_OK,
          "running A waits with five-tick timeout");
    CHECK(task_a.wake_ticks == 15U,
          "wait stores saturated deadline of 15 ticks");
    CHECK(task_a.state == TASK_STATE_BLOCKED,
          "timed wait blocks A before deadline");

    sched_host_test_set_now_ticks(14U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick before deadline returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_BLOCKED &&
              wait_queue.count == 1U,
          "pre-deadline tick leaves A blocked");

    sched_host_test_set_now_ticks(15U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick at deadline returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "deadline tick wakes A as RUNNABLE");
    CHECK(wait_queue.count == 0U, "deadline tick empties wait queue");
    CHECK(sched_count() == 1U, "deadline tick enqueues A");
}

static void test_sleep_timeout(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_sleep(0U) == SCHEDULER_OK,
          "sleep zero returns OK without blocking");
    CHECK(task_a.state == TASK_STATE_RUNNING,
          "sleep zero leaves A running");

    sched_host_test_set_now_ticks(0U);
    CHECK(sched_sleep(5U) == SCHEDULER_OK,
          "running A sleeps for five ticks");
    CHECK(task_a.state == TASK_STATE_BLOCKED,
          "sleep blocks A");
    CHECK(task_a.wake_ticks == 5U, "sleep stores five-tick deadline");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "sleep clears current task");

    sched_host_test_set_now_ticks(5U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick at sleep deadline returns BAD_STATE");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "sleep deadline wakes A as RUNNABLE");
    CHECK(sched_count() == 1U, "sleep deadline enqueues A");
}

static void test_concurrent_sleep_timeouts(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "concurrent sleep successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "concurrent sleep successor B enqueues");
    CHECK(sched_yield() == SCHEDULER_OK &&
              sched_current(&current) == SCHEDULER_OK &&
              current == &task_b,
          "concurrent sleep selects B");

    CHECK(sched_sleep(3U) == SCHEDULER_OK,
          "concurrent sleep B blocks with timeout");
    CHECK(sched_current(&current) == SCHEDULER_OK &&
              current == &task_a,
          "concurrent sleep B hands off to A");
    CHECK(sched_sleep(5U) == SCHEDULER_OK,
          "concurrent sleep A blocks with timeout");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "concurrent sleep leaves idle with no current");

    sched_host_test_set_now_ticks(3U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick wakes concurrent sleep B at deadline");
    CHECK(task_b.state == TASK_STATE_RUNNABLE && task_b.queued,
          "concurrent sleep B is RUNNABLE");
    CHECK(task_a.state == TASK_STATE_BLOCKED,
          "concurrent sleep A remains blocked");

    sched_host_test_set_now_ticks(5U);
    CHECK(sched_on_tick() == SCHEDULER_BAD_STATE,
          "idle tick wakes concurrent sleep A at deadline");
    CHECK(task_a.state == TASK_STATE_RUNNABLE && task_a.queued,
          "concurrent sleep A is RUNNABLE");
    CHECK(sched_count() == 2U,
          "concurrent sleep deadlines enqueue both tasks");
}

int main(void) {
    test_queue_init_and_empty_wake();
    test_invalid_wait();
    test_wait_blocks_and_wake_one();
    test_wait_wake_all();
    test_timeout_expiry();
    test_sleep_timeout();
    test_concurrent_sleep_timeouts();

    if (failures != 0) {
        fprintf(stderr, "Scheduler wait host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler wait host tests: ALL OK");
    return 0;
}

#endif
