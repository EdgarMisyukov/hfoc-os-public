#include "appliance_audit_chain.h"

#include "audit.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST

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
    record.principal_id = 2U;
    record.command_id = 3U;
    record.detail = 0x100U + operation;
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

static void digest_from_hex(
    const char *hex,
    uint8_t digest[SHA256_DIGEST_SIZE])
{
    uint32_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        uint8_t value = 0U;
        uint32_t half;

        for (half = 0U; half < 2U; ++half) {
            char digit = hex[index * 2U + half];

            value <<= 4U;
            if (digit >= '0' && digit <= '9') {
                value |= (uint8_t)(digit - '0');
            } else {
                value |= (uint8_t)(digit - 'a' + 10);
            }
        }
        digest[index] = value;
    }
}

/*
 * Known-answer vectors.  The expected digests were produced by an independent
 * implementation of the documented encoding:
 *
 *   digest(0) = SHA-256("HFOC-AUDIT-CHAIN-V1" || generation_le64)
 *   digest(n) = SHA-256("HFOC-AUDIT-LINK-V1" || sequence_le64 ||
 *                       digest(n-1) || sequence_le64 || type_le32 ||
 *                       operation_le32 || result_le32 || pid_le32 ||
 *                       domain_le32 || generation_le32 || session_le32 ||
 *                       principal_le32 || command_le32 || detail_le64)
 *
 * so a change to the tags, the field order, a field width or the byte order
 * fails here instead of silently redefining the chain.
 */
static void test_known_answer_vectors(void)
{
    struct appliance_audit_chain chain;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t expected[SHA256_DIGEST_SIZE];

    digest_from_hex(
        "bc2051e2a50c14d38dd5470917146ea36a5e24f3f7d304242642bc2c603c8ec4",
        expected);
    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_head_digest(&chain, digest) &&
              memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
          "genesis digest matches the known-answer vector");

    memset(&record, 0, sizeof(record));
    record.type = AUDIT_RECORD_MANAGEMENT;
    record.operation = 1U;
    record.session_id = 1U;
    record.principal_id = 1U;
    record.command_id = 1U;
    record.detail = 0x100U;
    digest_from_hex(
        "037938e07adcf63fdbd0751c40ac11ab3c67f16da808dea32303154ee4788bb1",
        expected);
    CHECK(appliance_audit_chain_append(&chain, &record, digest) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
          "first link digest matches the known-answer vector");

    memset(&record, 0, sizeof(record));
    record.type = AUDIT_RECORD_MANAGEMENT;
    record.operation = 2U;
    record.session_id = 1U;
    record.principal_id = 1U;
    record.command_id = 2U;
    record.detail = 0x200U;
    digest_from_hex(
        "d6d43c17714fee08eb4765d12a86bb3a695badd9a716a628cfbaacfc92a5ab37",
        expected);
    CHECK(appliance_audit_chain_append(&chain, &record, digest) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              memcmp(digest, expected, SHA256_DIGEST_SIZE) == 0,
          "second link digest matches the known-answer vector");
}

static void test_status_and_state_strings(void)
{
    CHECK(strcmp(appliance_audit_chain_status_string(
                     APPLIANCE_AUDIT_CHAIN_OK),
                 "APPLIANCE_AUDIT_CHAIN_OK") == 0,
          "status string for OK");
    CHECK(strcmp(appliance_audit_chain_status_string(
                     APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL),
                 "APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL") == 0,
          "status string for buffer overflow");
    CHECK(appliance_audit_chain_status_string(
              (enum appliance_audit_chain_status)99) == NULL,
          "unknown status string is null");
    CHECK(strcmp(appliance_audit_chain_state_string(
                     APPLIANCE_AUDIT_CHAIN_STATE_TAMPERED),
                 "TAMPERED") == 0,
          "state string for tampered chain");
    CHECK(appliance_audit_chain_state_string(
              (enum appliance_audit_chain_state)99) == NULL,
          "unknown state string is null");
}

static void test_init_failures(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain_verdict verdict;

    CHECK(appliance_audit_chain_init(NULL, 1U) ==
              APPLIANCE_AUDIT_CHAIN_NULL,
          "null chain is refused");
    CHECK(appliance_audit_chain_init(&chain, 0U) ==
              APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT,
          "generation zero is refused");
    CHECK(appliance_audit_chain_verify(NULL, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_NULL,
          "verify refuses a null chain");
    CHECK(appliance_audit_chain_verify(&chain, NULL) ==
              APPLIANCE_AUDIT_CHAIN_NULL,
          "verify refuses a null verdict");
    memset(&chain, 0, sizeof(chain));
    CHECK(appliance_audit_chain_verify(&chain, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_BAD_STATE,
          "verify refuses an uninitialized chain");
}

static void test_empty_chain(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain_verdict verdict;
    struct appliance_audit_chain_snapshot snapshot;
    char rendered[APPLIANCE_AUDIT_CHAIN_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    CHECK(appliance_audit_chain_init(&chain, 7U) ==
              APPLIANCE_AUDIT_CHAIN_OK,
          "empty chain initializes");
    CHECK(appliance_audit_chain_count(&chain) == 0U &&
              appliance_audit_chain_first_sequence(&chain) == 1U &&
              appliance_audit_chain_next_sequence(&chain) == 1U &&
              appliance_audit_chain_generation(&chain) == 7U,
          "empty chain cursors are exact");
    CHECK(appliance_audit_chain_verify(&chain, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && !verdict.tamper_detected &&
              verdict.checked_count == 0U &&
              verdict.head_sequence == 0U &&
              !verdict.anchored,
          "empty chain verifies as a genesis chain");
    CHECK(appliance_audit_chain_snapshot(&chain, &snapshot) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              snapshot.published &&
              snapshot.state == APPLIANCE_AUDIT_CHAIN_STATE_EMPTY &&
              snapshot.count == 0U &&
              snapshot.capacity == APPLIANCE_AUDIT_CHAIN_CAPACITY,
          "empty chain snapshot is exact");
    CHECK(appliance_audit_chain_render_snapshot(
              &snapshot, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_CHAIN_OK &&
              strstr(rendered,
                     "audit_chain v=1 generation=7 state=EMPTY "
                     "records=0 first=1 next=1 capacity=32 "
                     "appended=0 rotated=0 anchored=NO") != NULL,
          "empty chain renders the canonical view");
    CHECK(appliance_audit_chain_head_digest(&chain, NULL) == false,
          "empty chain publishes no head digest");
    {
        uint8_t digest[SHA256_DIGEST_SIZE];
        struct audit_record record;

        CHECK(!appliance_audit_chain_find(&chain, 1U, &record, digest),
              "empty chain finds no record");
    }
}

static void test_append_and_verify(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain first;
    struct appliance_audit_chain second;
    struct appliance_audit_chain_verdict verdict;
    struct appliance_audit_chain_snapshot snapshot;
    struct audit_record record;
    uint8_t digest_one[SHA256_DIGEST_SIZE];
    uint8_t digest_two[SHA256_DIGEST_SIZE];
    uint8_t head[SHA256_DIGEST_SIZE];
    uint8_t other_head[SHA256_DIGEST_SIZE];
    char rendered[APPLIANCE_AUDIT_CHAIN_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    CHECK(appliance_audit_chain_init(&chain, 3U) ==
              APPLIANCE_AUDIT_CHAIN_OK,
          "chain initializes for appends");
    CHECK(appliance_audit_chain_append(&chain, NULL, NULL) ==
              APPLIANCE_AUDIT_CHAIN_NULL,
          "append refuses a null record");
    record = make_record(1U);
    record.type = 0U;
    CHECK(appliance_audit_chain_append(&chain, &record, NULL) ==
              APPLIANCE_AUDIT_CHAIN_INVALID_RECORD,
          "append refuses an invalid record type");
    CHECK(appliance_audit_chain_count(&chain) == 0U &&
              appliance_audit_chain_next_sequence(&chain) == 1U,
          "a refused append leaves the chain untouched");

    CHECK(append_record(&chain, 1U) &&
              append_record(&chain, 2U),
          "records append in order");
    CHECK(appliance_audit_chain_count(&chain) == 2U &&
              appliance_audit_chain_first_sequence(&chain) == 1U &&
              appliance_audit_chain_next_sequence(&chain) == 3U,
          "append advances the sequence exactly");
    CHECK(appliance_audit_chain_verify(&chain, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && !verdict.tamper_detected &&
              verdict.checked_count == 2U &&
              verdict.first_sequence == 1U &&
              verdict.head_sequence == 2U &&
              !verdict.anchored,
          "appended chain verifies from genesis");
    CHECK(appliance_audit_chain_head_digest(&chain, head),
          "head digest is published");
    CHECK(memcmp(head, verdict.head_digest, SHA256_DIGEST_SIZE) == 0,
          "head digest matches the verified head");
    CHECK(appliance_audit_chain_find(&chain, 2U, &record, digest_two) &&
              record.sequence == 2U &&
              record.detail == make_record(2U).detail &&
              memcmp(digest_two, head, SHA256_DIGEST_SIZE) == 0,
          "find returns the retained record and its digest");
    CHECK(!appliance_audit_chain_find(&chain, 3U, &record, digest_one),
          "find refuses a sequence that was never appended");

    CHECK(appliance_audit_chain_snapshot(&chain, &snapshot) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              snapshot.state == APPLIANCE_AUDIT_CHAIN_STATE_OK &&
              snapshot.appended_count == 2U &&
              snapshot.rotated_count == 0U &&
              snapshot.next_sequence == 3U &&
              snapshot.anchor_is_genesis,
          "populated snapshot is exact");
    CHECK(appliance_audit_chain_render_snapshot(
              &snapshot, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_CHAIN_OK &&
              strstr(rendered,
                     "audit_chain v=1 generation=3 state=OK "
                     "records=2 first=1 next=3 capacity=32 "
                     "appended=2 rotated=0 anchored=NO") != NULL,
          "populated chain renders the canonical view");
    CHECK(appliance_audit_chain_render_verdict(
              &verdict, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_CHAIN_OK &&
              strstr(rendered,
                     "audit_chain_verify v=1 result=OK checked=2 "
                     "first=1 head=2 tamper=0") != NULL,
          "verify verdict renders the canonical view");
    CHECK(appliance_audit_chain_render_snapshot(
              &snapshot, rendered, 16U, &rendered_length) ==
              APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL,
          "render refuses a short buffer");
    CHECK(appliance_audit_chain_render_verdict(
              NULL, rendered, sizeof(rendered), &rendered_length) ==
              APPLIANCE_AUDIT_CHAIN_NULL,
          "verdict render refuses null input");

    CHECK(appliance_audit_chain_init(&first, 9U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_init(&second, 9U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&first, 1U) &&
              append_record(&first, 2U) &&
              append_record(&second, 1U) &&
              append_record(&second, 2U) &&
              appliance_audit_chain_head_digest(&first, head) &&
              appliance_audit_chain_head_digest(&second, other_head) &&
              memcmp(head, other_head, SHA256_DIGEST_SIZE) == 0,
          "identical generation and records produce an identical chain");
    CHECK(appliance_audit_chain_init(&second, 10U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&second, 1U) &&
              append_record(&second, 2U) &&
              appliance_audit_chain_head_digest(&second, other_head) &&
              memcmp(head, other_head, SHA256_DIGEST_SIZE) != 0,
          "a different generation produces a different chain");
}

static void test_tamper_detection(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain_verdict verdict;
    struct appliance_audit_chain_snapshot snapshot;
    char rendered[APPLIANCE_AUDIT_CHAIN_RENDER_MAX + 1U];
    size_t rendered_length = 0U;

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              append_record(&chain, 3U),
          "tamper fixture appends three records");
    CHECK(appliance_audit_chain_host_test_tamper(&chain, 1U),
          "host tamper hook rewrites the middle record");
    CHECK(appliance_audit_chain_verify(&chain, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              !verdict.chain_ok && verdict.tamper_detected &&
              verdict.tamper_sequence == 2U &&
              verdict.checked_count == 1U &&
              verdict.head_sequence == 3U,
          "tamper of the middle record is detected at its sequence");
    CHECK(appliance_audit_chain_snapshot(&chain, &snapshot) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              snapshot.state == APPLIANCE_AUDIT_CHAIN_STATE_TAMPERED,
          "a tampered chain publishes the TAMPERED state");
    CHECK(appliance_audit_chain_render_verdict(
              &verdict, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_CHAIN_OK &&
              strstr(rendered,
                     "audit_chain_verify v=1 result=TAMPERED checked=1 "
                     "first=1 head=3 tamper=2") != NULL,
          "tampered verdict renders the offending sequence");

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              appliance_audit_chain_host_test_tamper(&chain, 0U) &&
              appliance_audit_chain_verify(&chain, &verdict) ==
                  APPLIANCE_AUDIT_CHAIN_OK &&
              !verdict.chain_ok && verdict.tamper_sequence == 1U,
          "tamper of the oldest retained record is detected");
    {
        uint8_t digest[SHA256_DIGEST_SIZE];

        CHECK(!appliance_audit_chain_head_digest(&chain, digest),
              "a tampered chain publishes no head digest");
    }

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              append_record(&chain, 3U) &&
              appliance_audit_chain_host_test_tamper(&chain, 2U) &&
              appliance_audit_chain_verify(&chain, &verdict) ==
                  APPLIANCE_AUDIT_CHAIN_OK &&
              !verdict.chain_ok && verdict.tamper_sequence == 3U,
          "tamper of the newest retained record is detected");

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              appliance_audit_chain_host_test_tamper(&chain, 9U) == false,
          "host tamper hook refuses an out-of-range index");

    /* Reordering two entries breaks the link even though both records are
     * individually valid. */
    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              append_record(&chain, 3U),
          "reorder fixture appends three records");
    {
        struct appliance_audit_chain_entry swap =
            chain.entries[0];

        chain.entries[0] = chain.entries[2];
        chain.entries[2] = swap;
    }
    CHECK(appliance_audit_chain_verify(&chain, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              !verdict.chain_ok && verdict.tamper_sequence == 1U,
          "a reordered window is detected at the first mismatch");
}

static void test_full_and_rotation(void)
{
    struct appliance_audit_chain chain;
    struct appliance_audit_chain_verdict verdict;
    struct appliance_audit_chain_snapshot snapshot;
    struct audit_record record;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t anchor[SHA256_DIGEST_SIZE];
    char rendered[APPLIANCE_AUDIT_CHAIN_RENDER_MAX + 1U];
    size_t rendered_length = 0U;
    uint32_t index;

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK,
          "full fixture initializes");
    for (index = 0U; index < APPLIANCE_AUDIT_CHAIN_CAPACITY; ++index) {
        if (!append_record(&chain, index + 1U)) {
            break;
        }
    }
    CHECK(appliance_audit_chain_count(&chain) ==
              APPLIANCE_AUDIT_CHAIN_CAPACITY,
          "capacity records fit exactly");
    CHECK(appliance_audit_chain_append(
              &chain, &(struct audit_record){0}, NULL) ==
              APPLIANCE_AUDIT_CHAIN_INVALID_RECORD,
          "a full chain still validates the incoming record first");
    CHECK(appliance_audit_chain_append(
              &chain,
              &(struct audit_record){
                  .type = AUDIT_RECORD_MANAGEMENT,
                  .operation = 1U,
                  .session_id = 1U,
                  .principal_id = 1U,
                  .command_id = 1U,
              },
              NULL) == APPLIANCE_AUDIT_CHAIN_FULL,
          "a full chain refuses the next record");

    CHECK(appliance_audit_chain_rotate(&chain, 0U, NULL) ==
              APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT,
          "rotation refuses a zero count");
    CHECK(appliance_audit_chain_rotate(
              &chain, APPLIANCE_AUDIT_CHAIN_CAPACITY + 1U, NULL) ==
              APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT,
          "rotation refuses to retire more than it holds");

    CHECK(appliance_audit_chain_rotate(&chain, 4U, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && verdict.anchored &&
              verdict.first_sequence == 5U &&
              verdict.head_sequence == APPLIANCE_AUDIT_CHAIN_CAPACITY &&
              verdict.checked_count ==
                  APPLIANCE_AUDIT_CHAIN_CAPACITY - 4U,
          "rotation moves the anchor and the retained window verifies");
    CHECK(appliance_audit_chain_count(&chain) ==
              APPLIANCE_AUDIT_CHAIN_CAPACITY - 4U &&
              appliance_audit_chain_first_sequence(&chain) == 5U &&
              appliance_audit_chain_next_sequence(&chain) ==
                  APPLIANCE_AUDIT_CHAIN_CAPACITY + 1U,
          "rotation publishes the retained range");
    CHECK(!appliance_audit_chain_find(&chain, 4U, &record, digest) &&
              appliance_audit_chain_find(&chain, 5U, &record, digest) &&
              record.sequence == 5U,
          "retired records are no longer exported");
    CHECK(appliance_audit_chain_snapshot(&chain, &snapshot) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              snapshot.state == APPLIANCE_AUDIT_CHAIN_STATE_OK &&
              snapshot.rotated_count == 4U &&
              !snapshot.anchor_is_genesis &&
              snapshot.appended_count == APPLIANCE_AUDIT_CHAIN_CAPACITY,
          "rotation is visible in the snapshot");
    CHECK(appliance_audit_chain_render_snapshot(
              &snapshot, rendered, sizeof(rendered),
              &rendered_length) == APPLIANCE_AUDIT_CHAIN_OK &&
              strstr(rendered,
                     "state=OK records=28 first=5 next=33 capacity=32 "
                     "appended=32 rotated=4 anchored=YES") != NULL,
          "rotated chain renders the canonical view");

    CHECK(appliance_audit_chain_head_digest(&chain, anchor),
          "rotated chain publishes its anchor");
    CHECK(appliance_audit_chain_rotate(
              &chain, APPLIANCE_AUDIT_CHAIN_CAPACITY, NULL) ==
              APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT,
          "rotation refuses a count larger than the window");
    CHECK(appliance_audit_chain_rotate(&chain, 4U, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && verdict.anchored &&
              verdict.first_sequence == 9U,
          "a second rotation moves the anchor again");
    CHECK(appliance_audit_chain_host_test_tamper_anchor(&chain) &&
              appliance_audit_chain_verify(&chain, &verdict) ==
                  APPLIANCE_AUDIT_CHAIN_OK &&
              !verdict.chain_ok && verdict.tamper_sequence == 9U,
          "a tampered anchor is detected at the retained window start");

    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              appliance_audit_chain_head_digest(&chain, anchor),
          "rotation-continuity fixture appends two records");
    CHECK(appliance_audit_chain_rotate(&chain, 2U, &verdict) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_count(&chain) == 0U &&
              appliance_audit_chain_first_sequence(&chain) == 3U &&
              appliance_audit_chain_next_sequence(&chain) == 3U &&
              verdict.chain_ok && verdict.anchored &&
              verdict.head_sequence == 2U,
          "rotating the whole window keeps the anchor and the sequence");
    CHECK(appliance_audit_chain_head_digest(&chain, digest) &&
              memcmp(digest, anchor, SHA256_DIGEST_SIZE) == 0,
          "an empty rotated chain keeps the head digest as its anchor");
    CHECK(appliance_audit_chain_append(&chain, &(struct audit_record){
              .type = AUDIT_RECORD_MANAGEMENT,
              .operation = 1U,
              .session_id = 1U,
              .principal_id = 1U,
              .command_id = 1U,
          }, NULL) == APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_count(&chain) == 1U &&
              appliance_audit_chain_verify(&chain, &verdict) ==
                  APPLIANCE_AUDIT_CHAIN_OK &&
              verdict.chain_ok && verdict.anchored &&
              verdict.head_sequence == 3U,
          "appends after rotation continue the same anchored chain");
    CHECK(appliance_audit_chain_rotate(&chain, 1U, NULL) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              appliance_audit_chain_count(&chain) == 0U,
          "single-entry rotation retires the last record");
}

static void test_no_allocation(void)
{
    struct appliance_audit_chain chain;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(appliance_audit_chain_init(&chain, 1U) ==
              APPLIANCE_AUDIT_CHAIN_OK &&
              append_record(&chain, 1U) &&
              append_record(&chain, 2U) &&
              appliance_audit_chain_rotate(&chain, 1U, NULL) ==
                  APPLIANCE_AUDIT_CHAIN_OK,
          "no-allocation path completes");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "the audit chain never allocates");
}

int main(void)
{
    test_known_answer_vectors();
    test_status_and_state_strings();
    test_init_failures();
    test_empty_chain();
    test_append_and_verify();
    test_tamper_detection();
    test_full_and_rotation();
    test_no_allocation();

    if (failures != 0) {
        fprintf(stderr, "Appliance audit chain host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance audit chain host tests: ALL OK");
    return 0;
}

#endif
