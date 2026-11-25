#include "config_schema.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define CONFIG_FIELD_COUNT 8U

struct config_field_spec {
    const char *name;
    uint32_t minimum;
    uint32_t maximum;
};

static const struct config_field_spec config_fields[CONFIG_FIELD_COUNT] = {
    {"schema_version", CONFIG_SCHEMA_VERSION, CONFIG_SCHEMA_VERSION},
    {"recovery_mode", 0U, 2U},
    {"max_processes", 1U, 64U},
    {"max_user_memory_pages", 16U, 4096U},
    {"audit_ring_capacity", 16U, 64U},
    {"boot_timeout_ms", 1000U, 60000U},
    {"restart_limit", 0U, 16U},
    {"service_health_timeout_ms", 1000U, 30000U},
};

static bool config_key_char_valid(unsigned char value) {
    return (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool config_value_char_valid(unsigned char value) {
    return value >= (unsigned char)'0' && value <= (unsigned char)'9';
}

static int config_field_index(const char *key, size_t key_length) {
    uint32_t index;

    for (index = 0U; index < CONFIG_FIELD_COUNT; ++index) {
        const size_t name_length = strlen(config_fields[index].name);

        if (name_length == key_length &&
            memcmp(config_fields[index].name, key, key_length) == 0) {
            return (int)index;
        }
    }
    return -1;
}

static bool config_appliance_valid(const struct appliance_config *config) {
    if (config == NULL) {
        return false;
    }
    return config->schema_version == config_fields[0].minimum &&
           config->recovery_mode >= config_fields[1].minimum &&
           config->recovery_mode <= config_fields[1].maximum &&
           config->max_processes >= config_fields[2].minimum &&
           config->max_processes <= config_fields[2].maximum &&
           config->max_user_memory_pages >= config_fields[3].minimum &&
           config->max_user_memory_pages <= config_fields[3].maximum &&
           config->audit_ring_capacity >= config_fields[4].minimum &&
           config->audit_ring_capacity <= config_fields[4].maximum &&
           config->boot_timeout_ms >= config_fields[5].minimum &&
           config->boot_timeout_ms <= config_fields[5].maximum &&
           config->restart_limit >= config_fields[6].minimum &&
           config->restart_limit <= config_fields[6].maximum &&
           config->service_health_timeout_ms >= config_fields[7].minimum &&
           config->service_health_timeout_ms <= config_fields[7].maximum;
}

static enum config_status config_parse_u32(const char *value,
                                           size_t value_length,
                                           uint32_t *value_out)
{
    uint64_t parsed = 0U;
    size_t index;

    for (index = 0U; index < value_length; ++index) {
        const uint32_t digit =
            (uint32_t)((unsigned char)value[index] - (unsigned char)'0');

        if (parsed > ((uint64_t)UINT32_MAX - digit) / 10U) {
            return CONFIG_OUT_OF_RANGE;
        }
        parsed = parsed * 10U + digit;
    }
    *value_out = (uint32_t)parsed;
    return CONFIG_OK;
}

static enum config_status config_parse_line(
    const char *line,
    size_t line_length,
    struct appliance_config *draft,
    bool *seen)
{
    size_t equals_index = 0U;
    bool equals_found = false;
    size_t key_length;
    size_t value_length;
    size_t index;
    int field;
    enum config_status status;
    uint32_t parsed_value = 0U;

    if (line_length == 0U || line_length > CONFIG_LINE_MAX) {
        return CONFIG_MALFORMED_FIELD;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == '=') {
            if (equals_found) {
                return CONFIG_MALFORMED_FIELD;
            }
            equals_found = true;
            equals_index = index;
        }
    }
    if (!equals_found) {
        return CONFIG_MALFORMED_FIELD;
    }

    key_length = equals_index;
    value_length = line_length - equals_index - 1U;
    if (key_length == 0U || key_length > CONFIG_KEY_MAX ||
        value_length == 0U || value_length > CONFIG_VALUE_MAX) {
        return CONFIG_MALFORMED_FIELD;
    }
    for (index = 0U; index < key_length; ++index) {
        if (!config_key_char_valid((unsigned char)line[index])) {
            return CONFIG_MALFORMED_FIELD;
        }
    }
    for (index = 0U; index < value_length; ++index) {
        if (!config_value_char_valid(
                (unsigned char)line[equals_index + 1U + index])) {
            return CONFIG_MALFORMED_FIELD;
        }
    }

    field = config_field_index(line, key_length);
    if (field < 0) {
        return CONFIG_UNKNOWN_FIELD;
    }
    if (seen[field]) {
        return CONFIG_DUPLICATE_FIELD;
    }

    status = config_parse_u32(line + equals_index + 1U, value_length,
                              &parsed_value);
    if (status != CONFIG_OK) {
        return status;
    }
    if (parsed_value < config_fields[field].minimum ||
        parsed_value > config_fields[field].maximum) {
        if (field == 0) {
            return CONFIG_BAD_VERSION;
        }
        return CONFIG_OUT_OF_RANGE;
    }

    switch (field) {
    case 0:
        draft->schema_version = parsed_value;
        break;
    case 1:
        draft->recovery_mode = parsed_value;
        break;
    case 2:
        draft->max_processes = parsed_value;
        break;
    case 3:
        draft->max_user_memory_pages = parsed_value;
        break;
    case 4:
        draft->audit_ring_capacity = parsed_value;
        break;
    case 5:
        draft->boot_timeout_ms = parsed_value;
        break;
    case 6:
        draft->restart_limit = parsed_value;
        break;
    default:
        draft->service_health_timeout_ms = parsed_value;
        break;
    }
    seen[field] = true;
    return CONFIG_OK;
}

enum config_status config_schema_parse(
    const char *input,
    size_t input_length,
    struct appliance_config *config_out)
{
    struct appliance_config draft;
    bool seen[CONFIG_FIELD_COUNT] = {false};
    size_t position = 0U;
    uint32_t index;

    if (input == NULL || config_out == NULL) {
        return CONFIG_NULL;
    }
    if (input_length > CONFIG_INPUT_MAX) {
        return CONFIG_TOO_LONG;
    }
    memset(&draft, 0, sizeof(draft));

    while (position < input_length) {
        const size_t line_start = position;
        size_t line_length;
        enum config_status status;

        while (position < input_length && input[position] != '\n') {
            ++position;
        }
        line_length = position - line_start;
        status = config_parse_line(input + line_start, line_length,
                                   &draft, seen);
        if (status != CONFIG_OK) {
            return status;
        }
        if (position < input_length) {
            ++position;
        }
    }

    for (index = 0U; index < CONFIG_FIELD_COUNT; ++index) {
        if (!seen[index]) {
            return CONFIG_MISSING_FIELD;
        }
    }
    *config_out = draft;
    return CONFIG_OK;
}

const char *config_status_string(enum config_status status) {
    switch (status) {
    case CONFIG_OK:
        return "CONFIG_OK";
    case CONFIG_NULL:
        return "CONFIG_NULL";
    case CONFIG_TOO_LONG:
        return "CONFIG_TOO_LONG";
    case CONFIG_MISSING_FIELD:
        return "CONFIG_MISSING_FIELD";
    case CONFIG_DUPLICATE_FIELD:
        return "CONFIG_DUPLICATE_FIELD";
    case CONFIG_UNKNOWN_FIELD:
        return "CONFIG_UNKNOWN_FIELD";
    case CONFIG_MALFORMED_FIELD:
        return "CONFIG_MALFORMED_FIELD";
    case CONFIG_BAD_VERSION:
        return "CONFIG_BAD_VERSION";
    case CONFIG_OUT_OF_RANGE:
        return "CONFIG_OUT_OF_RANGE";
    default:
        return "CONFIG_UNKNOWN";
    }
}

enum config_snapshot_status config_snapshot_init(
    struct config_snapshot *snapshot,
    const struct appliance_config *initial)
{
    if (snapshot == NULL || initial == NULL) {
        return CONFIG_SNAPSHOT_NULL;
    }
    if (snapshot->has_active) {
        return CONFIG_SNAPSHOT_BAD_STATE;
    }
    if (!config_appliance_valid(initial)) {
        return CONFIG_SNAPSHOT_INVALID_CONFIG;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->active = *initial;
    snapshot->has_active = true;
    return CONFIG_SNAPSHOT_OK;
}

enum config_snapshot_status config_snapshot_prepare(
    struct config_snapshot *snapshot,
    const char *input,
    size_t input_length)
{
    struct appliance_config candidate;
    enum config_status status;

    if (snapshot == NULL || input == NULL) {
        return CONFIG_SNAPSHOT_NULL;
    }
    if (!snapshot->has_active) {
        return CONFIG_SNAPSHOT_BAD_STATE;
    }
    status = config_schema_parse(input, input_length, &candidate);
    if (status != CONFIG_OK) {
        return CONFIG_SNAPSHOT_PARSE_FAILED;
    }
    snapshot->pending = candidate;
    snapshot->has_pending = true;
    return CONFIG_SNAPSHOT_OK;
}

enum config_snapshot_status config_snapshot_commit(
    struct config_snapshot *snapshot,
    config_commit_validator validator,
    void *context)
{
    if (snapshot == NULL) {
        return CONFIG_SNAPSHOT_NULL;
    }
    if (!snapshot->has_active) {
        return CONFIG_SNAPSHOT_BAD_STATE;
    }
    if (!snapshot->has_pending) {
        return CONFIG_SNAPSHOT_NO_PENDING;
    }
    if (validator != NULL && !validator(&snapshot->pending, context)) {
        memset(&snapshot->pending, 0, sizeof(snapshot->pending));
        snapshot->has_pending = false;
        return CONFIG_SNAPSHOT_COMMIT_FAILED;
    }
    snapshot->active = snapshot->pending;
    memset(&snapshot->pending, 0, sizeof(snapshot->pending));
    snapshot->has_pending = false;
    return CONFIG_SNAPSHOT_OK;
}

enum config_snapshot_status config_snapshot_rollback(
    struct config_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return CONFIG_SNAPSHOT_NULL;
    }
    if (!snapshot->has_active) {
        return CONFIG_SNAPSHOT_BAD_STATE;
    }
    if (!snapshot->has_pending) {
        return CONFIG_SNAPSHOT_NO_PENDING;
    }
    memset(&snapshot->pending, 0, sizeof(snapshot->pending));
    snapshot->has_pending = false;
    return CONFIG_SNAPSHOT_OK;
}

enum config_snapshot_status config_snapshot_destroy(
    struct config_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return CONFIG_SNAPSHOT_NULL;
    }
    if (!snapshot->has_active) {
        return CONFIG_SNAPSHOT_BAD_STATE;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    return CONFIG_SNAPSHOT_OK;
}

const struct appliance_config *config_snapshot_active(
    const struct config_snapshot *snapshot)
{
    return snapshot != NULL && snapshot->has_active
        ? &snapshot->active
        : NULL;
}

bool config_snapshot_has_pending(const struct config_snapshot *snapshot) {
    return snapshot != NULL && snapshot->has_pending;
}

const char *config_snapshot_status_string(enum config_snapshot_status status) {
    switch (status) {
    case CONFIG_SNAPSHOT_OK:
        return "CONFIG_SNAPSHOT_OK";
    case CONFIG_SNAPSHOT_NULL:
        return "CONFIG_SNAPSHOT_NULL";
    case CONFIG_SNAPSHOT_BAD_STATE:
        return "CONFIG_SNAPSHOT_BAD_STATE";
    case CONFIG_SNAPSHOT_NO_PENDING:
        return "CONFIG_SNAPSHOT_NO_PENDING";
    case CONFIG_SNAPSHOT_PARSE_FAILED:
        return "CONFIG_SNAPSHOT_PARSE_FAILED";
    case CONFIG_SNAPSHOT_COMMIT_FAILED:
        return "CONFIG_SNAPSHOT_COMMIT_FAILED";
    case CONFIG_SNAPSHOT_INVALID_CONFIG:
        return "CONFIG_SNAPSHOT_INVALID_CONFIG";
    default:
        return "CONFIG_SNAPSHOT_UNKNOWN";
    }
}
