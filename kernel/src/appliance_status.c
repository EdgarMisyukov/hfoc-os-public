#include "appliance_status.h"

#include <stddef.h>
#include <stdint.h>

static bool appliance_status_service_state_valid(
    enum appliance_status_service_state state)
{
    return state == APPLIANCE_SERVICE_STOPPED ||
           state == APPLIANCE_SERVICE_RUNNING ||
           state == APPLIANCE_SERVICE_READY ||
           state == APPLIANCE_SERVICE_EXITED ||
           state == APPLIANCE_SERVICE_EXHAUSTED;
}

static bool appliance_status_service_health_valid(
    enum appliance_status_service_health health)
{
    return health == APPLIANCE_SERVICE_HEALTH_PENDING ||
           health == APPLIANCE_SERVICE_HEALTH_READY ||
           health == APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED ||
           health == APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT;
}

static enum appliance_status_status appliance_status_snapshot_validate(
    const struct appliance_status_snapshot *snapshot)
{
    uint32_t index;

    if (snapshot->version != APPLIANCE_STATUS_VERSION ||
        snapshot->state.version != APPLIANCE_STATUS_VERSION ||
        snapshot->storage.version != APPLIANCE_STATUS_VERSION ||
        snapshot->health.version != APPLIANCE_STATUS_VERSION) {
        return APPLIANCE_STATUS_BAD_VERSION;
    }
    if (snapshot->state.recovery_mode >
            APPLIANCE_STATUS_RECOVERY_MODE_MAX ||
        snapshot->state.active_process_count >
            APPLIANCE_STATUS_COUNT_MAX) {
        return APPLIANCE_STATUS_INVALID_SNAPSHOT;
    }
    if (snapshot->storage.state != APPLIANCE_STORAGE_OK &&
        snapshot->storage.state != APPLIANCE_STORAGE_DEGRADED &&
        snapshot->storage.state != APPLIANCE_STORAGE_FAILED) {
        return APPLIANCE_STATUS_INVALID_STORAGE;
    }
    if (snapshot->health.state != APPLIANCE_HEALTH_OK &&
        snapshot->health.state != APPLIANCE_HEALTH_DEGRADED &&
        snapshot->health.state != APPLIANCE_HEALTH_FAILED) {
        return APPLIANCE_STATUS_INVALID_HEALTH;
    }
    if (snapshot->health.ready_count > APPLIANCE_STATUS_COUNT_MAX ||
        snapshot->health.pending_count > APPLIANCE_STATUS_COUNT_MAX ||
        snapshot->health.failed_count > APPLIANCE_STATUS_COUNT_MAX) {
        return APPLIANCE_STATUS_INVALID_HEALTH;
    }
    if (snapshot->service_count > APPLIANCE_STATUS_SERVICE_MAX) {
        return APPLIANCE_STATUS_INVALID_SERVICE;
    }
    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *const entry =
            &snapshot->services[index];

        if (entry->id == 0U || entry->id > 255U ||
            !appliance_status_service_state_valid(entry->state) ||
            !appliance_status_service_health_valid(entry->health) ||
            entry->restart_count > APPLIANCE_STATUS_COUNT_MAX) {
            return APPLIANCE_STATUS_INVALID_SERVICE;
        }
        if (index > 0U &&
            entry->id <= snapshot->services[index - 1U].id) {
            return APPLIANCE_STATUS_INVALID_SERVICE;
        }
    }
    return APPLIANCE_STATUS_OK;
}

enum appliance_status_status appliance_status_snapshot_init(
    struct appliance_status_snapshot *snapshot,
    const struct appliance_status_snapshot *source)
{
    enum appliance_status_status status;
    uint32_t index;

    if (snapshot == NULL || source == NULL) {
        return APPLIANCE_STATUS_NULL;
    }
    if (snapshot->published) {
        return APPLIANCE_STATUS_BAD_STATE;
    }
    status = appliance_status_snapshot_validate(source);
    if (status != APPLIANCE_STATUS_OK) {
        return status;
    }

    snapshot->version = source->version;
    snapshot->state = source->state;
    snapshot->storage = source->storage;
    snapshot->health = source->health;
    snapshot->service_count = source->service_count;
    for (index = 0U; index < source->service_count; ++index) {
        snapshot->services[index] = source->services[index];
    }
    snapshot->published = true;
    return APPLIANCE_STATUS_OK;
}

static bool appliance_status_append_bytes(
    char *output,
    size_t *offset,
    const char *text,
    size_t length)
{
    size_t index;

    if (*offset > APPLIANCE_STATUS_OUTPUT_MAX ||
        length > APPLIANCE_STATUS_OUTPUT_MAX - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = text[index];
    }
    *offset += length;
    return true;
}

static bool appliance_status_append_char(
    char *output,
    size_t *offset,
    char value)
{
    return appliance_status_append_bytes(output, offset, &value, 1U);
}

static bool appliance_status_append_u64(
    char *output,
    size_t *offset,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        --count;
        if (!appliance_status_append_char(output, offset, digits[count])) {
            return false;
        }
    }
    return true;
}

static bool appliance_status_append_label_u64(
    char *output,
    size_t *offset,
    const char *label,
    size_t label_length,
    uint64_t value,
    bool leading_space,
    bool newline)
{
    if (leading_space &&
        !appliance_status_append_char(output, offset, ' ')) {
        return false;
    }
    if (!appliance_status_append_bytes(output, offset,
                                       label, label_length) ||
        !appliance_status_append_char(output, offset, '=') ||
        !appliance_status_append_u64(output, offset, value)) {
        return false;
    }
    return !newline || appliance_status_append_char(output, offset, '\n');
}

enum appliance_status_status appliance_status_render(
    const struct appliance_status_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_STATUS_OUTPUT_MAX + 1U];
    enum appliance_status_status status;
    size_t offset = 0U;
    uint32_t index;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_STATUS_NULL;
    }
    if (!snapshot->published) {
        return APPLIANCE_STATUS_BAD_STATE;
    }
    status = appliance_status_snapshot_validate(snapshot);
    if (status != APPLIANCE_STATUS_OK) {
        return status;
    }

    if (!appliance_status_append_bytes(
            rendered, &offset, "status v1=", 10U) ||
        !appliance_status_append_u64(
            rendered, &offset, snapshot->version) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "g", 1U,
            snapshot->state.generation, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "u", 1U,
            snapshot->state.uptime_ticks, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "r", 1U,
            snapshot->state.recovery_mode, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "c", 1U,
            snapshot->state.config_revision, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "a", 1U,
            snapshot->state.audit_sequence, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "p", 1U,
            snapshot->state.active_process_count, true, true) ||
        !appliance_status_append_bytes(
            rendered, &offset, "storage j=", 10U) ||
        !appliance_status_append_u64(
            rendered, &offset, snapshot->storage.journal_generation) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "b", 1U,
            snapshot->storage.backup_generation, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "s", 1U,
            snapshot->storage.schema_version, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "st", 2U,
            snapshot->storage.state, true, true) ||
        !appliance_status_append_bytes(
            rendered, &offset, "health st=", 10U) ||
        !appliance_status_append_u64(
            rendered, &offset, snapshot->health.state) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "r", 1U,
            snapshot->health.ready_count, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "p", 1U,
            snapshot->health.pending_count, true, false) ||
        !appliance_status_append_label_u64(
            rendered, &offset, "f", 1U,
            snapshot->health.failed_count, true, true) ||
        !appliance_status_append_bytes(
            rendered, &offset, "services=", 9U) ||
        !appliance_status_append_u64(
            rendered, &offset, snapshot->service_count) ||
        !appliance_status_append_char(rendered, &offset, '\n')) {
        return APPLIANCE_STATUS_OVERFLOW;
    }

    for (index = 0U; index < snapshot->service_count; ++index) {
        const struct appliance_status_service_entry *const entry =
            &snapshot->services[index];

        if (!appliance_status_append_u64(rendered, &offset, entry->id) ||
            !appliance_status_append_char(rendered, &offset, ':') ||
            !appliance_status_append_u64(rendered, &offset, entry->state) ||
            !appliance_status_append_char(rendered, &offset, '/') ||
            !appliance_status_append_u64(rendered, &offset, entry->health) ||
            !appliance_status_append_char(rendered, &offset, '/') ||
            !appliance_status_append_u64(
                rendered, &offset, entry->restart_count) ||
            !appliance_status_append_char(rendered, &offset, '\n')) {
            return APPLIANCE_STATUS_OVERFLOW;
        }
    }

    if (capacity <= offset) {
        return APPLIANCE_STATUS_OVERFLOW;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_STATUS_OK;
}

enum appliance_status_status appliance_status_command_descriptor(
    struct management_command_descriptor *descriptor)
{
    if (descriptor == NULL) {
        return APPLIANCE_STATUS_NULL;
    }
    descriptor->version = MANAGEMENT_COMMAND_VERSION;
    descriptor->command_id = APPLIANCE_STATUS_COMMAND_ID;
    descriptor->request_min_length = APPLIANCE_STATUS_REQUEST_LENGTH;
    descriptor->request_max_length = APPLIANCE_STATUS_REQUEST_LENGTH;
    descriptor->response_min_length = APPLIANCE_STATUS_RESPONSE_MIN;
    descriptor->response_max_length = APPLIANCE_STATUS_RESPONSE_MAX;
    return APPLIANCE_STATUS_OK;
}

enum appliance_status_status appliance_status_authorization_entry(
    struct authorization_entry *entry)
{
    uint32_t index;

    if (entry == NULL) {
        return APPLIANCE_STATUS_NULL;
    }
    for (index = 0U;
         index < AUTHORIZATION_ROLE_SET_MAX; ++index) {
        entry->roles[index] = 0U;
    }
    for (index = 0U;
         index < AUTHORIZATION_CAPABILITY_SET_MAX; ++index) {
        entry->capabilities[index] = 0U;
    }
    entry->version = AUTHORIZATION_VERSION;
    entry->command_id = APPLIANCE_STATUS_COMMAND_ID;
    entry->role_count = 1U;
    entry->roles[0] = AUTHORIZATION_ROLE_ADMIN;
    entry->capability_count = 1U;
    entry->capabilities[0] = AUTHORIZATION_CAPABILITY_STATUS_READ;
    return APPLIANCE_STATUS_OK;
}

const char *appliance_status_status_string(
    enum appliance_status_status status)
{
    switch (status) {
    case APPLIANCE_STATUS_OK:
        return "APPLIANCE_STATUS_OK";
    case APPLIANCE_STATUS_NULL:
        return "APPLIANCE_STATUS_NULL";
    case APPLIANCE_STATUS_BAD_VERSION:
        return "APPLIANCE_STATUS_BAD_VERSION";
    case APPLIANCE_STATUS_BAD_STATE:
        return "APPLIANCE_STATUS_BAD_STATE";
    case APPLIANCE_STATUS_INVALID_SNAPSHOT:
        return "APPLIANCE_STATUS_INVALID_SNAPSHOT";
    case APPLIANCE_STATUS_INVALID_SERVICE:
        return "APPLIANCE_STATUS_INVALID_SERVICE";
    case APPLIANCE_STATUS_INVALID_STORAGE:
        return "APPLIANCE_STATUS_INVALID_STORAGE";
    case APPLIANCE_STATUS_INVALID_HEALTH:
        return "APPLIANCE_STATUS_INVALID_HEALTH";
    case APPLIANCE_STATUS_OVERFLOW:
        return "APPLIANCE_STATUS_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_status_health_string(
    enum appliance_status_health_state state)
{
    switch (state) {
    case APPLIANCE_HEALTH_OK:
        return "APPLIANCE_HEALTH_OK";
    case APPLIANCE_HEALTH_DEGRADED:
        return "APPLIANCE_HEALTH_DEGRADED";
    case APPLIANCE_HEALTH_FAILED:
        return "APPLIANCE_HEALTH_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_status_storage_string(
    enum appliance_status_storage_state state)
{
    switch (state) {
    case APPLIANCE_STORAGE_OK:
        return "APPLIANCE_STORAGE_OK";
    case APPLIANCE_STORAGE_DEGRADED:
        return "APPLIANCE_STORAGE_DEGRADED";
    case APPLIANCE_STORAGE_FAILED:
        return "APPLIANCE_STORAGE_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_status_service_state_string(
    enum appliance_status_service_state state)
{
    switch (state) {
    case APPLIANCE_SERVICE_STOPPED:
        return "APPLIANCE_SERVICE_STOPPED";
    case APPLIANCE_SERVICE_RUNNING:
        return "APPLIANCE_SERVICE_RUNNING";
    case APPLIANCE_SERVICE_READY:
        return "APPLIANCE_SERVICE_READY";
    case APPLIANCE_SERVICE_EXITED:
        return "APPLIANCE_SERVICE_EXITED";
    case APPLIANCE_SERVICE_EXHAUSTED:
        return "APPLIANCE_SERVICE_EXHAUSTED";
    default:
        return NULL;
    }
}

const char *appliance_status_service_health_string(
    enum appliance_status_service_health health)
{
    switch (health) {
    case APPLIANCE_SERVICE_HEALTH_PENDING:
        return "APPLIANCE_SERVICE_HEALTH_PENDING";
    case APPLIANCE_SERVICE_HEALTH_READY:
        return "APPLIANCE_SERVICE_HEALTH_READY";
    case APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED:
        return "APPLIANCE_SERVICE_HEALTH_LIVENESS_FAILED";
    case APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT:
        return "APPLIANCE_SERVICE_HEALTH_READINESS_TIMEOUT";
    default:
        return NULL;
    }
}
