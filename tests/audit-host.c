#include "audit.h"
#include "event_ring.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUDIT_HOST_TEST

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

static void audit_reset(void) {
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
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

static void test_validation_and_lifecycle(void) {
    struct audit_record record;
    struct audit_record candidate;
    struct audit_record output;
    struct kernel_event event;

    audit_reset();
    memset(&record, 0, sizeof(record));
    memset(&candidate, 0, sizeof(candidate));
    memset(&output, 0, sizeof(output));
    memset(&event, 0, sizeof(event));

    CHECK(audit_record_init(NULL, AUDIT_RECORD_PROCESS, 1U, 0U, 7U,
                            1U, 2U, 0U) == AUDIT_NULL,
          "audit init rejects null record");
    CHECK(audit_record_init(&candidate, 0U, 1U, 0U, 7U, 1U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects invalid record type zero");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_TRUST + 1U,
                            1U, 0U, 7U, 1U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range record type");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_PROCESS, 0x100U,
                            0U, 7U, 1U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range operation");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_PROCESS, 1U,
                            0x100U, 7U, 1U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range result");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_PROCESS, 1U,
                            0U, 0x100U, 1U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range pid");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 0x100U, 2U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range domain");
    CHECK(audit_record_init(&candidate, AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 0x1000000U, 0U) ==
              AUDIT_INVALID_RECORD,
          "audit init rejects out-of-range generation");
    CHECK(candidate.sequence == 0U && candidate.type == 0U &&
              candidate.detail == 0U,
          "invalid audit init leaves record unchanged");

    CHECK(audit_record_init(&record, AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 2U, 0x4242U) == AUDIT_OK &&
              record_matches(&record, 0U, AUDIT_RECORD_PROCESS, 1U,
                             0U, 7U, 1U, 2U, 0x4242U),
          "audit init publishes exact process record fields");

    CHECK(audit_record_append(NULL, &record, NULL) == AUDIT_NULL,
          "audit append rejects null ring");
    CHECK(audit_record_append(&ring, NULL, NULL) == AUDIT_NULL,
          "audit append rejects null record");
    CHECK(audit_record_append(&ring, &record, NULL) == AUDIT_BAD_STATE,
          "audit append rejects unpublished ring");

    CHECK(audit_record_to_event(NULL, &event) == AUDIT_NULL,
          "audit encode rejects null record");
    CHECK(audit_record_to_event(&record, NULL) == AUDIT_NULL,
          "audit encode rejects null event");
    CHECK(audit_record_from_event(NULL, &output) == AUDIT_NULL,
          "audit decode rejects null event");
    CHECK(audit_record_from_event(&event, NULL) == AUDIT_NULL,
          "audit decode rejects null output");
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT &&
              output.sequence == 0U && output.type == 0U,
          "audit decode rejects zeroed event without output mutation");

    event.type = AUDIT_RECORD_PROCESS;
    event.flags = 0U;
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT,
          "audit decode rejects non-structured event");

    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.type = AUDIT_RECORD_TRUST + 1U;
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT,
          "audit decode rejects invalid packed type");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit validation and lifecycle perform no allocation");
}

static void test_exact_category_fields_and_sequence(void) {
    struct audit_record records[5];
    struct audit_record output;
    struct kernel_event event;
    uint64_t sequences[5] = { UINT64_MAX, UINT64_MAX, UINT64_MAX,
                              UINT64_MAX, UINT64_MAX };
    uint32_t index;

    audit_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "exact audit fixture initializes event ring");
    CHECK(audit_record_init(&records[0], AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 2U, 0x1111U) == AUDIT_OK &&
              audit_record_append(&ring, &records[0], &sequences[0]) ==
                  AUDIT_OK,
          "process audit record appends");
    CHECK(audit_record_init(&records[1], AUDIT_RECORD_SYSCALL, 1U,
                            7U, 7U, 1U, 2U, 0x2222U) == AUDIT_OK &&
              audit_record_append(&ring, &records[1], &sequences[1]) ==
                  AUDIT_OK,
          "syscall audit record appends");
    CHECK(audit_record_init(&records[2], AUDIT_RECORD_CAPABILITY, 1U,
                            0U, 8U, 1U, 3U, 0x3333U) == AUDIT_OK &&
              audit_record_append(&ring, &records[2], &sequences[2]) ==
                  AUDIT_OK,
          "capability audit record appends");
    CHECK(audit_record_init(&records[3], AUDIT_RECORD_FAULT, 1U,
                            14U, 9U, 1U, 4U, UINT64_C(0x12345678)) ==
              AUDIT_OK &&
              audit_record_append(&ring, &records[3], &sequences[3]) ==
                  AUDIT_OK,
          "fault audit record appends");
    CHECK(audit_record_init(&records[4], AUDIT_RECORD_REBOOT_REASON, 1U,
                            0U, 0U, 0U, 0U, 0x5555U) == AUDIT_OK &&
              audit_record_append(&ring, &records[4], &sequences[4]) ==
                  AUDIT_OK,
          "reboot-reason audit record appends");
    CHECK(sequences[0] == 0U && sequences[1] == 1U &&
              sequences[2] == 2U && sequences[3] == 3U &&
              sequences[4] == 4U &&
              event_ring_count(&ring) == 5U &&
              event_ring_write_cursor(&ring) == 5U &&
              event_ring_read_cursor(&ring) == 0U,
          "audit appends receive ring-owned ordered sequences");

    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.type == AUDIT_RECORD_PROCESS &&
              event.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              event.data[1] == UINT64_C(0x1111) &&
              audit_record_from_event(&event, &output) == AUDIT_OK &&
              record_matches(&output, 0U, AUDIT_RECORD_PROCESS, 1U,
                             0U, 7U, 1U, 2U, UINT64_C(0x1111)),
          "process audit record round-trips exact fields and sequence");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.type == AUDIT_RECORD_SYSCALL &&
              event.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              event.data[1] == UINT64_C(0x2222) &&
              audit_record_from_event(&event, &output) == AUDIT_OK &&
              record_matches(&output, 1U, AUDIT_RECORD_SYSCALL, 1U,
                             7U, 7U, 1U, 2U, UINT64_C(0x2222)),
          "syscall audit record round-trips exact fields and sequence");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.type == AUDIT_RECORD_CAPABILITY &&
              event.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              event.data[1] == UINT64_C(0x3333) &&
              audit_record_from_event(&event, &output) == AUDIT_OK &&
              record_matches(&output, 2U, AUDIT_RECORD_CAPABILITY, 1U,
                             0U, 8U, 1U, 3U, UINT64_C(0x3333)),
          "capability audit record round-trips exact fields and sequence");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.type == AUDIT_RECORD_FAULT &&
              event.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              event.data[1] == UINT64_C(0x12345678) &&
              audit_record_from_event(&event, &output) == AUDIT_OK &&
              record_matches(&output, 3U, AUDIT_RECORD_FAULT, 1U,
                             14U, 9U, 1U, 4U, UINT64_C(0x12345678)),
          "fault audit record round-trips exact fields and sequence");
    CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.type == AUDIT_RECORD_REBOOT_REASON &&
              event.flags == AUDIT_EVENT_FLAG_STRUCTURED &&
              event.data[1] == UINT64_C(0x5555) &&
              audit_record_from_event(&event, &output) == AUDIT_OK &&
              record_matches(&output, 4U, AUDIT_RECORD_REBOOT_REASON, 1U,
                             0U, 0U, 0U, 0U, UINT64_C(0x5555)),
          "reboot-reason audit record round-trips exact fields and sequence");
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 5U &&
              event_ring_read_cursor(&ring) == 5U,
          "exact audit records drain in FIFO order");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact audit record round-trips perform no allocation");
    (void)index;
}

static void test_full_ring_rejection_and_no_allocation(void) {
    struct audit_record record;
    struct audit_record output;
    struct kernel_event event;
    uint64_t sequence = UINT64_MAX;
    uint32_t index;

    audit_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "full audit fixture initializes event ring");
    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(audit_record_init(&record, AUDIT_RECORD_PROCESS,
                                index % 256U, index % 256U,
                                index % 256U, 1U, index % 0x1000000U,
                                index) == AUDIT_OK &&
                  audit_record_append(&ring, &record, NULL) == AUDIT_OK,
              "full audit fixture fills ring with bounded records");
    }
    CHECK(event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY &&
              event_ring_read_cursor(&ring) == 0U,
          "full audit fixture reaches exact capacity");

    CHECK(audit_record_init(&record, AUDIT_RECORD_PROCESS, 1U,
                            0U, 7U, 1U, 2U, 0U) == AUDIT_OK &&
              audit_record_append(&ring, &record, &sequence) == AUDIT_FULL &&
              sequence == UINT64_MAX &&
              event_ring_count(&ring) == KERNEL_EVENT_RING_CAPACITY &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY &&
              event_ring_read_cursor(&ring) == 0U,
          "full ring rejects audit append without state change");

    for (index = 0U; index < KERNEL_EVENT_RING_CAPACITY; ++index) {
        CHECK(event_ring_pop(&ring, &event) == EVENT_RING_OK &&
                  event.sequence == index &&
                  audit_record_from_event(&event, &output) == AUDIT_OK &&
                  output.type == AUDIT_RECORD_PROCESS &&
                  output.operation == index % 256U &&
                  output.result == index % 256U &&
                  output.pid == index % 256U &&
                  output.generation == index % 0x1000000U &&
                  output.detail == index,
              "full ring stores and drains ordered audit records");
    }
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY &&
              event_ring_read_cursor(&ring) ==
                  KERNEL_EVENT_RING_CAPACITY,
          "full ring drains all audit records with exact cursors");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full and drained audit records perform no allocation");
}

static void test_decode_rejects_malformed_events(void) {
    struct audit_record record;
    struct audit_record output;
    struct kernel_event event;
    uint64_t sequence;

    audit_reset();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "malformed-event fixture initializes event ring");
    CHECK(audit_record_init(&record, AUDIT_RECORD_FAULT, 1U,
                            14U, 9U, 1U, 4U, 0U) == AUDIT_OK &&
              audit_record_append(&ring, &record, &sequence) == AUDIT_OK &&
              event_ring_pop(&ring, &event) == EVENT_RING_OK &&
              event.sequence == 0U,
          "malformed-event fixture appends and pops a valid audit record");

    event.flags = 0U;
    memset(&output, 0, sizeof(output));
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT &&
              output.type == 0U && output.detail == 0U,
          "decode rejects missing structured flag");

    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.type = 0U;
    memset(&output, 0, sizeof(output));
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT &&
              output.type == 0U,
          "decode rejects zero packed type");

    event.type = AUDIT_RECORD_SYSCALL;
    event.data[0] = AUDIT_RECORD_SYSCALL;
    memset(&output, 0, sizeof(output));
    CHECK(audit_record_from_event(&event, &output) == AUDIT_OK &&
              output.type == AUDIT_RECORD_SYSCALL &&
              output.generation == 0U,
          "decode accepts valid packed type with zeroed fields");

    event.flags = AUDIT_EVENT_FLAG_STRUCTURED;
    event.type = AUDIT_RECORD_MANAGEMENT + 1U;
    memset(&output, 0, sizeof(output));
    CHECK(audit_record_from_event(&event, &output) == AUDIT_INVALID_EVENT &&
              output.type == 0U,
          "decode rejects out-of-range packed type");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-event decoding performs no allocation");
}

int main(void) {
    test_validation_and_lifecycle();
    test_exact_category_fields_and_sequence();
    test_full_ring_rejection_and_no_allocation();
    test_decode_rejects_malformed_events();

    if (failures != 0) {
        fprintf(stderr, "Audit record host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("Audit record host tests: ALL OK");
    return 0;
}

#endif
