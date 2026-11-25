#include "update_retention.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_UPDATE_RETENTION_HOST_TEST

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

static struct candidate_slot_inventory_entry make_entry(
    uint32_t slot_id,
    uint32_t generation,
    uint8_t seed)
{
    struct candidate_slot_inventory_entry entry;
    size_t index;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = slot_id;
    entry.generation = generation;
    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE; ++index) {
        entry.digest[index] = seed;
    }
    return entry;
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

static struct activation_intent_entry make_target(void)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = 2U;
    entry.generation = 1U;
    memset(entry.digest, 0x22, sizeof(entry.digest));
    return entry;
}

static bool entries_equal(
    const struct candidate_slot_inventory_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           memcmp(left->digest, right->digest,
                  CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE) == 0;
}

static bool entry_zero(
    const struct candidate_slot_inventory_entry *entry)
{
    return entry->slot_id == 0U &&
           entry->generation == 0U &&
           entry->digest[0] == 0U;
}

static struct update_retention_policy make_policy(uint32_t keep)
{
    struct update_retention_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = UPDATE_RETENTION_VERSION;
    policy.max_unreferenced_kept = keep;
    return policy;
}

static bool init_inventory(
    struct candidate_slot_inventory *inventory,
    const struct candidate_slot_inventory_entry *entries,
    size_t count)
{
    memset(inventory, 0, sizeof(*inventory));
    return candidate_slot_inventory_init(inventory) ==
               CANDIDATE_SLOT_INVENTORY_OK &&
           candidate_slot_inventory_publish(
               inventory, entries, count, 1U) ==
               CANDIDATE_SLOT_INVENTORY_OK;
}

static bool init_intent(
    struct activation_intent_store *intent,
    const struct activation_intent_entry *known_good)
{
    memset(intent, 0, sizeof(*intent));
    return activation_intent_init(intent, known_good) ==
               ACTIVATION_INTENT_OK;
}

static bool init_intent_with_pending(
    struct activation_intent_store *intent,
    const struct activation_intent_entry *known_good,
    const struct activation_intent_entry *target)
{
    struct candidate_staged_write_gate candidate;
    struct activation_intent_io io;

    memset(&io, 0, sizeof(io));
    memset(&candidate, 0, sizeof(candidate));
    candidate.version = CANDIDATE_STAGED_WRITE_VERSION;
    candidate.slot_id = target->slot_id;
    candidate.generation = target->generation;
    memcpy(candidate.digest, target->digest, sizeof(target->digest));
    candidate.bundle_size = TEST_BUNDLE_SIZE;
    candidate.ready = true;

    memset(intent, 0, sizeof(*intent));
    if (activation_intent_init(intent, known_good) != ACTIVATION_INTENT_OK) {
        return false;
    }
    return activation_intent_record(intent, &candidate, &io) ==
               ACTIVATION_INTENT_OK &&
           intent->pending.pending;
}

static bool list_has_slot(
    const struct candidate_slot_inventory_entry *entries,
    size_t count,
    uint32_t slot_id)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (entries[index].slot_id == slot_id) {
            return true;
        }
    }
    return false;
}

static void test_status_strings(void)
{
    static const struct {
        enum update_retention_status status;
        const char *string;
    } statuses[] = {
        {UPDATE_RETENTION_OK, "UPDATE_RETENTION_OK"},
        {UPDATE_RETENTION_NULL, "UPDATE_RETENTION_NULL"},
        {UPDATE_RETENTION_BAD_STATE, "UPDATE_RETENTION_BAD_STATE"},
        {UPDATE_RETENTION_BAD_POLICY, "UPDATE_RETENTION_BAD_POLICY"},
        {UPDATE_RETENTION_INACTIVE_INVENTORY,
         "UPDATE_RETENTION_INACTIVE_INVENTORY"},
        {UPDATE_RETENTION_INVALID_INVENTORY,
         "UPDATE_RETENTION_INVALID_INVENTORY"},
        {UPDATE_RETENTION_BAD_INTENT, "UPDATE_RETENTION_BAD_INTENT"},
        {UPDATE_RETENTION_ALREADY_PUBLISHED,
         "UPDATE_RETENTION_ALREADY_PUBLISHED"},
        {UPDATE_RETENTION_NO_EVIDENCE,
         "UPDATE_RETENTION_NO_EVIDENCE"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = update_retention_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "update retention status string is exact");
    }
    CHECK(update_retention_status_string(
              (enum update_retention_status)9U) == NULL,
          "update retention undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention status handling performs no allocation");
}

static void test_init_and_getters(void)
{
    struct update_retention_store store;
    struct update_retention_evidence evidence;

    memset(&store, 0, sizeof(store));
    memset(&evidence, 0xCD, sizeof(evidence));

    CHECK(update_retention_init(NULL) == UPDATE_RETENTION_NULL,
          "update retention init rejects null store");
    CHECK(update_retention_init(&store) == UPDATE_RETENTION_OK &&
              store.version == UPDATE_RETENTION_VERSION &&
              store.initialized && !store.published,
          "update retention init publishes clean store");
    CHECK(update_retention_init(&store) == UPDATE_RETENTION_BAD_STATE,
          "update retention init rejects reinitialization");
    CHECK(update_retention_evidence(NULL, &evidence) ==
              UPDATE_RETENTION_NULL,
          "update retention evidence rejects null store");
    CHECK(update_retention_evidence(&store, NULL) ==
              UPDATE_RETENTION_NULL,
          "update retention evidence rejects null output");
    CHECK(update_retention_evidence(&store, &evidence) ==
              UPDATE_RETENTION_NO_EVIDENCE,
          "update retention evidence is absent before run");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention init/getter handling performs no allocation");
}

static void test_run_validation(void)
{
    struct candidate_slot_inventory_entry entry =
        make_entry(1U, 1U, 0x11U);
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory inactive;
    struct candidate_slot_inventory invalid;
    struct activation_intent_store intent;
    struct activation_intent_store invalid_intent;
    struct update_retention_store store;
    struct update_retention_store uninitialized;
    struct update_retention_policy policy = make_policy(4U);
    struct update_retention_policy bad_policy = make_policy(4U);
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target();
    struct update_retention_store original;

    CHECK(init_inventory(&inventory, &entry, 1U),
          "update retention validation fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention validation fixture initializes intent");

    memset(&store, 0, sizeof(store));
    memset(&uninitialized, 0, sizeof(uninitialized));
    CHECK(update_retention_run(
              &uninitialized, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_BAD_STATE,
          "update retention run rejects uninitialized store");
    CHECK(update_retention_run(
              NULL, &inventory, &intent, &policy) == UPDATE_RETENTION_NULL,
          "update retention run rejects null store");
    CHECK(update_retention_run(
              &store, NULL, &intent, &policy) == UPDATE_RETENTION_NULL,
          "update retention run rejects null inventory");
    CHECK(update_retention_run(
              &store, &inventory, NULL, &policy) == UPDATE_RETENTION_NULL,
          "update retention run rejects null intent");
    CHECK(update_retention_run(
              &store, &inventory, &intent, NULL) == UPDATE_RETENTION_NULL,
          "update retention run rejects null policy");

    CHECK(update_retention_init(&store) == UPDATE_RETENTION_OK,
          "update retention validation store initializes");
    bad_policy.version = 2U;
    CHECK(update_retention_run(
              &store, &inventory, &intent, &bad_policy) ==
              UPDATE_RETENTION_BAD_POLICY &&
              !store.published,
          "update retention run rejects unknown policy version");
    bad_policy = make_policy(
        (uint32_t)UPDATE_RETENTION_POLICY_KEEP_MAX + 1U);
    CHECK(update_retention_run(
              &store, &inventory, &intent, &bad_policy) ==
              UPDATE_RETENTION_BAD_POLICY &&
              !store.published,
          "update retention run rejects over-limit keep policy");

    memset(&inactive, 0, sizeof(inactive));
    CHECK(candidate_slot_inventory_init(&inactive) ==
              CANDIDATE_SLOT_INVENTORY_OK,
          "update retention validation fixture initializes inactive inventory");
    CHECK(update_retention_run(
              &store, &inactive, &intent, &policy) ==
              UPDATE_RETENTION_INACTIVE_INVENTORY &&
              !store.published,
          "update retention run rejects inactive inventory");

    memcpy(&invalid, &inventory, sizeof(invalid));
    invalid.version = 0U;
    CHECK(update_retention_run(
              &store, &invalid, &intent, &policy) ==
              UPDATE_RETENTION_INVALID_INVENTORY &&
              !store.published,
          "update retention run rejects invalid inventory version");
    memcpy(&invalid, &inventory, sizeof(invalid));
    invalid.slot_count = CANDIDATE_SLOT_INVENTORY_MAX_SLOTS + 1U;
    CHECK(update_retention_run(
              &store, &invalid, &intent, &policy) ==
              UPDATE_RETENTION_INVALID_INVENTORY &&
              !store.published,
          "update retention run rejects over-limit inventory count");
    memcpy(&invalid, &inventory, sizeof(invalid));
    invalid.slots[0].slot_id = 0U;
    CHECK(update_retention_run(
              &store, &invalid, &intent, &policy) ==
              UPDATE_RETENTION_INVALID_INVENTORY &&
              !store.published,
          "update retention run rejects invalid inventory slot");
    memcpy(&invalid, &inventory, sizeof(invalid));
    memset(invalid.slots[0].digest, 0,
           sizeof(invalid.slots[0].digest));
    CHECK(update_retention_run(
              &store, &invalid, &intent, &policy) ==
              UPDATE_RETENTION_INVALID_INVENTORY &&
              !store.published,
          "update retention run rejects zero inventory digest");

    memset(&invalid_intent, 0, sizeof(invalid_intent));
    CHECK(update_retention_run(
              &store, &inventory, &invalid_intent, &policy) ==
              UPDATE_RETENTION_BAD_INTENT &&
              !store.published,
          "update retention run rejects uninitialized intent");
    memcpy(&invalid_intent, &intent, sizeof(invalid_intent));
    invalid_intent.known_good.slot_id = 0U;
    CHECK(update_retention_run(
              &store, &inventory, &invalid_intent, &policy) ==
              UPDATE_RETENTION_BAD_INTENT &&
              !store.published,
          "update retention run rejects invalid known-good entry");

    CHECK(init_intent_with_pending(
              &intent, &known_good, &target),
          "update retention validation fixture records pending intent");
    intent.pending.version = 0U;
    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_BAD_INTENT &&
              !store.published,
          "update retention run rejects malformed pending record");
    CHECK(init_intent_with_pending(
              &intent, &known_good, &target),
          "update retention same-slot fixture records pending intent");
    intent.pending.target = intent.pending.rollback;
    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_BAD_INTENT &&
              !store.published,
          "update retention run rejects pending target equal to rollback");

    memcpy(&original, &store, sizeof(store));
    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_BAD_INTENT &&
              memcmp(&store, &original, sizeof(store)) == 0,
          "failed update retention validation leaves store unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention validation performs no allocation");
}

static void test_successful_retention_no_pending(void)
{
    struct candidate_slot_inventory_entry entries[6];
    struct candidate_slot_inventory inventory;
    struct activation_intent_store intent;
    struct candidate_slot_inventory inventory_before;
    struct activation_intent_store intent_before;
    struct update_retention_store store;
    struct update_retention_evidence evidence;
    struct update_retention_policy policy = make_policy(4U);
    struct activation_intent_entry known_good = make_known_good();
    size_t index;

    memset(&store, 0, sizeof(store));
    entries[0] = make_entry(6U, 1U, 0x60U);
    entries[1] = make_entry(1U, 1U, 0x11U);
    entries[2] = make_entry(5U, 1U, 0x50U);
    entries[3] = make_entry(2U, 1U, 0x20U);
    entries[4] = make_entry(4U, 1U, 0x40U);
    entries[5] = make_entry(3U, 1U, 0x30U);

    CHECK(init_inventory(&inventory, entries, 6U),
          "update retention no-pending fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention no-pending fixture initializes intent");
    CHECK(update_retention_init(&store) == UPDATE_RETENTION_OK,
          "update retention no-pending store initializes");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    memcpy(&intent_before, &intent, sizeof(intent));

    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_OK,
          "update retention run publishes no-pending evidence");
    CHECK(update_retention_evidence(&store, &evidence) ==
              UPDATE_RETENTION_OK,
          "update retention evidence getter returns published evidence");
    CHECK(evidence.version == UPDATE_RETENTION_VERSION &&
              evidence.policy_max_unreferenced_kept == 4U &&
              evidence.retained_count == 4U &&
              evidence.reclaim_count == 1U,
          "update retention evidence records exact counts");
    CHECK(entries_equal(&evidence.protected_active, &known_good) &&
              entries_equal(&evidence.protected_rollback, &known_good),
          "update retention evidence protects exact known-good entry");
    CHECK(entry_zero(&evidence.protected_pending),
          "update retention evidence has no pending entry without intent");
    for (index = 0U; index < evidence.retained_count; ++index) {
        CHECK(evidence.retained[index].slot_id == (uint32_t)(index + 2U),
              "update retention retained slots are lowest unreferenced");
    }
    CHECK(evidence.reclaim[0].slot_id == 6U,
          "update retention reclaim slot is exact excess slot");
    CHECK(!list_has_slot(
              evidence.reclaim, evidence.reclaim_count, 1U) &&
              !list_has_slot(
                  evidence.retained, evidence.retained_count, 1U),
          "active generation is never selected for reclamation");
    CHECK(memcmp(&inventory, &inventory_before,
                 sizeof(inventory)) == 0,
          "update retention leaves candidate inventory unchanged");
    CHECK(memcmp(&intent, &intent_before, sizeof(intent)) == 0,
          "update retention leaves activation intent unchanged");
    CHECK(store.published,
          "update retention success publishes one-shot evidence");
    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_ALREADY_PUBLISHED,
          "update retention success is one-shot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention no-pending run performs no allocation");
}

static void test_pending_protection(void)
{
    struct candidate_slot_inventory_entry entries[8];
    struct candidate_slot_inventory inventory;
    struct activation_intent_store intent;
    struct update_retention_store store;
    struct update_retention_evidence evidence;
    struct update_retention_policy policy = make_policy(4U);
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target();
    size_t index;

    memset(&store, 0, sizeof(store));
    for (index = 0U; index < 8U; ++index) {
        entries[index] = make_entry(
            (uint32_t)index + 1U, 1U,
            (uint8_t)((index + 1U) * 0x10U));
    }
    entries[0] = make_entry(1U, 1U, 0x11U);
    entries[1] = make_entry(2U, 1U, 0x22U);

    CHECK(init_inventory(&inventory, entries, 8U),
          "update retention pending fixture publishes inventory");
    CHECK(init_intent_with_pending(
              &intent, &known_good, &target),
          "update retention pending fixture records pending intent");
    CHECK(update_retention_init(&store) == UPDATE_RETENTION_OK,
          "update retention pending store initializes");
    CHECK(update_retention_run(
              &store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_OK,
          "update retention run publishes pending evidence");
    CHECK(update_retention_evidence(&store, &evidence) ==
              UPDATE_RETENTION_OK,
          "update retention pending evidence getter succeeds");
    CHECK(entries_equal(&evidence.protected_active, &known_good) &&
              entries_equal(&evidence.protected_rollback, &known_good) &&
              entries_equal(&evidence.protected_pending, &target),
          "update retention evidence protects rollback and pending target");
    CHECK(evidence.retained_count == 4U &&
              evidence.reclaim_count == 2U,
          "update retention pending evidence records exact counts");
    for (index = 0U; index < evidence.retained_count; ++index) {
        CHECK(evidence.retained[index].slot_id == (uint32_t)(index + 3U),
              "update retention pending retained slots are exact");
    }
    CHECK(evidence.reclaim[0].slot_id == 7U &&
              evidence.reclaim[1].slot_id == 8U,
          "update retention pending reclaim slots are exact");
    CHECK(!list_has_slot(
              evidence.reclaim, evidence.reclaim_count, 1U) &&
              !list_has_slot(
                  evidence.reclaim, evidence.reclaim_count, 2U) &&
              !list_has_slot(
                  evidence.retained, evidence.retained_count, 1U) &&
              !list_has_slot(
                  evidence.retained, evidence.retained_count, 2U),
          "active and pending generations are never reclaimed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention pending run performs no allocation");
}

static void test_zero_and_upper_keep(void)
{
    struct candidate_slot_inventory_entry entries[8];
    struct candidate_slot_inventory inventory;
    struct activation_intent_store intent;
    struct update_retention_store zero_store;
    struct update_retention_store upper_store;
    struct update_retention_evidence zero_evidence;
    struct update_retention_evidence upper_evidence;
    struct update_retention_policy zero_policy = make_policy(0U);
    struct update_retention_policy upper_policy = make_policy(7U);
    struct activation_intent_entry known_good = make_known_good();
    size_t index;

    memset(&zero_store, 0, sizeof(zero_store));
    memset(&upper_store, 0, sizeof(upper_store));
    for (index = 0U; index < 4U; ++index) {
        entries[index] = make_entry(
            (uint32_t)index + 1U, 1U,
            (uint8_t)((index + 1U) * 0x10U));
    }
    entries[0] = make_entry(1U, 1U, 0x11U);
    CHECK(init_inventory(&inventory, entries, 4U),
          "update retention zero-keep fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention zero-keep fixture initializes intent");
    CHECK(update_retention_init(&zero_store) == UPDATE_RETENTION_OK,
          "update retention zero-keep store initializes");
    CHECK(update_retention_run(
              &zero_store, &inventory, &intent, &zero_policy) ==
              UPDATE_RETENTION_OK,
          "update retention zero-keep run succeeds");
    CHECK(update_retention_evidence(
              &zero_store, &zero_evidence) == UPDATE_RETENTION_OK &&
              zero_evidence.retained_count == 0U &&
              zero_evidence.reclaim_count == 3U,
          "zero-keep policy reclaims every unreferenced slot");
    CHECK(zero_evidence.reclaim[0].slot_id == 2U &&
              zero_evidence.reclaim[1].slot_id == 3U &&
              zero_evidence.reclaim[2].slot_id == 4U,
          "zero-keep policy reclaim list is exact");
    CHECK(!list_has_slot(
              zero_evidence.reclaim, zero_evidence.reclaim_count, 1U),
          "zero-keep policy never reclaims active generation");

    for (index = 0U; index < 8U; ++index) {
        entries[index] = make_entry(
            (uint32_t)index + 1U, 1U,
            (uint8_t)((index + 1U) * 0x10U));
    }
    entries[0] = make_entry(1U, 1U, 0x11U);
    CHECK(init_inventory(&inventory, entries, 8U),
          "update retention upper-bound fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention upper-bound fixture initializes intent");
    CHECK(update_retention_init(&upper_store) == UPDATE_RETENTION_OK,
          "update retention upper-bound store initializes");
    CHECK(update_retention_run(
              &upper_store, &inventory, &intent, &upper_policy) ==
              UPDATE_RETENTION_OK,
          "update retention upper-bound run succeeds");
    CHECK(update_retention_evidence(
              &upper_store, &upper_evidence) == UPDATE_RETENTION_OK &&
              upper_evidence.retained_count == 7U &&
              upper_evidence.reclaim_count == 0U,
          "upper-bound policy retains all unreferenced slots");
    CHECK(upper_evidence.retained[0].slot_id == 2U &&
              upper_evidence.retained[6].slot_id == 8U,
          "upper-bound policy retained list is exact");
    CHECK(!list_has_slot(
              upper_evidence.retained, upper_evidence.retained_count, 1U),
          "upper-bound policy never retains active generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention keep policy runs perform no allocation");
}

static void test_deterministic_evidence(void)
{
    struct candidate_slot_inventory_entry entries[6];
    struct candidate_slot_inventory inventory;
    struct activation_intent_store intent;
    struct update_retention_store first_store;
    struct update_retention_store second_store;
    struct update_retention_evidence first_evidence;
    struct update_retention_evidence second_evidence;
    struct update_retention_policy policy = make_policy(2U);
    struct activation_intent_entry known_good = make_known_good();

    memset(&first_store, 0, sizeof(first_store));
    memset(&second_store, 0, sizeof(second_store));
    entries[0] = make_entry(5U, 1U, 0x50U);
    entries[1] = make_entry(1U, 1U, 0x11U);
    entries[2] = make_entry(4U, 1U, 0x40U);
    entries[3] = make_entry(3U, 1U, 0x30U);
    entries[4] = make_entry(2U, 1U, 0x20U);
    entries[5] = make_entry(6U, 1U, 0x60U);

    CHECK(init_inventory(&inventory, entries, 6U),
          "update retention deterministic fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention deterministic fixture initializes intent");
    CHECK(update_retention_init(&first_store) == UPDATE_RETENTION_OK &&
              update_retention_init(&second_store) ==
                  UPDATE_RETENTION_OK,
          "update retention deterministic stores initialize");
    CHECK(update_retention_run(
              &first_store, &inventory, &intent, &policy) ==
              UPDATE_RETENTION_OK &&
              update_retention_run(
                  &second_store, &inventory, &intent, &policy) ==
                  UPDATE_RETENTION_OK,
          "update retention deterministic runs succeed");
    CHECK(update_retention_evidence(
              &first_store, &first_evidence) == UPDATE_RETENTION_OK &&
              update_retention_evidence(
                  &second_store, &second_evidence) ==
                  UPDATE_RETENTION_OK,
          "update retention deterministic evidence getters succeed");
    CHECK(memcmp(&first_evidence, &second_evidence,
                 sizeof(first_evidence)) == 0,
          "update retention evidence is deterministic");
    CHECK(first_evidence.retained_count == 2U &&
              first_evidence.reclaim_count == 3U &&
              first_evidence.retained[0].slot_id == 2U &&
              first_evidence.retained[1].slot_id == 3U &&
              first_evidence.reclaim[0].slot_id == 4U &&
              first_evidence.reclaim[1].slot_id == 5U &&
              first_evidence.reclaim[2].slot_id == 6U,
          "update retention evidence uses deterministic slot ordering");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention deterministic run performs no allocation");
}

static void test_failed_retry(void)
{
    struct candidate_slot_inventory_entry entry =
        make_entry(1U, 1U, 0x11U);
    struct candidate_slot_inventory inventory;
    struct activation_intent_store intent;
    struct update_retention_store store;
    struct update_retention_policy bad_policy = make_policy(8U);
    struct update_retention_policy good_policy = make_policy(0U);
    struct activation_intent_entry known_good = make_known_good();

    memset(&store, 0, sizeof(store));
    CHECK(init_inventory(&inventory, &entry, 1U),
          "update retention retry fixture publishes inventory");
    CHECK(init_intent(&intent, &known_good),
          "update retention retry fixture initializes intent");
    CHECK(update_retention_init(&store) == UPDATE_RETENTION_OK,
          "update retention retry store initializes");
    CHECK(update_retention_run(
              &store, &inventory, &intent, &bad_policy) ==
              UPDATE_RETENTION_BAD_POLICY,
          "update retention failed validation is rejected");
    CHECK(!store.published,
          "update retention failed validation remains unpublished");
    CHECK(update_retention_run(
              &store, &inventory, &intent, &good_policy) ==
              UPDATE_RETENTION_OK &&
              store.published,
          "update retention corrected run can retry after failure");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update retention retry performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_getters();
    test_run_validation();
    test_successful_retention_no_pending();
    test_pending_protection();
    test_zero_and_upper_keep();
    test_deterministic_evidence();
    test_failed_retry();

    if (failures != 0) {
        fprintf(stderr, "Update retention host tests: FAILED\n");
        return 1;
    }
    printf("Update retention host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_UPDATE_RETENTION_HOST_TEST"
#endif
