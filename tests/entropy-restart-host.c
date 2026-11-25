#include "entropy_restart.h"

#include "entropy_accumulator.h"
#include "entropy_health.h"
#include "entropy_seed_fill.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_ENTROPY_RESTART_HOST_TEST

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

static struct entropy_restart_config make_config(
    uint32_t restart_count_max,
    uint32_t min_restart_interval_ticks)
{
    struct entropy_restart_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_RESTART_VERSION;
    config.restart_count_max = restart_count_max;
    config.min_restart_interval_ticks = min_restart_interval_ticks;
    return config;
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

static struct entropy_seed_fill_config make_seed_config(
    uint32_t generation,
    uint32_t source_id,
    uint32_t required_credited_bytes)
{
    struct entropy_seed_fill_config config;

    memset(&config, 0, sizeof(config));
    config.version = ENTROPY_SEED_FILL_VERSION;
    config.generation = generation;
    config.source_id = source_id;
    config.required_credited_bytes = required_credited_bytes;
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
        block[index] = (uint8_t)(seed + index * 7U);
    }
}

static bool all_bytes_zero(const void *object, size_t size)
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

static void zeroize_object(void *object, size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        bytes[index] = 0U;
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
    uint32_t generation,
    uint32_t source_id,
    uint32_t credited_length)
{
    struct entropy_health_config health_config = make_health_config();
    struct entropy_accumulator_config accumulator_config =
        make_accumulator_config(generation, source_id);
    uint8_t block[ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX];

    fill_block(block, credited_length, 0x51U);
    memset(health, 0, sizeof(*health));
    CHECK(entropy_health_init(
              health, &health_config, generation, 10U) ==
              ENTROPY_HEALTH_OK &&
              entropy_health_observe(
                  health,
                  generation,
                  block,
                  credited_length,
                  15U) == ENTROPY_HEALTH_OK &&
              entropy_health_ready(health),
          "restart fixture publishes ready health");

    memset(accumulator, 0, sizeof(*accumulator));
    CHECK(entropy_accumulator_init(
              accumulator, &accumulator_config, 10U) ==
              ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_mix(
                  accumulator,
                  generation,
                  source_id,
                  block,
                  credited_length,
                  15U) == ENTROPY_ACCUMULATOR_OK &&
              entropy_accumulator_accumulating(accumulator),
          "restart fixture publishes accumulating accumulator");
}

static void init_seed_fill_ready(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator,
    uint32_t generation,
    uint32_t source_id)
{
    struct entropy_seed_fill_config config =
        make_seed_config(generation, source_id, 32U);
    uint8_t seed_output[64];
    uint32_t written = 0U;

    memset(controller, 0, sizeof(*controller));
    CHECK(entropy_seed_fill_init(
              controller, &config, accumulator) ==
              ENTROPY_SEED_FILL_OK &&
              entropy_seed_fill_ready(controller),
          "restart fixture publishes ready seed/fill controller");
    CHECK(entropy_seed_fill_seed(
              controller,
              accumulator,
              seed_output,
              (uint32_t)sizeof(seed_output),
              &written) == ENTROPY_SEED_FILL_OK &&
              written == 32U,
          "restart fixture publishes seed material");
}

static void init_ready_chain(
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *controller,
    uint32_t generation,
    uint32_t source_id)
{
    init_health_accumulator(
        health, accumulator, generation, source_id, 32U);
    init_seed_fill_ready(
        controller, accumulator, generation, source_id);
}

static bool lifecycle_state_is(
    const struct entropy_restart *restart,
    enum entropy_restart_state state,
    enum entropy_restart_fault fault)
{
    return entropy_restart_initialized(restart) &&
           entropy_restart_state(restart) == state &&
           entropy_restart_fault(restart) == fault;
}

static bool chain_zeroized(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *controller)
{
    return all_bytes_zero(health, sizeof(*health)) &&
           all_bytes_zero(accumulator, sizeof(*accumulator)) &&
           all_bytes_zero(controller, sizeof(*controller));
}

static void test_status_strings(void)
{
    static const struct {
        enum entropy_restart_status status;
        const char *string;
    } statuses[] = {
        {ENTROPY_RESTART_OK, "ENTROPY_RESTART_OK"},
        {ENTROPY_RESTART_NULL, "ENTROPY_RESTART_NULL"},
        {ENTROPY_RESTART_BAD_STATE, "ENTROPY_RESTART_BAD_STATE"},
        {ENTROPY_RESTART_ALREADY_PUBLISHED,
         "ENTROPY_RESTART_ALREADY_PUBLISHED"},
        {ENTROPY_RESTART_BAD_VERSION,
         "ENTROPY_RESTART_BAD_VERSION"},
        {ENTROPY_RESTART_BAD_RESTART_COUNT,
         "ENTROPY_RESTART_BAD_RESTART_COUNT"},
        {ENTROPY_RESTART_BAD_INTERVAL,
         "ENTROPY_RESTART_BAD_INTERVAL"},
        {ENTROPY_RESTART_BAD_GENERATION,
         "ENTROPY_RESTART_BAD_GENERATION"},
        {ENTROPY_RESTART_BAD_SOURCE_ID,
         "ENTROPY_RESTART_BAD_SOURCE_ID"},
        {ENTROPY_RESTART_MISSING_DEVICE,
         "ENTROPY_RESTART_MISSING_DEVICE"},
        {ENTROPY_RESTART_STALLED, "ENTROPY_RESTART_STALLED"},
        {ENTROPY_RESTART_REPEATED_OUTPUT,
         "ENTROPY_RESTART_REPEATED_OUTPUT"},
        {ENTROPY_RESTART_DEVICE_RESET,
         "ENTROPY_RESTART_DEVICE_RESET"},
        {ENTROPY_RESTART_UNHEALTHY, "ENTROPY_RESTART_UNHEALTHY"},
        {ENTROPY_RESTART_NOT_READY, "ENTROPY_RESTART_NOT_READY"},
        {ENTROPY_RESTART_STALE_GENERATION,
         "ENTROPY_RESTART_STALE_GENERATION"},
        {ENTROPY_RESTART_INTERVAL, "ENTROPY_RESTART_INTERVAL"},
        {ENTROPY_RESTART_EXHAUSTED, "ENTROPY_RESTART_EXHAUSTED"},
    };
    static const struct {
        enum entropy_restart_state state;
        const char *string;
    } states[] = {
        {ENTROPY_RESTART_STATE_UNINITIALIZED,
         "ENTROPY_RESTART_STATE_UNINITIALIZED"},
        {ENTROPY_RESTART_STATE_UNAVAILABLE,
         "ENTROPY_RESTART_STATE_UNAVAILABLE"},
        {ENTROPY_RESTART_STATE_READY,
         "ENTROPY_RESTART_STATE_READY"},
        {ENTROPY_RESTART_STATE_FAILED,
         "ENTROPY_RESTART_STATE_FAILED"},
        {ENTROPY_RESTART_STATE_STOPPED,
         "ENTROPY_RESTART_STATE_STOPPED"},
    };
    static const struct {
        enum entropy_restart_fault fault;
        const char *string;
    } faults[] = {
        {ENTROPY_RESTART_FAULT_NONE,
         "ENTROPY_RESTART_FAULT_NONE"},
        {ENTROPY_RESTART_FAULT_MISSING_DEVICE,
         "ENTROPY_RESTART_FAULT_MISSING_DEVICE"},
        {ENTROPY_RESTART_FAULT_STALLED,
         "ENTROPY_RESTART_FAULT_STALLED"},
        {ENTROPY_RESTART_FAULT_REPEATED_OUTPUT,
         "ENTROPY_RESTART_FAULT_REPEATED_OUTPUT"},
        {ENTROPY_RESTART_FAULT_DEVICE_RESET,
         "ENTROPY_RESTART_FAULT_DEVICE_RESET"},
        {ENTROPY_RESTART_FAULT_UNHEALTHY,
         "ENTROPY_RESTART_FAULT_UNHEALTHY"},
        {ENTROPY_RESTART_FAULT_SHUTDOWN,
         "ENTROPY_RESTART_FAULT_SHUTDOWN"},
    };
    size_t index;

    reset_fixture();
    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = entropy_restart_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "entropy restart status string is exact");
    }
    CHECK(entropy_restart_status_string(
              (enum entropy_restart_status)99U) == NULL,
          "entropy restart rejects undefined status string");

    for (index = 0U;
         index < sizeof(states) / sizeof(states[0]);
         ++index) {
        const char *string = entropy_restart_state_string(
            states[index].state);

        CHECK(string != NULL &&
                  strcmp(string, states[index].string) == 0,
              "entropy restart state string is exact");
    }
    CHECK(entropy_restart_state_string(
              (enum entropy_restart_state)99U) == NULL,
          "entropy restart rejects undefined state string");

    for (index = 0U;
         index < sizeof(faults) / sizeof(faults[0]);
         ++index) {
        const char *string = entropy_restart_fault_string(
            faults[index].fault);

        CHECK(string != NULL &&
                  strcmp(string, faults[index].string) == 0,
              "entropy restart fault string is exact");
    }
    CHECK(entropy_restart_fault_string(
              (enum entropy_restart_fault)99U) == NULL,
          "entropy restart rejects undefined fault string");
    expect_no_allocation(
        "entropy restart strings perform no allocation");
}

static void test_init_rejections(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config;

    memset(&restart, 0xA5, sizeof(restart));
    config = make_config(3U, 1U);

    reset_fixture();
    CHECK(entropy_restart_init(NULL, &config) == ENTROPY_RESTART_NULL,
          "entropy restart init rejects null lifecycle");
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, NULL) == ENTROPY_RESTART_NULL,
          "entropy restart init rejects null config");

    config = make_config(3U, 1U);
    config.version = 0U;
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_BAD_VERSION &&
          entropy_restart_state(&restart) ==
              ENTROPY_RESTART_STATE_UNINITIALIZED,
          "entropy restart init rejects bad config version");

    config = make_config(0U, 1U);
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_BAD_RESTART_COUNT,
          "entropy restart init rejects zero restart count");

    config = make_config(
        ENTROPY_RESTART_RESTART_COUNT_MAX + 1U, 1U);
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_BAD_RESTART_COUNT,
          "entropy restart init rejects over-limit restart count");

    config = make_config(3U, 0U);
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_BAD_INTERVAL,
          "entropy restart init rejects zero interval");

    config = make_config(
        3U, ENTROPY_RESTART_INTERVAL_MAX_TICKS + 1U);
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_BAD_INTERVAL,
          "entropy restart init rejects over-limit interval");

    config = make_config(3U, 1U);
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_UNAVAILABLE,
              ENTROPY_RESTART_FAULT_NONE) &&
          entropy_restart_generation(&restart) == 0U &&
          entropy_restart_last_generation(&restart) == 0U &&
          entropy_restart_restart_count(&restart) == 0U &&
          restart.health == NULL &&
          restart.accumulator == NULL &&
          restart.seed_fill == NULL,
          "valid entropy restart init publishes unavailable state");

    CHECK(entropy_restart_init(&restart, &config) ==
              ENTROPY_RESTART_ALREADY_PUBLISHED &&
          entropy_restart_restart_count(&restart) == 0U,
          "duplicate entropy restart init is rejected without mutation");
    expect_no_allocation(
        "entropy restart init paths perform no allocation");
}

static void test_missing_device_and_activate(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config = make_config(3U, 1U);
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_health uninitialized_health;
    struct entropy_accumulator uninitialized_accumulator;
    struct entropy_seed_fill uninitialized_controller;

    reset_fixture();
    memset(&health, 0xA5, sizeof(health));
    memset(&accumulator, 0xA5, sizeof(accumulator));
    memset(&controller, 0xA5, sizeof(controller));
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "missing-device fixture initializes lifecycle");
    CHECK(entropy_restart_activate(
              &restart,
              NULL,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_MISSING_DEVICE &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_MISSING_DEVICE) &&
          !entropy_restart_ready(&restart) &&
          restart.health == NULL &&
          restart.accumulator == NULL &&
          restart.seed_fill == NULL &&
          all_bytes_zero(&accumulator, sizeof(accumulator)) &&
          all_bytes_zero(&controller, sizeof(controller)),
          "activation with null device fails closed and zeroizes state");

    memset(&restart, 0, sizeof(restart));
    memset(&uninitialized_health, 0, sizeof(uninitialized_health));
    memset(&uninitialized_accumulator, 0xA5,
           sizeof(uninitialized_accumulator));
    memset(&uninitialized_controller, 0xA5,
           sizeof(uninitialized_controller));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "uninitialized-device fixture initializes lifecycle");
    CHECK(entropy_restart_activate(
              &restart,
              &uninitialized_health,
              &uninitialized_accumulator,
              &uninitialized_controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_MISSING_DEVICE &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_MISSING_DEVICE) &&
          all_bytes_zero(
              &uninitialized_accumulator,
              sizeof(uninitialized_accumulator)) &&
          all_bytes_zero(
              &uninitialized_controller,
              sizeof(uninitialized_controller)),
          "activation with uninitialized device fails closed");

    memset(&restart, 0, sizeof(restart));
    memset(&health, 0xA5, sizeof(health));
    memset(&accumulator, 0xA5, sizeof(accumulator));
    memset(&controller, 0xA5, sizeof(controller));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "ready-activation fixture initializes lifecycle");
    init_ready_chain(
        &health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_READY,
              ENTROPY_RESTART_FAULT_NONE) &&
          entropy_restart_ready(&restart) &&
          entropy_restart_generation(&restart) == 7U &&
          entropy_restart_last_generation(&restart) == 7U,
          "valid ready chain publishes ready lifecycle");
    CHECK(entropy_restart_check(&restart, 30U) == ENTROPY_RESTART_OK &&
          entropy_restart_ready(&restart),
          "ready lifecycle remains ready across check");

    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              30U) == ENTROPY_RESTART_ALREADY_PUBLISHED,
          "ready lifecycle rejects duplicate activation");
    expect_no_allocation(
        "entropy restart activation paths perform no allocation");
}

static void test_not_ready_promotion(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config = make_config(3U, 1U);
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    struct entropy_seed_fill_config seed_config =
        make_seed_config(7U, 42U, 32U);
    uint8_t additional[32];

    reset_fixture();
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "not-ready fixture initializes lifecycle");
    init_health_accumulator(
        &health, &accumulator, 7U, 42U, 16U);
    memset(&controller, 0, sizeof(controller));
    CHECK(entropy_seed_fill_init(
              &controller, &seed_config, &accumulator) ==
              ENTROPY_SEED_FILL_OK &&
              !entropy_seed_fill_ready(&controller),
          "not-ready fixture publishes degraded seed/fill state");
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_NOT_READY &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_UNAVAILABLE,
              ENTROPY_RESTART_FAULT_NONE) &&
          !entropy_restart_ready(&restart),
          "valid but not-ready chain stays unavailable");

    fill_block(additional, (uint32_t)sizeof(additional), 0x62U);
    CHECK(entropy_accumulator_mix(
              &accumulator,
              7U,
              42U,
              additional,
              (uint32_t)sizeof(additional),
              25U) == ENTROPY_ACCUMULATOR_OK,
          "not-ready fixture completes credited material");
    zeroize_object(&controller, sizeof(controller));
    init_seed_fill_ready(&controller, &accumulator, 7U, 42U);
    CHECK(entropy_restart_check(&restart, 30U) == ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_READY,
              ENTROPY_RESTART_FAULT_NONE) &&
          entropy_restart_ready(&restart) &&
          entropy_restart_last_generation(&restart) == 7U,
          "check promotes completed chain to ready");
    CHECK(entropy_restart_stop(&restart, 40U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 50U) == ENTROPY_RESTART_OK,
          "promoted fixture records restart policy");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              60U) == ENTROPY_RESTART_STALE_GENERATION,
          "promoted generation cannot be reused after restart");
    expect_no_allocation(
        "entropy restart not-ready paths perform no allocation");
}

static void test_failure_mapping(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config = make_config(3U, 1U);
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;
    uint8_t repeated[32];
    uint8_t reset_block[32];

    reset_fixture();
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "stall fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "stall fixture activates ready lifecycle");
    CHECK(entropy_health_progress(&health, 36U) ==
              ENTROPY_HEALTH_STALLED,
          "stall fixture fails health");
    CHECK(entropy_restart_check(&restart, 36U) ==
              ENTROPY_RESTART_STALLED &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_STALLED) &&
          !entropy_restart_ready(&restart) &&
          restart.health == NULL &&
          restart.accumulator == NULL &&
          restart.seed_fill == NULL &&
          chain_zeroized(&health, &accumulator, &controller),
          "health stall closes readiness and zeroizes supervised state");
    CHECK(entropy_restart_check(&restart, 37U) ==
              ENTROPY_RESTART_BAD_STATE,
          "failed lifecycle rejects further checks");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "repeated fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "repeated fixture activates ready lifecycle");
    fill_block(repeated, (uint32_t)sizeof(repeated), 0x51U);
    CHECK(entropy_health_observe(
              &health, 7U, repeated, (uint32_t)sizeof(repeated), 20U) ==
              ENTROPY_HEALTH_REPEATED_BLOCK,
          "repeated fixture fails health");
    CHECK(entropy_restart_check(&restart, 20U) ==
              ENTROPY_RESTART_REPEATED_OUTPUT &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_REPEATED_OUTPUT) &&
          chain_zeroized(&health, &accumulator, &controller),
          "repeated output closes readiness and zeroizes supervised state");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "reset fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "reset fixture activates ready lifecycle");
    fill_block(reset_block, (uint32_t)sizeof(reset_block), 0x73U);
    CHECK(entropy_health_observe(
              &health,
              8U,
              reset_block,
              (uint32_t)sizeof(reset_block),
              20U) == ENTROPY_HEALTH_DEVICE_RESET,
          "reset fixture fails health generation");
    CHECK(entropy_restart_check(&restart, 20U) ==
              ENTROPY_RESTART_DEVICE_RESET &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_DEVICE_RESET) &&
          chain_zeroized(&health, &accumulator, &controller),
          "device reset closes readiness and zeroizes supervised state");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "unhealthy fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "unhealthy fixture activates ready lifecycle");
    CHECK(entropy_health_observe(
              &health, 7U, repeated, 0U, 20U) ==
              ENTROPY_HEALTH_IMPOSSIBLE_LENGTH,
          "unhealthy fixture fails health length");
    CHECK(entropy_restart_check(&restart, 20U) ==
              ENTROPY_RESTART_UNHEALTHY &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_FAILED,
              ENTROPY_RESTART_FAULT_UNHEALTHY) &&
          chain_zeroized(&health, &accumulator, &controller),
          "other supervised failure maps to unhealthy");
    expect_no_allocation(
        "entropy restart failure paths perform no allocation");
}

static void test_stop_and_restart_policy(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config = make_config(3U, 1U);
    struct entropy_restart_config interval_config =
        make_config(3U, 50U);
    struct entropy_restart_config zero_config =
        make_config(3U, 1U);
    struct entropy_restart_config exhausted_config =
        make_config(1U, 1U);
    struct entropy_health health;
    struct entropy_accumulator accumulator;
    struct entropy_seed_fill controller;

    reset_fixture();
    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
          "stop fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "stop fixture activates ready lifecycle");
    CHECK(entropy_restart_stop(&restart, 100U) == ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_STOPPED,
              ENTROPY_RESTART_FAULT_SHUTDOWN) &&
          !entropy_restart_ready(&restart) &&
          entropy_restart_stopped(&restart) &&
          restart.health == NULL &&
          restart.accumulator == NULL &&
          restart.seed_fill == NULL &&
          chain_zeroized(&health, &accumulator, &controller),
          "stop closes readiness and zeroizes supervised state");
    CHECK(entropy_restart_stop(&restart, 101U) == ENTROPY_RESTART_OK &&
          entropy_restart_stopped(&restart),
          "stop is idempotent");
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              8U,
              42U,
              120U) == ENTROPY_RESTART_BAD_STATE,
          "stopped lifecycle requires explicit restart");

    CHECK(entropy_restart_restart(&restart, 120U) == ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_UNAVAILABLE,
              ENTROPY_RESTART_FAULT_NONE) &&
          entropy_restart_restart_count(&restart) == 1U &&
          entropy_restart_last_restart_tick(&restart) == 120U &&
          entropy_restart_last_generation(&restart) == 7U,
          "restart prepares next generation and records policy");

    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              130U) == ENTROPY_RESTART_STALE_GENERATION &&
          !entropy_restart_ready(&restart) &&
          entropy_restart_restart_count(&restart) == 1U,
          "same generation cannot be reactivated");

    init_ready_chain(&health, &accumulator, &controller, 8U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              8U,
              42U,
              130U) == ENTROPY_RESTART_OK &&
          lifecycle_state_is(
              &restart,
              ENTROPY_RESTART_STATE_READY,
              ENTROPY_RESTART_FAULT_NONE) &&
          entropy_restart_generation(&restart) == 8U &&
          entropy_restart_last_generation(&restart) == 8U,
          "newer healthy generation publishes ready after restart");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(
              &restart, &interval_config) == ENTROPY_RESTART_OK,
          "interval fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "interval fixture activates lifecycle");
    CHECK(entropy_restart_stop(&restart, 100U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 100U) == ENTROPY_RESTART_OK,
          "first restart is allowed");
    CHECK(entropy_restart_stop(&restart, 120U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 149U) ==
              ENTROPY_RESTART_INTERVAL,
          "restart before interval is rejected");
    CHECK(entropy_restart_restart(&restart, 150U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart_count(&restart) == 2U,
          "restart after interval is allowed");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(
              &restart, &zero_config) == ENTROPY_RESTART_OK,
          "zero-tick fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              0U) == ENTROPY_RESTART_OK &&
          entropy_restart_stop(&restart, 0U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 0U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart_count(&restart) == 1U,
          "first restart at zero tick is allowed");
    CHECK(entropy_restart_stop(&restart, 0U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 0U) ==
              ENTROPY_RESTART_INTERVAL,
          "second restart at zero tick honors monotonic interval");

    memset(&restart, 0, sizeof(restart));
    CHECK(entropy_restart_init(
              &restart, &exhausted_config) == ENTROPY_RESTART_OK,
          "exhausted fixture initializes lifecycle");
    init_ready_chain(&health, &accumulator, &controller, 7U, 42U);
    CHECK(entropy_restart_activate(
              &restart,
              &health,
              &accumulator,
              &controller,
              7U,
              42U,
              20U) == ENTROPY_RESTART_OK,
          "exhausted fixture activates lifecycle");
    CHECK(entropy_restart_stop(&restart, 100U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 100U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart_count(&restart) == 1U,
          "exhausted fixture consumes last restart");
    CHECK(entropy_restart_stop(&restart, 200U) == ENTROPY_RESTART_OK &&
          entropy_restart_restart(&restart, 200U) ==
              ENTROPY_RESTART_EXHAUSTED &&
          entropy_restart_restart_count(&restart) == 1U &&
          entropy_restart_stopped(&restart),
          "restart budget is bounded and exhausted");
    expect_no_allocation(
        "entropy restart stop/restart paths perform no allocation");
}

int main(void)
{
    failures = 0;
    alloc_attempts = 0U;
    free_attempts = 0U;

    test_status_strings();
    test_init_rejections();
    test_missing_device_and_activate();
    test_not_ready_promotion();
    test_failure_mapping();
    test_stop_and_restart_policy();

    if (failures != 0) {
        fprintf(stderr, "Entropy restart host tests: FAILED (%d)\n",
                failures);
        return 1;
    }
    printf("Entropy restart host tests: ALL OK\n");
    return 0;
}

#endif
