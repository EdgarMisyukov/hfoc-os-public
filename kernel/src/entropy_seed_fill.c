#include "entropy_seed_fill.h"

#include "entropy_accumulator.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool entropy_seed_fill_generation_valid(uint32_t generation)
{
    return generation >= ENTROPY_SEED_FILL_GENERATION_MIN &&
           generation <= ENTROPY_SEED_FILL_GENERATION_MAX;
}

static bool entropy_seed_fill_source_id_valid(uint32_t source_id)
{
    return source_id >= ENTROPY_SEED_FILL_SOURCE_ID_MIN &&
           source_id <= ENTROPY_SEED_FILL_SOURCE_ID_MAX;
}

static bool entropy_seed_fill_config_valid(
    const struct entropy_seed_fill_config *config)
{
    return config != NULL &&
           config->version == ENTROPY_SEED_FILL_VERSION &&
           entropy_seed_fill_generation_valid(config->generation) &&
           entropy_seed_fill_source_id_valid(config->source_id) &&
           config->required_credited_bytes >=
               ENTROPY_SEED_FILL_REQUIRED_BYTES_MIN &&
           config->required_credited_bytes <=
               ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX &&
           config->seed_length >= ENTROPY_SEED_FILL_SEED_LENGTH_MIN &&
           config->seed_length <= ENTROPY_SEED_FILL_SEED_LENGTH_MAX &&
           config->fill_length_max >= 1U &&
           config->fill_length_max <= ENTROPY_SEED_FILL_FILL_LENGTH_MAX &&
           config->fill_count_max >= 1U &&
           config->fill_count_max <= ENTROPY_SEED_FILL_FILL_COUNT_MAX;
}

static bool entropy_seed_fill_object_valid(
    const struct entropy_seed_fill *controller)
{
    return controller != NULL &&
           controller->initialized &&
           controller->version == ENTROPY_SEED_FILL_VERSION &&
           entropy_seed_fill_generation_valid(controller->generation) &&
           entropy_seed_fill_source_id_valid(controller->source_id) &&
           controller->required_credited_bytes >=
               ENTROPY_SEED_FILL_REQUIRED_BYTES_MIN &&
           controller->required_credited_bytes <=
               ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX &&
           controller->seed_length >= ENTROPY_SEED_FILL_SEED_LENGTH_MIN &&
           controller->seed_length <= ENTROPY_SEED_FILL_SEED_LENGTH_MAX &&
           controller->fill_length_max >= 1U &&
           controller->fill_length_max <=
               ENTROPY_SEED_FILL_FILL_LENGTH_MAX &&
           controller->fill_count_max >= 1U &&
           controller->fill_count_max <=
               ENTROPY_SEED_FILL_FILL_COUNT_MAX &&
           (controller->state == ENTROPY_SEED_FILL_STATE_UNAVAILABLE ||
            controller->state == ENTROPY_SEED_FILL_STATE_DEGRADED ||
            controller->state == ENTROPY_SEED_FILL_STATE_READY ||
            controller->state == ENTROPY_SEED_FILL_STATE_FAILED);
}

static bool entropy_seed_fill_accumulator_ready(
    const struct entropy_accumulator *accumulator)
{
    return entropy_accumulator_initialized(accumulator) &&
           entropy_accumulator_accumulating(accumulator);
}

static bool entropy_seed_fill_accumulator_matches(
    const struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator)
{
    return entropy_accumulator_generation(accumulator) ==
               controller->generation &&
           entropy_accumulator_source_id(accumulator) ==
               controller->source_id;
}

static bool entropy_seed_fill_accumulator_enough(
    const struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator)
{
    return entropy_accumulator_credited_bytes(accumulator) >=
           controller->required_credited_bytes;
}

static bool entropy_seed_fill_ranges_overlap(
    const void *first,
    size_t first_length,
    const void *second,
    size_t second_length)
{
    uintptr_t first_start = (uintptr_t)first;
    uintptr_t second_start = (uintptr_t)second;
    uintptr_t first_end;
    uintptr_t second_end;

    if (first_start > (uintptr_t)-1 - first_length ||
        second_start > (uintptr_t)-1 - second_length) {
        return true;
    }
    first_end = first_start + first_length;
    second_end = second_start + second_length;
    return first_start < second_end && second_start < first_end;
}

static void entropy_seed_fill_fail(
    struct entropy_seed_fill *controller,
    enum entropy_seed_fill_fault fault)
{
    controller->state = ENTROPY_SEED_FILL_STATE_FAILED;
    controller->fault = fault;
}

static void entropy_seed_fill_set_unavailable(
    struct entropy_seed_fill *controller)
{
    controller->state = ENTROPY_SEED_FILL_STATE_UNAVAILABLE;
    controller->fault = ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE;
}

static void entropy_seed_fill_set_degraded(
    struct entropy_seed_fill *controller,
    enum entropy_seed_fill_fault fault)
{
    controller->state = ENTROPY_SEED_FILL_STATE_DEGRADED;
    controller->fault = fault;
}

enum entropy_seed_fill_status entropy_seed_fill_init(
    struct entropy_seed_fill *controller,
    const struct entropy_seed_fill_config *config,
    const struct entropy_accumulator *accumulator)
{
    if (controller == NULL || config == NULL) {
        return ENTROPY_SEED_FILL_NULL;
    }
    if (controller->initialized) {
        return ENTROPY_SEED_FILL_ALREADY_PUBLISHED;
    }
    if (!entropy_seed_fill_config_valid(config)) {
        if (config->version != ENTROPY_SEED_FILL_VERSION) {
            return ENTROPY_SEED_FILL_BAD_VERSION;
        }
        if (!entropy_seed_fill_generation_valid(
                config->generation)) {
            return ENTROPY_SEED_FILL_BAD_GENERATION;
        }
        if (!entropy_seed_fill_source_id_valid(
                config->source_id)) {
            return ENTROPY_SEED_FILL_BAD_SOURCE_ID;
        }
        if (config->required_credited_bytes <
                ENTROPY_SEED_FILL_REQUIRED_BYTES_MIN ||
            config->required_credited_bytes >
                ENTROPY_SEED_FILL_REQUIRED_BYTES_MAX) {
            return ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES;
        }
        if (config->seed_length < ENTROPY_SEED_FILL_SEED_LENGTH_MIN ||
            config->seed_length > ENTROPY_SEED_FILL_SEED_LENGTH_MAX) {
            return ENTROPY_SEED_FILL_BAD_SEED_LENGTH;
        }
        if (config->fill_length_max < 1U ||
            config->fill_length_max >
                ENTROPY_SEED_FILL_FILL_LENGTH_MAX) {
            return ENTROPY_SEED_FILL_BAD_FILL_LENGTH;
        }
        return ENTROPY_SEED_FILL_BAD_FILL_COUNT;
    }

    memset(controller, 0, sizeof(*controller));
    controller->initialized = true;
    controller->version = ENTROPY_SEED_FILL_VERSION;
    controller->generation = config->generation;
    controller->source_id = config->source_id;
    controller->required_credited_bytes =
        config->required_credited_bytes;
    controller->seed_length = config->seed_length;
    controller->fill_length_max = config->fill_length_max;
    controller->fill_count_max = config->fill_count_max;

    if (accumulator == NULL ||
        !entropy_seed_fill_accumulator_ready(accumulator)) {
        entropy_seed_fill_set_unavailable(controller);
        return ENTROPY_SEED_FILL_OK;
    }
    if (entropy_accumulator_generation(accumulator) !=
        config->generation) {
        return ENTROPY_SEED_FILL_GENERATION_MISMATCH;
    }
    if (entropy_accumulator_source_id(accumulator) !=
        config->source_id) {
        return ENTROPY_SEED_FILL_SOURCE_MISMATCH;
    }
    if (!entropy_seed_fill_accumulator_enough(
            controller, accumulator)) {
        entropy_seed_fill_set_degraded(
            controller,
            ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES);
    } else {
        controller->state = ENTROPY_SEED_FILL_STATE_READY;
        controller->fault = ENTROPY_SEED_FILL_FAULT_NONE;
    }
    return ENTROPY_SEED_FILL_OK;
}

enum entropy_seed_fill_status entropy_seed_fill_seed(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t *bytes_written)
{
    if (controller == NULL || output == NULL || bytes_written == NULL) {
        return ENTROPY_SEED_FILL_NULL;
    }
    if (!entropy_seed_fill_object_valid(controller)) {
        return ENTROPY_SEED_FILL_BAD_STATE;
    }
    if (controller->state == ENTROPY_SEED_FILL_STATE_UNAVAILABLE) {
        return ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY;
    }
    if (controller->state == ENTROPY_SEED_FILL_STATE_FAILED) {
        return ENTROPY_SEED_FILL_BAD_STATE;
    }
    if (controller->seed_published) {
        return ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED;
    }
    if (controller->state != ENTROPY_SEED_FILL_STATE_READY) {
        return ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES;
    }
    if (!entropy_seed_fill_accumulator_ready(accumulator)) {
        entropy_seed_fill_set_unavailable(controller);
        return ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY;
    }
    if (!entropy_seed_fill_accumulator_matches(
            controller, accumulator)) {
        if (entropy_accumulator_generation(accumulator) !=
            controller->generation) {
            entropy_seed_fill_fail(
                controller,
                ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH);
            return ENTROPY_SEED_FILL_GENERATION_MISMATCH;
        }
        entropy_seed_fill_fail(
            controller,
            ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH);
        return ENTROPY_SEED_FILL_SOURCE_MISMATCH;
    }
    if (!entropy_seed_fill_accumulator_enough(
            controller, accumulator)) {
        entropy_seed_fill_set_degraded(
            controller,
            ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES);
        return ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES;
    }
    if (output_capacity < controller->seed_length) {
        return ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL;
    }
    if (entropy_seed_fill_ranges_overlap(
            accumulator,
            sizeof(*accumulator),
            output,
            controller->seed_length) ||
        entropy_seed_fill_ranges_overlap(
            controller,
            sizeof(*controller),
            output,
            controller->seed_length)) {
        entropy_seed_fill_fail(
            controller,
            ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP);
        return ENTROPY_SEED_FILL_OUTPUT_OVERLAP;
    }

    memcpy(output, accumulator->bytes, controller->seed_length);
    controller->seed_published = true;
    controller->seed_material_bytes = controller->seed_length;
    controller->material_offset = controller->seed_length;
    controller->state = ENTROPY_SEED_FILL_STATE_READY;
    controller->fault = ENTROPY_SEED_FILL_FAULT_NONE;
    *bytes_written = controller->seed_length;
    return ENTROPY_SEED_FILL_OK;
}

enum entropy_seed_fill_status entropy_seed_fill_fill(
    struct entropy_seed_fill *controller,
    const struct entropy_accumulator *accumulator,
    void *output,
    uint32_t output_capacity,
    uint32_t requested_length,
    uint32_t *bytes_written)
{
    if (controller == NULL || output == NULL || bytes_written == NULL) {
        return ENTROPY_SEED_FILL_NULL;
    }
    if (!entropy_seed_fill_object_valid(controller)) {
        return ENTROPY_SEED_FILL_BAD_STATE;
    }
    if (controller->state == ENTROPY_SEED_FILL_STATE_UNAVAILABLE) {
        return ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY;
    }
    if (controller->state == ENTROPY_SEED_FILL_STATE_FAILED) {
        return ENTROPY_SEED_FILL_BAD_STATE;
    }
    if (!controller->seed_published) {
        return ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED;
    }
    if (requested_length == 0U ||
        requested_length > controller->fill_length_max) {
        return ENTROPY_SEED_FILL_LENGTH_OVERFLOW;
    }
    if (controller->state != ENTROPY_SEED_FILL_STATE_READY) {
        if (controller->state == ENTROPY_SEED_FILL_STATE_DEGRADED &&
            controller->fault ==
                ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED) {
            return ENTROPY_SEED_FILL_FILL_EXHAUSTED;
        }
        return ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES;
    }
    if (!entropy_seed_fill_accumulator_ready(accumulator)) {
        entropy_seed_fill_set_unavailable(controller);
        return ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY;
    }
    if (!entropy_seed_fill_accumulator_matches(
            controller, accumulator)) {
        if (entropy_accumulator_generation(accumulator) !=
            controller->generation) {
            entropy_seed_fill_fail(
                controller,
                ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH);
            return ENTROPY_SEED_FILL_GENERATION_MISMATCH;
        }
        entropy_seed_fill_fail(
            controller,
            ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH);
        return ENTROPY_SEED_FILL_SOURCE_MISMATCH;
    }
    if (!entropy_seed_fill_accumulator_enough(
            controller, accumulator)) {
        entropy_seed_fill_set_degraded(
            controller,
            ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES);
        return ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES;
    }
    if (controller->fill_count >= controller->fill_count_max) {
        entropy_seed_fill_set_degraded(
            controller,
            ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED);
        return ENTROPY_SEED_FILL_FILL_EXHAUSTED;
    }
    if (controller->material_offset >
        ENTROPY_SEED_FILL_TOTAL_MATERIAL_MAX - requested_length) {
        entropy_seed_fill_set_degraded(
            controller,
            ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW);
        return ENTROPY_SEED_FILL_LENGTH_OVERFLOW;
    }
    if (output_capacity < requested_length) {
        return ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL;
    }
    if (entropy_seed_fill_ranges_overlap(
            accumulator,
            sizeof(*accumulator),
            output,
            requested_length) ||
        entropy_seed_fill_ranges_overlap(
            controller,
            sizeof(*controller),
            output,
            requested_length)) {
        entropy_seed_fill_fail(
            controller,
            ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP);
        return ENTROPY_SEED_FILL_OUTPUT_OVERLAP;
    }

    memcpy(
        output,
        accumulator->bytes + controller->material_offset,
        requested_length);
    controller->fill_count += 1U;
    controller->material_offset += requested_length;
    controller->last_fill_length = requested_length;
    *bytes_written = requested_length;
    return ENTROPY_SEED_FILL_OK;
}

enum entropy_seed_fill_state entropy_seed_fill_state(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller)) {
        return ENTROPY_SEED_FILL_STATE_UNINITIALIZED;
    }
    return controller->state;
}

enum entropy_seed_fill_fault entropy_seed_fill_fault(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller)) {
        return ENTROPY_SEED_FILL_FAULT_NONE;
    }
    return controller->fault;
}

bool entropy_seed_fill_initialized(
    const struct entropy_seed_fill *controller)
{
    return entropy_seed_fill_object_valid(controller);
}

bool entropy_seed_fill_ready(
    const struct entropy_seed_fill *controller)
{
    return entropy_seed_fill_object_valid(controller) &&
           controller->state == ENTROPY_SEED_FILL_STATE_READY;
}

bool entropy_seed_fill_seed_published(
    const struct entropy_seed_fill *controller)
{
    return entropy_seed_fill_object_valid(controller) &&
           controller->seed_published;
}

uint32_t entropy_seed_fill_generation(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller)) {
        return 0U;
    }
    return controller->generation;
}

uint32_t entropy_seed_fill_source_id(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller)) {
        return 0U;
    }
    return controller->source_id;
}

uint32_t entropy_seed_fill_seed_material_bytes(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller) ||
        !controller->seed_published) {
        return 0U;
    }
    return controller->seed_material_bytes;
}

uint32_t entropy_seed_fill_fill_count(
    const struct entropy_seed_fill *controller)
{
    if (!entropy_seed_fill_object_valid(controller)) {
        return 0U;
    }
    return controller->fill_count;
}

const char *entropy_seed_fill_status_string(
    enum entropy_seed_fill_status status)
{
    switch (status) {
    case ENTROPY_SEED_FILL_OK:
        return "ENTROPY_SEED_FILL_OK";
    case ENTROPY_SEED_FILL_NULL:
        return "ENTROPY_SEED_FILL_NULL";
    case ENTROPY_SEED_FILL_BAD_STATE:
        return "ENTROPY_SEED_FILL_BAD_STATE";
    case ENTROPY_SEED_FILL_ALREADY_PUBLISHED:
        return "ENTROPY_SEED_FILL_ALREADY_PUBLISHED";
    case ENTROPY_SEED_FILL_BAD_VERSION:
        return "ENTROPY_SEED_FILL_BAD_VERSION";
    case ENTROPY_SEED_FILL_BAD_GENERATION:
        return "ENTROPY_SEED_FILL_BAD_GENERATION";
    case ENTROPY_SEED_FILL_BAD_SOURCE_ID:
        return "ENTROPY_SEED_FILL_BAD_SOURCE_ID";
    case ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES:
        return "ENTROPY_SEED_FILL_BAD_REQUIRED_BYTES";
    case ENTROPY_SEED_FILL_BAD_SEED_LENGTH:
        return "ENTROPY_SEED_FILL_BAD_SEED_LENGTH";
    case ENTROPY_SEED_FILL_BAD_FILL_LENGTH:
        return "ENTROPY_SEED_FILL_BAD_FILL_LENGTH";
    case ENTROPY_SEED_FILL_BAD_FILL_COUNT:
        return "ENTROPY_SEED_FILL_BAD_FILL_COUNT";
    case ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY:
        return "ENTROPY_SEED_FILL_ACCUMULATOR_NOT_READY";
    case ENTROPY_SEED_FILL_GENERATION_MISMATCH:
        return "ENTROPY_SEED_FILL_GENERATION_MISMATCH";
    case ENTROPY_SEED_FILL_SOURCE_MISMATCH:
        return "ENTROPY_SEED_FILL_SOURCE_MISMATCH";
    case ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES:
        return "ENTROPY_SEED_FILL_INSUFFICIENT_CREDITED_BYTES";
    case ENTROPY_SEED_FILL_OUTPUT_OVERLAP:
        return "ENTROPY_SEED_FILL_OUTPUT_OVERLAP";
    case ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL:
        return "ENTROPY_SEED_FILL_OUTPUT_TOO_SMALL";
    case ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED:
        return "ENTROPY_SEED_FILL_SEED_NOT_PUBLISHED";
    case ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED:
        return "ENTROPY_SEED_FILL_SEED_ALREADY_PUBLISHED";
    case ENTROPY_SEED_FILL_FILL_EXHAUSTED:
        return "ENTROPY_SEED_FILL_FILL_EXHAUSTED";
    case ENTROPY_SEED_FILL_LENGTH_OVERFLOW:
        return "ENTROPY_SEED_FILL_LENGTH_OVERFLOW";
    default:
        return NULL;
    }
}

const char *entropy_seed_fill_state_string(
    enum entropy_seed_fill_state state)
{
    switch (state) {
    case ENTROPY_SEED_FILL_STATE_UNINITIALIZED:
        return "ENTROPY_SEED_FILL_STATE_UNINITIALIZED";
    case ENTROPY_SEED_FILL_STATE_UNAVAILABLE:
        return "ENTROPY_SEED_FILL_STATE_UNAVAILABLE";
    case ENTROPY_SEED_FILL_STATE_DEGRADED:
        return "ENTROPY_SEED_FILL_STATE_DEGRADED";
    case ENTROPY_SEED_FILL_STATE_READY:
        return "ENTROPY_SEED_FILL_STATE_READY";
    case ENTROPY_SEED_FILL_STATE_FAILED:
        return "ENTROPY_SEED_FILL_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *entropy_seed_fill_fault_string(
    enum entropy_seed_fill_fault fault)
{
    switch (fault) {
    case ENTROPY_SEED_FILL_FAULT_NONE:
        return "ENTROPY_SEED_FILL_FAULT_NONE";
    case ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE:
        return "ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE";
    case ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH:
        return "ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH";
    case ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH:
        return "ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH";
    case ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES:
        return "ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES";
    case ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP:
        return "ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP";
    case ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW:
        return "ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW";
    case ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED:
        return "ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED";
    default:
        return NULL;
    }
}
