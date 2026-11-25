#include "candidate_preflight.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CANDIDATE_PREFLIGHT_HOST_TEST

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

static void fill_digest(uint8_t digest[CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE],
                        uint8_t seed)
{
    size_t index;

    for (index = 0U; index < CANDIDATE_SLOT_INVENTORY_DIGEST_SIZE; ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

static struct candidate_slot_inventory_entry make_inventory_entry(void)
{
    struct candidate_slot_inventory_entry entry;

    memset(&entry, 0, sizeof(entry));
    entry.slot_id = 2U;
    entry.generation = 1U;
    fill_digest(entry.digest, 0x30U);
    return entry;
}

static bool make_inventory(struct candidate_slot_inventory *inventory)
{
    struct candidate_slot_inventory_entry entry = make_inventory_entry();

    memset(inventory, 0, sizeof(*inventory));
    return candidate_slot_inventory_init(inventory) ==
               CANDIDATE_SLOT_INVENTORY_OK &&
           candidate_slot_inventory_publish(
               inventory, &entry, 1U, 1U) ==
               CANDIDATE_SLOT_INVENTORY_OK;
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

static struct candidate_preflight_request make_request(void)
{
    struct candidate_preflight_request request;

    memset(&request, 0, sizeof(request));
    request.version = CANDIDATE_PREFLIGHT_VERSION;
    request.slot_id = 2U;
    request.generation = 1U;
    request.trust_status = SIGNED_METADATA_OK;
    request.bundle.size = 4096U;
    fill_digest(request.bundle.digest, 0x30U);
    request.compatibility = make_manifest();
    request.source = make_source();
    request.target = make_target();
    return request;
}

static void poison_gate(struct candidate_preflight_gate *gate)
{
    memset(gate, 0xA5, sizeof(*gate));
}

static bool gate_unchanged(
    const struct candidate_preflight_gate *gate,
    const struct candidate_preflight_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum candidate_preflight_status status;
        const char *string;
    } statuses[] = {
        {CANDIDATE_PREFLIGHT_OK, "CANDIDATE_PREFLIGHT_OK"},
        {CANDIDATE_PREFLIGHT_NULL, "CANDIDATE_PREFLIGHT_NULL"},
        {CANDIDATE_PREFLIGHT_BAD_STATE, "CANDIDATE_PREFLIGHT_BAD_STATE"},
        {CANDIDATE_PREFLIGHT_BAD_REQUEST,
         "CANDIDATE_PREFLIGHT_BAD_REQUEST"},
        {CANDIDATE_PREFLIGHT_BAD_BUNDLE,
         "CANDIDATE_PREFLIGHT_BAD_BUNDLE"},
        {CANDIDATE_PREFLIGHT_BAD_SIZE, "CANDIDATE_PREFLIGHT_BAD_SIZE"},
        {CANDIDATE_PREFLIGHT_TRUST_DENIED,
         "CANDIDATE_PREFLIGHT_TRUST_DENIED"},
        {CANDIDATE_PREFLIGHT_BAD_SLOT, "CANDIDATE_PREFLIGHT_BAD_SLOT"},
        {CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE,
         "CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE"},
        {CANDIDATE_PREFLIGHT_GENERATION_MISMATCH,
         "CANDIDATE_PREFLIGHT_GENERATION_MISMATCH"},
        {CANDIDATE_PREFLIGHT_DIGEST_MISMATCH,
         "CANDIDATE_PREFLIGHT_DIGEST_MISMATCH"},
        {CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED,
         "CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = candidate_preflight_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "candidate preflight status string is exact");
    }
    CHECK(candidate_preflight_status_string(
              (enum candidate_preflight_status)12U) == NULL,
          "candidate preflight undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate preflight status handling performs no allocation");
}

static void test_null_and_bad_state(void)
{
    struct candidate_preflight_gate gate;
    struct candidate_preflight_gate original;
    struct candidate_preflight_request request = make_request();
    struct candidate_slot_inventory inventory;
    struct candidate_preflight_gate zeroed;

    memset(&gate, 0xA5, sizeof(gate));
    memset(&zeroed, 0, sizeof(zeroed));
    memset(&inventory, 0xCD, sizeof(inventory));

    CHECK(candidate_preflight_init(NULL) == CANDIDATE_PREFLIGHT_NULL,
          "candidate preflight init rejects null gate");
    CHECK(candidate_preflight_run(
              NULL, &request, &inventory) == CANDIDATE_PREFLIGHT_NULL,
          "candidate preflight run rejects null gate");
    CHECK(candidate_preflight_run(
              &gate, NULL, &inventory) == CANDIDATE_PREFLIGHT_NULL,
          "candidate preflight run rejects null request");
    CHECK(candidate_preflight_run(
              &gate, &request, NULL) == CANDIDATE_PREFLIGHT_NULL,
          "candidate preflight run rejects null inventory");

    CHECK(candidate_preflight_run(
              &zeroed, &request, &inventory) ==
              CANDIDATE_PREFLIGHT_BAD_STATE,
          "candidate preflight run rejects uninitialized gate");
    CHECK(candidate_preflight_init(&gate) == CANDIDATE_PREFLIGHT_OK &&
              gate.version == CANDIDATE_PREFLIGHT_VERSION &&
              !gate.preflighted,
          "candidate preflight init publishes clean one-shot gate");
    CHECK(candidate_preflight_init(&gate) == CANDIDATE_PREFLIGHT_BAD_STATE,
          "candidate preflight init rejects reinitialization");

    memcpy(&original, &gate, sizeof(gate));
    request.version = 2U;
    CHECK(candidate_preflight_run(
              &gate, &request, &inventory) ==
              CANDIDATE_PREFLIGHT_BAD_REQUEST &&
              gate_unchanged(&gate, &original),
          "bad preflight request version leaves gate unchanged");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate preflight null handling performs no allocation");
}

static void test_valid_preflight(void)
{
    struct candidate_preflight_gate gate;
    struct candidate_preflight_request request = make_request();
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory inventory_before;

    CHECK(make_inventory(&inventory),
          "candidate preflight fixture publishes active inventory");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    poison_gate(&gate);
    CHECK(candidate_preflight_init(&gate) == CANDIDATE_PREFLIGHT_OK,
          "candidate preflight fixture initializes gate");
    CHECK(candidate_preflight_run(
              &gate, &request, &inventory) == CANDIDATE_PREFLIGHT_OK &&
              gate.version == CANDIDATE_PREFLIGHT_VERSION &&
              gate.preflighted &&
              memcmp(&gate.candidate, &request,
                     sizeof(request)) == 0,
          "valid candidate preflight publishes exact candidate");
    CHECK(memcmp(&inventory, &inventory_before,
                 sizeof(inventory)) == 0,
          "valid candidate preflight never mutates inventory");
    CHECK(candidate_preflight_run(
              &gate, &request, &inventory) ==
              CANDIDATE_PREFLIGHT_BAD_STATE,
          "candidate preflight run is one-shot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid candidate preflight performs no allocation");
}

static void check_failure_leaves_unchanged(
    struct candidate_preflight_gate *gate,
    struct candidate_preflight_gate *original,
    const struct candidate_preflight_request *request,
    const struct candidate_slot_inventory *inventory,
    const struct candidate_slot_inventory *inventory_before,
    enum candidate_preflight_status expected,
    const char *message)
{
    CHECK(candidate_preflight_run(
              gate, request, inventory) == expected &&
              gate_unchanged(gate, original) &&
              memcmp(inventory, inventory_before,
                     sizeof(*inventory)) == 0,
          message);
}

static void test_failures_leave_gate_and_inventory_unchanged(void)
{
    struct candidate_preflight_gate gate;
    struct candidate_preflight_gate original;
    struct candidate_preflight_request request = make_request();
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory inventory_before;

    CHECK(make_inventory(&inventory),
          "preflight failure fixture publishes active inventory");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    poison_gate(&gate);
    CHECK(candidate_preflight_init(&gate) == CANDIDATE_PREFLIGHT_OK,
          "preflight failure fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));

    request.version = 2U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_REQUEST,
        "bad request version cannot publish preflight");
    request = make_request();

    request.bundle.size = 0U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_SIZE,
        "zero bundle size cannot publish preflight");
    request = make_request();
    request.bundle.size = CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE + 1U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_SIZE,
        "over-limit bundle size cannot publish preflight");
    request = make_request();
    memset(request.bundle.digest, 0, sizeof(request.bundle.digest));
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_BUNDLE,
        "zero bundle digest cannot publish preflight");
    request = make_request();
    request.trust_status = SIGNED_METADATA_BAD_SIGNATURE;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_TRUST_DENIED,
        "bad signature trust result cannot publish preflight");
    request = make_request();
    request.trust_status = SIGNED_METADATA_PURPOSE_DENIED;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_TRUST_DENIED,
        "denied trust purpose cannot publish preflight");
    request = make_request();
    request.source.image_id = 99U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_COMPATIBILITY_DENIED,
        "incompatible source cannot publish preflight");
    request = make_request();
    request.slot_id = 0U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_SLOT,
        "zero slot ID cannot publish preflight");
    request = make_request();
    request.slot_id = 9U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_BAD_SLOT,
        "over-limit slot ID cannot publish preflight");
    request = make_request();
    request.generation = 2U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_GENERATION_MISMATCH,
        "generation-mismatched candidate cannot publish preflight");
    request = make_request();
    request.bundle.digest[0] ^= 0x01U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_DIGEST_MISMATCH,
        "digest-mismatched candidate cannot publish preflight");

    memset(&inventory, 0, sizeof(inventory));
    CHECK(candidate_slot_inventory_init(&inventory) ==
              CANDIDATE_SLOT_INVENTORY_OK,
          "preflight failure fixture initializes inactive inventory");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    request = make_request();
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE,
        "inactive inventory cannot publish preflight");

    CHECK(make_inventory(&inventory),
          "preflight retry fixture republishes active inventory");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    request = make_request();
    request.slot_id = 1U;
    check_failure_leaves_unchanged(
        &gate, &original, &request, &inventory, &inventory_before,
        CANDIDATE_PREFLIGHT_SLOT_UNAVAILABLE,
        "unknown slot cannot publish preflight");

    request = make_request();
    CHECK(candidate_preflight_run(
              &gate, &request, &inventory) == CANDIDATE_PREFLIGHT_OK &&
              gate.preflighted &&
              memcmp(&inventory, &inventory_before,
                     sizeof(inventory)) == 0,
          "failed preflight may be retried with a valid request");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate preflight failure paths perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_bad_state();
    test_valid_preflight();
    test_failures_leave_gate_and_inventory_unchanged();

    if (failures != 0) {
        fprintf(stderr, "Candidate preflight host tests: FAILED\n");
        return 1;
    }
    printf("Candidate preflight host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_CANDIDATE_PREFLIGHT_HOST_TEST"
#endif
