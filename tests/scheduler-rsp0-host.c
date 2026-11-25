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

#define IDLE_STACK_TOP UINT64_C(0xffff800000300000)
#define TASK_A_STACK_TOP UINT64_C(0xffff800000100000)
#define TASK_B_STACK_TOP UINT64_C(0xffff800000200000)

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
    task_a.kernel_stack_top = TASK_A_STACK_TOP;
    task_b.kernel_stack_top = TASK_B_STACK_TOP;
    memset(&switch_current, 0, sizeof(switch_current));
    memset(&switch_next, 0, sizeof(switch_next));
    current = NULL;
    switch_calls = 0U;
}

static void scheduler_ready(void) {
    sched_host_test_reset();
    CHECK(sched_init() == SCHEDULER_OK,
          "scheduler initializes for rsp0 host test");
    reset_tasks();
    sched_host_test_set_idle_stack_top(IDLE_STACK_TOP);
    CHECK(sched_host_test_set_switch_hook(record_switch) == NULL,
          "rsp0 switch hook installs with no previous hook");
}

static void start_running_a(void) {
    CHECK(context_frame_init(&task_a.context, TASK_A_STACK_TOP,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "rsp0 task A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "rsp0 task A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "idle selects rsp0 task A");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "rsp0 task A is published as running");
}

static void test_idle_to_task(void) {
    scheduler_ready();
    start_running_a();
    CHECK(sched_host_test_last_rsp0() == TASK_A_STACK_TOP,
          "idle-to-task publishes exact task A rsp0");
    CHECK(switch_calls == 1U, "idle-to-task performs one switch");
}

static void test_task_to_task(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context, TASK_B_STACK_TOP,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "rsp0 task B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "rsp0 task B enqueues after A");

    CHECK(sched_yield() == SCHEDULER_OK,
          "rsp0 task A yields to task B");
    CHECK(sched_host_test_last_rsp0() == TASK_B_STACK_TOP,
          "task-to-task publishes exact task B rsp0");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "rsp0 task B is current after switch");
}

static void test_task_to_idle(void) {
    scheduler_ready();
    start_running_a();

    CHECK(sched_yield() == SCHEDULER_OK,
          "rsp0 task A yields to idle");
    CHECK(sched_host_test_last_rsp0() == IDLE_STACK_TOP,
          "task-to-idle publishes exact idle stack rsp0");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == NULL,
          "rsp0 idle has no current task");
}

static void test_tick_preemption(void) {
    scheduler_ready();
    start_running_a();
    CHECK(context_frame_init(&task_b.context, TASK_B_STACK_TOP,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "rsp0 tick successor B context initializes");
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "rsp0 tick successor B enqueues after A");

    CHECK(sched_on_tick() == SCHEDULER_OK,
          "rsp0 tick preempts A to B");
    CHECK(sched_host_test_last_rsp0() == TASK_B_STACK_TOP,
          "tick preemption publishes exact successor rsp0");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_b,
          "rsp0 successor B is current after tick");
}

static void test_invalid_context_no_mutation(void) {
    scheduler_ready();
    start_running_a();
    task_b.context.rsp = 0U;
    task_b.context.rip = (uintptr_t)0x2000U;
    CHECK(sched_enqueue(&task_b) == SCHEDULER_OK,
          "invalid rsp0 successor B enqueues after A");
    const uintptr_t before = sched_host_test_last_rsp0();
    const unsigned int before_switches = switch_calls;

    CHECK(sched_on_tick() == SCHEDULER_BAD_CONTEXT,
          "rsp0 invalid successor context returns BAD_CONTEXT");
    CHECK(sched_host_test_last_rsp0() == before,
          "invalid successor does not change rsp0");
    CHECK(switch_calls == before_switches,
          "invalid successor performs no switch");
    CHECK(sched_current(&current) == SCHEDULER_OK && current == &task_a,
          "invalid successor leaves task A current");
}

static void test_sched_next_selection(void) {
    scheduler_ready();
    CHECK(context_frame_init(&task_a.context, TASK_A_STACK_TOP,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "rsp0 sched_next task A context initializes");
    CHECK(sched_enqueue(&task_a) == SCHEDULER_OK,
          "rsp0 sched_next task A enqueues");

    CHECK(sched_next(&current) == SCHEDULER_OK &&
              current == &task_a,
          "sched_next selects task A");
    CHECK(sched_host_test_last_rsp0() == TASK_A_STACK_TOP,
          "sched_next publishes exact task A rsp0");
}

int main(void) {
    test_idle_to_task();
    test_task_to_task();
    test_task_to_idle();
    test_tick_preemption();
    test_invalid_context_no_mutation();
    test_sched_next_selection();

    if (failures != 0) {
        fprintf(stderr, "Scheduler TSS rsp0 host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Scheduler TSS rsp0 host tests: ALL OK");
    return 0;
}

#endif
