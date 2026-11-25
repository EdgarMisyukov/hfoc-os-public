/*
 * ITEM-428: the main loop's heartbeat, proven on the host.
 *
 * The 2026-09-16 hardware run's boot log ended ten seconds in and nothing in
 * it distinguished a machine that was up with nothing to report from a machine
 * that had stopped.  The heartbeat is the one record published whether or not
 * anything changed, so its cadence is a contract: it fires on the first turn
 * of the loop, it fires again only after the interval, a clock that goes
 * backwards or stands still never produces a second record, and the interval
 * index counts what was actually published.
 */
#include "appliance_loop_heartbeat.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                          \
    do {                                                                   \
        ++checks;                                                          \
        if (!(condition)) {                                                \
            ++failures;                                                    \
            printf("FAIL: %s\n", (message));                               \
        }                                                                  \
    } while (0)

static void test_first_turn(void)
{
    struct appliance_loop_heartbeat heartbeat;

    memset(&heartbeat, 0, sizeof(heartbeat));
    CHECK(appliance_loop_heartbeat_due(&heartbeat, 0U),
          "the first turn of the loop publishes the boot heartbeat");
    CHECK(heartbeat.started && heartbeat.interval_index == 1U &&
              heartbeat.last_ms == 0U,
          "the first heartbeat is counted and dated");
    CHECK(!appliance_loop_heartbeat_due(&heartbeat, 1U),
          "the turn after the first heartbeat publishes nothing");
    CHECK(!appliance_loop_heartbeat_due(
              &heartbeat,
              APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS - 1U),
          "the heartbeat waits for its whole interval");
    CHECK(heartbeat.interval_index == 1U,
          "a turn that publishes nothing does not advance the interval");
}

static void test_interval(void)
{
    struct appliance_loop_heartbeat heartbeat;

    memset(&heartbeat, 0, sizeof(heartbeat));
    CHECK(appliance_loop_heartbeat_due(&heartbeat, 1000U),
          "the boot heartbeat is published at the boot tick");
    CHECK(appliance_loop_heartbeat_due(
              &heartbeat,
              1000U + APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS),
          "the next heartbeat is published exactly one interval later");
    CHECK(heartbeat.interval_index == 2U &&
              heartbeat.last_ms ==
                  1000U + APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS,
          "the second heartbeat is counted and dated");
    CHECK(appliance_loop_heartbeat_due(
              &heartbeat,
              1000U + 3U * APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS + 17U),
          "a loop that was busy for longer than one interval still "
          "publishes exactly one heartbeat");
    CHECK(heartbeat.interval_index == 3U,
          "the interval index counts the heartbeats, not the turns");

    /* Every turn between two intervals is silent, however many there are. */
    {
        uint32_t now = heartbeat.last_ms;
        uint32_t published = 0U;
        uint32_t turn;

        for (turn = 0U; turn < 5000U; ++turn) {
            now += 5U;
            if (now - heartbeat.last_ms >=
                APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS) {
                if (appliance_loop_heartbeat_due(&heartbeat, now)) {
                    ++published;
                }
            } else if (appliance_loop_heartbeat_due(&heartbeat, now)) {
                ++published;
            }
        }
        CHECK(published == 5000U * 5U /
                  APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS,
              "a five-second poll cadence publishes one heartbeat per "
              "interval and no more");
    }
}

static void test_clock_that_does_not_advance(void)
{
    struct appliance_loop_heartbeat heartbeat;

    memset(&heartbeat, 0, sizeof(heartbeat));
    CHECK(appliance_loop_heartbeat_due(&heartbeat, 500U),
          "the boot heartbeat is published");
    CHECK(!appliance_loop_heartbeat_due(&heartbeat, 500U),
          "a clock that stands still publishes no heartbeat");
    CHECK(!appliance_loop_heartbeat_due(&heartbeat, 400U),
          "a clock that went backwards publishes no heartbeat");
    CHECK(heartbeat.interval_index == 1U,
          "a stopped clock leaves the interval index where it was");
    CHECK(appliance_loop_heartbeat_due(
              &heartbeat,
              500U + APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS),
          "a clock that starts moving again publishes the next heartbeat");
    CHECK(heartbeat.interval_index == 2U,
          "the recovered clock publishes exactly one heartbeat");
}

static void test_rollover(void)
{
    struct appliance_loop_heartbeat heartbeat;
    /*
     * 0xFFFF8000 is 32768 ms before the millisecond clock wraps, so the
     * wrapped tick that follows it is more than one interval later.
     */
    const uint32_t before_wrap = UINT32_C(0xFFFF8000);
    const uint32_t after_wrap = UINT32_C(0x00000001);

    memset(&heartbeat, 0, sizeof(heartbeat));
    CHECK(appliance_loop_heartbeat_due(&heartbeat, before_wrap),
          "a heartbeat is published before the millisecond clock wraps");
    CHECK(appliance_loop_heartbeat_due(&heartbeat, after_wrap),
          "the heartbeat survives the millisecond clock wrapping");
    CHECK(heartbeat.interval_index == 2U,
          "the wrapped clock publishes one heartbeat, not a storm");
    CHECK(!appliance_loop_heartbeat_due(
              &heartbeat, after_wrap + 1U),
          "the turn after a wrapped heartbeat is silent");
}

static void test_missing_owner(void)
{
    CHECK(!appliance_loop_heartbeat_due(NULL, 0U),
          "a heartbeat with no owner is refused");
}

int main(void)
{
    test_first_turn();
    test_interval();
    test_clock_that_does_not_advance();
    test_rollover();
    test_missing_owner();

    if (failures != 0) {
        fprintf(stderr, "Loop heartbeat host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Loop heartbeat host tests: ALL OK (%d checks)\n", checks);
    return 0;
}
