#include "entropy_accumulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ENTROPY_ACCUMULATOR_HOST_TEST

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

static struct entropy_accumulator_config make_config(
    uint32_t generation,
    uint32_t source_id,
    uint32_t min_length,
    uint32_t max_length,
    uint32_t max_stall_ticks)
{
    struct entropy_accumulator_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_ACCUMULATOR_VERSION;
    config.generation = generation;
    config.source_id = source_id;
    config.block_length_min = min_length;
    config.block_length_max = max_length;
    config.max_stall_ticks = max_stall_ticks;
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

static bool accumulator_bytes_zero(
    const struct entropy_accumulator *accumulator)
{
    uint32_t index;

    for (index = 0U; index < ENTROPY_ACCUMULATOR_CAPACITY; ++index) {
        if (accumulator->bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool source_state_zeroized(
    const struct entropy_accumulator *accumulator)
{
    const struct entropy_accumulator_source_state *source =
        &accumulator->source;

    return !source->active &&
           source->generation == 0U &&
           source->source_id == 0U &&
           source->credited_bytes == 0U &&
           source->credited_blocks == 0U &&
           source->last_accept_tick == 0U;
}

static bool source_state_initial(
    const struct entropy_accumulator *accumulator,
    uint32_t initial_tick)
{
    const struct entropy_accumulator_source_state *source =
        &accumulator->source;

    return !source->active &&
           source->generation == 0U &&
           source->source_id == 0U &&
           source->credited_bytes == 0U &&
           source->credited_blocks == 0U &&
           source->last_accept_tick == initial_tick;
}

static bool source_state_is(
    const struct entropy_accumulator *accumulator,
    uint64_t credited_bytes,
    uint32_t credited_blocks,
    uint32_t last_accept_tick)
{
    const struct entropy_accumulator_source_state *source =
        &accumulator->source;

    return source->active &&
           source->generation == accumulator->generation &&
           source->source_id == accumulator->source_id &&
           source->credited_bytes == credited_bytes &&
           source->credited_blocks == credited_blocks &&
           source->last_accept_tick == last_accept_tick;
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
        enum entropy_accumulator_status status;
        const char *string;
    } statuses[] = {
        {ENTROPY_ACCUMULATOR_OK, "ENTROPY_ACCUMULATOR_OK"},
        {ENTROPY_ACCUMULATOR_NULL, "ENTROPY_ACCUMULATOR_NULL"},
        {ENTROPY_ACCUMULATOR_BAD_STATE,
         "ENTROPY_ACCUMULATOR_BAD_STATE"},
        {ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED,
         "ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED"},
        {ENTROPY_ACCUMULATOR_BAD_VERSION,
         "ENTROPY_ACCUMULATOR_BAD_VERSION"},
        {ENTROPY_ACCUMULATOR_BAD_GENERATION,
         "ENTROPY_ACCUMULATOR_BAD_GENERATION"},
        {ENTROPY_ACCUMULATOR_BAD_SOURCE_ID,
         "ENTROPY_ACCUMULATOR_BAD_SOURCE_ID"},
        {ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS,
         "ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS"},
        {ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT,
         "ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT"},
        {ENTROPY_ACCUMULATOR_GENERATION_MISMATCH,
         "ENTROPY_ACCUMULATOR_GENERATION_MISMATCH"},
        {ENTROPY_ACCUMULATOR_SOURCE_MISMATCH,
         "ENTROPY_ACCUMULATOR_SOURCE_MISMATCH"},
        {ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH,
         "ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH"},
        {ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK,
         "ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK"},
        {ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW,
         "ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW"},
        {ENTROPY_ACCUMULATOR_STALLED,
         "ENTROPY_ACCUMULATOR_STALLED"},
    };
    static const struct {
        enum entropy_accumulator_state state;
        const char *string;
    } states[] = {
        {ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED,
         "ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED"},
        {ENTROPY_ACCUMULATOR_STATE_INITIALIZING,
         "ENTROPY_ACCUMULATOR_STATE_INITIALIZING"},
        {ENTROPY_ACCUMULATOR_STATE_ACCUMULATING,
         "ENTROPY_ACCUMULATOR_STATE_ACCUMULATING"},
        {ENTROPY_ACCUMULATOR_STATE_FAILED,
         "ENTROPY_ACCUMULATOR_STATE_FAILED"},
    };
    static const struct {
        enum entropy_accumulator_fault fault;
        const char *string;
    } faults[] = {
        {ENTROPY_ACCUMULATOR_FAULT_NONE,
         "ENTROPY_ACCUMULATOR_FAULT_NONE"},
        {ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH,
         "ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH"},
        {ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH,
         "ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH"},
        {ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH,
         "ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH"},
        {ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK,
         "ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK"},
        {ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW,
         "ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW"},
        {ENTROPY_ACCUMULATOR_FAULT_STALLED,
         "ENTROPY_ACCUMULATOR_FAULT_STALLED"},
    };
    size_t index;

    reset_fixture();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = entropy_accumulator_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "entropy accumulator status string is exact");
    }
    CHECK(entropy_accumulator_status_string(
              (enum entropy_accumulator_status)99U) == NULL,
          "entropy accumulator rejects undefined status string");

    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        const char *string = entropy_accumulator_state_string(
            states[index].state);

        CHECK(string != NULL &&
                  strcmp(string, states[index].string) == 0,
              "entropy accumulator state string is exact");
    }
    CHECK(entropy_accumulator_state_string(
              (enum entropy_accumulator_state)99U) == NULL,
          "entropy accumulator rejects undefined state string");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = entropy_accumulator_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "entropy accumulator fault string is exact");
    }
    CHECK(entropy_accumulator_fault_string(
              (enum entropy_accumulator_fault)99U) == NULL,
          "entropy accumulator rejects undefined fault string");
    expect_no_allocation(
        "entropy accumulator strings perform no allocation");
}

static void test_init_rejections(void)
{
    struct entropy_accumulator accumulator;
    struct entropy_accumulator_config config;
    uint8_t block[8];

    memset(&accumulator, 0xA5, sizeof(accumulator));
    config = make_config(7U, 42U, 1U, 8U, 20U);
    fill_block(block, (uint32_t)sizeof(block), 0x31U);

    reset_fixture();
    CHECK(entropy_accumulator_init(
              NULL, &config, 100U) == ENTROPY_ACCUMULATOR_NULL,
          "entropy accumulator init rejects null accumulator");
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, NULL, 100U) == ENTROPY_ACCUMULATOR_NULL,
          "entropy accumulator init rejects null config");

    config = make_config(7U, 42U, 1U, 8U, 20U);
    config.version = 0U;
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_VERSION &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED,
          "entropy accumulator init rejects bad config version");

    config = make_config(0U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_GENERATION,
          "entropy accumulator init rejects zero generation");

    config = make_config(
        ENTROPY_ACCUMULATOR_GENERATION_MAX + 1U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_GENERATION,
          "entropy accumulator init rejects over-limit generation");

    config = make_config(7U, 0U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_SOURCE_ID,
          "entropy accumulator init rejects zero source identity");

    config = make_config(
        7U, ENTROPY_ACCUMULATOR_SOURCE_ID_MAX + 1U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_SOURCE_ID,
          "entropy accumulator init rejects over-limit source identity");

    config = make_config(7U, 42U, 0U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS,
          "entropy accumulator init rejects zero minimum length");

    config = make_config(7U, 42U, 8U, 1U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS,
          "entropy accumulator init rejects inverted length bounds");

    config = make_config(
        7U, 42U, 1U, ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX + 1U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS,
          "entropy accumulator init rejects over-limit maximum length");

    config = make_config(7U, 42U, 1U, 8U, 0U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT,
          "entropy accumulator init rejects zero stall limit");

    config = make_config(
        7U, 42U, 1U, 8U, ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX + 1U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) ==
              ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT,
          "entropy accumulator init rejects over-limit stall limit");

    config = make_config(7U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 100U) == ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_initialized(&accumulator) &&
          !entropy_accumulator_accumulating(&accumulator) &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_INITIALIZING &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_NONE &&
          entropy_accumulator_generation(&accumulator) == 7U &&
          entropy_accumulator_source_id(&accumulator) == 42U &&
          entropy_accumulator_credited_bytes(&accumulator) == 0U &&
          entropy_accumulator_credited_blocks(&accumulator) == 0U &&
          entropy_accumulator_last_accept_tick(&accumulator) == 100U &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_initial(&accumulator, 100U),
          "valid entropy accumulator init publishes initializing state");

    CHECK(entropy_accumulator_init(
              &accumulator, &config, 101U) ==
              ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED &&
          entropy_accumulator_generation(&accumulator) == 7U,
          "duplicate entropy accumulator init is rejected without mutation");

    expect_no_allocation(
        "entropy accumulator init paths perform no allocation");
}

static void test_valid_mix_and_progress(void)
{
    struct entropy_accumulator accumulator;
    struct entropy_accumulator accumulator_again;
    struct entropy_accumulator_config config;
    uint8_t first[8];
    uint8_t second[8];

    config = make_config(7U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    memset(&accumulator_again, 0, sizeof(accumulator_again));
    fill_block(first, (uint32_t)sizeof(first), 0x31U);
    fill_block(second, (uint32_t)sizeof(second), 0x62U);

    reset_fixture();
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "valid mix fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_init(
              &accumulator_again, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "deterministic fixture initializes second accumulator");

    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, first, 4U, 15U) ==
              ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_ACCUMULATING &&
          entropy_accumulator_accumulating(&accumulator) &&
          entropy_accumulator_credited_bytes(&accumulator) == 4U &&
          entropy_accumulator_credited_blocks(&accumulator) == 1U &&
          entropy_accumulator_last_accept_tick(&accumulator) == 15U &&
          source_state_is(
              &accumulator, 4U, 1U, 15U) &&
          !accumulator_bytes_zero(&accumulator),
          "valid first mix publishes ACCUMULATING and credits exact source");

    CHECK(entropy_accumulator_mix(
              &accumulator_again, 7U, 42U, first, 4U, 15U) ==
              ENTROPY_ACCUMULATOR_OK &&
          memcmp(
              accumulator.bytes,
              accumulator_again.bytes,
              ENTROPY_ACCUMULATOR_CAPACITY) == 0,
          "identical accepted blocks mix deterministically");

    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, second, (uint32_t)sizeof(second), 35U) ==
              ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_credited_bytes(&accumulator) == 12U &&
          entropy_accumulator_credited_blocks(&accumulator) == 2U &&
          source_state_is(
              &accumulator, 12U, 2U, 35U),
          "second accepted block updates credited source state");

    CHECK(entropy_accumulator_progress(
              &accumulator, 55U) == ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_ACCUMULATING,
          "progress at stall limit remains healthy");

    CHECK(entropy_accumulator_progress(
              &accumulator, 56U) == ENTROPY_ACCUMULATOR_STALLED &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_FAILED &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_STALLED &&
          !entropy_accumulator_accumulating(&accumulator) &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "progress above stall limit fails and zeroizes accumulator");

    CHECK(entropy_accumulator_progress(
              &accumulator, 57U) == ENTROPY_ACCUMULATOR_BAD_STATE,
          "progress after failure rejects further mutation");
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, second, 8U, 57U) ==
              ENTROPY_ACCUMULATOR_BAD_STATE,
          "mix after failure rejects further mutation");
    expect_no_allocation(
        "entropy accumulator mix/progress paths perform no allocation");
}

static void test_wraparound_progress(void)
{
    struct entropy_accumulator accumulator;
    struct entropy_accumulator_config config;
    uint8_t block[2];

    config = make_config(7U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    fill_block(block, (uint32_t)sizeof(block), 0x11U);

    reset_fixture();
    CHECK(entropy_accumulator_init(
              &accumulator, &config, UINT32_C(0xFFFFFFF0)) ==
              ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_mix(
              &accumulator, 7U, 42U, block, 2U,
              UINT32_C(0xFFFFFFF5)) == ENTROPY_ACCUMULATOR_OK &&
          entropy_accumulator_progress(
              &accumulator, UINT32_C(0x00000005)) ==
              ENTROPY_ACCUMULATOR_OK,
          "wraparound-safe monotonic progress remains healthy");
    expect_no_allocation(
        "entropy accumulator wraparound path performs no allocation");
}

static void test_fail_closed_paths(void)
{
    struct entropy_accumulator accumulator;
    struct entropy_accumulator_config config;
    uint8_t block[8];

    config = make_config(7U, 42U, 1U, 8U, 20U);
    memset(&accumulator, 0, sizeof(accumulator));
    fill_block(block, (uint32_t)sizeof(block), 0x41U);

    reset_fixture();
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "fail-closed fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_mix(
              &accumulator, 8U, 42U, block, 4U, 15U) ==
              ENTROPY_ACCUMULATOR_GENERATION_MISMATCH &&
          entropy_accumulator_state(&accumulator) ==
              ENTROPY_ACCUMULATOR_STATE_FAILED &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "generation mismatch fails closed and zeroizes state");

    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "source-mismatch fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 43U, block, 4U, 15U) ==
              ENTROPY_ACCUMULATOR_SOURCE_MISMATCH &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "source mismatch fails closed and zeroizes state");

    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "length fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, block, 0U, 15U) ==
              ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "zero-length accepted block fails closed");

    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "over-max fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, block, 9U, 15U) ==
              ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "over-max accepted block fails closed");

    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "overlap fixture initializes entropy accumulator");
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, &accumulator, 1U, 15U) ==
              ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "overlapping accepted block fails closed");

    memset(&accumulator, 0, sizeof(accumulator));
    CHECK(entropy_accumulator_init(
              &accumulator, &config, 10U) == ENTROPY_ACCUMULATOR_OK,
          "overflow fixture initializes entropy accumulator");
    accumulator.source.credited_bytes = UINT64_MAX;
    CHECK(entropy_accumulator_mix(
              &accumulator, 7U, 42U, block, 4U, 15U) ==
              ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW &&
          entropy_accumulator_fault(&accumulator) ==
              ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW &&
          accumulator_bytes_zero(&accumulator) &&
          source_state_zeroized(&accumulator),
          "credited-byte overflow fails closed and zeroizes state");
    expect_no_allocation(
        "entropy accumulator fail-closed paths perform no allocation");
}

int main(void)
{
    failures = 0;
    test_status_strings();
    test_init_rejections();
    test_valid_mix_and_progress();
    test_wraparound_progress();
    test_fail_closed_paths();

    if (failures != 0) {
        fprintf(stderr, "Entropy accumulator host tests: FAILED\n");
        return 1;
    }
    printf("Entropy accumulator host tests: ALL OK\n");
    return 0;
}

#endif
