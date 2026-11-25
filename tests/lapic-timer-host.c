#include "lapic_timer.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LAPIC_TIMER_HOST_TEST

void lapic_timer_test_reset(void);
bool lapic_timer_test_hw_arm(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count);
bool lapic_timer_test_hw_current_count(uint32_t *out_count);
void lapic_timer_test_force_elapsed(uint64_t elapsed);
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

static uint32_t mock_current_count;
static uint32_t mock_arm_calls;
static uint32_t mock_read_calls;
static bool mock_arm_ok = true;
static bool mock_read_ok = true;
static uint32_t mock_lvt_value;
static uint32_t mock_divide_value;
static uint32_t mock_initial_value;
static bool mock_lvt_mask_cleared;
static bool mock_runtime_program_ok = true;
static bool mock_runtime_readback_ok = true;
static bool mock_runtime_unmask_ok = true;
static bool mock_runtime_mask_ok = true;
static bool mock_runtime_readback_override;
static uint32_t mock_runtime_readback_lvt;
static uint32_t mock_runtime_readback_divide;
static uint32_t mock_runtime_readback_initial;
static uint32_t mock_runtime_program_calls;
static uint32_t mock_runtime_readback_calls;
static uint32_t mock_runtime_unmask_calls;
static uint32_t mock_runtime_mask_calls;
static uint32_t mock_oneshot_program_calls;
static uint32_t mock_oneshot_unmask_calls;
static bool mock_oneshot_program_ok = true;
static bool mock_oneshot_unmask_ok = true;
static uint32_t mock_readback_fail_at;
static uint32_t mock_readback_override_at;
static uint32_t mock_readback_override_lvt;
static uint32_t mock_readback_override_divide;
static uint32_t mock_readback_override_initial;
static char serial_capture[1024];
static size_t serial_capture_length;

bool lapic_timer_test_hw_arm(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    ++mock_arm_calls;
    mock_divide_value = divide_config;
    mock_initial_value = initial_count;
    if (!mock_arm_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector | (1U << 16U);
    mock_lvt_mask_cleared = (mock_lvt_value & (1U << 16U)) == 0U;
    return true;
}

bool lapic_timer_test_hw_current_count(uint32_t *out_count) {
    ++mock_read_calls;
    if (!mock_read_ok) {
        return false;
    }
    if (out_count != NULL) {
        *out_count = mock_current_count;
    }
    return true;
}

bool lapic_timer_test_program_masked_periodic(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    ++mock_runtime_program_calls;
    if (!mock_runtime_program_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector | (1U << 17U) | (1U << 16U);
    mock_divide_value = divide_config;
    mock_initial_value = initial_count;
    return true;
}

bool lapic_timer_test_readback(
    uint32_t *out_lvt,
    uint32_t *out_divide,
    uint32_t *out_initial_count)
{
    ++mock_runtime_readback_calls;
    if (mock_readback_fail_at != 0U &&
        mock_runtime_readback_calls == mock_readback_fail_at) {
        return false;
    }
    if (mock_readback_override_at != 0U &&
        mock_runtime_readback_calls == mock_readback_override_at) {
        if (out_lvt == NULL ||
            out_divide == NULL ||
            out_initial_count == NULL) {
            return false;
        }
        *out_lvt = mock_readback_override_lvt;
        *out_divide = mock_readback_override_divide;
        *out_initial_count = mock_readback_override_initial;
        return true;
    }
    if (!mock_runtime_readback_ok ||
        out_lvt == NULL ||
        out_divide == NULL ||
        out_initial_count == NULL) {
        return false;
    }
    if (mock_runtime_readback_override) {
        *out_lvt = mock_runtime_readback_lvt;
        *out_divide = mock_runtime_readback_divide;
        *out_initial_count = mock_runtime_readback_initial;
    } else {
        *out_lvt = mock_lvt_value;
        *out_divide = mock_divide_value;
        *out_initial_count = mock_initial_value;
    }
    return true;
}

bool lapic_timer_test_unmask_periodic(uint8_t vector) {
    ++mock_runtime_unmask_calls;
    if (!mock_runtime_unmask_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector | (1U << 17U);
    return true;
}

bool lapic_timer_test_mask_periodic(uint8_t vector) {
    ++mock_runtime_mask_calls;
    if (!mock_runtime_mask_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector | (1U << 17U) | (1U << 16U);
    return true;
}

bool lapic_timer_test_program_masked_oneshot(
    uint8_t vector,
    uint32_t divide_config,
    uint32_t initial_count)
{
    ++mock_oneshot_program_calls;
    if (!mock_oneshot_program_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector | (1U << 16U);
    mock_divide_value = divide_config;
    mock_initial_value = initial_count;
    return true;
}

bool lapic_timer_test_unmask_oneshot(uint8_t vector) {
    ++mock_oneshot_unmask_calls;
    if (!mock_oneshot_unmask_ok) {
        return false;
    }
    mock_lvt_value = (uint32_t)vector;
    return true;
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void serial_write(const char *text) {
    while (*text != '\0'
           && serial_capture_length + 1U < sizeof(serial_capture)) {
        serial_capture[serial_capture_length++] = *text++;
    }
    serial_capture[serial_capture_length] = '\0';
}

void serial_write_u64(uint64_t value) {
    char buffer[32];
    const int length = snprintf(
        buffer, sizeof(buffer), "%llu", (unsigned long long)value);
    if (length > 0) {
        for (int i = 0;
             i < length && serial_capture_length + 1U < sizeof(serial_capture);
             ++i) {
            serial_capture[serial_capture_length++] = buffer[i];
        }
        serial_capture[serial_capture_length] = '\0';
    }
}

void serial_write_hex(uint64_t value) {
    (void)value;
}

static void reset_mocks(void) {
    mock_current_count = 0U;
    mock_arm_calls = 0U;
    mock_read_calls = 0U;
    mock_arm_ok = true;
    mock_read_ok = true;
    mock_lvt_value = 0U;
    mock_divide_value = 0U;
    mock_initial_value = 0U;
    mock_lvt_mask_cleared = false;
    mock_runtime_program_ok = true;
    mock_runtime_readback_ok = true;
    mock_runtime_unmask_ok = true;
    mock_runtime_mask_ok = true;
    mock_runtime_readback_override = false;
    mock_runtime_readback_lvt = 0U;
    mock_runtime_readback_divide = 0U;
    mock_runtime_readback_initial = 0U;
    mock_runtime_program_calls = 0U;
    mock_runtime_readback_calls = 0U;
    mock_runtime_unmask_calls = 0U;
    mock_runtime_mask_calls = 0U;
    mock_oneshot_program_calls = 0U;
    mock_oneshot_unmask_calls = 0U;
    mock_oneshot_program_ok = true;
    mock_oneshot_unmask_ok = true;
    mock_readback_fail_at = 0U;
    mock_readback_override_at = 0U;
    mock_readback_override_lvt = 0U;
    mock_readback_override_divide = 0U;
    mock_readback_override_initial = 0U;
    serial_capture_length = 0U;
    serial_capture[0] = '\0';
}

static bool serial_contains(const char *text) {
    return strstr(serial_capture, text) != NULL;
}

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
    else { printf("PASS: %s\n", msg); } \
} while (0)

static void begin_case(void) {
    reset_mocks();
    lapic_timer_test_reset();
}

static void assert_terminal_idempotence(enum lapic_timer_status expected) {
    CHECK(lapic_timer_status() == expected,
          "terminal status remains published");
    const uint32_t arm_calls = mock_arm_calls;
    const uint32_t read_calls = mock_read_calls;
    lapic_timer_start_calibration(100U);
    lapic_timer_on_pit_tick();
    CHECK(mock_arm_calls == arm_calls,
          "repeated terminal calls do not arm again");
    CHECK(mock_read_calls == read_calls,
          "repeated terminal calls do not read again");
}

static void calibrate_ok(uint64_t elapsed) {
    begin_case();
    lapic_timer_start_calibration(100U);
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
    }
    lapic_timer_test_force_elapsed(elapsed);
    mock_current_count = 0U;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_OK,
          "calibration publishes LAPIC_TIMER_OK");
}

int main(void) {
    /* 1. Initial idle state and zero result. */
    begin_case();
    CHECK(lapic_timer_status() == LAPIC_TIMER_IDLE,
          "initial status is idle");
    CHECK(lapic_timer_ticks_per_second() == 0U,
          "idle result is zero");
    CHECK(mock_arm_calls == 0U, "idle start has not armed");
    CHECK(mock_read_calls == 0U, "idle start has not read");

    /* 1b. Runtime public contract starts idle and rejects pre-calibration. */
    begin_case();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "runtime status is initially idle");
    CHECK(!lapic_timer_runtime_active(),
          "runtime is not active initially");
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare fails before calibration");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "failed precondition leaves runtime idle");

    /* 1b-2. ITEM-475: the tick source each runtime state names. */
    CHECK(lapic_timer_tick_source_for(false) == LAPIC_TIMER_TICK_SOURCE_PIT,
          "an inactive runtime names PIT as the tick source");
    CHECK(lapic_timer_tick_source_for(true) == LAPIC_TIMER_TICK_SOURCE_LAPIC,
          "an active runtime names the LAPIC timer as the tick source");
    CHECK(strcmp(lapic_timer_tick_source_string(LAPIC_TIMER_TICK_SOURCE_PIT),
                 "pit") == 0,
          "the PIT tick source string is 'pit'");
    CHECK(strcmp(lapic_timer_tick_source_string(LAPIC_TIMER_TICK_SOURCE_LAPIC),
                 "lapic") == 0,
          "the LAPIC tick source string is 'lapic'");
    CHECK(!lapic_timer_runtime_active(),
          "the tick source reads PIT while the runtime is idle");
    CHECK(lapic_timer_tick_source() == LAPIC_TIMER_TICK_SOURCE_PIT,
          "the live tick source is PIT before any transition");
#ifndef HFOC_PRODUCTION_PIT_TICK
    CHECK(lapic_timer_runtime_transition_allowed(),
          "the drill/QEMU host build allows the LAPIC runtime transition");
#endif

    /*
     * 1b-3. ITEM-475: the calibration publishes the tick policy this build
     * compiled in, so a reader of the serial log can tell a production-PIT
     * machine from a transition-capable drill build without inferring it.
     */
    begin_case();
    lapic_timer_start_calibration(100U);
#ifdef HFOC_PRODUCTION_PIT_TICK
    CHECK(serial_contains("[LAPIC-TIMER] tick policy: pit"),
          "the production build publishes the PIT tick policy");
#else
    CHECK(serial_contains("[LAPIC-TIMER] tick policy: lapic-transition"),
          "the drill/QEMU build publishes the LAPIC-transition tick policy");
#endif

    /* 1c. Runtime prepare/activate/abort state machine. */
    calibrate_ok(625000U);
    CHECK(lapic_timer_ticks_per_second() == 6250000ULL,
          "runtime fixture computes 6.25 MHz LAPIC frequency");
    CHECK(lapic_timer_runtime_prepare(),
          "runtime prepare succeeds after calibration");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_PREPARED,
          "runtime prepare publishes PREPARED");
    CHECK(mock_runtime_program_calls == 1U,
          "runtime prepare programs LVT once");
    CHECK(mock_lvt_value == 0x00030042U,
          "runtime prepare programs masked periodic LVT 0x42");
    CHECK(mock_divide_value == 0x3U,
          "runtime prepare programs divide-by-16");
    CHECK(mock_initial_value == 62500U,
          "runtime prepare programs exact 10 ms initial count");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "runtime prepare leaves LVT masked");
    CHECK(!lapic_timer_runtime_active(),
          "runtime prepare does not publish active");
    CHECK(mock_runtime_unmask_calls == 0U,
          "runtime prepare does not unmask LVT");

    CHECK(lapic_timer_runtime_activate(),
          "runtime activate succeeds from PREPARED");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_ACTIVE,
          "runtime activate publishes ACTIVE");
    CHECK(lapic_timer_runtime_active(),
          "runtime active getter reflects ACTIVE");
    CHECK(mock_runtime_unmask_calls == 1U,
          "runtime activate unmasks LVT once");
    CHECK((mock_lvt_value & (1U << 16U)) == 0U,
          "runtime activate clears LVT mask bit");
    CHECK(serial_contains(
              "[LAPIC-TIMER] runtime: vector=0x42 hz=100; "
              "PIT sources masked"),
          "runtime success diagnostic is present");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "abort fixture prepares runtime");
    const uint32_t abort_mask_calls = mock_runtime_mask_calls;
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "runtime abort publishes FAILED");
    CHECK(mock_runtime_mask_calls == abort_mask_calls + 1U,
          "runtime abort remasks periodic LVT");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "runtime abort leaves LVT masked");
    CHECK(serial_contains(
              "[LAPIC-TIMER] runtime failed: vector=0x42; PIT retained"),
          "runtime failure diagnostic contains PIT retained");
    const size_t abort_serial_length = serial_capture_length;
    const uint32_t abort_mask_after = mock_runtime_mask_calls;
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "runtime abort remains FAILED");
    CHECK(mock_runtime_mask_calls == abort_mask_after,
          "repeated runtime abort does not remask again");
    CHECK(serial_capture_length == abort_serial_length,
          "repeated runtime abort emits no new diagnostic");

    calibrate_ok(625000U);
    CHECK(!lapic_timer_runtime_activate(),
          "runtime activate before prepare fails");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "activate-before-prepare leaves runtime idle");
    CHECK(mock_runtime_unmask_calls == 0U,
          "activate-before-prepare does not unmask LVT");

    calibrate_ok(1U);
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects zero initial count");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "zero-count failure leaves runtime idle before abort");
    CHECK(mock_runtime_program_calls == 0U,
          "zero-count failure does not program LVT");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "zero-count failure publishes FAILED after abort");

    calibrate_ok(50000000000U);
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects overflow initial count");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "overflow failure leaves runtime idle before abort");
    CHECK(mock_runtime_program_calls == 0U,
          "overflow failure does not program LVT");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "overflow failure publishes FAILED after abort");

    calibrate_ok(625000U);
    mock_runtime_readback_ok = false;
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects failed readback");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "readback failure leaves runtime idle before abort");
    CHECK(mock_runtime_program_calls == 1U,
          "readback failure still programs masked LVT");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "readback failure publishes FAILED after abort");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "readback failure remasks LVT");

    calibrate_ok(625000U);
    mock_runtime_readback_override = true;
    mock_runtime_readback_lvt = 0x00030042U ^ (1U << 16U);
    mock_runtime_readback_divide = 0x3U;
    mock_runtime_readback_initial = 62500U;
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects LVT readback mismatch");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "LVT mismatch leaves runtime idle before abort");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "LVT mismatch publishes FAILED after abort");

    calibrate_ok(625000U);
    mock_runtime_readback_override = true;
    mock_runtime_readback_lvt = 0x00030042U;
    mock_runtime_readback_divide = 0x4U;
    mock_runtime_readback_initial = 62500U;
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects divide readback mismatch");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "divide mismatch leaves runtime idle before abort");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "divide mismatch publishes FAILED after abort");

    calibrate_ok(625000U);
    mock_runtime_readback_override = true;
    mock_runtime_readback_lvt = 0x00030042U;
    mock_runtime_readback_divide = 0x3U;
    mock_runtime_readback_initial = 62501U;
    CHECK(!lapic_timer_runtime_prepare(),
          "runtime prepare rejects initial-count readback mismatch");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_IDLE,
          "initial-count mismatch leaves runtime idle before abort");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "initial-count mismatch publishes FAILED after abort");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "unmask-mismatch fixture prepares runtime");
    mock_runtime_readback_override = true;
    mock_runtime_readback_lvt = 0x00030042U;
    mock_runtime_readback_divide = 0x3U;
    mock_runtime_readback_initial = 62500U;
    CHECK(!lapic_timer_runtime_activate(),
          "runtime activate rejects unmask readback mismatch");
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_PREPARED,
          "unmask mismatch stays PREPARED before abort");
    CHECK(!lapic_timer_runtime_active(),
          "unmask mismatch does not publish active");
    lapic_timer_runtime_abort();
    CHECK(lapic_timer_runtime_status() == LAPIC_TIMER_RUNTIME_FAILED,
          "unmask mismatch publishes FAILED after abort");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "unmask mismatch remasks LVT");

    /* 1d. Transactional one-shot state machine. */
    calibrate_ok(625000U);
    CHECK(!lapic_timer_oneshot_arm(1U),
          "one-shot rejects inactive runtime");
    CHECK(lapic_timer_runtime_prepare(),
          "oneshot fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "oneshot fixture activates runtime");
    CHECK(mock_oneshot_program_calls == 0U,
          "one-shot is not programmed before arm");
    CHECK(lapic_timer_oneshot_arm(62500U),
          "one-shot arms from active periodic runtime");
    CHECK(lapic_timer_oneshot_active(),
          "one-shot publishes armed only after exact readback");
    CHECK((mock_lvt_value & 0xFFU) == 0x43U,
          "armed one-shot uses vector 0x43");
    CHECK((mock_lvt_value & (1U << 16U)) == 0U,
          "armed one-shot is unmasked");
    CHECK(mock_oneshot_program_calls == 1U,
          "one-shot programs LVT once");
    CHECK(!lapic_timer_oneshot_arm(1U),
          "duplicate one-shot arm is rejected");
    CHECK(lapic_timer_oneshot_active(),
          "duplicate arm keeps existing armed state");
    lapic_timer_oneshot_handle_irq();
    CHECK(!lapic_timer_oneshot_active(),
          "one-shot completion clears armed state");
    CHECK(mock_lvt_value == 0x00020042U &&
              mock_divide_value == 0x3U &&
              mock_initial_value == 62500U,
          "completion restores exact unmasked periodic state");
    CHECK(serial_contains(
              "[LAPIC-TIMER] oneshot complete: periodic vector=0x42 restored"),
          "one-shot completion diagnostic is present");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "zero-count fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "zero-count fixture activates runtime");
    CHECK(!lapic_timer_oneshot_arm(0U),
          "one-shot rejects zero count");
    CHECK(!lapic_timer_oneshot_active(),
          "zero count does not publish armed");
    CHECK(mock_oneshot_program_calls == 0U,
          "zero count does not program one-shot");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "LVT mismatch fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "LVT mismatch fixture activates runtime");
    mock_readback_override_at = mock_runtime_readback_calls + 3U;
    mock_readback_override_lvt = 0x43U;
    mock_readback_override_divide = 0x3U;
    mock_readback_override_initial = 62500U;
    CHECK(!lapic_timer_oneshot_arm(62500U),
          "one-shot arm rejects LVT readback mismatch");
    CHECK(!lapic_timer_oneshot_active(),
          "LVT mismatch does not publish armed");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "divide mismatch fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "divide mismatch fixture activates runtime");
    mock_readback_override_at = mock_runtime_readback_calls + 3U;
    mock_readback_override_lvt = 0x43U | (1U << 16U);
    mock_readback_override_divide = 0x4U;
    mock_readback_override_initial = 62500U;
    CHECK(!lapic_timer_oneshot_arm(62500U),
          "one-shot arm rejects divide readback mismatch");
    CHECK(!lapic_timer_oneshot_active(),
          "divide mismatch does not publish armed");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "count mismatch fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "count mismatch fixture activates runtime");
    mock_readback_override_at = mock_runtime_readback_calls + 3U;
    mock_readback_override_lvt = 0x43U | (1U << 16U);
    mock_readback_override_divide = 0x3U;
    mock_readback_override_initial = 62501U;
    CHECK(!lapic_timer_oneshot_arm(62500U),
          "one-shot arm rejects initial-count readback mismatch");
    CHECK(!lapic_timer_oneshot_active(),
          "initial-count mismatch does not publish armed");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "restore mismatch fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "restore mismatch fixture activates runtime");
    CHECK(lapic_timer_oneshot_arm(62500U),
          "restore mismatch fixture arms one-shot");
    mock_readback_override_at = mock_runtime_readback_calls + 1U;
    mock_readback_override_lvt = 0x00030043U;
    mock_readback_override_divide = 0x3U;
    mock_readback_override_initial = 62500U;
    lapic_timer_oneshot_handle_irq();
    CHECK(!lapic_timer_oneshot_active(),
          "restore mismatch clears armed state");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "restore mismatch leaves LVT masked");
    CHECK(serial_contains(
              "[LAPIC-TIMER] oneshot restore failed; LVT masked"),
          "restore failure diagnostic is present");

    calibrate_ok(625000U);
    CHECK(lapic_timer_runtime_prepare(),
          "final restore mismatch fixture prepares runtime");
    CHECK(lapic_timer_runtime_activate(),
          "final restore mismatch fixture activates runtime");
    CHECK(lapic_timer_oneshot_arm(62500U),
          "final restore mismatch fixture arms one-shot");
    mock_readback_override_at = mock_runtime_readback_calls + 2U;
    mock_readback_override_lvt = 0x00020043U;
    mock_readback_override_divide = 0x3U;
    mock_readback_override_initial = 62500U;
    lapic_timer_oneshot_handle_irq();
    CHECK(!lapic_timer_oneshot_active(),
          "final restore mismatch clears armed state");
    CHECK(mock_lvt_value == 0x00030042U,
          "final restore mismatch remasks exact periodic LVT");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "final restore mismatch leaves LVT masked");
    CHECK((mock_lvt_value & (1U << 17U)) != 0U,
          "final restore mismatch retains periodic bit");
    CHECK(!serial_contains(
              "[LAPIC-TIMER] oneshot complete: periodic vector=0x42 restored"),
          "final restore mismatch emits no completion diagnostic");
    CHECK(serial_contains(
              "[LAPIC-TIMER] oneshot restore failed; LVT masked"),
          "final restore mismatch confirms remasked LVT");

    /* 2. Exact arm sequence and successful ten-sample calibration. */
    begin_case();
    lapic_timer_start_calibration(100U);
    CHECK(lapic_timer_status() == LAPIC_TIMER_CALIBRATING,
          "start enters calibrating state");
    CHECK(mock_arm_calls == 1U, "start arms exactly once");
    CHECK((mock_lvt_value & 0xFFU) == 0x42U,
          "masked LVT stores non-delivered value 0x42");
    CHECK((mock_lvt_value & (1U << 16U)) != 0U,
          "LVT timer mask bit remains set");
    CHECK(!mock_lvt_mask_cleared, "test path never clears LVT mask");
    CHECK(mock_divide_value == 0x3U, "divide configuration is divide by 16");
    CHECK(mock_initial_value == 0xFFFFFFFFU,
          "initial count is maximum");
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
        CHECK(lapic_timer_status() == LAPIC_TIMER_CALIBRATING,
              "calibration remains active before tenth PIT sample");
    }
    CHECK(mock_read_calls == 0U, "current count not read before tenth tick");
    mock_current_count = 0xFFFFFFFFU - 625000U;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_OK,
          "tenth sample publishes success");
    CHECK(lapic_timer_ticks_per_second() == 6250000ULL,
          "successful calibration computes LAPIC ticks per second");
    CHECK(mock_read_calls == 1U, "current count read once at completion");
    CHECK(serial_contains(
              "[LAPIC-TIMER] calibrated: hz=6250000 samples=10 divide=16; "
              "PIT retained"),
          "success diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_OK);

    /* 3. No-LAPIC arm failure. */
    begin_case();
    mock_arm_ok = false;
    lapic_timer_start_calibration(100U);
    CHECK(lapic_timer_status() == LAPIC_TIMER_NO_LAPIC,
          "arm failure publishes no-LAPIC");
    CHECK(lapic_timer_ticks_per_second() == 0U,
          "no-LAPIC result stays zero");
    CHECK(mock_read_calls == 0U, "no-LAPIC does not read current count");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: no LAPIC; PIT retained"),
          "no-LAPIC diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_NO_LAPIC);

    /* 4. Zero PIT frequency. */
    begin_case();
    lapic_timer_start_calibration(0U);
    CHECK(lapic_timer_status() == LAPIC_TIMER_BAD_SAMPLE,
          "zero PIT frequency is a bad sample");
    CHECK(mock_arm_calls == 0U, "bad frequency does not arm");
    CHECK(mock_read_calls == 0U, "bad frequency does not read");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: bad pit_hz; "
                          "PIT retained"),
          "bad frequency diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_BAD_SAMPLE);

    /* 5. Non-decrementing current count. */
    begin_case();
    lapic_timer_start_calibration(100U);
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
    }
    mock_current_count = 0xFFFFFFFFU;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_BAD_SAMPLE,
          "non-decrementing sample is rejected");
    CHECK(lapic_timer_ticks_per_second() == 0U,
          "bad sample result stays zero");
    CHECK(mock_read_calls == 1U, "bad sample reads once");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: non-decrementing "
                          "sample; PIT retained"),
          "bad sample diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_BAD_SAMPLE);

    /* 6. Current-count read failure. */
    begin_case();
    lapic_timer_start_calibration(100U);
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
    }
    mock_read_ok = false;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_BAD_SAMPLE,
          "failed current-count read is rejected");
    CHECK(lapic_timer_ticks_per_second() == 0U,
          "read failure result stays zero");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: current count read "
                          "failed; PIT retained"),
          "read failure diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_BAD_SAMPLE);

    /* 7. Arithmetic overflow guard. */
    begin_case();
    lapic_timer_start_calibration(2U);
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
    }
    lapic_timer_test_force_elapsed(UINT64_MAX);
    mock_current_count = 0U;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_OVERFLOW,
          "arithmetic overflow publishes overflow");
    CHECK(lapic_timer_ticks_per_second() == 0U,
          "overflow result stays zero");
    CHECK(mock_read_calls == 1U, "overflow reads once");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: arithmetic overflow; "
                          "PIT retained"),
          "overflow diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_OVERFLOW);

    /* 8. Zero-frequency result after a valid sample. */
    begin_case();
    lapic_timer_start_calibration(1U);
    for (uint32_t i = 0U; i < 9U; ++i) {
        lapic_timer_on_pit_tick();
    }
    mock_current_count = 0xFFFFFFFEU;
    lapic_timer_on_pit_tick();
    CHECK(lapic_timer_status() == LAPIC_TIMER_BAD_SAMPLE,
          "zero computed frequency is rejected");
    CHECK(serial_contains("[LAPIC-TIMER] unavailable: zero frequency; "
                          "PIT retained"),
          "zero frequency diagnostic is present");
    assert_terminal_idempotence(LAPIC_TIMER_BAD_SAMPLE);

    if (failures != 0) {
        printf("LAPIC timer host tests: %d failure(s)\n", failures);
        return 1;
    }
    puts("LAPIC timer host tests: ALL OK");
    return 0;
}

#else
int main(void) {
    return 0;
}
#endif
