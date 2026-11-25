#include "entropy_diagnostics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool entropy_diagnostics_generation_valid(uint32_t generation)
{
    return generation >= ENTROPY_ACCUMULATOR_GENERATION_MIN &&
           generation <= ENTROPY_ACCUMULATOR_GENERATION_MAX;
}

static bool entropy_diagnostics_source_id_valid(uint32_t source_id)
{
    return source_id >= ENTROPY_ACCUMULATOR_SOURCE_ID_MIN &&
           source_id <= ENTROPY_ACCUMULATOR_SOURCE_ID_MAX;
}

static bool entropy_diagnostics_state_valid(
    enum entropy_diagnostics_state state)
{
    return state >= ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED &&
           state <= ENTROPY_DIAGNOSTICS_STATE_FAILED;
}

static bool entropy_diagnostics_fault_valid(
    enum entropy_diagnostics_fault fault)
{
    return fault >= ENTROPY_DIAGNOSTICS_FAULT_NONE &&
           fault <= ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED;
}

static bool entropy_diagnostics_health_valid(
    const struct entropy_health *health)
{
    return health != NULL &&
           entropy_health_state(health) !=
               ENTROPY_HEALTH_STATE_UNINITIALIZED &&
           entropy_health_generation(health) != 0U;
}

static bool entropy_diagnostics_accumulator_valid(
    const struct entropy_accumulator *accumulator)
{
    return accumulator != NULL &&
           entropy_accumulator_initialized(accumulator) &&
           entropy_accumulator_generation(accumulator) != 0U &&
           entropy_accumulator_source_id(accumulator) != 0U;
}

static bool entropy_diagnostics_controller_valid(
    const struct entropy_seed_fill *controller)
{
    return controller != NULL &&
           entropy_seed_fill_initialized(controller) &&
           entropy_seed_fill_generation(controller) != 0U &&
           entropy_seed_fill_source_id(controller) != 0U;
}

static bool entropy_diagnostics_snapshot_valid(
    const struct entropy_diagnostics_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->version == ENTROPY_DIAGNOSTICS_VERSION &&
           entropy_diagnostics_generation_valid(snapshot->generation) &&
           entropy_diagnostics_source_id_valid(snapshot->source_id) &&
           entropy_diagnostics_state_valid(snapshot->state) &&
           entropy_diagnostics_fault_valid(snapshot->fault) &&
           snapshot->credited_bytes <= ENTROPY_ACCUMULATOR_CAPACITY &&
           snapshot->credited_blocks <= ENTROPY_ACCUMULATOR_CAPACITY &&
           snapshot->seed_material_bytes <=
               ENTROPY_SEED_FILL_SEED_LENGTH_MAX &&
           snapshot->fill_count <= ENTROPY_SEED_FILL_FILL_COUNT_MAX;
}

static enum entropy_diagnostics_fault
entropy_diagnostics_map_health_fault(
    enum entropy_health_fault fault)
{
    switch (fault) {
    case ENTROPY_HEALTH_FAULT_REPEATED_BLOCK:
        return ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK;
    case ENTROPY_HEALTH_FAULT_IMPOSSIBLE_LENGTH:
        return ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH;
    case ENTROPY_HEALTH_FAULT_STALLED:
        return ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED;
    case ENTROPY_HEALTH_FAULT_DEVICE_RESET:
        return ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET;
    default:
        return ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    }
}

static enum entropy_diagnostics_fault
entropy_diagnostics_map_seed_fault(
    enum entropy_seed_fill_fault fault)
{
    switch (fault) {
    case ENTROPY_SEED_FILL_FAULT_ACCUMULATOR_STATE:
        return ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    case ENTROPY_SEED_FILL_FAULT_GENERATION_MISMATCH:
        return ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH;
    case ENTROPY_SEED_FILL_FAULT_SOURCE_MISMATCH:
        return ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH;
    case ENTROPY_SEED_FILL_FAULT_CREDITED_BYTES:
        return ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES;
    case ENTROPY_SEED_FILL_FAULT_OUTPUT_OVERLAP:
        return ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP;
    case ENTROPY_SEED_FILL_FAULT_LENGTH_OVERFLOW:
        return ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW;
    case ENTROPY_SEED_FILL_FAULT_FILL_EXHAUSTED:
        return ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED;
    default:
        return ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    }
}

static void entropy_diagnostics_store_le32(
    uint8_t *bytes,
    uint32_t value)
{
    bytes[0] = (uint8_t)(value);
    bytes[1] = (uint8_t)(value >> 8U);
    bytes[2] = (uint8_t)(value >> 16U);
    bytes[3] = (uint8_t)(value >> 24U);
}

static void entropy_diagnostics_store_le64(
    uint8_t *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (uint8_t)(value >> (8U * index));
    }
}

static size_t entropy_diagnostics_append_text(
    char *text,
    size_t position,
    const char *value)
{
    size_t length = strlen(value);

    memcpy(text + position, value, length);
    return position + length;
}

static size_t entropy_diagnostics_append_u64(
    char *text,
    size_t position,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;
    size_t index;

    do {
        digits[count] =
            (char)('0' + (unsigned int)(value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);

    for (index = 0U; index < count; ++index) {
        text[position + index] = digits[count - index - 1U];
    }
    return position + count;
}

static size_t entropy_diagnostics_append_u32(
    char *text,
    size_t position,
    uint32_t value)
{
    return entropy_diagnostics_append_u64(text, position, value);
}

enum entropy_diagnostics_status entropy_diagnostics_snapshot(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *controller,
    struct entropy_diagnostics_snapshot *snapshot_out)
{
    struct entropy_diagnostics_snapshot snapshot;
    enum entropy_health_state health_state;
    enum entropy_seed_fill_state controller_state;
    uint32_t health_generation;
    uint32_t accumulator_generation;
    uint32_t accumulator_source_id;
    uint32_t controller_generation;
    uint32_t controller_source_id;

    if (health == NULL ||
        accumulator == NULL ||
        controller == NULL ||
        snapshot_out == NULL) {
        return ENTROPY_DIAGNOSTICS_NULL;
    }
    if (!entropy_diagnostics_health_valid(health) ||
        !entropy_diagnostics_accumulator_valid(accumulator) ||
        !entropy_diagnostics_controller_valid(controller)) {
        return ENTROPY_DIAGNOSTICS_BAD_STATE;
    }

    health_generation = entropy_health_generation(health);
    accumulator_generation = entropy_accumulator_generation(accumulator);
    controller_generation = entropy_seed_fill_generation(controller);
    if (health_generation != accumulator_generation ||
        health_generation != controller_generation) {
        return ENTROPY_DIAGNOSTICS_BAD_GENERATION;
    }

    accumulator_source_id = entropy_accumulator_source_id(accumulator);
    controller_source_id = entropy_seed_fill_source_id(controller);
    if (accumulator_source_id != controller_source_id) {
        return ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = ENTROPY_DIAGNOSTICS_VERSION;
    snapshot.generation = health_generation;
    snapshot.source_id = accumulator_source_id;
    snapshot.credited_bytes =
        entropy_accumulator_credited_bytes(accumulator);
    snapshot.credited_blocks =
        entropy_accumulator_credited_blocks(accumulator);
    snapshot.last_accept_tick =
        entropy_accumulator_last_accept_tick(accumulator);
    snapshot.seed_published =
        entropy_seed_fill_seed_published(controller);
    snapshot.seed_material_bytes =
        entropy_seed_fill_seed_material_bytes(controller);
    snapshot.fill_count = entropy_seed_fill_fill_count(controller);

    health_state = entropy_health_state(health);
    controller_state = entropy_seed_fill_state(controller);
    if (health_state == ENTROPY_HEALTH_STATE_FAILED) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_FAILED;
        snapshot.fault = entropy_diagnostics_map_health_fault(
            entropy_health_fault(health));
    } else if (entropy_accumulator_state(accumulator) ==
               ENTROPY_ACCUMULATOR_STATE_FAILED) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_FAILED;
        snapshot.fault = ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    } else if (controller_state == ENTROPY_SEED_FILL_STATE_FAILED) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_FAILED;
        snapshot.fault = entropy_diagnostics_map_seed_fault(
            entropy_seed_fill_fault(controller));
    } else if (controller_state ==
               ENTROPY_SEED_FILL_STATE_UNAVAILABLE) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE;
        snapshot.fault =
            ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    } else if (controller_state == ENTROPY_SEED_FILL_STATE_DEGRADED) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_DEGRADED;
        snapshot.fault = entropy_diagnostics_map_seed_fault(
            entropy_seed_fill_fault(controller));
    } else if (health_state == ENTROPY_HEALTH_STATE_READY &&
               controller_state == ENTROPY_SEED_FILL_STATE_READY) {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_READY;
        snapshot.fault = ENTROPY_DIAGNOSTICS_FAULT_NONE;
    } else {
        snapshot.state = ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE;
        snapshot.fault =
            ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE;
    }

    if (!entropy_diagnostics_snapshot_valid(&snapshot)) {
        return ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT;
    }
    *snapshot_out = snapshot;
    return ENTROPY_DIAGNOSTICS_OK;
}

enum entropy_diagnostics_status entropy_diagnostics_serial(
    const struct entropy_diagnostics_snapshot *snapshot,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    char text[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    size_t position = 0U;
    size_t length;

    if (snapshot == NULL || buffer == NULL || length_out == NULL) {
        return ENTROPY_DIAGNOSTICS_NULL;
    }
    if (!entropy_diagnostics_snapshot_valid(snapshot)) {
        return ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT;
    }

    position = entropy_diagnostics_append_text(
        text, position, "ENTROPY_DIAG version=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->version);
    position = entropy_diagnostics_append_text(
        text, position, " generation=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->generation);
    position = entropy_diagnostics_append_text(
        text, position, " source_id=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->source_id);
    position = entropy_diagnostics_append_text(
        text, position, " state=");
    position = entropy_diagnostics_append_text(
        text, position,
        entropy_diagnostics_state_string(snapshot->state));
    position = entropy_diagnostics_append_text(
        text, position, " fault=");
    position = entropy_diagnostics_append_text(
        text, position,
        entropy_diagnostics_fault_string(snapshot->fault));
    position = entropy_diagnostics_append_text(
        text, position, " credited_bytes=");
    position = entropy_diagnostics_append_u64(
        text, position, snapshot->credited_bytes);
    position = entropy_diagnostics_append_text(
        text, position, " credited_blocks=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->credited_blocks);
    position = entropy_diagnostics_append_text(
        text, position, " last_accept_tick=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->last_accept_tick);
    position = entropy_diagnostics_append_text(
        text, position, " seed_published=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->seed_published ? 1U : 0U);
    position = entropy_diagnostics_append_text(
        text, position, " seed_material_bytes=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->seed_material_bytes);
    position = entropy_diagnostics_append_text(
        text, position, " fill_count=");
    position = entropy_diagnostics_append_u32(
        text, position, snapshot->fill_count);
    text[position] = '\n';
    ++position;
    length = position;

    if (capacity < length + 1U) {
        return ENTROPY_DIAGNOSTICS_TOO_SMALL;
    }
    memcpy(buffer, text, length);
    ((char *)buffer)[length] = '\0';
    *length_out = length;
    return ENTROPY_DIAGNOSTICS_OK;
}

enum entropy_diagnostics_status entropy_diagnostics_audit(
    const struct entropy_diagnostics_snapshot *snapshot,
    struct entropy_diagnostics_audit *audit_out)
{
    if (snapshot == NULL || audit_out == NULL) {
        return ENTROPY_DIAGNOSTICS_NULL;
    }
    if (!entropy_diagnostics_snapshot_valid(snapshot)) {
        return ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT;
    }

    memset(audit_out, 0, sizeof(*audit_out));
    audit_out->version = snapshot->version;
    audit_out->state = (uint32_t)snapshot->state;
    audit_out->fault = (uint32_t)snapshot->fault;
    audit_out->generation = snapshot->generation;
    audit_out->source_id = snapshot->source_id;
    audit_out->credited_bytes = snapshot->credited_bytes;
    audit_out->credited_blocks = snapshot->credited_blocks;
    audit_out->seed_material_bytes = snapshot->seed_material_bytes;
    audit_out->fill_count = snapshot->fill_count;
    audit_out->last_accept_tick = snapshot->last_accept_tick;
    return ENTROPY_DIAGNOSTICS_OK;
}

enum entropy_diagnostics_status entropy_diagnostics_support(
    const struct entropy_diagnostics_snapshot *snapshot,
    void *buffer,
    size_t capacity,
    size_t *length_out)
{
    static const uint8_t magic[8] = {
        'E', 'N', 'T', 'R', 'D', 'I', 'A', 'G'
    };
    uint8_t payload[ENTROPY_DIAGNOSTICS_SUPPORT_SIZE];

    if (snapshot == NULL || buffer == NULL || length_out == NULL) {
        return ENTROPY_DIAGNOSTICS_NULL;
    }
    if (!entropy_diagnostics_snapshot_valid(snapshot)) {
        return ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT;
    }
    if (capacity < ENTROPY_DIAGNOSTICS_SUPPORT_SIZE) {
        return ENTROPY_DIAGNOSTICS_TOO_SMALL;
    }

    memset(payload, 0, sizeof(payload));
    memcpy(payload, magic, sizeof(magic));
    payload[8] = (uint8_t)snapshot->version;
    entropy_diagnostics_store_le32(
        payload + 9U, snapshot->generation);
    entropy_diagnostics_store_le32(
        payload + 13U, snapshot->source_id);
    entropy_diagnostics_store_le32(
        payload + 17U, (uint32_t)snapshot->state);
    entropy_diagnostics_store_le32(
        payload + 21U, (uint32_t)snapshot->fault);
    entropy_diagnostics_store_le64(
        payload + 25U, snapshot->credited_bytes);
    entropy_diagnostics_store_le32(
        payload + 33U, snapshot->credited_blocks);
    entropy_diagnostics_store_le32(
        payload + 37U, snapshot->last_accept_tick);
    payload[41U] = snapshot->seed_published ? 1U : 0U;
    entropy_diagnostics_store_le32(
        payload + 42U, snapshot->seed_material_bytes);
    entropy_diagnostics_store_le32(
        payload + 46U, snapshot->fill_count);

    memcpy(buffer, payload, sizeof(payload));
    *length_out = sizeof(payload);
    return ENTROPY_DIAGNOSTICS_OK;
}

const char *entropy_diagnostics_state_string(
    enum entropy_diagnostics_state state)
{
    switch (state) {
    case ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED:
        return "ENTROPY_DIAGNOSTICS_STATE_UNINITIALIZED";
    case ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE:
        return "ENTROPY_DIAGNOSTICS_STATE_UNAVAILABLE";
    case ENTROPY_DIAGNOSTICS_STATE_DEGRADED:
        return "ENTROPY_DIAGNOSTICS_STATE_DEGRADED";
    case ENTROPY_DIAGNOSTICS_STATE_READY:
        return "ENTROPY_DIAGNOSTICS_STATE_READY";
    case ENTROPY_DIAGNOSTICS_STATE_FAILED:
        return "ENTROPY_DIAGNOSTICS_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *entropy_diagnostics_fault_string(
    enum entropy_diagnostics_fault fault)
{
    switch (fault) {
    case ENTROPY_DIAGNOSTICS_FAULT_NONE:
        return "ENTROPY_DIAGNOSTICS_FAULT_NONE";
    case ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK:
        return "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_REPEATED_BLOCK";
    case ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH:
        return "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_IMPOSSIBLE_LENGTH";
    case ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED:
        return "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_STALLED";
    case ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET:
        return "ENTROPY_DIAGNOSTICS_FAULT_HEALTH_DEVICE_RESET";
    case ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE:
        return "ENTROPY_DIAGNOSTICS_FAULT_ACCUMULATOR_STATE";
    case ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH:
        return "ENTROPY_DIAGNOSTICS_FAULT_GENERATION_MISMATCH";
    case ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH:
        return "ENTROPY_DIAGNOSTICS_FAULT_SOURCE_MISMATCH";
    case ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES:
        return "ENTROPY_DIAGNOSTICS_FAULT_CREDITED_BYTES";
    case ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP:
        return "ENTROPY_DIAGNOSTICS_FAULT_OUTPUT_OVERLAP";
    case ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW:
        return "ENTROPY_DIAGNOSTICS_FAULT_LENGTH_OVERFLOW";
    case ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED:
        return "ENTROPY_DIAGNOSTICS_FAULT_FILL_EXHAUSTED";
    default:
        return NULL;
    }
}

const char *entropy_diagnostics_status_string(
    enum entropy_diagnostics_status status)
{
    switch (status) {
    case ENTROPY_DIAGNOSTICS_OK:
        return "ENTROPY_DIAGNOSTICS_OK";
    case ENTROPY_DIAGNOSTICS_NULL:
        return "ENTROPY_DIAGNOSTICS_NULL";
    case ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT:
        return "ENTROPY_DIAGNOSTICS_BAD_SNAPSHOT";
    case ENTROPY_DIAGNOSTICS_BAD_STATE:
        return "ENTROPY_DIAGNOSTICS_BAD_STATE";
    case ENTROPY_DIAGNOSTICS_BAD_VERSION:
        return "ENTROPY_DIAGNOSTICS_BAD_VERSION";
    case ENTROPY_DIAGNOSTICS_BAD_GENERATION:
        return "ENTROPY_DIAGNOSTICS_BAD_GENERATION";
    case ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID:
        return "ENTROPY_DIAGNOSTICS_BAD_SOURCE_ID";
    case ENTROPY_DIAGNOSTICS_TOO_SMALL:
        return "ENTROPY_DIAGNOSTICS_TOO_SMALL";
    default:
        return NULL;
    }
}

uint32_t entropy_diagnostics_version(void)
{
    return ENTROPY_DIAGNOSTICS_VERSION;
}

const char *entropy_diagnostics_policy_id(void)
{
    return ENTROPY_DIAGNOSTICS_POLICY_ID;
}
