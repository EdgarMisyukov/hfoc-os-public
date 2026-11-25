#include "update_acceptance.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_UPDATE_ACCEPTANCE_HOST_TEST

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

struct acceptance_fixture {
    bool fail_phase0;
    bool fail_phase1;
    unsigned int phase0_calls;
    unsigned int phase1_calls;
    struct update_acceptance_evidence phase0_evidence;
    struct update_acceptance_evidence phase1_evidence;
};

static bool fixture_write(
    void *context,
    uint8_t phase,
    const struct update_acceptance_evidence *evidence)
{
    struct acceptance_fixture *fixture =
        (struct acceptance_fixture *)context;

    if (evidence == NULL) {
        return false;
    }
    if (phase == 0U) {
        ++fixture->phase0_calls;
        fixture->phase0_evidence = *evidence;
        return !fixture->fail_phase0;
    }
    if (phase == 1U) {
        ++fixture->phase1_calls;
        fixture->phase1_evidence = *evidence;
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

static bool init_intent_with_pending(
    struct activation_intent_store *store)
{
    struct activation_intent_entry known_good = make_known_good();
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;

    memset(&io, 0, sizeof(io));
    memset(store, 0, sizeof(*store));
    if (activation_intent_init(store, &known_good) !=
        ACTIVATION_INTENT_OK) {
        return false;
    }
    if (activation_intent_record(store, &candidate, &io) !=
        ACTIVATION_INTENT_OK) {
        return false;
    }
    return store->pending.pending;
}

static struct first_boot_health_requirements make_requirements(void)
{
    struct first_boot_health_requirements requirements;
    static const struct first_boot_health_milestone milestones[] = {
        {FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT, 1U},
    };
    size_t index;

    memset(&requirements, 0, sizeof(requirements));
    requirements.version = FIRST_BOOT_HEALTH_VERSION;
    requirements.milestone_count =
        sizeof(milestones) / sizeof(milestones[0]);
    for (index = 0U;
         index < requirements.milestone_count;
         ++index) {
        requirements.milestones[index] = milestones[index];
    }
    return requirements;
}

static enum first_boot_health_status report_milestone(
    struct first_boot_health_store *store,
    enum first_boot_health_subsystem subsystem,
    uint32_t milestone_id,
    bool healthy)
{
    struct first_boot_health_report report;

    memset(&report, 0, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = subsystem;
    report.milestone.milestone_id = milestone_id;
    report.healthy = healthy;
    return first_boot_health_report(store, &report);
}

static bool init_ready_health(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();
    struct first_boot_health_verdict verdict;
    size_t index;

    memset(store, 0, sizeof(*store));
    if (first_boot_health_init(store) != FIRST_BOOT_HEALTH_OK ||
        first_boot_health_define(store, &requirements) !=
            FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    for (index = 0U; index < 4U; ++index) {
        enum first_boot_health_subsystem subsystem =
            (enum first_boot_health_subsystem)(index + 1U);

        if (report_milestone(
                store, subsystem, 1U, true) !=
            FIRST_BOOT_HEALTH_OK) {
            return false;
        }
    }
    return first_boot_health_evaluate(store, &verdict) ==
               FIRST_BOOT_HEALTH_OK &&
           verdict.ready;
}

static bool init_not_ready_health(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();
    struct first_boot_health_verdict verdict;

    memset(store, 0, sizeof(*store));
    if (first_boot_health_init(store) != FIRST_BOOT_HEALTH_OK ||
        first_boot_health_define(store, &requirements) !=
            FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    if (report_milestone(
            store,
            FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
            1U,
            true) != FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    return first_boot_health_evaluate(store, &verdict) ==
               FIRST_BOOT_HEALTH_NOT_READY &&
           !verdict.ready;
}

static bool init_failed_health(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();
    struct first_boot_health_verdict verdict;

    memset(store, 0, sizeof(*store));
    if (first_boot_health_init(store) != FIRST_BOOT_HEALTH_OK ||
        first_boot_health_define(store, &requirements) !=
            FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    if (report_milestone(
            store,
            FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
            1U,
            true) != FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    if (report_milestone(
            store,
            FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
            1U,
            false) != FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    return first_boot_health_evaluate(store, &verdict) ==
               FIRST_BOOT_HEALTH_FAILED &&
           verdict.failed;
}

static bool entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           memcmp(left->digest, right->digest,
                  sizeof(left->digest)) == 0;
}

static bool verdicts_equal(
    const struct first_boot_health_verdict *left,
    const struct first_boot_health_verdict *right)
{
    return left->ready == right->ready &&
           left->failed == right->failed &&
           left->required_count == right->required_count &&
           left->observed_count == right->observed_count &&
           left->missing_count == right->missing_count &&
           left->first_missing.subsystem ==
               right->first_missing.subsystem &&
           left->first_missing.milestone_id ==
               right->first_missing.milestone_id;
}

static bool evidence_equals(
    const struct update_acceptance_evidence *left,
    const struct update_acceptance_evidence *right)
{
    return left->version == right->version &&
           entries_equal(&left->accepted_slot,
                         &right->accepted_slot) &&
           entries_equal(&left->rollback_slot,
                         &right->rollback_slot) &&
           verdicts_equal(&left->health_verdict,
                          &right->health_verdict) &&
           left->candidate_active == right->candidate_active;
}

static struct update_acceptance_io make_io(
    struct acceptance_fixture *fixture)
{
    struct update_acceptance_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.write = fixture_write;
    return io;
}

static void test_status_strings(void)
{
    static const struct {
        enum update_acceptance_status status;
        const char *string;
    } statuses[] = {
        {UPDATE_ACCEPTANCE_OK, "UPDATE_ACCEPTANCE_OK"},
        {UPDATE_ACCEPTANCE_NULL, "UPDATE_ACCEPTANCE_NULL"},
        {UPDATE_ACCEPTANCE_BAD_STATE,
         "UPDATE_ACCEPTANCE_BAD_STATE"},
        {UPDATE_ACCEPTANCE_BAD_REQUEST,
         "UPDATE_ACCEPTANCE_BAD_REQUEST"},
        {UPDATE_ACCEPTANCE_NO_PENDING,
         "UPDATE_ACCEPTANCE_NO_PENDING"},
        {UPDATE_ACCEPTANCE_HEALTH_NOT_READY,
         "UPDATE_ACCEPTANCE_HEALTH_NOT_READY"},
        {UPDATE_ACCEPTANCE_HEALTH_FAILED,
         "UPDATE_ACCEPTANCE_HEALTH_FAILED"},
        {UPDATE_ACCEPTANCE_BAD_ROLLBACK,
         "UPDATE_ACCEPTANCE_BAD_ROLLBACK"},
        {UPDATE_ACCEPTANCE_ALREADY_ACCEPTED,
         "UPDATE_ACCEPTANCE_ALREADY_ACCEPTED"},
        {UPDATE_ACCEPTANCE_NO_ACCEPTANCE,
         "UPDATE_ACCEPTANCE_NO_ACCEPTANCE"},
        {UPDATE_ACCEPTANCE_WRITE_FAULT,
         "UPDATE_ACCEPTANCE_WRITE_FAULT"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = update_acceptance_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "update acceptance status string is exact");
    }
    CHECK(update_acceptance_status_string(
              (enum update_acceptance_status)11U) == NULL,
          "update acceptance undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance strings perform no allocation");
}

static void test_init_and_getters(void)
{
    struct update_acceptance_store store;
    struct update_acceptance_evidence evidence;

    CHECK(update_acceptance_init(NULL) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance init rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(update_acceptance_evidence(NULL, &evidence) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance evidence rejects null store");
    CHECK(update_acceptance_evidence(&store, NULL) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance evidence rejects null output");
    CHECK(update_acceptance_evidence(&store, &evidence) ==
              UPDATE_ACCEPTANCE_BAD_STATE,
          "update acceptance evidence rejects uninitialized store");
    CHECK(update_acceptance_init(&store) ==
              UPDATE_ACCEPTANCE_OK &&
              store.version == UPDATE_ACCEPTANCE_VERSION &&
              store.initialized &&
              !store.accepted,
          "update acceptance init publishes clean store");
    CHECK(update_acceptance_init(&store) ==
              UPDATE_ACCEPTANCE_BAD_STATE,
          "update acceptance init rejects reinitialization");
    CHECK(update_acceptance_evidence(&store, &evidence) ==
              UPDATE_ACCEPTANCE_NO_ACCEPTANCE,
          "update acceptance evidence rejects unpublished store");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance init and getters perform no allocation");
}

static void test_commit_validation(void)
{
    struct update_acceptance_store acceptance;
    struct update_acceptance_store original;
    struct update_acceptance_evidence evidence;
    struct activation_intent_store intent;
    struct activation_intent_store intent_original;
    struct first_boot_health_store health;
    struct acceptance_fixture fixture;
    struct update_acceptance_io io = make_io(&fixture);

    memset(&fixture, 0, sizeof(fixture));
    io = make_io(&fixture);
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_commit(
              NULL, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance commit rejects null store");
    CHECK(update_acceptance_commit(
              &acceptance, NULL, &health, &io) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance commit rejects null intent");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, NULL, &io) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance commit rejects null health");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, NULL) ==
              UPDATE_ACCEPTANCE_NULL,
          "update acceptance commit rejects null io");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_BAD_STATE,
          "update acceptance commit rejects uninitialized store");

    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance validation fixture initializes store");
    memcpy(&original, &acceptance, sizeof(acceptance));
    CHECK(init_ready_health(&health),
          "update acceptance validation fixture defines ready health");
    memset(&intent, 0, sizeof(intent));
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_BAD_STATE &&
              memcmp(&acceptance, &original, sizeof(acceptance)) == 0,
          "update acceptance commit rejects uninitialized intent");

    CHECK(init_intent_with_pending(&intent),
          "update acceptance no-pending fixture commits intent");
    intent.pending.pending = false;
    memcpy(&intent_original, &intent, sizeof(intent));
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_NO_PENDING &&
              memcmp(&acceptance, &original, sizeof(acceptance)) == 0 &&
              memcmp(&intent, &intent_original, sizeof(intent)) == 0,
          "update acceptance commit rejects absent pending intent");
    intent.pending.pending = true;

    memset(&health, 0, sizeof(health));
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_BAD_STATE,
          "update acceptance commit rejects undefined health");
    CHECK(init_not_ready_health(&health),
          "update acceptance not-ready fixture defines health");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_HEALTH_NOT_READY,
          "update acceptance commit rejects not-ready health");
    CHECK(init_failed_health(&health),
          "update acceptance failed fixture defines health");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_HEALTH_FAILED,
          "update acceptance commit rejects failed health");
    CHECK(init_ready_health(&health),
          "update acceptance rollback fixture defines ready health");
    intent.known_good.slot_id = 3U;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_BAD_ROLLBACK,
          "update acceptance commit rejects rollback mismatch");
    intent.known_good = intent.pending.rollback;
    intent.pending.target = intent.pending.rollback;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_BAD_REQUEST,
          "update acceptance commit rejects same-slot target");
    CHECK(update_acceptance_evidence(
              &acceptance, &evidence) ==
              UPDATE_ACCEPTANCE_NO_ACCEPTANCE,
          "failed validation leaves update acceptance unpublished");
    CHECK(fixture.phase0_calls == 0U &&
              fixture.phase1_calls == 0U,
          "failed validation invokes no acceptance write gate");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance validation performs no allocation");
}

static void test_successful_commit(void)
{
    struct update_acceptance_store acceptance;
    struct update_acceptance_evidence evidence;
    struct activation_intent_store intent;
    struct activation_intent_store intent_original;
    struct first_boot_health_store health;
    struct first_boot_health_store health_original;
    struct acceptance_fixture fixture;
    struct update_acceptance_io io = make_io(&fixture);
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;

    memset(&fixture, 0, sizeof(fixture));
    io = make_io(&fixture);
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance success fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "update acceptance success fixture commits intent");
    CHECK(init_ready_health(&health),
          "update acceptance success fixture defines health");
    memcpy(&intent_original, &intent, sizeof(intent));
    memcpy(&health_original, &health, sizeof(health));

    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_OK,
          "complete healthy contract commits accepted candidate");
    CHECK(update_acceptance_evidence(
              &acceptance, &evidence) ==
              UPDATE_ACCEPTANCE_OK &&
              evidence.version == UPDATE_ACCEPTANCE_VERSION &&
              entries_equal(&evidence.accepted_slot, &target) &&
              entries_equal(&evidence.rollback_slot, &known_good) &&
              evidence.health_verdict.ready &&
              !evidence.health_verdict.failed &&
              evidence.health_verdict.required_count == 4U &&
              evidence.health_verdict.observed_count == 4U &&
              evidence.health_verdict.missing_count == 0U &&
              evidence.candidate_active,
          "update acceptance evidence records exact accepted candidate");
    CHECK(fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 1U,
          "update acceptance invokes both write phases once");
    CHECK(evidence_equals(&fixture.phase0_evidence, &evidence) &&
              evidence_equals(&fixture.phase1_evidence, &evidence),
          "update acceptance write gates receive exact evidence");
    CHECK(entries_equal(&intent.known_good, &target),
          "update acceptance marks candidate known-good");
    CHECK(activation_intent_known_good(
              &intent, &entry_out) == ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "activation intent known-good getter returns accepted candidate");
    CHECK(activation_intent_resolve(
              &intent, &entry_out) == ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "activation intent resolve returns accepted candidate");
    CHECK(activation_intent_pending(
              &intent, &record_out) == ACTIVATION_INTENT_NO_PENDING,
          "update acceptance clears committed pending intent");
    CHECK(memcmp(&health, &health_original, sizeof(health)) == 0,
          "update acceptance leaves first-boot health unchanged");
    CHECK(acceptance.accepted &&
              acceptance.initialized,
          "update acceptance store publishes one-shot evidence");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_ALREADY_ACCEPTED,
          "update acceptance success is one-shot");
    CHECK(memcmp(&intent, &intent_original, sizeof(intent)) != 0,
          "accepted intent is intentionally committed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance success performs no allocation");
}

static void test_write_faults(void)
{
    struct update_acceptance_store acceptance;
    struct update_acceptance_store acceptance_original;
    struct activation_intent_store intent;
    struct activation_intent_store intent_original;
    struct first_boot_health_store health;
    struct acceptance_fixture fixture;
    struct update_acceptance_io io = make_io(&fixture);

    memset(&fixture, 0, sizeof(fixture));
    io = make_io(&fixture);
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance phase-zero fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "update acceptance phase-zero fixture commits intent");
    CHECK(init_ready_health(&health),
          "update acceptance phase-zero fixture defines health");
    memcpy(&acceptance_original, &acceptance, sizeof(acceptance));
    memcpy(&intent_original, &intent, sizeof(intent));
    fixture.fail_phase0 = true;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_WRITE_FAULT &&
              fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 0U &&
              memcmp(&acceptance, &acceptance_original,
                     sizeof(acceptance)) == 0 &&
              memcmp(&intent, &intent_original, sizeof(intent)) == 0,
          "phase 0 fault leaves both stores unchanged");
    fixture.fail_phase0 = false;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_OK,
          "phase 0 fault may be retried");

    memset(&fixture, 0, sizeof(fixture));
    io = make_io(&fixture);
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance phase-one fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "update acceptance phase-one fixture commits intent");
    CHECK(init_ready_health(&health),
          "update acceptance phase-one fixture defines health");
    memcpy(&acceptance_original, &acceptance, sizeof(acceptance));
    memcpy(&intent_original, &intent, sizeof(intent));
    fixture.fail_phase1 = true;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_WRITE_FAULT &&
              fixture.phase0_calls == 1U &&
              fixture.phase1_calls == 1U &&
              memcmp(&acceptance, &acceptance_original,
                     sizeof(acceptance)) == 0 &&
              memcmp(&intent, &intent_original, sizeof(intent)) == 0,
          "phase 1 fault leaves both stores unchanged");
    fixture.fail_phase1 = false;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_OK,
          "phase 1 fault may be retried");

    memset(&fixture, 0, sizeof(fixture));
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance null-callback fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "update acceptance null-callback fixture commits intent");
    CHECK(init_ready_health(&health),
          "update acceptance null-callback fixture defines health");
    io.write = NULL;
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_OK &&
              acceptance.accepted,
          "null write callback performs in-memory acceptance commit");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance write faults perform no allocation");
}

static void test_restart_selection(void)
{
    struct update_acceptance_store acceptance;
    struct update_acceptance_store persisted_acceptance;
    struct update_acceptance_evidence evidence;
    struct activation_intent_store intent;
    struct activation_intent_store persisted_intent;
    struct first_boot_health_store health;
    struct acceptance_fixture fixture;
    struct update_acceptance_io io = make_io(&fixture);
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry entry_out;
    struct activation_intent_record record_out;

    memset(&fixture, 0, sizeof(fixture));
    io = make_io(&fixture);
    memset(&acceptance, 0, sizeof(acceptance));
    CHECK(update_acceptance_init(&acceptance) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance restart fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "update acceptance restart fixture commits intent");
    CHECK(init_ready_health(&health),
          "update acceptance restart fixture defines health");
    CHECK(update_acceptance_commit(
              &acceptance, &intent, &health, &io) ==
              UPDATE_ACCEPTANCE_OK,
          "update acceptance restart fixture commits candidate");

    memset(&persisted_acceptance, 0,
           sizeof(persisted_acceptance));
    memset(&persisted_intent, 0, sizeof(persisted_intent));
    memcpy(&persisted_acceptance, &acceptance,
           sizeof(acceptance));
    memcpy(&persisted_intent, &intent, sizeof(intent));

    CHECK(activation_intent_known_good(
              &persisted_intent, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "restart known-good getter selects accepted candidate");
    CHECK(activation_intent_resolve(
              &persisted_intent, &entry_out) ==
              ACTIVATION_INTENT_OK &&
              entries_equal(&entry_out, &target),
          "restart resolve selects accepted candidate");
    CHECK(activation_intent_pending(
              &persisted_intent, &record_out) ==
              ACTIVATION_INTENT_NO_PENDING,
          "restart activation intent has no pending record");
    CHECK(update_acceptance_evidence(
              &persisted_acceptance, &evidence) ==
              UPDATE_ACCEPTANCE_OK &&
              entries_equal(&evidence.accepted_slot, &target) &&
              evidence.candidate_active,
          "restart acceptance evidence remains published");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update acceptance restart selection performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_init_and_getters();
    test_commit_validation();
    test_successful_commit();
    test_write_faults();
    test_restart_selection();

    if (failures != 0) {
        fprintf(stderr, "Update acceptance host tests: FAILED\n");
        return 1;
    }
    printf("Update acceptance host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_UPDATE_ACCEPTANCE_HOST_TEST"
#endif
