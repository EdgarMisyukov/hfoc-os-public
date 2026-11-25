#ifdef HFOC_UPDATE_FAILURE_DRILL_QEMU_TEST

#include "activation_intent.h"
#include "automatic_rollback.h"
#include "candidate_preflight.h"
#include "candidate_slot_inventory.h"
#include "candidate_staged_write.h"
#include "fault_injection.h"
#include "first_boot_health.h"
#include "serial.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UPDATE_FAILURE_BUNDLE_SIZE 4096U
#define UPDATE_FAILURE_KNOWN_GOOD_SLOT 1U
#define UPDATE_FAILURE_TARGET_SLOT 2U

static uint8_t update_failure_bundle[UPDATE_FAILURE_BUNDLE_SIZE];

static void update_failure_drill_fail(const char *message)
    __attribute__((noreturn));

static void update_failure_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[UPDATE-FAILURE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void update_failure_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        update_failure_drill_fail(message);
    }
}

static void update_failure_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void update_failure_drill_copy(
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

static bool update_failure_drill_equal(
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

static void update_failure_drill_fill_bundle(void)
{
    size_t index;

    for (index = 0U;
         index < UPDATE_FAILURE_BUNDLE_SIZE;
         ++index) {
        update_failure_bundle[index] =
            (uint8_t)(0x40U + (index * 31U) + (index >> 4U));
    }
}

static void update_failure_drill_fill_digest(
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
update_failure_drill_make_known_good(void)
{
    struct activation_intent_entry entry;

    update_failure_drill_zero(&entry, sizeof(entry));
    entry.slot_id = UPDATE_FAILURE_KNOWN_GOOD_SLOT;
    entry.generation = 1U;
    update_failure_drill_fill_digest(entry.digest, 0x10U);
    return entry;
}

static struct activation_intent_entry
update_failure_drill_make_target(const uint8_t *target_digest)
{
    struct activation_intent_entry entry;

    update_failure_drill_zero(&entry, sizeof(entry));
    entry.slot_id = UPDATE_FAILURE_TARGET_SLOT;
    entry.generation = 1U;
    update_failure_drill_copy(
        entry.digest, target_digest, sizeof(entry.digest));
    return entry;
}

static bool update_failure_drill_entries_equal(
    const struct activation_intent_entry *left,
    const struct activation_intent_entry *right)
{
    return left->slot_id == right->slot_id &&
           left->generation == right->generation &&
           update_failure_drill_equal(
               left->digest, right->digest,
               sizeof(left->digest));
}

static struct update_compatibility_manifest
update_failure_drill_make_manifest(void)
{
    struct update_compatibility_manifest manifest;

    update_failure_drill_zero(&manifest, sizeof(manifest));
    manifest.compatibility_version = UPDATE_COMPATIBILITY_VERSION;
    manifest.source_image_id = 7U;
    manifest.target_image_id = 8U;
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
    manifest.source_policy_generation_min = 1U;
    manifest.source_policy_generation_max = 1U;
    manifest.target_policy_generation_min = 2U;
    manifest.target_policy_generation_max = 2U;
    return manifest;
}

static struct update_compatibility_source
update_failure_drill_make_source(void)
{
    struct update_compatibility_source source;

    update_failure_drill_zero(&source, sizeof(source));
    source.image_id = 7U;
    source.schema_version = 1U;
    source.kernel_abi = 2U;
    source.service_abi = 1U;
    source.policy_generation = 1U;
    return source;
}

static struct update_compatibility_target
update_failure_drill_make_compat_target(void)
{
    struct update_compatibility_target target;

    update_failure_drill_zero(&target, sizeof(target));
    target.image_id = 8U;
    target.schema_version = 1U;
    target.kernel_abi = 2U;
    target.service_abi = 1U;
    target.policy_generation = 2U;
    return target;
}

static struct candidate_preflight_request
update_failure_drill_make_request(
    const uint8_t *target_digest)
{
    struct candidate_preflight_request request;

    update_failure_drill_zero(&request, sizeof(request));
    request.version = CANDIDATE_PREFLIGHT_VERSION;
    request.slot_id = UPDATE_FAILURE_TARGET_SLOT;
    request.generation = 1U;
    request.trust_status = SIGNED_METADATA_OK;
    request.bundle.size = UPDATE_FAILURE_BUNDLE_SIZE;
    update_failure_drill_copy(
        request.bundle.digest, target_digest,
        sizeof(request.bundle.digest));
    request.compatibility =
        update_failure_drill_make_manifest();
    request.source = update_failure_drill_make_source();
    request.target = update_failure_drill_make_compat_target();
    return request;
}

static bool update_failure_drill_make_inventory(
    struct candidate_slot_inventory *inventory,
    const uint8_t *target_digest)
{
    struct candidate_slot_inventory_entry entries[2];
    struct activation_intent_entry known_good =
        update_failure_drill_make_known_good();

    update_failure_drill_zero(entries, sizeof(entries));
    entries[0].slot_id = known_good.slot_id;
    entries[0].generation = known_good.generation;
    update_failure_drill_copy(
        entries[0].digest, known_good.digest,
        sizeof(entries[0].digest));
    entries[1].slot_id = UPDATE_FAILURE_TARGET_SLOT;
    entries[1].generation = 1U;
    update_failure_drill_copy(
        entries[1].digest, target_digest,
        sizeof(entries[1].digest));
    update_failure_drill_zero(inventory, sizeof(*inventory));
    return candidate_slot_inventory_init(inventory) ==
               CANDIDATE_SLOT_INVENTORY_OK &&
           candidate_slot_inventory_publish(
               inventory, entries, 2U, 1U) ==
               CANDIDATE_SLOT_INVENTORY_OK;
}

static bool update_failure_drill_make_preflight(
    struct candidate_preflight_gate *gate,
    const struct candidate_slot_inventory *inventory,
    const uint8_t *target_digest)
{
    struct candidate_preflight_request request =
        update_failure_drill_make_request(target_digest);

    update_failure_drill_zero(gate, sizeof(*gate));
    return candidate_preflight_init(gate) == CANDIDATE_PREFLIGHT_OK &&
           candidate_preflight_run(
               gate, &request, inventory) == CANDIDATE_PREFLIGHT_OK;
}

struct update_failure_storage {
    uint8_t storage[CANDIDATE_SLOT_INVENTORY_SLOT_MAX]
                   [UPDATE_FAILURE_BUNDLE_SIZE];
    size_t stored[CANDIDATE_SLOT_INVENTORY_SLOT_MAX];
    bool fail_write;
    bool fail_readback;
    bool short_readback;
    bool corrupt_readback;
    unsigned int write_calls;
    unsigned int readback_calls;
    uint32_t last_write_slot;
};

static void update_failure_drill_reset_storage(
    struct update_failure_storage *storage)
{
    update_failure_drill_zero(storage, sizeof(*storage));
    update_failure_drill_zero(
        storage->storage, sizeof(storage->storage));
}

static bool update_failure_drill_fixture_write(
    void *context,
    uint32_t slot_id,
    const void *data,
    size_t size)
{
    struct update_failure_storage *storage = context;

    ++storage->write_calls;
    storage->last_write_slot = slot_id;
    if (storage->fail_write) {
        return false;
    }
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        data == NULL || size == 0U ||
        size > UPDATE_FAILURE_BUNDLE_SIZE) {
        return false;
    }
    update_failure_drill_copy(
        storage->storage[slot_id - 1U], data, size);
    storage->stored[slot_id - 1U] = size;
    return true;
}

static bool update_failure_drill_fixture_readback(
    void *context,
    uint32_t slot_id,
    size_t offset,
    void *data,
    size_t capacity,
    size_t *bytes_read)
{
    struct update_failure_storage *storage = context;
    size_t remaining;
    size_t take;

    ++storage->readback_calls;
    if (bytes_read == NULL) {
        return false;
    }
    *bytes_read = 0U;
    if (storage->fail_readback) {
        return false;
    }
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        data == NULL || capacity == 0U ||
        offset > storage->stored[slot_id - 1U]) {
        return false;
    }
    remaining = storage->stored[slot_id - 1U] - offset;
    if (remaining == 0U) {
        return true;
    }
    if (storage->short_readback && remaining <= capacity) {
        return true;
    }
    take = capacity < remaining ? capacity : remaining;
    update_failure_drill_copy(
        data, storage->storage[slot_id - 1U] + offset, take);
    if (storage->corrupt_readback &&
        storage->readback_calls == 1U) {
        ((uint8_t *)data)[0] ^= 0x01U;
    }
    *bytes_read = take;
    return true;
}

static struct candidate_staged_write_io
update_failure_drill_make_staged_io(
    struct update_failure_storage *storage)
{
    struct candidate_staged_write_io io;

    update_failure_drill_zero(&io, sizeof(io));
    io.context = storage;
    io.write = update_failure_drill_fixture_write;
    io.readback = update_failure_drill_fixture_readback;
    return io;
}

static bool update_failure_drill_init_intent(
    struct activation_intent_store *store)
{
    struct activation_intent_entry known_good =
        update_failure_drill_make_known_good();

    update_failure_drill_zero(store, sizeof(*store));
    return activation_intent_init(
        store, &known_good) == ACTIVATION_INTENT_OK;
}

static struct first_boot_health_requirements
update_failure_drill_make_requirements(void)
{
    struct first_boot_health_requirements requirements;
    static const struct first_boot_health_milestone milestones[] = {
        {FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_SERVICE, 1U},
        {FIRST_BOOT_HEALTH_SUBSYSTEM_MANAGEMENT, 1U},
    };
    size_t index;

    update_failure_drill_zero(&requirements, sizeof(requirements));
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

static enum first_boot_health_status
update_failure_drill_report(
    struct first_boot_health_store *store,
    enum first_boot_health_subsystem subsystem,
    bool healthy)
{
    struct first_boot_health_report report;

    update_failure_drill_zero(&report, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = subsystem;
    report.milestone.milestone_id = 1U;
    report.healthy = healthy;
    return first_boot_health_report(store, &report);
}

static bool update_failure_drill_make_failed_health(
    struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        update_failure_drill_make_requirements();
    struct first_boot_health_verdict verdict;

    update_failure_drill_zero(store, sizeof(*store));
    return first_boot_health_init(store) == FIRST_BOOT_HEALTH_OK &&
           first_boot_health_define(store, &requirements) ==
               FIRST_BOOT_HEALTH_OK &&
           update_failure_drill_report(
               store,
               FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
               true) == FIRST_BOOT_HEALTH_OK &&
           update_failure_drill_report(
               store,
               FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
               false) == FIRST_BOOT_HEALTH_OK &&
           first_boot_health_evaluate(store, &verdict) ==
               FIRST_BOOT_HEALTH_FAILED &&
           verdict.failed;
}

static bool update_failure_drill_rollback_evidence_equal(
    const struct automatic_rollback_evidence *left,
    const struct automatic_rollback_evidence *right)
{
    return left->version == right->version &&
           left->reason == right->reason &&
           update_failure_drill_entries_equal(
               &left->selected_slot, &right->selected_slot) &&
           update_failure_drill_entries_equal(
               &left->candidate_slot, &right->candidate_slot) &&
           left->health_verdict.ready ==
               right->health_verdict.ready &&
           left->health_verdict.failed ==
               right->health_verdict.failed &&
           left->health_verdict.required_count ==
               right->health_verdict.required_count &&
           left->health_verdict.observed_count ==
               right->health_verdict.observed_count &&
           left->health_verdict.missing_count ==
               right->health_verdict.missing_count &&
           left->health_verdict.first_missing.subsystem ==
               right->health_verdict.first_missing.subsystem &&
           left->health_verdict.first_missing.milestone_id ==
               right->health_verdict.first_missing.milestone_id &&
           left->candidate_active == right->candidate_active;
}

void update_failure_drill_qemu_test_init(void)
{
    /*
     * ITEM-393: the fixtures live in .bss, not on the boot stack.
     *
     * This proof runs once from kmain, on the stack the bootloader handed
     * over, and its fixtures used to be a stack frame of about 34 KiB - the
     * same shape that overflowed the boot stack and triple-faulted the
     * storage-interruption target into a silent reboot. A proof may not spend
     * the boot stack: the function stays single-shot and non-reentrant, and
     * `known_good` is assigned in the body because its value comes from a
     * function rather than a constant initializer.
     */
    static uint8_t target_digest[SHA256_DIGEST_SIZE];
    static struct candidate_slot_inventory inventory;
    static struct candidate_preflight_gate preflight;
    static struct activation_intent_store intent;
    static struct activation_intent_io intent_io;
    static struct activation_intent_entry known_good;
    static struct activation_intent_entry target;
    static struct activation_intent_entry selected_a;
    static struct activation_intent_entry selected_b;
    static struct activation_intent_record pending;
    static struct first_boot_health_store health;
    static struct first_boot_health_verdict verdict;
    static struct automatic_rollback_store rollback_a;
    static struct automatic_rollback_store rollback_b;
    static struct automatic_rollback_evidence evidence_a;
    static struct automatic_rollback_evidence evidence_b;
    static struct update_failure_storage storage;
    static struct candidate_staged_write_gate gate;
    static struct candidate_staged_write_io io;
    const struct fault_injection_site *site = NULL;

    known_good = update_failure_drill_make_known_good();
    serial_write("[UPDATE-FAILURE-DRILL] proof init\n");
    update_failure_drill_fill_bundle();
    update_failure_drill_require(
        sha256_compute(
            update_failure_bundle,
            UPDATE_FAILURE_BUNDLE_SIZE,
            target_digest) == SHA256_OK,
        "bundle digest computation");
    target = update_failure_drill_make_target(target_digest);
    update_failure_drill_require(
        update_failure_drill_make_inventory(
            &inventory, target_digest) &&
        update_failure_drill_make_preflight(
            &preflight, &inventory, target_digest) &&
        update_failure_drill_init_intent(&intent),
        "update drill fixture publication");
    update_failure_drill_zero(&intent_io, sizeof(intent_io));

    update_failure_drill_require(
        fault_injection_site_by_id(
            FAULT_INJECTION_UPDATE_STAGED_WRITE, &site) ==
            FAULT_INJECTION_OK &&
        site->subsystem == FAULT_INJECTION_SUBSYSTEM_UPDATE &&
        site->bounded && site->test_only &&
        site->expected_outcome != NULL &&
        site->expected_outcome[0] != '\0' &&
        fault_injection_site_by_id(
            FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH, &site) ==
            FAULT_INJECTION_OK &&
        site->subsystem == FAULT_INJECTION_SUBSYSTEM_UPDATE &&
        site->bounded && site->test_only &&
        site->expected_outcome != NULL &&
        site->expected_outcome[0] != '\0',
        "catalogue update fault points");
    serial_write(
        "[UPDATE-FAILURE-DRILL] catalogue update fault points exact\n");

    update_failure_drill_reset_storage(&storage);
    storage.fail_write = true;
    io = update_failure_drill_make_staged_io(&storage);
    update_failure_drill_zero(&gate, sizeof(gate));
    update_failure_drill_require(
        candidate_staged_write_init(&gate) ==
            CANDIDATE_STAGED_WRITE_OK &&
        candidate_staged_write_run(
            &gate, &preflight, &inventory,
            update_failure_bundle, &io) ==
            CANDIDATE_STAGED_WRITE_WRITE_FAULT &&
        !gate.ready && gate.slot_id == 0U,
        "staged write fault");
    update_failure_drill_require(
        activation_intent_record(&intent, &gate, &intent_io) ==
            ACTIVATION_INTENT_BAD_CANDIDATE &&
        activation_intent_pending(&intent, &pending) ==
            ACTIVATION_INTENT_NO_PENDING &&
        activation_intent_known_good(&intent, &selected_a) ==
            ACTIVATION_INTENT_OK &&
        update_failure_drill_entries_equal(
            &selected_a, &known_good),
        "staged write fault keeps known-good");

    update_failure_drill_reset_storage(&storage);
    storage.fail_readback = true;
    io = update_failure_drill_make_staged_io(&storage);
    update_failure_drill_zero(&gate, sizeof(gate));
    update_failure_drill_require(
        candidate_staged_write_init(&gate) ==
            CANDIDATE_STAGED_WRITE_OK &&
        candidate_staged_write_run(
            &gate, &preflight, &inventory,
            update_failure_bundle, &io) ==
            CANDIDATE_STAGED_WRITE_READBACK_FAULT &&
        !gate.ready && gate.slot_id == 0U &&
        activation_intent_record(&intent, &gate, &intent_io) ==
            ACTIVATION_INTENT_BAD_CANDIDATE &&
        activation_intent_pending(&intent, &pending) ==
            ACTIVATION_INTENT_NO_PENDING &&
        activation_intent_known_good(&intent, &selected_a) ==
            ACTIVATION_INTENT_OK &&
        update_failure_drill_entries_equal(
            &selected_a, &known_good),
        "staged readback fault keeps known-good");
    serial_write(
        "[UPDATE-FAILURE-DRILL] staged write interruption denied\n");
    serial_write(
        "[UPDATE-FAILURE-DRILL] known-good retained after staged fault\n");

    update_failure_drill_reset_storage(&storage);
    io = update_failure_drill_make_staged_io(&storage);
    update_failure_drill_zero(&gate, sizeof(gate));
    update_failure_drill_require(
        candidate_staged_write_init(&gate) ==
            CANDIDATE_STAGED_WRITE_OK &&
        candidate_staged_write_run(
            &gate, &preflight, &inventory,
            update_failure_bundle, &io) ==
            CANDIDATE_STAGED_WRITE_OK &&
        gate.ready && gate.slot_id == UPDATE_FAILURE_TARGET_SLOT,
        "clean staged retry");
    update_failure_drill_zero(&intent_io, sizeof(intent_io));
    update_failure_drill_require(
        activation_intent_record(
            &intent, &gate, &intent_io) == ACTIVATION_INTENT_OK &&
        intent.pending.pending &&
        update_failure_drill_entries_equal(
            &intent.pending.target, &target) &&
        update_failure_drill_entries_equal(
            &intent.pending.rollback, &known_good) &&
        activation_intent_known_good(&intent, &selected_a) ==
            ACTIVATION_INTENT_OK &&
        update_failure_drill_entries_equal(
            &selected_a, &known_good),
        "clean staged retry publishes pending intent");
    serial_write(
        "[UPDATE-FAILURE-DRILL] clean staged retry ready\n");

    update_failure_drill_require(
        update_failure_drill_make_failed_health(&health),
        "failed first-boot health");
    update_failure_drill_require(
        first_boot_health_evaluate(&health, &verdict) ==
            FIRST_BOOT_HEALTH_FAILED &&
        verdict.failed,
        "failed first-boot verdict");
    update_failure_drill_zero(
        &rollback_a, sizeof(rollback_a));
    update_failure_drill_zero(
        &rollback_b, sizeof(rollback_b));
    update_failure_drill_require(
        automatic_rollback_init(&rollback_a) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_init(&rollback_b) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_run(
            &rollback_a, &intent, &health) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_run(
            &rollback_b, &intent, &health) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_evidence(
            &rollback_a, &evidence_a) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_evidence(
            &rollback_b, &evidence_b) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_selected(
            &rollback_a, &selected_a) ==
            AUTOMATIC_ROLLBACK_OK &&
        automatic_rollback_selected(
            &rollback_b, &selected_b) ==
            AUTOMATIC_ROLLBACK_OK,
        "automatic rollback publication");
    update_failure_drill_require(
        evidence_a.reason ==
            AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
        evidence_a.candidate_active == false &&
        evidence_a.health_verdict.failed &&
        update_failure_drill_entries_equal(
            &evidence_a.selected_slot, &known_good) &&
        update_failure_drill_entries_equal(
            &evidence_a.candidate_slot, &target) &&
        update_failure_drill_entries_equal(
            &selected_a, &known_good) &&
        update_failure_drill_entries_equal(
            &selected_b, &known_good) &&
        update_failure_drill_rollback_evidence_equal(
            &evidence_a, &evidence_b),
        "deterministic known-good rollback");
    update_failure_drill_require(
        activation_intent_pending(&intent, &pending) ==
            ACTIVATION_INTENT_OK &&
        pending.pending &&
        update_failure_drill_entries_equal(
            &pending.target, &target) &&
        activation_intent_known_good(&intent, &selected_a) ==
            ACTIVATION_INTENT_OK &&
        update_failure_drill_entries_equal(
            &selected_a, &known_good),
        "rollback preserves pending and known-good");
    serial_write(
        "[UPDATE-FAILURE-DRILL] first boot health failure published\n");
    serial_write(
        "[UPDATE-FAILURE-DRILL] deterministic rollback selection\n");
    serial_write("[UPDATE-FAILURE-DRILL] proof OK\n");
}

#else
typedef int update_failure_drill_qemu_translation_unit_guard;
#endif
