#include "storage_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_STORAGE_HEALTH_HOST_TEST

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

struct fixture {
    struct config_kv_entry kv_entries[4U];
    struct config_kv kv;
    struct journal_compaction_checkpoint checkpoints[1U];
    struct journal_compaction_candidate candidate;
    struct journal_compaction_commit commit;
    struct candidate_slot_inventory_entry slots[2U];
    struct candidate_slot_inventory inventory;
    struct activation_intent_entry known_good;
    struct activation_intent_store intent;
    struct candidate_staged_write_gate gate;
};

static void fill_digest(uint8_t *digest, size_t length, uint8_t seed)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static struct candidate_slot_inventory_entry make_slot(
    uint32_t slot_id,
    uint32_t generation,
    uint8_t seed)
{
    struct candidate_slot_inventory_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = slot_id;
    entry.generation = generation;
    fill_digest(entry.digest, sizeof(entry.digest), seed);
    return entry;
}

static bool fixture_init(struct fixture *fixture, bool with_pending)
{
    struct candidate_slot_inventory_entry *slot;
    struct activation_intent_io io;
    bool replaced = false;

    memset(fixture, 0, sizeof(*fixture));
    if (config_kv_init(&fixture->kv, fixture->kv_entries,
                       sizeof(fixture->kv_entries) /
                           sizeof(fixture->kv_entries[0])) != CONFIG_KV_OK) {
        return false;
    }
    if (config_kv_put(&fixture->kv, CONFIG_KV_KIND_CONFIG,
                      "storage", 7U, "active", 6U,
                      &replaced) != CONFIG_KV_OK ||
        config_kv_put(&fixture->kv, CONFIG_KV_KIND_AUDIT,
                      "audit", 5U, "healthy", 7U,
                      &replaced) != CONFIG_KV_OK) {
        return false;
    }

    fixture->checkpoints[0].version =
        JOURNAL_COMPACTION_CHECKPOINT_VERSION;
    fixture->checkpoints[0].sequence = 42U;
    fixture->checkpoints[0].kv = &fixture->kv;
    if (journal_compaction_select(
            &fixture->candidate, fixture->checkpoints, 1U) !=
            JOURNAL_COMPACTION_OK ||
        journal_compaction_commit_init(
            &fixture->commit, &fixture->candidate) !=
            JOURNAL_COMPACTION_OK) {
        return false;
    }

    fixture->slots[0] = make_slot(1U, 1U, 0x11U);
    fixture->slots[1] = make_slot(2U, 2U, 0x21U);
    if (candidate_slot_inventory_init(&fixture->inventory) !=
            CANDIDATE_SLOT_INVENTORY_OK ||
        candidate_slot_inventory_publish(
            &fixture->inventory, fixture->slots, 2U, 1U) !=
            CANDIDATE_SLOT_INVENTORY_OK) {
        return false;
    }

    fixture->known_good.slot_id = 1U;
    fixture->known_good.generation = 1U;
    fill_digest(fixture->known_good.digest,
                sizeof(fixture->known_good.digest), 0x11U);
    if (activation_intent_init(
            &fixture->intent, &fixture->known_good) !=
            ACTIVATION_INTENT_OK) {
        return false;
    }

    if (with_pending) {
        fixture->gate.version = CANDIDATE_STAGED_WRITE_VERSION;
        fixture->gate.slot_id = 2U;
        fixture->gate.generation = 2U;
        fill_digest(fixture->gate.digest,
                    sizeof(fixture->gate.digest), 0x21U);
        fixture->gate.bundle_size = 4096U;
        fixture->gate.ready = true;
        memset(&io, 0, sizeof(io));
        if (activation_intent_record(
                &fixture->intent, &fixture->gate, &io) !=
                ACTIVATION_INTENT_OK) {
            return false;
        }
    }

    slot = &fixture->inventory.slots[0];
    return slot->slot_id == 1U &&
           fixture->intent.initialized;
}

static struct storage_health_input make_input(
    const struct fixture *fixture)
{
    struct storage_health_input input;

    memset(&input, 0, sizeof(input));
    input.version = STORAGE_HEALTH_VERSION;
    input.journal =
        &fixture->commit.slots[fixture->commit.active_slot];
    input.checkpoint_sequence =
        fixture->commit.slots[fixture->commit.active_slot].source_sequence;
    input.inventory = &fixture->inventory;
    input.intent = &fixture->intent;
    return input;
}

static void test_status_and_fault_strings(void)
{
    static const struct {
        enum storage_health_status status;
        const char *string;
    } statuses[] = {
        {STORAGE_HEALTH_OK, "STORAGE_HEALTH_OK"},
        {STORAGE_HEALTH_NULL, "STORAGE_HEALTH_NULL"},
        {STORAGE_HEALTH_BAD_VERSION, "STORAGE_HEALTH_BAD_VERSION"},
        {STORAGE_HEALTH_UNHEALTHY, "STORAGE_HEALTH_UNHEALTHY"},
    };
    static const struct {
        enum storage_health_fault fault;
        const char *string;
    } faults[] = {
        {STORAGE_HEALTH_FAULT_NONE, "STORAGE_HEALTH_FAULT_NONE"},
        {STORAGE_HEALTH_FAULT_JOURNAL, "STORAGE_HEALTH_FAULT_JOURNAL"},
        {STORAGE_HEALTH_FAULT_CHECKPOINT,
         "STORAGE_HEALTH_FAULT_CHECKPOINT"},
        {STORAGE_HEALTH_FAULT_SLOT, "STORAGE_HEALTH_FAULT_SLOT"},
        {STORAGE_HEALTH_FAULT_INTENT, "STORAGE_HEALTH_FAULT_INTENT"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = storage_health_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "storage health status string is exact");
    }
    CHECK(storage_health_status_string(
              (enum storage_health_status)4U) == NULL,
          "storage health undefined status is rejected");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = storage_health_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "storage health fault string is exact");
    }
    CHECK(storage_health_fault_string(
              (enum storage_health_fault)5U) == NULL,
          "storage health undefined fault is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "storage health strings perform no allocation");
}

static void test_null_and_version_failures(void)
{
    struct fixture fixture;
    struct storage_health_input input;
    struct storage_health_verdict verdict;

    CHECK(fixture_init(&fixture, false),
          "storage health null fixture initializes");
    input = make_input(&fixture);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(storage_health_evaluate(NULL, &verdict) ==
              STORAGE_HEALTH_NULL,
          "storage health evaluate rejects null input");
    CHECK(storage_health_evaluate(&input, NULL) ==
              STORAGE_HEALTH_NULL,
          "storage health evaluate rejects null verdict");

    input.journal = NULL;
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_NULL,
          "storage health evaluate rejects null journal");
    input = make_input(&fixture);
    input.inventory = NULL;
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_NULL,
          "storage health evaluate rejects null inventory");
    input = make_input(&fixture);
    input.intent = NULL;
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_NULL,
          "storage health evaluate rejects null intent");

    input = make_input(&fixture);
    input.version = 0U;
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_BAD_VERSION,
          "storage health evaluate rejects zero version");
    input = make_input(&fixture);
    input.version = STORAGE_HEALTH_VERSION + 1U;
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_BAD_VERSION,
          "storage health evaluate rejects future version");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "storage health null and version paths perform no allocation");
}

static void test_healthy_consistency(void)
{
    struct fixture fixture;
    struct storage_health_input input;
    struct storage_health_verdict verdict;

    CHECK(fixture_init(&fixture, false),
          "storage health healthy fixture initializes");
    input = make_input(&fixture);
    memset(&verdict, 0xA5, sizeof(verdict));

    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_OK &&
              verdict.version == STORAGE_HEALTH_VERSION &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.fault == STORAGE_HEALTH_FAULT_NONE &&
              verdict.journal_generation_id == 1U &&
              verdict.checkpoint_sequence == 42U &&
              verdict.slot_generation == 1U &&
              verdict.slot_count == 2U &&
              verdict.known_good_slot_id == 1U,
          "healthy storage state publishes PASS with exact identities");

    CHECK(fixture_init(&fixture, true),
          "storage health pending fixture initializes");
    input = make_input(&fixture);
    memset(&verdict, 0xA5, sizeof(verdict));
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_OK &&
              verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_PASS &&
              verdict.fault == STORAGE_HEALTH_FAULT_NONE &&
              verdict.known_good_slot_id == 1U,
          "pending target and rollback resolve to inventory entries");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "healthy storage evaluation performs no allocation");
}

static void expect_unhealthy(struct fixture *fixture,
                             enum storage_health_fault fault,
                             const char *message)
{
    struct storage_health_input input = make_input(fixture);
    struct storage_health_verdict verdict;

    memset(&verdict, 0, sizeof(verdict));
    CHECK(storage_health_evaluate(&input, &verdict) ==
              STORAGE_HEALTH_UNHEALTHY &&
              !verdict.healthy &&
              verdict.result == HEALTH_CHECK_RESULT_FAIL &&
              verdict.fault == fault,
          message);
}

static void test_injected_faults_are_visible(void)
{
    struct fixture fixture;
    struct journal_compaction_generation *journal;
    struct candidate_slot_inventory *inventory;
    struct activation_intent_store *intent;

    CHECK(fixture_init(&fixture, false),
          "storage health journal fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);

        journal = (struct journal_compaction_generation *)input.journal;
    }
    journal->valid = false;
    expect_unhealthy(&fixture, STORAGE_HEALTH_FAULT_JOURNAL,
                     "invalid compacted journal is visible as FAIL");

    CHECK(fixture_init(&fixture, false),
          "storage health checkpoint fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);
        struct storage_health_verdict verdict;

        input.checkpoint_sequence = 43U;
        memset(&verdict, 0, sizeof(verdict));
        CHECK(storage_health_evaluate(&input, &verdict) ==
                  STORAGE_HEALTH_UNHEALTHY &&
                  !verdict.healthy &&
                  verdict.result == HEALTH_CHECK_RESULT_FAIL &&
                  verdict.fault == STORAGE_HEALTH_FAULT_CHECKPOINT,
              "checkpoint sequence mismatch is visible as FAIL");
    }

    CHECK(fixture_init(&fixture, false),
          "storage health slot fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);

        inventory = (struct candidate_slot_inventory *)input.inventory;
    }
    inventory->slot_count = 0U;
    expect_unhealthy(&fixture, STORAGE_HEALTH_FAULT_SLOT,
                     "empty candidate inventory is visible as FAIL");

    CHECK(fixture_init(&fixture, false),
          "storage health intent fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);

        intent = (struct activation_intent_store *)input.intent;
    }
    intent->known_good.slot_id = 9U;
    expect_unhealthy(&fixture, STORAGE_HEALTH_FAULT_INTENT,
                     "unknown known-good slot is visible as FAIL");

    CHECK(fixture_init(&fixture, true),
          "storage health pending rollback fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);

        intent = (struct activation_intent_store *)input.intent;
    }
    intent->pending.rollback.slot_id = 9U;
    expect_unhealthy(&fixture, STORAGE_HEALTH_FAULT_INTENT,
                     "pending rollback mismatch is visible as FAIL");

    CHECK(fixture_init(&fixture, true),
          "storage health pending target fault fixture initializes");
    {
        struct storage_health_input input = make_input(&fixture);

        intent = (struct activation_intent_store *)input.intent;
    }
    intent->pending.target.slot_id = 9U;
    expect_unhealthy(&fixture, STORAGE_HEALTH_FAULT_INTENT,
                     "unknown pending target is visible as FAIL");

    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "injected storage faults perform no allocation");
}

int main(void)
{
    test_status_and_fault_strings();
    test_null_and_version_failures();
    test_healthy_consistency();
    test_injected_faults_are_visible();

    if (failures != 0) {
        fprintf(stderr, "Storage health host tests: FAILED\n");
        return 1;
    }
    printf("Storage health host tests: ALL OK\n");
    return 0;
}

#endif
