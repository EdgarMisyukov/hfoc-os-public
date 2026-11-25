#include "automatic_rollback.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_AUTOMATIC_ROLLBACK_HOST_TEST

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

static bool entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           memcmp(left->digest, right->digest,
                  sizeof(left->digest)) == 0;
}

static void test_status_and_reason_strings(void)
{
    static const struct {
        enum automatic_rollback_status status;
        const char *string;
    } statuses[] = {
        {AUTOMATIC_ROLLBACK_OK, "AUTOMATIC_ROLLBACK_OK"},
        {AUTOMATIC_ROLLBACK_NULL, "AUTOMATIC_ROLLBACK_NULL"},
        {AUTOMATIC_ROLLBACK_BAD_STATE,
         "AUTOMATIC_ROLLBACK_BAD_STATE"},
        {AUTOMATIC_ROLLBACK_BAD_REQUEST,
         "AUTOMATIC_ROLLBACK_BAD_REQUEST"},
        {AUTOMATIC_ROLLBACK_NO_PENDING,
         "AUTOMATIC_ROLLBACK_NO_PENDING"},
        {AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED,
         "AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED"},
        {AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED,
         "AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED"},
        {AUTOMATIC_ROLLBACK_NO_EVIDENCE,
         "AUTOMATIC_ROLLBACK_NO_EVIDENCE"},
    };
    static const struct {
        enum automatic_rollback_reason reason;
        const char *string;
    } reasons[] = {
        {AUTOMATIC_ROLLBACK_REASON_NONE,
         "AUTOMATIC_ROLLBACK_REASON_NONE"},
        {AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED,
         "AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = automatic_rollback_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "automatic rollback status string is exact");
    }
    CHECK(automatic_rollback_status_string(
              (enum automatic_rollback_status)8U) == NULL,
          "automatic rollback undefined status is rejected");
    for (index = 0U;
         index < sizeof(reasons) / sizeof(reasons[0]);
         ++index) {
        const char *string = automatic_rollback_reason_string(
            reasons[index].reason);

        CHECK(string != NULL &&
                  strcmp(string, reasons[index].string) == 0,
              "automatic rollback reason string is exact");
    }
    CHECK(automatic_rollback_reason_string(
              (enum automatic_rollback_reason)2U) == NULL,
          "automatic rollback undefined reason is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "automatic rollback strings perform no allocation");
}

static void test_init_and_getters(void)
{
    struct automatic_rollback_store store;
    struct automatic_rollback_evidence evidence;
    struct activation_intent_entry entry;

    CHECK(automatic_rollback_init(NULL) ==
              AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback init rejects null store");
    memset(&store, 0, sizeof(store));
    CHECK(automatic_rollback_evidence(NULL, &evidence) ==
              AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback evidence rejects null store");
    CHECK(automatic_rollback_evidence(&store, NULL) ==
              AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback evidence rejects null output");
    CHECK(automatic_rollback_selected(NULL, &entry) ==
              AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback selected rejects null store");
    CHECK(automatic_rollback_selected(&store, NULL) ==
              AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback selected rejects null output");
    CHECK(automatic_rollback_evidence(&store, &evidence) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback evidence rejects uninitialized store");
    CHECK(automatic_rollback_selected(&store, &entry) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback selected rejects uninitialized store");
    CHECK(automatic_rollback_init(&store) ==
              AUTOMATIC_ROLLBACK_OK &&
              store.version == AUTOMATIC_ROLLBACK_VERSION &&
              store.initialized,
          "automatic rollback init publishes clean store");
    CHECK(automatic_rollback_init(&store) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback init rejects reinitialization");
    CHECK(automatic_rollback_evidence(&store, &evidence) ==
              AUTOMATIC_ROLLBACK_NO_EVIDENCE,
          "automatic rollback evidence rejects unpublished store");
    CHECK(automatic_rollback_selected(&store, &entry) ==
              AUTOMATIC_ROLLBACK_NO_EVIDENCE,
          "automatic rollback selected rejects unpublished store");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "automatic rollback init and getters perform no allocation");
}

static void test_run_validation(void)
{
    struct automatic_rollback_store rollback;
    struct automatic_rollback_store original;
    struct automatic_rollback_evidence evidence;
    struct activation_intent_store intent;
    struct first_boot_health_store health;

    memset(&rollback, 0, sizeof(rollback));
    CHECK(automatic_rollback_run(
              NULL, &intent, &health) == AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback run rejects null store");
    CHECK(automatic_rollback_run(
              &rollback, NULL, &health) == AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback run rejects null intent");
    CHECK(automatic_rollback_run(
              &rollback, &intent, NULL) == AUTOMATIC_ROLLBACK_NULL,
          "automatic rollback run rejects null health");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback run rejects uninitialized store");

    CHECK(automatic_rollback_init(&rollback) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback validation fixture initializes store");
    memcpy(&original, &rollback, sizeof(rollback));
    CHECK(init_failed_health(&health),
          "automatic rollback validation fixture defines failed health");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_BAD_STATE &&
              memcmp(&rollback, &original, sizeof(rollback)) == 0,
          "automatic rollback run rejects uninitialized intent");

    CHECK(init_intent_with_pending(&intent),
          "automatic rollback no-pending fixture commits intent");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback failed-health fixture publishes selection");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED,
          "automatic rollback run is one-shot after success");

    memset(&rollback, 0, sizeof(rollback));
    CHECK(automatic_rollback_init(&rollback) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback no-pending fixture initializes store");
    memset(&intent, 0, sizeof(intent));
    CHECK(init_failed_health(&health),
          "automatic rollback no-pending fixture defines failed health");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback run rejects uninitialized intent without pending");
    CHECK(init_intent_with_pending(&intent),
          "automatic rollback no-pending fixture commits pending intent");
    memcpy(&original, &rollback, sizeof(rollback));
    intent.pending.pending = false;
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_NO_PENDING &&
              memcmp(&rollback, &original, sizeof(rollback)) == 0,
          "automatic rollback run rejects absent pending intent");
    intent.pending.pending = true;

    memset(&rollback, 0, sizeof(rollback));
    CHECK(automatic_rollback_init(&rollback) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback undefined-health fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "automatic rollback undefined-health fixture commits intent");
    memset(&health, 0, sizeof(health));
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback run rejects undefined health");

    CHECK(init_not_ready_health(&health),
          "automatic rollback not-ready fixture defines health");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED,
          "automatic rollback run rejects not-ready health");
    CHECK(init_ready_health(&health),
          "automatic rollback ready fixture defines health");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_HEALTH_NOT_FAILED,
          "automatic rollback run rejects healthy health");
    CHECK(automatic_rollback_evidence(&rollback, &evidence) ==
              AUTOMATIC_ROLLBACK_NO_EVIDENCE,
          "failed validation leaves automatic rollback unpublished");

    memset(&rollback, 0, sizeof(rollback));
    CHECK(automatic_rollback_init(&rollback) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback mismatch fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "automatic rollback mismatch fixture commits intent");
    CHECK(init_failed_health(&health),
          "automatic rollback mismatch fixture defines failed health");
    intent.known_good.slot_id = 3U;
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_BAD_STATE,
          "automatic rollback run rejects rollback mismatch");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "automatic rollback validation performs no allocation");
}

static void test_successful_selection(void)
{
    struct automatic_rollback_store rollback;
    struct automatic_rollback_evidence evidence;
    struct activation_intent_store intent;
    struct activation_intent_store intent_original;
    struct first_boot_health_store health;
    struct first_boot_health_store health_original;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();
    struct activation_intent_entry selected;

    memset(&rollback, 0, sizeof(rollback));
    CHECK(automatic_rollback_init(&rollback) ==
              AUTOMATIC_ROLLBACK_OK,
          "automatic rollback success fixture initializes store");
    CHECK(init_intent_with_pending(&intent),
          "automatic rollback success fixture commits intent");
    CHECK(init_failed_health(&health),
          "automatic rollback success fixture defines failed health");
    memcpy(&intent_original, &intent, sizeof(intent));
    memcpy(&health_original, &health, sizeof(health));

    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_OK,
          "failed candidate health publishes automatic rollback selection");
    CHECK(automatic_rollback_evidence(&rollback, &evidence) ==
              AUTOMATIC_ROLLBACK_OK &&
              evidence.version == AUTOMATIC_ROLLBACK_VERSION &&
              evidence.reason ==
                  AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
              !evidence.candidate_active,
          "automatic rollback evidence records failed-health reason and non-active candidate");
    CHECK(entries_equal(&evidence.selected_slot, &known_good),
          "automatic rollback selects exact last-known-good slot");
    CHECK(entries_equal(&evidence.candidate_slot, &target),
          "automatic rollback evidence records exact candidate slot");
    CHECK(!entries_equal(&evidence.selected_slot, &evidence.candidate_slot),
          "automatic rollback never selects candidate target");
    CHECK(evidence.health_verdict.failed &&
              evidence.health_verdict.required_count == 4U &&
              evidence.health_verdict.observed_count == 2U,
          "automatic rollback evidence copies failed health verdict");
    CHECK(automatic_rollback_selected(&rollback, &selected) ==
              AUTOMATIC_ROLLBACK_OK &&
              entries_equal(&selected, &known_good),
          "automatic rollback selected getter returns last-known-good slot");
    CHECK(memcmp(&intent, &intent_original, sizeof(intent)) == 0,
          "automatic rollback leaves activation intent unchanged");
    CHECK(memcmp(&health, &health_original, sizeof(health)) == 0,
          "automatic rollback leaves first-boot health unchanged");
    CHECK(rollback.published && rollback.initialized,
          "automatic rollback store publishes one-shot evidence");
    CHECK(automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_ALREADY_PUBLISHED,
          "automatic rollback success is one-shot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "automatic rollback selection performs no allocation");
}

int main(void)
{
    test_status_and_reason_strings();
    test_init_and_getters();
    test_run_validation();
    test_successful_selection();

    if (failures != 0) {
        fprintf(stderr, "Automatic rollback host tests: FAILED\n");
        return 1;
    }
    printf("Automatic rollback host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_AUTOMATIC_ROLLBACK_HOST_TEST"
#endif
