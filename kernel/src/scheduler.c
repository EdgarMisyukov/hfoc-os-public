#include "scheduler.h"
#include "lapic_timer.h"
#include "arch/x86_64/tss.h"
#include "arch/x86_64/timer.h"
#include "pmm.h"
#include "serial.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern void sched_kernel_thread_trampoline(void);
void sched_reclaim_deferred(void);

#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
static volatile uint32_t stress_reclaimed_pages;
#endif

#ifndef HFOC_SCHEDULER_HOST_TEST
static uint8_t scheduler_idle_stack[KERNEL_THREAD_STACK_SIZE]
    __attribute__((aligned(16)));

static void scheduler_idle_entry(void) {
    for (;;) {
        sched_reclaim_deferred();
        __asm__ volatile ("hlt" : : : "memory");
        (void)sched_yield();
    }
}
#endif

struct scheduler {
    struct scheduler_task *head;
    struct scheduler_task *tail;
    struct scheduler_task *current;
    struct scheduler_task *blocked_head;
    struct scheduler_task *deferred_head;
    struct scheduler_task *deferred_tail;
    struct scheduler_wait_queue timeout_queue;
    struct context_frame idle_context;
    uintptr_t idle_stack_top;
    uint32_t count;
    uint32_t blocked_count;
    uint64_t switch_count;
    uint64_t wake_count;
    bool initialized;
    bool yield_in_progress;
};

static struct scheduler bsp_scheduler;
/* ITEM-627: syscall entry runs on the selected task's own kernel stack. */
uint64_t sched_syscall_stack_top;
struct scheduler_task *sched_running_task;
static scheduler_task_limit_hook task_limit_hook;
static scheduler_activate_hook activate_hook;

#ifdef HFOC_SCHEDULER_HOST_TEST
static scheduler_switch_hook switch_hook;
static uint64_t host_now_ticks;
static scheduler_stack_free_hook stack_free_hook;
static uintptr_t host_last_rsp0;

void sched_host_test_reset(void) {
    memset(&bsp_scheduler, 0, sizeof(bsp_scheduler));
    switch_hook = NULL;
    task_limit_hook = NULL;
    host_now_ticks = 0U;
    stack_free_hook = NULL;
    host_last_rsp0 = 0U;
}

scheduler_switch_hook sched_host_test_set_switch_hook(
    scheduler_switch_hook hook)
{
    scheduler_switch_hook const previous = switch_hook;
    switch_hook = hook;
    return previous;
}

void sched_host_test_set_yield_in_progress(bool in_progress) {
    bsp_scheduler.yield_in_progress = in_progress;
}

void sched_host_test_set_now_ticks(uint64_t now_ticks) {
    host_now_ticks = now_ticks;
}

uintptr_t sched_host_test_last_rsp0(void) {
    return host_last_rsp0;
}

void sched_host_test_set_idle_stack_top(uintptr_t stack_top) {
    bsp_scheduler.idle_stack_top = stack_top;
}

scheduler_stack_free_hook sched_host_test_set_stack_free_hook(
    scheduler_stack_free_hook hook)
{
    scheduler_stack_free_hook const previous = stack_free_hook;
    stack_free_hook = hook;
    return previous;
}
#endif

static void scheduler_set_rsp0(const struct scheduler_task *task) {
    const uintptr_t stack_top = task != NULL ?
        task->kernel_stack_top : bsp_scheduler.idle_stack_top;
    sched_syscall_stack_top = stack_top;
    sched_running_task = (struct scheduler_task *)task;
    if (activate_hook != NULL) {
        activate_hook(task != NULL ? task->process_owner : NULL);
    }
#ifdef HFOC_SCHEDULER_HOST_TEST
    host_last_rsp0 = stack_top;
#else
    tss_set_rsp0(stack_top);
#endif
}

static uint64_t scheduler_now_ticks(void) {
#ifdef HFOC_SCHEDULER_HOST_TEST
    return host_now_ticks;
#else
    return timer_ticks();
#endif
}

static bool scheduler_irq_disable(void) {
#ifdef HFOC_SCHEDULER_HOST_TEST
    return false;
#else
    uint64_t rflags;
    __asm__ volatile ("pushfq; pop %0" : "=r"(rflags) : : "memory");
    __asm__ volatile ("cli" : : : "memory");
    return (rflags & (UINT64_C(1) << 9U)) != 0U;
#endif
}

static void scheduler_irq_restore(bool restore_interrupts) {
#ifndef HFOC_SCHEDULER_HOST_TEST
    if (restore_interrupts) {
        __asm__ volatile ("sti" : : : "memory");
    }
#else
    (void)restore_interrupts;
#endif
}

static enum scheduler_status scheduler_switch(struct context_frame *current,
                                              struct context_frame *next)
{
#ifdef HFOC_SCHEDULER_HOST_TEST
    if (switch_hook != NULL) {
        const enum scheduler_status status =
            switch_hook(current, next);
        if (status == SCHEDULER_OK) {
            ++bsp_scheduler.switch_count;
        }
        return status;
    }
#endif
    const enum context_status context_status =
        context_switch(current, next);
    if (context_status == CONTEXT_OK) {
        ++bsp_scheduler.switch_count;
        return SCHEDULER_OK;
    }
    if (context_status == CONTEXT_NULL) {
        return SCHEDULER_NULL;
    }
    return SCHEDULER_BAD_CONTEXT;
}

static void scheduler_yield_finish(bool restore_interrupts) {
    bsp_scheduler.yield_in_progress = false;
    scheduler_irq_restore(restore_interrupts);
}

void sched_reclaim_deferred(void) {
    while (bsp_scheduler.deferred_head != NULL) {
        struct scheduler_task *const task =
            bsp_scheduler.deferred_head;
        bsp_scheduler.deferred_head = task->deferred_next;
        if (bsp_scheduler.deferred_tail == task) {
            bsp_scheduler.deferred_tail = NULL;
        }
        task->deferred_next = NULL;
        if (task->stack_phys != 0U && task->stack_pages != 0U) {
#ifdef HFOC_SCHEDULER_HOST_TEST
            if (stack_free_hook != NULL) {
                stack_free_hook(task->stack_phys, task->stack_pages);
            }
#else
            (void)pmm_free_contiguous_pages(task->stack_phys,
                                            task->stack_pages);
#endif
#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
        stress_reclaimed_pages += task->stack_pages;
#endif
        }
        task->stack_phys = 0U;
        task->stack_pages = 0U;
    }
}

enum scheduler_status sched_init(void) {
    bsp_scheduler.initialized = true;
    bsp_scheduler.head = NULL;
    bsp_scheduler.tail = NULL;
    bsp_scheduler.current = NULL;
    bsp_scheduler.blocked_head = NULL;
    bsp_scheduler.deferred_head = NULL;
    bsp_scheduler.deferred_tail = NULL;
    bsp_scheduler.timeout_queue.head = NULL;
    bsp_scheduler.timeout_queue.tail = NULL;
    bsp_scheduler.timeout_queue.count = 0U;
    bsp_scheduler.idle_stack_top = 0U;
#ifdef HFOC_SCHEDULER_HOST_TEST
    memset(&bsp_scheduler.idle_context, 0, sizeof(bsp_scheduler.idle_context));
#else
    bsp_scheduler.idle_stack_top =
        (uintptr_t)scheduler_idle_stack + sizeof(scheduler_idle_stack);
    const enum context_status idle_status = context_frame_init(
        &bsp_scheduler.idle_context,
        (uintptr_t)scheduler_idle_stack + sizeof(scheduler_idle_stack),
        scheduler_idle_entry);
    if (idle_status != CONTEXT_OK) {
        bsp_scheduler.initialized = false;
        return SCHEDULER_BAD_CONTEXT;
    }
#endif
    bsp_scheduler.yield_in_progress = false;
    bsp_scheduler.count = 0U;
    bsp_scheduler.blocked_count = 0U;
    bsp_scheduler.switch_count = 0U;
    bsp_scheduler.wake_count = 0U;
    return SCHEDULER_OK;
}

bool sched_initialized(void)
{
    return bsp_scheduler.initialized;
}

scheduler_task_limit_hook sched_set_limit_hook(
    scheduler_task_limit_hook hook)
{
    scheduler_task_limit_hook const previous = task_limit_hook;
    task_limit_hook = hook;
    return previous;
}

void sched_set_activate_hook(scheduler_activate_hook hook) {
    activate_hook = hook;
}

static bool transition_allowed(enum task_state from, enum task_state to) {
    switch (from) {
    case TASK_STATE_CREATED:
        return to == TASK_STATE_RUNNABLE || to == TASK_STATE_EXITED;
    case TASK_STATE_RUNNABLE:
        return to == TASK_STATE_RUNNING;
    case TASK_STATE_RUNNING:
        return to == TASK_STATE_RUNNABLE || to == TASK_STATE_BLOCKED ||
               to == TASK_STATE_EXITED;
    case TASK_STATE_BLOCKED:
        return to == TASK_STATE_RUNNABLE || to == TASK_STATE_EXITED;
    case TASK_STATE_STOPPED:
        return to == TASK_STATE_RUNNABLE || to == TASK_STATE_EXITED;
    case TASK_STATE_EXITED:
        return false;
    }
    return false;
}

static uint64_t scheduler_deadline(uint64_t now, uint64_t ticks) {
    if (ticks == 0U) {
        return 0U;
    }
    if (now > UINT64_MAX - ticks) {
        return UINT64_MAX;
    }
    return now + ticks;
}

static void scheduler_wait_queue_remove(struct scheduler_wait_queue *queue,
                                        struct scheduler_task *task)
{
    struct scheduler_task *previous = NULL;
    struct scheduler_task *entry = queue->head;
    while (entry != NULL) {
        if (entry == task) {
            if (previous == NULL) {
                queue->head = task->wait_next;
            } else {
                previous->wait_next = task->wait_next;
            }
            if (queue->tail == task) {
                queue->tail = previous;
            }
            --queue->count;
            task->wait_next = NULL;
            return;
        }
        previous = entry;
        entry = entry->wait_next;
    }
}

static bool scheduler_timeout_remove(struct scheduler_task *task) {
    struct scheduler_task *previous = NULL;
    struct scheduler_task *entry = bsp_scheduler.timeout_queue.head;
    while (entry != NULL) {
        if (entry == task) {
            if (previous == NULL) {
                bsp_scheduler.timeout_queue.head = task->blocked_next;
            } else {
                previous->blocked_next = task->blocked_next;
            }
            if (bsp_scheduler.timeout_queue.tail == task) {
                bsp_scheduler.timeout_queue.tail = previous;
            }
            --bsp_scheduler.timeout_queue.count;
            task->blocked_next = NULL;
            return true;
        }
        previous = entry;
        entry = entry->blocked_next;
    }
    return false;
}

static enum scheduler_status sched_enqueue_locked(
    struct scheduler_task *task);
static enum scheduler_status sched_next_locked(
    struct scheduler_task **task);
static enum scheduler_status sched_transition_locked(
    struct scheduler_task *task,
    enum task_state next_state);

static enum scheduler_status scheduler_wake_one_locked(
    struct scheduler_wait_queue *queue)
{
    if (queue == NULL) {
        return SCHEDULER_NULL;
    }
    if (queue->head == NULL) {
        return SCHEDULER_EMPTY;
    }

    struct scheduler_task *const task = queue->head;
    if (task->state != TASK_STATE_BLOCKED || task->queued) {
        return SCHEDULER_BAD_STATE;
    }
    scheduler_wait_queue_remove(queue, task);
    if (task->wake_ticks != 0U) {
        (void)scheduler_timeout_remove(task);
    }
    task->wait_queue = NULL;
    task->wake_ticks = 0U;
    const enum scheduler_status enqueue_status =
        sched_enqueue_locked(task);
    if (enqueue_status != SCHEDULER_OK) {
        return enqueue_status;
    }
    ++task->accounting.wake_count;
    ++bsp_scheduler.wake_count;
    if (bsp_scheduler.blocked_count != 0U) {
        --bsp_scheduler.blocked_count;
    }
    return SCHEDULER_OK;
}

static enum scheduler_status scheduler_expire_timeouts(void) {
    struct scheduler_task *entry = bsp_scheduler.timeout_queue.head;
    const uint64_t now = scheduler_now_ticks();
    while (entry != NULL) {
        struct scheduler_task *const next = entry->blocked_next;
        if (entry->wake_ticks != 0U && entry->wake_ticks <= now) {
            scheduler_timeout_remove(entry);
            if (entry->wait_queue != NULL) {
                scheduler_wait_queue_remove(entry->wait_queue, entry);
            }
            entry->wait_queue = NULL;
            entry->wake_ticks = 0U;
            const enum scheduler_status enqueue_status =
                sched_enqueue_locked(entry);
            if (enqueue_status != SCHEDULER_OK) {
                return enqueue_status;
            }
            ++entry->accounting.wake_count;
            ++bsp_scheduler.wake_count;
            if (bsp_scheduler.blocked_count != 0U) {
                --bsp_scheduler.blocked_count;
            }
        }
        entry = next;
    }
    return SCHEDULER_OK;
}

enum scheduler_status sched_enqueue(struct scheduler_task *task) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    const enum scheduler_status status =
        sched_enqueue_locked(task);
    scheduler_yield_finish(restore_interrupts);
    return status;
}

static enum scheduler_status sched_enqueue_locked(
    struct scheduler_task *task)
{
    if (task == NULL) {
        return SCHEDULER_NULL;
    }
    if (task->queued) {
        return SCHEDULER_DOUBLE_ENQUEUE;
    }
    if (task->state != TASK_STATE_CREATED &&
        task->state != TASK_STATE_RUNNABLE &&
        task->state != TASK_STATE_BLOCKED) {
        return SCHEDULER_BAD_STATE;
    }

    task->next = NULL;
    task->state = TASK_STATE_RUNNABLE;
    task->queued = true;
    if (bsp_scheduler.tail != NULL) {
        bsp_scheduler.tail->next = task;
    } else {
        bsp_scheduler.head = task;
    }
    bsp_scheduler.tail = task;
    ++bsp_scheduler.count;
    return SCHEDULER_OK;
}

enum scheduler_status sched_next(struct scheduler_task **task) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    const enum scheduler_status status =
        sched_next_locked(task);
    scheduler_yield_finish(restore_interrupts);
    return status;
}

static enum scheduler_status sched_next_locked(
    struct scheduler_task **task)
{
    if (task == NULL) {
        return SCHEDULER_NULL;
    }
    if (bsp_scheduler.count == 0U || bsp_scheduler.head == NULL) {
        *task = NULL;
        return SCHEDULER_EMPTY;
    }

    struct scheduler_task *result = bsp_scheduler.head;
    bsp_scheduler.head = result->next;
    if (bsp_scheduler.head == NULL) {
        bsp_scheduler.tail = NULL;
    }
    result->next = NULL;
    result->state = TASK_STATE_RUNNING;
    result->queued = false;
    bsp_scheduler.current = result;
    scheduler_set_rsp0(result);
    ++result->accounting.run_count;
    result->accounting.last_run_tick = scheduler_now_ticks();
    *task = result;
    --bsp_scheduler.count;
    return SCHEDULER_OK;
}

enum scheduler_status sched_transition(struct scheduler_task *task,
                                       enum task_state next_state)
{
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    const enum scheduler_status status =
        sched_transition_locked(task, next_state);
    scheduler_yield_finish(restore_interrupts);
    return status;
}

static enum scheduler_status sched_transition_locked(
    struct scheduler_task *task,
    enum task_state next_state)
{
    if (task == NULL) {
        return SCHEDULER_NULL;
    }
    if (!transition_allowed(task->state, next_state)) {
        return SCHEDULER_BAD_STATE;
    }

    task->state = next_state;
    return SCHEDULER_OK;
}

enum scheduler_status sched_current(struct scheduler_task **task) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (task == NULL) {
        return SCHEDULER_NULL;
    }
    *task = bsp_scheduler.current;
    return SCHEDULER_OK;
}

enum scheduler_status sched_yield(void) {
    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;

    if (!bsp_scheduler.initialized) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (bsp_scheduler.current != NULL &&
        bsp_scheduler.current->state != TASK_STATE_RUNNING) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }

    struct scheduler_task *next = NULL;
    struct scheduler_task *const yielding = bsp_scheduler.current;
    if (bsp_scheduler.head != NULL &&
        !context_frame_is_valid(&bsp_scheduler.head->context)) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_BAD_CONTEXT;
    }
    const bool idle_selected =
        yielding != NULL && bsp_scheduler.head == NULL;

    if (yielding != NULL) {
        bsp_scheduler.current = NULL;
        yielding->state = TASK_STATE_RUNNABLE;
        const enum scheduler_status enqueue_status =
            sched_enqueue_locked(yielding);
        if (enqueue_status != SCHEDULER_OK) {
            yielding->state = TASK_STATE_RUNNING;
            bsp_scheduler.current = yielding;
            scheduler_yield_finish(restore_interrupts);
            return enqueue_status;
        }
    }

    if (idle_selected) {
        bsp_scheduler.yield_in_progress = false;
        scheduler_set_rsp0(NULL);
        const enum scheduler_status switch_status =
            scheduler_switch(&yielding->context,
                             &bsp_scheduler.idle_context);
        sched_reclaim_deferred();
        scheduler_irq_restore(restore_interrupts);
        return switch_status;
    }

    const enum scheduler_status next_status = sched_next_locked(&next);
    if (next_status == SCHEDULER_EMPTY) {
        if (yielding == NULL) {
            scheduler_yield_finish(restore_interrupts);
            return SCHEDULER_OK;
        }
        bsp_scheduler.yield_in_progress = false;
        scheduler_set_rsp0(NULL);
        const enum scheduler_status switch_status =
            scheduler_switch(&yielding->context,
                             &bsp_scheduler.idle_context);
        sched_reclaim_deferred();
        scheduler_irq_restore(restore_interrupts);
        return switch_status;
    }
    if (next_status != SCHEDULER_OK) {
        if (yielding != NULL) {
            yielding->state = TASK_STATE_RUNNABLE;
            yielding->queued = true;
            bsp_scheduler.current = yielding;
        }
        scheduler_yield_finish(restore_interrupts);
        return next_status;
    }

    struct context_frame *const current_frame =
        (yielding != NULL)
            ? &yielding->context
            : &bsp_scheduler.idle_context;
    bsp_scheduler.yield_in_progress = false;
    const enum scheduler_status switch_status =
        scheduler_switch(current_frame, &next->context);
    sched_reclaim_deferred();
    scheduler_irq_restore(restore_interrupts);
    return switch_status;
}

static enum scheduler_status scheduler_exit_current_locked(void) {
    struct scheduler_task *const exiting = bsp_scheduler.current;

    if (exiting == NULL ||
        exiting->state != TASK_STATE_RUNNING ||
        exiting->queued ||
        exiting->wait_queue != NULL) {
        return SCHEDULER_BAD_STATE;
    }
    if (bsp_scheduler.head != NULL &&
        !context_frame_is_valid(&bsp_scheduler.head->context)) {
        return SCHEDULER_BAD_CONTEXT;
    }

    exiting->state = TASK_STATE_EXITED;
    exiting->queued = false;
    exiting->wait_queue = NULL;
    exiting->wake_ticks = 0U;
    exiting->wait_next = NULL;
    exiting->blocked_next = NULL;
    exiting->deferred_next = NULL;
    bsp_scheduler.current = NULL;

    const bool has_stack =
        exiting->stack_phys != 0U && exiting->stack_pages != 0U;
    if (has_stack) {
        if (bsp_scheduler.deferred_tail != NULL) {
            bsp_scheduler.deferred_tail->deferred_next = exiting;
        } else {
            bsp_scheduler.deferred_head = exiting;
        }
        bsp_scheduler.deferred_tail = exiting;
    }

    struct scheduler_task *next = NULL;
    const enum scheduler_status next_status =
        sched_next_locked(&next);
    if (next_status == SCHEDULER_EMPTY) {
        bsp_scheduler.yield_in_progress = false;
        scheduler_set_rsp0(NULL);
        return scheduler_switch(&exiting->context,
                                &bsp_scheduler.idle_context);
    }
    if (next_status != SCHEDULER_OK) {
        if (has_stack) {
            struct scheduler_task *previous = NULL;
            struct scheduler_task *entry =
                bsp_scheduler.deferred_head;
            while (entry != NULL && entry != exiting) {
                previous = entry;
                entry = entry->deferred_next;
            }
            if (entry == exiting) {
                if (previous == NULL) {
                    bsp_scheduler.deferred_head =
                        exiting->deferred_next;
                } else {
                    previous->deferred_next =
                        exiting->deferred_next;
                }
                if (bsp_scheduler.deferred_tail == exiting) {
                    bsp_scheduler.deferred_tail = previous;
                }
                exiting->deferred_next = NULL;
            }
        }
        exiting->state = TASK_STATE_RUNNING;
        bsp_scheduler.current = exiting;
        return next_status;
    }

    bsp_scheduler.yield_in_progress = false;
    return scheduler_switch(&exiting->context, &next->context);
}

static enum scheduler_status scheduler_on_tick_locked(void) {
    const enum scheduler_status timeout_status =
        scheduler_expire_timeouts();
    if (timeout_status != SCHEDULER_OK) {
        return timeout_status;
    }
    if (bsp_scheduler.current == NULL ||
        bsp_scheduler.current->state != TASK_STATE_RUNNING) {
        return SCHEDULER_BAD_STATE;
    }
    ++bsp_scheduler.current->accounting.tick_count;
    struct scheduler_task *const limited = bsp_scheduler.current;
    if (limited->cpu_tick_limit != 0U &&
        limited->accounting.tick_count >= limited->cpu_tick_limit) {
        if (bsp_scheduler.head != NULL &&
            !context_frame_is_valid(&bsp_scheduler.head->context)) {
            return SCHEDULER_BAD_CONTEXT;
        }
        if (task_limit_hook == NULL) {
            return SCHEDULER_BAD_STATE;
        }
        const enum scheduler_status hook_status =
            task_limit_hook(limited);
        if (hook_status != SCHEDULER_OK) {
            return hook_status;
        }
        return scheduler_exit_current_locked();
    }
    if (bsp_scheduler.head == NULL) {
        return SCHEDULER_OK;
    }
    if (!context_frame_is_valid(&bsp_scheduler.head->context)) {
        return SCHEDULER_BAD_CONTEXT;
    }

    struct scheduler_task *const preempted = bsp_scheduler.current;
    struct scheduler_task *successor = NULL;
    bsp_scheduler.current = NULL;
    preempted->state = TASK_STATE_RUNNABLE;
    const enum scheduler_status enqueue_status =
        sched_enqueue_locked(preempted);
    if (enqueue_status != SCHEDULER_OK) {
        preempted->state = TASK_STATE_RUNNING;
        bsp_scheduler.current = preempted;
        return enqueue_status;
    }

    const enum scheduler_status next_status =
        sched_next_locked(&successor);
    if (next_status != SCHEDULER_OK) {
        preempted->state = TASK_STATE_RUNNING;
        preempted->queued = false;
        bsp_scheduler.current = preempted;
        return next_status;
    }

    bsp_scheduler.yield_in_progress = false;
    const enum scheduler_status switch_status =
        scheduler_switch(&preempted->context, &successor->context);
    if (switch_status != SCHEDULER_OK) {
        return switch_status;
    }
    return SCHEDULER_OK;
}

enum scheduler_status sched_on_tick(void) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (bsp_scheduler.yield_in_progress) {
        return SCHEDULER_OK;
    }

    const bool restore_interrupts = scheduler_irq_disable();
    bsp_scheduler.yield_in_progress = true;
    const enum scheduler_status status =
        scheduler_on_tick_locked();
    sched_reclaim_deferred();
    scheduler_yield_finish(restore_interrupts);
    return status;
}

/* ITEM-633: a PIT tick only needs to wake expired waits.  It must not
   preempt from the IRQ handler; the ordinary loop's sched_yield runs the
   woken task. */
enum scheduler_status sched_timeout_tick(void) {
    enum scheduler_status status;

    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (bsp_scheduler.yield_in_progress) {
        return SCHEDULER_OK;
    }
    const bool restore_interrupts = scheduler_irq_disable();
    bsp_scheduler.yield_in_progress = true;
    status = scheduler_expire_timeouts();
    scheduler_yield_finish(restore_interrupts);
    return status;
}

enum scheduler_status sched_wait_queue_init(
    struct scheduler_wait_queue *queue)
{
    if (queue == NULL) {
        return SCHEDULER_NULL;
    }
    queue->head = NULL;
    queue->tail = NULL;
    queue->count = 0U;
    return SCHEDULER_OK;
}

enum scheduler_status sched_wait(struct scheduler_wait_queue *queue,
                                 uint64_t timeout_ticks)
{
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (queue == NULL) {
        return SCHEDULER_NULL;
    }

    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;

    if (bsp_scheduler.current == NULL ||
        bsp_scheduler.current->state != TASK_STATE_RUNNING ||
        bsp_scheduler.current->queued ||
        bsp_scheduler.current->wait_queue != NULL) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    if (bsp_scheduler.head != NULL &&
        !context_frame_is_valid(&bsp_scheduler.head->context)) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_BAD_CONTEXT;
    }

    struct scheduler_task *const task = bsp_scheduler.current;

    ++task->accounting.block_count;
    ++bsp_scheduler.blocked_count;
    task->state = TASK_STATE_BLOCKED;
    task->queued = false;
    task->wait_next = NULL;
    task->blocked_next = NULL;
    const bool timeout_as_wait_queue =
        (queue == &bsp_scheduler.timeout_queue);
    task->wait_queue = timeout_as_wait_queue ? NULL : queue;
    task->wake_ticks =
        scheduler_deadline(scheduler_now_ticks(), timeout_ticks);
    if (!timeout_as_wait_queue) {
        if (queue->tail != NULL) {
            queue->tail->wait_next = task;
        } else {
            queue->head = task;
        }
        queue->tail = task;
        ++queue->count;
    }
    if (task->wake_ticks != 0U) {
        if (bsp_scheduler.timeout_queue.tail != NULL) {
            bsp_scheduler.timeout_queue.tail->blocked_next = task;
        } else {
            bsp_scheduler.timeout_queue.head = task;
        }
        bsp_scheduler.timeout_queue.tail = task;
        ++bsp_scheduler.timeout_queue.count;
    }
    bsp_scheduler.current = NULL;

    bsp_scheduler.yield_in_progress = false;
    struct scheduler_task *next = NULL;
    const enum scheduler_status next_status =
        sched_next_locked(&next);
    if (next_status == SCHEDULER_EMPTY) {
        scheduler_set_rsp0(NULL);
        const enum scheduler_status switch_status =
            scheduler_switch(&task->context,
                             &bsp_scheduler.idle_context);
        sched_reclaim_deferred();
        scheduler_irq_restore(restore_interrupts);
        return switch_status;
    }
    if (next_status != SCHEDULER_OK) {
        if (task->wait_queue != NULL) {
            scheduler_wait_queue_remove(task->wait_queue, task);
        }
        if (task->wake_ticks != 0U) {
            (void)scheduler_timeout_remove(task);
        }
        task->wait_queue = NULL;
        task->wake_ticks = 0U;
        task->state = TASK_STATE_RUNNING;
        if (task->accounting.block_count != 0U) {
            --task->accounting.block_count;
        }
        if (bsp_scheduler.blocked_count != 0U) {
            --bsp_scheduler.blocked_count;
        }
        bsp_scheduler.current = task;
        scheduler_irq_restore(restore_interrupts);
        return next_status;
    }

    const enum scheduler_status switch_status =
        scheduler_switch(&task->context, &next->context);
    sched_reclaim_deferred();
    if (switch_status != SCHEDULER_OK) {
        if (task->wait_queue != NULL) {
            scheduler_wait_queue_remove(task->wait_queue, task);
        }
        if (task->wake_ticks != 0U) {
            (void)scheduler_timeout_remove(task);
        }
        task->wait_queue = NULL;
        task->wake_ticks = 0U;
        task->state = TASK_STATE_RUNNING;
        if (task->accounting.block_count != 0U) {
            --task->accounting.block_count;
        }
        if (bsp_scheduler.blocked_count != 0U) {
            --bsp_scheduler.blocked_count;
        }
        next->state = TASK_STATE_RUNNABLE;
        next->queued = false;
        if (next->accounting.run_count != 0U) {
            --next->accounting.run_count;
        }
        next->accounting.last_run_tick = 0U;
        (void)sched_enqueue_locked(next);
        bsp_scheduler.current = task;
        scheduler_irq_restore(restore_interrupts);
        return switch_status;
    }
    scheduler_irq_restore(restore_interrupts);
    return SCHEDULER_OK;
}

enum scheduler_status sched_wake_one(struct scheduler_wait_queue *queue) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (queue == NULL) {
        return SCHEDULER_NULL;
    }

    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    if (queue->head == NULL) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_EMPTY;
    }
    const enum scheduler_status status =
        scheduler_wake_one_locked(queue);
    scheduler_yield_finish(restore_interrupts);
    return status;
}

enum scheduler_status sched_wake_all(struct scheduler_wait_queue *queue) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (queue == NULL) {
        return SCHEDULER_NULL;
    }

    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    if (queue->head == NULL) {
        scheduler_yield_finish(restore_interrupts);
        return SCHEDULER_EMPTY;
    }
    while (queue->head != NULL) {
        const enum scheduler_status status =
            scheduler_wake_one_locked(queue);
        if (status != SCHEDULER_OK) {
            scheduler_yield_finish(restore_interrupts);
            return status;
        }
    }
    scheduler_yield_finish(restore_interrupts);
    return SCHEDULER_OK;
}

enum scheduler_status sched_sleep(uint64_t ticks) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (ticks == 0U) {
        return SCHEDULER_OK;
    }
    return sched_wait(&bsp_scheduler.timeout_queue, ticks);
}

enum scheduler_status sched_exit(void) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }

    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    const enum scheduler_status status =
        scheduler_exit_current_locked();
    scheduler_yield_finish(restore_interrupts);
    return status;
}

enum scheduler_status sched_cancel(struct scheduler_task *task) {
    struct scheduler_task *previous = NULL;
    struct scheduler_task *entry;
    const bool restore_interrupts = scheduler_irq_disable();

    if (!bsp_scheduler.initialized) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (task == NULL) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_NULL;
    }
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    if (task == bsp_scheduler.current ||
        task->state == TASK_STATE_RUNNING ||
        task->state == TASK_STATE_EXITED) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    if (task->wait_queue != NULL) {
        scheduler_wait_queue_remove(task->wait_queue, task);
        task->wait_queue = NULL;
        if (bsp_scheduler.blocked_count != 0U) {
            --bsp_scheduler.blocked_count;
        }
    }
    if (task->wake_ticks != 0U) {
        (void)scheduler_timeout_remove(task);
        task->wake_ticks = 0U;
    }
    if (task->queued) {
        for (entry = bsp_scheduler.head; entry != NULL;
             entry = entry->next) {
            if (entry == task) {
                if (previous == NULL) {
                    bsp_scheduler.head = task->next;
                } else {
                    previous->next = task->next;
                }
                if (bsp_scheduler.tail == task) {
                    bsp_scheduler.tail = previous;
                }
                if (bsp_scheduler.count != 0U) {
                    --bsp_scheduler.count;
                }
                break;
            }
            previous = entry;
        }
    }
    task->next = NULL;
    task->wait_next = NULL;
    task->blocked_next = NULL;
    task->queued = false;
    task->state = TASK_STATE_EXITED;
    scheduler_yield_finish(restore_interrupts);
    return SCHEDULER_OK;
}

/* ITEM-633: remove a non-current task from every queue and hold it stopped
   until sched_resume.  This is the job-control half of sched_cancel and does
   not change which task the scheduler picks next. */
enum scheduler_status sched_suspend(struct scheduler_task *task) {
    struct scheduler_task *previous = NULL;
    struct scheduler_task *entry;
    const bool restore_interrupts = scheduler_irq_disable();

    if (!bsp_scheduler.initialized) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (task == NULL) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_NULL;
    }
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    if (task == bsp_scheduler.current ||
        task->state == TASK_STATE_RUNNING ||
        task->state == TASK_STATE_EXITED ||
        task->state == TASK_STATE_STOPPED) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    if (task->wait_queue != NULL) {
        scheduler_wait_queue_remove(task->wait_queue, task);
        task->wait_queue = NULL;
        if (bsp_scheduler.blocked_count != 0U) {
            --bsp_scheduler.blocked_count;
        }
    }
    if (task->wake_ticks != 0U) {
        (void)scheduler_timeout_remove(task);
        task->wake_ticks = 0U;
    }
    if (task->queued) {
        for (entry = bsp_scheduler.head; entry != NULL;
             entry = entry->next) {
            if (entry == task) {
                if (previous == NULL) {
                    bsp_scheduler.head = task->next;
                } else {
                    previous->next = task->next;
                }
                if (bsp_scheduler.tail == task) {
                    bsp_scheduler.tail = previous;
                }
                if (bsp_scheduler.count != 0U) {
                    --bsp_scheduler.count;
                }
                break;
            }
            previous = entry;
        }
    }
    task->next = NULL;
    task->wait_next = NULL;
    task->blocked_next = NULL;
    task->queued = false;
    task->state = TASK_STATE_STOPPED;
    scheduler_yield_finish(restore_interrupts);
    return SCHEDULER_OK;
}

enum scheduler_status sched_resume(struct scheduler_task *task) {
    enum scheduler_status status;

    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (task == NULL) {
        return SCHEDULER_NULL;
    }
    const bool restore_interrupts = scheduler_irq_disable();
    if (bsp_scheduler.yield_in_progress) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    if (task->state != TASK_STATE_STOPPED) {
        scheduler_irq_restore(restore_interrupts);
        return SCHEDULER_BAD_STATE;
    }
    bsp_scheduler.yield_in_progress = true;
    task->state = TASK_STATE_RUNNABLE;
    status = sched_enqueue_locked(task);
    scheduler_yield_finish(restore_interrupts);
    return status;
}

enum scheduler_status sched_task_accounting(
    const struct scheduler_task *task,
    struct scheduler_task_accounting *accounting)
{
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (task == NULL || accounting == NULL) {
        return SCHEDULER_NULL;
    }
    *accounting = task->accounting;
    return SCHEDULER_OK;
}

enum scheduler_status sched_snapshot(struct scheduler_snapshot *snapshot) {
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (snapshot == NULL) {
        return SCHEDULER_NULL;
    }
    snapshot->ticks = scheduler_now_ticks();
    snapshot->switch_count = bsp_scheduler.switch_count;
    snapshot->wake_count = bsp_scheduler.wake_count;
    snapshot->runnable_count = bsp_scheduler.count;
    snapshot->blocked_count = bsp_scheduler.blocked_count;
    snapshot->timeout_count = bsp_scheduler.timeout_queue.count;
    snapshot->current = bsp_scheduler.current;
    return SCHEDULER_OK;
}

#ifdef HFOC_SCHEDULER_YIELD_QEMU_TEST
static struct kernel_thread yield_thread_a;
static struct kernel_thread yield_thread_b;

static void sched_yield_thread_a_entry(void) {
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SCHED] thread-A step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        (void)sched_yield();
    }
}

static void sched_yield_thread_b_entry(void) {
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SCHED] thread-B step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        (void)sched_yield();
    }
}

enum scheduler_status sched_yield_qemu_test_init(uint64_t hhdm_offset) {
    const enum scheduler_status init_status = sched_init();
    if (init_status != SCHEDULER_OK) {
        return init_status;
    }
    const enum scheduler_status status_a =
        sched_spawn_kernel_thread(&yield_thread_a,
                                  sched_yield_thread_a_entry,
                                  hhdm_offset);
    if (status_a != SCHEDULER_OK) {
        return status_a;
    }
    const enum scheduler_status status_b =
        sched_spawn_kernel_thread(&yield_thread_b,
                                  sched_yield_thread_b_entry,
                                  hhdm_offset);
    if (status_b != SCHEDULER_OK) {
        return status_b;
    }
    return SCHEDULER_OK;
}
#endif

#ifdef HFOC_SCHEDULER_TICK_QEMU_TEST
static struct kernel_thread tick_thread_a;
static struct kernel_thread tick_thread_b;
static volatile bool tick_bootstrap_published;

static void sched_tick_thread_a_entry(void) {
    while (!tick_bootstrap_published) {
    }
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SCHED-TICK] thread-A step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
}

static void sched_tick_thread_b_entry(void) {
    tick_bootstrap_published = true;
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[SCHED-TICK] thread-B step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
}

enum scheduler_status sched_tick_qemu_test_init(uint64_t hhdm_offset) {
    const enum scheduler_status init_status = sched_init();
    if (init_status != SCHEDULER_OK) {
        return init_status;
    }
    const enum scheduler_status status_a =
        sched_spawn_kernel_thread(&tick_thread_a,
                                  sched_tick_thread_a_entry,
                                  hhdm_offset);
    if (status_a != SCHEDULER_OK) {
        return status_a;
    }
    const enum scheduler_status status_b =
        sched_spawn_kernel_thread(&tick_thread_b,
                                  sched_tick_thread_b_entry,
                                  hhdm_offset);
    if (status_b != SCHEDULER_OK) {
        return status_b;
    }
    return SCHEDULER_OK;
}
#endif

#ifdef HFOC_SCHEDULER_WAIT_QEMU_TEST
static struct kernel_thread wait_thread;

static void sched_wait_thread_entry(void) {
    /*
     * ITEM-635 gate repair: this proof claims the LAPIC timer EOI precedes
     * sleep completion, so it must not start the sleep while the LAPIC timer
     * runtime is still being brought up.  On the current boot order the PIT
     * woke a 5-tick sleep before the runtime marker, which made the proof fail
     * deterministically on main as well as on this branch.  This code is
     * compiled only into the HFOC_SCHEDULER_WAIT_QEMU_TEST kernel; production
     * scheduling and IRQ ownership are unchanged.
     */
    for (uint32_t spin = 0U; spin < 100000U; ++spin) {
        if (lapic_timer_runtime_active()) {
            break;
        }
        if (sched_yield() != SCHEDULER_OK) {
            break;
        }
    }
    serial_write("[WAIT] sleep start\n");
    const enum scheduler_status sleep_status = sched_sleep(5U);
    if (sleep_status != SCHEDULER_OK) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[WAIT] sleep failed: ");
            serial_write(scheduler_status_string(sleep_status));
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        for (;;) {
        }
    }
    serial_write("[WAIT] sleep done\n");
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[WAIT] progress step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
}

enum scheduler_status sched_wait_qemu_test_init(uint64_t hhdm_offset) {
    const enum scheduler_status init_status = sched_init();
    if (init_status != SCHEDULER_OK) {
        return init_status;
    }
    return sched_spawn_kernel_thread(&wait_thread,
                                     sched_wait_thread_entry,
                                     hhdm_offset);
}
#endif

#ifdef HFOC_SCHEDULER_EXIT_QEMU_TEST
static struct kernel_thread exit_thread_a;
static struct kernel_thread exit_thread_b;

static void sched_exit_thread_a_entry(void) {
    serial_write("[EXIT] thread-A before exit\n");
    const enum scheduler_status exit_status = sched_exit();
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[EXIT] thread-A after exit: ");
        serial_write(scheduler_status_string(exit_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
    }
}

static void sched_exit_thread_b_entry(void) {
    uint64_t step = 0U;
    for (;;) {
        ++step;
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[EXIT] thread-B step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        (void)sched_yield();
    }
}

enum scheduler_status sched_exit_qemu_test_init(uint64_t hhdm_offset) {
    const enum scheduler_status init_status = sched_init();
    if (init_status != SCHEDULER_OK) {
        return init_status;
    }
    const enum scheduler_status status_a =
        sched_spawn_kernel_thread(&exit_thread_a,
                                  sched_exit_thread_a_entry,
                                  hhdm_offset);
    if (status_a != SCHEDULER_OK) {
        return status_a;
    }
    return sched_spawn_kernel_thread(&exit_thread_b,
                                     sched_exit_thread_b_entry,
                                     hhdm_offset);
}
#endif

#ifdef HFOC_SCHEDULER_STRESS_QEMU_TEST
#define SCHED_STRESS_GENERATIONS 4U
#define SCHED_STRESS_WORKERS 2U
#define SCHED_STRESS_STEPS 3U

static struct kernel_thread stress_driver;
static struct kernel_thread stress_workers[SCHED_STRESS_WORKERS];
static uint64_t stress_hhdm_offset;
static volatile uint32_t stress_completed;
static volatile uint32_t stress_exited;
static volatile bool stress_fault_pending;
static volatile bool stress_fault_handled;
static struct scheduler_wait_queue stress_done_queue;

static void sched_stress_worker_entry(void) {
    uint32_t step;
    serial_write("[STRESS] worker start\n");
    for (step = 1U; step <= SCHED_STRESS_STEPS; ++step) {
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[STRESS] worker step=");
            serial_write_u64(step);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
        (void)sched_yield();
        if (sched_sleep(2U) != SCHEDULER_OK) {
            serial_write("[STRESS] worker sleep failed\n");
            for (;;) {
            }
        }
    }
    serial_write("[STRESS] worker done\n");
    ++stress_completed;
    (void)sched_wake_all(&stress_done_queue);
    serial_write("[STRESS] worker exit\n");
    ++stress_exited;
    if (sched_exit() != SCHEDULER_OK) {
        serial_write("[STRESS] worker exit failed\n");
        for (;;) {
        }
    }
    for (;;) {
    }
}

static enum scheduler_status stress_spawn_worker(uint32_t index) {
    if (stress_fault_pending) {
        stress_fault_pending = false;
        return SCHEDULER_NO_MEMORY;
    }
    return sched_spawn_kernel_thread(&stress_workers[index],
                                     sched_stress_worker_entry,
                                     stress_hhdm_offset);
}

static void sched_stress_driver_entry(void) {
    uint32_t generation;
    serial_write("[STRESS] driver start\n");
    for (generation = 1U;
         generation <= SCHED_STRESS_GENERATIONS;
         ++generation) {
        uint32_t worker;
        uint32_t expected_workers = SCHED_STRESS_WORKERS;
        stress_completed = 0U;
        for (worker = 0U; worker < SCHED_STRESS_WORKERS; ++worker) {
            if (generation == 2U && worker == 1U) {
                stress_fault_pending = true;
            }
            const enum scheduler_status status =
                stress_spawn_worker(worker);
            if (status == SCHEDULER_NO_MEMORY) {
                stress_fault_handled = true;
                serial_write("[STRESS] fault injected and handled\n");
                --expected_workers;
                continue;
            }
            if (status != SCHEDULER_OK) {
                serial_write("[STRESS] fault unhandled\n");
                for (;;) {
                }
            }
        }
        while (stress_completed < expected_workers) {
            if (sched_wait(&stress_done_queue, 0U) != SCHEDULER_OK) {
                serial_write("[STRESS] driver wait failed\n");
                for (;;) {
                }
            }
        }
        (void)sched_reclaim_deferred();
        {
            const uint64_t serial_flags = serial_sequence_begin();
            serial_write("[STRESS] generation complete=");
            serial_write_u64(generation);
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[STRESS] complete workers=");
        serial_write_u64(stress_exited);
        serial_write(" reclaimed pages=");
        serial_write_u64(stress_reclaimed_pages);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (sched_exit() != SCHEDULER_OK) {
        serial_write("[STRESS] driver exit failed\n");
        for (;;) {
        }
    }
    for (;;) {
    }
}

enum scheduler_status sched_stress_qemu_test_init(uint64_t hhdm_offset) {
    const enum scheduler_status init_status = sched_init();
    if (init_status != SCHEDULER_OK) {
        return init_status;
    }
    const enum scheduler_status queue_status =
        sched_wait_queue_init(&stress_done_queue);
    if (queue_status != SCHEDULER_OK) {
        return queue_status;
    }
    stress_hhdm_offset = hhdm_offset;
    return sched_spawn_kernel_thread(&stress_driver,
                                     sched_stress_driver_entry,
                                     hhdm_offset);
}
#endif

#if !defined(HFOC_SCHEDULER_HOST_TEST) || defined(HFOC_SCHEDULER_THREAD_HOST_TEST)
enum scheduler_status sched_spawn_kernel_thread(
    struct kernel_thread *thread,
    void (*entry)(void),
    uint64_t hhdm_offset)
{
    if (!bsp_scheduler.initialized) {
        return SCHEDULER_NOT_INITIALIZED;
    }
    if (thread == NULL || entry == NULL) {
        return SCHEDULER_NULL;
    }

    const uint64_t stack_phys =
        pmm_alloc_contiguous_pages(KERNEL_THREAD_STACK_PAGES);
    if (stack_phys == 0U) {
        return SCHEDULER_NO_MEMORY;
    }
    if (stack_phys > UINT64_MAX - KERNEL_THREAD_STACK_SIZE) {
        (void)pmm_free_contiguous_pages(
            stack_phys, KERNEL_THREAD_STACK_PAGES);
        return SCHEDULER_NO_MEMORY;
    }

    const uint64_t stack_end = stack_phys + KERNEL_THREAD_STACK_SIZE;
    if (stack_end > UINT64_MAX - hhdm_offset) {
        (void)pmm_free_contiguous_pages(
            stack_phys, KERNEL_THREAD_STACK_PAGES);
        return SCHEDULER_NO_MEMORY;
    }
    const uintptr_t stack_top = (uintptr_t)(hhdm_offset + stack_end);

    thread->task.next = NULL;
    thread->task.wait_next = NULL;
    thread->task.blocked_next = NULL;
    thread->task.deferred_next = NULL;
    thread->task.wait_queue = NULL;
    thread->task.process_owner = NULL;
    thread->task.wake_ticks = 0U;
    memset(&thread->task.accounting, 0,
           sizeof(thread->task.accounting));
    thread->task.cpu_tick_limit = 0U;
    thread->task.state = TASK_STATE_CREATED;
    thread->task.queued = false;
    thread->task.stack_phys = (uintptr_t)stack_phys;
    thread->task.stack_pages = KERNEL_THREAD_STACK_PAGES;
    thread->task.kernel_stack_top = stack_top;
    thread->entry = entry;
    thread->stack_phys = (uintptr_t)stack_phys;
    thread->stack_top = stack_top;
    thread->task.context.r15 = 0U;
    thread->task.context.r14 = 0U;
    thread->task.context.r13 = 0U;
    thread->task.context.r12 = 0U;
    thread->task.context.rbx = 0U;
    thread->task.context.rbp = 0U;
    thread->task.context.rip = (uintptr_t)sched_kernel_thread_trampoline;
    thread->task.context.rsp = stack_top - 8U;
    *(uintptr_t *)(thread->task.context.rsp) = (uintptr_t)entry;

    const enum scheduler_status enqueue_status =
        sched_enqueue(&thread->task);
    if (enqueue_status != SCHEDULER_OK) {
        (void)pmm_free_contiguous_pages(
            stack_phys, KERNEL_THREAD_STACK_PAGES);
        thread->task.next = NULL;
        thread->task.wait_next = NULL;
        thread->task.blocked_next = NULL;
        thread->task.deferred_next = NULL;
        thread->task.wait_queue = NULL;
        thread->task.process_owner = NULL;
        thread->task.wake_ticks = 0U;
        memset(&thread->task.accounting, 0,
               sizeof(thread->task.accounting));
        thread->task.cpu_tick_limit = 0U;
        thread->task.state = TASK_STATE_CREATED;
        thread->task.queued = false;
        thread->task.stack_phys = 0U;
        thread->task.stack_pages = 0U;
        thread->task.kernel_stack_top = 0U;
        thread->entry = NULL;
        thread->stack_phys = 0U;
        thread->stack_top = 0U;
        thread->task.context.r15 = 0U;
        thread->task.context.r14 = 0U;
        thread->task.context.r13 = 0U;
        thread->task.context.r12 = 0U;
        thread->task.context.rbx = 0U;
        thread->task.context.rbp = 0U;
        thread->task.context.rip = 0U;
        thread->task.context.rsp = 0U;
        return enqueue_status;
    }

    return SCHEDULER_OK;
}
#endif

uint32_t sched_count(void) {
    return bsp_scheduler.count;
}

const char *scheduler_status_string(enum scheduler_status status) {
    switch (status) {
    case SCHEDULER_OK:
        return "SCHEDULER_OK";
    case SCHEDULER_NOT_INITIALIZED:
        return "SCHEDULER_NOT_INITIALIZED";
    case SCHEDULER_NULL:
        return "SCHEDULER_NULL";
    case SCHEDULER_BAD_STATE:
        return "SCHEDULER_BAD_STATE";
    case SCHEDULER_DOUBLE_ENQUEUE:
        return "SCHEDULER_DOUBLE_ENQUEUE";
    case SCHEDULER_EMPTY:
        return "SCHEDULER_EMPTY";
    case SCHEDULER_NO_MEMORY:
        return "SCHEDULER_NO_MEMORY";
    case SCHEDULER_BAD_CONTEXT:
        return "SCHEDULER_BAD_CONTEXT";
    }
    return "SCHEDULER_UNKNOWN";
}

const char *task_state_string(enum task_state state) {
    switch (state) {
    case TASK_STATE_CREATED:
        return "TASK_STATE_CREATED";
    case TASK_STATE_RUNNABLE:
        return "TASK_STATE_RUNNABLE";
    case TASK_STATE_RUNNING:
        return "TASK_STATE_RUNNING";
    case TASK_STATE_BLOCKED:
        return "TASK_STATE_BLOCKED";
    case TASK_STATE_EXITED:
        return "TASK_STATE_EXITED";
    case TASK_STATE_STOPPED:
        return "TASK_STATE_STOPPED";
    }
    return "TASK_STATE_UNKNOWN";
}
