#include "entropy_restart.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool entropy_restart_generation_valid(uint32_t generation)
{
    return generation >= ENTROPY_RESTART_GENERATION_MIN &&
           generation <= ENTROPY_RESTART_GENERATION_MAX;
}

static bool entropy_restart_source_id_valid(uint32_t source_id)
{
    return source_id >= ENTROPY_RESTART_SOURCE_ID_MIN &&
           source_id <= ENTROPY_RESTART_SOURCE_ID_MAX;
}

static bool entropy_restart_config_valid(
    const struct entropy_restart_config *config)
{
    return config != NULL &&
           config->version == ENTROPY_RESTART_VERSION &&
           config->restart_count_max >= 1U &&
           config->restart_count_max <=
               ENTROPY_RESTART_RESTART_COUNT_MAX &&
           config->min_restart_interval_ticks >=
               ENTROPY_RESTART_INTERVAL_MIN_TICKS &&
           config->min_restart_interval_ticks <=
               ENTROPY_RESTART_INTERVAL_MAX_TICKS;
}

static bool entropy_restart_state_valid(
    enum entropy_restart_state state)
{
    return state == ENTROPY_RESTART_STATE_UNAVAILABLE ||
           state == ENTROPY_RESTART_STATE_READY ||
           state == ENTROPY_RESTART_STATE_FAILED ||
           state == ENTROPY_RESTART_STATE_STOPPED;
}

static bool entropy_restart_object_valid(
    const struct entropy_restart *restart)
{
    return restart != NULL &&
           restart->initialized &&
           restart->version == ENTROPY_RESTART_VERSION &&
           restart->restart_count_max >= 1U &&
           restart->restart_count_max <=
               ENTROPY_RESTART_RESTART_COUNT_MAX &&
           restart->min_restart_interval_ticks >=
               ENTROPY_RESTART_INTERVAL_MIN_TICKS &&
           restart->min_restart_interval_ticks <=
               ENTROPY_RESTART_INTERVAL_MAX_TICKS &&
           restart->restart_count <= restart->restart_count_max &&
           entropy_restart_state_valid(restart->state) &&
           (restart->generation == 0U ||
            entropy_restart_generation_valid(restart->generation)) &&
           (restart->source_id == 0U ||
            entropy_restart_source_id_valid(restart->source_id)) &&
           (restart->last_generation == 0U ||
            entropy_restart_generation_valid(
                restart->last_generation));
}

static void entropy_restart_zeroize_object(
    void *object,
    size_t size)
{
    volatile uint8_t *bytes = (volatile uint8_t *)object;
    size_t index;

    for (index = 0U; index < size; ++index) {
        bytes[index] = 0U;
    }
}

static void entropy_restart_fail(
    struct entropy_restart *restart,
    enum entropy_restart_fault fault,
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *seed_fill)
{
    if (health != NULL) {
        entropy_restart_zeroize_object(
            health, sizeof(*health));
    }
    if (accumulator != NULL) {
        entropy_restart_zeroize_object(
            accumulator, sizeof(*accumulator));
    }
    if (seed_fill != NULL) {
        entropy_restart_zeroize_object(
            seed_fill, sizeof(*seed_fill));
    }

    restart->generation = 0U;
    restart->source_id = 0U;
    restart->health = NULL;
    restart->accumulator = NULL;
    restart->seed_fill = NULL;
    restart->state = ENTROPY_RESTART_STATE_FAILED;
    restart->fault = fault;
}

static enum entropy_restart_status
entropy_restart_health_failure(
    enum entropy_health_fault fault,
    enum entropy_restart_fault *restart_fault)
{
    switch (fault) {
    case ENTROPY_HEALTH_FAULT_REPEATED_BLOCK:
        *restart_fault = ENTROPY_RESTART_FAULT_REPEATED_OUTPUT;
        return ENTROPY_RESTART_REPEATED_OUTPUT;
    case ENTROPY_HEALTH_FAULT_STALLED:
        *restart_fault = ENTROPY_RESTART_FAULT_STALLED;
        return ENTROPY_RESTART_STALLED;
    case ENTROPY_HEALTH_FAULT_DEVICE_RESET:
        *restart_fault = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    default:
        *restart_fault = ENTROPY_RESTART_FAULT_UNHEALTHY;
        return ENTROPY_RESTART_UNHEALTHY;
    }
}

static enum entropy_restart_status
entropy_restart_accumulator_failure(
    enum entropy_accumulator_fault fault,
    enum entropy_restart_fault *restart_fault)
{
    switch (fault) {
    case ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH:
        *restart_fault = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    case ENTROPY_ACCUMULATOR_FAULT_STALLED:
        *restart_fault = ENTROPY_RESTART_FAULT_STALLED;
        return ENTROPY_RESTART_STALLED;
    default:
        *restart_fault = ENTROPY_RESTART_FAULT_UNHEALTHY;
        return ENTROPY_RESTART_UNHEALTHY;
    }
}

static enum entropy_restart_status
entropy_restart_seed_failure(
    enum entropy_seed_fill_fault fault,
    enum entropy_restart_fault *restart_fault)
{
    switch (fault) {
    case ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH:
        *restart_fault = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    default:
        *restart_fault = ENTROPY_RESTART_FAULT_UNHEALTHY;
        return ENTROPY_RESTART_UNHEALTHY;
    }
}

static enum entropy_restart_status
entropy_restart_chain_status(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *seed_fill,
    uint32_t expected_generation,
    uint32_t expected_source_id,
    enum entropy_restart_fault *fault_out)
{
    uint32_t health_generation;
    uint32_t accumulator_generation;
    uint32_t seed_generation;
    uint32_t accumulator_source_id;
    uint32_t seed_source_id;

    if (health == NULL ||
        accumulator == NULL ||
        seed_fill == NULL ||
        entropy_health_state(health) ==
            ENTROPY_HEALTH_STATE_UNINITIALIZED ||
        !entropy_accumulator_initialized(accumulator) ||
        !entropy_seed_fill_initialized(seed_fill)) {
        *fault_out = ENTROPY_RESTART_FAULT_MISSING_DEVICE;
        return ENTROPY_RESTART_MISSING_DEVICE;
    }

    health_generation = entropy_health_generation(health);
    accumulator_generation =
        entropy_accumulator_generation(accumulator);
    seed_generation = entropy_seed_fill_generation(seed_fill);
    accumulator_source_id =
        entropy_accumulator_source_id(accumulator);
    seed_source_id = entropy_seed_fill_source_id(seed_fill);

    if (health_generation == 0U ||
        accumulator_generation == 0U ||
        seed_generation == 0U ||
        accumulator_source_id == 0U ||
        seed_source_id == 0U ||
        health_generation != accumulator_generation ||
        health_generation != seed_generation ||
        accumulator_source_id != seed_source_id) {
        *fault_out = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    }

    if (expected_generation != 0U &&
        (health_generation != expected_generation ||
         accumulator_source_id != expected_source_id)) {
        *fault_out = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    }
    if (expected_source_id != 0U &&
        seed_source_id != expected_source_id) {
        *fault_out = ENTROPY_RESTART_FAULT_DEVICE_RESET;
        return ENTROPY_RESTART_DEVICE_RESET;
    }

    if (entropy_health_state(health) ==
        ENTROPY_HEALTH_STATE_FAILED) {
        return entropy_restart_health_failure(
            entropy_health_fault(health), fault_out);
    }
    if (entropy_accumulator_state(accumulator) ==
        ENTROPY_ACCUMULATOR_STATE_FAILED) {
        return entropy_restart_accumulator_failure(
            entropy_accumulator_fault(accumulator), fault_out);
    }
    if (entropy_seed_fill_state(seed_fill) ==
        ENTROPY_SEED_FILL_STATE_FAILED) {
        return entropy_restart_seed_failure(
            entropy_seed_fill_fault(seed_fill), fault_out);
    }

    *fault_out = ENTROPY_RESTART_FAULT_NONE;
    return ENTROPY_RESTART_OK;
}

static bool entropy_restart_chain_ready(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *seed_fill)
{
    return entropy_health_state(health) ==
               ENTROPY_HEALTH_STATE_READY &&
           entropy_accumulator_state(accumulator) ==
               ENTROPY_ACCUMULATOR_STATE_ACCUMULATING &&
           entropy_seed_fill_state(seed_fill) ==
               ENTROPY_SEED_FILL_STATE_READY;
}

enum entropy_restart_status entropy_restart_init(
    struct entropy_restart *restart,
    const struct entropy_restart_config *config)
{
    if (restart == NULL || config == NULL) {
        return ENTROPY_RESTART_NULL;
    }
    if (restart->initialized) {
        return ENTROPY_RESTART_ALREADY_PUBLISHED;
    }
    if (!entropy_restart_config_valid(config)) {
        if (config->version != ENTROPY_RESTART_VERSION) {
            return ENTROPY_RESTART_BAD_VERSION;
        }
        if (config->restart_count_max < 1U ||
            config->restart_count_max >
                ENTROPY_RESTART_RESTART_COUNT_MAX) {
            return ENTROPY_RESTART_BAD_RESTART_COUNT;
        }
        return ENTROPY_RESTART_BAD_INTERVAL;
    }

    entropy_restart_zeroize_object(
        restart, sizeof(*restart));
    restart->initialized = true;
    restart->version = ENTROPY_RESTART_VERSION;
    restart->restart_count_max = config->restart_count_max;
    restart->min_restart_interval_ticks =
        config->min_restart_interval_ticks;
    restart->state = ENTROPY_RESTART_STATE_UNAVAILABLE;
    restart->fault = ENTROPY_RESTART_FAULT_NONE;
    return ENTROPY_RESTART_OK;
}

enum entropy_restart_status entropy_restart_activate(
    struct entropy_restart *restart,
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *seed_fill,
    uint32_t generation,
    uint32_t source_id,
    uint32_t now_tick)
{
    enum entropy_restart_fault fault;
    enum entropy_restart_status status;

    (void)now_tick;

    if (restart == NULL) {
        return ENTROPY_RESTART_NULL;
    }
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->state == ENTROPY_RESTART_STATE_READY) {
        return ENTROPY_RESTART_ALREADY_PUBLISHED;
    }
    if (restart->state != ENTROPY_RESTART_STATE_UNAVAILABLE) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (!entropy_restart_generation_valid(generation)) {
        return ENTROPY_RESTART_BAD_GENERATION;
    }
    if (!entropy_restart_source_id_valid(source_id)) {
        return ENTROPY_RESTART_BAD_SOURCE_ID;
    }
    if (restart->last_generation != 0U &&
        generation <= restart->last_generation) {
        return ENTROPY_RESTART_STALE_GENERATION;
    }

    status = entropy_restart_chain_status(
        health,
        accumulator,
        seed_fill,
        generation,
        source_id,
        &fault);
    if (status != ENTROPY_RESTART_OK) {
        entropy_restart_fail(
            restart, fault, health, accumulator, seed_fill);
        return status;
    }

    restart->health = health;
    restart->accumulator = accumulator;
    restart->seed_fill = seed_fill;
    restart->generation = generation;
    restart->source_id = source_id;
    if (!entropy_restart_chain_ready(
            health, accumulator, seed_fill)) {
        restart->state = ENTROPY_RESTART_STATE_UNAVAILABLE;
        restart->fault = ENTROPY_RESTART_FAULT_NONE;
        return ENTROPY_RESTART_NOT_READY;
    }

    restart->last_generation = generation;
    restart->state = ENTROPY_RESTART_STATE_READY;
    restart->fault = ENTROPY_RESTART_FAULT_NONE;
    return ENTROPY_RESTART_OK;
}

enum entropy_restart_status entropy_restart_check(
    struct entropy_restart *restart,
    uint32_t now_tick)
{
    enum entropy_restart_fault fault;
    enum entropy_restart_status status;

    (void)now_tick;

    if (restart == NULL) {
        return ENTROPY_RESTART_NULL;
    }
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->state == ENTROPY_RESTART_STATE_FAILED ||
        restart->state == ENTROPY_RESTART_STATE_STOPPED) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->health == NULL &&
        restart->accumulator == NULL &&
        restart->seed_fill == NULL &&
        restart->generation == 0U) {
        return ENTROPY_RESTART_OK;
    }

    status = entropy_restart_chain_status(
        restart->health,
        restart->accumulator,
        restart->seed_fill,
        restart->generation,
        restart->source_id,
        &fault);
    if (status != ENTROPY_RESTART_OK) {
        entropy_restart_fail(
            restart,
            fault,
            restart->health,
            restart->accumulator,
            restart->seed_fill);
        return status;
    }

    if (entropy_restart_chain_ready(
            restart->health,
            restart->accumulator,
            restart->seed_fill)) {
        if (restart->generation > restart->last_generation) {
            restart->last_generation = restart->generation;
        }
        restart->state = ENTROPY_RESTART_STATE_READY;
        restart->fault = ENTROPY_RESTART_FAULT_NONE;
    } else {
        restart->state = ENTROPY_RESTART_STATE_UNAVAILABLE;
        restart->fault = ENTROPY_RESTART_FAULT_NONE;
    }
    return ENTROPY_RESTART_OK;
}

enum entropy_restart_status entropy_restart_stop(
    struct entropy_restart *restart,
    uint32_t now_tick)
{
    (void)now_tick;

    if (restart == NULL) {
        return ENTROPY_RESTART_NULL;
    }
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->state == ENTROPY_RESTART_STATE_STOPPED) {
        return ENTROPY_RESTART_OK;
    }

    entropy_restart_fail(
        restart,
        ENTROPY_RESTART_FAULT_SHUTDOWN,
        restart->health,
        restart->accumulator,
        restart->seed_fill);
    restart->state = ENTROPY_RESTART_STATE_STOPPED;
    restart->fault = ENTROPY_RESTART_FAULT_SHUTDOWN;
    return ENTROPY_RESTART_OK;
}

enum entropy_restart_status entropy_restart_restart(
    struct entropy_restart *restart,
    uint32_t now_tick)
{
    uint32_t elapsed;

    if (restart == NULL) {
        return ENTROPY_RESTART_NULL;
    }
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->state != ENTROPY_RESTART_STATE_FAILED &&
        restart->state != ENTROPY_RESTART_STATE_STOPPED) {
        return ENTROPY_RESTART_BAD_STATE;
    }
    if (restart->restart_count >= restart->restart_count_max) {
        return ENTROPY_RESTART_EXHAUSTED;
    }

    elapsed = (uint32_t)(now_tick - restart->last_restart_tick);
    if (restart->restart_count != 0U &&
        elapsed < restart->min_restart_interval_ticks) {
        return ENTROPY_RESTART_INTERVAL;
    }

    ++restart->restart_count;
    restart->last_restart_tick = now_tick;
    restart->generation = 0U;
    restart->source_id = 0U;
    restart->health = NULL;
    restart->accumulator = NULL;
    restart->seed_fill = NULL;
    restart->state = ENTROPY_RESTART_STATE_UNAVAILABLE;
    restart->fault = ENTROPY_RESTART_FAULT_NONE;
    return ENTROPY_RESTART_OK;
}

bool entropy_restart_initialized(
    const struct entropy_restart *restart)
{
    return entropy_restart_object_valid(restart);
}

bool entropy_restart_ready(
    const struct entropy_restart *restart)
{
    return entropy_restart_state(restart) ==
           ENTROPY_RESTART_STATE_READY;
}

bool entropy_restart_stopped(
    const struct entropy_restart *restart)
{
    return entropy_restart_state(restart) ==
           ENTROPY_RESTART_STATE_STOPPED;
}

enum entropy_restart_state entropy_restart_state(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_STATE_UNINITIALIZED;
    }
    return restart->state;
}

enum entropy_restart_fault entropy_restart_fault(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return ENTROPY_RESTART_FAULT_NONE;
    }
    return restart->fault;
}

uint32_t entropy_restart_generation(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return 0U;
    }
    return restart->generation;
}

uint32_t entropy_restart_last_generation(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return 0U;
    }
    return restart->last_generation;
}

uint32_t entropy_restart_restart_count(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return 0U;
    }
    return restart->restart_count;
}

uint32_t entropy_restart_last_restart_tick(
    const struct entropy_restart *restart)
{
    if (!entropy_restart_object_valid(restart)) {
        return 0U;
    }
    return restart->last_restart_tick;
}

const char *entropy_restart_status_string(
    enum entropy_restart_status status)
{
    switch (status) {
    case ENTROPY_RESTART_OK:
        return "ENTROPY_RESTART_OK";
    case ENTROPY_RESTART_NULL:
        return "ENTROPY_RESTART_NULL";
    case ENTROPY_RESTART_BAD_STATE:
        return "ENTROPY_RESTART_BAD_STATE";
    case ENTROPY_RESTART_ALREADY_PUBLISHED:
        return "ENTROPY_RESTART_ALREADY_PUBLISHED";
    case ENTROPY_RESTART_BAD_VERSION:
        return "ENTROPY_RESTART_BAD_VERSION";
    case ENTROPY_RESTART_BAD_RESTART_COUNT:
        return "ENTROPY_RESTART_BAD_RESTART_COUNT";
    case ENTROPY_RESTART_BAD_INTERVAL:
        return "ENTROPY_RESTART_BAD_INTERVAL";
    case ENTROPY_RESTART_BAD_GENERATION:
        return "ENTROPY_RESTART_BAD_GENERATION";
    case ENTROPY_RESTART_BAD_SOURCE_ID:
        return "ENTROPY_RESTART_BAD_SOURCE_ID";
    case ENTROPY_RESTART_MISSING_DEVICE:
        return "ENTROPY_RESTART_MISSING_DEVICE";
    case ENTROPY_RESTART_STALLED:
        return "ENTROPY_RESTART_STALLED";
    case ENTROPY_RESTART_REPEATED_OUTPUT:
        return "ENTROPY_RESTART_REPEATED_OUTPUT";
    case ENTROPY_RESTART_DEVICE_RESET:
        return "ENTROPY_RESTART_DEVICE_RESET";
    case ENTROPY_RESTART_UNHEALTHY:
        return "ENTROPY_RESTART_UNHEALTHY";
    case ENTROPY_RESTART_NOT_READY:
        return "ENTROPY_RESTART_NOT_READY";
    case ENTROPY_RESTART_STALE_GENERATION:
        return "ENTROPY_RESTART_STALE_GENERATION";
    case ENTROPY_RESTART_INTERVAL:
        return "ENTROPY_RESTART_INTERVAL";
    case ENTROPY_RESTART_EXHAUSTED:
        return "ENTROPY_RESTART_EXHAUSTED";
    default:
        return NULL;
    }
}

const char *entropy_restart_state_string(
    enum entropy_restart_state state)
{
    switch (state) {
    case ENTROPY_RESTART_STATE_UNINITIALIZED:
        return "ENTROPY_RESTART_STATE_UNINITIALIZED";
    case ENTROPY_RESTART_STATE_UNAVAILABLE:
        return "ENTROPY_RESTART_STATE_UNAVAILABLE";
    case ENTROPY_RESTART_STATE_READY:
        return "ENTROPY_RESTART_STATE_READY";
    case ENTROPY_RESTART_STATE_FAILED:
        return "ENTROPY_RESTART_STATE_FAILED";
    case ENTROPY_RESTART_STATE_STOPPED:
        return "ENTROPY_RESTART_STATE_STOPPED";
    default:
        return NULL;
    }
}

const char *entropy_restart_fault_string(
    enum entropy_restart_fault fault)
{
    switch (fault) {
    case ENTROPY_RESTART_FAULT_NONE:
        return "ENTROPY_RESTART_FAULT_NONE";
    case ENTROPY_RESTART_FAULT_MISSING_DEVICE:
        return "ENTROPY_RESTART_FAULT_MISSING_DEVICE";
    case ENTROPY_RESTART_FAULT_STALLED:
        return "ENTROPY_RESTART_FAULT_STALLED";
    case ENTROPY_RESTART_FAULT_REPEATED_OUTPUT:
        return "ENTROPY_RESTART_FAULT_REPEATED_OUTPUT";
    case ENTROPY_RESTART_FAULT_DEVICE_RESET:
        return "ENTROPY_RESTART_FAULT_DEVICE_RESET";
    case ENTROPY_RESTART_FAULT_UNHEALTHY:
        return "ENTROPY_RESTART_FAULT_UNHEALTHY";
    case ENTROPY_RESTART_FAULT_SHUTDOWN:
        return "ENTROPY_RESTART_FAULT_SHUTDOWN";
    default:
        return NULL;
    }
}
