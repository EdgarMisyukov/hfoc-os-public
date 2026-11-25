#ifdef HFOC_POWER_CYCLE_CONTINUITY_DRILL_QEMU_TEST

#include "activation_intent.h"
#include "audit.h"
#include "automatic_rollback.h"
#include "candidate_slot_inventory.h"
#include "candidate_staged_write.h"
#include "event_ring.h"
#include "first_boot_health.h"
#include "health_check.h"
#include "serial.h"
#include "service.h"
#include "service_health.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define POWER_CYCLE_DRILL_STATE_GENERATION 7U
#define POWER_CYCLE_DRILL_KNOWN_GOOD_SLOT 1U
#define POWER_CYCLE_DRILL_TARGET_SLOT 2U
#define POWER_CYCLE_DRILL_BUNDLE_SIZE 4096U

static void power_cycle_drill_fail(const char *message)
    __attribute__((noreturn));

static void power_cycle_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[POWER-CYCLE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void power_cycle_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        power_cycle_drill_fail(message);
    }
}

static void power_cycle_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void power_cycle_drill_copy(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *const out = destination;
    const unsigned char *const in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool power_cycle_drill_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *const a = left;
    const unsigned char *const b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static bool power_cycle_drill_string_equal(
    const char *left,
    const char *right)
{
    size_t index = 0U;

    for (;;) {
        if (left[index] != right[index]) {
            return false;
        }
        if (left[index] == '\0') {
            return true;
        }
        ++index;
    }
}

static bool power_cycle_drill_entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           power_cycle_drill_equal(
               left->digest, right->digest,
               sizeof(left->digest));
}

static void power_cycle_drill_fill_digest(
    uint8_t *digest,
    uint8_t seed)
{
    size_t index;

    for (index = 0U;
         index < ACTIVATION_INTENT_DIGEST_SIZE;
         ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static struct activation_intent_entry
power_cycle_drill_make_known_good(void)
{
    struct activation_intent_entry entry;

    power_cycle_drill_zero(&entry, sizeof(entry));
    entry.slot_id = POWER_CYCLE_DRILL_KNOWN_GOOD_SLOT;
    entry.generation = 1U;
    power_cycle_drill_fill_digest(entry.digest, 0x10U);
    return entry;
}

static struct activation_intent_entry
power_cycle_drill_make_target(void)
{
    struct activation_intent_entry entry;

    power_cycle_drill_zero(&entry, sizeof(entry));
    entry.slot_id = POWER_CYCLE_DRILL_TARGET_SLOT;
    entry.generation = 1U;
    power_cycle_drill_fill_digest(entry.digest, 0x20U);
    return entry;
}

static struct candidate_staged_write_gate
power_cycle_drill_make_candidate(void)
{
    struct candidate_staged_write_gate candidate;
    struct activation_intent_entry target =
        power_cycle_drill_make_target();

    power_cycle_drill_zero(&candidate, sizeof(candidate));
    candidate.version = CANDIDATE_STAGED_WRITE_VERSION;
    candidate.slot_id = target.slot_id;
    candidate.generation = target.generation;
    power_cycle_drill_copy(
        candidate.digest, target.digest, sizeof(candidate.digest));
    candidate.bundle_size = POWER_CYCLE_DRILL_BUNDLE_SIZE;
    candidate.ready = true;
    return candidate;
}

static bool power_cycle_drill_fixture_write(
    void *context,
    uint8_t phase,
    const struct activation_intent_record *record)
{
    (void)context;
    (void)phase;
    (void)record;
    return true;
}

static struct first_boot_health_requirements
power_cycle_drill_make_requirements(void)
{
    static const struct first_boot_health_milestone milestones[] = {
        {FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT, 1U},
    };
    struct first_boot_health_requirements requirements;
    size_t index;

    power_cycle_drill_zero(&requirements, sizeof(requirements));
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

static void power_cycle_drill_init_first_boot(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        power_cycle_drill_make_requirements();

    power_cycle_drill_zero(store, sizeof(*store));
    power_cycle_drill_require(
        first_boot_health_init(store) == FIRST_BOOT_HEALTH_OK &&
        first_boot_health_define(
            store, &requirements) == FIRST_BOOT_HEALTH_OK,
        "first-boot requirements");
}

static void power_cycle_drill_report_failure(
    struct first_boot_health_store *store)
{
    struct first_boot_health_report report;

    power_cycle_drill_zero(&report, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT;
    report.milestone.milestone_id = 1U;
    report.healthy = false;
    power_cycle_drill_require(
        first_boot_health_report(store, &report) ==
            FIRST_BOOT_HEALTH_OK,
        "failed first-boot milestone");
}

static void power_cycle_drill_copy_name(
    char *destination,
    const char *source)
{
    uint32_t index = 0U;

    while (source[index] != '\0' && index < SERVICE_NAME_MAX) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static void power_cycle_drill_prove_audit(void)
{
    struct event_ring ring;
    struct audit_record record;
    struct audit_record records[1];
    uint64_t sequence = 0U;
    uint32_t count = 0U;

    power_cycle_drill_zero(&ring, sizeof(ring));
    power_cycle_drill_zero(&record, sizeof(record));
    power_cycle_drill_zero(records, sizeof(records));
    power_cycle_drill_require(
        event_ring_init(&ring) == EVENT_RING_OK,
        "audit ring init");
    power_cycle_drill_require(
        audit_record_init(
            &record, AUDIT_RECORD_REBOOT_REASON, 1U, 0U,
            0U, 0U, 0U, UINT64_C(0x189)) == AUDIT_OK,
        "reboot-reason audit record");
    power_cycle_drill_require(
        audit_record_append(
            &ring, &record, &sequence) == AUDIT_OK &&
        sequence == 0U &&
        event_ring_count(&ring) == 1U,
        "audit sequence");
    power_cycle_drill_require(
        audit_snapshot(
            &ring, records, 1U, &count) == AUDIT_OK &&
        count == 1U &&
        records[0].sequence == 0U &&
        records[0].type == AUDIT_RECORD_REBOOT_REASON &&
        records[0].operation == 1U &&
        records[0].result == 0U &&
        records[0].detail == UINT64_C(0x189),
        "audit snapshot");
}

static void power_cycle_drill_prove_state_generation(void)
{
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory original;
    struct candidate_slot_inventory_entry entries[2];
    struct candidate_slot_inventory_entry output;
    struct activation_intent_entry known_good =
        power_cycle_drill_make_known_good();
    struct activation_intent_entry target =
        power_cycle_drill_make_target();

    power_cycle_drill_zero(entries, sizeof(entries));
    entries[0].slot_id = known_good.slot_id;
    entries[0].generation = known_good.generation;
    power_cycle_drill_copy(
        entries[0].digest, known_good.digest,
        sizeof(entries[0].digest));
    entries[1].slot_id = target.slot_id;
    entries[1].generation = target.generation;
    power_cycle_drill_copy(
        entries[1].digest, target.digest,
        sizeof(entries[1].digest));
    power_cycle_drill_zero(&inventory, sizeof(inventory));
    power_cycle_drill_zero(&output, sizeof(output));

    power_cycle_drill_require(
        candidate_slot_inventory_init(&inventory) ==
            CANDIDATE_SLOT_INVENTORY_OK &&
        candidate_slot_inventory_publish(
            &inventory, entries, 2U,
            POWER_CYCLE_DRILL_STATE_GENERATION) ==
            CANDIDATE_SLOT_INVENTORY_OK &&
        candidate_slot_inventory_active(&inventory) &&
        candidate_slot_inventory_generation(&inventory) ==
            POWER_CYCLE_DRILL_STATE_GENERATION &&
        candidate_slot_inventory_slot_count(&inventory) == 2U,
        "state generation");
    power_cycle_drill_require(
        candidate_slot_inventory_lookup(
            &inventory, POWER_CYCLE_DRILL_TARGET_SLOT, &output) ==
            CANDIDATE_SLOT_INVENTORY_OK &&
        output.slot_id == POWER_CYCLE_DRILL_TARGET_SLOT &&
        output.generation == 1U &&
        power_cycle_drill_equal(
            output.digest, target.digest, sizeof(target.digest)),
        "slot lookup");
    power_cycle_drill_copy(
        &original, &inventory, sizeof(inventory));
    power_cycle_drill_require(
        candidate_slot_inventory_publish(
            &inventory, entries, 2U,
            POWER_CYCLE_DRILL_STATE_GENERATION) ==
            CANDIDATE_SLOT_INVENTORY_STALE_GENERATION &&
        power_cycle_drill_equal(
            &inventory, &original, sizeof(inventory)),
        "stale state generation rejected");
}

static void power_cycle_drill_prove_health(void)
{
    struct health_check_store store;
    struct health_check_descriptor descriptor;
    struct health_check_snapshot snapshot;

    power_cycle_drill_zero(&store, sizeof(store));
    power_cycle_drill_zero(&descriptor, sizeof(descriptor));
    power_cycle_drill_zero(&snapshot, sizeof(snapshot));
    descriptor.version = HEALTH_CHECK_VERSION;
    descriptor.check_id = 1U;
    descriptor.severity = HEALTH_CHECK_SEVERITY_CRITICAL;
    descriptor.cadence_ms = 5000U;

    power_cycle_drill_require(
        health_check_init(&store) == HEALTH_CHECK_OK &&
        health_check_publish(
            &store, &descriptor, 1U) == HEALTH_CHECK_OK &&
        store.generation == 1U,
        "health generation");
    power_cycle_drill_require(
        health_check_result(
            &store, 1U, HEALTH_CHECK_RESULT_PASS) == HEALTH_CHECK_OK &&
        health_check_snapshot(
            &store, &snapshot) == HEALTH_CHECK_OK &&
        snapshot.version == HEALTH_CHECK_VERSION &&
        snapshot.generation == 1U &&
        snapshot.descriptor_count == 1U &&
        snapshot.entries[0].result == HEALTH_CHECK_RESULT_PASS,
        "health snapshot");
}

static void power_cycle_drill_prove_slot_and_recovery(void)
{
    struct activation_intent_store intent;
    struct activation_intent_entry selected;
    struct activation_intent_record pending;
    struct first_boot_health_store health;
    struct first_boot_health_verdict verdict;
    struct automatic_rollback_store rollback;
    struct automatic_rollback_evidence evidence;
    struct candidate_staged_write_gate candidate =
        power_cycle_drill_make_candidate();
    struct activation_intent_io io;
    struct activation_intent_entry known_good =
        power_cycle_drill_make_known_good();
    struct activation_intent_entry target =
        power_cycle_drill_make_target();

    power_cycle_drill_zero(&io, sizeof(io));
    io.write = power_cycle_drill_fixture_write;
    power_cycle_drill_zero(&intent, sizeof(intent));
    power_cycle_drill_zero(&selected, sizeof(selected));
    power_cycle_drill_zero(&pending, sizeof(pending));
    power_cycle_drill_require(
        activation_intent_init(
            &intent, &known_good) == ACTIVATION_INTENT_OK &&
        activation_intent_record(
            &intent, &candidate, &io) == ACTIVATION_INTENT_OK &&
        activation_intent_pending(
            &intent, &pending) == ACTIVATION_INTENT_OK &&
        pending.pending &&
        power_cycle_drill_entries_equal(
            &pending.target, &target) &&
        power_cycle_drill_entries_equal(
            &pending.rollback, &known_good),
        "pending slot choice");
    power_cycle_drill_require(
        activation_intent_resolve(
            &intent, &selected) == ACTIVATION_INTENT_OK &&
        power_cycle_drill_entries_equal(
            &selected, &target),
        "resolved target slot");

    power_cycle_drill_init_first_boot(&health);
    power_cycle_drill_report_failure(&health);
    power_cycle_drill_require(
        first_boot_health_evaluate(
            &health, &verdict) == FIRST_BOOT_HEALTH_FAILED &&
        verdict.failed,
        "failed first-boot health");
    power_cycle_drill_zero(&rollback, sizeof(rollback));
    power_cycle_drill_zero(&evidence, sizeof(evidence));
    power_cycle_drill_require(
        automatic_rollback_init(
            &rollback) == AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_run(
            &rollback, &intent, &health) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_evidence(
            &rollback, &evidence) == AUTOMATIC_ROLLBACK_OK &&
        evidence.reason ==
            AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
        power_cycle_drill_entries_equal(
            &evidence.selected_slot, &known_good) &&
        power_cycle_drill_entries_equal(
            &evidence.candidate_slot, &target) &&
        evidence.candidate_active == false &&
        evidence.health_verdict.failed,
        "recovery reason");
    power_cycle_drill_require(
        automatic_rollback_reason_string(
            evidence.reason) != NULL &&
        power_cycle_drill_string_equal(
            automatic_rollback_reason_string(evidence.reason),
            "AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED"),
        "recovery reason string");
}

static void power_cycle_drill_prove_service_state(void)
{
    struct service_supervisor_snapshot snapshot;
    struct service_supervisor_snapshot_entry entry;
    struct service_health_input input;
    struct service_health_verdict verdict;

    power_cycle_drill_zero(&snapshot, sizeof(snapshot));
    power_cycle_drill_zero(&entry, sizeof(entry));
    power_cycle_drill_zero(&input, sizeof(input));
    power_cycle_drill_zero(&verdict, sizeof(verdict));
    entry.descriptor.id = 1U;
    entry.descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    entry.descriptor.enabled = true;
    entry.descriptor.restart_limit = 0U;
    entry.descriptor.readiness_timeout_ms = 1000U;
    entry.descriptor.cpu_ticks_limit = 0U;
    entry.descriptor.user_memory_limit_pages = 0U;
    power_cycle_drill_copy_name(
        entry.descriptor.name, "power-cycle");
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

    power_cycle_drill_require(
        service_descriptor_validate(
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
        "service state verdict");
    power_cycle_drill_require(
        service_health_fault_string(
            verdict.fault) != NULL &&
        power_cycle_drill_string_equal(
            service_health_fault_string(verdict.fault),
            "SERVICE_HEALTH_FAULT_NONE"),
        "service fault string");
}

void power_cycle_continuity_drill_qemu_test_init(void)
{
    serial_write("[POWER-CYCLE-DRILL] proof init\n");
    power_cycle_drill_prove_audit();
    serial_write("[POWER-CYCLE-DRILL] audit reboot reason coherent\n");
    power_cycle_drill_prove_state_generation();
    serial_write("[POWER-CYCLE-DRILL] state generation coherent\n");
    power_cycle_drill_prove_health();
    serial_write("[POWER-CYCLE-DRILL] health coherent\n");
    power_cycle_drill_prove_slot_and_recovery();
    serial_write("[POWER-CYCLE-DRILL] slot choice coherent\n");
    serial_write("[POWER-CYCLE-DRILL] recovery reason coherent\n");
    power_cycle_drill_prove_service_state();
    serial_write("[POWER-CYCLE-DRILL] service state coherent\n");
    serial_write("[POWER-CYCLE-DRILL] clean reboot\n");
    serial_write("[POWER-CYCLE-DRILL] proof OK\n");
}

#else
typedef int power_cycle_continuity_drill_qemu_translation_unit_guard;
#endif
