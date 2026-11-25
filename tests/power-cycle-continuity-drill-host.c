#include "activation_intent.h"
#include "audit.h"
#include "automatic_rollback.h"
#include "candidate_slot_inventory.h"
#include "candidate_staged_write.h"
#include "event_ring.h"
#include "first_boot_health.h"
#include "health_check.h"
#include "service.h"
#include "service_health.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_POWER_CYCLE_CONTINUITY_DRILL_HOST_TEST

#define DRILL_STATE_GENERATION 7U
#define DRILL_KNOWN_GOOD_SLOT 1U
#define DRILL_TARGET_SLOT 2U
#define DRILL_BUNDLE_SIZE 4096U

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

static void fill_digest(uint8_t *digest, uint8_t seed)
{
    size_t index;

    for (index = 0U;
         index < ACTIVATION_INTENT_DIGEST_SIZE;
         ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static struct activation_intent_entry make_known_good(void)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = DRILL_KNOWN_GOOD_SLOT;
    entry.generation = 1U;
    fill_digest(entry.digest, 0x10U);
    return entry;
}

static struct activation_intent_entry make_target_entry(void)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = DRILL_TARGET_SLOT;
    entry.generation = 1U;
    fill_digest(entry.digest, 0x20U);
    return entry;
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

static struct candidate_staged_write_gate make_candidate(void)
{
    struct candidate_staged_write_gate candidate;
    struct activation_intent_entry target = make_target_entry();

    memset(&candidate, 0, sizeof(candidate));
    candidate.version = CANDIDATE_STAGED_WRITE_VERSION;
    candidate.slot_id = target.slot_id;
    candidate.generation = target.generation;
    memcpy(candidate.digest, target.digest, sizeof(candidate.digest));
    candidate.bundle_size = DRILL_BUNDLE_SIZE;
    candidate.ready = true;
    return candidate;
}

static bool fixture_write(
    void *context,
    uint8_t phase,
    const struct activation_intent_record *record)
{
    (void)context;
    (void)phase;
    (void)record;
    return true;
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

static void init_first_boot(struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();

    memset(store, 0, sizeof(*store));
    CHECK(first_boot_health_init(store) == FIRST_BOOT_HEALTH_OK &&
              first_boot_health_define(
                  store, &requirements) == FIRST_BOOT_HEALTH_OK,
          "power-cycle drill defines first-boot requirements");
}

static void report_first_boot_failure(
    struct first_boot_health_store *store)
{
    struct first_boot_health_report report;

    memset(&report, 0, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT;
    report.milestone.milestone_id = 1U;
    report.healthy = false;
    CHECK(first_boot_health_report(store, &report) ==
              FIRST_BOOT_HEALTH_OK,
          "power-cycle drill records failed first-boot milestone");
}

static void test_audit_continuity(void)
{
    struct event_ring ring;
    struct audit_record record;
    struct audit_record records[1];
    uint64_t sequence = UINT64_MAX;
    uint32_t count = 0U;

    memset(&ring, 0, sizeof(ring));
    memset(&record, 0, sizeof(record));
    memset(records, 0, sizeof(records));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK,
          "power-cycle drill initializes audit ring");
    CHECK(audit_record_init(
              &record, AUDIT_RECORD_REBOOT_REASON, 1U, 0U,
              0U, 0U, 0U, UINT64_C(0x189)) == AUDIT_OK,
          "power-cycle drill initializes reboot-reason audit record");
    CHECK(audit_record_append(
              &ring, &record, &sequence) == AUDIT_OK &&
              sequence == 0U &&
              event_ring_count(&ring) == 1U,
          "power-cycle drill appends exact audit sequence");
    CHECK(audit_snapshot(
              &ring, records, 1U, &count) == AUDIT_OK &&
              count == 1U &&
              records[0].sequence == 0U &&
              records[0].type == AUDIT_RECORD_REBOOT_REASON &&
              records[0].operation == 1U &&
              records[0].result == 0U &&
              records[0].detail == UINT64_C(0x189),
          "power-cycle drill snapshot is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "power-cycle audit proof performs no allocation");
}

static void test_state_generation(void)
{
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory original;
    struct candidate_slot_inventory_entry entries[2];
    struct candidate_slot_inventory_entry output;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();

    memset(entries, 0, sizeof(entries));
    entries[0].slot_id = known_good.slot_id;
    entries[0].generation = known_good.generation;
    memcpy(entries[0].digest, known_good.digest,
           sizeof(entries[0].digest));
    entries[1].slot_id = target.slot_id;
    entries[1].generation = target.generation;
    memcpy(entries[1].digest, target.digest,
           sizeof(entries[1].digest));
    memset(&inventory, 0, sizeof(inventory));
    memset(&output, 0, sizeof(output));

    CHECK(candidate_slot_inventory_init(&inventory) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
          candidate_slot_inventory_publish(
              &inventory, entries, 2U,
              DRILL_STATE_GENERATION) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
          candidate_slot_inventory_active(&inventory) &&
          candidate_slot_inventory_generation(&inventory) ==
              DRILL_STATE_GENERATION &&
          candidate_slot_inventory_slot_count(&inventory) == 2U,
          "power-cycle drill publishes coherent state generation");
    CHECK(candidate_slot_inventory_lookup(
              &inventory, DRILL_TARGET_SLOT, &output) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
              output.slot_id == DRILL_TARGET_SLOT &&
              output.generation == 1U &&
              memcmp(output.digest, target.digest,
                     sizeof(target.digest)) == 0,
          "power-cycle drill slot lookup is exact");

    memcpy(&original, &inventory, sizeof(inventory));
    CHECK(candidate_slot_inventory_publish(
              &inventory, entries, 2U,
              DRILL_STATE_GENERATION) ==
              CANDIDATE_SLOT_INVENTORY_STALE_GENERATION &&
              memcmp(&inventory, &original, sizeof(inventory)) == 0,
          "power-cycle drill rejects stale state generation");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "power-cycle state proof performs no allocation");
}

static void test_health_continuity(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptor;
    struct health_check_snapshot snapshot;

    memset(&store, 0, sizeof(store));
    memset(&descriptor, 0, sizeof(descriptor));
    memset(&snapshot, 0, sizeof(snapshot));
    descriptor.version = HEALTH_CHECK_VERSION;
    descriptor.check_id = 1U;
    descriptor.severity = HEALTH_CHECK_SEVERITY_CRITICAL;
    descriptor.cadence_ms = 5000U;

    CHECK(health_check_init(&store) == HEALTH_CHECK_OK &&
          health_check_publish(
              &store, &descriptor, 1U) == HEALTH_CHECK_OK &&
          store.generation == 1U,
          "power-cycle drill publishes health generation");
    CHECK(health_check_result(
              &store, 1U, HEALTH_CHECK_RESULT_PASS) == HEALTH_CHECK_OK &&
          health_check_snapshot(
              &store, &snapshot) == HEALTH_CHECK_OK &&
          snapshot.version == HEALTH_CHECK_VERSION &&
          snapshot.generation == 1U &&
          snapshot.descriptor_count == 1U &&
          snapshot.entries[0].result == HEALTH_CHECK_RESULT_PASS,
          "power-cycle drill health snapshot is coherent");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "power-cycle health proof performs no allocation");
}

static void test_slot_choice_and_recovery_reason(void)
{
    struct activation_intent_store intent;
    struct activation_intent_entry selected;
    struct activation_intent_record pending;
    struct first_boot_health_store health;
    struct first_boot_health_verdict verdict;
    struct automatic_rollback_store rollback;
    struct automatic_rollback_evidence evidence;
    struct candidate_staged_write_gate candidate = make_candidate();
    struct activation_intent_io io;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target = make_target_entry();

    memset(&io, 0, sizeof(io));
    io.write = fixture_write;
    memset(&intent, 0, sizeof(intent));
    memset(&selected, 0, sizeof(selected));
    memset(&pending, 0, sizeof(pending));
    CHECK(activation_intent_init(
              &intent, &known_good) == ACTIVATION_INTENT_OK &&
          activation_intent_record(
              &intent, &candidate, &io) == ACTIVATION_INTENT_OK &&
          activation_intent_pending(
              &intent, &pending) == ACTIVATION_INTENT_OK &&
          pending.pending &&
          entries_equal(&pending.target, &target) &&
          entries_equal(&pending.rollback, &known_good),
          "power-cycle drill records pending slot choice");
    CHECK(activation_intent_resolve(
              &intent, &selected) == ACTIVATION_INTENT_OK &&
              entries_equal(&selected, &target),
          "power-cycle drill resolves pending target slot");

    init_first_boot(&health);
    report_first_boot_failure(&health);
    CHECK(first_boot_health_evaluate(
              &health, &verdict) == FIRST_BOOT_HEALTH_FAILED &&
              verdict.failed,
          "power-cycle drill observes failed first-boot health");
    memset(&rollback, 0, sizeof(rollback));
    memset(&evidence, 0, sizeof(evidence));
    CHECK(automatic_rollback_init(
              &rollback) == AUTOMATIC_ROLLBACK_OK &&
          automatic_rollback_run(
              &rollback, &intent, &health) ==
              AUTOMATIC_ROLLBACK_OK &&
          automatic_rollback_evidence(
              &rollback, &evidence) == AUTOMATIC_ROLLBACK_OK &&
          evidence.reason ==
              AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
          entries_equal(&evidence.selected_slot, &known_good) &&
          entries_equal(&evidence.candidate_slot, &target) &&
          evidence.candidate_active == false &&
          evidence.health_verdict.failed,
          "power-cycle drill recovery reason is exact");
    CHECK(automatic_rollback_reason_string(
              evidence.reason) != NULL &&
              strcmp(
                  automatic_rollback_reason_string(evidence.reason),
                  "AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED") == 0,
          "power-cycle drill recovery reason string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "power-cycle recovery proof performs no allocation");
}

static void test_service_state(void)
{
    struct service_supervisor_snapshot snapshot;
    struct service_supervisor_snapshot_entry entry;
    struct service_health_input input;
    struct service_health_verdict verdict;

    memset(&snapshot, 0, sizeof(snapshot));
    memset(&entry, 0, sizeof(entry));
    memset(&input, 0, sizeof(input));
    memset(&verdict, 0, sizeof(verdict));
    entry.descriptor.id = 1U;
    entry.descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    entry.descriptor.enabled = true;
    entry.descriptor.restart_limit = 0U;
    entry.descriptor.readiness_timeout_ms = 1000U;
    entry.descriptor.cpu_ticks_limit = 0U;
    entry.descriptor.user_memory_limit_pages = 0U;
    strcpy(entry.descriptor.name, "power-cycle");
    entry.state = SERVICE_SUPERVISOR_STATE_RUNNING;
    entry.health_state = SERVICE_HEALTH_READY;
    snapshot.published = true;
    snapshot.version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    snapshot.generation = 1U;
    snapshot.active_count = 1U;
    snapshot.entry_count = 1U;
    snapshot.entries[0] = entry;
    input.version = SERVICE_HEALTH_VERSION;
    input.snapshot = &snapshot;

    CHECK(service_descriptor_validate(
              &entry.descriptor) == SERVICE_DESCRIPTOR_OK &&
          service_health_evaluate(
              &input, &verdict) == SERVICE_HEALTH_OK &&
          verdict.healthy &&
          verdict.result == HEALTH_CHECK_RESULT_PASS &&
          verdict.service_count == 1U &&
          verdict.ready_count == 1U &&
          verdict.pending_count == 0U &&
          verdict.failed_count == 0U &&
          verdict.exhausted_count == 0U &&
          verdict.dependency_blocked_count == 0U &&
          verdict.worst_service_id == 1U,
          "power-cycle drill service state verdict is coherent");
    CHECK(service_health_fault_string(
              verdict.fault) != NULL &&
              strcmp(service_health_fault_string(verdict.fault),
                     "SERVICE_HEALTH_FAULT_NONE") == 0,
          "power-cycle drill service fault string is exact");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "power-cycle service proof performs no allocation");
}

int main(void)
{
    test_audit_continuity();
    test_state_generation();
    test_health_continuity();
    test_slot_choice_and_recovery_reason();
    test_service_state();

    if (failures != 0) {
        fprintf(stderr,
                "Power-cycle continuity drill host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Power-cycle continuity drill host tests: ALL OK\n");
    return 0;
}

#endif
