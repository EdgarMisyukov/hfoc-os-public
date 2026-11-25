#include "integration_event_cursor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_EVENT_CURSOR_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static struct kernel_event make_event(
    uint64_t source_sequence,
    uint32_t type,
    uint64_t data)
{
    struct kernel_event event;

    memset(&event, 0, sizeof(event));
    event.sequence = source_sequence;
    event.type = type;
    event.flags = 1U;
    event.data[0] = data;
    event.data[1] = data + 1U;
    event.data[2] = data + 2U;
    return event;
}

static struct integration_event_cursor make_cursor(uint32_t capacity)
{
    struct integration_event_cursor cursor;

    memset(&cursor, 0xA5, sizeof(cursor));
    CHECK(integration_event_cursor_init(&cursor, capacity) ==
              INTEGRATION_EVENT_CURSOR_OK,
          "outbound event cursor fixture initializes");
    return cursor;
}

static void capture_cursor(const struct integration_event_cursor *cursor,
                           unsigned char *original)
{
    memcpy(original, cursor, sizeof(*cursor));
}

static void check_cursor_unchanged(
    const struct integration_event_cursor *cursor,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(cursor, original, sizeof(*cursor)) == 0, message);
}

static void check_no_allocation(void)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "outbound event cursor performs no allocation");
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_EVENT_CURSOR_VERSION == 1U,
          "outbound event cursor version is exactly 1");
    CHECK(INTEGRATION_EVENT_CURSOR_MAX_RECORDS == 64U,
          "outbound event cursor maximum is exactly 64 records");
    check_no_allocation();
}

static void test_cursor_init(void)
{
    struct integration_event_cursor cursor;
    uint32_t index;

    CHECK(integration_event_cursor_init(NULL, 1U) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor init rejects null cursor");
    CHECK(integration_event_cursor_init(&cursor, 0U) ==
              INTEGRATION_EVENT_CURSOR_BAD_CAPACITY,
          "outbound event cursor init rejects zero capacity");
    CHECK(integration_event_cursor_init(
              &cursor, INTEGRATION_EVENT_CURSOR_MAX_RECORDS + 1U) ==
              INTEGRATION_EVENT_CURSOR_BAD_CAPACITY,
          "outbound event cursor init rejects over-limit capacity");
    CHECK(integration_event_cursor_init(&cursor, 3U) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.version == INTEGRATION_EVENT_CURSOR_VERSION &&
              cursor.capacity == 3U &&
              cursor.next_export_sequence == 0U &&
              cursor.last_ack_sequence == 0U &&
              cursor.mutation_generation == 0U &&
              cursor.pending_count == 0U,
          "outbound event cursor init publishes exact empty state");
    for (index = 0U;
         index < INTEGRATION_EVENT_CURSOR_MAX_RECORDS; ++index) {
        CHECK(cursor.records[index].export_sequence == 0U &&
                  cursor.records[index].source_sequence == 0U &&
                  cursor.records[index].generation == 0U &&
                  !cursor.records[index].acknowledged,
              "outbound event cursor init zeroes every record");
    }
    check_no_allocation();
}

static void test_export_validation_failures(void)
{
    struct integration_event_cursor cursor = make_cursor(3U);
    struct integration_event_cursor bad_state = cursor;
    struct kernel_event event = make_event(1U, 1U, 0x11U);
    struct kernel_event invalid_event = make_event(2U, 0U, 0x22U);
    unsigned char original[sizeof(cursor)];
    uint32_t count_out = 0x5A5A5A5AU;
    uint64_t generation_out = UINT64_C(0x5A5A5A5A5A5A5A5A);

    capture_cursor(&cursor, original);
    CHECK(integration_event_cursor_export(NULL, &event) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor export rejects null cursor");
    CHECK(integration_event_cursor_export(&cursor, NULL) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor export rejects null event");
    check_cursor_unchanged(&cursor, original,
                           "null export failures leave cursor unchanged");

    bad_state.version = 0U;
    CHECK(integration_event_cursor_export(&bad_state, &event) ==
              INTEGRATION_EVENT_CURSOR_BAD_STATE,
          "outbound event cursor export rejects unknown version");
    bad_state = cursor;
    bad_state.capacity = 0U;
    CHECK(integration_event_cursor_export(&bad_state, &event) ==
              INTEGRATION_EVENT_CURSOR_BAD_CAPACITY,
          "outbound event cursor export rejects zero capacity");
    bad_state = cursor;
    bad_state.pending_count = bad_state.capacity + 1U;
    CHECK(integration_event_cursor_export(&bad_state, &event) ==
              INTEGRATION_EVENT_CURSOR_BAD_STATE,
          "outbound event cursor export rejects corrupted count");
    CHECK(integration_event_cursor_export(&cursor, &invalid_event) ==
              INTEGRATION_EVENT_CURSOR_INVALID_EVENT,
          "outbound event cursor export rejects zero event type");
    check_cursor_unchanged(&cursor, original,
                           "invalid export failures leave cursor unchanged");

    CHECK(integration_event_cursor_acknowledge(NULL, 0U) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor acknowledge rejects null cursor");
    CHECK(integration_event_cursor_snapshot(
              NULL, NULL, 0U, &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor snapshot rejects null cursor");
    CHECK(integration_event_cursor_snapshot(
              &cursor, NULL, 1U, &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor snapshot rejects output/capacity mismatch");
    CHECK(integration_event_cursor_snapshot(
              &cursor, NULL, 0U, NULL, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor snapshot rejects null count output");
    CHECK(integration_event_cursor_snapshot(
              &cursor, NULL, 0U, &count_out, NULL) ==
              INTEGRATION_EVENT_CURSOR_NULL,
          "outbound event cursor snapshot rejects null generation output");
    check_cursor_unchanged(&cursor, original,
                           "snapshot null failures leave cursor unchanged");
    check_no_allocation();
}

static void test_ordered_export(void)
{
    struct integration_event_cursor cursor = make_cursor(4U);
    struct kernel_event first = make_event(100U, 1U, 0x11U);
    struct kernel_event second = make_event(101U, 2U, 0x22U);

    CHECK(integration_event_cursor_export(&cursor, &first) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 1U &&
              cursor.next_export_sequence == 1U &&
              cursor.mutation_generation == 1U &&
              cursor.records[0].export_sequence == 0U &&
              cursor.records[0].source_sequence == 100U &&
              cursor.records[0].generation == 0U &&
              !cursor.records[0].acknowledged &&
              cursor.records[0].event.sequence == 100U &&
              cursor.records[0].event.type == 1U &&
              cursor.records[0].event.data[0] == 0x11U &&
              cursor.records[0].event.data[2] == 0x13U,
          "outbound event cursor export appends exact first ordered record");

    CHECK(integration_event_cursor_export(&cursor, &second) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 2U &&
              cursor.next_export_sequence == 2U &&
              cursor.mutation_generation == 2U &&
              cursor.records[1].export_sequence == 1U &&
              cursor.records[1].source_sequence == 101U &&
              cursor.records[1].generation == 1U &&
              cursor.records[1].event.type == 2U &&
              cursor.records[1].event.data[0] == 0x22U,
          "outbound event cursor export appends exact second ordered record");

    CHECK(integration_event_cursor_pending_count(&cursor) == 2U &&
              integration_event_cursor_capacity(&cursor) == 4U &&
              integration_event_cursor_next_export_sequence(&cursor) == 2U &&
              integration_event_cursor_last_ack_sequence(&cursor) == 0U,
          "outbound event cursor accessors publish exact state");
    check_no_allocation();
}

static void test_bounded_retention_and_slot_reuse(void)
{
    struct integration_event_cursor cursor = make_cursor(2U);
    struct kernel_event first = make_event(1U, 1U, 0x11U);
    struct kernel_event second = make_event(2U, 2U, 0x22U);
    struct kernel_event third = make_event(3U, 3U, 0x33U);
    struct kernel_event fourth = make_event(4U, 4U, 0x44U);
    unsigned char original[sizeof(cursor)];

    CHECK(integration_event_cursor_export(&cursor, &first) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &second) ==
                  INTEGRATION_EVENT_CURSOR_OK,
          "bounded retention fixture fills cursor capacity");
    capture_cursor(&cursor, original);
    CHECK(integration_event_cursor_export(&cursor, &third) ==
              INTEGRATION_EVENT_CURSOR_FULL,
          "full outbound event cursor rejects new export");
    check_cursor_unchanged(&cursor, original,
                           "full-cursor rejection leaves cursor unchanged");

    CHECK(integration_event_cursor_acknowledge(&cursor, 0U) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 1U &&
              cursor.last_ack_sequence == 0U &&
              cursor.records[0].export_sequence == 1U &&
              cursor.records[0].source_sequence == 2U,
          "outbound event cursor acknowledge reclaims oldest slot");
    CHECK(integration_event_cursor_export(&cursor, &fourth) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 2U &&
              cursor.records[1].export_sequence == 2U &&
              cursor.records[1].source_sequence == 4U,
          "acknowledged cursor slot is reusable");
    check_no_allocation();
}

static void test_acknowledgement_order(void)
{
    struct integration_event_cursor cursor = make_cursor(4U);
    struct kernel_event first = make_event(1U, 1U, 1U);
    struct kernel_event second = make_event(2U, 2U, 2U);
    struct kernel_event third = make_event(3U, 3U, 3U);
    unsigned char original[sizeof(cursor)];

    CHECK(integration_event_cursor_export(&cursor, &first) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &second) ==
                  INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &third) ==
                  INTEGRATION_EVENT_CURSOR_OK,
          "acknowledgement fixture exports three ordered events");

    capture_cursor(&cursor, original);
    CHECK(integration_event_cursor_acknowledge(&cursor, 99U) ==
              INTEGRATION_EVENT_CURSOR_NOT_FOUND,
          "absent acknowledgement is rejected");
    check_cursor_unchanged(&cursor, original,
                           "absent acknowledgement leaves cursor unchanged");
    CHECK(integration_event_cursor_acknowledge(&cursor, 1U) ==
              INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER,
          "non-oldest acknowledgement is rejected");
    check_cursor_unchanged(&cursor, original,
                           "out-of-order acknowledgement leaves cursor unchanged");

    CHECK(integration_event_cursor_acknowledge(&cursor, 0U) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 2U &&
              cursor.last_ack_sequence == 0U &&
              cursor.records[0].export_sequence == 1U,
          "oldest acknowledgement succeeds once");
    CHECK(integration_event_cursor_acknowledge(&cursor, 0U) ==
              INTEGRATION_EVENT_CURSOR_NOT_FOUND,
          "duplicate acknowledgement is rejected");
    CHECK(integration_event_cursor_acknowledge(&cursor, 1U) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 1U &&
              cursor.last_ack_sequence == 1U &&
              cursor.records[0].export_sequence == 2U,
          "next oldest acknowledgement succeeds in order");

    CHECK(integration_event_cursor_acknowledge(&cursor, 2U) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              cursor.pending_count == 0U,
          "final acknowledgement empties cursor");
    CHECK(integration_event_cursor_acknowledge(&cursor, 3U) ==
              INTEGRATION_EVENT_CURSOR_EMPTY,
          "acknowledgement on empty cursor is rejected");
    check_no_allocation();
}

static void test_snapshot_exact_and_read_only(void)
{
    struct integration_event_cursor cursor = make_cursor(4U);
    struct kernel_event first = make_event(10U, 1U, 0x10U);
    struct kernel_event second = make_event(11U, 2U, 0x20U);
    struct kernel_event third = make_event(12U, 3U, 0x30U);
    struct integration_event_cursor_record records[
        INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
    struct integration_event_cursor_record saved[
        INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
    unsigned char cursor_before[sizeof(cursor)];
    uint32_t count_out = 0U;
    uint64_t generation_out = 0U;
    struct kernel_event later = make_event(13U, 4U, 0x40U);

    CHECK(integration_event_cursor_export(&cursor, &first) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &second) ==
                  INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &third) ==
                  INTEGRATION_EVENT_CURSOR_OK,
          "snapshot fixture exports three ordered events");

    memset(records, 0xA5, sizeof(records));
    count_out = 0x5A5A5A5AU;
    generation_out = UINT64_C(0x5A5A5A5A5A5A5A5A);
    CHECK(integration_event_cursor_snapshot(
              &cursor, records, 2U, &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_OVERFLOW &&
              count_out == 0x5A5A5A5AU &&
              generation_out == UINT64_C(0x5A5A5A5A5A5A5A5A),
          "overflow cursor snapshot leaves output unchanged");

    memset(records, 0, sizeof(records));
    CHECK(integration_event_cursor_snapshot(
              &cursor, records, INTEGRATION_EVENT_CURSOR_MAX_RECORDS,
              &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              count_out == 3U &&
              generation_out == 3U &&
              records[0].export_sequence == 0U &&
              records[0].source_sequence == 10U &&
              records[0].generation == 0U &&
              records[0].event.type == 1U &&
              records[1].export_sequence == 1U &&
              records[1].source_sequence == 11U &&
              records[1].generation == 1U &&
              records[2].export_sequence == 2U &&
              records[2].source_sequence == 12U &&
              records[2].generation == 2U,
          "outbound event cursor snapshot returns exact ordered pending records");
    memcpy(saved, records, sizeof(records));
    capture_cursor(&cursor, cursor_before);

    CHECK(integration_event_cursor_snapshot(
              &cursor, records, INTEGRATION_EVENT_CURSOR_MAX_RECORDS,
              &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              memcmp(records, saved, sizeof(records)) == 0,
          "outbound event cursor snapshot is deterministic");
    check_cursor_unchanged(&cursor, cursor_before,
                           "outbound event cursor snapshot is read-only");

    CHECK(integration_event_cursor_export(&cursor, &later) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_acknowledge(&cursor, 0U) ==
                  INTEGRATION_EVENT_CURSOR_OK &&
              memcmp(records, saved, sizeof(records)) == 0,
          "later cursor mutation does not change published snapshot");
    check_no_allocation();
}

static void test_snapshot_empty(void)
{
    struct integration_event_cursor cursor = make_cursor(2U);
    struct integration_event_cursor_record records[
        INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
    uint32_t count_out = 0x5A5A5A5AU;
    uint64_t generation_out = UINT64_C(0x5A5A5A5A5A5A5A5A);

    memset(records, 0xA5, sizeof(records));
    CHECK(integration_event_cursor_snapshot(
              &cursor, records, INTEGRATION_EVENT_CURSOR_MAX_RECORDS,
              &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              count_out == 0U &&
              generation_out == 0U,
          "empty outbound event cursor snapshot is exact");
    check_no_allocation();
}

static void test_stale_generation(void)
{
    struct integration_event_cursor cursor = make_cursor(4U);
    struct kernel_event first = make_event(1U, 1U, 1U);
    struct kernel_event second = make_event(2U, 2U, 2U);
    struct integration_event_cursor_record records[
        INTEGRATION_EVENT_CURSOR_MAX_RECORDS];
    uint32_t count_out = 0x5A5A5A5AU;
    uint64_t generation_out = UINT64_C(0x5A5A5A5A5A5A5A5A);

    CHECK(integration_event_cursor_export(&cursor, &first) ==
              INTEGRATION_EVENT_CURSOR_OK &&
              integration_event_cursor_export(&cursor, &second) ==
                  INTEGRATION_EVENT_CURSOR_OK,
          "stale-generation fixture exports two events");
    memset(records, 0xA5, sizeof(records));
    integration_event_cursor_host_test_set_snapshot_mutate(true);
    CHECK(integration_event_cursor_snapshot(
              &cursor, records, INTEGRATION_EVENT_CURSOR_MAX_RECORDS,
              &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_STALE_GENERATION &&
              count_out == 0x5A5A5A5AU &&
              generation_out == UINT64_C(0x5A5A5A5A5A5A5A5A),
          "mid-publication cursor mutation is rejected without partial snapshot");
    integration_event_cursor_host_test_set_snapshot_mutate(false);

    CHECK(integration_event_cursor_snapshot(
              &cursor, records, INTEGRATION_EVENT_CURSOR_MAX_RECORDS,
              &count_out, &generation_out) ==
              INTEGRATION_EVENT_CURSOR_OK,
          "snapshot succeeds after mutation injection is disabled");
    check_no_allocation();
}

static const char *expected_status_string(
    enum integration_event_cursor_status status)
{
    switch (status) {
    case INTEGRATION_EVENT_CURSOR_OK:
        return "INTEGRATION_EVENT_CURSOR_OK";
    case INTEGRATION_EVENT_CURSOR_NULL:
        return "INTEGRATION_EVENT_CURSOR_NULL";
    case INTEGRATION_EVENT_CURSOR_BAD_CAPACITY:
        return "INTEGRATION_EVENT_CURSOR_BAD_CAPACITY";
    case INTEGRATION_EVENT_CURSOR_BAD_STATE:
        return "INTEGRATION_EVENT_CURSOR_BAD_STATE";
    case INTEGRATION_EVENT_CURSOR_INVALID_EVENT:
        return "INTEGRATION_EVENT_CURSOR_INVALID_EVENT";
    case INTEGRATION_EVENT_CURSOR_FULL:
        return "INTEGRATION_EVENT_CURSOR_FULL";
    case INTEGRATION_EVENT_CURSOR_EMPTY:
        return "INTEGRATION_EVENT_CURSOR_EMPTY";
    case INTEGRATION_EVENT_CURSOR_NOT_FOUND:
        return "INTEGRATION_EVENT_CURSOR_NOT_FOUND";
    case INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER:
        return "INTEGRATION_EVENT_CURSOR_OUT_OF_ORDER";
    case INTEGRATION_EVENT_CURSOR_OVERFLOW:
        return "INTEGRATION_EVENT_CURSOR_OVERFLOW";
    case INTEGRATION_EVENT_CURSOR_STALE_GENERATION:
        return "INTEGRATION_EVENT_CURSOR_STALE_GENERATION";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_event_cursor_status status;

    for (status = INTEGRATION_EVENT_CURSOR_OK;
         status <= INTEGRATION_EVENT_CURSOR_STALE_GENERATION; ++status) {
        const char *string = integration_event_cursor_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "outbound event cursor status string is exact");
    }
    CHECK(integration_event_cursor_status_string(
              (enum integration_event_cursor_status)11U) == NULL,
          "undefined outbound event cursor status is rejected");
    check_no_allocation();
}

int main(void)
{
    test_exact_constants();
    test_cursor_init();
    test_export_validation_failures();
    test_ordered_export();
    test_bounded_retention_and_slot_reuse();
    test_acknowledgement_order();
    test_snapshot_exact_and_read_only();
    test_snapshot_empty();
    test_stale_generation();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration event cursor host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration event cursor host tests: ALL OK\n");
    return 0;
}

#endif
