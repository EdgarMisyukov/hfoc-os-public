#include "activation_intent.h"
#include "candidate_staged_write.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ACTIVATION_INTENT_HOST_TEST

#define TEST_BUNDLE_SIZE 10000U

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

struct intent_fixture {
    bool fail_phase0;
    bool fail_phase1;
    unsigned int phase0_calls;
    unsigned int phase1_calls;
    struct activation_intent_record phase0_record;
    struct activation_intent_record phase1_record;
};

static bool fixture_write(
    void *context,
    uint8_t phase,
    const struct activation_intent_record *record)
{
    struct intent_fixture *fixture = (struct intent_fixture *)context;

    if (record == NULL) {
        return false;
    }
    if (phase == 0U) {
        ++fixture->phase0_calls;
        fixture->phase0_record = *record;
        return !fixture->fail_phase0;
    }
    if (phase == 1U) {
        ++fixture->phase1_calls;
        fixture->phase1_record = *record;
        return !fixture->fail_phase1;
    }
    return false;
}

static struct activation_intent_entry make_known_good(void)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = 1U;
    entry.generation = 1U;
    memset(entry.digest, 0x11, sizeof(entry.digest));
    return entry;
}

static struct activation_intent_entry make_target_entry(void)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = 2U;
    entry.generation = 1U;
    memset(entry.digest, 0x22, sizeof(entry.digest));
    return entry;
}

static struct candidate_staged_write_gate make_candidate(void)
{
    struct candidate_staged_write_gate candidate;
    struct activation_intent_entry target = make_target_entry();

    memset(&candidate, 0, sizeof(candidate));
    candidate.version = CANDIDATE_STAGED_WRITE_VERSION;
    candidate.slot_id = target.slot_id;
    candidate.generation = target.generation;
    memcpy(candidate.digest, target.digest, sizeof(target.digest));
    candidate.bundle_size = TEST_BUNDLE_SIZE;
    candidate.ready = true;
    return candidate;
}

static bool init_store(struct activation_intent_store *store)
{
    struct activation_intent_entry known_good = make_known_good();

    memset(store, 0, sizeof(*store));
    return activation_intent_init(store, &known_good) ==
               ACTIVATION_INTENT_OK;
}

static bool entries_equal(const struct activation_intent_entry *left,
                          const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           memcmp(left->digest, right->digest,
                  sizeof(left->digest)) == 0;
}

static bool records_equal(const struct activation_intent_record *left,
                          const struct activation_intent_record *right)
{
    return left->version == right->version &&
           entries_equal(&left->target, &right->target) &&
           entries_equal(&left->rollback, &right->rollback) &&
           left->bundle_size == right->bundle_size &&
           left->pending == right->pending;
}

static void test_status_strings(void)
{
    static const struct {
        enum activation_intent_status status;
        const char *string;
    } statuses[] = {
        {ACTIVATION_INTENT_OK, "ACTIVATION_INTENT_OK"},
        {ACTIVATION_INTENT_NULL, "ACTIVATION_INTENT_NULL"},
        {ACTIVATION_INTENT_BAD_STATE,
         "ACTIVATION_INTENT_BAD_STATE"},
        {ACTIVATION_INTENT_BAD_REQUEST,
         "ACTIVATION_INTENT_BAD_REQUEST"},
        {ACTIVATION_INTENT_BAD_CANDIDATE,
         "ACTIVATION_INTENT_BAD_CANDIDATE"},
        {ACTIVATION_INTENT_BAD_ROLLBACK,
         "ACTIVATION_INTENT_BAD_ROLLBACK"},
        {ACTIVATION_INTENT_SAME_SLOT,
         "ACTIVATION_INTENT_SAME_SLOT"},
        {ACTIVATION_INTENT_WRITE_FAULT,
         "ACTIVATION_INTENT_WRITE_FAULT"},
        {ACTIVATION_INTENT_NO_PENDING,
         "ACTIVATION_INTENT_NO_PENDING"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = activation_intent_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "activation intent status string is exact");
    }
    CHECK(activation_intent_status_string(
              (enum activation_intent_status)9U) == NULL,
          "activation intent undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation intent status handling performs no allocation");
}

static void test_init_and_null(void)
{
    struct activation_intent_store store;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry invalid = make_known_good();
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;
    struct intent_fixture fixture;

    memset(&io, 0, sizeof(io));
    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = fixture_write;

    CHECK(activation_intent_init(NULL, &known_good) ==
              ACTIVATION_INTENT_NULL,
          "activation intent init rejects null store");
    CHECK(activation_intent_init(&store, NULL) ==
              ACTIVATION_INTENT_NULL,
          "activation intent init rejects null known good");
    invalid.slot_id = 0U;
    memset(&store, 0, sizeof(store));
    CHECK(activation_intent_init(&store, &invalid) ==
              ACTIVATION_INTENT_BAD_ROLLBACK,
          "activation intent init rejects invalid known-good slot");
    invalid = make_known_good();
    invalid.generation = 0U;
    memset(&store, 0, sizeof(store));
    CHECK(activation_intent_init(&store, &invalid) ==
              ACTIVATION_INTENT_BAD_ROLLBACK,
          "activation intent init rejects invalid known-good generation");
    invalid = make_known_good();
    memset(invalid.digest, 0, sizeof(invalid.digest));
    memset(&store, 0, sizeof(store));
    CHECK(activation_intent_init(&store, &invalid) ==
              ACTIVATION_INTENT_BAD_ROLLBACK,
          "activation intent init rejects zero known-good digest");
    CHECK(init_store(&store),
          "activation intent init publishes clean store");
    CHECK(activation_intent_init(&store, &known_good) ==
              ACTIVATION_INTENT_BAD_STATE,
          "activation intent init rejects reinitialization");
    CHECK(store.version == ACTIVATION_INTENT_VERSION &&
              store.initialized &&
              entries_equal(&store.known_good, &known_good),
          "activation intent init publishes exact known good");

    CHECK(activation_intent_known_good(NULL, &entry_out) ==
              ACTIVATION_INTENT_NULL,
          "activation intent known-good getter rejects null store");
    CHECK(activation_intent_known_good(&store, NULL) ==
              ACTIVATION_INTENT_NULL,
          "activation intent known-good getter rejects null output");
    CHECK(activation_intent_pending(NULL, &record_out) ==
              ACTIVATION_INTENT_NULL,
          "activation intent pending getter rejects null store");
    CHECK(activation_intent_pending(&store, NULL) ==
              ACTIVATION_INTENT_NULL,
          "activation intent pending getter rejects null output");
    CHECK(activation_intent_resolve(NULL, &entry_out) ==
              ACTIVATION_INTENT_NULL,
          "activation intent resolve rejects null store");
    CHECK(activation_intent_resolve(&store, NULL) ==
              ACTIVATION_INTENT_NULL,
          "activation intent resolve rejects null output");

    memset(&store, 0, sizeof(store));
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_STATE,
          "activation intent record rejects uninitialized store");
    CHECK(init_store(&store),
          "activation intent null fixture republishes store");
    CHECK(activation_intent_record(
              NULL, &candidate, &io) == ACTIVATION_INTENT_NULL,
          "activation intent record rejects null store");
    CHECK(activation_intent_record(
              &store, NULL, &io) == ACTIVATION_INTENT_NULL,
          "activation intent record rejects null candidate");
    CHECK(activation_intent_record(
              &store, &candidate, NULL) == ACTIVATION_INTENT_NULL,
          "activation intent record rejects null io");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation intent init and null handling performs no allocation");
}

static void test_local_failures_never_commit(void)
{
    struct activation_intent_store store;
    struct activation_intent_store original;
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;
    struct intent_fixture fixture;
    struct activation_intent_entry known_good = make_known_good();

    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = fixture_write;
    CHECK(init_store(&store),
          "activation intent failure fixture initializes store");
    memcpy(&original, &store, sizeof(store));

    candidate.version = 2U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_REQUEST &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U &&
              fixture.phase1_calls == 0U,
          "bad candidate request version cannot commit intent");
    candidate = make_candidate();
    candidate.ready = false;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_CANDIDATE &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "not-ready candidate cannot commit intent");
    candidate = make_candidate();
    candidate.slot_id = 0U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_CANDIDATE &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "invalid candidate slot cannot commit intent");
    candidate = make_candidate();
    candidate.generation = 0U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_CANDIDATE &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "invalid candidate generation cannot commit intent");
    candidate = make_candidate();
    memset(candidate.digest, 0, sizeof(candidate.digest));
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_CANDIDATE &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "zero candidate digest cannot commit intent");
    candidate = make_candidate();
    candidate.bundle_size = 0U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_REQUEST &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "zero candidate bundle size cannot commit intent");
    candidate = make_candidate();
    candidate.bundle_size = ACTIVATION_INTENT_BUNDLE_MAX_SIZE + 1U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_REQUEST &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "over-limit candidate bundle size cannot commit intent");
    candidate = make_candidate();
    candidate.slot_id = known_good.slot_id;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_SAME_SLOT &&
              memcmp(&store, &original, sizeof(store)) == 0 &&
              fixture.phase0_calls == 0U,
          "same-slot candidate cannot commit intent");
    candidate = make_candidate();
    store.known_good.slot_id = 9U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_ROLLBACK &&
              fixture.phase0_calls == 0U,
          "invalid known-good entry cannot commit intent");
    store = original;

    CHECK(activation_intent_pending(&store, &record_out) ==
              ACTIVATION_INTENT_NO_PENDING,
          "local failure fixture has no pending intent");
    CHECK(activation_intent_resolve(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &known_good),
          "resolve without intent returns known good");
    CHECK(activation_intent_known_good(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &known_good),
          "known-good getter returns exact entry");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation intent local failures perform no allocation");
}

static void test_successful_record(void)
{
    struct activation_intent_store store;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;
    struct intent_fixture fixture;
    struct activation_intent_store original;

    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = fixture_write;
    CHECK(init_store(&store),
          "activation intent success fixture initializes store");
    memcpy(&original, &store, sizeof(store));
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_OK,
          "valid candidate records activation intent");
    CHECK(fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 1U,
          "valid activation intent invokes both commit phases once");
    CHECK(!fixture.phase0_record.pending &&
              fixture.phase0_record.version == ACTIVATION_INTENT_VERSION &&
              fixture.phase0_record.bundle_size == TEST_BUNDLE_SIZE &&
              entries_equal(&fixture.phase0_record.target, &target) &&
              entries_equal(&fixture.phase0_record.rollback, &known_good),
          "phase 0 receives exact staged intent");
    CHECK(fixture.phase1_record.pending &&
              fixture.phase1_record.version == ACTIVATION_INTENT_VERSION &&
              fixture.phase1_record.bundle_size == TEST_BUNDLE_SIZE &&
              entries_equal(&fixture.phase1_record.target, &target) &&
              entries_equal(&fixture.phase1_record.rollback, &known_good),
          "phase 1 receives exact committed intent");
    CHECK(store.pending.pending &&
              store.pending.version == ACTIVATION_INTENT_VERSION &&
              store.pending.bundle_size == TEST_BUNDLE_SIZE &&
              entries_equal(&store.pending.target, &target) &&
              entries_equal(&store.pending.rollback, &known_good),
          "store publishes exact committed pending intent");
    CHECK(entries_equal(&store.known_good, &known_good),
          "successful activation intent leaves known good unchanged");
    CHECK(activation_intent_pending(&store, &record_out) ==
              ACTIVATION_INTENT_OK &&
              records_equal(&record_out, &store.pending),
          "pending getter returns exact committed record");
    CHECK(activation_intent_resolve(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "resolve with committed intent returns target");
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_BAD_STATE,
          "activation intent record is one-shot after success");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "successful activation intent performs no allocation");

    CHECK(init_store(&store),
          "activation intent null-callback fixture initializes store");
    io.write = NULL;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_OK &&
              store.pending.pending,
          "null callback publishes in-memory activation intent");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "null-callback activation intent performs no allocation");
}

static void test_torn_intent_resolves_to_known_good(void)
{
    struct activation_intent_store store;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;
    struct intent_fixture fixture;

    memset(&fixture, 0, sizeof(fixture));
    io.context = &fixture;
    io.write = fixture_write;
    CHECK(init_store(&store),
          "activation intent phase-zero fixture initializes store");
    fixture.fail_phase0 = true;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_WRITE_FAULT &&
              fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 0U,
          "phase 0 fault aborts before commit");
    CHECK(activation_intent_pending(&store, &record_out) ==
              ACTIVATION_INTENT_NO_PENDING,
          "phase 0 fault leaves no pending intent");
    CHECK(activation_intent_resolve(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &known_good),
          "phase 0 fault resolves to known good");
    fixture.fail_phase0 = false;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_OK &&
              store.pending.pending,
          "phase 0 fault may be retried");

    CHECK(init_store(&store),
          "activation intent phase-one fixture reinitializes store");
    fixture.fail_phase0 = false;
    fixture.fail_phase1 = true;
    fixture.phase0_calls = 0U;
    fixture.phase1_calls = 0U;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_WRITE_FAULT &&
              fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 1U,
          "phase 1 fault is reported as torn commit");
    CHECK(store.pending.version == ACTIVATION_INTENT_VERSION &&
              !store.pending.pending &&
              store.pending.bundle_size == TEST_BUNDLE_SIZE &&
              entries_equal(&store.pending.target, &target) &&
              entries_equal(&store.pending.rollback, &known_good),
          "phase 1 fault leaves exact torn pending record");
    CHECK(activation_intent_pending(&store, &record_out) ==
              ACTIVATION_INTENT_NO_PENDING,
          "torn pending record is not returned as pending");
    CHECK(activation_intent_resolve(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &known_good) &&
              store.pending.version == 0U,
          "torn intent resolves to previous known good and clears");
    fixture.fail_phase1 = false;
    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_OK &&
              store.pending.pending,
          "torn intent may be retried after resolution");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "torn activation intent performs no allocation");
}

static void test_commit_known_good(void)
{
    struct activation_intent_store store;
    struct activation_intent_store original;
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry entry_out;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;

    memset(&io, 0, sizeof(io));
    CHECK(activation_intent_commit_known_good(NULL) ==
              ACTIVATION_INTENT_NULL,
          "activation intent commit rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(activation_intent_commit_known_good(&store) ==
              ACTIVATION_INTENT_BAD_STATE,
          "activation intent commit rejects uninitialized store");
    CHECK(init_store(&store),
          "activation intent commit fixture initializes store");
    memcpy(&original, &store, sizeof(store));
    CHECK(activation_intent_commit_known_good(&store) ==
              ACTIVATION_INTENT_NO_PENDING &&
              memcmp(&store, &original, sizeof(store)) == 0,
          "activation intent commit rejects absent pending intent");

    CHECK(activation_intent_record(
              &store, &candidate, &io) == ACTIVATION_INTENT_OK,
          "activation intent commit fixture records pending intent");
    store.known_good.slot_id = 3U;
    memcpy(&original, &store, sizeof(store));
    CHECK(activation_intent_commit_known_good(&store) ==
              ACTIVATION_INTENT_BAD_ROLLBACK &&
              memcmp(&store, &original, sizeof(store)) == 0,
          "activation intent commit rejects rollback mismatch");

    store.known_good.slot_id = 1U;
    CHECK(activation_intent_commit_known_good(&store) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&store.known_good, &target) &&
              store.pending.version == 0U &&
              !store.pending.pending,
          "activation intent commit publishes exact known good");
    CHECK(activation_intent_known_good(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "activation intent commit known-good getter returns target");
    CHECK(activation_intent_resolve(&store, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "activation intent commit resolve returns target");
    CHECK(activation_intent_commit_known_good(&store) ==
              ACTIVATION_INTENT_NO_PENDING,
          "activation intent commit is not repeatable without pending");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "activation intent commit performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_null();
    test_local_failures_never_commit();
    test_successful_record();
    test_torn_intent_resolves_to_known_good();
    test_commit_known_good();

    if (failures != 0) {
        fprintf(stderr, "Activation intent host tests: FAILED\n");
        return 1;
    }
    printf("Activation intent host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_ACTIVATION_INTENT_HOST_TEST"
#endif
