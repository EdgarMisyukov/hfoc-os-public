#include "entropy_diagnostics.h"

#include "entropy_accumulator.h"
#include "entropy_health.h"
#include "entropy_seed_fill.h"
#include "appliance_production_entropy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ENTROPY_DIAGNOSTICS_HOST_TEST

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

static struct entropy_health_config make_health_config(void)
{
    struct entropy_health_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_HEALTH_VERSION;
    config.block_length_min = 1U;
    config.block_length_max = ENTROPY_HEALTH_BLOCK_LENGTH_MAX;
    config.max_stall_ticks = 20U;
    return config;
}

static struct entropy_accumulator_config make_accumulator_config(void)
{
    struct entropy_accumulator_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_ACCUMULATOR_VERSION;
    config.generation = 7U;
    config.source_id = 42U;
    config.block_length_min = 1U;
    config.block_length_max = ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX;
    config.max_stall_ticks = 20U;
    return config;
}

static struct entropy_seed_fill_config make_seed_config(void)
{
    struct entropy_seed_fill_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_SEED_FILL_VERSION;
    config.generation = 7U;
    config.source_id = 42U;
    config.required_credited_bytes = 32U;
    config.seed_length = 32U;
    config.fill_length_max = 8U;
    config.fill_count_max = 4U;
    return config;
}

static void fill_block(
    uint8_t *block,
    uint32_t block_length,
    uint8_t seed)
{
    uint32_t index;

    for (index = 0U; index < block_length; ++index) {
        block[index] = (uint8_t)(seed + index * 17U);
    }
}

static void reset_fixture(void)
{
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static void expect_no_allocation(const char *message)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U, message);
}

static void init_health_accumulator(
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    const uint8_t *block,
    uint32_t block_length)
{
    struct entropy_health_config health_config = make_health_config();
    struct entropy_accumulator_config accumulator_config =
        make_accumulator_config();

    memset(health, 0, sizeof(*health));
    CHECK(entropy_health_init(
              health, &health_config, 7U, 10U) == ENTROPY_HEALTH_OK &&
              entropy_health_observe(
                  health, 7U, block, block_length, 15U) ==
                  ENTROPY_HEALTH_OK &&
              entropy_health_ready(health),
          "diagnostics fixture publishes ready health");

    memset(accumulator, 0, sizeof(*accumulator));
    CHECK(entropy_accumulator_init(
              accumulator, &accumulator_config, 10U) ==
              ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_mix(
                  accumulator,
                  7U,
                  42U,
                  block,
                  block_length,
                  15U) == ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_accumulating(accumulator),
          "diagnostics fixture publishes ready accumulator");
}

static void init_ready_controller(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator)
{
    struct entropy_seed_fill_config config = make_seed_config();
    uint8_t seed_output[64];
    uint8_t fill_output[8];
    uint32_t written;

    memset(controller, 0, sizeof(*controller));
    CHECK(entropy_seed_fill_init(
              controller, &config, accumulator) ==
              ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_ready(controller),
          "diagnostics fixture publishes ready seed/fill controller");
    written = 0U;
    CHECK(entropy_seed_fill_seed(
              controller,
              accumulator,
              seed_output,
              (uint32_t)sizeof(seed_output),
              &written) == ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_seed_published(controller),
          "diagnostics fixture publishes seed");
    written = 0U;
    CHECK(entropy_seed_fill_fill(
              controller,
              accumulator,
              fill_output,
              (uint32_t)sizeof(fill_output),
              8U,
              &written) == ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_fill_count(controller) == 1U,
          "diagnostics fixture publishes one fill");
}

static void init_ready_chain(
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *controller,
    uint8_t *block,
    uint32_t block_length)
{
    fill_block(block, block_length, 0xA5U);
    init_health_accumulator(health, accumulator, block, block_length);
    init_ready_controller(controller, accumulator);
}

static bool bytes_contain_sequence(
    const uint8_t *bytes,
    size_t bytes_length,
    const uint8_t *needle,
    size_t needle_length)
{
    size_t index;
    size_t offset;

    if (needle_length == 0U || needle_length > bytes_length) {
        return false;
    }
    for (index = 0U;
         index <= bytes_length - needle_length;
         ++index) {
        bool found = true;

        for (offset = 0U; offset < needle_length; ++offset) {
            if (bytes[index + offset] != needle[offset]) {
                found = false;
                break;
            }
        }
        if (found) {
            return true;
        }
    }
    return false;
}

static void test_strings_and_policy(void)
{
    static const struct {
        enum entropy_diagnostics_state state;
        const char *string;
    } states[] = {
        {ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED,
         "ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED"},
        {ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE,
         "ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE"},
        {ENTROPY_DIAGNOSTICS_STATE_DEGRADED,
         "ENTROPY_DIAGNOSTICS_STATE_DEGRADED"},
        {ENTROPY_DIAGNOSTICS_STATE_READY,
         "ENTROPY_DIAGNOSTICS_STATE_READY"},
        {ENTROPY_DIAGNOSTICS_STATE_FAILED,
         "ENTROPY_DIAGNOSTICS_STATE_FAILED"},
    };
    static const struct {
        enum entropy_diagnostics_fault fault;
        const char *string;
    } faults[] = {
        {ENTROPY_DIAGNOSTICS_FAULT_NONE,
         "ENTROPY_DIAGNOSTICS_FAULT_NONE"},
        {ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK,
         "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK"},
        {ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH,
         "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH"},
        {ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED,
         "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED"},
        {ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET,
         "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET"},
        {ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE,
         "ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE"},
        {ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH,
         "ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH"},
        {ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH,
         "ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH"},
        {ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES,
         "ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES"},
        {ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP,
         "ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP"},
        {ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW,
         "ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW"},
        {ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED,
         "ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED"},
    };
    static const struct {
        enum entropy_diagnostics_status status;
        const char *string;
    } statuses[] = {
        {ENTROPY_DIAGNOSTICS_OK, "ENTROPY_DIAGNOSTICS_OK"},
        {ENTROPY_DIAGNOSTICS_NULL, "ENTROPY_DIAGNOSTICS_NULL"},
        {ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT,
         "ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT"},
        {ENTROPY_DIAGNOSTICS_BAD_STATE,
         "ENTROPY_DIAGNOSTICS_BAD_STATE"},
        {ENTROPY_DIAGNOSTICS_BAD_VERSION,
         "ENTROPY_DIAGNOSTICS_BAD_VERSION"},
        {ENTROPY_DIAGNOSTICS_BAD_GENERATION,
         "ENTROPY_DIAGNOSTICS_BAD_GENERATION"},
        {ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID,
         "ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID"},
        {ENTROPY_DIAGNOSTICS_TOO_SMALL,
         "ENTROPY_DIAGNOSTICS_TOO_SMALL"},
    };
    size_t index;

    reset_fixture();
    CHECK(entropy_diagnostics_version() == ENTROPY_DIAGNOSTICS_VERSION,
          "entropy diagnostics version is exact");
    CHECK(entropy_diagnostics_policy_id() != NULL &&
              strlen(entropy_diagnostics_policy_id()) ==
                  ENTROPY_DIAGNOSTICS_POLICY_ID_LENGTH &&
              memcmp(entropy_diagnostics_policy_id(),
                     ENTROPY_DIAGNOSTICS_POLICY_ID,
                     ENTROPY_DIAGNOSTICS_POLICY_ID_LENGTH) == 0,
          "entropy diagnostics policy ID is exact");
    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        const char *string = entropy_diagnostics_state_string(
            states[index].state);

        CHECK(string != NULL &&
                  strcmp(string, states[index].string) == 0,
              "entropy diagnostics state string is exact");
    }
    CHECK(entropy_diagnostics_state_string(
              (enum entropy_diagnostics_state)99U) == NULL,
          "entropy diagnostics rejects undefined state string");
    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = entropy_diagnostics_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "entropy diagnostics fault string is exact");
    }
    CHECK(entropy_diagnostics_fault_string(
              (enum entropy_diagnostics_fault)99U) == NULL,
          "entropy diagnostics rejects undefined fault string");
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = entropy_diagnostics_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "entropy diagnostics status string is exact");
    }
    CHECK(entropy_diagnostics_status_string(
              (enum entropy_diagnostics_status)99U) == NULL,
          "entropy diagnostics rejects undefined status string");
    expect_no_allocation(
        "entropy diagnostics strings perform no allocation");
}

static void test_snapshot_rejections(void)
{
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_diagnostics_snapshot snapshot;
    uint8_t block[64];
    unsigned char before[sizeof(snapshot)];

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    memset(&snapshot, 0xA5, sizeof(snapshot));
    memcpy(before, &snapshot, sizeof(before));

    reset_fixture();
    CHECK(entropy_diagnostics_snapshot(
              NULL, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_NULL,
          "snapshot rejects null health");
    CHECK(entropy_diagnostics_snapshot(
              &health, NULL, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_NULL,
          "snapshot rejects null accumulator");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, NULL, &snapshot) ==
              ENTROPY_DIAGNOSTICS_NULL,
          "snapshot rejects null controller");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, NULL) ==
              ENTROPY_DIAGNOSTICS_NULL,
          "snapshot rejects null snapshot output");
    CHECK(memcmp(&snapshot, before, sizeof(before)) == 0,
          "snapshot rejection leaves output unchanged");

    memset(&health, 0, sizeof(health));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_BAD_STATE,
          "snapshot rejects uninitialized health");
    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_BAD_STATE,
          "snapshot rejects uninitialized accumulator");
    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_BAD_STATE,
          "snapshot rejects uninitialized controller");
    expect_no_allocation(
        "snapshot rejection paths perform no allocation");
}

static void test_snapshot_mismatches(void)
{
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_diagnostics_snapshot snapshot;
    uint8_t block[64];
    struct entropy_accumulator_config accumulator_config =
        make_accumulator_config();
    uint8_t other_block[64];

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    accumulator_config.generation = 8U;
    fill_block(other_block, (uint32_t)sizeof(other_block), 0x5AU);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &accumulator_config, 10U) ==
              ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_mix(
                  &accumulator,
                  8U,
                  42U,
                  other_block,
                  (uint32_t)sizeof(other_block),
                  15U) == ENTROPY_ACCUMULATOR_OK,
          "mismatch fixture publishes other-generation accumulator");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_BAD_GENERATION,
          "snapshot rejects generation mismatch");

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    accumulator_config = make_accumulator_config();
    accumulator_config.source_id = 43U;
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &accumulator_config, 10U) ==
              ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_mix(
                  &accumulator,
                  7U,
                  43U,
                  other_block,
                  (uint32_t)sizeof(other_block),
                  15U) == ENTROPY_ACCUMULATOR_OK,
          "source-mismatch fixture publishes other-source accumulator");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID,
          "snapshot rejects source-identity mismatch");
    expect_no_allocation(
        "snapshot mismatch paths perform no allocation");
}

static void test_ready_snapshot(void)
{
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_diagnostics_snapshot snapshot;
    uint8_t block[64];

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    reset_fixture();
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK &&
              snapshot.version == ENTROPY_DIAGNOSTICS_VERSION &&
              snapshot.generation == 7U &&
              snapshot.source_id == 42U &&
              snapshot.state == ENTROPY_DIAGNOSTICS_STATE_READY &&
              snapshot.fault == ENTROPY_DIAGNOSTICS_FAULT_NONE &&
              snapshot.credited_bytes == 64U &&
              snapshot.credited_blocks == 1U &&
              snapshot.last_accept_tick == 15U &&
              snapshot.seed_published &&
              snapshot.seed_material_bytes == 32U &&
              snapshot.fill_count == 1U,
          "ready snapshot publishes exact safe fields");
    expect_no_allocation(
        "ready snapshot performs no allocation");
}

static void test_state_mapping(void)
{
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config seed_config = make_seed_config();
    struct entropy_seed_fill_config fresh_config = make_seed_config();
    struct entropy_diagnostics_snapshot snapshot;
    uint8_t block[64];
    uint8_t output[64];
    uint32_t written;

    fill_block(block, (uint32_t)sizeof(block), 0xA5U);
    init_health_accumulator(
        &health, &accumulator, block, (uint32_t)sizeof(block));
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &seed_config, NULL) ==
              ENTROPY_SEED_FILL_OK,
          "unavailable fixture publishes unavailable controller");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK &&
              snapshot.state ==
                  ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE &&
              snapshot.fault ==
                  ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE,
          "unavailable controller maps to unavailable diagnostics state");

    fill_block(block, 8U, 0x31U);
    memset(&accumulator, 0, sizeof(accumulator));
    {
        struct entropy_accumulator_config accumulator_config =
            make_accumulator_config();

        CHECK(entropy_accumulator_init(
                  &accumulator, &accumulator_config, 10U) ==
                  ENTROPY_ACCUMULATOR_OK &&
                  entropy_accumulator_mix(
                      &accumulator, 7U, 42U, block, 8U, 15U) ==
                      ENTROPY_ACCUMULATOR_OK,
              "degraded fixture publishes credited accumulator");
    }
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &seed_config, &accumulator) ==
              ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_state(&controller) ==
                  ENTROPY_SEED_FILL_STATE_DEGRADED,
          "degraded fixture publishes degraded controller");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK &&
              snapshot.state == ENTROPY_DIAGNOSTICS_STATE_DEGRADED &&
              snapshot.fault ==
                  ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES,
          "degraded controller maps to degraded diagnostics state");

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    CHECK(entropy_health_progress(
              &health, 36U) == ENTROPY_HEALTH_STALLED,
          "failed fixture fails health by stall");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK &&
              snapshot.state == ENTROPY_DIAGNOSTICS_STATE_FAILED &&
              snapshot.fault ==
                  ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED,
          "failed health maps to failed diagnostics state");

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &fresh_config, &accumulator) ==
              ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_ready(&controller),
          "controller-failed fixture reinitializes unseeded controller");
    written = 0U;
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              &controller,
              (uint32_t)sizeof(controller),
              &written) == ENTROPY_SEED_FILL_OUTPUT_OVERLAP &&
              entropy_seed_fill_state(&controller) ==
                  ENTROPY_SEED_FILL_STATE_FAILED,
          "controller-failed fixture fails seed by overlap");
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK &&
              snapshot.state == ENTROPY_DIAGNOSTICS_STATE_FAILED &&
              snapshot.fault ==
                  ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP,
          "failed controller maps to failed diagnostics state");
    (void)output;
    expect_no_allocation(
        "diagnostics state mapping performs no allocation");
}

static void test_serial_negative_and_too_small(void)
{
    struct entropy_diagnostics_snapshot snapshot;
    static const char expected[] =
        "ENTROPY_DIAG version=1 generation=7 source_id=42 "
        "state=ENTROPY_DIAGNOSTICS_STATE_READY "
        "fault=ENTROPY_DIAGNOSTICS_FAULT_NONE credited_bytes=64 "
        "credited_blocks=1 last_accept_tick=15 "
        "seed_published=1 seed_material_bytes=32 fill_count=1\n";
    char serial[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    char before[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    size_t length = 0U;
    uint8_t block[64];
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK,
          "serial fixture publishes snapshot");

    memset(serial, 0xCC, sizeof(serial));
    memset(before, 0xCC, sizeof(before));
    CHECK(entropy_diagnostics_serial(
              &snapshot, serial, sizeof(expected) - 2U, &length) ==
              ENTROPY_DIAGNOSTICS_TOO_SMALL &&
              memcmp(serial, before, sizeof(before)) == 0 &&
              length == 0U,
          "short serial output returns too-small without mutation");

    length = 0U;
    CHECK(entropy_diagnostics_serial(
              &snapshot, serial, sizeof(serial), &length) ==
              ENTROPY_DIAGNOSTICS_OK &&
              length == sizeof(expected) - 1U &&
              memcmp(serial, expected, sizeof(expected)) == 0,
          "serial output is deterministic and exact");
    CHECK(!bytes_contain_sequence(
              (const uint8_t *)serial, length, block, 4U),
          "serial negative fixture contains no raw entropy block bytes");
    CHECK(entropy_diagnostics_serial(
              NULL, serial, sizeof(serial), &length) ==
              ENTROPY_DIAGNOSTICS_NULL,
          "serial rejects null snapshot");
    expect_no_allocation(
        "serial rendering performs no allocation");
}

static void test_serial_worst_case_bounds(void)
{
    struct entropy_diagnostics_snapshot snapshot;
    char serial[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    char before[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    size_t length = 0U;
    size_t index;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = ENTROPY_DIAGNOSTICS_VERSION;
    snapshot.generation = ENTROPY_ACCUMULATOR_GENERATION_MAX;
    snapshot.source_id = ENTROPY_ACCUMULATOR_SOURCE_ID_MAX;
    snapshot.state = ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED;
    snapshot.fault =
        ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH;
    snapshot.credited_bytes = ENTROPY_ACCUMULATOR_CAPACITY;
    snapshot.credited_blocks = ENTROPY_ACCUMULATOR_CAPACITY;
    snapshot.last_accept_tick = UINT32_MAX;
    snapshot.seed_published = true;
    snapshot.seed_material_bytes = ENTROPY_SEED_FILL_SEED_LENGTH_MAX;
    snapshot.fill_count = ENTROPY_SEED_FILL_FILL_COUNT_MAX;

    memset(serial, 0xCC, sizeof(serial));
    reset_fixture();
    CHECK(entropy_diagnostics_serial(
              &snapshot, serial, sizeof(serial), &length) ==
              ENTROPY_DIAGNOSTICS_OK &&
              length == 292U &&
              length < ENTROPY_DIAGNOSTICS_SERIAL_MAX &&
              serial[length] == '\0',
          "worst-case serial line fits bounded buffer");

    memcpy(before, serial, sizeof(before));
    CHECK(entropy_diagnostics_serial(
              &snapshot, serial, length, &length) ==
              ENTROPY_DIAGNOSTICS_TOO_SMALL &&
              memcmp(serial, before, sizeof(before)) == 0 &&
              length == 292U,
          "worst-case serial line enforces NUL capacity");

    for (index = 0U; index < length; ++index) {
        if (serial[index] == '\0') {
            CHECK(false, "worst-case serial line has no embedded NUL");
            return;
        }
    }
    expect_no_allocation(
        "worst-case serial rendering performs no allocation");
}

static void test_audit_payload(void)
{
    struct entropy_diagnostics_snapshot snapshot;
    struct entropy_diagnostics_audit audit;
    uint8_t block[64];
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK,
          "audit fixture publishes snapshot");
    reset_fixture();
    CHECK(entropy_diagnostics_audit(
              &snapshot, &audit) == ENTROPY_DIAGNOSTICS_OK &&
              audit.version == ENTROPY_DIAGNOSTICS_VERSION &&
              audit.state == ENTROPY_DIAGNOSTICS_STATE_READY &&
              audit.fault == ENTROPY_DIAGNOSTICS_FAULT_NONE &&
              audit.generation == 7U &&
              audit.source_id == 42U &&
              audit.credited_bytes == 64U &&
              audit.credited_blocks == 1U &&
              audit.seed_material_bytes == 32U &&
              audit.fill_count == 1U &&
              audit.last_accept_tick == 15U,
          "audit payload contains exact safe numeric fields");
    CHECK(entropy_diagnostics_audit(
              NULL, &audit) == ENTROPY_DIAGNOSTICS_NULL,
          "audit rejects null snapshot");
    CHECK(entropy_diagnostics_audit(
              &snapshot, NULL) == ENTROPY_DIAGNOSTICS_NULL,
          "audit rejects null audit output");
    expect_no_allocation(
        "audit payload rendering performs no allocation");
}

static uint32_t load_le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t load_le64(const uint8_t *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void test_support_negative_and_too_small(void)
{
    struct entropy_diagnostics_snapshot snapshot;
    uint8_t support[ENTROPY_DIAGNOSTICS_SUPPORT_SIZE];
    uint8_t before[ENTROPY_DIAGNOSTICS_SUPPORT_SIZE];
    size_t length = 0U;
    uint8_t block[64];
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    size_t index;

    init_ready_chain(
        &health, &accumulator, &controller, block,
        (uint32_t)sizeof(block));
    CHECK(entropy_diagnostics_snapshot(
              &health, &accumulator, &controller, &snapshot) ==
              ENTROPY_DIAGNOSTICS_OK,
          "support fixture publishes snapshot");

    memset(support, 0xCC, sizeof(support));
    memcpy(before, support, sizeof(before));
    CHECK(entropy_diagnostics_support(
              &snapshot, support,
              ENTROPY_DIAGNOSTICS_SUPPORT_SIZE - 1U,
              &length) == ENTROPY_DIAGNOSTICS_TOO_SMALL &&
              memcmp(support, before, sizeof(before)) == 0 &&
              length == 0U,
          "short support output returns too-small without mutation");

    CHECK(entropy_diagnostics_support(
              &snapshot, support, sizeof(support), &length) ==
              ENTROPY_DIAGNOSTICS_OK &&
              length == ENTROPY_DIAGNOSTICS_SUPPORT_SIZE &&
              memcmp(support, "ENTRDIAG", 8U) == 0 &&
              support[8] == ENTROPY_DIAGNOSTICS_VERSION &&
              load_le32(support + 9U) == 7U &&
              load_le32(support + 13U) == 42U &&
              load_le32(support + 17U) ==
                  ENTROPY_DIAGNOSTICS_STATE_READY &&
              load_le32(support + 21U) ==
                  ENTROPY_DIAGNOSTICS_FAULT_NONE &&
              load_le64(support + 25U) == 64U &&
              load_le32(support + 33U) == 1U &&
              load_le32(support + 37U) == 15U &&
              support[41U] == 1U &&
              load_le32(support + 42U) == 32U &&
              load_le32(support + 46U) == 1U,
          "support payload is deterministic and exact");
    for (index = 50U; index < sizeof(support); ++index) {
        if (support[index] != 0U) {
            CHECK(false, "support reserved bytes are zero");
            return;
        }
    }
    CHECK(!bytes_contain_sequence(
              support, sizeof(support), block, 4U),
          "support negative fixture contains no raw entropy block bytes");
    expect_no_allocation(
        "support rendering performs no allocation");
}

/*
 * ITEM-581: the CPU profiles the production boot must handle.  The installed
 * Toshiba refused `credentials` with NO_RANDOM; the chosen source is a pure
 * function of the CPUID capabilities, so the three profiles are pinned here -
 * RDSEED when both bits are advertised, RDRAND when only that, and NONE when
 * neither (a CPU with no leaf 7 cannot claim RDSEED even if a model lies about
 * the leaf's bits).
 */
static void test_entropy_cpuid_profiles(void)
{
    CHECK(appliance_production_entropy_source_for_cpuid(13U, true, true) ==
              APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDSEED,
          "a CPU with RDRAND and RDSEED selects RDSEED");
    CHECK(appliance_production_entropy_source_for_cpuid(13U, true, false) ==
              APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND,
          "a CPU with RDRAND only selects RDRAND");
    CHECK(appliance_production_entropy_source_for_cpuid(13U, false, false) ==
              APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE,
          "a CPU with neither source selects NONE");
    CHECK(appliance_production_entropy_source_for_cpuid(6U, false, true) ==
              APPLIANCE_PRODUCTION_ENTROPY_SOURCE_NONE,
          "no CPUID leaf 7 means no RDSEED, whatever the bit would say");
    CHECK(appliance_production_entropy_source_for_cpuid(6U, true, false) ==
              APPLIANCE_PRODUCTION_ENTROPY_SOURCE_RDRAND,
          "leaf 1's RDRAND bit is still honoured without leaf 7");
}

/*
 * ITEM-585: the operator seed is the explicit fallback on a CPU without a
 * hardware generator.  These are the exact acceptance rules the hidden console
 * prompt uses: 64 hex characters, 32 bytes, at least 16 distinct values and two
 * halves that are not identical.  A rule that accepts a constant would turn
 * the fallback into the substitution the queue forbids.
 */
static void test_operator_seed_rule(void)
{
    uint8_t seed[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_BYTES];
    char text[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS + 2U];
    size_t index;

    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              NULL, 64U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_NULL_ARGUMENT,
          "a null seed text is refused");
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              "", 0U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH,
          "an empty seed text is the wrong length");
    memset(text, '0', sizeof(text));
    text[APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS] = '\0';
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 63U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH,
          "63 hex characters are refused");
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 65U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_WRONG_LENGTH,
          "65 hex characters are refused");
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 64U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY,
          "a constant seed is refused even when it is the right length");
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS;
         ++index) {
        text[index] = "0123456789abcdef"[index % 16U];
    }
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 64U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_LOW_DIVERSITY,
          "two identical halves are refused");
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS;
         ++index) {
        text[index] = "0123456789abcdef"[index % 16U];
    }
    text[0] = 'z';
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 64U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_NOT_HEX,
          "a non-hex character is named as such");
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ENTROPY_OPERATOR_SEED_HEX_CHARS;
         ++index) {
        text[index] = "0123456789abcdef"[index % 16U];
    }
    text[0] = '0';
    text[1] = '1';
    text[2] = '2';
    text[3] = '3';
    text[4] = '4';
    text[5] = '5';
    text[6] = '6';
    text[7] = '7';
    text[8] = '8';
    text[9] = '9';
    text[10] = 'a';
    text[11] = 'b';
    text[12] = 'c';
    text[13] = 'd';
    text[14] = 'e';
    text[15] = 'f';
    text[16] = 'f';
    text[17] = 'e';
    text[18] = 'd';
    text[19] = 'c';
    text[20] = 'b';
    text[21] = 'a';
    text[22] = '9';
    text[23] = '8';
    text[24] = '7';
    text[25] = '6';
    text[26] = '5';
    text[27] = '4';
    text[28] = '3';
    text[29] = '2';
    text[30] = '1';
    text[31] = '0';
    text[32] = '5';
    text[33] = 'a';
    text[34] = '3';
    text[35] = 'c';
    text[36] = '9';
    text[37] = 'e';
    text[38] = '1';
    text[39] = '7';
    text[40] = 'd';
    text[41] = '2';
    text[42] = '4';
    text[43] = '8';
    text[44] = '6';
    text[45] = 'b';
    text[46] = '0';
    text[47] = 'f';
    text[48] = 'c';
    text[49] = '6';
    text[50] = 'a';
    text[51] = '2';
    text[52] = 'e';
    text[53] = '8';
    text[54] = '4';
    text[55] = '0';
    text[56] = 'b';
    text[57] = 'd';
    text[58] = 'f';
    text[59] = '1';
    text[60] = '3';
    text[61] = '5';
    text[62] = '7';
    text[63] = '9';
    CHECK(appliance_production_entropy_operator_seed_status_from_hex(
              text, 64U, seed) ==
              APPLIANCE_PRODUCTION_ENTROPY_SEED_OK,
          "a diverse 256-bit hex seed is accepted");
    CHECK(seed[0] == 0x01U && seed[1] == 0x23U && seed[2] == 0x45U &&
              seed[3] == 0x67U,
          "hex decoding preserves the high nibble first");
    CHECK(appliance_production_entropy_operator_seed_valid(NULL, 32U) ==
              false,
          "a null seed is not valid");
    CHECK(appliance_production_entropy_operator_seed_valid(seed, 31U) ==
              false,
          "a 31-byte seed is not valid");
    memset(seed, 0, sizeof(seed));
    CHECK(appliance_production_entropy_operator_seed_valid(
              seed, sizeof(seed)) == false,
          "an all-zero seed is not valid");
    for (index = 0U; index < sizeof(seed); ++index) {
        seed[index] = (uint8_t)index;
    }
    CHECK(appliance_production_entropy_operator_seed_valid(
              seed, sizeof(seed)) == true,
          "a diverse 32-byte seed is valid");
}

int main(void)
{
    test_entropy_cpuid_profiles();
    test_operator_seed_rule();
    test_strings_and_policy();
    test_snapshot_rejections();
    test_snapshot_mismatches();
    test_ready_snapshot();
    test_state_mapping();
    test_serial_negative_and_too_small();
    test_serial_worst_case_bounds();
    test_audit_payload();
    test_support_negative_and_too_small();

    if (failures != 0) {
        fprintf(stderr, "Entropy diagnostics host tests: FAIL (%d)\n",
                failures);
        return 1;
    }
    printf("Entropy diagnostics host tests: ALL OK\n");
    return 0;
}

#else

int main(void)
{
    return 0;
}

#endif
