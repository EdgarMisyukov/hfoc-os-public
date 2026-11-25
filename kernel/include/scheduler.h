#ifndef HFOC_SCHEDULER_H
#define HFOC_SCHEDULER_H

#include "context.h"
#include <stdbool.h>
#include <stdint.h>

#define KERNEL_THREAD_STACK_PAGES 4U
#define KERNEL_THREAD_STACK_SIZE (KERNEL_THREAD_STACK_PAGES * 4096U)

enum task_state {
    TASK_STATE_CREATED = 0,
    TASK_STATE_RUNNABLE = 1,
    TASK_STATE_RUNNING = 2,
    TASK_STATE_BLOCKED = 3,
    TASK_STATE_EXITED = 4,
    /* ITEM-633: job control stop; the task is off every queue until CONT. */
    TASK_STATE_STOPPED = 5,
};

struct scheduler_task_accounting {
    uint64_t run_count;
    uint64_t tick_count;
    uint64_t block_count;
    uint64_t wake_count;
    uint64_t last_run_tick;
};

struct scheduler_wait_queue {
    struct scheduler_task *head;
    struct scheduler_task *tail;
    uint32_t count;
};

struct scheduler_task {
    /* ITEM-627: the architectural frame of the syscall in flight.  Keeping it
       in the task, not on the shared or per-task kernel stack, survives a
       cooperative switch from inside a blocking syscall. */
    uint64_t syscall_arg2;
    uint64_t syscall_arg1;
    uint64_t syscall_arg0;
    uint64_t syscall_rflags;
    uint64_t syscall_rip;
    uint64_t syscall_user_rsp;
    struct scheduler_task *next;
    struct scheduler_task *wait_next;
    struct scheduler_task *blocked_next;
    struct scheduler_task *deferred_next;
    struct scheduler_wait_queue *wait_queue;
    void *process_owner;
    uint64_t wake_ticks;
    enum task_state state;
    bool queued;
    uintptr_t stack_phys;
    uint32_t stack_pages;
    uintptr_t kernel_stack_top;
    struct scheduler_task_accounting accounting;
    uint64_t cpu_tick_limit;
    struct context_frame context;
};

struct scheduler_snapshot {
    uint64_t ticks;
    uint64_t switch_count;
    uint64_t wake_count;
    uint32_t runnable_count;
    uint32_t blocked_count;
    uint32_t timeout_count;
    struct scheduler_task *current;
};

enum scheduler_status {
    SCHEDULER_OK = 0,
    SCHEDULER_NOT_INITIALIZED = 1,
    SCHEDULER_NULL = 2,
    SCHEDULER_BAD_STATE = 3,
    SCHEDULER_DOUBLE_ENQUEUE = 4,
    SCHEDULER_EMPTY = 5,
    SCHEDULER_NO_MEMORY = 6,
    SCHEDULER_BAD_CONTEXT = 7,
};

struct kernel_thread {
    struct scheduler_task task;
    void (*entry)(void);
    uintptr_t stack_phys;
    uintptr_t stack_top;
};

typedef enum scheduler_status (*scheduler_switch_hook)(
    struct context_frame *current,
    struct context_frame *next);
typedef enum scheduler_status (*scheduler_task_limit_hook)(
    struct scheduler_task *task);
/* ITEM-627: publish the selected task's page table before it resumes. */
typedef void (*scheduler_activate_hook)(void *process_owner);
typedef void (*scheduler_stack_free_hook)(uintptr_t physical_address,
                                          uint32_t page_count);

/* ITEM-627: top of the running task's kernel stack for syscall entry. */
extern uint64_t sched_syscall_stack_top;
/* ITEM-627: the task currently selected on this BSP. */
extern struct scheduler_task *sched_running_task;

enum scheduler_status sched_init(void);
bool sched_initialized(void);
scheduler_task_limit_hook sched_set_limit_hook(
    scheduler_task_limit_hook hook);
void sched_set_activate_hook(scheduler_activate_hook hook);
enum scheduler_status sched_enqueue(struct scheduler_task *task);
enum scheduler_status sched_next(struct scheduler_task **task);
enum scheduler_status sched_transition(struct scheduler_task *task,
                                       enum task_state next_state);
enum scheduler_status sched_current(struct scheduler_task **task);
enum scheduler_status sched_yield(void);
enum scheduler_status sched_on_tick(void);
/* ITEM-633: expire wait timeouts from a tick without preempting. */
enum scheduler_status sched_timeout_tick(void);
enum scheduler_status sched_wait_queue_init(
    struct scheduler_wait_queue *queue);
enum scheduler_status sched_wait(struct scheduler_wait_queue *queue,
                                 uint64_t timeout_ticks);
enum scheduler_status sched_wake_one(struct scheduler_wait_queue *queue);
enum scheduler_status sched_wake_all(struct scheduler_wait_queue *queue);
enum scheduler_status sched_sleep(uint64_t ticks);
enum scheduler_status sched_exit(void);
/*
 * ITEM-625: remove a non-current task from the run/wait queues and mark it
 * exited.  This is the only cancellation path; it never runs the cancelled
 * task and never frees its stack, which stays the process owner's until reap.
 */
enum scheduler_status sched_cancel(struct scheduler_task *task);
/* ITEM-633: stop/resume a non-current task without running it. */
enum scheduler_status sched_suspend(struct scheduler_task *task);
enum scheduler_status sched_resume(struct scheduler_task *task);
enum scheduler_status sched_task_accounting(
    const struct scheduler_task *task,
    struct scheduler_task_accounting *accounting);
enum scheduler_status sched_snapshot(struct scheduler_snapshot *snapshot);
enum scheduler_status sched_spawn_kernel_thread(
    struct kernel_thread *thread,
    void (*entry)(void),
    uint64_t hhdm_offset);
uint32_t sched_count(void);
const char *scheduler_status_string(enum scheduler_status status);
const char *task_state_string(enum task_state state);

#ifdef HFOC_SCHEDULER_HOST_TEST
void sched_host_test_reset(void);
scheduler_switch_hook sched_host_test_set_switch_hook(
    scheduler_switch_hook hook);
void sched_host_test_set_yield_in_progress(bool in_progress);
void sched_host_test_set_now_ticks(uint64_t now_ticks);
scheduler_stack_free_hook sched_host_test_set_stack_free_hook(
    scheduler_stack_free_hook hook);
uintptr_t sched_host_test_last_rsp0(void);
void sched_host_test_set_idle_stack_top(uintptr_t stack_top);
#endif

#ifdef HFOC_SCHEDULER_YIELD_QEMU_TEST
enum scheduler_status sched_yield_qemu_test_init(uint64_t hhdm_offset);
#endif

#ifdef HFOC_SCHEDULER_TICK_QEMU_TEST
enum scheduler_status sched_tick_qemu_test_init(uint64_t hhdm_offset);
#endif

#ifdef HFOC_SCHEDULER_WAIT_QEMU_TEST
enum scheduler_status sched_wait_qemu_test_init(uint64_t hhdm_offset);
#endif

#ifdef HFOC_SCHEDULER_EXIT_QEMU_TEST
enum scheduler_status sched_exit_qemu_test_init(uint64_t hhdm_offset);
#endif

#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
enum scheduler_status sched_stress_qemu_test_init(uint64_t hhdm_offset);
#endif

#endif
