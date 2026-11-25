#include "hfoc_lwip_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint32_t test_now_ms;

static uint32_t test_source(void)
{
    return test_now_ms;
}

static void set_now(uint32_t now_ms)
{
    test_now_ms = now_ms;
}

static bool init_time(void)
{
    hfoc_lwip_time_shutdown();
    return hfoc_lwip_time_init(test_source);
}

static uint32_t callback_log[HFOC_LWIP_TIME_SLOT_COUNT];
static size_t callback_log_count;

struct record_arg {
    uint32_t id;
};

static void reset_callbacks(void)
{
    size_t index;

    callback_log_count = 0U;
    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        callback_log[index] = 0U;
    }
}

static void record_callback(void *opaque)
{
    const struct record_arg *arg = opaque;

    if (callback_log_count < HFOC_LWIP_TIME_SLOT_COUNT) {
        callback_log[callback_log_count] = arg->id;
    }
    ++callback_log_count;
}

static bool snapshot_is_empty(const struct hfoc_lwip_time_snapshot *snapshot)
{
    return snapshot->version == 0U &&
           snapshot->now_ms == 0U &&
           snapshot->active_count == 0U &&
           snapshot->invoked_count == 0U &&
           snapshot->scheduled_count == 0U &&
           snapshot->next_deadline_ms ==
               HFOC_LWIP_TIME_NEXT_DEADLINE_NONE;
}

static void test_unpublished_fail_closed(void)
{
    struct hfoc_lwip_time_snapshot snapshot;
    struct record_arg arg = { 1U };

    hfoc_lwip_time_shutdown();
    set_now(100U);
    CHECK(!hfoc_lwip_time_ready(),
          "unpublished time adapter is not ready");
    CHECK(hfoc_lwip_time_now() == 0U,
          "unpublished time adapter returns zero now");
    CHECK(hfoc_lwip_time_arm(1U, record_callback, &arg, 1U) ==
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "unpublished time adapter rejects arm");
    CHECK(!hfoc_lwip_time_disarm(1U, 1U),
          "unpublished time adapter rejects disarm");
    CHECK(hfoc_lwip_time_teardown_generation(1U) == 0U,
          "unpublished time adapter teardown is empty");
    CHECK(hfoc_lwip_time_poll() == 0U,
          "unpublished time adapter poll is empty");
    CHECK(hfoc_lwip_time_next_deadline_ms() ==
              HFOC_LWIP_TIME_NEXT_DEADLINE_NONE,
          "unpublished time adapter has no deadline");
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot_is_empty(&snapshot),
          "unpublished time adapter snapshot is empty");
}

static void test_init_validation_and_snapshot(void)
{
    struct hfoc_lwip_time_snapshot snapshot;

    CHECK(!hfoc_lwip_time_init(NULL),
          "time adapter rejects null source");
    CHECK(init_time(), "time adapter publishes fixed state");
    CHECK(!hfoc_lwip_time_init(test_source),
          "duplicate time adapter init is rejected");
    CHECK(hfoc_lwip_time_ready(),
          "published time adapter is ready");

    set_now(12345U);
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot.version == HFOC_LWIP_TIME_VERSION &&
              snapshot.now_ms == 12345U &&
              snapshot.active_count == 0U &&
              snapshot.invoked_count == 0U &&
              snapshot.scheduled_count == 0U &&
              snapshot.next_deadline_ms ==
                  HFOC_LWIP_TIME_NEXT_DEADLINE_NONE,
          "published time snapshot has exact initial state");
    CHECK(hfoc_lwip_time_now() == 12345U,
          "published time now returns source value");
}

static void test_arm_validation_and_exhaustion(void)
{
    uint32_t tokens[HFOC_LWIP_TIME_SLOT_COUNT];
    uint32_t replacement;
    uint32_t exhausted;
    struct hfoc_lwip_time_snapshot snapshot;
    struct record_arg arg = { 9U };
    size_t index;

    CHECK(init_time(), "arm validation fixture initializes time");
    set_now(1000U);
    CHECK(hfoc_lwip_time_arm(10U, NULL, &arg, 1U) ==
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "arm rejects null callback");
    CHECK(hfoc_lwip_time_arm(10U, record_callback, &arg, 0U) ==
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "arm rejects zero generation");
    CHECK(hfoc_lwip_time_arm(
              HFOC_LWIP_TIME_DELAY_MAX + 1U,
              record_callback, &arg, 1U) ==
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "arm rejects over-maximum delay");
    CHECK(hfoc_lwip_time_teardown_generation(1U) == 0U,
          "rejected arms publish no teardown work");

    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        tokens[index] = hfoc_lwip_time_arm(
            100U + (uint32_t)index, record_callback, &arg, 1U);
        CHECK(tokens[index] != HFOC_LWIP_TIME_TOKEN_INVALID,
              "time slot arms until exhaustion");
    }
    exhausted = hfoc_lwip_time_arm(
        100U, record_callback, &arg, 1U);
    CHECK(exhausted == HFOC_LWIP_TIME_TOKEN_INVALID,
          "time slot exhaustion returns invalid token");
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot.active_count == HFOC_LWIP_TIME_SLOT_COUNT,
          "time snapshot records full active count");

    CHECK(hfoc_lwip_time_disarm(tokens[0], 1U),
          "time token disarms its exact slot");
    replacement = hfoc_lwip_time_arm(
        100U, record_callback, &arg, 1U);
    CHECK(replacement != HFOC_LWIP_TIME_TOKEN_INVALID &&
              replacement != tokens[0],
          "freed time slot reuses with a new token");
    CHECK(!hfoc_lwip_time_disarm(tokens[0], 1U),
          "stale time token cannot disarm reused slot");
    CHECK(hfoc_lwip_time_disarm(replacement, 1U),
          "current time token disarms reused slot");
}

static void test_wraparound_ordering(void)
{
    const uint32_t wrap_base = UINT32_MAX - 50U;
    const uint32_t first_due = (uint32_t)(wrap_base + 100U);
    const uint32_t second_due = (uint32_t)(wrap_base + 200U);
    const uint32_t third_due = (uint32_t)(wrap_base + 300U);
    struct record_arg first_arg = { 1U };
    struct record_arg second_arg = { 2U };
    struct record_arg third_arg = { 3U };
    struct hfoc_lwip_time_snapshot snapshot;
    uint32_t first_token;
    uint32_t second_token;
    uint32_t third_token;

    CHECK(init_time(), "wraparound fixture initializes time");
    reset_callbacks();
    set_now(wrap_base);
    first_token = hfoc_lwip_time_arm(
        100U, record_callback, &first_arg, 1U);
    second_token = hfoc_lwip_time_arm(
        200U, record_callback, &second_arg, 1U);
    third_token = hfoc_lwip_time_arm(
        300U, record_callback, &third_arg, 1U);
    CHECK(first_token != HFOC_LWIP_TIME_TOKEN_INVALID &&
              second_token != HFOC_LWIP_TIME_TOKEN_INVALID &&
              third_token != HFOC_LWIP_TIME_TOKEN_INVALID,
          "wraparound fixture arms three timers");
    CHECK(hfoc_lwip_time_next_deadline_ms() == first_due,
          "wraparound ordering selects the earliest wrapped deadline");

    set_now(first_due - 1U);
    CHECK(hfoc_lwip_time_poll() == 0U &&
              callback_log_count == 0U,
          "no timer fires before its wrapped deadline");
    set_now(first_due);
    CHECK(hfoc_lwip_time_poll() == 1U &&
              callback_log_count == 1U &&
              callback_log[0] == 1U,
          "first wrapped timer fires exactly at deadline");

    set_now(second_due - 1U);
    CHECK(hfoc_lwip_time_poll() == 0U,
          "later wrapped timer remains pending");
    set_now(second_due);
    CHECK(hfoc_lwip_time_poll() == 1U &&
              callback_log_count == 2U &&
              callback_log[1] == 2U,
          "second wrapped timer fires in due order");

    set_now(third_due);
    CHECK(hfoc_lwip_time_poll() == 1U &&
              callback_log_count == 3U &&
              callback_log[2] == 3U,
          "third wrapped timer fires in due order");
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot.active_count == 0U &&
              snapshot.invoked_count == 3U,
          "wraparound poll leaves exact counters");
}

static void test_teardown_generation(void)
{
    struct record_arg first_arg = { 1U };
    struct record_arg second_arg = { 2U };
    struct record_arg third_arg = { 3U };
    struct hfoc_lwip_time_snapshot snapshot;
    uint32_t token;

    CHECK(init_time(), "generation teardown fixture initializes time");
    reset_callbacks();
    set_now(1000U);
    CHECK(hfoc_lwip_time_arm(
              100U, record_callback, &first_arg, 7U) !=
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "generation seven first timer arms");
    CHECK(hfoc_lwip_time_arm(
              200U, record_callback, &second_arg, 7U) !=
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "generation seven second timer arms");
    token = hfoc_lwip_time_arm(
        300U, record_callback, &third_arg, 8U);
    CHECK(token != HFOC_LWIP_TIME_TOKEN_INVALID,
          "generation eight timer arms");
    CHECK(hfoc_lwip_time_teardown_generation(7U) == 2U,
          "generation teardown cancels exact count");
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot.active_count == 1U,
          "generation teardown leaves unrelated timer");
    CHECK(!hfoc_lwip_time_disarm(token, 7U) &&
              hfoc_lwip_time_disarm(token, 8U) &&
              hfoc_lwip_time_arm(
                  300U, record_callback, &third_arg, 8U) !=
                  HFOC_LWIP_TIME_TOKEN_INVALID,
          "disarm validates generation identity");
    set_now(2000U);
    CHECK(hfoc_lwip_time_poll() == 1U &&
              callback_log_count == 1U &&
              callback_log[0] == 3U,
          "torn-down generation callback never runs");
}

static void test_disarm_stale_and_reuse(void)
{
    struct record_arg arg = { 4U };
    uint32_t token;
    uint32_t replacement;

    CHECK(init_time(), "disarm fixture initializes time");
    reset_callbacks();
    set_now(500U);
    token = hfoc_lwip_time_arm(
        100U, record_callback, &arg, 7U);
    CHECK(token != HFOC_LWIP_TIME_TOKEN_INVALID,
          "disarm fixture arms timer");
    CHECK(!hfoc_lwip_time_disarm(token, 6U),
          "disarm rejects wrong generation");
    CHECK(!hfoc_lwip_time_disarm(token ^ 1U, 7U),
          "disarm rejects wrong slot token");
    CHECK(hfoc_lwip_time_disarm(token, 7U),
          "disarm accepts exact token and generation");
    CHECK(!hfoc_lwip_time_disarm(token, 7U),
          "disarm rejects already disarmed timer");
    set_now(600U);
    CHECK(hfoc_lwip_time_poll() == 0U &&
              callback_log_count == 0U,
          "disarmed timer never runs");

    replacement = hfoc_lwip_time_arm(
        100U, record_callback, &arg, 7U);
    CHECK(replacement != HFOC_LWIP_TIME_TOKEN_INVALID &&
              replacement != token,
          "reused slot publishes new token");
    CHECK(!hfoc_lwip_time_disarm(token, 7U),
          "stale token cannot cancel reused timer");
    CHECK(hfoc_lwip_time_disarm(replacement, 7U),
          "new token cancels reused timer");
}

static void test_shutdown_reset(void)
{
    struct record_arg arg = { 5U };
    struct hfoc_lwip_time_snapshot snapshot;

    CHECK(init_time(), "shutdown fixture initializes time");
    set_now(1000U);
    CHECK(hfoc_lwip_time_arm(
              100U, record_callback, &arg, 7U) !=
              HFOC_LWIP_TIME_TOKEN_INVALID,
          "shutdown fixture arms timer");
    hfoc_lwip_time_shutdown();
    CHECK(!hfoc_lwip_time_ready(),
          "shutdown makes time adapter unpublished");
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot_is_empty(&snapshot),
          "shutdown clears time snapshot");

    CHECK(init_time(), "time adapter re-initializes after shutdown");
    set_now(0U);
    snapshot = hfoc_lwip_time_snapshot();
    CHECK(snapshot.version == HFOC_LWIP_TIME_VERSION &&
              snapshot.active_count == 0U &&
              snapshot.invoked_count == 0U &&
              snapshot.scheduled_count == 0U,
          "re-init restores exact empty state");
}

int main(void)
{
    test_unpublished_fail_closed();
    test_init_validation_and_snapshot();
    test_arm_validation_and_exhaustion();
    test_wraparound_ordering();
    test_teardown_generation();
    test_disarm_stale_and_reuse();
    test_shutdown_reset();

    if (failures == 0) {
        printf("lwIP monotonic time adapter host tests: ALL OK\n");
    }
    return failures != 0;
}
