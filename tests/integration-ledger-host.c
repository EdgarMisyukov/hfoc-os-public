#include "integration_ledger.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INTEGRATION_LEDGER_HOST_TEST

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

static void fill_identity(uint8_t *identity, uint8_t first_byte)
{
    uint32_t index;

    memset(identity, 0, INTEGRATION_LEDGER_IDENTITY_SIZE);
    identity[0] = first_byte;
    for (index = 1U; index < INTEGRATION_LEDGER_IDENTITY_SIZE; ++index) {
        identity[index] = (uint8_t)(first_byte + index);
    }
}

static void make_envelope_identity(uint8_t *identity)
{
    static const unsigned char payload[] = "accounting hand-off payload";
    static const unsigned char correlation_id[
        INTEGRATION_ENVELOPE_CORRELATION_ID_SIZE] = {
        0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
        0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U,
        0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U, 0x18U,
        0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU, 0x20U,
    };
    unsigned char envelope[5120];
    struct integration_envelope_layout layout;
    size_t length = 0U;

    CHECK(integration_envelope_seal(
              INTEGRATION_ENVELOPE_SCHEMA_VERSION_MIN,
              INTEGRATION_ENVELOPE_PURPOSE_AUDIT_HANDOFF,
              correlation_id, payload, sizeof(payload),
              envelope, sizeof(envelope), &length) ==
              INTEGRATION_ENVELOPE_OK,
          "idempotency fixture seals a valid accounting envelope");
    CHECK(integration_envelope_validate(
              envelope, length, &layout) == INTEGRATION_ENVELOPE_OK &&
              layout.valid,
          "idempotency fixture validates the accounting envelope");
    memcpy(identity, envelope + layout.digest_offset,
           INTEGRATION_LEDGER_IDENTITY_SIZE);
}

static struct integration_ledger make_ledger(uint32_t capacity)
{
    struct integration_ledger ledger;

    memset(&ledger, 0xA5, sizeof(ledger));
    CHECK(integration_ledger_init(&ledger, capacity) ==
              INTEGRATION_LEDGER_OK,
          "idempotency ledger fixture initializes");
    return ledger;
}

static void capture_ledger(const struct integration_ledger *ledger,
                           unsigned char *original)
{
    memcpy(original, ledger, sizeof(*ledger));
}

static void check_ledger_unchanged(
    const struct integration_ledger *ledger,
    const unsigned char *original,
    const char *message)
{
    CHECK(memcmp(ledger, original, sizeof(*ledger)) == 0, message);
}

static void test_exact_constants(void)
{
    CHECK(INTEGRATION_LEDGER_VERSION == 1U,
          "idempotency ledger version is exactly 1");
    CHECK(INTEGRATION_LEDGER_IDENTITY_SIZE ==
              INTEGRATION_ENVELOPE_DIGEST_SIZE &&
              INTEGRATION_LEDGER_IDENTITY_SIZE == 32U,
          "idempotency ledger identity is the 32-byte envelope digest");
    CHECK(INTEGRATION_LEDGER_MAX_ENTRIES == 64U,
          "idempotency ledger maximum is exactly 64 entries");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "idempotency constant checking performs no allocation");
}

static void test_ledger_init(void)
{
    struct integration_ledger ledger;
    uint32_t index;

    CHECK(integration_ledger_init(NULL, 1U) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency ledger init rejects null ledger");
    CHECK(integration_ledger_init(&ledger, 0U) ==
              INTEGRATION_LEDGER_BAD_CAPACITY,
          "idempotency ledger init rejects zero capacity");
    CHECK(integration_ledger_init(
              &ledger, INTEGRATION_LEDGER_MAX_ENTRIES + 1U) ==
              INTEGRATION_LEDGER_BAD_CAPACITY,
          "idempotency ledger init rejects over-limit capacity");
    CHECK(integration_ledger_init(&ledger, 3U) ==
              INTEGRATION_LEDGER_OK &&
              ledger.version == INTEGRATION_LEDGER_VERSION &&
              ledger.capacity == 3U &&
              ledger.count == 0U,
          "idempotency ledger init publishes exact empty state");
    for (index = 0U;
         index < INTEGRATION_LEDGER_MAX_ENTRIES; ++index) {
        CHECK(ledger.entries[index].result == 0U &&
                  !ledger.entries[index].pending &&
                  !ledger.entries[index].committed,
              "idempotency ledger init zeroes every entry");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "idempotency ledger init performs no allocation");
}

static void test_validation_failures(void)
{
    struct integration_ledger ledger = make_ledger(3U);
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint8_t zero_identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    struct integration_ledger bad_state = ledger;
    unsigned char original[sizeof(ledger)];
    uint32_t result_out = 0x5A5A5A5AU;

    fill_identity(identity, 0x11U);
    memset(zero_identity, 0, sizeof(zero_identity));
    capture_ledger(&ledger, original);

    CHECK(integration_ledger_prepare(NULL, identity, 7U, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency prepare rejects null ledger");
    CHECK(integration_ledger_prepare(&ledger, NULL, 7U, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency prepare rejects null identity");
    CHECK(integration_ledger_prepare(&ledger, identity, 7U, NULL) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency prepare rejects null result output");
    CHECK(integration_ledger_commit(NULL, identity, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency commit rejects null ledger");
    CHECK(integration_ledger_commit(&ledger, NULL, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency commit rejects null identity");
    CHECK(integration_ledger_commit(&ledger, identity, NULL) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency commit rejects null result output");
    CHECK(integration_ledger_replay(NULL, identity, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency replay rejects null ledger");
    CHECK(integration_ledger_replay(&ledger, NULL, &result_out) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency replay rejects null identity");
    CHECK(integration_ledger_replay(&ledger, identity, NULL) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency replay rejects null result output");
    CHECK(integration_ledger_rollback(NULL, identity) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency rollback rejects null ledger");
    CHECK(integration_ledger_rollback(&ledger, NULL) ==
              INTEGRATION_LEDGER_NULL,
          "idempotency rollback rejects null identity");
    check_ledger_unchanged(&ledger, original,
                           "null idempotency failures leave ledger unchanged");

    bad_state.version = 0U;
    check_ledger_unchanged(
        &ledger, original, "bad-state fixture starts from valid ledger");
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(&bad_state, identity, 7U, &result_out) ==
              INTEGRATION_LEDGER_BAD_STATE &&
              result_out == 0x5A5A5A5AU,
          "idempotency prepare rejects unknown ledger version");
    bad_state = ledger;
    bad_state.capacity = 0U;
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(&bad_state, identity, 7U, &result_out) ==
              INTEGRATION_LEDGER_BAD_CAPACITY &&
              result_out == 0x5A5A5A5AU,
          "idempotency prepare rejects zero capacity");
    bad_state = ledger;
    bad_state.capacity = INTEGRATION_LEDGER_MAX_ENTRIES + 1U;
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(&bad_state, identity, 7U, &result_out) ==
              INTEGRATION_LEDGER_BAD_CAPACITY &&
              result_out == 0x5A5A5A5AU,
          "idempotency prepare rejects over-limit capacity");
    bad_state = ledger;
    bad_state.count = bad_state.capacity + 1U;
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(&bad_state, identity, 7U, &result_out) ==
              INTEGRATION_LEDGER_BAD_STATE &&
              result_out == 0x5A5A5A5AU,
          "idempotency prepare rejects corrupted count");
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(&ledger, zero_identity, 7U,
                                    &result_out) ==
              INTEGRATION_LEDGER_INVALID_IDENTITY &&
              result_out == 0x5A5A5A5AU,
          "idempotency prepare rejects zero identity");
    check_ledger_unchanged(&ledger, original,
                           "idempotency validation failures leave ledger unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "idempotency validation failures perform no allocation");
}

static void test_prepare_commit_replay(void)
{
    struct integration_ledger ledger = make_ledger(4U);
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint32_t result_out = 0x5A5A5A5AU;
    unsigned char before_duplicate[sizeof(ledger)];
    unsigned char before_replay[sizeof(ledger)];

    make_envelope_identity(identity);
    CHECK(integration_ledger_prepare(
              &ledger, identity, 0x1234U, &result_out) ==
              INTEGRATION_LEDGER_OK &&
              result_out == 0x1234U &&
              ledger.count == 1U &&
              ledger.entries[0].pending &&
              !ledger.entries[0].committed &&
              memcmp(ledger.entries[0].identity, identity,
                     INTEGRATION_LEDGER_IDENTITY_SIZE) == 0 &&
              ledger.entries[0].result == 0x1234U,
          "idempotency prepare appends exact envelope identity and result");

    capture_ledger(&ledger, before_duplicate);
    result_out = 0U;
    CHECK(integration_ledger_prepare(
              &ledger, identity, 0x9999U, &result_out) ==
              INTEGRATION_LEDGER_DUPLICATE &&
              result_out == 0x1234U,
          "duplicate pending prepare returns existing result");
    check_ledger_unchanged(&ledger, before_duplicate,
                           "duplicate pending prepare leaves ledger unchanged");

    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_replay(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_NOT_COMMITTED &&
              result_out == 0x5A5A5A5AU,
          "replay before commit reports pending identity as uncommitted");

    result_out = 0U;
    CHECK(integration_ledger_commit(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_OK &&
              result_out == 0x1234U &&
              ledger.count == 1U &&
              !ledger.entries[0].pending &&
              ledger.entries[0].committed &&
              ledger.entries[0].result == 0x1234U,
          "idempotency commit atomically publishes the prepared result");

    capture_ledger(&ledger, before_replay);
    result_out = 0U;
    CHECK(integration_ledger_replay(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_OK &&
              result_out == 0x1234U,
          "idempotency replay returns exact committed result");
    check_ledger_unchanged(&ledger, before_replay,
                           "idempotency replay is read-only");

    capture_ledger(&ledger, before_duplicate);
    result_out = 0U;
    CHECK(integration_ledger_prepare(
              &ledger, identity, 0x7777U, &result_out) ==
              INTEGRATION_LEDGER_ALREADY_COMMITTED &&
              result_out == 0x1234U,
          "prepare after commit returns the existing committed result");
    check_ledger_unchanged(&ledger, before_duplicate,
                           "prepare after commit has no duplicate side effect");

    result_out = 0U;
    CHECK(integration_ledger_commit(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_ALREADY_COMMITTED &&
              result_out == 0x1234U,
          "second commit returns the same committed result");
    check_ledger_unchanged(&ledger, before_duplicate,
                           "second commit has no duplicate side effect");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "idempotency prepare/commit/replay performs no allocation");
}

static void test_commit_not_prepared(void)
{
    struct integration_ledger ledger = make_ledger(2U);
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint32_t result_out = 0x5A5A5A5AU;
    unsigned char original[sizeof(ledger)];

    fill_identity(identity, 0x21U);
    capture_ledger(&ledger, original);
    CHECK(integration_ledger_commit(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_NOT_PREPARED &&
              result_out == 0x5A5A5A5AU,
          "commit without prepare is rejected");
    check_ledger_unchanged(&ledger, original,
                           "commit-without-prepare leaves ledger unchanged");
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_replay(
              &ledger, identity, &result_out) ==
              INTEGRATION_LEDGER_NOT_FOUND &&
              result_out == 0x5A5A5A5AU,
          "replay without entry is not found");
    CHECK(integration_ledger_rollback(&ledger, identity) ==
              INTEGRATION_LEDGER_NOT_FOUND,
          "rollback without entry is not found");
    check_ledger_unchanged(&ledger, original,
                           "absent identity failures leave ledger unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "absent identity failures perform no allocation");
}

static void test_rollback_and_slot_reuse(void)
{
    struct integration_ledger ledger = make_ledger(2U);
    uint8_t first[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint8_t second[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint32_t result_out = 0U;
    unsigned char original[sizeof(ledger)];

    fill_identity(first, 0x31U);
    fill_identity(second, 0x41U);
    CHECK(integration_ledger_prepare(
              &ledger, first, 0x11U, &result_out) ==
              INTEGRATION_LEDGER_OK,
          "rollback fixture prepares first identity");
    CHECK(integration_ledger_prepare(
              &ledger, second, 0x22U, &result_out) ==
              INTEGRATION_LEDGER_OK,
          "rollback fixture prepares second identity");
    capture_ledger(&ledger, original);
    CHECK(integration_ledger_commit(
              &ledger, first, &result_out) == INTEGRATION_LEDGER_OK,
          "rollback fixture commits first identity");
    capture_ledger(&ledger, original);
    CHECK(integration_ledger_rollback(&ledger, first) ==
              INTEGRATION_LEDGER_ALREADY_COMMITTED,
          "committed identity cannot be rolled back");
    check_ledger_unchanged(&ledger, original,
                           "committed rollback leaves ledger unchanged");

    CHECK(integration_ledger_rollback(&ledger, second) ==
              INTEGRATION_LEDGER_OK &&
              ledger.count == 1U &&
              !ledger.entries[1].pending &&
              !ledger.entries[1].committed,
          "rollback removes exactly one pending identity");
    CHECK(integration_ledger_prepare(
              &ledger, second, 0x33U, &result_out) ==
              INTEGRATION_LEDGER_OK &&
              ledger.count == 2U,
          "rolled-back slot is reusable");
    CHECK(integration_ledger_rollback(&ledger, second) ==
              INTEGRATION_LEDGER_OK &&
              ledger.count == 1U,
          "second pending identity rolls back cleanly");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rollback and slot reuse perform no allocation");
}

static void test_full_ledger(void)
{
    struct integration_ledger ledger = make_ledger(2U);
    uint8_t first[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint8_t second[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint8_t third[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint32_t result_out = 0U;
    unsigned char original[sizeof(ledger)];

    fill_identity(first, 0x51U);
    fill_identity(second, 0x61U);
    fill_identity(third, 0x71U);
    CHECK(integration_ledger_prepare(
              &ledger, first, 1U, &result_out) ==
              INTEGRATION_LEDGER_OK,
          "full-ledger fixture accepts first identity");
    CHECK(integration_ledger_prepare(
              &ledger, second, 2U, &result_out) ==
              INTEGRATION_LEDGER_OK,
          "full-ledger fixture accepts second identity");
    capture_ledger(&ledger, original);
    result_out = 0x5A5A5A5AU;
    CHECK(integration_ledger_prepare(
              &ledger, third, 3U, &result_out) ==
              INTEGRATION_LEDGER_FULL &&
              result_out == 0x5A5A5A5AU,
          "full idempotency ledger rejects new identity");
    check_ledger_unchanged(&ledger, original,
                           "full-ledger rejection leaves ledger unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "full-ledger path performs no allocation");
}

static const char *expected_status_string(
    enum integration_ledger_status status)
{
    switch (status) {
    case INTEGRATION_LEDGER_OK:
        return "INTEGRATION_LEDGER_OK";
    case INTEGRATION_LEDGER_NULL:
        return "INTEGRATION_LEDGER_NULL";
    case INTEGRATION_LEDGER_BAD_CAPACITY:
        return "INTEGRATION_LEDGER_BAD_CAPACITY";
    case INTEGRATION_LEDGER_BAD_STATE:
        return "INTEGRATION_LEDGER_BAD_STATE";
    case INTEGRATION_LEDGER_INVALID_IDENTITY:
        return "INTEGRATION_LEDGER_INVALID_IDENTITY";
    case INTEGRATION_LEDGER_FULL:
        return "INTEGRATION_LEDGER_FULL";
    case INTEGRATION_LEDGER_DUPLICATE:
        return "INTEGRATION_LEDGER_DUPLICATE";
    case INTEGRATION_LEDGER_NOT_FOUND:
        return "INTEGRATION_LEDGER_NOT_FOUND";
    case INTEGRATION_LEDGER_NOT_PREPARED:
        return "INTEGRATION_LEDGER_NOT_PREPARED";
    case INTEGRATION_LEDGER_NOT_COMMITTED:
        return "INTEGRATION_LEDGER_NOT_COMMITTED";
    case INTEGRATION_LEDGER_ALREADY_COMMITTED:
        return "INTEGRATION_LEDGER_ALREADY_COMMITTED";
    default:
        return NULL;
    }
}

static void test_status_strings(void)
{
    enum integration_ledger_status status;

    for (status = INTEGRATION_LEDGER_OK;
         status <= INTEGRATION_LEDGER_ALREADY_COMMITTED; ++status) {
        const char *string = integration_ledger_status_string(status);
        const char *expected = expected_status_string(status);

        CHECK(string != NULL && expected != NULL &&
                  strcmp(string, expected) == 0,
              "idempotency ledger status string is exact");
    }
    CHECK(integration_ledger_status_string(
              (enum integration_ledger_status)11U) == NULL,
          "undefined idempotency ledger status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "idempotency status handling performs no allocation");
}

int main(void)
{
    test_exact_constants();
    test_ledger_init();
    test_validation_failures();
    test_prepare_commit_replay();
    test_commit_not_prepared();
    test_rollback_and_slot_reuse();
    test_full_ledger();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "Integration ledger host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Integration ledger host tests: ALL OK\n");
    return 0;
}

#endif
