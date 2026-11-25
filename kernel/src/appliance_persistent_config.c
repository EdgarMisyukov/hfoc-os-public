#include "appliance_persistent_config.h"

#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char appliance_persistent_config_prefix[] =
    "v1:system.hostname=";

static const char appliance_persistent_config_language_prefix[] =
    APPLIANCE_CONFIG_VALIDATION_LANGUAGE_PREFIX;

static const char appliance_persistent_config_panel_after_boot_prefix[] =
    APPLIANCE_CONFIG_VALIDATION_PANEL_AFTER_BOOT_PREFIX;

static void appliance_persistent_config_zero_bytes(void *destination,
                                                   size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_persistent_config_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool appliance_persistent_config_bytes_equal(
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

static enum appliance_persistent_config_status
appliance_persistent_config_map_data_region(
    enum data_region_status status)
{
    switch (status) {
    case DATA_REGION_OK:
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    case DATA_REGION_NULL:
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    case DATA_REGION_IO_ERROR:
        return APPLIANCE_PERSISTENT_CONFIG_IO_ERROR;
    case DATA_REGION_BAD_DEVICE:
    case DATA_REGION_UNSUPPORTED_BLOCK_SIZE:
    case DATA_REGION_BAD_MAGIC:
    case DATA_REGION_UNSUPPORTED_VERSION:
    case DATA_REGION_RESERVED_NONZERO:
    case DATA_REGION_INVALID_LAYOUT:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE;
    default:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_config_status
appliance_persistent_config_map_journal(
    enum config_journal_status status)
{
    switch (status) {
    case CONFIG_JOURNAL_OK:
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    case CONFIG_JOURNAL_NULL:
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    case CONFIG_JOURNAL_BAD_DEVICE:
    case CONFIG_JOURNAL_BAD_REGION:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE;
    case CONFIG_JOURNAL_IO_ERROR:
        return APPLIANCE_PERSISTENT_CONFIG_IO_ERROR;
    case CONFIG_JOURNAL_CORRUPT:
    case CONFIG_JOURNAL_BAD_STATE:
        return APPLIANCE_PERSISTENT_CONFIG_CORRUPT;
    case CONFIG_JOURNAL_BAD_RECORD:
    case CONFIG_JOURNAL_TOO_SMALL:
    case CONFIG_JOURNAL_EMPTY:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD;
    case CONFIG_JOURNAL_FULL:
        return APPLIANCE_PERSISTENT_CONFIG_FULL;
    default:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_config_status
appliance_persistent_config_map_validation(
    enum appliance_config_validation_status status)
{
    switch (status) {
    case APPLIANCE_CONFIG_VALIDATION_OK:
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    case APPLIANCE_CONFIG_VALIDATION_NULL:
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    case APPLIANCE_CONFIG_VALIDATION_BAD_STATE:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    case APPLIANCE_CONFIG_VALIDATION_BAD_RECORD:
    case APPLIANCE_CONFIG_VALIDATION_UNSUPPORTED_VERSION:
    case APPLIANCE_CONFIG_VALIDATION_UNKNOWN_KEY:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD;
    case APPLIANCE_CONFIG_VALIDATION_BAD_HOSTNAME:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME;
    case APPLIANCE_CONFIG_VALIDATION_BAD_LANGUAGE:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE;
    case APPLIANCE_CONFIG_VALIDATION_BAD_PANEL_AFTER_BOOT:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT;
    case APPLIANCE_CONFIG_VALIDATION_BUFFER_TOO_SMALL:
        return APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL;
    default:
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_config_status
appliance_persistent_config_build_record(
    const char *hostname,
    size_t hostname_length,
    enum appliance_language language,
    bool panel_after_boot_open,
    unsigned char *record,
    size_t record_capacity,
    size_t *record_length_out)
{
    size_t prefix_length =
        sizeof(appliance_persistent_config_prefix) - 1U;
    size_t language_prefix_length =
        sizeof(appliance_persistent_config_language_prefix) - 1U;
    size_t panel_prefix_length =
        sizeof(appliance_persistent_config_panel_after_boot_prefix) - 1U;
    const char *code;
    const char *panel_value;
    size_t code_length = 0U;
    size_t panel_value_length = 0U;
    size_t record_length;
    size_t position;

    if (record == NULL || record_length_out == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (appliance_config_validation_validate_hostname(
            hostname, hostname_length) !=
        APPLIANCE_CONFIG_VALIDATION_OK) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME;
    }
    code = appliance_language_code(language);
    if (code == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE;
    }
    while (code[code_length] != '\0') {
        ++code_length;
    }
    if (code_length > APPLIANCE_PERSISTENT_CONFIG_LANGUAGE_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE;
    }
    /*
     * ITEM-516: only the non-default `open` value is ever written.  `closed`
     * is the default, so a record that never enabled the policy is the record
     * of the previous format, byte for byte.
     */
    panel_value = appliance_config_validation_panel_after_boot_value(
        panel_after_boot_open);
    if (panel_value == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT;
    }
    if (panel_after_boot_open) {
        while (panel_value[panel_value_length] != '\0') {
            ++panel_value_length;
        }
        if (panel_value_length >
            APPLIANCE_PERSISTENT_CONFIG_PANEL_AFTER_BOOT_MAX) {
            return APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT;
        }
    }

    record_length = prefix_length + hostname_length + 1U;
    if (language != APPLIANCE_PERSISTENT_CONFIG_DEFAULT_LANGUAGE) {
        record_length += language_prefix_length + code_length + 1U;
    }
    if (panel_after_boot_open) {
        record_length += panel_prefix_length + panel_value_length + 1U;
    }
    if (record_length > record_capacity ||
        record_length > APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD;
    }

    appliance_persistent_config_copy_bytes(
        record, appliance_persistent_config_prefix, prefix_length);
    appliance_persistent_config_copy_bytes(
        record + prefix_length, hostname, hostname_length);
    position = prefix_length + hostname_length;
    record[position] = (unsigned char)'\n';
    ++position;
    if (language != APPLIANCE_PERSISTENT_CONFIG_DEFAULT_LANGUAGE) {
        appliance_persistent_config_copy_bytes(
            record + position, appliance_persistent_config_language_prefix,
            language_prefix_length);
        position += language_prefix_length;
        appliance_persistent_config_copy_bytes(
            record + position, code, code_length);
        position += code_length;
        record[position] = (unsigned char)'\n';
        ++position;
    }
    if (panel_after_boot_open) {
        appliance_persistent_config_copy_bytes(
            record + position,
            appliance_persistent_config_panel_after_boot_prefix,
            panel_prefix_length);
        position += panel_prefix_length;
        appliance_persistent_config_copy_bytes(
            record + position, panel_value, panel_value_length);
        position += panel_value_length;
        record[position] = (unsigned char)'\n';
        ++position;
    }
    *record_length_out = position;
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_persistent_config_status
appliance_persistent_config_load(
    struct appliance_persistent_config *config,
    struct block_device *device)
{
    struct data_region region;
    enum data_region_status data_region_status;
    enum config_journal_status journal_status;
    enum appliance_config_validation_status validation_status;
    enum appliance_persistent_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL || device == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (config->initialized) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }

    data_region_status = data_region_discover(device, &region);
    status = appliance_persistent_config_map_data_region(
        data_region_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    appliance_persistent_config_zero_bytes(
        &config->journal, sizeof(config->journal));
    appliance_persistent_config_zero_bytes(
        &config->values, sizeof(config->values));
    journal_status = config_journal_init(
        &config->journal, device,
        region.block_offset, region.block_count);
    status = appliance_persistent_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    appliance_persistent_config_zero_bytes(
        record, sizeof(record));
    journal_status = config_journal_head(
        &config->journal, record, sizeof(record), &record_length);
    if (journal_status == CONFIG_JOURNAL_EMPTY) {
        validation_status =
            appliance_config_validation_publish_default(
                &config->values,
                APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING);
        status = appliance_persistent_config_map_validation(
            validation_status);
        if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
            return status;
        }
        config->initialized = true;
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    }
    status = appliance_persistent_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    validation_status = appliance_config_validation_validate_record(
        record, record_length, &config->values);
    if (validation_status != APPLIANCE_CONFIG_VALIDATION_OK) {
        appliance_persistent_config_zero_bytes(
            &config->values, sizeof(config->values));
        validation_status =
            appliance_config_validation_publish_default(
                &config->values,
                APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID);
        status = appliance_persistent_config_map_validation(
            validation_status);
        if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
            return status;
        }
        config->initialized = true;
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    }
    config->initialized = true;
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_persistent_config_status
appliance_persistent_config_set_hostname(
    struct appliance_persistent_config *config,
    const char *hostname,
    size_t hostname_length)
{
    enum config_journal_status journal_status;
    enum appliance_config_validation_status validation_status;
    enum appliance_persistent_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL || hostname == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized ||
        !config->values.initialized ||
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
        config->values.hostname_length == 0U ||
        config->values.hostname_length >
            APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    if (appliance_config_validation_validate_hostname(
            hostname, hostname_length) !=
        APPLIANCE_CONFIG_VALIDATION_OK) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME;
    }
    if (hostname_length == config->values.hostname_length &&
        appliance_persistent_config_bytes_equal(
            config->values.hostname, hostname, hostname_length) &&
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    }

    status = appliance_persistent_config_build_record(
        hostname, hostname_length, config->values.language,
        config->values.panel_after_boot_open, record,
        sizeof(record), &record_length);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    validation_status =
        appliance_config_validation_publish_hostname(
            &config->values, hostname, hostname_length,
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED);
    status = appliance_persistent_config_map_validation(
        validation_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_persistent_config_status
appliance_persistent_config_hostname(
    const struct appliance_persistent_config *config,
    char *hostname,
    size_t capacity,
    size_t *hostname_length)
{
    if (config == NULL || hostname == NULL ||
        hostname_length == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized ||
        !config->values.initialized ||
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
        config->values.hostname_length == 0U ||
        config->values.hostname_length >
            APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    if (capacity < (size_t)config->values.hostname_length + 1U) {
        return APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL;
    }

    appliance_persistent_config_copy_bytes(
        hostname, config->values.hostname,
        config->values.hostname_length);
    hostname[config->values.hostname_length] = '\0';
    *hostname_length = config->values.hostname_length;
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_persistent_config_status
appliance_persistent_config_set_language(
    struct appliance_persistent_config *config,
    enum appliance_language language)
{
    enum config_journal_status journal_status;
    enum appliance_config_validation_status validation_status;
    enum appliance_persistent_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized ||
        !config->values.initialized ||
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
        config->values.hostname_length == 0U ||
        config->values.hostname_length >
            APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    if (appliance_language_code(language) == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE;
    }
    if (language == config->values.language &&
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    }

    status = appliance_persistent_config_build_record(
        config->values.hostname, config->values.hostname_length, language,
        config->values.panel_after_boot_open, record, sizeof(record),
        &record_length);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    validation_status = appliance_config_validation_publish_language(
        &config->values, language,
        APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED);
    return appliance_persistent_config_map_validation(
        validation_status);
}

enum appliance_persistent_config_status
appliance_persistent_config_language(
    const struct appliance_persistent_config *config,
    enum appliance_language *language)
{
    enum appliance_config_validation_status validation_status;

    if (config == NULL || language == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    validation_status = appliance_config_validation_language(
        &config->values, language);
    if (validation_status != APPLIANCE_CONFIG_VALIDATION_OK) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_persistent_config_status
appliance_persistent_config_set_panel_after_boot(
    struct appliance_persistent_config *config,
    bool open)
{
    enum config_journal_status journal_status;
    enum appliance_config_validation_status validation_status;
    enum appliance_persistent_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized ||
        !config->values.initialized ||
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
        config->values.hostname_length == 0U ||
        config->values.hostname_length >
            APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    if (open == config->values.panel_after_boot_open &&
        config->values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED) {
        return APPLIANCE_PERSISTENT_CONFIG_OK;
    }

    status = appliance_persistent_config_build_record(
        config->values.hostname, config->values.hostname_length,
        config->values.language, open, record, sizeof(record),
        &record_length);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_config_map_journal(journal_status);
    if (status != APPLIANCE_PERSISTENT_CONFIG_OK) {
        return status;
    }

    validation_status =
        appliance_config_validation_publish_panel_after_boot(
            &config->values, open,
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED);
    return appliance_persistent_config_map_validation(validation_status);
}

enum appliance_persistent_config_status
appliance_persistent_config_panel_after_boot(
    const struct appliance_persistent_config *config,
    bool *open_out)
{
    enum appliance_config_validation_status validation_status;

    if (config == NULL || open_out == NULL) {
        return APPLIANCE_PERSISTENT_CONFIG_NULL;
    }
    if (!config->initialized) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    validation_status = appliance_config_validation_panel_after_boot(
        &config->values, open_out);
    if (validation_status != APPLIANCE_CONFIG_VALIDATION_OK) {
        return APPLIANCE_PERSISTENT_CONFIG_BAD_STATE;
    }
    return APPLIANCE_PERSISTENT_CONFIG_OK;
}

enum appliance_config_validation_source
appliance_persistent_config_source(
    const struct appliance_persistent_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE;
    }
    return appliance_config_validation_source(&config->values);
}

const char *appliance_persistent_config_status_string(
    enum appliance_persistent_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_CONFIG_OK:
        return "APPLIANCE_PERSISTENT_CONFIG_OK";
    case APPLIANCE_PERSISTENT_CONFIG_NULL:
        return "APPLIANCE_PERSISTENT_CONFIG_NULL";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_STATE:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_STATE";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE";
    case APPLIANCE_PERSISTENT_CONFIG_IO_ERROR:
        return "APPLIANCE_PERSISTENT_CONFIG_IO_ERROR";
    case APPLIANCE_PERSISTENT_CONFIG_CORRUPT:
        return "APPLIANCE_PERSISTENT_CONFIG_CORRUPT";
    case APPLIANCE_PERSISTENT_CONFIG_FULL:
        return "APPLIANCE_PERSISTENT_CONFIG_FULL";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME";
    case APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL:
        return "APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_LANGUAGE";
    case APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT:
        return "APPLIANCE_PERSISTENT_CONFIG_BAD_PANEL_AFTER_BOOT";
    default:
        return NULL;
    }
}
