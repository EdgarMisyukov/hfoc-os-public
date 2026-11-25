#include "integration_config_intent.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INTEGRATION_CONFIG_INTENT_TEXT_MAX 255U

struct integration_config_intent_field_spec {
    uint32_t intent_id;
    const char *name;
    size_t name_length;
    uint32_t minimum;
    uint32_t maximum;
};

static const struct integration_config_intent_field_spec
integration_config_intent_fields[INTEGRATION_CONFIG_INTENT_COUNT] = {
    {
        INTEGRATION_CONFIG_INTENT_RECOVERY_MODE,
        "recovery_mode",
        sizeof("recovery_mode") - 1U,
        0U,
        2U,
    },
    {
        INTEGRATION_CONFIG_INTENT_MAX_PROCESSES,
        "max_processes",
        sizeof("max_processes") - 1U,
        1U,
        64U,
    },
    {
        INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES,
        "max_user_memory_pages",
        sizeof("max_user_memory_pages") - 1U,
        16U,
        4096U,
    },
    {
        INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY,
        "audit_ring_capacity",
        sizeof("audit_ring_capacity") - 1U,
        16U,
        64U,
    },
    {
        INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS,
        "boot_timeout_ms",
        sizeof("boot_timeout_ms") - 1U,
        1000U,
        60000U,
    },
    {
        INTEGRATION_CONFIG_INTENT_RESTART_LIMIT,
        "restart_limit",
        sizeof("restart_limit") - 1U,
        0U,
        16U,
    },
    {
        INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS,
        "service_health_timeout_ms",
        sizeof("service_health_timeout_ms") - 1U,
        1000U,
        30000U,
    },
};

static const struct integration_config_intent_field_spec *
integration_config_intent_find_field(uint32_t intent_id)
{
    uint32_t index;

    for (index = 0U; index < INTEGRATION_CONFIG_INTENT_COUNT; ++index) {
        if (integration_config_intent_fields[index].intent_id ==
            intent_id) {
            return &integration_config_intent_fields[index];
        }
    }
    return NULL;
}

enum integration_config_intent_status integration_config_intent_validate(
    const struct integration_config_intent *intent)
{
    const struct integration_config_intent_field_spec *spec;

    if (intent == NULL) {
        return INTEGRATION_CONFIG_INTENT_NULL;
    }
    if (intent->version != INTEGRATION_CONFIG_INTENT_VERSION) {
        return INTEGRATION_CONFIG_INTENT_BAD_VERSION;
    }
    if (intent->peer_id < INTEGRATION_CONFIG_INTENT_PEER_ID_MIN ||
        intent->peer_id > INTEGRATION_CONFIG_INTENT_PEER_ID_MAX) {
        return INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID;
    }

    spec = integration_config_intent_find_field(intent->intent_id);
    if (spec == NULL) {
        return INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID;
    }
    if (intent->value < spec->minimum ||
        intent->value > spec->maximum) {
        return INTEGRATION_CONFIG_INTENT_INVALID_VALUE;
    }
    return INTEGRATION_CONFIG_INTENT_OK;
}

static size_t integration_config_intent_append_u32(
    char *out,
    size_t capacity,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;
    size_t index;

    do {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);
    if (count > capacity) {
        return 0U;
    }
    for (index = 0U; index < count; ++index) {
        out[index] = digits[count - 1U - index];
    }
    return count;
}

static size_t integration_config_intent_append_field(
    char *out,
    size_t capacity,
    const char *name,
    size_t name_length,
    uint32_t value)
{
    size_t position = 0U;
    size_t digits;

    if (capacity == 0U || name_length + 1U > capacity) {
        return 0U;
    }
    memcpy(out, name, name_length);
    position = name_length;
    out[position] = '=';
    ++position;
    if (position >= capacity) {
        return 0U;
    }
    digits = integration_config_intent_append_u32(
        out + position, capacity - position, value);
    if (digits == 0U) {
        return 0U;
    }
    position += digits;
    if (position >= capacity) {
        return 0U;
    }
    out[position] = '\n';
    ++position;
    return position;
}

static enum integration_config_intent_status
integration_config_intent_render_candidate(
    const struct appliance_config *active,
    const struct integration_config_intent_field_spec *spec,
    uint32_t value,
    char *out,
    size_t *length_out)
{
    struct appliance_config candidate = *active;
    char text[INTEGRATION_CONFIG_INTENT_TEXT_MAX + 1U];
    size_t position = 0U;

    switch (spec->intent_id) {
    case INTEGRATION_CONFIG_INTENT_RECOVERY_MODE:
        candidate.recovery_mode = value;
        break;
    case INTEGRATION_CONFIG_INTENT_MAX_PROCESSES:
        candidate.max_processes = value;
        break;
    case INTEGRATION_CONFIG_INTENT_MAX_USER_MEMORY_PAGES:
        candidate.max_user_memory_pages = value;
        break;
    case INTEGRATION_CONFIG_INTENT_AUDIT_RING_CAPACITY:
        candidate.audit_ring_capacity = value;
        break;
    case INTEGRATION_CONFIG_INTENT_BOOT_TIMEOUT_MS:
        candidate.boot_timeout_ms = value;
        break;
    case INTEGRATION_CONFIG_INTENT_RESTART_LIMIT:
        candidate.restart_limit = value;
        break;
    case INTEGRATION_CONFIG_INTENT_SERVICE_HEALTH_TIMEOUT_MS:
        candidate.service_health_timeout_ms = value;
        break;
    default:
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }

#define APPEND_FIELD(field, name) do { \
    size_t written; \
    if (position >= sizeof(text)) { \
        return INTEGRATION_CONFIG_INTENT_BAD_STATE; \
    } \
    written = integration_config_intent_append_field( \
        text + position, sizeof(text) - position, \
        name, sizeof(name) - 1U, candidate.field); \
    if (written == 0U || written > sizeof(text) - position) { \
        return INTEGRATION_CONFIG_INTENT_BAD_STATE; \
    } \
    position += written; \
} while (0)

    APPEND_FIELD(schema_version, "schema_version");
    APPEND_FIELD(recovery_mode, "recovery_mode");
    APPEND_FIELD(max_processes, "max_processes");
    APPEND_FIELD(max_user_memory_pages, "max_user_memory_pages");
    APPEND_FIELD(audit_ring_capacity, "audit_ring_capacity");
    APPEND_FIELD(boot_timeout_ms, "boot_timeout_ms");
    APPEND_FIELD(restart_limit, "restart_limit");
    APPEND_FIELD(service_health_timeout_ms,
                 "service_health_timeout_ms");

#undef APPEND_FIELD

    if (position == 0U ||
        position > INTEGRATION_CONFIG_INTENT_TEXT_MAX) {
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
    memcpy(out, text, position);
    *length_out = position;
    return INTEGRATION_CONFIG_INTENT_OK;
}

static enum integration_config_intent_status
integration_config_intent_audit_denied(
    struct event_ring *ring,
    const struct audit_correlation *correlation,
    uint64_t detail,
    uint64_t *sequence_out)
{
    struct audit_correlation denied;
    enum audit_correlation_status status;

    if (ring == NULL || correlation == NULL) {
        return INTEGRATION_CONFIG_INTENT_NULL;
    }
    denied = *correlation;
    denied.result = INTEGRATION_CONFIG_INTENT_AUDIT_RESULT_DENIED;
    status = audit_correlation_append(
        ring,
        INTEGRATION_CONFIG_INTENT_AUDIT_OPERATION,
        &denied,
        detail,
        sequence_out);
    if (status != AUDIT_CORRELATION_OK) {
        return INTEGRATION_CONFIG_INTENT_AUDIT_FAILED;
    }
    return INTEGRATION_CONFIG_INTENT_OK;
}

enum integration_config_intent_status integration_config_intent_apply(
    struct config_snapshot *snapshot,
    const struct integration_config_intent *intent,
    const struct authorization_matrix *matrix,
    const uint8_t *roles,
    size_t role_count,
    const uint8_t *capabilities,
    size_t capability_count,
    const struct audit_correlation *correlation,
    config_commit_validator validator,
    void *context,
    struct event_ring *audit_ring,
    uint64_t *audit_sequence_out)
{
    const struct integration_config_intent_field_spec *spec;
    const struct appliance_config *active;
    enum integration_config_intent_status intent_status;
    enum audit_correlation_status correlation_status;
    enum authorization_status authorization_status;
    enum controlled_configuration_status control_status;
    char text[INTEGRATION_CONFIG_INTENT_TEXT_MAX + 1U];
    size_t text_length = 0U;

    if (snapshot == NULL || intent == NULL || matrix == NULL ||
        correlation == NULL || audit_ring == NULL ||
        (role_count > 0U && roles == NULL) ||
        (capability_count > 0U && capabilities == NULL)) {
        return INTEGRATION_CONFIG_INTENT_NULL;
    }
    correlation_status = audit_correlation_validate(correlation);
    if (correlation_status != AUDIT_CORRELATION_OK) {
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
    if (!snapshot->has_active) {
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
    if (!event_ring_is_published(audit_ring)) {
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
    if (event_ring_count(audit_ring) >= KERNEL_EVENT_RING_CAPACITY) {
        return INTEGRATION_CONFIG_INTENT_AUDIT_FAILED;
    }

    intent_status = integration_config_intent_validate(intent);
    if (intent_status != INTEGRATION_CONFIG_INTENT_OK) {
        if (integration_config_intent_audit_denied(
                audit_ring, correlation,
                CONTROLLED_CONFIGURATION_COMMAND_ID,
                audit_sequence_out) != INTEGRATION_CONFIG_INTENT_OK) {
            return INTEGRATION_CONFIG_INTENT_AUDIT_FAILED;
        }
        return intent_status;
    }

    authorization_status = authorization_check(
        matrix,
        CONTROLLED_CONFIGURATION_COMMAND_ID,
        roles,
        role_count,
        capabilities,
        capability_count);
    if (authorization_status != AUTHORIZATION_OK) {
        if (authorization_status == AUTHORIZATION_MISSING_ROLE ||
            authorization_status ==
                AUTHORIZATION_MISSING_CAPABILITY) {
            if (integration_config_intent_audit_denied(
                    audit_ring, correlation,
                    CONTROLLED_CONFIGURATION_COMMAND_ID,
                    audit_sequence_out) !=
                INTEGRATION_CONFIG_INTENT_OK) {
                return INTEGRATION_CONFIG_INTENT_AUDIT_FAILED;
            }
            return INTEGRATION_CONFIG_INTENT_DENIED;
        }
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }

    spec = integration_config_intent_find_field(intent->intent_id);
    active = config_snapshot_active(snapshot);
    if (spec == NULL || active == NULL) {
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
    intent_status = integration_config_intent_render_candidate(
        active, spec, intent->value, text, &text_length);
    if (intent_status != INTEGRATION_CONFIG_INTENT_OK) {
        return intent_status;
    }

    control_status = controlled_configuration_apply_correlated(
        snapshot, text, text_length,
        validator, context, correlation,
        audit_ring, audit_sequence_out);
    switch (control_status) {
    case CONTROLLED_CONFIGURATION_OK:
        return INTEGRATION_CONFIG_INTENT_OK;
    case CONTROLLED_CONFIGURATION_NULL:
        return INTEGRATION_CONFIG_INTENT_NULL;
    case CONTROLLED_CONFIGURATION_BAD_STATE:
    case CONTROLLED_CONFIGURATION_TOO_LONG:
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    case CONTROLLED_CONFIGURATION_PARSE_FAILED:
        return INTEGRATION_CONFIG_INTENT_PARSE_FAILED;
    case CONTROLLED_CONFIGURATION_COMMIT_FAILED:
        return INTEGRATION_CONFIG_INTENT_COMMIT_FAILED;
    case CONTROLLED_CONFIGURATION_AUDIT_FAILED:
        return INTEGRATION_CONFIG_INTENT_AUDIT_FAILED;
    default:
        return INTEGRATION_CONFIG_INTENT_BAD_STATE;
    }
}

const char *integration_config_intent_status_string(
    enum integration_config_intent_status status)
{
    switch (status) {
    case INTEGRATION_CONFIG_INTENT_OK:
        return "INTEGRATION_CONFIG_INTENT_OK";
    case INTEGRATION_CONFIG_INTENT_NULL:
        return "INTEGRATION_CONFIG_INTENT_NULL";
    case INTEGRATION_CONFIG_INTENT_BAD_STATE:
        return "INTEGRATION_CONFIG_INTENT_BAD_STATE";
    case INTEGRATION_CONFIG_INTENT_BAD_VERSION:
        return "INTEGRATION_CONFIG_INTENT_BAD_VERSION";
    case INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID:
        return "INTEGRATION_CONFIG_INTENT_INVALID_PEER_ID";
    case INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID:
        return "INTEGRATION_CONFIG_INTENT_INVALID_INTENT_ID";
    case INTEGRATION_CONFIG_INTENT_INVALID_VALUE:
        return "INTEGRATION_CONFIG_INTENT_INVALID_VALUE";
    case INTEGRATION_CONFIG_INTENT_DENIED:
        return "INTEGRATION_CONFIG_INTENT_DENIED";
    case INTEGRATION_CONFIG_INTENT_PARSE_FAILED:
        return "INTEGRATION_CONFIG_INTENT_PARSE_FAILED";
    case INTEGRATION_CONFIG_INTENT_COMMIT_FAILED:
        return "INTEGRATION_CONFIG_INTENT_COMMIT_FAILED";
    case INTEGRATION_CONFIG_INTENT_AUDIT_FAILED:
        return "INTEGRATION_CONFIG_INTENT_AUDIT_FAILED";
    default:
        return NULL;
    }
}
