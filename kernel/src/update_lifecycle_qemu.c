#ifdef HFOC_UPDATE_LIFECYCLE_QEMU_TEST

#include "activation_intent.h"
#include "automatic_rollback.h"
#include "candidate_preflight.h"
#include "candidate_slot_inventory.h"
#include "candidate_staged_write.h"
#include "config_kv.h"
#include "first_boot_health.h"
#include "serial.h"
#include "sha256.h"
#include "update_acceptance.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_LIFECYCLE_BUNDLE_SIZE 4096U
#define UPDATE_LIFECYCLE_KNOWN_GOOD_SLOT 1U
#define UPDATE_LIFECYCLE_COMPATIBLE_SLOT 2U
#define UPDATE_LIFECYCLE_ROLLBACK_SLOT 3U
#define UPDATE_LIFECYCLE_KV_CAPACITY 4U
#define UPDATE_LIFECYCLE_ACCEPTANCE_KEY "acceptance_v1"
#define UPDATE_LIFECYCLE_ACCEPTANCE_KEY_LEN 13U

static uint8_t update_lifecycle_bundle_a[UPDATE_LIFECYCLE_BUNDLE_SIZE];
static uint8_t update_lifecycle_bundle_b[UPDATE_LIFECYCLE_BUNDLE_SIZE];

struct update_lifecycle_storage_fixture {
    uint8_t storage[CANDIDATE_SLOT_INVENTORY_SLOT_MAX]
                   [UPDATE_LIFECYCLE_BUNDLE_SIZE];
    size_t stored[CANDIDATE_SLOT_INVENTORY_SLOT_MAX];
};

struct update_lifecycle_acceptance_fixture {
    struct update_acceptance_evidence phase0;
    struct update_acceptance_evidence phase1;
    unsigned int phase0_calls;
    unsigned int phase1_calls;
    bool fail_phase0;
    bool fail_phase1;
};

static void update_lifecycle_qemu_fail(const char *message)
    __attribute__((noreturn));

static void update_lifecycle_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[UPDATE-LIFECYCLE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void update_lifecycle_qemu_require(bool condition,
                                          const char *message)
{
    if (!condition) {
        update_lifecycle_qemu_fail(message);
    }
}

static void update_lifecycle_qemu_zero(void *destination, size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void update_lifecycle_qemu_copy(void *destination,
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

static bool update_lifecycle_qemu_equal(const void *left,
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

static void update_lifecycle_qemu_fill_bundle(uint8_t *bundle,
                                              uint8_t seed)
{
    size_t index;

    for (index = 0U; index < UPDATE_LIFECYCLE_BUNDLE_SIZE; ++index) {
        bundle[index] = (uint8_t)(seed + (index * 31U) +
                                  (index >> 4U));
    }
}

static bool update_lifecycle_qemu_entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           update_lifecycle_qemu_equal(
               left->digest, right->digest,
               sizeof(left->digest));
}

static void update_lifecycle_qemu_make_entry(
    struct activation_intent_entry *entry,
    uint32_t slot_id,
    uint32_t generation,
    const uint8_t *digest)
{
    update_lifecycle_qemu_zero(entry, sizeof(*entry));
    entry->slot_id = slot_id;
    entry->generation = generation;
    update_lifecycle_qemu_copy(
        entry->digest, digest, sizeof(entry->digest));
}

static void update_lifecycle_qemu_make_candidate_entry(
    struct candidate_slot_inventory_entry *entry,
    uint32_t slot_id,
    uint32_t generation,
    const uint8_t *digest)
{
    update_lifecycle_qemu_zero(entry, sizeof(*entry));
    entry->slot_id = slot_id;
    entry->generation = generation;
    update_lifecycle_qemu_copy(
        entry->digest, digest, sizeof(entry->digest));
}

static void update_lifecycle_qemu_fill_digest(uint8_t *digest,
                                              uint8_t seed)
{
    size_t index;

    for (index = 0U; index < ACTIVATION_INTENT_DIGEST_SIZE; ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static struct update_compatibility_manifest
update_lifecycle_qemu_make_manifest(uint32_t source_image_id,
                                    uint32_t target_image_id,
                                    uint32_t source_policy_generation,
                                    uint32_t target_policy_generation)
{
    struct update_compatibility_manifest manifest;

    update_lifecycle_qemu_zero(&manifest, sizeof(manifest));
    manifest.compatibility_version = UPDATE_COMPATIBILITY_VERSION;
    manifest.source_image_id = source_image_id;
    manifest.target_image_id = target_image_id;
    manifest.source_schema_min = 1U;
    manifest.source_schema_max = 1U;
    manifest.target_schema_min = 1U;
    manifest.target_schema_max = 2U;
    manifest.source_kernel_abi_min = 1U;
    manifest.source_kernel_abi_max = 3U;
    manifest.target_kernel_abi_min = 1U;
    manifest.target_kernel_abi_max = 3U;
    manifest.source_service_abi_min = 1U;
    manifest.source_service_abi_max = 1U;
    manifest.target_service_abi_min = 1U;
    manifest.target_service_abi_max = 1U;
    manifest.source_policy_generation_min = source_policy_generation;
    manifest.source_policy_generation_max = source_policy_generation;
    manifest.target_policy_generation_min = target_policy_generation;
    manifest.target_policy_generation_max = target_policy_generation;
    return manifest;
}

static struct update_compatibility_source
update_lifecycle_qemu_make_source(uint32_t image_id,
                                  uint32_t policy_generation)
{
    struct update_compatibility_source source;

    update_lifecycle_qemu_zero(&source, sizeof(source));
    source.image_id = image_id;
    source.schema_version = 1U;
    source.kernel_abi = 2U;
    source.service_abi = 1U;
    source.policy_generation = policy_generation;
    return source;
}

static struct update_compatibility_target
update_lifecycle_qemu_make_target(uint32_t image_id,
                                  uint32_t policy_generation)
{
    struct update_compatibility_target target;

    update_lifecycle_qemu_zero(&target, sizeof(target));
    target.image_id = image_id;
    target.schema_version = 1U;
    target.kernel_abi = 2U;
    target.service_abi = 1U;
    target.policy_generation = policy_generation;
    return target;
}

static struct candidate_preflight_request
update_lifecycle_qemu_make_request(
    uint32_t slot_id,
    uint32_t generation,
    const uint8_t *digest,
    const struct update_compatibility_manifest *manifest,
    const struct update_compatibility_source *source,
    const struct update_compatibility_target *target)
{
    struct candidate_preflight_request request;

    update_lifecycle_qemu_zero(&request, sizeof(request));
    request.version = CANDIDATE_PREFLIGHT_VERSION;
    request.slot_id = slot_id;
    request.generation = generation;
    request.trust_status = SIGNED_METADATA_OK;
    request.bundle.size = UPDATE_LIFECYCLE_BUNDLE_SIZE;
    update_lifecycle_qemu_copy(
        request.bundle.digest, digest, sizeof(request.bundle.digest));
    request.compatibility = *manifest;
    request.source = *source;
    request.target = *target;
    return request;
}

static bool update_lifecycle_qemu_fixture_write(
    void *context,
    uint32_t slot_id,
    const void *data,
    size_t size)
{
    struct update_lifecycle_storage_fixture *fixture = context;

    if (fixture == NULL || data == NULL ||
        slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        size == 0U || size > UPDATE_LIFECYCLE_BUNDLE_SIZE) {
        return false;
    }
    update_lifecycle_qemu_copy(
        fixture->storage[slot_id - 1U], data, size);
    fixture->stored[slot_id - 1U] = size;
    return true;
}

static bool update_lifecycle_qemu_fixture_readback(
    void *context,
    uint32_t slot_id,
    size_t offset,
    void *data,
    size_t capacity,
    size_t *bytes_read)
{
    struct update_lifecycle_storage_fixture *fixture = context;
    size_t remaining;
    size_t take;

    if (fixture == NULL || data == NULL || bytes_read == NULL) {
        return false;
    }
    *bytes_read = 0U;
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        capacity == 0U || offset > fixture->stored[slot_id - 1U]) {
        return false;
    }
    remaining = fixture->stored[slot_id - 1U] - offset;
    if (remaining == 0U) {
        return true;
    }
    take = capacity < remaining ? capacity : remaining;
    update_lifecycle_qemu_copy(
        data, fixture->storage[slot_id - 1U] + offset, take);
    *bytes_read = take;
    return true;
}

static bool update_lifecycle_qemu_acceptance_write(
    void *context,
    uint8_t phase,
    const struct update_acceptance_evidence *evidence)
{
    struct update_lifecycle_acceptance_fixture *fixture = context;

    if (fixture == NULL || evidence == NULL) {
        return false;
    }
    if (phase == 0U) {
        ++fixture->phase0_calls;
        fixture->phase0 = *evidence;
        return !fixture->fail_phase0;
    }
    if (phase == 1U) {
        ++fixture->phase1_calls;
        fixture->phase1 = *evidence;
        return !fixture->fail_phase1;
    }
    return false;
}

static bool update_lifecycle_qemu_make_health(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements;
    static const struct first_boot_health_milestone milestones[] = {
        {FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT, 1U},
    };
    size_t index;

    update_lifecycle_qemu_zero(&requirements, sizeof(requirements));
    requirements.version = FIRST_BOOT_HEALTH_VERSION;
    requirements.milestone_count =
        sizeof(milestones) / sizeof(milestones[0]);
    for (index = 0U;
         index < requirements.milestone_count;
         ++index) {
        requirements.milestones[index] = milestones[index];
    }
    update_lifecycle_qemu_zero(store, sizeof(*store));
    return first_boot_health_init(store) == FIRST_BOOT_HEALTH_OK &&
           first_boot_health_define(store, &requirements) ==
               FIRST_BOOT_HEALTH_OK;
}

static enum first_boot_health_status update_lifecycle_qemu_report(
    struct first_boot_health_store *store,
    enum first_boot_health_subsystem subsystem,
    bool healthy)
{
    struct first_boot_health_report report;

    update_lifecycle_qemu_zero(&report, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = subsystem;
    report.milestone.milestone_id = 1U;
    report.healthy = healthy;
    return first_boot_health_report(store, &report);
}

void update_lifecycle_qemu_test_init(void)
{
    static uint8_t digest_a[SHA256_DIGEST_SIZE];
    static uint8_t digest_b[SHA256_DIGEST_SIZE];
    static uint8_t known_good_digest[ACTIVATION_INTENT_DIGEST_SIZE];
    static struct candidate_slot_inventory inventory;
    static struct candidate_slot_inventory_entry inventory_entries[3];
    static struct activation_intent_entry known_good;
    static struct update_compatibility_manifest manifest_a;
    static struct update_compatibility_source source_a;
    static struct update_compatibility_target target_a;
    static struct update_compatibility_manifest manifest_incompatible;
    static struct update_compatibility_manifest manifest_b;
    static struct update_compatibility_source source_b;
    static struct update_compatibility_target target_b;
    static struct candidate_preflight_request request_a;
    static struct candidate_preflight_request request_incompatible;
    static struct candidate_preflight_request request_b;
    static struct candidate_preflight_gate preflight_a;
    static struct candidate_preflight_gate preflight_b;
    static enum candidate_preflight_status preflight_b_status;
    static struct candidate_staged_write_gate staged_a;
    static struct candidate_staged_write_gate staged_b;
    static struct update_lifecycle_storage_fixture storage;
    static struct candidate_staged_write_io staged_io;
    static struct activation_intent_store intent;
    static struct activation_intent_io intent_io;
    static struct first_boot_health_store healthy_health;
    static struct first_boot_health_store failed_health;
    static struct update_acceptance_store acceptance;
    static struct update_lifecycle_acceptance_fixture acceptance_fixture;
    static struct update_acceptance_io acceptance_io;
    static struct update_acceptance_evidence evidence;
    static struct update_acceptance_evidence loaded_evidence;
    static struct config_kv_entry audit_entries[UPDATE_LIFECYCLE_KV_CAPACITY];
    static struct config_kv audit_kv;
    static unsigned char loaded_bytes[sizeof(loaded_evidence)];
    static size_t loaded_value_length = 0U;
    static uint64_t loaded_revision = 0U;
    static bool replaced = false;
    static struct automatic_rollback_store rollback;
    static struct automatic_rollback_evidence rollback_evidence;
    static struct activation_intent_entry selected;
    static struct first_boot_health_verdict verdict;
    static size_t index;

    update_lifecycle_qemu_fill_bundle(
        update_lifecycle_bundle_a, 0x40U);
    update_lifecycle_qemu_fill_bundle(
        update_lifecycle_bundle_b, 0x60U);
    update_lifecycle_qemu_require(
        sha256_compute(
            update_lifecycle_bundle_a,
            UPDATE_LIFECYCLE_BUNDLE_SIZE,
            digest_a) == SHA256_OK &&
        sha256_compute(
            update_lifecycle_bundle_b,
            UPDATE_LIFECYCLE_BUNDLE_SIZE,
            digest_b) == SHA256_OK,
        "bundle digest computation");
    update_lifecycle_qemu_fill_digest(known_good_digest, 0x10U);
    update_lifecycle_qemu_make_entry(
        &known_good, UPDATE_LIFECYCLE_KNOWN_GOOD_SLOT,
        1U, known_good_digest);

    update_lifecycle_qemu_make_candidate_entry(
        &inventory_entries[0], UPDATE_LIFECYCLE_KNOWN_GOOD_SLOT,
        1U, known_good_digest);
    update_lifecycle_qemu_make_candidate_entry(
        &inventory_entries[1], UPDATE_LIFECYCLE_COMPATIBLE_SLOT,
        1U, digest_a);
    update_lifecycle_qemu_make_candidate_entry(
        &inventory_entries[2], UPDATE_LIFECYCLE_ROLLBACK_SLOT,
        1U, digest_b);
    update_lifecycle_qemu_require(
        candidate_slot_inventory_init(&inventory) ==
            CANDIDATE_SLOT_INVENTORY_OK &&
        candidate_slot_inventory_publish(
            &inventory, inventory_entries, 3U, 1U) ==
            CANDIDATE_SLOT_INVENTORY_OK,
        "candidate slot inventory publication");
    serial_write("[UPDATE-LIFECYCLE] proof init\n");

    manifest_a = update_lifecycle_qemu_make_manifest(
        7U, 8U, 1U, 2U);
    source_a = update_lifecycle_qemu_make_source(7U, 1U);
    target_a = update_lifecycle_qemu_make_target(8U, 2U);
    request_a = update_lifecycle_qemu_make_request(
        UPDATE_LIFECYCLE_COMPATIBLE_SLOT, 1U, digest_a,
        &manifest_a, &source_a, &target_a);
    manifest_incompatible = update_lifecycle_qemu_make_manifest(
        7U, 99U, 1U, 2U);
    request_incompatible = update_lifecycle_qemu_make_request(
        UPDATE_LIFECYCLE_COMPATIBLE_SLOT, 1U, digest_a,
        &manifest_incompatible, &source_a, &target_a);

    update_lifecycle_qemu_zero(&preflight_a, sizeof(preflight_a));
    update_lifecycle_qemu_require(
        candidate_preflight_init(&preflight_a) ==
            CANDIDATE_PREFLIGHT_OK &&
        candidate_preflight_run(
            &preflight_a, &request_incompatible, &inventory) ==
            CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED &&
        !preflight_a.preflighted,
        "incompatible candidate preflight rejection");
    serial_write("[UPDATE-LIFECYCLE] incompatible update rejected\n");

    update_lifecycle_qemu_require(
        candidate_preflight_run(
            &preflight_a, &request_a, &inventory) ==
            CANDIDATE_PREFLIGHT_OK &&
        preflight_a.preflighted,
        "compatible candidate preflight");
    update_lifecycle_qemu_zero(&storage, sizeof(storage));
    update_lifecycle_qemu_zero(&staged_io, sizeof(staged_io));
    staged_io.context = &storage;
    staged_io.write = update_lifecycle_qemu_fixture_write;
    staged_io.readback = update_lifecycle_qemu_fixture_readback;
    update_lifecycle_qemu_zero(&staged_a, sizeof(staged_a));
    update_lifecycle_qemu_require(
        candidate_staged_write_init(&staged_a) ==
            CANDIDATE_STAGED_WRITE_OK &&
        candidate_staged_write_run(
            &staged_a, &preflight_a, &inventory,
            update_lifecycle_bundle_a, &staged_io) ==
            CANDIDATE_STAGED_WRITE_OK &&
        staged_a.ready &&
        staged_a.slot_id == UPDATE_LIFECYCLE_COMPATIBLE_SLOT,
        "compatible candidate staged write verification");
    serial_write("[UPDATE-LIFECYCLE] compatible update staged\n");

    update_lifecycle_qemu_zero(&intent_io, sizeof(intent_io));
    update_lifecycle_qemu_zero(&intent, sizeof(intent));
    update_lifecycle_qemu_require(
        activation_intent_init(&intent, &known_good) ==
            ACTIVATION_INTENT_OK &&
        activation_intent_record(
            &intent, &staged_a, &intent_io) ==
            ACTIVATION_INTENT_OK &&
        intent.pending.pending &&
        intent.pending.target.slot_id ==
            UPDATE_LIFECYCLE_COMPATIBLE_SLOT &&
        update_lifecycle_qemu_entries_equal(
            &intent.pending.rollback, &known_good),
        "activation intent publication");
    serial_write("[UPDATE-LIFECYCLE] activation intent recorded\n");

    update_lifecycle_qemu_require(
        update_lifecycle_qemu_make_health(&healthy_health),
        "healthy first-boot requirements");
    for (index = 0U; index < 4U; ++index) {
        enum first_boot_health_subsystem subsystem =
            (enum first_boot_health_subsystem)(index + 1U);

        update_lifecycle_qemu_require(
            update_lifecycle_qemu_report(
                &healthy_health, subsystem, true) ==
                FIRST_BOOT_HEALTH_OK,
            "healthy first-boot milestone report");
    }
    update_lifecycle_qemu_require(
        first_boot_health_evaluate(
            &healthy_health, &verdict) == FIRST_BOOT_HEALTH_OK &&
        verdict.ready &&
        verdict.missing_count == 0U,
        "healthy first-boot verdict");
    update_lifecycle_qemu_zero(
        &acceptance_fixture, sizeof(acceptance_fixture));
    update_lifecycle_qemu_zero(&acceptance_io, sizeof(acceptance_io));
    acceptance_io.context = &acceptance_fixture;
    acceptance_io.write = update_lifecycle_qemu_acceptance_write;
    update_lifecycle_qemu_zero(&acceptance, sizeof(acceptance));
    update_lifecycle_qemu_require(
        update_acceptance_init(&acceptance) ==
            UPDATE_ACCEPTANCE_OK &&
        update_acceptance_commit(
            &acceptance, &intent, &healthy_health,
            &acceptance_io) == UPDATE_ACCEPTANCE_OK &&
        acceptance.accepted &&
        acceptance.evidence.candidate_active &&
        update_lifecycle_qemu_entries_equal(
            &intent.known_good,
            &acceptance.evidence.accepted_slot) &&
        intent.known_good.slot_id ==
            UPDATE_LIFECYCLE_COMPATIBLE_SLOT &&
        acceptance_fixture.phase0_calls == 1U &&
        acceptance_fixture.phase1_calls == 1U,
        "healthy update acceptance commit");
    serial_write(
        "[UPDATE-LIFECYCLE] healthy acceptance committed\n");

    update_lifecycle_qemu_require(
        update_acceptance_evidence(
            &acceptance, &evidence) == UPDATE_ACCEPTANCE_OK,
        "update acceptance evidence");
    update_lifecycle_qemu_require(
        config_kv_init(
            &audit_kv, audit_entries,
            UPDATE_LIFECYCLE_KV_CAPACITY) == CONFIG_KV_OK &&
        config_kv_put(
            &audit_kv, CONFIG_KV_KIND_AUDIT,
            UPDATE_LIFECYCLE_ACCEPTANCE_KEY,
            UPDATE_LIFECYCLE_ACCEPTANCE_KEY_LEN,
            &evidence, sizeof(evidence), &replaced) == CONFIG_KV_OK &&
        !replaced,
        "update acceptance evidence audit put");
    update_lifecycle_qemu_zero(
        loaded_bytes, sizeof(loaded_bytes));
    update_lifecycle_qemu_require(
        config_kv_get(
            &audit_kv, CONFIG_KV_KIND_AUDIT,
            UPDATE_LIFECYCLE_ACCEPTANCE_KEY,
            UPDATE_LIFECYCLE_ACCEPTANCE_KEY_LEN,
            loaded_bytes, sizeof(loaded_bytes),
            &loaded_value_length, &loaded_revision) == CONFIG_KV_OK &&
        loaded_value_length == sizeof(evidence) &&
        loaded_revision == 1U,
        "update acceptance evidence audit get");
    update_lifecycle_qemu_copy(
        &loaded_evidence, loaded_bytes, sizeof(loaded_evidence));
    update_lifecycle_qemu_require(
        loaded_evidence.version == evidence.version &&
        update_lifecycle_qemu_entries_equal(
            &loaded_evidence.accepted_slot,
            &evidence.accepted_slot) &&
        update_lifecycle_qemu_entries_equal(
            &loaded_evidence.rollback_slot,
            &evidence.rollback_slot) &&
        loaded_evidence.health_verdict.ready ==
            evidence.health_verdict.ready &&
        loaded_evidence.health_verdict.failed ==
            evidence.health_verdict.failed &&
        loaded_evidence.health_verdict.missing_count ==
            evidence.health_verdict.missing_count &&
        loaded_evidence.candidate_active ==
            evidence.candidate_active,
        "update acceptance evidence continuity");
    serial_write(
        "[UPDATE-LIFECYCLE] audit continuity verified\n");

    manifest_b = update_lifecycle_qemu_make_manifest(
        8U, 9U, 2U, 3U);
    source_b = update_lifecycle_qemu_make_source(8U, 2U);
    target_b = update_lifecycle_qemu_make_target(9U, 3U);
    request_b = update_lifecycle_qemu_make_request(
        UPDATE_LIFECYCLE_ROLLBACK_SLOT, 1U, digest_b,
        &manifest_b, &source_b, &target_b);
    update_lifecycle_qemu_zero(&preflight_b, sizeof(preflight_b));
    update_lifecycle_qemu_require(
        candidate_preflight_init(&preflight_b) ==
            CANDIDATE_PREFLIGHT_OK,
        "failed candidate preflight init");
    preflight_b_status = candidate_preflight_run(
        &preflight_b, &request_b, &inventory);
    update_lifecycle_qemu_require(
        preflight_b_status == CANDIDATE_PREFLIGHT_OK,
        candidate_preflight_status_string(preflight_b_status));
    update_lifecycle_qemu_zero(&staged_b, sizeof(staged_b));
    update_lifecycle_qemu_require(
        candidate_staged_write_init(&staged_b) ==
            CANDIDATE_STAGED_WRITE_OK &&
        candidate_staged_write_run(
            &staged_b, &preflight_b, &inventory,
            update_lifecycle_bundle_b, &staged_io) ==
            CANDIDATE_STAGED_WRITE_OK &&
        staged_b.ready &&
        staged_b.slot_id == UPDATE_LIFECYCLE_ROLLBACK_SLOT,
        "failed candidate staged write verification");
    update_lifecycle_qemu_require(
        activation_intent_record(
            &intent, &staged_b, &intent_io) ==
            ACTIVATION_INTENT_OK &&
        intent.pending.target.slot_id ==
            UPDATE_LIFECYCLE_ROLLBACK_SLOT &&
        intent.pending.rollback.slot_id ==
            UPDATE_LIFECYCLE_COMPATIBLE_SLOT,
        "failed candidate activation intent");
    update_lifecycle_qemu_require(
        update_lifecycle_qemu_make_health(&failed_health),
        "failed first-boot requirements");
    update_lifecycle_qemu_require(
        update_lifecycle_qemu_report(
            &failed_health,
            FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
            true) == FIRST_BOOT_HEALTH_OK &&
        update_lifecycle_qemu_report(
            &failed_health,
            FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
            false) == FIRST_BOOT_HEALTH_OK &&
        first_boot_health_evaluate(
            &failed_health, &verdict) ==
            FIRST_BOOT_HEALTH_FAILED &&
        verdict.failed,
        "failed first-boot verdict");
    update_lifecycle_qemu_zero(&rollback, sizeof(rollback));
    update_lifecycle_qemu_require(
        automatic_rollback_init(&rollback) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_run(
            &rollback, &intent, &failed_health) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_evidence(
            &rollback, &rollback_evidence) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_selected(
            &rollback, &selected) == AUTOMATIC_ROLLBACK_OK &&
        rollback_evidence.reason ==
            AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
        selected.slot_id == UPDATE_LIFECYCLE_COMPATIBLE_SLOT &&
        rollback_evidence.candidate_slot.slot_id ==
            UPDATE_LIFECYCLE_ROLLBACK_SLOT &&
        !rollback_evidence.candidate_active,
        "failed candidate automatic rollback selection");
    serial_write(
        "[UPDATE-LIFECYCLE] failed candidate rollback\n");

    serial_write("[UPDATE-LIFECYCLE] clean reboot\n");
    serial_write("[UPDATE-LIFECYCLE] proof OK\n");
}

#else
typedef int update_lifecycle_qemu_translation_unit_guard;
#endif
