#include "appliance_audit_export.h"

#include "appliance_audit_chain.h"
#include "audit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_AUDIT_EXPORT_HOST_TEST

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

static struct audit_record make_record(uint32_t operation)
{
    struct audit_record record;

    memset(&record, 0, sizeof(record));
    record.type = AUDIT_RECORD_MANAGEMENT;
    record.operation = operation;
    record.result = 0U;
    record.session_id = 1U;
    record.principal_id = 1U;
    record.command_id = operation;
    record.detail = 0x200U + operation;
    return record;
}

static bool append_record(
    struct appliance_audit_chain *chain,
    uint32_t operation)
{
    struct audit_record record = make_record(operation);

    return appliance_audit_chain_append(chain, &record, NULL) ==
           APPLIANCE_AUDIT_CHAIN_OK;
}

static bool build_chain(
    struct appliance_audit_chain *chain,
    uint32_t records)
{
    uint32_t index;

    if (appliance_audit_chain_init(chain, 5U) !=
        APPLIANCE_AUDIT_CHAIN_OK) {
        return false;
    }
    for (index = 0U; index < records; ++index) {
        if (!append_record(chain, index + 1U)) {
            return false;
        }
    }
    return true;
}

static void test_status_and_state_strings(void)
{
    CHECK(strcmp(appliance_audit_export_status_string(
                     APPLIANCE_AUDIT_EXPORT_OK),
                 "APPLIANCE_AUDIT_EXPORT_OK") == 0,
          "status string for OK");
    CHECK(strcmp(appliance_audit_export_status_string(
                     APPLIANCE_AUDIT_EXPORT_STALE_GENERATION),
                 "APPLIANCE_AUDIT_EXPORT_STALE_GENERATION") == 0,
          "status string for a stale generation");
    CHECK(appliance_audit_export_status_string(
              (enum appliance_audit_export_status)99) == NULL,
          "unknown status string is null");
    CHECK(strcmp(appliance_audit_export_state_string(
                     APPLIANCE_AUDIT_EXPORT_STATE_EXPIRED),
                 "EXPIRED") == 0,
          "state string for an expired cursor");
    CHECK(appliance_audit_export_state_string(
              (enum appliance_audit_export_state)99) == NULL,
          "unknown state string is null");
}

static void test_begin_failures(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;

    CHECK(build_chain(&chain, 3U), "begin fixture builds three records");
    CHECK(appliance_audit_export_cursor_init(NULL) ==
              APPLIANCE_AUDIT_EXPORT_NULL,
          "cursor init refuses null");
    CHECK(appliance_audit_export_cursor_init(&cursor) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              cursor.version == APPLIANCE_AUDIT_EXPORT_VERSION &&
              !cursor.active &&
              !appliance_audit_export_cursor_active(&cursor),
          "cursor init publishes the idle state");
    CHECK(appliance_audit_export_begin(NULL, &cursor, 0U) ==
              APPLIANCE_AUDIT_EXPORT_NULL,
          "begin refuses a null chain");
    CHECK(appliance_audit_export_begin(&chain, NULL, 0U) ==
              APPLIANCE_AUDIT_EXPORT_NULL,
          "begin refuses a null cursor");
    {
        struct appliance_audit_chain unpublished;

        memset(&unpublished, 0, sizeof(unpublished));
        CHECK(appliance_audit_export_begin(
                  &unpublished, &cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_BAD_STATE,
              "begin refuses an uninitialized chain");
    }
    {
        struct appliance_audit_export_cursor bad;

        memset(&bad, 0, sizeof(bad));
        CHECK(appliance_audit_export_begin(&chain, &bad, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_BAD_VERSION,
              "begin refuses an uninitialized cursor");
    }
    CHECK(appliance_audit_export_begin(&chain, &cursor, 9U) ==
              APPLIANCE_AUDIT_EXPORT_OUT_OF_RANGE &&
              !cursor.active &&
              appliance_audit_export_next_sequence(&cursor) == 0U,
          "begin refuses a start beyond the chain end");
    CHECK(appliance_audit_export_begin(&chain, &cursor, 0U) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              cursor.active && cursor.generation == 5U &&
              appliance_audit_export_next_sequence(&cursor) == 1U,
          "begin without a start opens the oldest retained record");
    CHECK(appliance_audit_export_begin(&chain, &cursor, 1U) ==
              APPLIANCE_AUDIT_EXPORT_ALREADY_ACTIVE,
          "begin refuses an already active cursor");
}

static void test_export_and_ack(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct appliance_audit_export_snapshot snapshot;
    struct audit_record record;
    struct audit_record batch[4];
    uint8_t digests[4 * SHA256_DIGEST_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;
    uint32_t count = 0U;
    char rendered[APPLIANCE_AUDIT_EXPORT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    CHECK(build_chain(&chain, 3U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "export fixture opens a cursor");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, &record, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              sequence == 1U && record.sequence == 1U &&
              record.detail == make_record(1U).detail,
          "next exports the oldest record first");
    CHECK(appliance_audit_chain_find(&chain, 1U, NULL, expected) &&
              memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
          "the exported digest matches the chain entry digest");
    CHECK(appliance_audit_export_ack(&cursor, 1U) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_last_ack_sequence(&cursor) == 1U &&
              cursor.ack_count == 1U,
          "a contiguous acknowledgement is accepted");
    CHECK(appliance_audit_export_ack(&cursor, 1U) ==
              APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              appliance_audit_export_ack(&cursor, 3U) ==
                  APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              appliance_audit_export_ack(&cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT,
          "duplicate and out-of-order acknowledgements are refused");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, NULL, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_NULL &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, NULL, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_NULL,
          "next refuses null output pointers");
    CHECK(appliance_audit_export_batch(
              &chain, &cursor, batch, digests, 4U, &count) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              count == 2U &&
              batch[0].sequence == 2U && batch[1].sequence == 3U,
          "batch exports the bounded remainder");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, &record, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_EMPTY &&
              appliance_audit_export_batch(
                  &chain, &cursor, batch, digests, 4U, &count) ==
                  APPLIANCE_AUDIT_EXPORT_EMPTY,
          "an exhausted cursor reports EMPTY");
    CHECK(appliance_audit_export_ack(&cursor, 3U) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_ack(&cursor, 2U) ==
                  APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              appliance_audit_export_snapshot(&cursor, &snapshot) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              snapshot.published &&
              snapshot.state ==
                  APPLIANCE_AUDIT_EXPORT_STATE_COMPLETE &&
              snapshot.next_sequence == 4U &&
              snapshot.end_sequence == 4U &&
              snapshot.last_exported_sequence == 3U &&
              snapshot.last_ack_sequence == 3U &&
              snapshot.exported_count == 3U &&
              snapshot.ack_count == 2U &&
              snapshot.gap_count == 0U,
          "a fully acknowledged cursor publishes COMPLETE");
    CHECK(appliance_audit_export_ack(&cursor, 4U) ==
              APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER,
          "an acknowledgement of a record that was never exported is refused");
    CHECK(appliance_audit_export_render(
              &snapshot, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_EXPORT_OK &&
              strstr(rendered,
                     "audit_export v=1 state=COMPLETE generation=5 "
                     "first=1 next=4 end=4 ack=3 exported=3 acked=2 "
                     "gap=0") != NULL,
          "cursor renders the canonical export view");
    CHECK(appliance_audit_export_render(
              &snapshot, rendered, 8U, &rendered_length) ==
              APPLIANCE_AUDIT_EXPORT_BUFFER_TOO_SMALL &&
              appliance_audit_export_render(
                  NULL, rendered, sizeof(rendered),
                  &rendered_length) == APPLIANCE_AUDIT_EXPORT_NULL,
          "cursor render fails closed");
    CHECK(appliance_audit_export_batch(
              &chain, &cursor, batch, digests, 0U, &count) ==
              APPLIANCE_AUDIT_EXPORT_BAD_ARGUMENT &&
              appliance_audit_export_batch(
                  &chain, NULL, batch, digests, 4U, &count) ==
                  APPLIANCE_AUDIT_EXPORT_NULL,
          "batch validates its arguments");
    CHECK(appliance_audit_export_ack(NULL, 1U) ==
              APPLIANCE_AUDIT_EXPORT_NULL &&
              appliance_audit_export_snapshot(NULL, &snapshot) ==
                  APPLIANCE_AUDIT_EXPORT_NULL,
          "ack and snapshot refuse null input");
}

static void test_idle_and_not_active(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct appliance_audit_export_snapshot snapshot;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;
    char rendered[APPLIANCE_AUDIT_EXPORT_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    CHECK(build_chain(&chain, 2U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "idle fixture initializes");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, &record, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE &&
              appliance_audit_export_ack(&cursor, 1U) ==
                  APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE,
          "an idle cursor cannot export or acknowledge");
    CHECK(appliance_audit_export_snapshot(&cursor, &snapshot) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              snapshot.state == APPLIANCE_AUDIT_EXPORT_STATE_IDLE &&
              appliance_audit_export_render(
                  &snapshot, rendered, sizeof(rendered),
                  &rendered_length) == APPLIANCE_AUDIT_EXPORT_OK &&
              strstr(rendered,
                     "audit_export v=1 state=IDLE") != NULL,
          "an idle cursor renders IDLE");
}

static void test_gap_and_resume(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct appliance_audit_export_snapshot snapshot;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;

    CHECK(build_chain(&chain, 5U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 1U) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, digest, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "gap fixture exports one record");
    CHECK(appliance_audit_chain_rotate(&chain, 3U, NULL) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_first_sequence(&chain) == 4U,
          "the chain rotates past the consumer cursor");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, &record, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_EXPIRED &&
              appliance_audit_export_gap_count(&cursor) == 1U &&
              appliance_audit_export_next_sequence(&cursor) == 4U &&
              appliance_audit_export_ack(&cursor, 3U) ==
                  APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              cursor.ack_count == 0U,
          "a rotated-past cursor reports the gap instead of skipping");
    CHECK(appliance_audit_export_next(
              &chain, &cursor, &record, digest, &sequence) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              sequence == 4U &&
              appliance_audit_export_snapshot(&cursor, &snapshot) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              snapshot.state == APPLIANCE_AUDIT_EXPORT_STATE_EXPIRED &&
              snapshot.gap_count == 1U,
          "the cursor resynchronizes to the oldest retained record");

    CHECK(appliance_audit_export_cursor_init(&cursor) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 1U) ==
                  APPLIANCE_AUDIT_EXPORT_EXPIRED &&
              cursor.active && cursor.gap_declared &&
              appliance_audit_export_next_sequence(&cursor) == 4U,
          "begin declares the gap for an expired absolute start");
    CHECK(appliance_audit_export_cursor_init(&cursor) ==
              APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 5U) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_ack(&cursor, 4U) ==
                  APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              appliance_audit_export_ack(&cursor, 5U) ==
                  APPLIANCE_AUDIT_EXPORT_OUT_OF_ORDER &&
              cursor.ack_count == 0U &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, digest, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              sequence == 5U &&
              appliance_audit_export_ack(&cursor, 5U) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "an explicit retained start resumes exactly");
}

static void test_tampered_chain_is_never_exported(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;

    CHECK(build_chain(&chain, 3U) &&
              appliance_audit_chain_host_test_tamper(&chain, 0U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_BAD_STATE &&
              !appliance_audit_export_cursor_active(&cursor) &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, digest, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_NOT_ACTIVE,
          "a cursor is never opened on a tampered chain");
    CHECK(appliance_audit_export_begin(
              &chain, &cursor, 1U) ==
              APPLIANCE_AUDIT_EXPORT_BAD_STATE,
          "even an explicit start is refused on a tampered chain");
}

static void test_stale_generation(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;

    CHECK(build_chain(&chain, 2U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "generation fixture opens a cursor");
    CHECK(appliance_audit_chain_init(&chain, 6U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, digest, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_STALE_GENERATION,
          "a cursor from another chain generation is refused");
}

static void test_no_allocation(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_export_cursor cursor;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint64_t sequence = 0U;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(build_chain(&chain, 2U) &&
              appliance_audit_export_cursor_init(&cursor) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_begin(&chain, &cursor, 0U) ==
                  APPLIANCE_AUDIT_EXPORT_OK &&
              appliance_audit_export_next(
                  &chain, &cursor, &record, digest, &sequence) ==
                  APPLIANCE_AUDIT_EXPORT_OK,
          "no-allocation export path completes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "audit export never allocates");
}

int main(void)
{
    test_status_and_state_strings();
    test_begin_failures();
    test_export_and_ack();
    test_idle_and_not_active();
    test_gap_and_resume();
    test_tampered_chain_is_never_exported();
    test_stale_generation();
    test_no_allocation();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance audit export host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance audit export host tests: ALL OK");
    return 0;
}

#endif
