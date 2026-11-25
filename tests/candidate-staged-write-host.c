#include "candidate_staged_write.h"
#include "candidate_preflight.h"
#include "candidate_slot_inventory.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_CANDIDATE_STAGED_WRITE_HOST_TEST

#define TEST_BUNDLE_SIZE 10000U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;
static uint8_t test_source[TEST_BUNDLE_SIZE];

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

static void initialize_source(void)
{
    size_t index;

    for (index = 0U; index < TEST_BUNDLE_SIZE; ++index) {
        test_source[index] = (uint8_t)((index * 31U) + (index >> 4U));
    }
}

static void source_digest(uint8_t digest[SHA256_DIGEST_SIZE])
{
    CHECK(sha256_compute(test_source, TEST_BUNDLE_SIZE, digest) ==
              SHA256_OK,
          "staged write fixture computes source digest");
}

struct write_fixture {
    uint8_t storage[CANDIDATE_SLOT_INVENTORY_SLOT_MAX][TEST_BUNDLE_SIZE];
    size_t stored;
    bool fail_write;
    bool fail_readback;
    bool short_readback;
    bool corrupt_readback;
    unsigned int write_calls;
    unsigned int readback_calls;
    uint32_t last_write_slot;
};

static bool fixture_write(void *context,
                          uint32_t slot_id,
                          const void *data,
                          size_t size)
{
    struct write_fixture *fixture = (struct write_fixture *)context;

    ++fixture->write_calls;
    fixture->last_write_slot = slot_id;
    if (fixture->fail_write) {
        return false;
    }
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        data == NULL || size == 0U || size > TEST_BUNDLE_SIZE) {
        return false;
    }
    memcpy(fixture->storage[slot_id - 1U], data, size);
    fixture->stored = size;
    return true;
}

static bool fixture_readback(void *context,
                             uint32_t slot_id,
                             size_t offset,
                             void *data,
                             size_t capacity,
                             size_t *bytes_read)
{
    struct write_fixture *fixture = (struct write_fixture *)context;
    size_t remaining;
    size_t take;

    ++fixture->readback_calls;
    if (bytes_read == NULL) {
        return false;
    }
    *bytes_read = 0U;
    if (fixture->fail_readback) {
        return false;
    }
    if (slot_id < CANDIDATE_SLOT_INVENTORY_SLOT_MIN ||
        slot_id > CANDIDATE_SLOT_INVENTORY_SLOT_MAX ||
        data == NULL || capacity == 0U ||
        offset > fixture->stored) {
        return false;
    }
    remaining = fixture->stored - offset;
    if (remaining == 0U) {
        return true;
    }
    if (fixture->short_readback && remaining <= capacity) {
        return true;
    }
    take = capacity < remaining ? capacity : remaining;
    memcpy(data, fixture->storage[slot_id - 1U] + offset, take);
    if (fixture->corrupt_readback && fixture->readback_calls == 1U) {
        ((uint8_t *)data)[0] ^= 0x01U;
    }
    *bytes_read = take;
    return true;
}

static struct candidate_staged_write_io make_io(
    struct write_fixture *fixture)
{
    struct candidate_staged_write_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.write = fixture_write;
    io.readback = fixture_readback;
    return io;
}

static void reset_fixture(struct write_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    memset(fixture->storage, 0xA5, sizeof(fixture->storage));
}

static bool fixture_storage_untouched(const struct write_fixture *fixture)
{
    size_t slot;
    size_t index;

    for (slot = 0U;
         slot < CANDIDATE_SLOT_INVENTORY_SLOT_MAX;
         ++slot) {
        for (index = 0U; index < TEST_BUNDLE_SIZE; ++index) {
            if (fixture->storage[slot][index] != 0xA5U) {
                return false;
            }
        }
    }
    return true;
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
    uint8_t digest[SHA256_DIGEST_SIZE];

    memset(&request, 0, sizeof(request));
    source_digest(digest);
    request.version = CANDIDATE_PREFLIGHT_VERSION;
    request.slot_id = 2U;
    request.generation = 1U;
    request.trust_status = SIGNED_METADATA_OK;
    request.bundle.size = TEST_BUNDLE_SIZE;
    memcpy(request.bundle.digest, digest, sizeof(digest));
    request.compatibility = make_manifest();
    request.source = make_source();
    request.target = make_target();
    return request;
}

static bool make_inventory(struct candidate_slot_inventory *inventory)
{
    struct candidate_slot_inventory_entry entry;
    uint8_t digest[SHA256_DIGEST_SIZE];

    memset(inventory, 0, sizeof(*inventory));
    memset(&entry, 0, sizeof(entry));
    source_digest(digest);
    entry.slot_id = 2U;
    entry.generation = 1U;
    memcpy(entry.digest, digest, sizeof(digest));
    return candidate_slot_inventory_init(inventory) ==
               CANDIDATE_SLOT_INVENTORY_OK &&
           candidate_slot_inventory_publish(
               inventory, &entry, 1U, 1U) ==
               CANDIDATE_SLOT_INVENTORY_OK;
}

static bool make_preflight(struct candidate_preflight_gate *gate,
                           const struct candidate_slot_inventory *inventory)
{
    struct candidate_preflight_request request = make_request();

    memset(gate, 0, sizeof(*gate));
    return candidate_preflight_init(gate) == CANDIDATE_PREFLIGHT_OK &&
           candidate_preflight_run(
               gate, &request, inventory) == CANDIDATE_PREFLIGHT_OK;
}

static void poison_gate(struct candidate_staged_write_gate *gate)
{
    memset(gate, 0xA5, sizeof(*gate));
}

static bool gate_unchanged(
    const struct candidate_staged_write_gate *gate,
    const struct candidate_staged_write_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum candidate_staged_write_status status;
        const char *string;
    } statuses[] = {
        {CANDIDATE_STAGED_WRITE_OK,
         "CANDIDATE_STAGED_WRITE_OK"},
        {CANDIDATE_STAGED_WRITE_NULL,
         "CANDIDATE_STAGED_WRITE_NULL"},
        {CANDIDATE_STAGED_WRITE_BAD_STATE,
         "CANDIDATE_STAGED_WRITE_BAD_STATE"},
        {CANDIDATE_STAGED_WRITE_BAD_REQUEST,
         "CANDIDATE_STAGED_WRITE_BAD_REQUEST"},
        {CANDIDATE_STAGED_WRITE_BAD_BUNDLE,
         "CANDIDATE_STAGED_WRITE_BAD_BUNDLE"},
        {CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED,
         "CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED"},
        {CANDIDATE_STAGED_WRITE_BAD_SLOT,
         "CANDIDATE_STAGED_WRITE_BAD_SLOT"},
        {CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE,
         "CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE"},
        {CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH,
         "CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH"},
        {CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH,
         "CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH"},
        {CANDIDATE_STAGED_WRITE_WRITE_FAULT,
         "CANDIDATE_STAGED_WRITE_WRITE_FAULT"},
        {CANDIDATE_STAGED_WRITE_READBACK_FAULT,
         "CANDIDATE_STAGED_WRITE_READBACK_FAULT"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = candidate_staged_write_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "candidate staged write status string is exact");
    }
    CHECK(candidate_staged_write_status_string(
              (enum candidate_staged_write_status)12U) == NULL,
          "candidate staged write undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate staged write status handling performs no allocation");
}

static void test_null_and_bad_state(void)
{
    struct candidate_staged_write_gate gate;
    struct candidate_preflight_gate preflight;
    struct candidate_slot_inventory inventory;
    struct write_fixture fixture;
    struct candidate_staged_write_io io;
    struct candidate_staged_write_gate zeroed;

    reset_fixture(&fixture);
    io = make_io(&fixture);
    memset(&gate, 0xA5, sizeof(gate));
    memset(&zeroed, 0, sizeof(zeroed));
    CHECK(make_inventory(&inventory) &&
              make_preflight(&preflight, &inventory),
          "staged write null fixture publishes preflight");

    CHECK(candidate_staged_write_init(NULL) ==
              CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write init rejects null gate");
    CHECK(candidate_staged_write_run(
              NULL, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write run rejects null gate");
    CHECK(candidate_staged_write_run(
              &gate, NULL, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write run rejects null preflight");
    CHECK(candidate_staged_write_run(
              &gate, &preflight, NULL, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write run rejects null inventory");
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, NULL, &io) ==
              CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write run rejects null data");
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, NULL) ==
              CANDIDATE_STAGED_WRITE_NULL,
          "candidate staged write run rejects null io");
    CHECK(candidate_staged_write_run(
              &zeroed, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_BAD_STATE,
          "candidate staged write run rejects uninitialized gate");

    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK &&
              gate.version == CANDIDATE_STAGED_WRITE_VERSION &&
              !gate.ready,
          "candidate staged write init publishes clean one-shot gate");
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_BAD_STATE,
          "candidate staged write init rejects reinitialization");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate staged write null handling performs no allocation");
}

static void test_valid_verification(void)
{
    struct candidate_staged_write_gate gate;
    struct candidate_staged_write_gate original;
    struct candidate_preflight_gate preflight;
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory inventory_before;
    struct write_fixture fixture;
    struct candidate_staged_write_io io;
    uint8_t digest[SHA256_DIGEST_SIZE];
    uint8_t source_before[TEST_BUNDLE_SIZE];
    size_t slot;

    reset_fixture(&fixture);
    io = make_io(&fixture);
    CHECK(make_inventory(&inventory) &&
              make_preflight(&preflight, &inventory),
          "staged write success fixture publishes preflight");
    memcpy(&inventory_before, &inventory, sizeof(inventory));
    memcpy(source_before, test_source, sizeof(test_source));
    source_digest(digest);
    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write success fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_OK &&
              gate.version == CANDIDATE_STAGED_WRITE_VERSION &&
              gate.slot_id == 2U &&
              gate.generation == 1U &&
              gate.bundle_size == TEST_BUNDLE_SIZE &&
              gate.ready &&
              memcmp(gate.digest, digest, SHA256_DIGEST_SIZE) == 0,
          "valid staged write publishes exact ready gate");
    CHECK(memcmp(&inventory, &inventory_before,
                 sizeof(inventory)) == 0,
          "valid staged write never mutates inventory");
    CHECK(memcmp(test_source, source_before, sizeof(source_before)) == 0,
          "valid staged write never mutates source data");
    CHECK(fixture.write_calls == 1U &&
              fixture.last_write_slot == 2U,
          "valid staged write writes exactly the requested slot once");
    CHECK(fixture.readback_calls == 3U,
          "valid staged write reads back every bounded chunk");
    CHECK(memcmp(fixture.storage[1U], test_source,
                 TEST_BUNDLE_SIZE) == 0,
          "valid staged write stores exact candidate bytes");
    for (slot = 0U; slot < CANDIDATE_SLOT_INVENTORY_SLOT_MAX; ++slot) {
        if (slot == 1U) {
            continue;
        }
        CHECK(memcmp(fixture.storage[slot],
                     fixture.storage[0U], TEST_BUNDLE_SIZE) == 0,
              "valid staged write does not touch other storage slots");
    }
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_BAD_STATE,
          "candidate staged write run is one-shot");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid staged write performs no allocation");
}

static void test_local_failures_never_write(void)
{
    struct candidate_staged_write_gate gate;
    struct candidate_staged_write_gate original;
    struct candidate_preflight_gate preflight;
    struct candidate_slot_inventory inventory;
    struct candidate_slot_inventory inactive;
    struct write_fixture fixture;
    struct candidate_staged_write_io io;
    struct candidate_staged_write_io bad_io;

    reset_fixture(&fixture);
    io = make_io(&fixture);
    CHECK(make_inventory(&inventory) &&
              make_preflight(&preflight, &inventory),
          "staged write failure fixture publishes preflight");
    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write failure fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));

    memset(&preflight, 0, sizeof(preflight));
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_PREFLIGHT_DENIED &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U &&
              fixture.readback_calls == 0U &&
              fixture_storage_untouched(&fixture),
          "not-preflighted candidate cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture republishes preflight");

    preflight.candidate.version = 2U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "bad preflight request version cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores request version");
    preflight.candidate.bundle.size = 0U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "zero bundle size cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores bundle size");
    preflight.candidate.bundle.size =
        CANDIDATE_PREFLIGHT_BUNDLE_MAX_SIZE + 1U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "over-limit bundle size cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores over-limit size");
    memset(preflight.candidate.bundle.digest, 0,
           sizeof(preflight.candidate.bundle.digest));
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_BUNDLE &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "zero bundle digest cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores bundle digest");
    preflight.candidate.slot_id = 0U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_SLOT &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "zero slot ID cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores slot ID");
    preflight.candidate.slot_id = 9U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_BAD_SLOT &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "over-limit slot ID cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores slot bounds");
    preflight.candidate.slot_id = 1U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "unknown slot cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores known slot");
    preflight.candidate.generation = 2U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_GENERATION_MISMATCH &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "generation-mismatched candidate cannot start staged write");
    CHECK(make_preflight(&preflight, &inventory),
          "staged write failure fixture restores generation");
    preflight.candidate.bundle.digest[0] ^= 0x01U;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "inventory digest mismatch cannot start staged write");

    CHECK(candidate_slot_inventory_init(&inactive) ==
              CANDIDATE_SLOT_INVENTORY_OK &&
              make_preflight(&preflight, &inventory),
          "staged write failure fixture prepares inactive inventory");
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inactive, test_source, &io) ==
              CANDIDATE_STAGED_WRITE_SLOT_UNAVAILABLE &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "inactive inventory cannot start staged write");

    bad_io = make_io(&fixture);
    bad_io.write = NULL;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &bad_io) ==
              CANDIDATE_STAGED_WRITE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "missing write callback cannot start staged write");
    bad_io = make_io(&fixture);
    bad_io.readback = NULL;
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory, test_source, &bad_io) ==
              CANDIDATE_STAGED_WRITE_BAD_REQUEST &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 0U,
          "missing readback callback cannot start staged write");

    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_OK &&
              gate.ready,
          "local staged write failure may be retried with valid request");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate staged write local failures perform no allocation");
}

static void test_write_and_readback_faults(void)
{
    struct candidate_staged_write_gate gate;
    struct candidate_staged_write_gate original;
    struct candidate_preflight_gate preflight;
    struct candidate_slot_inventory inventory;
    struct write_fixture fixture;
    struct candidate_staged_write_io io;

    CHECK(make_inventory(&inventory) &&
              make_preflight(&preflight, &inventory),
          "staged write fault fixture publishes preflight");

    reset_fixture(&fixture);
    fixture.fail_write = true;
    io = make_io(&fixture);
    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write fault fixture initializes write-fault gate");
    memcpy(&original, &gate, sizeof(gate));
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_WRITE_FAULT &&
              gate_unchanged(&gate, &original) &&
              fixture.write_calls == 1U &&
              fixture.readback_calls == 0U,
          "write fault never invokes readback or marks ready");

    reset_fixture(&fixture);
    io = make_io(&fixture);
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_OK &&
              gate.ready,
          "write fault may be retried after storage fault clears");

    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write fault fixture initializes readback-fault gate");
    memcpy(&original, &gate, sizeof(gate));
    reset_fixture(&fixture);
    fixture.fail_readback = true;
    io = make_io(&fixture);
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_READBACK_FAULT &&
              gate_unchanged(&gate, &original) &&
              !gate.ready &&
              fixture.write_calls == 1U &&
              fixture.readback_calls == 1U,
          "readback fault never marks candidate ready");

    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write fault fixture initializes short-readback gate");
    memcpy(&original, &gate, sizeof(gate));
    reset_fixture(&fixture);
    fixture.short_readback = true;
    io = make_io(&fixture);
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_READBACK_FAULT &&
              gate_unchanged(&gate, &original) &&
              !gate.ready &&
              fixture.write_calls == 1U,
          "incomplete readback never marks candidate ready");

    poison_gate(&gate);
    CHECK(candidate_staged_write_init(&gate) ==
              CANDIDATE_STAGED_WRITE_OK,
          "staged write fault fixture initializes corrupt-readback gate");
    memcpy(&original, &gate, sizeof(gate));
    reset_fixture(&fixture);
    fixture.corrupt_readback = true;
    io = make_io(&fixture);
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_DIGEST_MISMATCH &&
              gate_unchanged(&gate, &original) &&
              !gate.ready &&
              fixture.write_calls == 1U,
          "readback digest mismatch never marks candidate ready");

    reset_fixture(&fixture);
    io = make_io(&fixture);
    CHECK(candidate_staged_write_run(
              &gate, &preflight, &inventory,
              test_source, &io) == CANDIDATE_STAGED_WRITE_OK &&
              gate.ready,
          "readback failure may be retried after fault clears");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "candidate staged write faults perform no allocation");
}

int main(void)
{
    initialize_source();
    test_status_strings();
    test_null_and_bad_state();
    test_valid_verification();
    test_local_failures_never_write();
    test_write_and_readback_faults();

    if (failures != 0) {
        fprintf(stderr, "Candidate staged write host tests: FAILED\n");
        return 1;
    }
    printf("Candidate staged write host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_CANDIDATE_STAGED_WRITE_HOST_TEST"
#endif
