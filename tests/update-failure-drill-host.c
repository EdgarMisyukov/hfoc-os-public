#include "activation_intent.h"
#include "automatic_rollback.h"
#include "candidate_preflight.h"
#include "candidate_slot_inventory.h"
#include "candidate_staged_write.h"
#include "fault_injection.h"
#include "first_boot_health.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "update_compatibility.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_UPDATE_FAILURE_DRILL_HOST_TEST

#define DRILL_BUNDLE_SIZE 4096U
#define DRILL_KNOWN_GOOD_SLOT 1U
#define DRILL_TARGET_SLOT 2U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static uint8_t drill_bundle[DRILL_BUNDLE_SIZE];

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

static void fill_bundle(void)
{
    size_t index;

    for (index = 0U; index < DRILL_BUNDLE_SIZE; ++index) {
        drill_bundle[index] =
            (uint8_t)(0x40U + (index * 31U) + (index >> 4U));
    }
}

static bool bundle_digest(uint8_t digest[SHA256_DIGEST_SIZE])
{
    return sha256_compute(
        drill_bundle, DRILL_BUNDLE_SIZE, digest) == SHA256_OK;
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

static struct activation_intent_entry make_target_entry(
    const uint8_t *target_digest)
{
    struct activation_intent_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = DRILL_TARGET_SLOT;
    entry.generation = 1U;
    memcpy(entry.digest, target_digest, sizeof(entry.digest));
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

static struct update_compatibility_manifest make_manifest(void)
{
    struct update_compatibility_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
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

static struct update_compatibility_source make_source(void)
{
    struct update_compatibility_source source;

    memset(&source, 0, sizeof(source));
    source.image_id = 7U;
    source.schema_version = 1U;
    source.kernel_abi = 2U;
    source.service_abi = 1U;
    source.policy_generation = 1U;
    return source;
}

static struct update_compatibility_target make_target(void)
{
    struct update_compatibility_target target;

    memset(&target, 0, sizeof(target));
    target.image_id = 8U;
    target.schema_version = 1U;
    target.kernel_abi = 2U;
    target.service_abi = 1U;
    target.policy_generation = 2U;
    return target;
}

static bool make_inventory(
    struct candidate_slot_inventory *inventory,
    const uint8_t *target_digest)
{
    struct candidate_slot_inventory_entry entries[2];
    struct activation_intent_entry known_good = make_known_good();

    memset(entries, 0, sizeof(entries));
    entries[0].slot_id = known_good.slot_id;
    entries[0].generation = known_good.generation;
    memcpy(entries[0].digest, known_good.digest,
           sizeof(entries[0].digest));
    entries[1].slot_id = DRILL_TARGET_SLOT;
    entries[1].generation = 1U;
    memcpy(entries[1].digest, target_digest,
           sizeof(entries[1].digest));
    memset(inventory, 0, sizeof(*inventory));
    return candidate_slot_inventory_init(inventory) ==
               CANDIDATE_SLOT_INVENTORY_OK &&
           candidate_slot_inventory_publish(
               inventory, entries, 2U, 1U) ==
               CANDIDATE_SLOT_INVENTORY_OK;
}

static struct candidate_preflight_request make_request(
    const uint8_t *target_digest)
{
    struct candidate_preflight_request request;

    memset(&request, 0, sizeof(request));
    request.version = CANDIDATE_PREFLIGHT_VERSION;
    request.slot_id = DRILL_TARGET_SLOT;
    request.generation = 1U;
    request.trust_status = SIGNED_METADATA_OK;
    request.bundle.size = DRILL_BUNDLE_SIZE;
    memcpy(request.bundle.digest, target_digest,
           sizeof(request.bundle.digest));
    request.compatibility = make_manifest();
    request.source = make_source();
    request.target = make_target();
    return request;
}

static bool make_preflight(
    struct candidate_preflight_gate *gate,
    const struct candidate_slot_inventory *inventory,
    const uint8_t *target_digest)
{
    struct candidate_preflight_request request =
        make_request(target_digest);

    memset(gate, 0, sizeof(*gate));
    return candidate_preflight_init(gate) == CANDIDATE_PREFLIGHT_OK &&
           candidate_preflight_run(
               gate, &request, inventory) == CANDIDATE_PREFLIGHT_OK;
}

struct drill_storage {
    uint8_t storage[CANDIDATE_SLOT_INVENTORY_SLOT_MAX]
                   [DRILL_BUNDLE_SIZE];
    size_t stored[CANDIDATE_SLOT_INVENTORY_SLOT_MAX];
    bool fail_write;
    bool fail_readback;
    bool short_readback;
    bool corrupt_readback;
    unsigned int write_calls;
    unsigned int readback_calls;
    uint32_t last_write_slot;
};

static void reset_storage(struct drill_storage *storage)
{
    memset(storage, 0, sizeof(*storage));
    memset(storage->storage, 0xA5, sizeof(storage->storage));
}

static bool fixture_write(
    void *context,
    uint32_t slot_id,
    const void *data,
    size_t size)
{
    struct drill_storage *storage =
        (struct drill_storage *)context;

    ++storage->write_calls;
    storage->last_write_slot = slot_id;
    if (storage->fail_write) {
        return false;
    }
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        data == NULL || size == 0U || size > DRILL_BUNDLE_SIZE) {
        return false;
    }
    memcpy(storage->storage[slot_id - 1U], data, size);
    storage->stored[slot_id - 1U] = size;
    return true;
}

static bool fixture_readback(
    void *context,
    uint32_t slot_id,
    size_t offset,
    void *data,
    size_t capacity,
    size_t *bytes_read)
{
    struct drill_storage *storage =
        (struct drill_storage *)context;
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
    memcpy(data, storage->storage[slot_id - 1U] + offset, take);
    if (storage->corrupt_readback &&
        storage->readback_calls == 1U) {
        ((uint8_t *)data)[0] ^= 0x01U;
    }
    *bytes_read = take;
    return true;
}

static struct candidate_staged_write_io make_staged_io(
    struct drill_storage *storage)
{
    struct candidate_staged_write_io io;

    memset(&io, 0, sizeof(io));
    io.context = storage;
    io.write = fixture_write;
    io.readback = fixture_readback;
    return io;
}

static bool init_intent(struct activation_intent_store *store)
{
    struct activation_intent_entry known_good = make_known_good();

    memset(store, 0, sizeof(*store));
    return activation_intent_init(
        store, &known_good) == ACTIVATION_INTENT_OK;
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
    bool healthy)
{
    struct first_boot_health_report report;

    memset(&report, 0, sizeof(report));
    report.version = FIRST_BOOT_HEALTH_VERSION;
    report.milestone.subsystem = subsystem;
    report.milestone.milestone_id = 1U;
    report.healthy = healthy;
    return first_boot_health_report(store, &report);
}

static bool init_failed_health(struct first_boot_health_store *store)
{
    struct first_boot_health_requirements requirements =
        make_requirements();
    struct first_boot_health_verdict verdict;

    memset(store, 0, sizeof(*store));
    if (first_boot_health_init(store) != FIRST_BOOT_HEALTH_OK ||
        first_boot_health_define(store, &requirements) !=
            FIRST_BOOT_HEALTH_OK ||
        report_milestone(
            store,
            FIRST_BOOT_HEALTH_SUBSYSTEM_BOOT,
            true) != FIRST_BOOT_HEALTH_OK ||
        report_milestone(
            store,
            FIRST_BOOT_HEALTH_SUBSYSTEM_STORAGE,
            false) != FIRST_BOOT_HEALTH_OK) {
        return false;
    }
    return first_boot_health_evaluate(store, &verdict) ==
               FIRST_BOOT_HEALTH_FAILED &&
           verdict.failed;
}

static void test_catalogue_update_faults(void)
{
    static const struct {
        enum fault_injection_id id;
        const char *name;
    } update_faults[] = {
        {FAULT_INJECTION_UPDATE_STAGED_WRITE,
         "FAULT_INJECTION_UPDATE_STAGED_WRITE"},
        {FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH,
         "FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH"},
    };
    size_t index;

    alloc_attempts = 0U;
    free_attempts = 0U;
    for (index = 0U;
         index < sizeof(update_faults) / sizeof(update_faults[0]);
         ++index) {
        const struct fault_injection_site *site = NULL;

        CHECK(fault_injection_site_by_id(
                  update_faults[index].id,
                  &site) == FAULT_INJECTION_OK &&
                  site != NULL &&
                  site->subsystem ==
                      FAULT_INJECTION_SUBSYSTEM_UPDATE &&
                  site->bounded &&
                  site->test_only &&
                  site->expected_outcome != NULL &&
                  site->expected_outcome[0] != '\0' &&
                  strcmp(site->name,
                         update_faults[index].name) == 0,
              "drill consumes exact update fault point");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update catalogue drill performs no allocation");
}

static void test_staged_write_interruption_keeps_known_good(void)
{
    struct candidate_slot_inventory inventory;
    struct candidate_preflight_gate preflight;
    struct activation_intent_store intent;
    struct activation_intent_io intent_io;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry known_good_out;
    struct activation_intent_entry target;
    struct activation_intent_record record_out;
    uint8_t target_digest[SHA256_DIGEST_SIZE];
    struct drill_storage storage;
    struct candidate_staged_write_gate gate;
    struct candidate_staged_write_io io;
    struct candidate_staged_write_gate clean_gate;
    enum candidate_staged_write_status status;
    unsigned int fault_index;

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(bundle_digest(target_digest),
          "update drill computes target digest");
    target = make_target_entry(target_digest);
    CHECK(make_inventory(&inventory, target_digest),
          "update drill publishes active slot inventory");
    CHECK(make_preflight(&preflight, &inventory, target_digest),
          "update drill passes candidate preflight");
    CHECK(init_intent(&intent),
          "update drill initializes activation intent");
    memset(&intent_io, 0, sizeof(intent_io));

    for (fault_index = 0U; fault_index < 4U; ++fault_index) {
        const char *fault_name;
        enum candidate_staged_write_status expected_status;

        reset_storage(&storage);
        io = make_staged_io(&storage);
        if (fault_index == 0U) {
            storage.fail_write = true;
            expected_status = CANDIDATE_STAGED_WRITE_WRITE_FAULT;
            fault_name = "write";
        } else if (fault_index == 1U) {
            storage.fail_readback = true;
            expected_status = CANDIDATE_STAGED_WRITE_READBACK_FAULT;
            fault_name = "readback";
        } else if (fault_index == 2U) {
            storage.short_readback = true;
            expected_status = CANDIDATE_STAGED_WRITE_READBACK_FAULT;
            fault_name = "short readback";
        } else {
            storage.corrupt_readback = true;
            expected_status = CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH;
            fault_name = "corrupt readback";
        }
        memset(&gate, 0, sizeof(gate));
        CHECK(candidate_staged_write_init(&gate) ==
                  CANDIDATE_STAGED_WRITE_OK,
              "staged-write fault gate initializes");
        status = candidate_staged_write_run(
            &gate, &preflight, &inventory,
            drill_bundle, &io);
        CHECK(candidate_staged_write_init(&gate) ==
                  CANDIDATE_STAGED_WRITE_BAD_STATE &&
                  status == expected_status &&
                  !gate.ready &&
                  gate.slot_id == 0U,
              "staged-write interruption returns exact gate failure");
        CHECK(activation_intent_record(
                  &intent, &gate, &intent_io) ==
                  ACTIVATION_INTENT_BAD_CANDIDATE &&
                  activation_intent_pending(
                      &intent, &record_out) ==
                      ACTIVATION_INTENT_NO_PENDING &&
                  activation_intent_known_good(
                      &intent, &known_good_out) ==
                      ACTIVATION_INTENT_OK &&
                  entries_equal(&known_good_out, &known_good),
              "staged-write interruption keeps known-good selected");
        printf("PASS: staged-write %s fault retains known-good\n",
               fault_name);
    }

    reset_storage(&storage);
    io = make_staged_io(&storage);
    memset(&clean_gate, 0, sizeof(clean_gate));
    CHECK(candidate_staged_write_init(&clean_gate) ==
              CANDIDATE_STAGED_WRITE_OK &&
              candidate_staged_write_run(
                  &clean_gate, &preflight, &inventory,
                  drill_bundle, &io) == CANDIDATE_STAGED_WRITE_OK &&
              clean_gate.ready &&
              clean_gate.slot_id == DRILL_TARGET_SLOT,
          "clean staged retry publishes verified candidate");
    CHECK(activation_intent_record(
              &intent, &clean_gate, &intent_io) ==
              ACTIVATION_INTENT_OK &&
              intent.pending.pending &&
              entries_equal(
                  &intent.pending.target,
                  &target) &&
              entries_equal(
                  &intent.pending.rollback, &known_good) &&
              activation_intent_known_good(
                  &intent, &known_good_out) ==
                  ACTIVATION_INTENT_OK &&
              entries_equal(&known_good_out, &known_good),
          "clean retry records pending target with exact rollback");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "staged-write drill performs no allocation");
}

static void test_first_boot_health_failure_selects_known_good(void)
{
    struct candidate_slot_inventory inventory;
    struct candidate_preflight_gate preflight;
    struct activation_intent_store intent;
    struct activation_intent_io intent_io;
    struct activation_intent_entry known_good = make_known_good();
    struct activation_intent_entry target;
    struct activation_intent_entry selected_a;
    struct activation_intent_entry selected_b;
    struct activation_intent_record pending;
    struct first_boot_health_store health;
    struct first_boot_health_verdict verdict;
    struct automatic_rollback_store rollback_a;
    struct automatic_rollback_store rollback_b;
    struct automatic_rollback_evidence evidence_a;
    struct automatic_rollback_evidence evidence_b;
    struct drill_storage storage;
    struct candidate_staged_write_gate gate;
    struct candidate_staged_write_io io;
    uint8_t target_digest[SHA256_DIGEST_SIZE];

    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(bundle_digest(target_digest),
          "rollback drill computes target digest");
    CHECK(make_inventory(&inventory, target_digest) &&
              make_preflight(&preflight, &inventory, target_digest) &&
              init_intent(&intent),
          "rollback drill prepares update fixture");
    reset_storage(&storage);
    io = make_staged_io(&storage);
    memset(&gate, 0, sizeof(gate));
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK &&
              candidate_staged_write_run(
                  &gate, &preflight, &inventory,
                  drill_bundle, &io) == CANDIDATE_STAGED_WRITE_OK &&
              gate.ready,
          "rollback drill stages verified candidate");
    memset(&intent_io, 0, sizeof(intent_io));
    CHECK(activation_intent_record(
              &intent, &gate, &intent_io) ==
              ACTIVATION_INTENT_OK &&
              intent.pending.pending,
          "rollback drill records pending activation intent");
    target = make_target_entry(target_digest);
    CHECK(init_failed_health(&health) &&
              first_boot_health_evaluate(
                  &health, &verdict) ==
                  FIRST_BOOT_HEALTH_FAILED &&
              verdict.failed,
          "rollback drill publishes failed first-boot health");

    memset(&rollback_a, 0, sizeof(rollback_a));
    memset(&rollback_b, 0, sizeof(rollback_b));
    CHECK(automatic_rollback_init(&rollback_a) ==
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
          "independent rollback stores publish evidence twice");
    CHECK(evidence_a.reason ==
              AUTOMATIC_ROLLBACK_REASON_FIRST_BOOT_HEALTH_FAILED &&
              evidence_a.candidate_active == false &&
              evidence_a.health_verdict.failed &&
              entries_equal(&evidence_a.selected_slot, &known_good) &&
              entries_equal(
                  &evidence_a.candidate_slot, &target) &&
              entries_equal(&selected_a, &known_good) &&
              memcmp(&selected_a, &selected_b,
                     sizeof(selected_a)) == 0 &&
              memcmp(&evidence_a, &evidence_b,
                     sizeof(evidence_a)) == 0,
          "first-boot health failure selects known-good deterministically");
    CHECK(activation_intent_pending(
              &intent, &pending) == ACTIVATION_INTENT_OK &&
              pending.pending &&
              entries_equal(&pending.target, &target) &&
              activation_intent_known_good(
                  &intent, &selected_a) ==
                  ACTIVATION_INTENT_OK &&
              entries_equal(&selected_a, &known_good),
          "rollback selection does not mutate pending or known-good");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "rollback drill performs no allocation");
}

int main(void)
{
    fill_bundle();
    test_catalogue_update_faults();
    test_staged_write_interruption_keeps_known_good();
    test_first_boot_health_failure_selects_known_good();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Update failure drill host tests performed allocation\n");
        ++failures;
    }
    if (failures != 0) {
        fprintf(stderr,
                "Update failure drill host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Update failure drill host tests: ALL OK\n");
    return 0;
}

#else
#error "update-failure-drill-host.c must be compiled with HFOC_UPDATE_FAILURE_DRILL_HOST_TEST"
#endif
