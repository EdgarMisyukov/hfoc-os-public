#include "hfoc_mbedtls_drbg.h"

#include "entropy_accumulator.h"
#include "entropy_seed_fill.h"
#include "hfoc_mbedtls_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct hfoc_mbedtls_drbg_config make_config(
    uint32_t purpose_id,
    uint32_t generation,
    uint32_t source_id,
    uint32_t required_credited_bytes,
    uint32_t fill_count_max,
    uint32_t reseed_interval)
{
    struct hfoc_mbedtls_drbg_config config;

    memset(&config, 0, sizeof(config));
    config.version = HFOC_MBEDTLS_DRBG_VERSION;
    config.purpose_id = purpose_id;
    config.generation = generation;
    config.source_id = source_id;
    config.required_credited_bytes = required_credited_bytes;
    config.fill_count_max = fill_count_max;
    config.reseed_interval = reseed_interval;
    return config;
}

static void reset_drbg(struct hfoc_mbedtls_drbg *drbg)
{
    memset(drbg, 0, sizeof(*drbg));
}

static void fill_accumulator(
    struct entropy_accumulator *accumulator,
    uint32_t generation,
    uint32_t source_id,
    uint32_t credited_bytes)
{
    struct entropy_accumulator_config config;
    uint8_t block[ENTROPY_ACCUMULATOR_CAPACITY];
    uint32_t index;

    memset(accumulator, 0, sizeof(*accumulator));
    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_ACCUMULATOR_VERSION;
    config.generation = generation;
    config.source_id = source_id;
    config.block_length_min = 1U;
    config.block_length_max = ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX;
    config.max_stall_ticks = 100U;

    CHECK(entropy_accumulator_init(
              accumulator, &config, 10U) ==
              ENTROPY_ACCUMULATOR_OK,
          "DRBG fixture publishes accumulator");

    for (index = 0U; index < credited_bytes; ++index) {
        block[index] = (uint8_t)(
            0x41U + (index * 29U) + generation + source_id);
    }
    CHECK(entropy_accumulator_mix(
              accumulator,
              generation,
              source_id,
              block,
              credited_bytes,
              20U) == ENTROPY_ACCUMULATOR_OK,
          "DRBG fixture credits accumulator bytes");
}

static bool all_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool not_all_zero(const void *object, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        if (bytes[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool snapshot_equal(
    const struct hfoc_mbedtls_alloc_snapshot *left,
    const struct hfoc_mbedtls_alloc_snapshot *right)
{
    return left != NULL &&
           right != NULL &&
           left->total_used_bytes == right->total_used_bytes &&
           left->total_free_bytes == right->total_free_bytes &&
           left->total_high_water_bytes == right->total_high_water_bytes;
}

static void test_strings_and_unpublished(void)
{
    struct hfoc_mbedtls_drbg drbg;
    uint32_t status;
    uint32_t state;
    uint32_t fault;

    for (status = 0U;
         status <= HFOC_MBEDTLS_DRBG_CONTEXT_STORAGE_OVERFLOW;
         ++status) {
        CHECK(hfoc_mbedtls_drbg_status_string(
                  (enum hfoc_mbedtls_drbg_status)status) != NULL,
              "DRBG status string is defined for every status");
    }
    CHECK(hfoc_mbedtls_drbg_status_string(
              (enum hfoc_mbedtls_drbg_status)999U) == NULL,
          "DRBG undefined status string is rejected");

    for (state = 0U; state <= HFOC_MBEDTLS_DRBG_STATE_FAILED; ++state) {
        CHECK(hfoc_mbedtls_drbg_state_string(
                  (enum hfoc_mbedtls_drbg_state)state) != NULL,
              "DRBG state string is defined for every state");
    }
    CHECK(hfoc_mbedtls_drbg_state_string(
              (enum hfoc_mbedtls_drbg_state)99U) == NULL,
          "DRBG undefined state string is rejected");

    for (fault = 0U;
         fault <= HFOC_MBEDTLS_DRBG_FAULT_CONTEXT_NOT_PUBLISHED;
         ++fault) {
        CHECK(hfoc_mbedtls_drbg_fault_string(
                  (enum hfoc_mbedtls_drbg_fault)fault) != NULL,
              "DRBG fault string is defined for every fault");
    }
    CHECK(hfoc_mbedtls_drbg_fault_string(
              (enum hfoc_mbedtls_drbg_fault)99U) == NULL,
          "DRBG undefined fault string is rejected");

    reset_drbg(&drbg);
    CHECK(!hfoc_mbedtls_drbg_initialized(&drbg),
          "unpublished DRBG is not initialized");
    CHECK(!hfoc_mbedtls_drbg_published(&drbg),
          "unpublished DRBG is not published");
    CHECK(hfoc_mbedtls_drbg_state(&drbg) ==
              HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED,
          "unpublished DRBG state is uninitialized");
    CHECK(hfoc_mbedtls_drbg_fault(&drbg) ==
              HFOC_MBEDTLS_DRBG_FAULT_NONE,
          "unpublished DRBG fault is none");
    CHECK(hfoc_mbedtls_drbg_purpose_id(&drbg) == 0U &&
              hfoc_mbedtls_drbg_generation(&drbg) == 0U &&
              hfoc_mbedtls_drbg_source_id(&drbg) == 0U &&
              hfoc_mbedtls_drbg_fill_count(&drbg) == 0U &&
              hfoc_mbedtls_drbg_random_calls(&drbg) == 0U,
          "unpublished DRBG accessors are conservative");
    CHECK(hfoc_mbedtls_drbg_publish(&drbg, NULL) ==
              HFOC_MBEDTLS_DRBG_BAD_STATE,
          "unpublished DRBG publish fails closed");
}

static void test_init_validation(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);

    CHECK(hfoc_mbedtls_drbg_init(NULL, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_NULL,
          "DRBG init rejects null object");
    CHECK(hfoc_mbedtls_drbg_init(&drbg, NULL, &accumulator) ==
              HFOC_MBEDTLS_DRBG_NULL,
          "DRBG init rejects null config");

    reset_drbg(&drbg);
    config.version = 0U;
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_VERSION,
          "DRBG init rejects bad version");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    config.purpose_id = 0U;
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_PURPOSE,
          "DRBG init rejects zero purpose");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    config.purpose_id = UINT32_C(0xFFFFFFFF);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_PURPOSE,
          "DRBG init rejects over-limit purpose");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    config.generation = 0U;
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_GENERATION,
          "DRBG init rejects zero generation");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    config.source_id = 0U;
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_SOURCE_ID,
          "DRBG init rejects zero source id");
    config = make_config(1U, 1U, 7U, 0U, 4U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES,
          "DRBG init rejects zero required bytes");
    config = make_config(1U, 1U, 7U, 4097U, 4U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_REQUIRED_BYTES,
          "DRBG init rejects over-limit required bytes");
    config = make_config(1U, 1U, 7U, 4096U, 0U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT,
          "DRBG init rejects zero fill count");
    config = make_config(1U, 1U, 7U, 4096U, 65U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_FILL_COUNT,
          "DRBG init rejects over-limit fill count");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 0U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL,
          "DRBG init rejects zero reseed interval");
    config = make_config(1U, 1U, 7U, 4096U, 4U, 1001U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_BAD_RESEED_INTERVAL,
          "DRBG init rejects over-limit reseed interval");
}

static void test_init_states(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config;

    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, NULL) ==
              HFOC_MBEDTLS_DRBG_OK,
          "DRBG init publishes unavailable with null accumulator");
    CHECK(hfoc_mbedtls_drbg_initialized(&drbg) &&
              hfoc_mbedtls_drbg_state(&drbg) ==
                  HFOC_MBEDTLS_DRBG_STATE_UNAVAILABLE &&
              hfoc_mbedtls_drbg_fault(&drbg) ==
                  HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_UNAVAILABLE,
          "unavailable DRBG state and fault are exact");

    fill_accumulator(&accumulator, 1U, 7U, 32U);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "DRBG init publishes degraded with low credit");
    CHECK(hfoc_mbedtls_drbg_state(&drbg) ==
              HFOC_MBEDTLS_DRBG_STATE_DEGRADED &&
              hfoc_mbedtls_drbg_fault(&drbg) ==
                  HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_DEGRADED,
          "degraded DRBG state and fault are exact");

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "DRBG init publishes ready with sufficient credit");
    CHECK(hfoc_mbedtls_drbg_state(&drbg) ==
              HFOC_MBEDTLS_DRBG_STATE_READY &&
              hfoc_mbedtls_drbg_fault(&drbg) ==
                  HFOC_MBEDTLS_DRBG_FAULT_NONE &&
              hfoc_mbedtls_drbg_generation(&drbg) == 1U &&
              hfoc_mbedtls_drbg_source_id(&drbg) == 7U &&
              hfoc_mbedtls_drbg_purpose_id(&drbg) == 1U,
          "ready DRBG publishes exact identity");
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED,
          "DRBG init rejects duplicate publication");

    fill_accumulator(&accumulator, 2U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH,
          "DRBG init rejects generation mismatch");
    CHECK(!hfoc_mbedtls_drbg_initialized(&drbg),
          "generation-mismatched init publishes nothing");

    fill_accumulator(&accumulator, 1U, 8U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    config = make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_SOURCE_MISMATCH,
          "DRBG init rejects source mismatch");
    CHECK(!hfoc_mbedtls_drbg_initialized(&drbg),
          "source-mismatched init publishes nothing");
}

static void test_publish_gates(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);

    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, NULL) ==
              HFOC_MBEDTLS_DRBG_OK,
          "publish gate uses unavailable DRBG");
    CHECK(hfoc_mbedtls_drbg_publish(&drbg, NULL) ==
              HFOC_MBEDTLS_DRBG_ENTROPY_UNAVAILABLE &&
              !hfoc_mbedtls_drbg_published(&drbg),
          "unavailable entropy prevents DRBG publication");

    fill_accumulator(&accumulator, 1U, 7U, 32U);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "publish gate uses degraded DRBG");
    CHECK(hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_ENTROPY_DEGRADED &&
              !hfoc_mbedtls_drbg_published(&drbg) &&
              hfoc_mbedtls_drbg_fill_count(&drbg) == 0U,
          "degraded entropy prevents DRBG publication");

    CHECK(hfoc_mbedtls_drbg_publish(NULL, &accumulator) ==
              HFOC_MBEDTLS_DRBG_NULL,
          "DRBG publish rejects null object");
}

static void test_publish_generate_reseed(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    uint8_t output[HFOC_MBEDTLS_DRBG_RANDOM_MAX];
    uint32_t written = 0U;
    enum hfoc_mbedtls_drbg_status status;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "publish fixture is ready");
    CHECK(hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "ready entropy publishes DRBG");
    CHECK(hfoc_mbedtls_drbg_published(&drbg) &&
              hfoc_mbedtls_drbg_state(&drbg) ==
                  HFOC_MBEDTLS_DRBG_STATE_PUBLISHED &&
              hfoc_mbedtls_drbg_fill_count(&drbg) == 0U,
          "published DRBG state is exact");
    CHECK(hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED,
          "duplicate DRBG publish is rejected");

    memset(output, 0, sizeof(output));
    status = hfoc_mbedtls_drbg_generate(
        &drbg, &accumulator, output, sizeof(output), 32U, &written);
    CHECK(status == HFOC_MBEDTLS_DRBG_OK &&
              written == 32U &&
              hfoc_mbedtls_drbg_random_calls(&drbg) == 1U &&
              not_all_zero(output, 32U),
          "published DRBG generates bounded random bytes");

    written = 0U;
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, 16U, 32U, &written) ==
              HFOC_MBEDTLS_DRBG_OUTPUT_TOO_SMALL,
          "DRBG generate rejects small output");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, sizeof(output), 0U, &written) ==
              HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW,
          "DRBG generate rejects zero length");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, sizeof(output),
              HFOC_MBEDTLS_DRBG_RANDOM_MAX + 1U, &written) ==
              HFOC_MBEDTLS_DRBG_LENGTH_OVERFLOW,
          "DRBG generate rejects over-limit length");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, NULL, sizeof(output), 32U, &written) ==
              HFOC_MBEDTLS_DRBG_NULL,
          "DRBG generate rejects null output");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, drbg.context_storage,
              sizeof(drbg.context_storage), 16U, &written) ==
              HFOC_MBEDTLS_DRBG_OUTPUT_OVERLAP,
          "DRBG generate rejects output overlap with adapter");

    CHECK(hfoc_mbedtls_drbg_reseed(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_fill_count(&drbg) == 1U &&
              hfoc_mbedtls_drbg_published(&drbg),
          "DRBG reseed consumes one bounded fill");

    hfoc_mbedtls_drbg_shutdown(&drbg);
    written = 0U;
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, sizeof(output), 32U, &written) ==
              HFOC_MBEDTLS_DRBG_NOT_PUBLISHED,
          "shutdown DRBG cannot generate");
}

static void test_entropy_rejection_invalidates(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 1U, 1U);
    uint8_t output[32U];
    uint32_t written = 0U;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "rejection fixture publishes DRBG");
    CHECK(hfoc_mbedtls_drbg_reseed(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK,
          "first reseed consumes the only fill");
    CHECK(hfoc_mbedtls_drbg_reseed(&drbg, &accumulator) ==
              HFOC_MBEDTLS_DRBG_ENTROPY_REJECTED,
          "exhausted reseed rejects entropy");
    CHECK(!hfoc_mbedtls_drbg_published(&drbg) &&
              hfoc_mbedtls_drbg_state(&drbg) ==
                  HFOC_MBEDTLS_DRBG_STATE_FAILED &&
              hfoc_mbedtls_drbg_fault(&drbg) ==
                  HFOC_MBEDTLS_DRBG_FAULT_ENTROPY_REJECTED,
          "rejected entropy invalidates published DRBG");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, sizeof(output), 32U, &written) ==
              HFOC_MBEDTLS_DRBG_NOT_PUBLISHED,
          "failed DRBG cannot generate after entropy rejection");
}

static void test_generation_invalidation(void)
{
    struct entropy_accumulator accumulator;
    struct entropy_accumulator next_accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    uint8_t output[32U];
    uint32_t written = 0U;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "invalidation fixture publishes DRBG");
    CHECK(hfoc_mbedtls_drbg_invalidate(&drbg, 1U) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_published(&drbg),
          "same-generation invalidate is a no-op");
    CHECK(hfoc_mbedtls_drbg_invalidate(&drbg, 2U) ==
              HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH &&
              !hfoc_mbedtls_drbg_published(&drbg) &&
              hfoc_mbedtls_drbg_fault(&drbg) ==
                  HFOC_MBEDTLS_DRBG_FAULT_GENERATION_MISMATCH,
          "new-generation invalidate closes DRBG");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &accumulator, output, sizeof(output), 32U, &written) ==
              HFOC_MBEDTLS_DRBG_NOT_PUBLISHED,
          "invalidated DRBG cannot generate");

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    fill_accumulator(
        &next_accumulator, 2U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "stale-generation fixture publishes DRBG");
    CHECK(hfoc_mbedtls_drbg_generate(
              &drbg, &next_accumulator, output, sizeof(output), 32U,
              &written) == HFOC_MBEDTLS_DRBG_GENERATION_MISMATCH &&
              !hfoc_mbedtls_drbg_published(&drbg),
          "stale accumulator generation invalidates DRBG");
}

static void test_purpose_separation(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg first;
    struct hfoc_mbedtls_drbg second;
    struct hfoc_mbedtls_drbg_config first_config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    struct hfoc_mbedtls_drbg_config second_config =
        make_config(2U, 1U, 7U, 4096U, 4U, 2U);
    uint8_t first_output[32U];
    uint8_t second_output[32U];
    uint32_t first_written = 0U;
    uint32_t second_written = 0U;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&first);
    reset_drbg(&second);
    CHECK(hfoc_mbedtls_drbg_init(
              &first, &first_config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&first, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "first purpose DRBG publishes");
    CHECK(hfoc_mbedtls_drbg_init(
              &second, &second_config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&second, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "second purpose DRBG publishes");
    CHECK(hfoc_mbedtls_drbg_purpose_id(&first) == 1U &&
              hfoc_mbedtls_drbg_purpose_id(&second) == 2U &&
              hfoc_mbedtls_drbg_published(&first) &&
              hfoc_mbedtls_drbg_published(&second),
          "purpose identities are distinct and bound");
    CHECK(hfoc_mbedtls_drbg_init(
              &first, &second_config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_ALREADY_PUBLISHED,
          "published purpose DRBG cannot be repurposed");
    CHECK(hfoc_mbedtls_drbg_generate(
              &first, &accumulator, first_output, sizeof(first_output),
              32U, &first_written) == HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_generate(
                  &second, &accumulator, second_output,
                  sizeof(second_output), 32U, &second_written) ==
                  HFOC_MBEDTLS_DRBG_OK,
          "both purpose DRBGs generate independently");
}

static void test_accumulator_pointer_rebinding(void)
{
    struct entropy_accumulator first;
    struct entropy_accumulator second;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    uint8_t block[1U];

    fill_accumulator(
        &first, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    fill_accumulator(
        &second, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(
              &drbg, &config, &first) == HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(
                  &drbg, &second) == HFOC_MBEDTLS_DRBG_OK,
          "DRBG publish rebinds to the supplied accumulator");

    block[0] = 0x11U;
    CHECK(entropy_accumulator_mix(
              &first,
              99U,
              7U,
              block,
              sizeof(block),
              30U) == ENTROPY_ACCUMULATOR_GENERATION_MISMATCH,
          "DRBG fixture invalidates the old accumulator only");
    CHECK(hfoc_mbedtls_drbg_reseed(
              &drbg, &second) == HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_published(&drbg),
          "DRBG entropy callback uses the rebound accumulator");
}

static void test_shutdown_zeroizes(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    uint8_t output[32U];
    uint32_t written = 0U;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_generate(
                  &drbg, &accumulator, output, sizeof(output), 32U,
                  &written) == HFOC_MBEDTLS_DRBG_OK,
          "shutdown fixture publishes and generates");

    hfoc_mbedtls_drbg_shutdown(&drbg);
    CHECK(!hfoc_mbedtls_drbg_initialized(&drbg) &&
              !hfoc_mbedtls_drbg_published(&drbg) &&
              hfoc_mbedtls_drbg_state(&drbg) ==
                  HFOC_MBEDTLS_DRBG_STATE_UNINITIALIZED &&
              all_zero(
                  drbg.context_storage,
                  sizeof(drbg.context_storage)) &&
              !drbg.seed_fill.initialized,
          "shutdown zeroizes adapter and DRBG context storage");
    hfoc_mbedtls_drbg_shutdown(NULL);
}

static void test_allocator_balance(void)
{
    struct entropy_accumulator accumulator;
    struct hfoc_mbedtls_drbg drbg;
    struct hfoc_mbedtls_drbg_config config =
        make_config(1U, 1U, 7U, 4096U, 4U, 2U);
    struct hfoc_mbedtls_alloc_snapshot before;
    struct hfoc_mbedtls_alloc_snapshot after;
    uint8_t output[32U];
    uint32_t written = 0U;

    fill_accumulator(&accumulator, 1U, 7U, ENTROPY_ACCUMULATOR_CAPACITY);
    before = hfoc_mbedtls_alloc_snapshot();
    reset_drbg(&drbg);
    CHECK(hfoc_mbedtls_drbg_init(&drbg, &config, &accumulator) ==
              HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_publish(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_reseed(&drbg, &accumulator) ==
                  HFOC_MBEDTLS_DRBG_OK &&
              hfoc_mbedtls_drbg_generate(
                  &drbg, &accumulator, output, sizeof(output), 32U,
                  &written) == HFOC_MBEDTLS_DRBG_OK,
          "allocator balance fixture completes");
    hfoc_mbedtls_drbg_shutdown(&drbg);
    after = hfoc_mbedtls_alloc_snapshot();
    CHECK(snapshot_equal(&before, &after),
          "DRBG adapter leaves allocator counts balanced");
}

int main(void)
{
    hfoc_mbedtls_alloc_shutdown();
    CHECK(hfoc_mbedtls_alloc_init(),
          "DRBG host fixture initializes mbedTLS allocator");

    test_strings_and_unpublished();
    test_init_validation();
    test_init_states();
    test_publish_gates();
    test_publish_generate_reseed();
    test_entropy_rejection_invalidates();
    test_generation_invalidation();
    test_purpose_separation();
    test_accumulator_pointer_rebinding();
    test_shutdown_zeroizes();
    test_allocator_balance();

    hfoc_mbedtls_alloc_shutdown();
    if (failures != 0) {
        fprintf(stderr, "mbedTLS DRBG adapter host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("mbedTLS DRBG adapter host tests: ALL OK\n");
    return 0;
}
