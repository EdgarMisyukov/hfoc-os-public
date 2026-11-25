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

static struct scheduler_task task_a;
static struct scheduler_task task_b;
static struct scheduler_task task_c;
static struct scheduler_task *next_task;

static void reset_task(struct scheduler_task *task, enum task_state state) {
    memset(task, 0, sizeof(*task));
    task->state = state;
}

static void scheduler_ready(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK, "scheduler initializes");
}

static void test_uninitialized_and_null(void) {
    sched_host_test_reset();
    reset_task(&task_a, TASK_STATE_CREATED);

    CHECK(sched_enqueue(&task_a) == SCHEDULER_NOT_INITIALIZED,
          "enqueue before init returns NOT_INITIALIZED");
    CHECK(sched_next(&next_task) == SCHEDULER_NOT_INITIALIZED,
          "next before init returns NOT_INITIALIZED");
    CHECK(sched_transition(&task_a, TASK_STATE_RUNNABLE) ==
              SCHEDULER_NOT_INITIALIZED,
          "transition before init returns NOT_INITIALIZED");

    scheduler_ready();
    CHECK(sched_enqueue(NULL) == SCHEDULER_NULL,
          "null enqueue returns NULL");
    CHECK(sched_next(NULL) == SCHEDULER_NULL,
          "null next output returns NULL");
    CHECK(sched_transition(NULL, TASK_STATE_RUNNABLE) == SCHEDULER_NULL,
          "null transition returns NULL");
}

static void test_fifo_order(void) {
    scheduler_ready();
    reset_task(&task_a, TASK_STATE_CREATED);
    reset_task(&task_b, TASK_STATE_CREATED);
    reset_task(&task_c, TASK_STATE_CREATED);

    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK, "enqueue A succeeds");
    CHECK(task_a.state == TASK_STATE_RUNNABLE, "enqueue A publishes RUNNABLE");
    CHECK(task_a.queued, "enqueue A marks task queued");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK, "enqueue B succeeds");
    CHECK(sched_enqueue(&task_c) == SCHEDULER_OK, "enqueue C succeeds");
    CHECK(sched_count() == 3U, "FIFO queue count is three");

    CHECK(sched_next(&next_task) == SCHEDULER_OK &&
              next_task == &task_a,
          "FIFO next returns A first");
    CHECK(task_a.state == TASK_STATE_RUNNING, "next A publishes RUNNING");
    CHECK(!task_a.queued, "next A removes queued flag");
    CHECK(sched_next(&next_task) == SCHEDULER_OK &&
              next_task == &task_b,
          "FIFO next returns B second");
    CHECK(sched_next(&next_task) == SCHEDULER_OK &&
              next_task == &task_c,
          "FIFO next returns C third");
    CHECK(sched_count() == 0U, "FIFO queue is empty after three nexts");
    CHECK(sched_next(&next_task) == SCHEDULER_EMPTY,
          "empty FIFO next returns EMPTY");
}

static void test_double_enqueue_rejection(void) {
    scheduler_ready();
    reset_task(&task_a, TASK_STATE_CREATED);

    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK, "first enqueue succeeds");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_DOUBLE_ENQUEUE,
          "second enqueue returns DOUBLE_ENQUEUE");
    CHECK(sched_count() == 1U, "double enqueue leaves count at one");
    CHECK(task_a.state == TASK_STATE_RUNNABLE,
          "double enqueue leaves RUNNABLE state");
    CHECK(task_a.queued, "double enqueue leaves queued flag set");

    CHECK(sched_next(&next_task) == SCHEDULER_OK &&
              next_task == &task_a,
          "queued task is dequeued after rejection");
    CHECK(sched_transition(&task_a, TASK_STATE_RUNNABLE) == SCHEDULER_OK,
          "running task transitions back to RUNNABLE");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "dequeued task can be enqueued again");
}

static void test_enqueue_state_rules(void) {
    scheduler_ready();

    reset_task(&task_a, TASK_STATE_RUNNING);
    CHECK(sched_enqueue(&task_a) == SCHEDULER_BAD_STATE,
          "running task enqueue returns BAD_STATE");

    reset_task(&task_a, TASK_STATE_RUNNABLE);
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "runnable task enqueue succeeds");
    CHECK(task_a.state == TASK_STATE_RUNNABLE,
          "runnable task enqueue keeps RUNNABLE state");

    scheduler_ready();
    reset_task(&task_a, TASK_STATE_EXITED);
    CHECK(sched_enqueue(&task_a) == SCHEDULER_BAD_STATE,
          "exited task enqueue returns BAD_STATE");

    scheduler_ready();
    reset_task(&task_a, TASK_STATE_BLOCKED);
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "blocked task enqueue succeeds");
    CHECK(task_a.state == TASK_STATE_RUNNABLE,
          "blocked task enqueue publishes RUNNABLE");
}

static void expect_transition(enum task_state from, enum task_state to,
                              enum scheduler_status expected)
{
    char message[160];
    scheduler_ready();
    reset_task(&task_a, from);
    const enum scheduler_status status =
        sched_transition(&task_a, to);
    (void)snprintf(message, sizeof(message),
                   "transition %s -> %s is %s",
                   task_state_string(from), task_state_string(to),
                   expected == SCHEDULER_OK ? "allowed" : "rejected");
    CHECK(status == expected, message);
    CHECK(task_a.state == (expected == SCHEDULER_OK ? to : from),
          "transition publishes only successful next state");
}

static void test_transition_matrix(void) {
    expect_transition(TASK_STATE_CREATED, TASK_STATE_RUNNABLE, SCHEDULER_OK);
    expect_transition(TASK_STATE_CREATED, TASK_STATE_EXITED, SCHEDULER_OK);
    expect_transition(TASK_STATE_CREATED, TASK_STATE_RUNNING,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_CREATED, TASK_STATE_BLOCKED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_CREATED, TASK_STATE_CREATED,
                      SCHEDULER_BAD_STATE);

    expect_transition(TASK_STATE_RUNNABLE, TASK_STATE_RUNNING, SCHEDULER_OK);
    expect_transition(TASK_STATE_RUNNABLE, TASK_STATE_BLOCKED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_RUNNABLE, TASK_STATE_EXITED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_RUNNABLE, TASK_STATE_CREATED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_RUNNABLE, TASK_STATE_RUNNABLE,
                      SCHEDULER_BAD_STATE);

    expect_transition(TASK_STATE_RUNNING, TASK_STATE_RUNNABLE, SCHEDULER_OK);
    expect_transition(TASK_STATE_RUNNING, TASK_STATE_BLOCKED, SCHEDULER_OK);
    expect_transition(TASK_STATE_RUNNING, TASK_STATE_EXITED, SCHEDULER_OK);
    expect_transition(TASK_STATE_RUNNING, TASK_STATE_CREATED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_RUNNING, TASK_STATE_RUNNING,
                      SCHEDULER_BAD_STATE);

    expect_transition(TASK_STATE_BLOCKED, TASK_STATE_RUNNABLE, SCHEDULER_OK);
    expect_transition(TASK_STATE_BLOCKED, TASK_STATE_EXITED, SCHEDULER_OK);
    expect_transition(TASK_STATE_BLOCKED, TASK_STATE_RUNNING,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_BLOCKED, TASK_STATE_CREATED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_BLOCKED, TASK_STATE_BLOCKED,
                      SCHEDULER_BAD_STATE);

    expect_transition(TASK_STATE_EXITED, TASK_STATE_CREATED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_EXITED, TASK_STATE_RUNNABLE,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_EXITED, TASK_STATE_RUNNING,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_EXITED, TASK_STATE_BLOCKED,
                      SCHEDULER_BAD_STATE);
    expect_transition(TASK_STATE_EXITED, TASK_STATE_EXITED,
                      SCHEDULER_BAD_STATE);
}

static void test_status_strings(void) {
    CHECK(strcmp(scheduler_status_string(SCHEDULER_OK), "SCHEDULER_OK") == 0,
          "SCHEDULER_OK status string is exact");
    CHECK(strcmp(task_state_string(TASK_STATE_RUNNING),
                 "TASK_STATE_RUNNING") == 0,
          "TASK_STATE_RUNNING string is exact");
}

int main(void) {
    test_uninitialized_and_null();
    test_fifo_order();
    test_double_enqueue_rejection();
    test_enqueue_state_rules();
    test_transition_matrix();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Scheduler host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Scheduler host tests: ALL OK");
    return 0;
}

#endif
