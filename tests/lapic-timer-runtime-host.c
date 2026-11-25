#include "arch/x86_64/interrupts.h"
#include "arch/x86_64/pic.h"
#include "arch/x86_64/tss.h"
#include "cpu.h"
#include "ioapic.h"
#include "lapic_timer.h"
#include "scheduler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HFOC_LAPIC_TIMER_RUNTIME_HOST_TEST

void lapic_timer_runtime_host_test_reset(void);
bool pic_disable_host_test_is_published(void);

void *isr_stub_table[256];

static char trace[256];
static size_t trace_length;
static unsigned int timer_calls;
static unsigned int keyboard_calls;
static unsigned int lapic_eoi_calls;
static unsigned int pic_eoi_calls;
static unsigned int ioapic_restore_calls;
static unsigned int pic_save_calls;
static unsigned int pic_restore_calls;
static unsigned int pic_unmask_calls;
static unsigned int pic_disable_calls;
static unsigned int oneshot_arm_calls;
static unsigned int oneshot_handle_calls;
static bool mock_oneshot_arm_ok = true;
static bool mock_oneshot_active;
static uint64_t mock_ticks_per_second = 6250000U;
static uint32_t mock_last_oneshot_ticks;

static bool mock_prepare_ok = true;
static bool mock_activate_ok = true;
static bool mock_ioapic_mask_ok = true;
static bool mock_ioapic_restore_ok = true;
static bool mock_pic_mask_ok = true;
static bool mock_pic_save_ok = true;
static bool mock_pic_restore_ok = true;
static bool mock_pic_unmask_ok = true;
static bool mock_irq1_routed;
static bool mock_pic_disable_ok = true;
static enum lapic_timer_status mock_timer_status = LAPIC_TIMER_OK;
static enum lapic_timer_runtime_status mock_runtime_status =
    LAPIC_TIMER_RUNTIME_IDLE;
static bool mock_runtime_active;
static uint64_t mock_saved_rte;
static uint64_t mock_last_restored_rte;
static uint8_t mock_pic_master;
static uint8_t mock_last_restored_pic_mask;
static struct cpu bsp_cpu;
static struct cpu ap_cpu;

static int failures;

static struct scheduler_task tick_task_a;
static struct scheduler_task tick_task_b;
static struct scheduler_task *tick_current;
static struct context_frame tick_switch_current;
static struct context_frame tick_switch_next;
static unsigned int tick_switch_calls;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void trace_add(const char *name) {
    const size_t name_length = strlen(name);
    if (trace_length != 0U && trace_length < sizeof(trace) - 1U) {
        trace[trace_length++] = ' ';
    }
    if (name_length < sizeof(trace) - trace_length) {
        memcpy(trace + trace_length, name, name_length);
        trace_length += name_length;
    }
    trace[trace_length] = '\0';
}

enum scheduler_status __real_sched_on_tick(void);

enum scheduler_status __wrap_sched_on_tick(void) {
    trace_add("sched_on_tick");
    return __real_sched_on_tick();
}

static enum scheduler_status record_tick_switch(struct context_frame *current_frame,
                                                struct context_frame *next_frame)
{
    if (current_frame == NULL || next_frame == NULL) {
        return SCHEDULER_NULL;
    }
    tick_switch_current = *current_frame;
    tick_switch_next = *next_frame;
    ++tick_switch_calls;
    return SCHEDULER_OK;
}

void timer_handle_irq(void) {
    ++timer_calls;
    trace_add("timer");
}

void keyboard_handle_irq(void) {
    ++keyboard_calls;
}

void lapic_eoi(void) {
    ++lapic_eoi_calls;
    trace_add("lapic_eoi");
}

bool pic_send_eoi(uint8_t irq) {
    if (irq == 0U) {
        ++pic_eoi_calls;
        trace_add("pic_eoi");
    }
    return true;
}

bool pic_is_spurious_irq(uint8_t irq, bool *is_spurious) {
    (void)irq;
    if (is_spurious != NULL) {
        *is_spurious = false;
    }
    return false;
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void serial_write(const char *text) {
    (void)text;
}

void serial_write_u64(uint64_t value) {
    (void)value;
}

void serial_write_hex(uint64_t value) {
    (void)value;
}

void isa_irq3_7_handle_irq(uint8_t irq) {
    (void)irq;
}

void isa_irq8_15_handle_irq(uint8_t irq) {
    (void)irq;
}

_Noreturn void kernel_panic(const char *message) {
    (void)message;
    exit(2);
}

_Noreturn void kernel_panic_exception(
    const char *message,
    const struct interrupt_frame *frame)
{
    (void)message;
    (void)frame;
    exit(2);
}

static uint8_t dummy_ist1_stack[16];
static uint8_t dummy_ist2_stack[16];

const void *tss_ist1_stack(void) {
    return dummy_ist1_stack;
}

size_t tss_ist1_stack_size(void) {
    return sizeof(dummy_ist1_stack);
}

const void *tss_ist2_stack(void) {
    return dummy_ist2_stack;
}

size_t tss_ist2_stack_size(void) {
    return sizeof(dummy_ist2_stack);
}

enum lapic_timer_status lapic_timer_status(void) {
    return mock_timer_status;
}

bool lapic_timer_runtime_prepare(void) {
    trace_add("prepare");
    if (!mock_prepare_ok) {
        return false;
    }
    mock_runtime_status = LAPIC_TIMER_RUNTIME_PREPARED;
    return true;
}

bool lapic_timer_runtime_activate(void) {
    trace_add("activate");
    if (!mock_activate_ok) {
        return false;
    }
    mock_runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;
    mock_runtime_active = true;
    trace_add("publish");
    return true;
}

void lapic_timer_runtime_abort(void) {
    trace_add("abort");
    mock_runtime_status = LAPIC_TIMER_RUNTIME_FAILED;
    mock_runtime_active = false;
}

bool lapic_timer_runtime_active(void) {
    return mock_runtime_active;
}

enum lapic_timer_runtime_status lapic_timer_runtime_status(void) {
    return mock_runtime_status;
}

/*
 * ITEM-475: the interrupt host proof drives the transition-allowed path, so
 * the stub reports the drill/QEMU policy.  The production-PIT policy and the
 * pure tick-source mapping are proven by the lapic-timer host proof.
 */
bool lapic_timer_runtime_transition_allowed(void) {
    return true;
}

bool ioapic_irq0_mask_save(uint64_t *out_previous_rte) {
    trace_add("ioapic-mask");
    if (out_previous_rte != NULL) {
        *out_previous_rte = mock_saved_rte;
    }
    if (!mock_ioapic_mask_ok) {
        return false;
    }
    return true;
}

bool ioapic_irq0_restore(uint64_t previous_rte) {
    ++ioapic_restore_calls;
    mock_last_restored_rte = previous_rte;
    trace_add("ioapic-restore");
    return mock_ioapic_restore_ok;
}

uint8_t pic_master_mask(void) {
    return mock_pic_master;
}

bool pic_mask(uint8_t irq) {
    if (irq == 0U) {
        trace_add("pic-mask");
        if (!mock_pic_mask_ok) {
            return false;
        }
        mock_pic_master |= 1U;
    }
    return true;
}

bool pic_unmask(uint8_t irq) {
    if (irq == 0U) {
        ++pic_unmask_calls;
        trace_add("pic-unmask");
        if (!mock_pic_unmask_ok) {
            return false;
        }
        mock_pic_master &= (uint8_t)~1U;
    }
    return true;
}

bool pic_irq0_mask_save(uint8_t *out_previous_master_mask) {
    ++pic_save_calls;
    trace_add("pic-mask");
    if (out_previous_master_mask != NULL) {
        *out_previous_master_mask = mock_pic_master;
    }
    if (!mock_pic_save_ok) {
        return false;
    }
    mock_pic_master |= 1U;
    return true;
}

bool pic_master_mask_restore(uint8_t previous_master_mask) {
    ++pic_restore_calls;
    mock_last_restored_pic_mask = previous_master_mask;
    trace_add("pic-restore");
    if (!mock_pic_restore_ok) {
        return false;
    }
    mock_pic_master = previous_master_mask;
    return true;
}

bool ioapic_is_irq1_routed(void) {
    return mock_irq1_routed;
}

bool pic_disable_transactional(void) {
    ++pic_disable_calls;
    trace_add("pic-disable");
    return mock_pic_disable_ok;
}

uint64_t lapic_timer_ticks_per_second(void) {
    return mock_ticks_per_second;
}

bool lapic_timer_oneshot_arm(uint32_t lapic_ticks) {
    ++oneshot_arm_calls;
    mock_last_oneshot_ticks = lapic_ticks;
    trace_add("oneshot-arm");
    return mock_oneshot_arm_ok;
}

void lapic_timer_oneshot_handle_irq(void) {
    ++oneshot_handle_calls;
    trace_add("oneshot-handle");
}

bool lapic_timer_oneshot_active(void) {
    return mock_oneshot_active;
}

static void reset_case(void) {
    memset(&bsp_cpu, 0, sizeof(bsp_cpu));
    memset(&ap_cpu, 0, sizeof(ap_cpu));
    bsp_cpu.index = 0U;
    bsp_cpu.state = CPU_STATE_BSP;
    ap_cpu.index = 1U;
    ap_cpu.state = CPU_STATE_ONLINE;
    cpu_host_test_set_current(&bsp_cpu);
    trace[0] = '\0';
    trace_length = 0U;
    timer_calls = 0U;
    keyboard_calls = 0U;
    lapic_eoi_calls = 0U;
    pic_eoi_calls = 0U;
    ioapic_restore_calls = 0U;
    pic_save_calls = 0U;
    pic_restore_calls = 0U;
    pic_unmask_calls = 0U;
    pic_disable_calls = 0U;
    oneshot_arm_calls = 0U;
    oneshot_handle_calls = 0U;
    mock_oneshot_arm_ok = true;
    mock_oneshot_active = false;
    mock_ticks_per_second = 6250000U;
    mock_last_oneshot_ticks = 0U;
    mock_prepare_ok = true;
    mock_activate_ok = true;
    mock_ioapic_mask_ok = true;
    mock_ioapic_restore_ok = true;
    mock_pic_mask_ok = true;
    mock_pic_save_ok = true;
    mock_pic_restore_ok = true;
    mock_pic_unmask_ok = true;
    mock_irq1_routed = false;
    mock_pic_disable_ok = true;
    mock_timer_status = LAPIC_TIMER_OK;
    mock_runtime_status = LAPIC_TIMER_RUNTIME_IDLE;
    mock_runtime_active = false;
    mock_saved_rte = ((uint64_t)0x05000000U << 32U) | IOAPIC_IRQ0_VECTOR;
    mock_last_restored_rte = 0U;
    mock_pic_master = 0xFEU;
    mock_last_restored_pic_mask = 0U;
    lapic_timer_runtime_host_test_reset();
    irq0_ioapic_active_publish(false);
    sched_host_test_reset();
    memset(&tick_task_a, 0, sizeof(tick_task_a));
    memset(&tick_task_b, 0, sizeof(tick_task_b));
    memset(&tick_switch_current, 0, sizeof(tick_switch_current));
    memset(&tick_switch_next, 0, sizeof(tick_switch_next));
    tick_current = NULL;
    tick_switch_calls = 0U;
}

static void scheduler_fresh_ready(void) {
    CHECK(sched_init() == SCHEDULER_OK,
          "tick dispatch scheduler initializes");
    tick_task_a.state = TASK_STATE_CREATED;
    tick_task_b.state = TASK_STATE_CREATED;
    CHECK(sched_host_test_set_switch_hook(record_tick_switch) == NULL,
          "tick dispatch switch hook installs with no previous hook");
    CHECK(context_frame_init(&tick_task_a.context,
                             (uintptr_t)0xffff800000010000U,
                             (void (*)(void))0x1000U) == CONTEXT_OK,
          "tick dispatch running A context initializes");
    CHECK(sched_enqueue(&tick_task_a) == SCHEDULER_OK,
          "tick dispatch running A enqueues");
    CHECK(sched_yield() == SCHEDULER_OK,
          "tick dispatch idle selects running A");
    CHECK(sched_current(&tick_current) == SCHEDULER_OK &&
              tick_current == &tick_task_a,
          "tick dispatch publishes A as running");
    tick_switch_calls = 0U;
    CHECK(context_frame_init(&tick_task_b.context,
                             (uintptr_t)0xffff800000011000U,
                             (void (*)(void))0x2000U) == CONTEXT_OK,
          "tick dispatch fresh successor B context initializes");
    CHECK(sched_enqueue(&tick_task_b) == SCHEDULER_OK,
          "tick dispatch fresh successor B enqueues");
}

static void check_trace(const char *expected) {
    if (strcmp(trace, expected) != 0) {
        fprintf(stderr, "FAIL: trace expected \"%s\", got \"%s\"\n",
                expected, trace);
        ++failures;
        return;
    }
    printf("PASS: trace \"%s\"\n", expected);
}

static void test_ioapic_success(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);
    CHECK(timer_calls == 1U,
          "IOAPIC success calls timer_handle_irq once");
    CHECK(lapic_eoi_calls == 1U,
          "IOAPIC success sends normal LAPIC EOI");
    CHECK(pic_eoi_calls == 0U,
          "IOAPIC success sends no PIC EOI");
    CHECK(ioapic_restore_calls == 0U,
          "IOAPIC success does not restore source");
    CHECK(mock_runtime_active,
          "IOAPIC success publishes runtime active");
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    CHECK(oneshot_arm_calls == 1U,
          "IOAPIC success runs test one-shot hook once");
    CHECK(mock_last_oneshot_ticks == 62500U,
          "test one-shot hook uses bounded 10ms count");
    check_trace(
        "timer prepare ioapic-mask activate publish oneshot-arm lapic_eoi");
#else
    CHECK(oneshot_arm_calls == 0U,
          "base runtime harness does not run one-shot hook");
    check_trace(
        "timer prepare ioapic-mask activate publish lapic_eoi");
    CHECK(strstr(trace,
                 "timer prepare ioapic-mask activate publish lapic_eoi")
              != NULL,
          "IOAPIC success retains base runtime trace");
#endif
}

static void test_pic_success(void) {
    reset_case();
    struct interrupt_frame frame = { .vector = 0x20U };
    interrupt_dispatch(&frame);
    CHECK(timer_calls == 1U,
          "PIC success calls timer_handle_irq once");
    CHECK(pic_eoi_calls == 1U,
          "PIC success sends normal PIC EOI");
    CHECK(lapic_eoi_calls == 0U,
          "PIC success sends no LAPIC EOI");
    CHECK(pic_save_calls == 1U,
          "PIC success uses transactional PIC IRQ0 mask save");
    CHECK(mock_runtime_active,
          "PIC success publishes runtime active");
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    CHECK(oneshot_arm_calls == 1U,
          "PIC success runs test one-shot hook once");
    check_trace(
        "timer prepare pic-mask activate publish oneshot-arm pic_eoi");
#else
    CHECK(oneshot_arm_calls == 0U,
          "base runtime harness does not run one-shot hook");
    check_trace(
        "timer prepare pic-mask activate publish pic_eoi");
    CHECK(strstr(trace,
                 "timer prepare pic-mask activate publish pic_eoi") != NULL,
          "PIC success retains base runtime trace");
#endif
}

static void test_prepare_failure(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_prepare_ok = false;
    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);
    CHECK(timer_calls == 1U,
          "prepare failure still accounts the PIT tick");
    CHECK(lapic_eoi_calls == 1U,
          "prepare failure sends normal LAPIC EOI");
    CHECK(!mock_runtime_active,
          "prepare failure does not publish active");
    CHECK(mock_runtime_status == LAPIC_TIMER_RUNTIME_FAILED,
          "prepare failure publishes FAILED");
    check_trace("timer prepare abort lapic_eoi");
}

static void test_ioapic_mask_failure(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_ioapic_mask_ok = false;
    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);
    CHECK(ioapic_restore_calls == 0U,
          "IOAPIC mask failure does not restore unpublished state");
    CHECK(!mock_runtime_active,
          "IOAPIC mask failure does not publish active");
    CHECK(mock_runtime_status == LAPIC_TIMER_RUNTIME_FAILED,
          "IOAPIC mask failure publishes FAILED");
    check_trace("timer prepare ioapic-mask abort lapic_eoi");
}

static void test_pic_mask_failure(void) {
    reset_case();
    mock_pic_mask_ok = false;
    mock_pic_save_ok = false;
    struct interrupt_frame frame = { .vector = 0x20U };
    interrupt_dispatch(&frame);
    CHECK(pic_save_calls == 1U,
          "PIC mask failure uses transactional PIC IRQ0 mask save");
    CHECK(pic_restore_calls == 0U,
          "PIC mask failure does not restore unpublished state");
    CHECK(pic_unmask_calls == 0U,
          "PIC mask failure does not use pic_unmask rollback");
    CHECK(mock_pic_master == 0xFEU,
          "PIC mask failure leaves prior mask unchanged");
    CHECK(mock_runtime_status == LAPIC_TIMER_RUNTIME_FAILED,
          "PIC mask failure publishes FAILED");
    check_trace("timer prepare pic-mask abort pic_eoi");
}

static void test_activate_failure_ioapic(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_activate_ok = false;
    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);
    CHECK(ioapic_restore_calls == 1U,
          "IOAPIC activate failure restores saved RTE");
    CHECK(mock_last_restored_rte == mock_saved_rte,
          "IOAPIC activate failure restores exact saved RTE");
    CHECK(!mock_runtime_active,
          "IOAPIC activate failure does not publish active");
    CHECK(mock_runtime_status == LAPIC_TIMER_RUNTIME_FAILED,
          "IOAPIC activate failure publishes FAILED");
    check_trace(
        "timer prepare ioapic-mask activate abort ioapic-restore lapic_eoi");
}

static void test_activate_failure_pic(void) {
    reset_case();
    mock_activate_ok = false;
    struct interrupt_frame frame = { .vector = 0x20U };
    interrupt_dispatch(&frame);
    CHECK(pic_restore_calls == 1U,
          "PIC activate failure restores exact previous master mask");
    CHECK(mock_last_restored_pic_mask == 0xFEU,
          "PIC activate failure restores exact FE byte");
    CHECK(mock_pic_master == 0xFEU,
          "PIC activate failure restores exact previous mask");
    CHECK(!mock_runtime_active,
          "PIC activate failure does not publish active");
    CHECK(mock_runtime_status == LAPIC_TIMER_RUNTIME_FAILED,
          "PIC activate failure publishes FAILED");
    check_trace(
        "timer prepare pic-mask activate abort pic-restore pic_eoi");
}

static void test_runtime_owned_vectors(void) {
    reset_case();
    mock_runtime_active = true;
    mock_runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;

    struct interrupt_frame lapic = { .vector = 0x42U };
    interrupt_dispatch(&lapic);
    CHECK(timer_calls == 1U,
          "runtime active vector 0x42 calls timer_handle_irq once");
    CHECK(lapic_eoi_calls == 1U,
          "runtime active vector 0x42 sends LAPIC EOI");
    CHECK(pic_eoi_calls == 0U,
          "runtime active vector 0x42 sends no PIC EOI");
    check_trace("timer lapic_eoi sched_on_tick");

    reset_case();
    mock_runtime_active = true;
    mock_runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;
    irq0_ioapic_active_publish(true);
    struct interrupt_frame ioapic = { .vector = 0x40U };
    interrupt_dispatch(&ioapic);
    CHECK(timer_calls == 0U,
          "runtime active vector 0x40 does not call timer_handle_irq");
    CHECK(lapic_eoi_calls == 1U,
          "runtime active vector 0x40 remains LAPIC EOI only");
    check_trace("lapic_eoi");

    reset_case();
    mock_runtime_active = true;
    mock_runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;
    struct interrupt_frame pic = { .vector = 0x20U };
    interrupt_dispatch(&pic);
    CHECK(timer_calls == 0U,
          "runtime active vector 0x20 does not call timer_handle_irq");
    CHECK(pic_eoi_calls == 1U,
          "runtime active vector 0x20 remains PIC EOI only");
    CHECK(lapic_eoi_calls == 0U,
          "runtime active vector 0x20 sends no LAPIC EOI");
    check_trace("pic_eoi");
}

static void test_runtime_tick_preempts_fresh_successor(void) {
    reset_case();
    mock_runtime_active = true;
    mock_runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;
    scheduler_fresh_ready();

    struct interrupt_frame timer = { .vector = 0x42U };
    interrupt_dispatch(&timer);

    CHECK(timer_calls == 1U,
          "tick dispatch calls timer_handle_irq once");
    CHECK(lapic_eoi_calls == 1U,
          "tick dispatch sends LAPIC EOI before sched_on_tick");
    CHECK(tick_switch_calls == 1U,
          "tick dispatch preempts running A to fresh successor B");
    CHECK(tick_task_a.state == TASK_STATE_RUNNABLE && tick_task_a.queued,
          "tick dispatch re-enqueues preempted A as RUNNABLE");
    CHECK(tick_task_b.state == TASK_STATE_RUNNING && !tick_task_b.queued,
          "tick dispatch publishes fresh successor B as RUNNING");
    CHECK(sched_current(&tick_current) == SCHEDULER_OK &&
              tick_current == &tick_task_b,
          "tick dispatch publishes B as current");
    CHECK(tick_switch_next.rsp == tick_task_b.context.rsp &&
              tick_switch_next.rip == tick_task_b.context.rip,
          "tick dispatch restores fresh successor B context");
    check_trace("timer lapic_eoi sched_on_tick");
}

#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
static void test_oneshot_vector_dispatch(void) {
    reset_case();
    struct interrupt_frame frame = { .vector = 0x43U };
    interrupt_dispatch(&frame);
    CHECK(oneshot_handle_calls == 1U,
          "vector 0x43 dispatches one-shot handler once");
    CHECK(timer_calls == 0U,
          "vector 0x43 does not own ticks");
    CHECK(lapic_eoi_calls == 1U && pic_eoi_calls == 0U,
          "vector 0x43 sends LAPIC EOI only");
    check_trace("oneshot-handle lapic_eoi");
}
#endif

static void test_runtime_without_irq1_does_not_disable_pic(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_irq1_routed = false;

    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);

    CHECK(pic_disable_calls == 0U,
          "runtime without verified IRQ1 does not disable PIC");
    CHECK(!pic_disable_host_test_is_published(),
          "missing IRQ1 precondition does not publish PIC disable");
    CHECK(mock_runtime_active,
          "runtime still activates without verified IRQ1");
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    CHECK(oneshot_arm_calls == 1U,
          "test one-shot hook runs after runtime publish");
    check_trace(
        "timer prepare ioapic-mask activate publish oneshot-arm lapic_eoi");
#else
    CHECK(oneshot_arm_calls == 0U,
          "base runtime harness does not run one-shot hook");
    check_trace(
        "timer prepare ioapic-mask activate publish lapic_eoi");
#endif
}

static void test_active_runtime_with_irq1_disables_pic_once(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_irq1_routed = true;

    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);

    CHECK(pic_disable_calls == 1U,
          "active runtime with verified IRQ1 disables PIC once");
    CHECK(pic_disable_host_test_is_published(),
          "successful PIC transaction publishes disabled state");
    CHECK(mock_runtime_active,
          "runtime remains active after PIC disable policy");
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    CHECK(oneshot_arm_calls == 1U,
          "test one-shot hook runs after PIC-disable policy");
    check_trace(
        "timer prepare ioapic-mask activate publish pic-disable "
        "oneshot-arm lapic_eoi");
#else
    CHECK(oneshot_arm_calls == 0U,
          "base runtime harness does not run one-shot hook");
    check_trace(
        "timer prepare ioapic-mask activate publish pic-disable lapic_eoi");
#endif

    struct interrupt_frame timer = { .vector = 0x42U };
    interrupt_dispatch(&timer);
    CHECK(pic_disable_calls == 1U,
          "later runtime ticks do not retry PIC-disable policy");
}

static void test_failed_pic_transaction_not_published(void) {
    reset_case();
    irq0_ioapic_active_publish(true);
    mock_irq1_routed = true;
    mock_pic_disable_ok = false;

    struct interrupt_frame frame = { .vector = 0x40U };
    interrupt_dispatch(&frame);

    CHECK(pic_disable_calls == 1U,
          "failed PIC transaction is still attempted once");
    CHECK(!pic_disable_host_test_is_published(),
          "failed PIC transaction does not publish disabled state");
    CHECK(mock_runtime_active,
          "runtime stays active after PIC disable failure");
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    CHECK(oneshot_arm_calls == 1U,
          "test one-shot hook still runs after PIC-disable failure");
    check_trace(
        "timer prepare ioapic-mask activate publish pic-disable "
        "oneshot-arm lapic_eoi");
#else
    CHECK(oneshot_arm_calls == 0U,
          "base runtime harness does not run one-shot hook");
    check_trace(
        "timer prepare ioapic-mask activate publish pic-disable lapic_eoi");
#endif
}

static void test_ap_policy(void) {
    reset_case();
    mock_runtime_active = true;
    cpu_host_test_set_current(&ap_cpu);
    struct interrupt_frame timer = { .vector = 0x42U };
    interrupt_dispatch(&timer);
    CHECK(timer_calls == 0U,
          "vector 0x42 on AP never calls timer_handle_irq");
    CHECK(lapic_eoi_calls == 0U,
          "vector 0x42 on AP does not call BSP lapic_eoi");
    CHECK(pic_eoi_calls == 0U,
          "vector 0x42 on AP does not send PIC EOI");

    reset_case();
    cpu_host_test_set_current(&ap_cpu);
    struct interrupt_frame keyboard = { .vector = 0x41U };
    interrupt_dispatch(&keyboard);
    CHECK(keyboard_calls == 0U,
          "vector 0x41 on AP never calls keyboard_handle_irq");
    CHECK(lapic_eoi_calls == 0U,
          "vector 0x41 on AP does not call BSP lapic_eoi");
    CHECK(pic_eoi_calls == 0U,
          "vector 0x41 on AP does not send PIC EOI");
}

int main(void) {
    test_ioapic_success();
    test_pic_success();
    test_prepare_failure();
    test_ioapic_mask_failure();
    test_pic_mask_failure();
    test_activate_failure_ioapic();
    test_activate_failure_pic();
    test_runtime_owned_vectors();
    test_runtime_tick_preempts_fresh_successor();
#ifdef HFOC_LAPIC_TIMER_ONESHOT_TEST
    test_oneshot_vector_dispatch();
#endif
    test_runtime_without_irq1_does_not_disable_pic();
    test_active_runtime_with_irq1_disables_pic_once();
    test_failed_pic_transaction_not_published();
    test_ap_policy();
    if (failures != 0) {
        printf("LAPIC timer runtime host tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("LAPIC timer runtime host tests: ALL OK");
    return 0;
}

#else
int main(void) {
    return 0;
}
#endif
