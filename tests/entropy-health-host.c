#include "entropy_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ENTROPY_HEALTH_HOST_TEST

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

static struct entropy_health_config make_config(
    uint32_t min_length,
    uint32_t max_length,
    uint32_t max_stall_ticks)
{
    struct entropy_health_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_HEALTH_VERSION;
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

static bool blocks_equal(
    const uint8_t *left,
    const uint8_t *right,
    uint32_t block_length)
{
    uint32_t index;

    for (index = 0U; index < block_length; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool previous_block_is(
    const struct entropy_health *health,
    const uint8_t *block,
    uint32_t block_length)
{
    return health->previous_block_valid &&
           health->previous_block_length == block_length &&
           blocks_equal(health->previous_block, block, block_length);
}

static bool previous_block_zeroized(
    const struct entropy_health *health)
{
    return !health->previous_block_valid &&
           health->previous_block_length == 0U;
}

static bool status_string_matches(
    enum entropy_health_status status,
    const char *string)
{
    switch (status) {
    case ENTROPY_HEALTH_OK:
        return strcmp(string, "ENTROPY_HEALTH_OK") == 0;
    case ENTROPY_HEALTH_NULL:
        return strcmp(string, "ENTROPY_HEALTH_NULL") == 0;
    case ENTROPY_HEALTH_BAD_STATE:
        return strcmp(string, "ENTROPY_HEALTH_BAD_STATE") == 0;
    case ENTROPY_HEALTH_ALREADY_PUBLISHED:
        return strcmp(
            string, "ENTROPY_HEALTH_ALREADY_PUBLISHED") == 0;
    case ENTROPY_HEALTH_BAD_VERSION:
        return strcmp(string, "ENTROPY_HEALTH_BAD_VERSION") == 0;
    case ENTROPY_HEALTH_BAD_GENERATION:
        return strcmp(
            string, "ENTROPY_HEALTH_BAD_GENERATION") == 0;
    case ENTROPY_HEALTH_BAD_LENGTH_BOUNDS:
        return strcmp(
            string, "ENTROPY_HEALTH_BAD_LENGTH_BOUNDS") == 0;
    case ENTROPY_HEALTH_BAD_STALL_LIMIT:
        return strcmp(
            string, "ENTROPY_HEALTH_BAD_STALL_LIMIT") == 0;
    case ENTROPY_HEALTH_REPEATED_BLOCK:
        return strcmp(
            string, "ENTROPY_HEALTH_REPEATED_BLOCK") == 0;
    case ENTROPY_HEALTH_IMPOSSIBLE_LENGTH:
        return strcmp(
            string, "ENTROPY_HEALTH_IMPOSSIBLE_LENGTH") == 0;
    case ENTROPY_HEALTH_STALLED:
        return strcmp(string, "ENTROPY_HEALTH_STALLED") == 0;
    case ENTROPY_HEALTH_DEVICE_RESET:
        return strcmp(
            string, "ENTROPY_HEALTH_DEVICE_RESET") == 0;
    default:
        return false;
    }
}

static void test_status_strings(void)
{
    static const enum entropy_health_status statuses[] = {
        ENTROPY_HEALTH_OK,
        ENTROPY_HEALTH_NULL,
        ENTROPY_HEALTH_BAD_STATE,
        ENTROPY_HEALTH_ALREADY_PUBLISHED,
        ENTROPY_HEALTH_BAD_VERSION,
        ENTROPY_HEALTH_BAD_GENERATION,
        ENTROPY_HEALTH_BAD_LENGTH_BOUNDS,
        ENTROPY_HEALTH_BAD_STALL_LIMIT,
        ENTROPY_HEALTH_REPEATED_BLOCK,
        ENTROPY_HEALTH_IMPOSSIBLE_LENGTH,
        ENTROPY_HEALTH_STALLED,
        ENTROPY_HEALTH_DEVICE_RESET,
    };
    size_t index;

    for (index = 0U; index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        CHECK(status_string_matches(
                  statuses[index],
                  entropy_health_status_string(statuses[index])),
              "entropy health status string is exact");
    }
    CHECK(entropy_health_status_string(
              (enum entropy_health_status)99U) == NULL,
          "entropy health rejects undefined status string");

    CHECK(strcmp(
              entropy_health_state_string(
                  ENTROPY_HEALTH_STATE_UNINITIALIZED),
              "ENTROPY_HEALTH_STATE_UNINITIALIZED") == 0 &&
          strcmp(
              entropy_health_state_string(
                  ENTROPY_HEALTH_STATE_INITIALIZING),
              "ENTROPY_HEALTH_STATE_INITIALIZING") == 0 &&
          strcmp(
              entropy_health_state_string(
                  ENTROPY_HEALTH_STATE_READY),
              "ENTROPY_HEALTH_STATE_READY") == 0 &&
          strcmp(
              entropy_health_state_string(
                  ENTROPY_HEALTH_STATE_FAILED),
              "ENTROPY_HEALTH_STATE_FAILED") == 0,
          "entropy health state strings are exact");
    CHECK(entropy_health_state_string(
              (enum entropy_health_state)99U) == NULL,
          "entropy health rejects undefined state string");

    CHECK(strcmp(
              entropy_health_fault_string(
                  ENTROPY_HEALTH_FAULT_NONE),
              "ENTROPY_HEALTH_FAULT_NONE") == 0 &&
          strcmp(
              entropy_health_fault_string(
                  ENTROPY_HEALTH_FAULT_REPEATED_BLOCK),
              "ENTROPY_HEALTH_FAULT_REPEATED_BLOCK") == 0 &&
          strcmp(
              entropy_health_fault_string(
                  ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH),
              "ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH") == 0 &&
          strcmp(
              entropy_health_fault_string(
                  ENTROPY_HEALTH_FAULT_STALLED),
              "ENTROPY_HEALTH_FAULT_STALLED") == 0 &&
          strcmp(
              entropy_health_fault_string(
                  ENTROPY_HEALTH_FAULT_DEVICE_RESET),
              "ENTROPY_HEALTH_FAULT_DEVICE_RESET") == 0,
          "entropy health fault strings are exact");
    CHECK(entropy_health_fault_string(
              (enum entropy_health_fault)99U) == NULL,
          "entropy health rejects undefined fault string");
}

static void test_init_rejections(void)
{
    struct entropy_health health;
    struct entropy_health_config config;

    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              NULL, &config, 7U, 100U) == ENTROPY_HEALTH_NULL,
          "entropy health init rejects null state");

    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, NULL, 7U, 100U) == ENTROPY_HEALTH_NULL,
          "entropy health init rejects null config");

    config = make_config(1U, 8U, 20U);
    config.version = 0U;
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_VERSION &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_UNINITIALIZED,
          "entropy health init rejects bad config version");

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 0U, 100U) ==
              ENTROPY_HEALTH_BAD_GENERATION,
          "entropy health init rejects zero generation");

    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config,
              ENTROPY_HEALTH_GENERATION_MAX + 1U, 100U) ==
              ENTROPY_HEALTH_BAD_GENERATION,
          "entropy health init rejects over-limit generation");

    config = make_config(0U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_LENGTH_BOUNDS,
          "entropy health init rejects zero minimum length");

    config = make_config(8U, 1U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_LENGTH_BOUNDS,
          "entropy health init rejects inverted length bounds");

    config = make_config(
        1U, ENTROPY_HEALTH_BLOCK_LENGTH_MAX + 1U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_LENGTH_BOUNDS,
          "entropy health init rejects over-limit maximum length");

    config = make_config(1U, 8U, 0U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_STALL_LIMIT,
          "entropy health init rejects zero stall limit");

    config = make_config(
        1U, 8U, ENTROPY_HEALTH_STALL_LIMIT_MAX + 1U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) ==
              ENTROPY_HEALTH_BAD_STALL_LIMIT,
          "entropy health init rejects over-limit stall limit");

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 100U) == ENTROPY_HEALTH_OK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_INITIALIZING &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_NONE &&
          entropy_health_generation(&health) == 7U &&
          !entropy_health_ready(&health) &&
          health.last_accept_tick == 100U &&
          previous_block_zeroized(&health),
          "valid entropy health init publishes initializing state");

    CHECK(entropy_health_init(
              &health, &config, 8U, 101U) ==
              ENTROPY_HEALTH_ALREADY_PUBLISHED &&
          entropy_health_generation(&health) == 7U,
          "duplicate entropy health init is rejected without mutation");
}

static void test_observation_and_progress(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t first[8];
    uint8_t second[8];

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(first, (uint32_t)sizeof(first), 0x21U);
    fill_block(second, (uint32_t)sizeof(second), 0x42U);

    CHECK(entropy_health_init(
              &health, &config, 7U, 10U) == ENTROPY_HEALTH_OK,
          "observation fixture initializes entropy health");
    CHECK(entropy_health_observe(
              &health, 7U, first, (uint32_t)sizeof(first), 15U) ==
              ENTROPY_HEALTH_OK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_READY &&
          entropy_health_ready(&health) &&
          health.last_accept_tick == 15U &&
          previous_block_is(
              &health, first, (uint32_t)sizeof(first)),
          "valid first observation publishes READY and exact block");

    CHECK(entropy_health_progress(
              &health, 35U) == ENTROPY_HEALTH_OK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_READY,
          "progress at stall limit remains healthy");

    CHECK(entropy_health_progress(
              &health, 36U) == ENTROPY_HEALTH_STALLED &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_FAILED &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_STALLED &&
          !entropy_health_ready(&health) &&
          previous_block_zeroized(&health),
          "progress above stall limit fails and zeroizes previous block");

    CHECK(entropy_health_progress(
              &health, 37U) == ENTROPY_HEALTH_BAD_STATE,
          "progress after failure rejects further mutation");
    CHECK(entropy_health_observe(
              &health, 7U, second, (uint32_t)sizeof(second), 37U) ==
              ENTROPY_HEALTH_BAD_STATE,
          "observe after failure rejects further mutation");
}

static void test_initial_stall(void)
{
    struct entropy_health health;
    struct entropy_health_config config;

    config = make_config(1U, 8U, 5U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK,
          "initial-stall fixture initializes entropy health");
    CHECK(entropy_health_progress(
              &health, 6U) == ENTROPY_HEALTH_STALLED &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_STALLED,
          "initializing health fails when first block stalls");
}

static void test_repeated_and_different_blocks(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t first[8];
    uint8_t second[8];

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(first, (uint32_t)sizeof(first), 0x31U);
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, first, (uint32_t)sizeof(first), 1U) ==
              ENTROPY_HEALTH_OK,
          "repeated-block fixture accepts first block");
    CHECK(entropy_health_observe(
              &health, 7U, first, (uint32_t)sizeof(first), 2U) ==
              ENTROPY_HEALTH_REPEATED_BLOCK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_FAILED &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_REPEATED_BLOCK &&
          previous_block_zeroized(&health),
          "exact repeated block fails and zeroizes previous block");

    memset(&health, 0, sizeof(health));
    fill_block(second, (uint32_t)sizeof(second), 0x51U);
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, first, (uint32_t)sizeof(first), 1U) ==
              ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, second, (uint32_t)sizeof(second), 2U) ==
              ENTROPY_HEALTH_OK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_READY &&
          previous_block_is(
              &health, second, (uint32_t)sizeof(second)),
          "different block replaces previous block and remains ready");
}

static void test_impossible_lengths(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t block[16];
    uint32_t length;

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(block, (uint32_t)sizeof(block), 0x61U);
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK,
          "impossible-length fixture initializes entropy health");
    CHECK(entropy_health_observe(
              &health, 7U, block, 0U, 1U) ==
              ENTROPY_HEALTH_IMPOSSIBLE_LENGTH &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH &&
          previous_block_zeroized(&health),
          "zero length fails as impossible");

    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, 9U, 1U) ==
              ENTROPY_HEALTH_IMPOSSIBLE_LENGTH,
          "length above configured maximum fails as impossible");

    config = make_config(2U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, 1U, 1U) ==
              ENTROPY_HEALTH_IMPOSSIBLE_LENGTH,
          "length below configured minimum fails as impossible");

    config = make_config(1U, ENTROPY_HEALTH_BLOCK_LENGTH_MAX, 20U);
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block,
              ENTROPY_HEALTH_BLOCK_LENGTH_MAX + 1U, 1U) ==
              ENTROPY_HEALTH_IMPOSSIBLE_LENGTH,
          "length above global maximum fails as impossible");

    length = 2U;
    memset(&health, 0, sizeof(health));
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, length, 1U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, length, 2U) ==
              ENTROPY_HEALTH_REPEATED_BLOCK,
          "valid minimum-length block is accepted once then repeated");
}

static void test_device_reset(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t block[8];

    config = make_config(1U, 8U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(block, (uint32_t)sizeof(block), 0x71U);
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, (uint32_t)sizeof(block), 1U) ==
              ENTROPY_HEALTH_OK,
          "device-reset fixture accepts first generation block");
    CHECK(entropy_health_observe(
              &health, 8U, block, (uint32_t)sizeof(block), 2U) ==
              ENTROPY_HEALTH_DEVICE_RESET &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_FAILED &&
          entropy_health_fault(&health) ==
              ENTROPY_HEALTH_FAULT_DEVICE_RESET &&
          previous_block_zeroized(&health),
          "generation change fails as device reset");
}

static void test_wraparound_progress(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t block[4];

    config = make_config(1U, 4U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(block, (uint32_t)sizeof(block), 0x81U);
    CHECK(entropy_health_init(
              &health, &config, 7U, UINT32_MAX - 2U) ==
              ENTROPY_HEALTH_OK,
          "wraparound fixture initializes near counter wrap");
    CHECK(entropy_health_progress(
              &health, 10U) == ENTROPY_HEALTH_OK,
          "wraparound-safe progress within stall limit is accepted");
    CHECK(entropy_health_observe(
              &health, 7U, block, (uint32_t)sizeof(block), 12U) ==
              ENTROPY_HEALTH_OK &&
          entropy_health_state(&health) ==
              ENTROPY_HEALTH_STATE_READY,
          "wraparound-safe observation publishes READY");
    CHECK(entropy_health_progress(
              &health, 33U) == ENTROPY_HEALTH_STALLED,
          "wraparound-safe stall limit still fails closed");
}

static void test_no_allocation(void)
{
    struct entropy_health health;
    struct entropy_health_config config;
    uint8_t block[4];

    config = make_config(1U, 4U, 20U);
    memset(&health, 0, sizeof(health));
    fill_block(block, (uint32_t)sizeof(block), 0x91U);
    alloc_attempts = 0U;
    free_attempts = 0U;
    CHECK(entropy_health_init(
              &health, &config, 7U, 0U) == ENTROPY_HEALTH_OK &&
          entropy_health_observe(
              &health, 7U, block, (uint32_t)sizeof(block), 1U) ==
              ENTROPY_HEALTH_OK &&
          entropy_health_progress(
              &health, 2U) == ENTROPY_HEALTH_OK,
          "no-allocation fixture exercises all healthy paths");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "entropy health performs no allocation or free");
}

int main(void)
{
    test_status_strings();
    test_init_rejections();
    test_observation_and_progress();
    test_initial_stall();
    test_repeated_and_different_blocks();
    test_impossible_lengths();
    test_device_reset();
    test_wraparound_progress();
    test_no_allocation();

    if (failures == 0) {
        printf("Entropy health host tests: ALL OK\n");
        return 0;
    }
    fprintf(stderr, "Entropy health host tests: %d FAILURES\n",
            failures);
    return 1;
}

#endif
