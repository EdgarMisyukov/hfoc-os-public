#include "lapic.h"
#include "lapic_timer.h"
#include "serial.h"
#include <stdint.h>

#define LAPIC_TIMER_CALIBRATION_SAMPLES 10U
#define LAPIC_TIMER_DIVIDE_BY_16 0x3U
#define LAPIC_TIMER_INITIAL_COUNT 0xFFFFFFFFU
#define LAPIC_TIMER_MASKED_VALUE 0x42U
#define LAPIC_TIMER_RUNTIME_HZ 100U
#define LAPIC_TIMER_ONESHOT_VECTOR 0x43U

static enum lapic_timer_status status = LAPIC_TIMER_IDLE;
static uint32_t pit_hz;
static uint32_t pit_samples;
static uint32_t initial_count;
static uint64_t ticks_per_second;
static enum lapic_timer_runtime_status runtime_status =
    LAPIC_TIMER_RUNTIME_IDLE;
static uint32_t runtime_initial_count;

enum lapic_timer_oneshot_state {
    LAPIC_TIMER_ONESHOT_IDLE,
    LAPIC_TIMER_ONESHOT_ARMED,
    LAPIC_TIMER_ONESHOT_RESTORED,
    LAPIC_TIMER_ONESHOT_FAILED,
};

static enum lapic_timer_oneshot_state oneshot_state =
    LAPIC_TIMER_ONESHOT_IDLE;
static uint32_t oneshot_saved_lvt;
static uint32_t oneshot_saved_divide;
static uint32_t oneshot_saved_initial;
static uint32_t oneshot_initial_count;

#ifdef HFOC_LAPIC_TIMER_HOST_TEST
bool lapic_timer_test_hw_arm(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_test_hw_current_count(uint32_t *out_count);
bool lapic_timer_test_program_masked_periodic(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_test_readback(
    uint32_t *out_lvt,
    uint32_t *out_divide,
    uint32_t *out_initial_count);
bool lapic_timer_test_unmask_periodic(uint8_t vector);
bool lapic_timer_test_mask_periodic(uint8_t vector);
bool lapic_timer_test_program_masked_oneshot(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_test_unmask_oneshot(uint8_t vector);
void lapic_timer_test_reset(void);
void lapic_timer_test_force_elapsed(uint64_t elapsed);

static uint64_t test_forced_elapsed;

void lapic_timer_test_reset(void) {
    status = LAPIC_TIMER_IDLE;
    pit_hz = 0U;
    pit_samples = 0U;
    initial_count = 0U;
    ticks_per_second = 0U;
    test_forced_elapsed = 0U;
    runtime_status = LAPIC_TIMER_RUNTIME_IDLE;
    runtime_initial_count = 0U;
    oneshot_state = LAPIC_TIMER_ONESHOT_IDLE;
    oneshot_saved_lvt = 0U;
    oneshot_saved_divide = 0U;
    oneshot_saved_initial = 0U;
    oneshot_initial_count = 0U;
}

void lapic_timer_test_force_elapsed(uint64_t elapsed) {
    test_forced_elapsed = elapsed;
}

#define lapic_timer_hw_arm lapic_timer_test_hw_arm
#define lapic_timer_hw_current_count lapic_timer_test_hw_current_count
#define lapic_timer_hw_program_masked_periodic \
    lapic_timer_test_program_masked_periodic
#define lapic_timer_hw_readback lapic_timer_test_readback
#define lapic_timer_hw_unmask_periodic lapic_timer_test_unmask_periodic
#define lapic_timer_hw_mask_periodic lapic_timer_test_mask_periodic
#define lapic_timer_hw_program_masked_oneshot \
    lapic_timer_test_program_masked_oneshot
#define lapic_timer_hw_unmask_oneshot lapic_timer_test_unmask_oneshot
#endif

static void emit_success_diagnostic(void) {
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[LAPIC-TIMER] calibrated: hz=");
        serial_write_u64(ticks_per_second);
        serial_write(" samples=");
        serial_write_u64(LAPIC_TIMER_CALIBRATION_SAMPLES);
        serial_write(" divide=16; PIT retained\n");
        serial_sequence_end(serial_flags);
    }
}

static void emit_unavailable_diagnostic(const char *reason) {
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[LAPIC-TIMER] unavailable: ");
        serial_write(reason);
        serial_write("; PIT retained\n");
        serial_sequence_end(serial_flags);
    }
}

static void emit_runtime_failure_diagnostic(void) {
    serial_write("[LAPIC-TIMER] runtime failed: vector=0x42; PIT retained\n");
}

static void emit_runtime_success_diagnostic(void) {
    serial_write("[LAPIC-TIMER] runtime: vector=0x42 hz=100; "
                 "PIT sources masked\n");
}

enum lapic_timer_status lapic_timer_status(void) {
    return status;
}

/*
 * ITEM-475 (round 14): the tick source the build policy allows and the one
 * currently in force.
 *
 * `lapic_timer_runtime_transition_allowed()` is the single place the
 * production-PIT policy is expressed.  It is compiled to `false` only in the
 * ordinary production build (`HFOC_PRODUCTION_PIT_TICK`), so the interrupt
 * paths are free of the Local APIC transition there while every drill/QEMU
 * build keeps the calibrated periodic timer and its proofs.  The pure
 * `lapic_timer_tick_source_for()` is what the host proof drives.
 */
bool lapic_timer_runtime_transition_allowed(void) {
#ifdef HFOC_PRODUCTION_PIT_TICK
    return false;
#else
    return true;
#endif
}

enum lapic_timer_tick_source lapic_timer_tick_source_for(bool runtime_active) {
    return runtime_active ? LAPIC_TIMER_TICK_SOURCE_LAPIC
                          : LAPIC_TIMER_TICK_SOURCE_PIT;
}

enum lapic_timer_tick_source lapic_timer_tick_source(void) {
    return lapic_timer_tick_source_for(lapic_timer_runtime_active());
}

const char *lapic_timer_tick_source_string(
    enum lapic_timer_tick_source source) {
    return source == LAPIC_TIMER_TICK_SOURCE_LAPIC ? "lapic" : "pit";
}

static const char *lapic_timer_tick_policy_text(void) {
#ifdef HFOC_PRODUCTION_PIT_TICK
    return "[LAPIC-TIMER] tick policy: pit";
#else
    return "[LAPIC-TIMER] tick policy: lapic-transition";
#endif
}

uint64_t lapic_timer_ticks_per_second(void) {
    return ticks_per_second;
}

bool lapic_timer_runtime_prepare(void) {
    if (status != LAPIC_TIMER_OK ||
        runtime_status != LAPIC_TIMER_RUNTIME_IDLE) {
        return false;
    }
    const uint64_t initial =
        ticks_per_second / LAPIC_TIMER_RUNTIME_HZ;
    if (initial == 0U || initial > UINT32_MAX) {
        return false;
    }
    runtime_initial_count = (uint32_t)initial;

    if (!lapic_timer_hw_program_masked_periodic(
            LAPIC_TIMER_MASKED_VALUE,
            LAPIC_TIMER_DIVIDE_BY_16,
            runtime_initial_count)) {
        return false;
    }

    uint32_t lvt = 0U;
    uint32_t divide = 0U;
    uint32_t count = 0U;
    if (!lapic_timer_hw_readback(&lvt, &divide, &count)) {
        return false;
    }
    const uint32_t expected_lvt =
        LAPIC_TIMER_MASKED_VALUE | (1U << 17U) | (1U << 16U);
    if (lvt != expected_lvt ||
        divide != LAPIC_TIMER_DIVIDE_BY_16 ||
        count != runtime_initial_count) {
        return false;
    }

    runtime_status = LAPIC_TIMER_RUNTIME_PREPARED;
    return true;
}

bool lapic_timer_runtime_activate(void) {
    if (runtime_status != LAPIC_TIMER_RUNTIME_PREPARED) {
        return false;
    }
    if (!lapic_timer_hw_unmask_periodic(LAPIC_TIMER_MASKED_VALUE)) {
        return false;
    }

    uint32_t lvt = 0U;
    uint32_t divide = 0U;
    uint32_t count = 0U;
    if (!lapic_timer_hw_readback(&lvt, &divide, &count)) {
        return false;
    }
    const uint32_t expected_lvt =
        LAPIC_TIMER_MASKED_VALUE | (1U << 17U);
    if (lvt != expected_lvt ||
        divide != LAPIC_TIMER_DIVIDE_BY_16 ||
        count != runtime_initial_count) {
        return false;
    }

    runtime_status = LAPIC_TIMER_RUNTIME_ACTIVE;
    emit_runtime_success_diagnostic();
    return true;
}

void lapic_timer_runtime_abort(void) {
    if (runtime_status == LAPIC_TIMER_RUNTIME_FAILED) {
        return;
    }
    (void)lapic_timer_hw_mask_periodic(LAPIC_TIMER_MASKED_VALUE);
    uint32_t lvt = 0U;
    uint32_t divide = 0U;
    uint32_t count = 0U;
    (void)lapic_timer_hw_readback(&lvt, &divide, &count);
    runtime_status = LAPIC_TIMER_RUNTIME_FAILED;
    emit_runtime_failure_diagnostic();
}

bool lapic_timer_runtime_active(void) {
    return runtime_status == LAPIC_TIMER_RUNTIME_ACTIVE;
}

enum lapic_timer_runtime_status lapic_timer_runtime_status(void) {
    return runtime_status;
}

static bool oneshot_readback_matches(
    uint32_t expected_lvt,
    uint32_t expected_divide,
    uint32_t expected_initial)
{
    uint32_t lvt = 0U;
    uint32_t divide = 0U;
    uint32_t initial = 0U;
    if (!lapic_timer_hw_readback(&lvt, &divide, &initial)) {
        return false;
    }
    return lvt == expected_lvt &&
           divide == expected_divide &&
           initial == expected_initial;
}

static bool restore_saved_periodic(void) {
    const uint32_t masked_lvt = oneshot_saved_lvt | (1U << 16U);
    if (!lapic_timer_hw_program_masked_periodic(
            LAPIC_TIMER_MASKED_VALUE,
            oneshot_saved_divide,
            oneshot_saved_initial)) {
        return false;
    }
    if (!oneshot_readback_matches(
            masked_lvt,
            oneshot_saved_divide,
            oneshot_saved_initial)) {
        return false;
    }
    if (!lapic_timer_hw_unmask_periodic(LAPIC_TIMER_MASKED_VALUE)) {
        return false;
    }
    return oneshot_readback_matches(
        oneshot_saved_lvt,
        oneshot_saved_divide,
        oneshot_saved_initial);
}

static bool oneshot_fail_safe_remask(void) {
    const uint32_t masked_lvt = oneshot_saved_lvt | (1U << 16U);
    if (!lapic_timer_hw_program_masked_periodic(
            LAPIC_TIMER_MASKED_VALUE,
            oneshot_saved_divide,
            oneshot_saved_initial)) {
        return false;
    }
    return oneshot_readback_matches(
        masked_lvt,
        oneshot_saved_divide,
        oneshot_saved_initial);
}

static void oneshot_restore_failed(void) {
    const bool masked_confirmed = oneshot_fail_safe_remask();
    oneshot_state = LAPIC_TIMER_ONESHOT_FAILED;
    if (masked_confirmed) {
        serial_write(
            "[LAPIC-TIMER] oneshot restore failed; LVT masked\n");
    } else {
        serial_write(
            "[LAPIC-TIMER] oneshot restore failed; "
            "LVT state not confirmed\n");
    }
}

static void oneshot_arm_failed(void) {
    (void)oneshot_fail_safe_remask();
    oneshot_state = LAPIC_TIMER_ONESHOT_FAILED;
}

bool lapic_timer_oneshot_arm(uint32_t lapic_ticks) {
    if (status != LAPIC_TIMER_OK ||
        runtime_status != LAPIC_TIMER_RUNTIME_ACTIVE ||
        oneshot_state != LAPIC_TIMER_ONESHOT_IDLE ||
        lapic_ticks == 0U) {
        return false;
    }

    if (!lapic_timer_hw_readback(
            &oneshot_saved_lvt,
            &oneshot_saved_divide,
            &oneshot_saved_initial)) {
        return false;
    }

    const uint32_t masked_periodic_lvt =
        oneshot_saved_lvt | (1U << 16U);
    if (!lapic_timer_hw_mask_periodic(LAPIC_TIMER_MASKED_VALUE)) {
        oneshot_arm_failed();
        return false;
    }
    if (!oneshot_readback_matches(
            masked_periodic_lvt,
            oneshot_saved_divide,
            oneshot_saved_initial)) {
        oneshot_arm_failed();
        return false;
    }

    oneshot_initial_count = lapic_ticks;
    const uint32_t masked_oneshot_lvt =
        (uint32_t)LAPIC_TIMER_ONESHOT_VECTOR | (1U << 16U);
    if (!lapic_timer_hw_program_masked_oneshot(
            LAPIC_TIMER_ONESHOT_VECTOR,
            oneshot_saved_divide,
            oneshot_initial_count)) {
        oneshot_arm_failed();
        return false;
    }
    if (!oneshot_readback_matches(
            masked_oneshot_lvt,
            oneshot_saved_divide,
            oneshot_initial_count)) {
        oneshot_arm_failed();
        return false;
    }

    if (!lapic_timer_hw_unmask_oneshot(LAPIC_TIMER_ONESHOT_VECTOR)) {
        oneshot_arm_failed();
        return false;
    }
    if (!oneshot_readback_matches(
            LAPIC_TIMER_ONESHOT_VECTOR,
            oneshot_saved_divide,
            oneshot_initial_count)) {
        oneshot_arm_failed();
        return false;
    }

    oneshot_state = LAPIC_TIMER_ONESHOT_ARMED;
    return true;
}

void lapic_timer_oneshot_handle_irq(void) {
    if (oneshot_state != LAPIC_TIMER_ONESHOT_ARMED) {
        return;
    }

    if (!lapic_timer_hw_program_masked_oneshot(
            LAPIC_TIMER_ONESHOT_VECTOR,
            oneshot_saved_divide,
            oneshot_initial_count)) {
        oneshot_restore_failed();
        return;
    }

    if (!restore_saved_periodic()) {
        oneshot_restore_failed();
        return;
    }

    oneshot_state = LAPIC_TIMER_ONESHOT_RESTORED;
    serial_write(
        "[LAPIC-TIMER] oneshot complete: periodic vector=0x42 restored\n");
}

bool lapic_timer_oneshot_active(void) {
    return oneshot_state == LAPIC_TIMER_ONESHOT_ARMED;
}

void lapic_timer_start_calibration(uint32_t pit_hz_arg) {
    if (status != LAPIC_TIMER_IDLE) {
        return;
    }
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(lapic_timer_tick_policy_text());
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    if (pit_hz_arg == 0U) {
        status = LAPIC_TIMER_BAD_SAMPLE;
        emit_unavailable_diagnostic("bad pit_hz");
        return;
    }
    if (!lapic_timer_hw_arm(
            LAPIC_TIMER_MASKED_VALUE,
            LAPIC_TIMER_DIVIDE_BY_16,
            LAPIC_TIMER_INITIAL_COUNT)) {
        status = LAPIC_TIMER_NO_LAPIC;
        emit_unavailable_diagnostic("no LAPIC");
        return;
    }
    pit_hz = pit_hz_arg;
    pit_samples = 0U;
    initial_count = LAPIC_TIMER_INITIAL_COUNT;
    status = LAPIC_TIMER_CALIBRATING;
}

void lapic_timer_on_pit_tick(void) {
    if (status != LAPIC_TIMER_CALIBRATING) {
        return;
    }
    ++pit_samples;
    if (pit_samples < LAPIC_TIMER_CALIBRATION_SAMPLES) {
        return;
    }

    uint32_t current_count = 0U;
    if (!lapic_timer_hw_current_count(&current_count)) {
        status = LAPIC_TIMER_BAD_SAMPLE;
        emit_unavailable_diagnostic("current count read failed");
        return;
    }
    if (current_count >= LAPIC_TIMER_INITIAL_COUNT) {
        status = LAPIC_TIMER_BAD_SAMPLE;
        emit_unavailable_diagnostic("non-decrementing sample");
        return;
    }

#ifdef HFOC_LAPIC_TIMER_HOST_TEST
    const uint64_t elapsed =
        test_forced_elapsed != 0U
        ? test_forced_elapsed
        : (uint64_t)(initial_count - current_count);
#else
    const uint64_t elapsed = (uint64_t)(initial_count - current_count);
#endif

    if (elapsed > UINT64_MAX / pit_hz) {
        status = LAPIC_TIMER_OVERFLOW;
        emit_unavailable_diagnostic("arithmetic overflow");
        return;
    }
    ticks_per_second =
        elapsed * (uint64_t)pit_hz / LAPIC_TIMER_CALIBRATION_SAMPLES;
    if (ticks_per_second == 0U) {
        status = LAPIC_TIMER_BAD_SAMPLE;
        emit_unavailable_diagnostic("zero frequency");
        return;
    }
    status = LAPIC_TIMER_OK;
    emit_success_diagnostic();
}
