#include "entropy_accumulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool entropy_accumulator_generation_valid(uint32_t generation)
{
    return generation >= ENTROPY_ACCUMULATOR_GENERATION_MIN &&
           generation <= ENTROPY_ACCUMULATOR_GENERATION_MAX;
}

static bool entropy_accumulator_source_id_valid(uint32_t source_id)
{
    return source_id >= ENTROPY_ACCUMULATOR_SOURCE_ID_MIN &&
           source_id <= ENTROPY_ACCUMULATOR_SOURCE_ID_MAX;
}

static bool entropy_accumulator_config_valid(
    const struct entropy_accumulator_config *config)
{
    return config != NULL &&
           config->version == ENTROPY_ACCUMULATOR_VERSION &&
           entropy_accumulator_generation_valid(config->generation) &&
           entropy_accumulator_source_id_valid(config->source_id) &&
           config->block_length_min >=
               ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MIN &&
           config->block_length_max <=
               ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX &&
           config->block_length_min <= config->block_length_max &&
           config->max_stall_ticks >=
               ENTROPY_ACCUMULATOR_STALL_LIMIT_MIN &&
           config->max_stall_ticks <=
               ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX;
}

static bool entropy_accumulator_object_valid(
    const struct entropy_accumulator *accumulator)
{
    if (accumulator == NULL ||
        !accumulator->initialized ||
        accumulator->version != ENTROPY_ACCUMULATOR_VERSION ||
        !entropy_accumulator_generation_valid(accumulator->generation) ||
        !entropy_accumulator_source_id_valid(accumulator->source_id) ||
        accumulator->block_length_min <
            ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MIN ||
        accumulator->block_length_max >
            ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX ||
        accumulator->block_length_min > accumulator->block_length_max ||
        accumulator->max_stall_ticks <
            ENTROPY_ACCUMULATOR_STALL_LIMIT_MIN ||
        accumulator->max_stall_ticks >
            ENTROPY_ACCUMULATOR_STALL_LIMIT_MAX) {
        return false;
    }
    return accumulator->state ==
               ENTROPY_ACCUMULATOR_STATE_INITIALIZING ||
           accumulator->state ==
               ENTROPY_ACCUMULATOR_STATE_ACCUMULATING ||
           accumulator->state == ENTROPY_ACCUMULATOR_STATE_FAILED;
}

static uint8_t entropy_accumulator_rotl8(
    uint8_t value,
    unsigned int shift)
{
    shift &= 7U;
    if (shift == 0U) {
        return value;
    }
    return (uint8_t)(
        (value << shift) | (value >> (8U - shift)));
}

static void entropy_accumulator_zeroize_bytes(
    uint8_t *bytes,
    size_t length)
{
    volatile uint8_t *scratch = bytes;
    size_t index;

    for (index = 0U; index < length; ++index) {
        scratch[index] = 0U;
    }
}

static void entropy_accumulator_zeroize_object(
    struct entropy_accumulator *accumulator)
{
    entropy_accumulator_zeroize_bytes(
        accumulator->bytes, sizeof(accumulator->bytes));
    memset(&accumulator->source, 0, sizeof(accumulator->source));
}

static void entropy_accumulator_fail(
    struct entropy_accumulator *accumulator,
    enum entropy_accumulator_fault fault)
{
    entropy_accumulator_zeroize_object(accumulator);
    accumulator->state = ENTROPY_ACCUMULATOR_STATE_FAILED;
    accumulator->fault = fault;
}

static bool entropy_accumulator_stalled(
    const struct entropy_accumulator *accumulator,
    uint32_t now_tick)
{
    uint32_t elapsed =
        (uint32_t)(now_tick - accumulator->source.last_accept_tick);

    return elapsed > accumulator->max_stall_ticks;
}

static bool entropy_accumulator_overlaps(
    const void *block,
    uint32_t block_length,
    const struct entropy_accumulator *accumulator)
{
    uintptr_t block_start = (uintptr_t)block;
    uintptr_t block_end;
    uintptr_t object_start = (uintptr_t)accumulator;
    uintptr_t object_end =
        object_start + sizeof(struct entropy_accumulator);

    if (block_start > (uintptr_t)-1 - block_length) {
        return true;
    }
    block_end = block_start + block_length;
    return block_start < object_end && object_start < block_end;
}

static void entropy_accumulator_mix_block(
    struct entropy_accumulator *accumulator,
    const uint8_t *block,
    uint32_t block_length)
{
    uint8_t scratch[ENTROPY_ACCUMULATOR_SCRATCH_SIZE];
    uint8_t carry = 0U;
    uint32_t offset;
    uint32_t index;

    for (offset = 0U; offset < block_length;
         offset += ENTROPY_ACCUMULATOR_SCRATCH_SIZE) {
        uint32_t chunk = block_length - offset;
        uint32_t scratch_index;

        if (chunk > ENTROPY_ACCUMULATOR_SCRATCH_SIZE) {
            chunk = ENTROPY_ACCUMULATOR_SCRATCH_SIZE;
        }
        memcpy(scratch, block + offset, chunk);
        for (scratch_index = 0U; scratch_index < chunk;
             ++scratch_index) {
            uint32_t round =
                accumulator->source.credited_blocks & 7U;
            uint32_t target =
                (offset + scratch_index) %
                ENTROPY_ACCUMULATOR_CAPACITY;
            uint8_t mixed = entropy_accumulator_rotl8(
                (uint8_t)(
                    scratch[scratch_index] ^
                    (uint8_t)(round * 17U)),
                scratch_index & 7U);

            accumulator->bytes[target] ^= mixed;
        }
        entropy_accumulator_zeroize_bytes(
            scratch, chunk);
    }

    for (index = 0U; index < ENTROPY_ACCUMULATOR_CAPACITY;
         ++index) {
        uint32_t source_index =
            (index + 17U) % ENTROPY_ACCUMULATOR_CAPACITY;
        uint8_t current = accumulator->bytes[index];
        uint8_t mixed = (uint8_t)(
            accumulator->bytes[index] ^
            accumulator->bytes[source_index] ^
            carry);

        accumulator->bytes[index] = entropy_accumulator_rotl8(
            mixed, 1U + (index & 7U));
        carry = current;
    }
    entropy_accumulator_zeroize_bytes(&carry, sizeof(carry));
}

enum entropy_accumulator_status entropy_accumulator_init(
    struct entropy_accumulator *accumulator,
    const struct entropy_accumulator_config *config,
    uint32_t initial_tick)
{
    if (accumulator == NULL || config == NULL) {
        return ENTROPY_ACCUMULATOR_NULL;
    }
    if (accumulator->initialized) {
        return ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED;
    }
    if (!entropy_accumulator_config_valid(config)) {
        if (config->version != ENTROPY_ACCUMULATOR_VERSION) {
            return ENTROPY_ACCUMULATOR_BAD_VERSION;
        }
        if (!entropy_accumulator_generation_valid(
                config->generation)) {
            return ENTROPY_ACCUMULATOR_BAD_GENERATION;
        }
        if (!entropy_accumulator_source_id_valid(
                config->source_id)) {
            return ENTROPY_ACCUMULATOR_BAD_SOURCE_ID;
        }
        if (config->block_length_min <
                ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MIN ||
            config->block_length_max >
                ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX ||
            config->block_length_min > config->block_length_max) {
            return ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS;
        }
        return ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT;
    }

    memset(accumulator, 0, sizeof(*accumulator));
    accumulator->initialized = true;
    accumulator->version = ENTROPY_ACCUMULATOR_VERSION;
    accumulator->generation = config->generation;
    accumulator->source_id = config->source_id;
    accumulator->block_length_min = config->block_length_min;
    accumulator->block_length_max = config->block_length_max;
    accumulator->max_stall_ticks = config->max_stall_ticks;
    accumulator->state = ENTROPY_ACCUMULATOR_STATE_INITIALIZING;
    accumulator->fault = ENTROPY_ACCUMULATOR_FAULT_NONE;
    accumulator->source.last_accept_tick = initial_tick;
    return ENTROPY_ACCUMULATOR_OK;
}

enum entropy_accumulator_status entropy_accumulator_mix(
    struct entropy_accumulator *accumulator,
    uint32_t generation,
    uint32_t source_id,
    const void *block,
    uint32_t block_length,
    uint32_t now_tick)
{
    if (accumulator == NULL || block == NULL) {
        return ENTROPY_ACCUMULATOR_NULL;
    }
    if (!entropy_accumulator_object_valid(accumulator) ||
        accumulator->state == ENTROPY_ACCUMULATOR_STATE_FAILED) {
        return ENTROPY_ACCUMULATOR_BAD_STATE;
    }
    if (generation != accumulator->generation) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH);
        return ENTROPY_ACCUMULATOR_GENERATION_MISMATCH;
    }
    if (source_id != accumulator->source_id) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH);
        return ENTROPY_ACCUMULATOR_SOURCE_MISMATCH;
    }
    if (block_length < accumulator->block_length_min ||
        block_length > accumulator->block_length_max) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH);
        return ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH;
    }
    if (entropy_accumulator_overlaps(
            block, block_length, accumulator)) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK);
        return ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK;
    }
    if (entropy_accumulator_stalled(accumulator, now_tick)) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_STALLED);
        return ENTROPY_ACCUMULATOR_STALLED;
    }
    if (accumulator->source.credited_bytes >
        UINT64_MAX - block_length) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW);
        return ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW;
    }

    entropy_accumulator_mix_block(
        accumulator, (const uint8_t *)block, block_length);
    accumulator->source.active = true;
    accumulator->source.generation = accumulator->generation;
    accumulator->source.source_id = accumulator->source_id;
    accumulator->source.credited_bytes += block_length;
    accumulator->source.credited_blocks += 1U;
    accumulator->source.last_accept_tick = now_tick;
    accumulator->state = ENTROPY_ACCUMULATOR_STATE_ACCUMULATING;
    accumulator->fault = ENTROPY_ACCUMULATOR_FAULT_NONE;
    return ENTROPY_ACCUMULATOR_OK;
}

enum entropy_accumulator_status entropy_accumulator_progress(
    struct entropy_accumulator *accumulator,
    uint32_t now_tick)
{
    if (accumulator == NULL) {
        return ENTROPY_ACCUMULATOR_NULL;
    }
    if (!entropy_accumulator_object_valid(accumulator) ||
        accumulator->state == ENTROPY_ACCUMULATOR_STATE_FAILED) {
        return ENTROPY_ACCUMULATOR_BAD_STATE;
    }
    if (entropy_accumulator_stalled(accumulator, now_tick)) {
        entropy_accumulator_fail(
            accumulator,
            ENTROPY_ACCUMULATOR_FAULT_STALLED);
        return ENTROPY_ACCUMULATOR_STALLED;
    }
    return ENTROPY_ACCUMULATOR_OK;
}

enum entropy_accumulator_state entropy_accumulator_state(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED;
    }
    return accumulator->state;
}

enum entropy_accumulator_fault entropy_accumulator_fault(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return ENTROPY_ACCUMULATOR_FAULT_NONE;
    }
    return accumulator->fault;
}

bool entropy_accumulator_initialized(
    const struct entropy_accumulator *accumulator)
{
    return entropy_accumulator_object_valid(accumulator);
}

bool entropy_accumulator_accumulating(
    const struct entropy_accumulator *accumulator)
{
    return entropy_accumulator_object_valid(accumulator) &&
           accumulator->state ==
               ENTROPY_ACCUMULATOR_STATE_ACCUMULATING;
}

uint32_t entropy_accumulator_generation(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return 0U;
    }
    return accumulator->generation;
}

uint32_t entropy_accumulator_source_id(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return 0U;
    }
    return accumulator->source_id;
}

uint64_t entropy_accumulator_credited_bytes(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return 0U;
    }
    return accumulator->source.credited_bytes;
}

uint32_t entropy_accumulator_credited_blocks(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return 0U;
    }
    return accumulator->source.credited_blocks;
}

uint32_t entropy_accumulator_last_accept_tick(
    const struct entropy_accumulator *accumulator)
{
    if (!entropy_accumulator_object_valid(accumulator)) {
        return 0U;
    }
    return accumulator->source.last_accept_tick;
}

const char *entropy_accumulator_status_string(
    enum entropy_accumulator_status status)
{
    switch (status) {
    case ENTROPY_ACCUMULATOR_OK:
        return "ENTROPY_ACCUMULATOR_OK";
    case ENTROPY_ACCUMULATOR_NULL:
        return "ENTROPY_ACCUMULATOR_NULL";
    case ENTROPY_ACCUMULATOR_BAD_STATE:
        return "ENTROPY_ACCUMULATOR_BAD_STATE";
    case ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED:
        return "ENTROPY_ACCUMULATOR_ALREADY_PUBLISHED";
    case ENTROPY_ACCUMULATOR_BAD_VERSION:
        return "ENTROPY_ACCUMULATOR_BAD_VERSION";
    case ENTROPY_ACCUMULATOR_BAD_GENERATION:
        return "ENTROPY_ACCUMULATOR_BAD_GENERATION";
    case ENTROPY_ACCUMULATOR_BAD_SOURCE_ID:
        return "ENTROPY_ACCUMULATOR_BAD_SOURCE_ID";
    case ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS:
        return "ENTROPY_ACCUMULATOR_BAD_LENGTH_BOUNDS";
    case ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT:
        return "ENTROPY_ACCUMULATOR_BAD_STALL_LIMIT";
    case ENTROPY_ACCUMULATOR_GENERATION_MISMATCH:
        return "ENTROPY_ACCUMULATOR_GENERATION_MISMATCH";
    case ENTROPY_ACCUMULATOR_SOURCE_MISMATCH:
        return "ENTROPY_ACCUMULATOR_SOURCE_MISMATCH";
    case ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH:
        return "ENTROPY_ACCUMULATOR_IMPOSSIBLE_LENGTH";
    case ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK:
        return "ENTROPY_ACCUMULATOR_OVERLAPPING_BLOCK";
    case ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW:
        return "ENTROPY_ACCUMULATOR_CREDIT_OVERFLOW";
    case ENTROPY_ACCUMULATOR_STALLED:
        return "ENTROPY_ACCUMULATOR_STALLED";
    default:
        return NULL;
    }
}

const char *entropy_accumulator_state_string(
    enum entropy_accumulator_state state)
{
    switch (state) {
    case ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED:
        return "ENTROPY_ACCUMULATOR_STATE_UNINITIALIZED";
    case ENTROPY_ACCUMULATOR_STATE_INITIALIZING:
        return "ENTROPY_ACCUMULATOR_STATE_INITIALIZING";
    case ENTROPY_ACCUMULATOR_STATE_ACCUMULATING:
        return "ENTROPY_ACCUMULATOR_STATE_ACCUMULATING";
    case ENTROPY_ACCUMULATOR_STATE_FAILED:
        return "ENTROPY_ACCUMULATOR_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *entropy_accumulator_fault_string(
    enum entropy_accumulator_fault fault)
{
    switch (fault) {
    case ENTROPY_ACCUMULATOR_FAULT_NONE:
        return "ENTROPY_ACCUMULATOR_FAULT_NONE";
    case ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH:
        return "ENTROPY_ACCUMULATOR_FAULT_GENERATION_MISMATCH";
    case ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH:
        return "ENTROPY_ACCUMULATOR_FAULT_SOURCE_MISMATCH";
    case ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH:
        return "ENTROPY_ACCUMULATOR_FAULT_IMPOSSIBLE_LENGTH";
    case ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK:
        return "ENTROPY_ACCUMULATOR_FAULT_OVERLAPPING_BLOCK";
    case ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW:
        return "ENTROPY_ACCUMULATOR_FAULT_CREDIT_OVERFLOW";
    case ENTROPY_ACCUMULATOR_FAULT_STALLED:
        return "ENTROPY_ACCUMULATOR_FAULT_STALLED";
    default:
        return NULL;
    }
}
