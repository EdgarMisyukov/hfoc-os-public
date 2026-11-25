#include "audit.h"
#include "event_ring.h"
#include "network_event.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_EVENT_HOST_TEST

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

static void reset_all(void)
{
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static struct network_event make_event(
    uint32_t type,
    uint32_t result,
    uint32_t device_id,
    uint32_t generation,
    uint64_t detail)
{
    struct network_event event;

    memset(&event, 0, sizeof(event));
    event.version = NETWORK_EVENT_VERSION;
    event.type = type;
    event.result = result;
    event.device_id = device_id;
    event.generation = generation;
    event.detail = detail;
    return event;
}

static bool event_matches(
    const struct network_event *event,
    uint32_t type,
    uint32_t result,
    uint32_t device_id,
    uint32_t generation,
    uint64_t detail)
{
    return event != NULL &&
           event->version == NETWORK_EVENT_VERSION &&
           event->type == type &&
           event->result == result &&
           event->device_id == device_id &&
           event->generation == generation &&
           event->detail == detail;
}

static void test_status_strings(void)
{
    static const struct {
        enum network_event_status status;
        const char *string;
    } statuses[] = {
        {NETWORK_EVENT_OK, "NETWORK_EVENT_OK"},
        {NETWORK_EVENT_NULL, "NETWORK_EVENT_NULL"},
        {NETWORK_EVENT_BAD_VERSION, "NETWORK_EVENT_BAD_VERSION"},
        {NETWORK_EVENT_INVALID_TYPE, "NETWORK_EVENT_INVALID_TYPE"},
        {NETWORK_EVENT_INVALID_RESULT, "NETWORK_EVENT_INVALID_RESULT"},
        {NETWORK_EVENT_INVALID_DEVICE_ID,
         "NETWORK_EVENT_INVALID_DEVICE_ID"},
        {NETWORK_EVENT_INVALID_GENERATION,
         "NETWORK_EVENT_INVALID_GENERATION"},
        {NETWORK_EVENT_BAD_STATE, "NETWORK_EVENT_BAD_STATE"},
        {NETWORK_EVENT_FULL, "NETWORK_EVENT_FULL"},
        {NETWORK_EVENT_EMPTY, "NETWORK_EVENT_EMPTY"},
        {NETWORK_EVENT_INVALID_EVENT, "NETWORK_EVENT_INVALID_EVENT"},
        {NETWORK_EVENT_SNAPSHOT_OVERFLOW,
         "NETWORK_EVENT_SNAPSHOT_OVERFLOW"},
    };
    size_t index;

    reset_all();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = network_event_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "network event status string is exact");
    }
    CHECK(network_event_status_string(
              (enum network_event_status)12U) == NULL,
          "network event undefined status is rejected");
    CHECK(strcmp(network_event_type_string(NETWORK_EVENT_TYPE_LINK),
                 "NETWORK_EVENT_TYPE_LINK") == 0 &&
              strcmp(network_event_type_string(
                         NETWORK_EVENT_TYPE_ADDRESS),
                     "NETWORK_EVENT_TYPE_ADDRESS") == 0 &&
              strcmp(network_event_type_string(
                         NETWORK_EVENT_TYPE_ADMISSION),
                     "NETWORK_EVENT_TYPE_ADMISSION") == 0 &&
              strcmp(network_event_type_string(
                         NETWORK_EVENT_TYPE_TRANSPORT),
                     "NETWORK_EVENT_TYPE_TRANSPORT") == 0,
          "network event type strings are exact");
    CHECK(network_event_type_string(0U) == NULL &&
              network_event_type_string(NETWORK_EVENT_TYPE_COUNT) == NULL,
          "network event undefined type string is rejected");
    CHECK(strcmp(network_event_result_string(NETWORK_EVENT_RESULT_OK),
                 "NETWORK_EVENT_RESULT_OK") == 0 &&
              strcmp(network_event_result_string(
                         NETWORK_EVENT_RESULT_FAILURE),
                     "NETWORK_EVENT_RESULT_FAILURE") == 0,
          "network event result strings are exact");
    CHECK(network_event_result_string(2U) == NULL,
          "network event undefined result string is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network event status handling performs no allocation");
}

static void test_validation(void)
{
    struct network_event event = make_event(
        NETWORK_EVENT_TYPE_LINK, NETWORK_EVENT_RESULT_FAILURE,
        3U, 7U, UINT64_C(0x1111222233334444));
    struct network_event bad;

    reset_all();
    CHECK(network_event_validate(NULL) == NETWORK_EVENT_NULL,
          "network event validation rejects null event");
    bad = event;
    bad.version = NETWORK_EVENT_VERSION + 1U;
    CHECK(network_event_validate(&bad) == NETWORK_EVENT_BAD_VERSION,
          "network event validation rejects unknown version");
    bad = event;
    bad.type = 0U;
    CHECK(network_event_validate(&bad) == NETWORK_EVENT_INVALID_TYPE,
          "network event validation rejects zero type");
    bad = event;
    bad.type = NETWORK_EVENT_TYPE_COUNT;
    CHECK(network_event_validate(&bad) == NETWORK_EVENT_INVALID_TYPE,
          "network event validation rejects out-of-range type");
    bad = event;
    bad.result = 2U;
    CHECK(network_event_validate(&bad) == NETWORK_EVENT_INVALID_RESULT,
          "network event validation rejects unknown result");
    bad = event;
    bad.device_id = NETWORK_EVENT_DEVICE_ID_MAX + 1U;
    CHECK(network_event_validate(&bad) ==
              NETWORK_EVENT_INVALID_DEVICE_ID,
          "network event validation rejects over-limit device id");
    bad = event;
    bad.generation = NETWORK_EVENT_GENERATION_MAX + 1U;
    CHECK(network_event_validate(&bad) ==
              NETWORK_EVENT_INVALID_GENERATION,
          "network event validation rejects over-limit generation");
    CHECK(network_event_validate(&event) == NETWORK_EVENT_OK,
          "network event validation accepts exact bounded event");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network event validation performs no allocation");
}

static void test_append_round_trip(void)
{
    struct event_ring ring;
    struct network_event link = make_event(
        NETWORK_EVENT_TYPE_LINK, NETWORK_EVENT_RESULT_FAILURE,
        3U, 7U, UINT64_C(0x1111222233334444));
    struct network_event address = make_event(
        NETWORK_EVENT_TYPE_ADDRESS, NETWORK_EVENT_RESULT_FAILURE,
        0U, 5U, UINT64_C(0x0A00000100000100));
    struct network_event admission = make_event(
        NETWORK_EVENT_TYPE_ADMISSION, NETWORK_EVENT_RESULT_FAILURE,
        0U, 0U, UINT64_C(0x0000000A07010000));
    struct network_event transport = make_event(
        NETWORK_EVENT_TYPE_TRANSPORT, NETWORK_EVENT_RESULT_FAILURE,
        0U, 0U, UINT64_C(0x0000000000000BB8));
    struct network_event output;
    struct kernel_event raw;
    uint64_t sequence;

    reset_all();
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "network event round-trip fixture initializes ring");

    sequence = UINT64_MAX;
    CHECK(network_event_append(&ring, &link, &sequence) ==
              NETWORK_EVENT_OK && sequence == 0U,
          "link failure event appends with sequence zero");
    CHECK(network_event_append(&ring, &address, NULL) ==
              NETWORK_EVENT_OK,
          "address failure event appends");
    CHECK(network_event_append(&ring, &admission, NULL) ==
              NETWORK_EVENT_OK,
          "admission failure event appends");
    CHECK(network_event_append(&ring, &transport, NULL) ==
              NETWORK_EVENT_OK,
          "transport failure event appends");

    CHECK(event_ring_pop(&ring, &raw) == EVENT_RING_OK &&
              raw.sequence == 0U &&
              raw.type == AUDIT_RECORD_NETWORK &&
              raw.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              network_event_from_event(&raw, &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, NETWORK_EVENT_TYPE_LINK,
                            NETWORK_EVENT_RESULT_FAILURE, 3U, 7U,
                            UINT64_C(0x1111222233334444)),
          "link event round-trips exact failure evidence");
    CHECK(event_ring_pop(&ring, &raw) == EVENT_RING_OK &&
              raw.sequence == 1U &&
              network_event_from_event(&raw, &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, NETWORK_EVENT_TYPE_ADDRESS,
                            NETWORK_EVENT_RESULT_FAILURE, 0U, 5U,
                            UINT64_C(0x0A00000100000100)),
          "address event round-trips exact failure evidence");
    CHECK(event_ring_pop(&ring, &raw) == EVENT_RING_OK &&
              raw.sequence == 2U &&
              network_event_from_event(&raw, &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, NETWORK_EVENT_TYPE_ADMISSION,
                            NETWORK_EVENT_RESULT_FAILURE, 0U, 0U,
                            UINT64_C(0x0000000A07010000)),
          "admission event round-trips exact failure evidence");
    CHECK(event_ring_pop(&ring, &raw) == EVENT_RING_OK &&
              raw.sequence == 3U &&
              network_event_from_event(&raw, &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, NETWORK_EVENT_TYPE_TRANSPORT,
                            NETWORK_EVENT_RESULT_FAILURE, 0U, 0U,
                            UINT64_C(0x0000000000000BB8)),
          "transport event round-trips exact failure evidence");
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 4U &&
              event_ring_read_cursor(&ring) == 4U,
          "event round-trip drains all four events");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network event append round-trip performs no allocation");
}

static void test_audit_normalizer_retention(void)
{
    struct event_ring ring;
    struct audit_record records[4];
    struct network_event events[4];
    struct network_event output;
    struct audit_record audit_output;
    uint32_t count = UINT32_MAX;
    uint32_t index;

    reset_all();
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "audit normalizer fixture initializes ring");
    events[0] = make_event(NETWORK_EVENT_TYPE_LINK,
                           NETWORK_EVENT_RESULT_FAILURE, 1U, 2U,
                           UINT64_C(0xAA));
    events[1] = make_event(NETWORK_EVENT_TYPE_ADDRESS,
                           NETWORK_EVENT_RESULT_FAILURE, 0U, 3U,
                           UINT64_C(0xBB));
    events[2] = make_event(NETWORK_EVENT_TYPE_ADMISSION,
                           NETWORK_EVENT_RESULT_FAILURE, 0U, 0U,
                           UINT64_C(0xCC));
    events[3] = make_event(NETWORK_EVENT_TYPE_TRANSPORT,
                           NETWORK_EVENT_RESULT_FAILURE, 0U, 0U,
                           UINT64_C(0xDD));
    for (index = 0U; index < 4U; ++index) {
        CHECK(network_event_append(&ring, &events[index], NULL) ==
                  NETWORK_EVENT_OK,
              "audit normalizer fixture appends network event");
    }

    CHECK(audit_snapshot(&ring, records, 4U, &count) == AUDIT_OK &&
              count == 4U &&
              network_event_from_audit(&records[0], &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, events[0].type, events[0].result,
                            events[0].device_id, events[0].generation,
                            events[0].detail) &&
              network_event_from_audit(&records[3], &output) ==
                  NETWORK_EVENT_OK &&
              event_matches(&output, events[3].type, events[3].result,
                            events[3].device_id, events[3].generation,
                            events[3].detail),
          "audit snapshot normalizer retains exact failure evidence");

    for (index = 0U; index < 4U; ++index) {
        CHECK(audit_record_pop(&ring, &audit_output) == AUDIT_OK &&
                  network_event_from_audit(&audit_output, &output) ==
                      NETWORK_EVENT_OK &&
                  event_matches(&output, events[index].type,
                                events[index].result,
                                events[index].device_id,
                                events[index].generation,
                                events[index].detail),
              "audit exact-once normalizer retains exact failure evidence");
    }
    CHECK(event_ring_count(&ring) == 0U,
          "audit normalizer drains without leakage");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit normalizer retention performs no allocation");
}

static void test_network_snapshot_and_pop(void)
{
    struct event_ring ring;
    struct network_event events[3];
    struct network_event output[3];
    struct network_event small[2];
    struct network_event small_saved[2];
    struct network_event single;
    uint32_t count = UINT32_MAX;

    reset_all();
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "network snapshot fixture initializes ring");
    events[0] = make_event(NETWORK_EVENT_TYPE_LINK,
                           NETWORK_EVENT_RESULT_FAILURE, 1U, 2U, 1U);
    events[1] = make_event(NETWORK_EVENT_TYPE_ADDRESS,
                           NETWORK_EVENT_RESULT_FAILURE, 0U, 3U, 2U);
    events[2] = make_event(NETWORK_EVENT_TYPE_TRANSPORT,
                           NETWORK_EVENT_RESULT_FAILURE, 0U, 0U, 3U);
    CHECK(network_event_append(&ring, &events[0], NULL) ==
              NETWORK_EVENT_OK &&
              network_event_append(&ring, &events[1], NULL) ==
                  NETWORK_EVENT_OK &&
              network_event_append(&ring, &events[2], NULL) ==
                  NETWORK_EVENT_OK,
          "network snapshot fixture appends three events");

    memset(small, 0xAA, sizeof(small));
    memcpy(small_saved, small, sizeof(small));
    count = UINT32_MAX;
    CHECK(network_event_snapshot(&ring, small, 2U, &count) ==
              NETWORK_EVENT_SNAPSHOT_OVERFLOW &&
              count == 3U &&
              memcmp(small, small_saved, sizeof(small)) == 0,
          "network snapshot overflow reports count without mutation");

    count = UINT32_MAX;
    CHECK(network_event_snapshot(&ring, output, 3U, &count) ==
              NETWORK_EVENT_OK &&
              count == 3U &&
              event_matches(&output[0], events[0].type, events[0].result,
                            events[0].device_id, events[0].generation,
                            events[0].detail) &&
              event_matches(&output[2], events[2].type, events[2].result,
                            events[2].device_id, events[2].generation,
                            events[2].detail) &&
              event_ring_count(&ring) == 3U &&
              event_ring_read_cursor(&ring) == 0U,
          "network snapshot copies exact events without consuming ring");

    CHECK(network_event_pop(&ring, &single) == NETWORK_EVENT_OK &&
              event_matches(&single, events[0].type, events[0].result,
                            events[0].device_id, events[0].generation,
                            events[0].detail) &&
              network_event_pop(&ring, &single) == NETWORK_EVENT_OK &&
              event_matches(&single, events[1].type, events[1].result,
                            events[1].device_id, events[1].generation,
                            events[1].detail) &&
              network_event_pop(&ring, &single) == NETWORK_EVENT_OK &&
              event_matches(&single, events[2].type, events[2].result,
                            events[2].device_id, events[2].generation,
                            events[2].detail) &&
              network_event_pop(&ring, &single) == NETWORK_EVENT_EMPTY,
          "network event pop returns exact FIFO events once");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "network snapshot and pop perform no allocation");
}

static void test_malformed_and_mixed_ring_rejection(void)
{
    struct event_ring ring;
    struct network_event event = make_event(
        NETWORK_EVENT_TYPE_ADMISSION, NETWORK_EVENT_RESULT_FAILURE,
        0U, 0U, UINT64_C(0xEE));
    struct network_event output[2];
    struct network_event output_saved[2];
    struct audit_record record;
    struct kernel_event raw;
    uint32_t count = UINT32_MAX;

    reset_all();
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "mixed-ring fixture initializes event ring");
    CHECK(network_event_append(&ring, &event, NULL) == NETWORK_EVENT_OK,
          "mixed-ring fixture appends one network event");
    CHECK(audit_record_init(&record, AUDIT_RECORD_SYSCALL, 1U, 0U,
                            2U, 1U, 0U, 0U) == AUDIT_OK &&
              audit_record_append(&ring, &record, NULL) == AUDIT_OK,
          "mixed-ring fixture appends one non-network audit record");

    memset(output, 0xAA, sizeof(output));
    memcpy(output_saved, output, sizeof(output));
    count = UINT32_MAX;
    CHECK(network_event_snapshot(&ring, output, 2U, &count) ==
              NETWORK_EVENT_INVALID_EVENT &&
              count == UINT32_MAX &&
              memcmp(output, output_saved, sizeof(output)) == 0,
          "network snapshot rejects mixed evidence without mutation");
    CHECK(network_event_pop(&ring, &event) == NETWORK_EVENT_OK &&
              event_ring_count(&ring) == 1U,
          "mixed-ring fixture consumes first valid network event");
    CHECK(network_event_pop(&ring, &event) == NETWORK_EVENT_INVALID_EVENT &&
              event_ring_count(&ring) == 1U &&
              event_ring_read_cursor(&ring) == 1U,
          "network pop rejects non-network evidence without consumption");

    memset(&raw, 0, sizeof(raw));
    raw.type = AUDIT_RECORD_NETWORK;
    raw.flags = 0U;
    raw.data[0] = AUDIT_RECORD_NETWORK;
    CHECK(network_event_from_event(&raw, &event) ==
              NETWORK_EVENT_INVALID_EVENT,
          "network decode rejects non-structured event");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "mixed-ring rejection performs no allocation");
}

static void test_ring_rejection_paths(void)
{
    struct event_ring ring;
    struct network_event event = make_event(
        NETWORK_EVENT_TYPE_LINK, NETWORK_EVENT_RESULT_FAILURE,
        1U, 1U, 0U);
    struct network_event output;
    uint64_t sequence = UINT64_MAX;
    uint32_t count = UINT32_MAX;
    uint32_t index;

    reset_all();
    memset(&ring, 0, sizeof(ring));
    CHECK(network_event_append(NULL, &event, &sequence) ==
              NETWORK_EVENT_NULL,
          "network append rejects null ring");
    CHECK(network_event_append(&ring, NULL, &sequence) ==
              NETWORK_EVENT_NULL,
          "network append rejects null event");
    CHECK(network_event_append(&ring, &event, &sequence) ==
              NETWORK_EVENT_BAD_STATE && sequence == UINT64_MAX,
          "network append rejects unpublished ring");
    CHECK(network_event_pop(&ring, &output) == NETWORK_EVENT_BAD_STATE,
          "network pop rejects unpublished ring");
    CHECK(network_event_snapshot(&ring, NULL, 0U, &count) ==
              NETWORK_EVENT_BAD_STATE,
          "network snapshot count query rejects unpublished ring");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "full-ring fixture initializes ring");
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(network_event_append(&ring, &event, NULL) ==
                  NETWORK_EVENT_OK,
              "full-ring fixture fills ring");
    }
    sequence = UINT64_MAX;
    CHECK(network_event_append(&ring, &event, &sequence) ==
              NETWORK_EVENT_FULL &&
              sequence == UINT64_MAX &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY,
          "full ring rejects network event without state change");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "ring rejection paths perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_validation();
    test_append_round_trip();
    test_audit_normalizer_retention();
    test_network_snapshot_and_pop();
    test_malformed_and_mixed_ring_rejection();
    test_ring_rejection_paths();

    if (failures != 0) {
        fprintf(stderr, "Network event host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Network event host tests: ALL OK");
    return 0;
}

#endif
