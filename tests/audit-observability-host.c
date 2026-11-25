#include "audit.h"
#include "event_ring.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUDIT_OBSERVABILITY_HOST_TEST

static int failures;
static struct event_ring ring;
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

static void reset_all(void) {
    memset(&ring, 0, sizeof(ring));
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static enum audit_status append_fixture_record(
    struct event_ring *target,
    uint32_t operation,
    uint32_t result,
    uint32_t pid,
    uint32_t domain,
    uint32_t generation,
    uint64_t detail)
{
    struct audit_record record;
    const enum audit_status init_status = audit_record_init(
        &record, AUDIT_RECORD_SYSCALL, operation, result, pid, domain,
        generation, detail);

    if (init_status != AUDIT_OK) {
        return init_status;
    }
    return audit_record_append(target, &record, NULL);
}

static bool record_matches(const struct audit_record *record,
                           uint64_t sequence,
                           uint32_t operation,
                           uint32_t result,
                           uint32_t pid,
                           uint32_t domain,
                           uint32_t generation,
                           uint64_t detail)
{
    return record != NULL &&
           record->sequence == sequence &&
           record->type == AUDIT_RECORD_SYSCALL &&
           record->operation == operation &&
           record->result == result &&
           record->pid == pid &&
           record->domain == domain &&
           record->generation == generation &&
           record->detail == detail;
}

static void test_validation(void) {
    struct audit_record record;

    reset_all();
    CHECK(audit_record_pop(NULL, &record) == AUDIT_NULL,
          "exact-once pop rejects null ring");
    CHECK(audit_record_pop(&ring, NULL) == AUDIT_NULL,
          "exact-once pop rejects null output");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_BAD_STATE,
          "exact-once pop rejects unpublished ring");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact-once validation performs no allocation");
}

static void test_empty_ring(void) {
    struct audit_record record;

    reset_all();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "empty-ring fixture initializes ring");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_EMPTY,
          "empty ring reports no audit record");
    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 0U &&
              event_ring_read_cursor(&ring) == 0U,
          "empty ring pop leaves ring state unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "empty ring pop performs no allocation");
}

static void test_exact_once_fifo(void) {
    struct audit_record record;

    reset_all();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "exact-once fixture initializes ring");
    CHECK(append_fixture_record(&ring, 0U, 6U, 2U, 1U, 0U, 0U) ==
              AUDIT_OK,
          "exact-once fixture appends write record");
    CHECK(append_fixture_record(&ring, 1U, 0U, 2U, 1U, 0U, 0U) ==
              AUDIT_OK,
          "exact-once fixture appends yield record");
    CHECK(append_fixture_record(&ring, 2U, 0U, 2U, 1U, 0U, 0U) ==
              AUDIT_OK,
          "exact-once fixture appends exit record");

    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              record_matches(&record, 0U, 0U, 6U, 2U, 1U, 0U, 0U),
          "first pop returns exact write audit record");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              record_matches(&record, 1U, 1U, 0U, 2U, 1U, 0U, 0U),
          "second pop returns exact yield audit record");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              record_matches(&record, 2U, 2U, 0U, 2U, 1U, 0U, 0U),
          "third pop returns exact exit audit record");

    CHECK(event_ring_count(&ring) == 0U &&
              event_ring_write_cursor(&ring) == 3U &&
              event_ring_read_cursor(&ring) == 3U,
          "exact-once consumption advances both cursors by three");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_EMPTY,
          "fourth pop confirms no duplicate or hidden record");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "exact-once FIFO consumption performs no allocation");
}

static void test_malformed_event_not_consumed(void) {
    struct kernel_event raw;
    struct audit_record record;

    reset_all();
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "malformed-event fixture initializes ring");
    memset(&raw, 0, sizeof(raw));
    raw.type = AUDIT_RECORD_SYSCALL;
    raw.flags = 0U;
    raw.data[0] = 1U;
    CHECK(event_ring_push(&ring, &raw) == EVENT_RING_OK,
          "malformed-event fixture publishes raw event");
    CHECK(audit_record_pop(&ring, &record) == AUDIT_INVALID_EVENT,
          "malformed event is rejected by exact-once pop");
    CHECK(event_ring_count(&ring) == 1U &&
              event_ring_write_cursor(&ring) == 1U &&
              event_ring_read_cursor(&ring) == 0U,
          "malformed event rejection does not consume ring state");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "malformed-event rejection performs no allocation");
}

int main(void) {
    test_validation();
    test_empty_ring();
    test_exact_once_fifo();
    test_malformed_event_not_consumed();

    if (failures != 0) {
        fprintf(stderr, "Audit observability host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Audit observability host tests: ALL OK");
    return 0;
}

#endif
