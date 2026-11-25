#include "entropy_health.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool entropy_health_generation_valid(uint32_t generation)
{
    return generation >= ENTROPY_HEALTH_GENERATION_MIN &&
           generation <= ENTROPY_HEALTH_GENERATION_MAX;
}

static bool entropy_health_config_valid(
    const struct entropy_health_config *config)
{
    return config != NULL &&
           config->version == ENTROPY_HEALTH_VERSION &&
           config->block_length_min >= ENTROPY_HEALTH_BLOCK_LENGTH_MIN &&
           config->block_length_max <= ENTROPY_HEALTH_BLOCK_LENGTH_MAX &&
           config->block_length_min <= config->block_length_max &&
           config->max_stall_ticks >= ENTROPY_HEALTH_STALL_LIMIT_MIN &&
           config->max_stall_ticks <= ENTROPY_HEALTH_STALL_LIMIT_MAX;
}

static bool entropy_health_object_valid(
    const struct entropy_health *health)
{
    return health != NULL &&
           health->initialized &&
           health->version == ENTROPY_HEALTH_VERSION &&
           entropy_health_generation_valid(health->generation) &&
           health->block_length_min >= ENTROPY_HEALTH_BLOCK_LENGTH_MIN &&
           health->block_length_max <= ENTROPY_HEALTH_BLOCK_LENGTH_MAX &&
           health->block_length_min <= health->block_length_max &&
           health->max_stall_ticks >= ENTROPY_HEALTH_STALL_LIMIT_MIN &&
           health->max_stall_ticks <= ENTROPY_HEALTH_STALL_LIMIT_MAX;
}

static void entropy_health_zeroize_previous(
    struct entropy_health *health)
{
    volatile uint8_t *bytes = health->previous_block;
    size_t index;

    for (index = 0U; index < sizeof(health->previous_block); ++index) {
        bytes[index] = 0U;
    }
    health->previous_block_length = 0U;
    health->previous_block_valid = false;
}

static bool entropy_health_previous_matches(
    const struct entropy_health *health,
    const void *block,
    uint32_t block_length)
{
    return health->previous_block_valid &&
           health->previous_block_length == block_length &&
           memcmp(health->previous_block, block, block_length) == 0;
}

static bool entropy_health_stalled(
    const struct entropy_health *health,
    uint32_t now_tick)
{
    uint32_t elapsed =
        (uint32_t)(now_tick - health->last_accept_tick);

    return elapsed > health->max_stall_ticks;
}

static void entropy_health_fail(
    struct entropy_health *health,
    enum entropy_health_fault fault)
{
    health->state = ENTROPY_HEALTH_STATE_FAILED;
    health->fault = fault;
    entropy_health_zeroize_previous(health);
}

enum entropy_health_status entropy_health_init(
    struct entropy_health *health,
    const struct entropy_health_config *config,
    uint32_t generation,
    uint32_t initial_tick)
{
    if (health == NULL || config == NULL) {
        return ENTROPY_HEALTH_NULL;
    }
    if (health->initialized) {
        return ENTROPY_HEALTH_ALREADY_PUBLISHED;
    }
    if (!entropy_health_config_valid(config)) {
        if (config->version != ENTROPY_HEALTH_VERSION) {
            return ENTROPY_HEALTH_BAD_VERSION;
        }
        if (config->block_length_min == 0U ||
            config->block_length_max >
                ENTROPY_HEALTH_BLOCK_LENGTH_MAX ||
            config->block_length_min > config->block_length_max) {
            return ENTROPY_HEALTH_BAD_LENGTH_BOUNDS;
        }
        return ENTROPY_HEALTH_BAD_STALL_LIMIT;
    }
    if (!entropy_health_generation_valid(generation)) {
        return ENTROPY_HEALTH_BAD_GENERATION;
    }

    memset(health, 0, sizeof(*health));
    health->initialized = true;
    health->version = ENTROPY_HEALTH_VERSION;
    health->generation = generation;
    health->block_length_min = config->block_length_min;
    health->block_length_max = config->block_length_max;
    health->max_stall_ticks = config->max_stall_ticks;
    health->state = ENTROPY_HEALTH_STATE_INITIALIZING;
    health->fault = ENTROPY_HEALTH_FAULT_NONE;
    health->last_accept_tick = initial_tick;
    return ENTROPY_HEALTH_OK;
}

enum entropy_health_status entropy_health_observe(
    struct entropy_health *health,
    uint32_t generation,
    const void *block,
    uint32_t block_length,
    uint32_t now_tick)
{
    if (health == NULL || block == NULL) {
        return ENTROPY_HEALTH_NULL;
    }
    if (!entropy_health_object_valid(health)) {
        return ENTROPY_HEALTH_BAD_STATE;
    }
    if (health->state == ENTROPY_HEALTH_STATE_FAILED) {
        return ENTROPY_HEALTH_BAD_STATE;
    }
    if (generation != health->generation) {
        entropy_health_fail(
            health, ENTROPY_HEALTH_FAULT_DEVICE_RESET);
        return ENTROPY_HEALTH_DEVICE_RESET;
    }
    if (block_length < health->block_length_min ||
        block_length > health->block_length_max) {
        entropy_health_fail(
            health, ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH);
        return ENTROPY_HEALTH_IMPOSSIBLE_LENGTH;
    }
    if (entropy_health_stalled(health, now_tick)) {
        entropy_health_fail(
            health, ENTROPY_HEALTH_FAULT_STALLED);
        return ENTROPY_HEALTH_STALLED;
    }
    if (entropy_health_previous_matches(
            health, block, block_length)) {
        entropy_health_fail(
            health, ENTROPY_HEALTH_FAULT_REPEATED_BLOCK);
        return ENTROPY_HEALTH_REPEATED_BLOCK;
    }

    entropy_health_zeroize_previous(health);
    memcpy(health->previous_block, block, block_length);
    health->previous_block_length = block_length;
    health->previous_block_valid = true;
    health->last_accept_tick = now_tick;
    health->state = ENTROPY_HEALTH_STATE_READY;
    health->fault = ENTROPY_HEALTH_FAULT_NONE;
    return ENTROPY_HEALTH_OK;
}

enum entropy_health_status entropy_health_progress(
    struct entropy_health *health,
    uint32_t now_tick)
{
    if (health == NULL) {
        return ENTROPY_HEALTH_NULL;
    }
    if (!entropy_health_object_valid(health)) {
        return ENTROPY_HEALTH_BAD_STATE;
    }
    if (health->state == ENTROPY_HEALTH_STATE_FAILED) {
        return ENTROPY_HEALTH_BAD_STATE;
    }
    if (entropy_health_stalled(health, now_tick)) {
        entropy_health_fail(
            health, ENTROPY_HEALTH_FAULT_STALLED);
        return ENTROPY_HEALTH_STALLED;
    }
    return ENTROPY_HEALTH_OK;
}

enum entropy_health_state entropy_health_state(
    const struct entropy_health *health)
{
    if (!entropy_health_object_valid(health)) {
        return ENTROPY_HEALTH_STATE_UNINITIALIZED;
    }
    return health->state;
}

enum entropy_health_fault entropy_health_fault(
    const struct entropy_health *health)
{
    if (!entropy_health_object_valid(health)) {
        return ENTROPY_HEALTH_FAULT_NONE;
    }
    return health->fault;
}

uint32_t entropy_health_generation(
    const struct entropy_health *health)
{
    if (!entropy_health_object_valid(health)) {
        return 0U;
    }
    return health->generation;
}

bool entropy_health_ready(
    const struct entropy_health *health)
{
    return entropy_health_state(health) ==
           ENTROPY_HEALTH_STATE_READY;
}

const char *entropy_health_status_string(
    enum entropy_health_status status)
{
    switch (status) {
    case ENTROPY_HEALTH_OK:
        return "ENTROPY_HEALTH_OK";
    case ENTROPY_HEALTH_NULL:
        return "ENTROPY_HEALTH_NULL";
    case ENTROPY_HEALTH_BAD_STATE:
        return "ENTROPY_HEALTH_BAD_STATE";
    case ENTROPY_HEALTH_ALREADY_PUBLISHED:
        return "ENTROPY_HEALTH_ALREADY_PUBLISHED";
    case ENTROPY_HEALTH_BAD_VERSION:
        return "ENTROPY_HEALTH_BAD_VERSION";
    case ENTROPY_HEALTH_BAD_GENERATION:
        return "ENTROPY_HEALTH_BAD_GENERATION";
    case ENTROPY_HEALTH_BAD_LENGTH_BOUNDS:
        return "ENTROPY_HEALTH_BAD_LENGTH_BOUNDS";
    case ENTROPY_HEALTH_BAD_STALL_LIMIT:
        return "ENTROPY_HEALTH_BAD_STALL_LIMIT";
    case ENTROPY_HEALTH_REPEATED_BLOCK:
        return "ENTROPY_HEALTH_REPEATED_BLOCK";
    case ENTROPY_HEALTH_IMPOSSIBLE_LENGTH:
        return "ENTROPY_HEALTH_IMPOSSIBLE_LENGTH";
    case ENTROPY_HEALTH_STALLED:
        return "ENTROPY_HEALTH_STALLED";
    case ENTROPY_HEALTH_DEVICE_RESET:
        return "ENTROPY_HEALTH_DEVICE_RESET";
    default:
        return NULL;
    }
}

const char *entropy_health_state_string(
    enum entropy_health_state state)
{
    switch (state) {
    case ENTROPY_HEALTH_STATE_UNINITIALIZED:
        return "ENTROPY_HEALTH_STATE_UNINITIALIZED";
    case ENTROPY_HEALTH_STATE_INITIALIZING:
        return "ENTROPY_HEALTH_STATE_INITIALIZING";
    case ENTROPY_HEALTH_STATE_READY:
        return "ENTROPY_HEALTH_STATE_READY";
    case ENTROPY_HEALTH_STATE_FAILED:
        return "ENTROPY_HEALTH_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *entropy_health_fault_string(
    enum entropy_health_fault fault)
{
    switch (fault) {
    case ENTROPY_HEALTH_FAULT_NONE:
        return "ENTROPY_HEALTH_FAULT_NONE";
    case ENTROPY_HEALTH_FAULT_REPEATED_BLOCK:
        return "ENTROPY_HEALTH_FAULT_REPEATED_BLOCK";
    case ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH:
        return "ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH";
    case ENTROPY_HEALTH_FAULT_STALLED:
        return "ENTROPY_HEALTH_FAULT_STALLED";
    case ENTROPY_HEALTH_FAULT_DEVICE_RESET:
        return "ENTROPY_HEALTH_FAULT_DEVICE_RESET";
    default:
        return NULL;
    }
}
