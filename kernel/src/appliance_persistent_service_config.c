#include "appliance_persistent_service_config.h"

#include "appliance_service_manager.h"
#include "data_region.h"
#include "service.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char appliance_persistent_service_config_count_key[] =
    "v1:services=";
static const char appliance_persistent_service_config_service_prefix[] =
    "v1:service.";

static void appliance_persistent_service_config_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_persistent_service_config_bytes_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_persistent_service_config_append_bytes(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    const void *source,
    size_t length)
{
    const unsigned char *bytes = source;
    size_t index;

    if (*position > capacity ||
        length > capacity - *position) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*position + index] = bytes[index];
    }
    *position += length;
    return true;
}

static bool appliance_persistent_service_config_append_string(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_persistent_service_config_append_bytes(
        output, capacity, position, text, length);
}

static bool appliance_persistent_service_config_append_u32(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    uint32_t value)
{
    char digits[10U];
    size_t digit_count = 0U;
    uint32_t remaining = value;

    do {
        digits[digit_count++] =
            (char)('0' + (remaining % 10U));
        remaining /= 10U;
    } while (remaining != 0U);

    while (digit_count > 0U) {
        --digit_count;
        if (!appliance_persistent_service_config_append_bytes(
                output, capacity, position,
                digits + digit_count, 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_persistent_service_config_append_values(
    const struct service_descriptor *descriptor,
    unsigned char *output,
    size_t capacity,
    size_t *position)
{
    if (!appliance_persistent_service_config_append_string(
            output, capacity, position,
            appliance_persistent_service_config_service_prefix) ||
        !appliance_persistent_service_config_append_u32(
            output, capacity, position, descriptor->id) ||
        !appliance_persistent_service_config_append_bytes(
            output, capacity, position, "=", 1U) ||
        !appliance_persistent_service_config_append_bytes(
            output, capacity, position,
            descriptor->enabled ? "1" : "0", 1U) ||
        !appliance_persistent_service_config_append_bytes(
            output, capacity, position, ",", 1U) ||
        !appliance_persistent_service_config_append_u32(
            output, capacity, position, descriptor->restart_limit) ||
        !appliance_persistent_service_config_append_bytes(
            output, capacity, position, ",", 1U) ||
        !appliance_persistent_service_config_append_u32(
            output, capacity, position,
            descriptor->readiness_timeout_ms) ||
        !appliance_persistent_service_config_append_bytes(
            output, capacity, position, "\n", 1U)) {
        return false;
    }
    return true;
}

static bool appliance_persistent_service_config_dependency_available(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    const struct service_descriptor *service,
    uint32_t dependency_id)
{
    uint32_t index;

    if (service == NULL) {
        return false;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (descriptors[index].id == dependency_id) {
            return descriptors[index].enabled;
        }
    }
    return false;
}

static bool appliance_persistent_service_config_available(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count)
{
    uint32_t index;
    uint32_t dependency_index;

    if (descriptor_count > 0U && descriptors == NULL) {
        return false;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (!descriptors[index].enabled) {
            continue;
        }
        for (dependency_index = 0U;
             dependency_index < descriptors[index].dependency_count;
             ++dependency_index) {
            if (!appliance_persistent_service_config_dependency_available(
                    descriptors,
                    descriptor_count,
                    &descriptors[index],
                    descriptors[index].dependencies[dependency_index])) {
                return false;
            }
        }
    }
    return true;
}

static bool appliance_persistent_service_config_baseline_valid(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count)
{
    if (descriptor_count > APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY ||
        (descriptor_count > 0U && descriptors == NULL)) {
        return false;
    }
    if (service_dependency_set_validate(
            descriptors, descriptor_count) != SERVICE_DEPENDENCY_OK) {
        return false;
    }
    return appliance_persistent_service_config_available(
        descriptors, descriptor_count);
}

static enum appliance_persistent_service_config_status
appliance_persistent_service_config_map_data_region(
    enum data_region_status status)
{
    switch (status) {
    case DATA_REGION_OK:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
    case DATA_REGION_NULL:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    case DATA_REGION_IO_ERROR:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR;
    case DATA_REGION_BAD_DEVICE:
    case DATA_REGION_UNSUPPORTED_BLOCK_SIZE:
    case DATA_REGION_BAD_MAGIC:
    case DATA_REGION_UNSUPPORTED_VERSION:
    case DATA_REGION_RESERVED_NONZERO:
    case DATA_REGION_INVALID_LAYOUT:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE;
    default:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_service_config_status
appliance_persistent_service_config_map_journal(
    enum config_journal_status status)
{
    switch (status) {
    case CONFIG_JOURNAL_OK:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
    case CONFIG_JOURNAL_NULL:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    case CONFIG_JOURNAL_BAD_DEVICE:
    case CONFIG_JOURNAL_BAD_REGION:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE;
    case CONFIG_JOURNAL_IO_ERROR:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR;
    case CONFIG_JOURNAL_CORRUPT:
    case CONFIG_JOURNAL_BAD_STATE:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT;
    case CONFIG_JOURNAL_BAD_RECORD:
    case CONFIG_JOURNAL_TOO_SMALL:
    case CONFIG_JOURNAL_EMPTY:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD;
    case CONFIG_JOURNAL_FULL:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL;
    default:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_service_config_status
appliance_persistent_service_config_publish_default(
    struct appliance_persistent_service_config *config,
    const struct service_descriptor *baseline,
    uint32_t baseline_count,
    enum appliance_persistent_service_config_source source)
{
    if (config == NULL ||
        (baseline_count > 0U && baseline == NULL)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (!appliance_persistent_service_config_baseline_valid(
            baseline, baseline_count)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }
    if (source !=
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING &&
        source !=
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
    if (baseline_count > 0U) {
        memcpy(config->descriptors, baseline,
               baseline_count * sizeof(config->descriptors[0]));
    }
    config->descriptor_count = baseline_count;
    config->source = source;
    config->initialized = true;
    return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
}

static enum appliance_persistent_service_config_status
appliance_persistent_service_config_build_record(
    const struct service_descriptor *descriptors,
    uint32_t descriptor_count,
    unsigned char *record,
    size_t capacity,
    size_t *record_length)
{
    size_t position = 0U;
    uint32_t index;

    if (record == NULL || record_length == NULL ||
        (descriptor_count > 0U && descriptors == NULL)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (!appliance_persistent_service_config_baseline_valid(
            descriptors, descriptor_count)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }
    if (!appliance_persistent_service_config_append_string(
            record, capacity, &position,
            appliance_persistent_service_config_count_key) ||
        !appliance_persistent_service_config_append_u32(
            record, capacity, &position, descriptor_count) ||
        !appliance_persistent_service_config_append_bytes(
            record, capacity, &position, "\n", 1U)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD;
    }
    for (index = 0U; index < descriptor_count; ++index) {
        if (!appliance_persistent_service_config_append_values(
                &descriptors[index], record, capacity, &position)) {
            return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD;
        }
    }
    if (position > APPLIANCE_PERSISTENT_SERVICE_CONFIG_RECORD_MAX) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD;
    }
    *record_length = position;
    return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
}

static bool appliance_persistent_service_config_parse_decimal(
    const char *value,
    size_t value_length,
    uint64_t *parsed_out)
{
    uint64_t parsed = 0U;
    size_t index;

    if (value == NULL || parsed_out == NULL ||
        value_length == 0U ||
        (value_length > 1U && value[0] == '0')) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        const unsigned char byte =
            (unsigned char)value[index];
        const uint64_t digit =
            (uint64_t)(byte - (unsigned char)'0');

        if (byte < (unsigned char)'0' ||
            byte > (unsigned char)'9' ||
            parsed > (UINT64_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *parsed_out = parsed;
    return true;
}

static bool appliance_persistent_service_config_parse_count(
    const unsigned char *line,
    size_t line_length,
    uint32_t *count_out)
{
    const size_t key_length =
        sizeof(appliance_persistent_service_config_count_key) - 1U;
    uint64_t parsed;

    if (line_length <= key_length ||
        !appliance_persistent_service_config_bytes_equal(
            line, appliance_persistent_service_config_count_key,
            key_length) ||
        !appliance_persistent_service_config_parse_decimal(
            (const char *)line + key_length,
            line_length - key_length,
            &parsed) ||
        parsed > UINT32_MAX) {
        return false;
    }
    *count_out = (uint32_t)parsed;
    return true;
}

static bool appliance_persistent_service_config_parse_service_line(
    const unsigned char *line,
    size_t line_length,
    uint32_t *service_id_out,
    struct appliance_persistent_service_config_values *values_out)
{
    const size_t prefix_length =
        sizeof(appliance_persistent_service_config_service_prefix) - 1U;
    size_t position;
    size_t start;
    uint32_t field;
    uint64_t parsed[3U];

    if (line_length <= prefix_length ||
        !appliance_persistent_service_config_bytes_equal(
            line, appliance_persistent_service_config_service_prefix,
            prefix_length)) {
        return false;
    }

    position = prefix_length;
    start = position;
    while (position < line_length && line[position] != (unsigned char)'=') {
        ++position;
    }
    if (position == start || position >= line_length ||
        !appliance_persistent_service_config_parse_decimal(
            (const char *)line + start,
            position - start,
            &parsed[0]) ||
        parsed[0] > UINT32_MAX ||
        parsed[0] < SERVICE_ID_MIN) {
        return false;
    }
    *service_id_out = (uint32_t)parsed[0];

    ++position;
    start = position;
    for (field = 0U; field < 3U; ++field) {
        size_t token_length;

        while (position < line_length &&
               line[position] != (unsigned char)',') {
            ++position;
        }
        token_length = position - start;
        if (!appliance_persistent_service_config_parse_decimal(
                (const char *)line + start,
                token_length,
                &parsed[field])) {
            return false;
        }
        if (field + 1U < 3U) {
            if (position >= line_length) {
                return false;
            }
            ++position;
            start = position;
        } else if (position != line_length) {
            return false;
        }
    }

    if (parsed[0] > 1U ||
        parsed[1] > SERVICE_RESTART_LIMIT_MAX ||
        parsed[2] < SERVICE_READINESS_TIMEOUT_MIN_MS ||
        parsed[2] > SERVICE_READINESS_TIMEOUT_MAX_MS) {
        return false;
    }
    if (values_out != NULL) {
        values_out->enabled = parsed[0] != 0U;
        values_out->restart_limit = (uint32_t)parsed[1];
        values_out->readiness_timeout_ms = (uint32_t)parsed[2];
    }
    return true;
}

static bool appliance_persistent_service_config_parse_record(
    const void *record,
    size_t record_length,
    const struct service_descriptor *baseline,
    uint32_t baseline_count,
    struct service_descriptor *restored,
    uint32_t *restored_count)
{
    const unsigned char *bytes = record;
    size_t position = 0U;
    uint32_t expected_count;
    uint32_t parsed_lines = 0U;
    bool seen[
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY];

    if (record == NULL ||
        (baseline_count > 0U && baseline == NULL) ||
        restored == NULL || restored_count == NULL) {
        return false;
    }
    if (record_length == 0U ||
        record_length > APPLIANCE_PERSISTENT_SERVICE_CONFIG_RECORD_MAX ||
        bytes[record_length - 1U] != (unsigned char)'\n') {
        return false;
    }

    while (position < record_length) {
        const size_t line_start = position;
        size_t line_length;

        while (position < record_length &&
               bytes[position] != (unsigned char)'\n') {
            ++position;
        }
        line_length = position - line_start;
        if (position >= record_length ||
            (parsed_lines == 0U &&
             !appliance_persistent_service_config_parse_count(
                 bytes + line_start, line_length,
                 &expected_count))) {
            return false;
        }
        ++position;
        if (parsed_lines == 0U) {
            uint32_t index;

            if (expected_count != baseline_count) {
                return false;
            }
            memcpy(restored, baseline,
                   baseline_count * sizeof(restored[0]));
            for (index = 0U;
                 index < APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY;
                 ++index) {
                seen[index] = false;
            }
            parsed_lines = 1U;
            continue;
        }

        {
            struct appliance_persistent_service_config_values values;
            uint32_t service_id;
            uint32_t descriptor_index;
            bool matched = false;

            if (!appliance_persistent_service_config_parse_service_line(
                    bytes + line_start, line_length,
                    &service_id, &values)) {
                return false;
            }
            for (descriptor_index = 0U;
                 descriptor_index < baseline_count;
                 ++descriptor_index) {
                if (restored[descriptor_index].id != service_id) {
                    continue;
                }
                if (seen[descriptor_index]) {
                    return false;
                }
                seen[descriptor_index] = true;
                restored[descriptor_index].enabled = values.enabled;
                restored[descriptor_index].restart_limit =
                    values.restart_limit;
                restored[descriptor_index].readiness_timeout_ms =
                    values.readiness_timeout_ms;
                matched = true;
                break;
            }
            if (!matched ||
                service_descriptor_validate(
                    &restored[descriptor_index]) !=
                    SERVICE_DESCRIPTOR_OK) {
                return false;
            }
        }
        ++parsed_lines;
    }

    if (parsed_lines - 1U != baseline_count) {
        return false;
    }
    if (!appliance_persistent_service_config_baseline_valid(
            restored, baseline_count)) {
        return false;
    }
    *restored_count = baseline_count;
    return true;
}

enum appliance_persistent_service_config_status
appliance_persistent_service_config_load(
    struct appliance_persistent_service_config *config,
    struct block_device *device,
    const struct service_descriptor *baseline,
    uint32_t baseline_count)
{
    struct data_region region;
    enum data_region_status data_region_status;
    enum config_journal_status journal_status;
    struct service_descriptor restored[
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY];
    uint32_t restored_count = 0U;
    unsigned char record[APPLIANCE_PERSISTENT_SERVICE_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL || device == NULL) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (config->initialized) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
    if (baseline_count > 0U && baseline == NULL) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (!appliance_persistent_service_config_baseline_valid(
            baseline, baseline_count)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }

    data_region_status = data_region_discover(device, &region);
    if (appliance_persistent_service_config_map_data_region(
            data_region_status) !=
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK) {
        return appliance_persistent_service_config_map_data_region(
            data_region_status);
    }

    appliance_persistent_service_config_zero_bytes(
        &config->journal, sizeof(config->journal));
    appliance_persistent_service_config_zero_bytes(
        config, sizeof(*config));
    journal_status = config_journal_init(
        &config->journal, device,
        region.block_offset, region.block_count);
    if (appliance_persistent_service_config_map_journal(
            journal_status) !=
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK) {
        return appliance_persistent_service_config_map_journal(
            journal_status);
    }

    appliance_persistent_service_config_zero_bytes(
        record, sizeof(record));
    journal_status = config_journal_head(
        &config->journal, record, sizeof(record), &record_length);
    if (journal_status == CONFIG_JOURNAL_EMPTY) {
        return appliance_persistent_service_config_publish_default(
            config, baseline, baseline_count,
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING);
    }
    if (appliance_persistent_service_config_map_journal(
            journal_status) !=
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK) {
        return appliance_persistent_service_config_map_journal(
            journal_status);
    }

    appliance_persistent_service_config_zero_bytes(
        restored, sizeof(restored));
    if (!appliance_persistent_service_config_parse_record(
            record, record_length,
            baseline, baseline_count,
            restored, &restored_count)) {
        return appliance_persistent_service_config_publish_default(
            config, baseline, baseline_count,
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID);
    }

    memcpy(config->descriptors, restored,
           restored_count * sizeof(config->descriptors[0]));
    config->descriptor_count = restored_count;
    config->source =
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED;
    config->initialized = true;
    return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
}

enum appliance_persistent_service_config_status
appliance_persistent_service_config_set_config(
    struct appliance_persistent_service_config *config,
    uint32_t service_id,
    const struct appliance_persistent_service_config_values *values)
{
    struct service_descriptor candidate[
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_CAPACITY];
    uint32_t target_index = UINT32_MAX;
    uint32_t index;
    enum config_journal_status journal_status;
    unsigned char record[APPLIANCE_PERSISTENT_SERVICE_CONFIG_RECORD_MAX];
    size_t record_length = 0U;
    enum appliance_persistent_service_config_status status;

    if (config == NULL || values == NULL) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (!config->initialized) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
    if (values->restart_limit > SERVICE_RESTART_LIMIT_MAX ||
        values->readiness_timeout_ms <
            SERVICE_READINESS_TIMEOUT_MIN_MS ||
        values->readiness_timeout_ms >
            SERVICE_READINESS_TIMEOUT_MAX_MS) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }

    for (index = 0U; index < config->descriptor_count; ++index) {
        if (config->descriptors[index].id == service_id) {
            target_index = index;
            break;
        }
    }
    if (target_index == UINT32_MAX) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE;
    }

    memcpy(candidate, config->descriptors,
           config->descriptor_count * sizeof(candidate[0]));
    candidate[target_index].enabled = values->enabled;
    candidate[target_index].restart_limit = values->restart_limit;
    candidate[target_index].readiness_timeout_ms =
        values->readiness_timeout_ms;
    if (service_descriptor_validate(
            &candidate[target_index]) != SERVICE_DESCRIPTOR_OK ||
        !appliance_persistent_service_config_available(
            candidate, config->descriptor_count)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }

    status = appliance_persistent_service_config_build_record(
        candidate, config->descriptor_count,
        record, sizeof(record), &record_length);
    if (status != APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK) {
        return status;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    if (appliance_persistent_service_config_map_journal(
            journal_status) !=
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK) {
        return appliance_persistent_service_config_map_journal(
            journal_status);
    }

    memcpy(config->descriptors, candidate,
           config->descriptor_count * sizeof(config->descriptors[0]));
    config->source =
        APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED;
    return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
}

enum appliance_persistent_service_config_status
appliance_persistent_service_config_apply(
    const struct appliance_persistent_service_config *config,
    struct appliance_service_manager *manager,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    enum appliance_service_manager_status manager_status;

    if (config == NULL || manager == NULL ||
        (entry_count > 0U && entries == NULL)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    }
    if (!config->initialized) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE;
    }
    if (service_dependency_set_validate(
            config->descriptors, config->descriptor_count) !=
            SERVICE_DEPENDENCY_OK ||
        !appliance_persistent_service_config_available(
            config->descriptors, config->descriptor_count)) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG;
    }
    if (!manager->initialized ||
        manager->state != APPLIANCE_SERVICE_MANAGER_STATE_STOPPED ||
        manager->graph.published) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE;
    }

    manager_status = appliance_service_manager_configure(
        manager,
        config->descriptors,
        config->descriptor_count,
        entries,
        entry_count);
    switch (manager_status) {
    case APPLIANCE_SERVICE_MANAGER_OK:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK;
    case APPLIANCE_SERVICE_MANAGER_NULL:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL;
    case APPLIANCE_SERVICE_MANAGER_BAD_STATE:
    case APPLIANCE_SERVICE_MANAGER_NOT_CONFIGURED:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE;
    case APPLIANCE_SERVICE_MANAGER_INVALID_DESCRIPTOR_SET:
    case APPLIANCE_SERVICE_MANAGER_INVALID_ENTRIES:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE;
    default:
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE;
    }
}

uint32_t appliance_persistent_service_config_count(
    const struct appliance_persistent_service_config *config)
{
    if (config == NULL || !config->initialized) {
        return 0U;
    }
    return config->descriptor_count;
}

const struct service_descriptor *
appliance_persistent_service_config_descriptor(
    const struct appliance_persistent_service_config *config,
    uint32_t position)
{
    if (config == NULL || !config->initialized ||
        position >= config->descriptor_count) {
        return NULL;
    }
    return &config->descriptors[position];
}

bool appliance_persistent_service_config_enabled(
    const struct appliance_persistent_service_config *config,
    uint32_t service_id)
{
    uint32_t index;

    if (config == NULL || !config->initialized) {
        return false;
    }
    for (index = 0U; index < config->descriptor_count; ++index) {
        if (config->descriptors[index].id == service_id) {
            return config->descriptors[index].enabled;
        }
    }
    return false;
}

enum appliance_persistent_service_config_source
appliance_persistent_service_config_source(
    const struct appliance_persistent_service_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE;
    }
    return config->source;
}

const char *appliance_persistent_service_config_source_string(
    enum appliance_persistent_service_config_source source)
{
    switch (source) {
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_NONE";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_INVALID";
    default:
        return NULL;
    }
}

const char *appliance_persistent_service_config_status_string(
    enum appliance_persistent_service_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE";
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE:
        return "APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE";
    default:
        return NULL;
    }
}
