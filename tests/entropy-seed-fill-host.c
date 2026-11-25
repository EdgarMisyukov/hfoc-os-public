#include "entropy_seed_fill.h"

#include "entropy_accumulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ENTROPY_SEED_FILL_HOST_TEST

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

static struct entropy_seed_fill_config make_config(
    uint32_t generation,
    uint32_t source_id,
    uint32_t required_credited_bytes,
    uint32_t seed_length,
    uint32_t fill_length_max,
    uint32_t fill_count_max)
{
    struct entropy_seed_fill_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_SEED_FILL_VERSION;
    config.generation = generation;
    config.source_id = source_id;
    config.required_credited_bytes = required_credited_bytes;
    config.seed_length = seed_length;
    config.fill_length_max = fill_length_max;
    config.fill_count_max = fill_count_max;
    return config;
}

static struct entropy_accumulator_config make_accumulator_config(
    uint32_t generation,
    uint32_t source_id)
{
    struct entropy_accumulator_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_ACCUMULATOR_VERSION;
    config.generation = generation;
    config.source_id = source_id;
    config.block_length_min = 1U;
    config.block_length_max = ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX;
    config.max_stall_ticks = 20U;
    return config;
}

static void fill_block(
    uint8_t *block,
    uint32_t block_length,
    uint8_t seed)
{
    uint32_t index;

    for (index = 0U; index < block_length; ++index) {
        block[index] = (uint8_t)(seed + index);
    }
}

static void init_ready_accumulator(
    struct entropy_accumulator *accumulator,
    uint32_t generation,
    uint32_t source_id,
    uint32_t credited_length)
{
    struct entropy_accumulator_config config =
        make_accumulator_config(generation, source_id);
    uint8_t block[128];

    memset(accumulator, 0, sizeof(*accumulator));
    fill_block(block, credited_length, 0x51U);
    CHECK(entropy_accumulator_init(
              accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_mix(
                  accumulator,
                  generation,
                  source_id,
                  block,
                  credited_length,
                  15U) == ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_accumulating(accumulator),
          "seed/fill fixture publishes a ready entropy accumulator");
}

static bool controller_state_is(
    const struct entropy_seed_fill *controller,
    enum entropy_seed_fill_state state,
    enum entropy_seed_fill_fault fault)
{
    return entropy_seed_fill_initialized(controller) &&
           entropy_seed_fill_state(controller) == state &&
           entropy_seed_fill_fault(controller) == fault;
}

static void expect_no_allocation(const char *message)
{
    CHECK(alloc_attempts == 0U && free_attempts == 0U, message);
}

static void reset_fixture(void)
{
    alloc_attempts = 0U;
    free_attempts = 0U;
}

static void test_status_strings(void)
{
    static const struct {
        enum entropy_seed_fill_status status;
        const char *string;
    } statuses[] = {
        {ENTROPY_SEED_FILL_OK, "ENTROPY_SEED_FILL_OK"},
        {ENTROPY_SEED_FILL_NULL, "ENTROPY_SEED_FILL_NULL"},
        {ENTROPY_SEED_FILL_BAD_STATE, "ENTROPY_SEED_FILL_BAD_STATE"},
        {ENTROPY_SEED_FILL_ALREADY_PUBLISHED,
         "ENTROPY_SEED_FILL_ALREADY_PUBLISHED"},
        {ENTROPY_SEED_FILL_BAD_VERSION,
         "ENTROPY_SEED_FILL_BAD_VERSION"},
        {ENTROPY_SEED_FILL_BAD_GENERATION,
         "ENTROPY_SEED_FILL_BAD_GENERATION"},
        {ENTROPY_SEED_FILL_BAD_SOURCE_ID,
         "ENTROPY_SEED_FILL_BAD_SOURCE_ID"},
        {ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES,
         "ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES"},
        {ENTROPY_SEED_FILL_BAD_SEED_LENGTH,
         "ENTROPY_SEED_FILL_BAD_SEED_LENGTH"},
        {ENTROPY_SEED_FILL_BAD_FILL_LENGTH,
         "ENTROPY_SEED_FILL_BAD_FILL_LENGTH"},
        {ENTROPY_SEED_FILL_BAD_FILL_COUNT,
         "ENTROPY_SEED_FILL_BAD_FILL_COUNT"},
        {ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY,
         "ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY"},
        {ENTROPY_SEED_FILL_GENERATION_MISMATCH,
         "ENTROPY_SEED_FILL_GENERATION_MISMATCH"},
        {ENTROPY_SEED_FILL_SOURCE_MISMATCH,
         "ENTROPY_SEED_FILL_SOURCE_MISMATCH"},
        {ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES,
         "ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES"},
        {ENTROPY_SEED_FILL_OUTPUT_OVERLAP,
         "ENTROPY_SEED_FILL_OUTPUT_OVERLAP"},
        {ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL,
         "ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL"},
        {ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED,
         "ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED"},
        {ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED,
         "ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED"},
        {ENTROPY_SEED_FILL_FILL_EXHAUSTED,
         "ENTROPY_SEED_FILL_FILL_EXHAUSTED"},
        {ENTROPY_SEED_FILL_LENGTH_OVERFLOW,
         "ENTROPY_SEED_FILL_LENGTH_OVERFLOW"},
    };
    static const struct {
        enum entropy_seed_fill_state state;
        const char *string;
    } states[] = {
        {ENTROPY_SEED_FILL_STATE_UNINITIALIZED,
         "ENTROPY_SEED_FILL_STATE_UNINITIALIZED"},
        {ENTROPY_SEED_FILL_STATE_UNAVAILABLE,
         "ENTROPY_SEED_FILL_STATE_UNAVAILABLE"},
        {ENTROPY_SEED_FILL_STATE_DEGRADED,
         "ENTROPY_SEED_FILL_STATE_DEGRADED"},
        {ENTROPY_SEED_FILL_STATE_READY,
         "ENTROPY_SEED_FILL_STATE_READY"},
        {ENTROPY_SEED_FILL_STATE_FAILED,
         "ENTROPY_SEED_FILL_STATE_FAILED"},
    };
    static const struct {
        enum entropy_seed_fill_fault fault;
        const char *string;
    } faults[] = {
        {ENTROPY_SEED_FILL_FAULT_NONE,
         "ENTROPY_SEED_FILL_FAULT_NONE"},
        {ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE,
         "ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE"},
        {ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH,
         "ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH"},
        {ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH,
         "ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH"},
        {ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES,
         "ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES"},
        {ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP,
         "ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP"},
        {ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW,
         "ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW"},
        {ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED,
         "ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED"},
    };
    size_t index;

    reset_fixture();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = entropy_seed_fill_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "seed/fill status string is exact");
    }
    CHECK(entropy_seed_fill_status_string(
              (enum entropy_seed_fill_status)99U) == NULL,
          "seed/fill rejects undefined status string");

    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        const char *string = entropy_seed_fill_state_string(
            states[index].state);

        CHECK(string != NULL &&
                  strcmp(string, states[index].string) == 0,
              "seed/fill state string is exact");
    }
    CHECK(entropy_seed_fill_state_string(
              (enum entropy_seed_fill_state)99U) == NULL,
          "seed/fill rejects undefined state string");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = entropy_seed_fill_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "seed/fill fault string is exact");
    }
    CHECK(entropy_seed_fill_fault_string(
              (enum entropy_seed_fill_fault)99U) == NULL,
          "seed/fill rejects undefined fault string");
    expect_no_allocation(
        "seed/fill strings perform no allocation");
}

static void test_init_rejections(void)
{
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config config;

    memset(&controller, 0xA5, sizeof(controller));
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);

    reset_fixture();
    CHECK(entropy_seed_fill_init(
              NULL, &config, NULL) == ENTROPY_SEED_FILL_NULL,
          "seed/fill init rejects null controller");
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, NULL, NULL) == ENTROPY_SEED_FILL_NULL,
          "seed/fill init rejects null config");

    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    config.version = 0U;
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_VERSION &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_UNINITIALIZED,
          "seed/fill init rejects bad config version");

    config = make_config(0U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_GENERATION,
          "seed/fill init rejects zero generation");

    config = make_config(
        ENTROPY_SEED_FILL_GENERATION_MAX + 1U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_GENERATION,
          "seed/fill init rejects over-limit generation");

    config = make_config(7U, 0U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_SOURCE_ID,
          "seed/fill init rejects zero source identity");

    config = make_config(
        7U, ENTROPY_SEED_FILL_SOURCE_ID_MAX + 1U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_SOURCE_ID,
          "seed/fill init rejects over-limit source identity");

    config = make_config(7U, 42U, 0U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES,
          "seed/fill init rejects zero required bytes");

    config = make_config(
        7U, 42U, ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX + 1U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES,
          "seed/fill init rejects over-limit required bytes");

    config = make_config(7U, 42U, 32U, 0U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_SEED_LENGTH,
          "seed/fill init rejects zero seed length");

    config = make_config(
        7U, 42U, 32U, ENTROPY_SEED_FILL_SEED_LENGTH_MAX + 1U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_SEED_LENGTH,
          "seed/fill init rejects over-limit seed length");

    config = make_config(7U, 42U, 32U, 32U, 0U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_FILL_LENGTH,
          "seed/fill init rejects zero fill length");

    config = make_config(
        7U, 42U, 32U, 32U, ENTROPY_SEED_FILL_FILL_LENGTH_MAX + 1U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_FILL_LENGTH,
          "seed/fill init rejects over-limit fill length");

    config = make_config(7U, 42U, 32U, 32U, 8U, 0U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_FILL_COUNT,
          "seed/fill init rejects zero fill count");

    config = make_config(
        7U, 42U, 32U, 32U, 8U, ENTROPY_SEED_FILL_FILL_COUNT_MAX + 1U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_BAD_FILL_COUNT,
          "seed/fill init rejects over-limit fill count");

    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) == ENTROPY_SEED_FILL_OK &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_UNAVAILABLE,
          "seed/fill init publishes unavailable for missing accumulator");
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) ==
              ENTROPY_SEED_FILL_ALREADY_PUBLISHED,
          "duplicate seed/fill init is rejected without mutation");
    expect_no_allocation(
        "seed/fill init rejection paths perform no allocation");
}

static void test_init_states(void)
{
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config config;
    struct entropy_accumulator accumulator;
    struct entropy_accumulator other;
    struct entropy_accumulator_config accumulator_config;
    uint8_t block[128];

    reset_fixture();

    memset(&controller, 0, sizeof(controller));
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    CHECK(entropy_seed_fill_init(
              &controller, &config, NULL) == ENTROPY_SEED_FILL_OK &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_UNAVAILABLE,
              ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE),
          "seed/fill init publishes unavailable for null accumulator");

    accumulator_config = make_accumulator_config(7U, 42U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &accumulator_config, 10U) ==
              ENTROPY_ACCUMULATOR_OK,
          "non-accumulating fixture initializes accumulator");
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_UNAVAILABLE,
              ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE),
          "seed/fill init publishes unavailable for non-accumulating source");

    init_ready_accumulator(&accumulator, 7U, 42U, 8U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_DEGRADED,
              ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES),
          "seed/fill init publishes degraded below credited threshold");

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_READY,
              ENTROPY_SEED_FILL_FAULT_NONE) &&
          entropy_seed_fill_ready(&controller) &&
          entropy_seed_fill_generation(&controller) == 7U &&
          entropy_seed_fill_source_id(&controller) == 42U,
          "seed/fill init publishes ready above credited threshold");

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    config = make_config(8U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) ==
              ENTROPY_SEED_FILL_GENERATION_MISMATCH &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_UNINITIALIZED,
          "seed/fill init rejects generation mismatch without publication");

    config = make_config(7U, 43U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) ==
              ENTROPY_SEED_FILL_SOURCE_MISMATCH &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_UNINITIALIZED,
          "seed/fill init rejects source mismatch without publication");

    init_ready_accumulator(&other, 7U, 42U, 64U);
    (void)other;
    (void)block;
    expect_no_allocation(
        "seed/fill init state paths perform no allocation");
}

static void test_valid_seed_and_fill(void)
{
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config config;
    struct entropy_accumulator accumulator;
    uint8_t output[64];
    uint8_t fill[64];
    uint32_t written;

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    memset(output, 0xEE, sizeof(output));
    memset(fill, 0xEE, sizeof(fill));

    reset_fixture();
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "valid seed/fill fixture initializes ready controller");
    written = 0U;
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_OK &&
          written == 32U &&
          memcmp(output, accumulator.bytes, 32U) == 0 &&
          entropy_seed_fill_seed_published(&controller) &&
          entropy_seed_fill_seed_material_bytes(&controller) == 32U &&
          entropy_seed_fill_fill_count(&controller) == 0U &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_READY,
              ENTROPY_SEED_FILL_FAULT_NONE),
          "valid seed copies exact accumulator seed bytes once");

    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              fill,
              (uint32_t)sizeof(fill),
              &written) == ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED &&
          written == 32U &&
          entropy_seed_fill_seed_material_bytes(&controller) == 32U,
          "repeated seed is rejected without consuming more material");

    written = 0U;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_OK &&
          written == 8U &&
          memcmp(output, accumulator.bytes + 32U, 8U) == 0 &&
          entropy_seed_fill_fill_count(&controller) == 1U &&
          controller_state_is(
              &controller,
              ENTROPY_SEED_FILL_STATE_READY,
              ENTROPY_SEED_FILL_FAULT_NONE),
          "valid fill copies next bounded accumulator material");

    written = 0U;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_OK &&
          written == 8U &&
          memcmp(output, accumulator.bytes + 40U, 8U) == 0 &&
          entropy_seed_fill_fill_count(&controller) == 2U,
          "second valid fill advances material offset exactly");

    written = 0U;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_OK &&
          written == 8U &&
          memcmp(output, accumulator.bytes + 48U, 8U) == 0 &&
          entropy_seed_fill_fill_count(&controller) == 3U,
          "third valid fill consumes offset 48");

    written = 0U;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_OK &&
          written == 8U &&
          memcmp(output, accumulator.bytes + 56U, 8U) == 0 &&
          entropy_seed_fill_fill_count(&controller) == 4U,
          "fourth valid fill reaches configured count");

    written = 0xDEADU;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_FILL_EXHAUSTED &&
          written == 0xDEADU &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_DEGRADED &&
          entropy_seed_fill_fault(&controller) ==
              ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED,
          "fill-count exhaustion is rejected without output mutation");
    expect_no_allocation(
        "seed/fill valid and exhausted paths perform no allocation");
}

static void test_fill_rejections(void)
{
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config config;
    struct entropy_accumulator accumulator;
    uint8_t output[64];
    uint32_t written;

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    memset(output, 0xEE, sizeof(output));

    reset_fixture();
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "fill-rejection fixture initializes controller");
    written = 0U;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED &&
          written == 0U,
          "fill before seed is rejected");

    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_OK,
          "fill-rejection fixture publishes seed");

    written = 0xDEADU;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              7U,
              8U,
              &written) == ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL &&
          written == 0xDEADU,
          "fill with small output is rejected without mutation");

    written = 0xDEADU;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              0U,
              &written) == ENTROPY_SEED_FILL_LENGTH_OVERFLOW &&
          written == 0xDEADU,
          "fill with zero requested length is rejected");

    written = 0xDEADU;
    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              9U,
              &written) == ENTROPY_SEED_FILL_LENGTH_OVERFLOW &&
          written == 0xDEADU,
          "fill with over-limit requested length is rejected");

    CHECK(entropy_seed_fill_fill(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              8U,
              NULL) == ENTROPY_SEED_FILL_NULL,
          "fill with null bytes-written is rejected");
    expect_no_allocation(
        "fill rejection paths perform no allocation");
}

static void test_fail_closed_paths(void)
{
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config config;
    struct entropy_accumulator accumulator;
    struct entropy_accumulator other;
    uint8_t output[64];
    uint32_t written;

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    memset(output, 0xEE, sizeof(output));

    reset_fixture();
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "fail-closed fixture initializes ready controller");
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_OK,
          "fail-closed fixture publishes seed");

    written = 0xDEADU;
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              NULL,
              32U,
              &written) == ENTROPY_SEED_FILL_NULL &&
          written == 0xDEADU,
          "seed with null output is rejected");

    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "seed-size fixture initializes controller");
    written = 0xDEADU;
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              31U,
              &written) == ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL &&
          written == 0xDEADU,
          "seed with small output is rejected without mutation");

    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "overlap fixture initializes controller");
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              &controller,
              (uint32_t)sizeof(controller),
              &written) == ENTROPY_SEED_FILL_OUTPUT_OVERLAP &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_FAILED &&
          entropy_seed_fill_fault(&controller) ==
              ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP,
          "seed overlapping controller fails closed");

    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "stale-accumulator fixture initializes controller");
    CHECK(entropy_accumulator_progress(
              &accumulator, 36U) == ENTROPY_ACCUMULATOR_STALLED,
          "stale-accumulator fixture fails accumulator");
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_UNAVAILABLE &&
          entropy_seed_fill_fault(&controller) ==
              ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE,
          "seed with failed accumulator publishes unavailable");

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "generation-mismatch fixture initializes controller");
    init_ready_accumulator(&other, 8U, 42U, 64U);
    CHECK(entropy_seed_fill_seed(
              &controller,
              &other,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_GENERATION_MISMATCH &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_FAILED &&
          entropy_seed_fill_fault(&controller) ==
              ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH,
          "seed generation mismatch fails closed");

    init_ready_accumulator(&accumulator, 7U, 42U, 64U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK,
          "source-mismatch fixture initializes controller");
    init_ready_accumulator(&other, 7U, 43U, 64U);
    CHECK(entropy_seed_fill_seed(
              &controller,
              &other,
              output,
              (uint32_t)sizeof(output),
              &written) == ENTROPY_SEED_FILL_SOURCE_MISMATCH &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_FAILED &&
          entropy_seed_fill_fault(&controller) ==
              ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH,
          "seed source mismatch fails closed");

    CHECK(entropy_seed_fill_fill(
              &controller,
              &other,
              output,
              (uint32_t)sizeof(output),
              8U,
              &written) == ENTROPY_SEED_FILL_BAD_STATE,
          "failed controller rejects later fill");

    init_ready_accumulator(&accumulator, 7U, 42U, 8U);
    config = make_config(7U, 42U, 32U, 32U, 8U, 4U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &config, &accumulator) == ENTROPY_SEED_FILL_OK &&
          entropy_seed_fill_state(&controller) ==
              ENTROPY_SEED_FILL_STATE_DEGRADED,
          "degraded fixture publishes below threshold");
    CHECK(entropy_seed_fill_seed(
              &controller,
              &accumulator,
              output,
              (uint32_t)sizeof(output),
              &written) ==
              ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES,
          "seed below credited threshold is rejected");
    expect_no_allocation(
        "seed/fill fail-closed paths perform no allocation");
}

int main(void)
{
    failures = 0;
    test_status_strings();
    test_init_rejections();
    test_init_states();
    test_valid_seed_and_fill();
    test_fill_rejections();
    test_fail_closed_paths();

    if (failures != 0) {
        fprintf(stderr, "Entropy seed/fill host tests: FAILED\n");
        return 1;
    }
    printf("Entropy seed/fill host tests: ALL OK\n");
    return 0;
}

#endif
