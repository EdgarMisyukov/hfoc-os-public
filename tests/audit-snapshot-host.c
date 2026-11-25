#include "audit.h"
#include "event_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUDIT_SNAPSHOT_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct event_ring ring;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

void *kmalloc(size_t size) {
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr) {
    (void)ptr;
    ++free_attempts;
    return false;
}

static void snapshot_reset(void) {
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static struct kernel_event make_event(uint32_t type,
                                      uint64_t data0,
                                      uint64_t data1)
{
    struct kernel_event event;

    memset(&event, 0, sizeof(event));
    event.type = type;
    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.data[0] = data0;
    event.data[1] = data1;
    return event;
}

static bool event_matches(const struct kernel_event *event,
                          uint64_t sequence,
                          uint32_t type,
                          uint32_t flags,
                          uint64_t data0,
                          uint64_t data1)
{
    return event != NULL &&
           event->sequence == sequence &&
           event->type == type &&
           event->flags == flags &&
           event->data[0] == data0 &&
           event->data[1] == data1;
}

static bool record_matches(const struct audit_record *record,
                           uint64_t sequence,
                           uint32_t type,
                           uint32_t operation,
                           uint32_t result,
                           uint32_t pid,
                           uint32_t domain,
                           uint32_t generation,
                           uint64_t detail)
{
    return record != NULL &&
           record->sequence == sequence &&
           record->type == type &&
           record->operation == operation &&
           record->result == result &&
           record->pid == pid &&
           record->domain == domain &&
           record->generation == generation &&
           record->detail == detail;
}

static void test_raw_snapshot_validation(void) {
    struct kernel_event event = make_event(1U, 2U, 3U);
    struct kernel_event output;
    uint32_t count = UINT32_MAX;

    snapshot_reset();
    CHECK(event_ring_snapshot(NULL, &event, 1U, &count) ==
              EVENT_RING_NULL,
          "raw snapshot rejects null ring");
    CHECK(event_ring_snapshot(&ring, &event, 1U, NULL) ==
              EVENT_RING_NULL,
          "raw snapshot rejects null count output");
    CHECK(event_ring_snapshot(&ring, NULL, 1U, &count) ==
              EVENT_RING_NULL,
          "raw snapshot rejects non-null capacity without output");
    CHECK(event_ring_snapshot(&ring, &event, 0U, &count) ==
              EVENT_RING_NULL,
          "raw snapshot rejects non-null output with zero capacity");
    CHECK(event_ring_snapshot(&ring, NULL, 0U, &count) ==
              EVENT_RING_BAD_STATE,
          "raw snapshot rejects unpublished count query");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "raw snapshot validation initializes ring");
    count = UINT32_MAX;
    CHECK(event_ring_snapshot(&ring, NULL, 0U, &count) ==
              EVENT_RING_OK &&
              count == 0U,
          "raw snapshot count query reports empty ring");

    output = event;
    count = UINT32_MAX;
    CHECK(event_ring_snapshot(&ring, &output, 1U, &count) ==
              EVENT_RING_OK &&
              count == 0U,
          "raw snapshot returns zero events for empty ring");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "raw snapshot validation performs no allocation");
    (void)output;
}

static void test_raw_snapshot_exact_wrapped_and_overflow(void) {
    struct kernel_event events[KERNEL_EVENT_RING_CAPACITY];
    struct kernel_event overflow[4];
    struct kernel_event overflow_saved[4];
    struct kernel_event event;
    uint32_t count = UINT32_MAX;
    uint32_t index;

    snapshot_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "raw snapshot fixture initializes ring");
    CHECK(event_ring_push(&ring, &(struct kernel_event) {
              .type = 1U, .flags = 1U,
              .data[0] = 10U, .data[1] = 11U }) == EVENT_RING_OK &&
              event_ring_push(&ring, &(struct kernel_event) {
                  .type = 2U, .flags = 1U,
                  .data[0] = 20U, .data[1] = 21U }) == EVENT_RING_OK &&
              event_ring_push(&ring, &(struct kernel_event) {
                  .type = 3U, .flags = 1U,
                  .data[0] = 30U, .data[1] = 31U }) == EVENT_RING_OK,
          "raw snapshot fixture pushes initial events");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.sequence == 0U,
          "raw snapshot fixture consumes first event");
    CHECK(event_ring_push(&ring, &(struct kernel_event) {
              .type = 4U, .flags = 1U,
              .data[0] = 40U, .data[1] = 41U }) == EVENT_RING_OK &&
              event_ring_push(&ring, &(struct kernel_event) {
                  .type = 5U, .flags = 1U,
                  .data[0] = 50U, .data[1] = 51U }) == EVENT_RING_OK &&
              event_ring_push(&ring, &(struct kernel_event) {
                  .type = 6U, .flags = 1U,
                  .data[0] = 60U, .data[1] = 61U }) == EVENT_RING_OK,
          "raw snapshot fixture adds wrapped remaining events");

    memset(events, 0, sizeof(events));
    count = UINT32_MAX;
    CHECK(event_ring_snapshot(&ring, events,
                              KERNEL_EVENT_RING_CAPACITY, &count) ==
              EVENT_RING_OK &&
              count == 5U &&
              event_ring_count(&ring) == 5U &&
              event_ring_write_cursor(&ring) == 6U &&
              event_ring_read_cursor(&ring) == 1U,
          "raw snapshot copies exact wrapped FIFO set without consuming ring");
    CHECK(event_matches(&events[0], 1U, 2U, 1U, 20U, 21U) &&
              event_matches(&events[1], 2U, 3U, 1U, 30U, 31U) &&
              event_matches(&events[2], 3U, 4U, 1U, 40U, 41U) &&
              event_matches(&events[3], 4U, 5U, 1U, 50U, 51U) &&
              event_matches(&events[4], 5U, 6U, 1U, 60U, 61U),
          "raw snapshot preserves exact wrapped sequence order");

    memset(overflow, 0xAA, sizeof(overflow));
    memcpy(overflow_saved, overflow, sizeof(overflow));
    count = UINT32_MAX;
    CHECK(event_ring_snapshot(&ring, overflow, 4U, &count) ==
              EVENT_RING_OVERFLOW &&
              count == 5U &&
              memcmp(overflow, overflow_saved, sizeof(overflow)) == 0,
          "raw snapshot overflow reports count without partial output");

    count = UINT32_MAX;
    CHECK(event_ring_snapshot(&ring, NULL, 0U, &count) == EVENT_RING_OK &&
              count == 5U,
          "raw snapshot count query reports available events");

    for (index = 0U; index < 5U; ++index) {
        CHECK(events[index].sequence == index + 1U,
              "raw snapshot copies ring-owned ordered sequences");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "raw exact/overflow snapshot performs no allocation");
}

static void test_audit_snapshot_validation(void) {
    struct audit_record record;
    struct audit_record output[2];
    uint32_t count = UINT32_MAX;

    snapshot_reset();
    CHECK(audit_snapshot(NULL, output, 2U, &count) == AUDIT_NULL,
          "audit snapshot rejects null ring");
    CHECK(audit_snapshot(&ring, output, 2U, NULL) == AUDIT_NULL,
          "audit snapshot rejects null count output");
    CHECK(audit_snapshot(&ring, NULL, 1U, &count) == AUDIT_NULL,
          "audit snapshot rejects non-null capacity without output");
    CHECK(audit_snapshot(&ring, output, 0U, &count) == AUDIT_NULL,
          "audit snapshot rejects non-null output with zero capacity");
    CHECK(audit_snapshot(&ring, NULL, 0U, &count) == AUDIT_BAD_STATE,
          "audit snapshot rejects unpublished count query");

    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "audit snapshot validation initializes ring");
    count = UINT32_MAX;
    CHECK(audit_snapshot(&ring, NULL, 0U, &count) == AUDIT_OK &&
              count == 0U,
          "audit snapshot count query reports empty ring");

    memset(&record, 0, sizeof(record));
    CHECK(audit_record_init(&record, AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 2U, 0x11U) == AUDIT_OK,
          "audit snapshot validation builds one valid record");
    CHECK(audit_record_append(&ring, &record, NULL) == AUDIT_OK &&
              audit_snapshot(&ring, NULL, 0U, &count) == AUDIT_OK &&
              count == 1U,
          "audit snapshot count query reports available audit records");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit snapshot validation performs no allocation");
}

static void test_audit_snapshot_exact_and_overflow(void) {
    struct audit_record records[5];
    struct audit_record overflow[2];
    struct audit_record overflow_saved[2];
    struct audit_record output[5];
    uint32_t count = UINT32_MAX;

    snapshot_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "audit snapshot fixture initializes ring");
    CHECK(audit_record_init(&records[0], AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 2U, 0x1111U) == AUDIT_OK &&
              audit_record_append(&ring, &records[0], NULL) == AUDIT_OK &&
              audit_record_init(&records[1], AUDIT_RECORD_SYSCALL, 2U,
                                0U, 7U, 1U, 2U, 0x2222U) == AUDIT_OK &&
              audit_record_append(&ring, &records[1], NULL) == AUDIT_OK &&
              audit_record_init(&records[2], AUDIT_RECORD_CAPABILITY, 3U,
                                0U, 8U, 1U, 3U, 0x3333U) == AUDIT_OK &&
              audit_record_append(&ring, &records[2], NULL) == AUDIT_OK,
          "audit snapshot fixture appends three structured records");

    memset(output, 0, sizeof(output));
    count = UINT32_MAX;
    CHECK(audit_snapshot(&ring, output, 5U, &count) == AUDIT_OK &&
              count == 3U &&
              event_ring_count(&ring) == 3U &&
              event_ring_write_cursor(&ring) == 3U &&
              event_ring_read_cursor(&ring) == 0U,
          "audit snapshot copies exact records without consuming ring");
    CHECK(record_matches(&output[0], 0U, AUDIT_RECORD_PROCESS, 1U,
                         0U, 7U, 1U, 2U, 0x1111U) &&
              record_matches(&output[1], 1U, AUDIT_RECORD_SYSCALL, 2U,
                             0U, 7U, 1U, 2U, 0x2222U) &&
              record_matches(&output[2], 2U, AUDIT_RECORD_CAPABILITY, 3U,
                             0U, 8U, 1U, 3U, 0x3333U),
          "audit snapshot decodes exact structured fields in FIFO order");

    memset(overflow, 0xAA, sizeof(overflow));
    memcpy(overflow_saved, overflow, sizeof(overflow));
    count = UINT32_MAX;
    CHECK(audit_snapshot(&ring, overflow, 2U, &count) ==
              AUDIT_SNAPSHOT_OVERFLOW &&
              count == 3U &&
              memcmp(overflow, overflow_saved, sizeof(overflow)) == 0,
          "audit snapshot overflow reports count without partial output");

    count = UINT32_MAX;
    CHECK(audit_snapshot(&ring, NULL, 0U, &count) == AUDIT_OK &&
              count == 3U,
          "audit snapshot count query reports available records");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit exact/overflow snapshot performs no allocation");
}

static void test_audit_snapshot_rejects_malformed_without_mutation(void) {
    struct audit_record output[4];
    struct audit_record output_saved[4];
    struct audit_record record;
    struct kernel_event raw_event;
    uint32_t count = UINT32_MAX;

    snapshot_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "malformed snapshot fixture initializes ring");
    CHECK(audit_record_init(&record, AUDIT_RECORD_FAULT, 4U,
                            0U, 9U, 1U, 4U, 0x4444U) == AUDIT_OK &&
              audit_record_append(&ring, &record, NULL) == AUDIT_OK,
          "malformed snapshot fixture appends one structured record");

    memset(&raw_event, 0, sizeof(raw_event));
    raw_event.type = 99U;
    raw_event.flags = 0U;
    raw_event.data[0] = 99U;
    raw_event.data[1] = 0x99U;
    CHECK(event_ring_push(&ring, &raw_event) == EVENT_RING_OK,
          "malformed snapshot fixture pushes non-structured event");

    memset(output, 0xAA, sizeof(output));
    memcpy(output_saved, output, sizeof(output));
    count = UINT32_MAX;
    CHECK(audit_snapshot(&ring, output, 4U, &count) ==
              AUDIT_INVALID_EVENT &&
              count == UINT32_MAX &&
              memcmp(output, output_saved, sizeof(output)) == 0,
          "malformed audit snapshot rejects without partial record mutation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed audit snapshot performs no allocation");
}

int main(void) {
    test_raw_snapshot_validation();
    test_raw_snapshot_exact_wrapped_and_overflow();
    test_audit_snapshot_validation();
    test_audit_snapshot_exact_and_overflow();
    test_audit_snapshot_rejects_malformed_without_mutation();

    if (failures != 0) {
        fprintf(stderr, "Audit/event snapshot host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Audit/event snapshot host tests: ALL OK");
    return 0;
}

#endif
