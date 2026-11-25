#include "appliance_persistent_network_config.h"

#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const char appliance_persistent_network_config_interface_key[] =
    "v1:network.interface=";
static const char appliance_persistent_network_config_mode_key[] =
    "v1:network.mode=";
static const char appliance_persistent_network_config_address_key[] =
    "v1:network.ipv4_address=";
static const char appliance_persistent_network_config_prefix_key[] =
    "v1:network.ipv4_prefix=";
static const char appliance_persistent_network_config_gateway_key[] =
    "v1:network.ipv4_gateway=";
static const char appliance_persistent_network_config_dns_key[] =
    "v1:network.ipv4_dns=";

static void appliance_persistent_network_config_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_persistent_network_config_copy_bytes(
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

static bool appliance_persistent_network_config_bytes_equal(
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

static bool appliance_persistent_network_config_append_bytes(
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

static bool appliance_persistent_network_config_append_string(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_persistent_network_config_append_bytes(
        output, capacity, position, text, length);
}

static bool appliance_persistent_network_config_append_u8(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    uint8_t value)
{
    char digits[3U];
    size_t digit_count = 0U;
    uint16_t remaining = value;

    do {
        digits[digit_count++] =
            (char)('0' + (remaining % 10U));
        remaining /= 10U;
    } while (remaining != 0U);

    while (digit_count > 0U) {
        if (!appliance_persistent_network_config_append_bytes(
                output, capacity, position,
                digits + digit_count - 1U, 1U)) {
            return false;
        }
        --digit_count;
    }
    return true;
}

static bool appliance_persistent_network_config_append_address(
    unsigned char *output,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t octet;

    for (octet = 0U;
         octet < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet) {
        if (!appliance_persistent_network_config_append_u8(
                output, capacity, position,
                address->octets[octet]) ||
            (octet + 1U < NETWORK_IPV4_ADDRESS_OCTETS &&
             !appliance_persistent_network_config_append_bytes(
                 output, capacity, position, ".", 1U))) {
            return false;
        }
    }
    return true;
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_map_data_region(
    enum data_region_status status)
{
    switch (status) {
    case DATA_REGION_OK:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    case DATA_REGION_NULL:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    case DATA_REGION_IO_ERROR:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR;
    case DATA_REGION_BAD_DEVICE:
    case DATA_REGION_UNSUPPORTED_BLOCK_SIZE:
    case DATA_REGION_BAD_MAGIC:
    case DATA_REGION_UNSUPPORTED_VERSION:
    case DATA_REGION_RESERVED_NONZERO:
    case DATA_REGION_INVALID_LAYOUT:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE;
    default:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_map_journal(
    enum config_journal_status status)
{
    switch (status) {
    case CONFIG_JOURNAL_OK:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    case CONFIG_JOURNAL_NULL:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    case CONFIG_JOURNAL_BAD_DEVICE:
    case CONFIG_JOURNAL_BAD_REGION:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE;
    case CONFIG_JOURNAL_IO_ERROR:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR;
    case CONFIG_JOURNAL_CORRUPT:
    case CONFIG_JOURNAL_BAD_STATE:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT;
    case CONFIG_JOURNAL_BAD_RECORD:
    case CONFIG_JOURNAL_TOO_SMALL:
    case CONFIG_JOURNAL_EMPTY:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    case CONFIG_JOURNAL_FULL:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL;
    default:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_publish_default(
    struct appliance_persistent_network_config *config,
    enum appliance_persistent_network_config_source source)
{
    enum appliance_network_config_status model_status;

    model_status = appliance_network_config_publish_default(&config->values);
    if (model_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    config->source = source;
    return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_build_record(
    const struct appliance_network_config *config,
    unsigned char *record,
    size_t capacity,
    size_t *record_length)
{
    const struct network_ipv4_config *static_config;
    size_t position = 0U;

    if (config == NULL || record == NULL || record_length == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (appliance_network_config_validate(config) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    if (!appliance_persistent_network_config_append_string(
            record, capacity, &position,
            appliance_persistent_network_config_interface_key) ||
        !appliance_persistent_network_config_append_bytes(
            record, capacity, &position,
            config->interface, config->interface_length) ||
        !appliance_persistent_network_config_append_bytes(
            record, capacity, &position, "\n", 1U) ||
        !appliance_persistent_network_config_append_string(
            record, capacity, &position,
            appliance_persistent_network_config_mode_key) ||
        !appliance_persistent_network_config_append_string(
            record, capacity, &position,
            appliance_network_config_mode_string(config->mode)) ||
        !appliance_persistent_network_config_append_bytes(
            record, capacity, &position, "\n", 1U)) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }

    if (config->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        static_config = &config->static_config;
        if (!appliance_persistent_network_config_append_string(
                record, capacity, &position,
                appliance_persistent_network_config_address_key) ||
            !appliance_persistent_network_config_append_address(
                record, capacity, &position,
                &static_config->address) ||
            !appliance_persistent_network_config_append_bytes(
                record, capacity, &position, "\n", 1U) ||
            !appliance_persistent_network_config_append_string(
                record, capacity, &position,
                appliance_persistent_network_config_prefix_key) ||
            !appliance_persistent_network_config_append_u8(
                record, capacity, &position,
                static_config->prefix_length) ||
            !appliance_persistent_network_config_append_bytes(
                record, capacity, &position, "\n", 1U) ||
            !appliance_persistent_network_config_append_string(
                record, capacity, &position,
                appliance_persistent_network_config_gateway_key) ||
            !appliance_persistent_network_config_append_address(
                record, capacity, &position,
                &static_config->gateway) ||
            !appliance_persistent_network_config_append_bytes(
                record, capacity, &position, "\n", 1U) ||
            !appliance_persistent_network_config_append_string(
                record, capacity, &position,
                appliance_persistent_network_config_dns_key) ||
            !appliance_persistent_network_config_append_address(
                record, capacity, &position,
                &static_config->dns) ||
            !appliance_persistent_network_config_append_bytes(
                record, capacity, &position, "\n", 1U)) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
    }
    if (position > APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }
    *record_length = position;
    return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

static bool appliance_persistent_network_config_parse_decimal(
    const char *value,
    size_t value_length,
    uint32_t *parsed_out)
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
            byte > (unsigned char)'9') {
            return false;
        }
        if (parsed >
            ((uint64_t)UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    if (parsed > UINT32_MAX) {
        return false;
    }
    *parsed_out = (uint32_t)parsed;
    return true;
}

static bool appliance_persistent_network_config_parse_address(
    const char *value,
    size_t value_length,
    struct network_ipv4_address *address_out)
{
    struct network_ipv4_address draft;
    size_t position = 0U;
    size_t octet_index;

    if (value == NULL || address_out == NULL) {
        return false;
    }
    appliance_persistent_network_config_zero_bytes(
        &draft, sizeof(draft));
    for (octet_index = 0U;
         octet_index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet_index) {
        const size_t octet_start = position;
        size_t octet_length = 0U;
        uint32_t octet_value = 0U;

        while (position < value_length &&
               value[position] != '.') {
            ++position;
            ++octet_length;
        }
        if (octet_length == 0U ||
            octet_length > 3U ||
            !appliance_persistent_network_config_parse_decimal(
                value + octet_start, octet_length,
                &octet_value) ||
            octet_value > 255U) {
            return false;
        }
        draft.octets[octet_index] = (uint8_t)octet_value;
        if (octet_index + 1U < NETWORK_IPV4_ADDRESS_OCTETS) {
            if (position >= value_length ||
                value[position] != '.') {
                return false;
            }
            ++position;
        }
    }
    if (position != value_length) {
        return false;
    }
    *address_out = draft;
    return true;
}

enum appliance_persistent_network_config_field {
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE = 0,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_MODE = 1,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS = 2,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX = 3,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY = 4,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS = 5,
    APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_COUNT = 6,
};

static bool appliance_persistent_network_config_field_is(
    const unsigned char *key,
    size_t key_length,
    const char *expected,
    enum appliance_persistent_network_config_field *field,
    enum appliance_persistent_network_config_field field_id)
{
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    if (key_length != expected_length ||
        !appliance_persistent_network_config_bytes_equal(
            key, expected, expected_length)) {
        return false;
    }
    *field = field_id;
    return true;
}

static bool appliance_persistent_network_config_line_field(
    const unsigned char *line,
    size_t line_length,
    enum appliance_persistent_network_config_field *field_out,
    const unsigned char **value_out,
    size_t *value_length_out)
{
    const char *expected_interface =
        "v1:network.interface";
    const char *expected_mode =
        "v1:network.mode";
    const char *expected_address =
        "v1:network.ipv4_address";
    const char *expected_prefix =
        "v1:network.ipv4_prefix";
    const char *expected_gateway =
        "v1:network.ipv4_gateway";
    const char *expected_dns =
        "v1:network.ipv4_dns";
    enum appliance_persistent_network_config_field field =
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE;
    size_t equals_index = 0U;
    bool equals_found = false;
    size_t index;

    if (line == NULL || field_out == NULL ||
        value_out == NULL || value_length_out == NULL) {
        return false;
    }
    if (line_length == 0U ||
        line_length > APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX) {
        return false;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == (unsigned char)'=') {
            if (equals_found) {
                return false;
            }
            equals_found = true;
            equals_index = index;
        } else if (line[index] == (unsigned char)'\n' ||
                   line[index] == (unsigned char)'\r' ||
                   line[index] == (unsigned char)' ') {
            return false;
        }
    }
    if (!equals_found ||
        equals_index == 0U ||
        equals_index == line_length - 1U) {
        return false;
    }
    if (!appliance_persistent_network_config_field_is(
            line, equals_index, expected_interface, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE) &&
        !appliance_persistent_network_config_field_is(
            line, equals_index, expected_mode, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_MODE) &&
        !appliance_persistent_network_config_field_is(
            line, equals_index, expected_address, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS) &&
        !appliance_persistent_network_config_field_is(
            line, equals_index, expected_prefix, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX) &&
        !appliance_persistent_network_config_field_is(
            line, equals_index, expected_gateway, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY) &&
        !appliance_persistent_network_config_field_is(
            line, equals_index, expected_dns, &field,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS)) {
        return false;
    }
    *field_out = field;
    *value_out = line + equals_index + 1U;
    *value_length_out = line_length - equals_index - 1U;
    return true;
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_parse_record(
    const unsigned char *record,
    size_t record_length,
    struct appliance_persistent_network_config *config)
{
    struct appliance_network_config candidate;
    struct network_ipv4_config static_config;
    char interface[APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX + 1U];
    size_t interface_length = 0U;
    char mode[8U];
    size_t mode_length = 0U;
    bool seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_COUNT] = {
        false, false, false, false, false, false
    };
    size_t position = 0U;
    enum appliance_network_config_status model_status;

    if (record == NULL || config == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (record_length == 0U ||
        record_length > APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX ||
        record[record_length - 1U] != (unsigned char)'\n') {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }
    appliance_persistent_network_config_zero_bytes(
        &candidate, sizeof(candidate));
    appliance_persistent_network_config_zero_bytes(
        &static_config, sizeof(static_config));
    static_config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    appliance_persistent_network_config_zero_bytes(
        interface, sizeof(interface));
    appliance_persistent_network_config_zero_bytes(
        mode, sizeof(mode));

    while (position < record_length) {
        const size_t line_start = position;
        size_t line_length = 0U;
        enum appliance_persistent_network_config_field field;
        const unsigned char *value = NULL;
        size_t value_length = 0U;

        while (position < record_length &&
               record[position] != (unsigned char)'\n') {
            ++position;
        }
        if (position >= record_length) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        line_length = position - line_start;
        ++position;
        if (line_length == 0U ||
            !appliance_persistent_network_config_line_field(
                record + line_start, line_length, &field,
                &value, &value_length)) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        if (seen[field]) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        seen[field] = true;
        if ((field == APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE &&
             value_length >
                 APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX) ||
            (field == APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_MODE &&
             value_length > sizeof(mode) - 1U)) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }

        switch (field) {
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE:
            interface_length = value_length;
            appliance_persistent_network_config_copy_bytes(
                interface, value, value_length);
            interface[interface_length] = '\0';
            break;
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_MODE:
            mode_length = value_length;
            appliance_persistent_network_config_copy_bytes(
                mode, value, value_length);
            mode[mode_length] = '\0';
            break;
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS:
            if (!appliance_persistent_network_config_parse_address(
                    (const char *)value, value_length,
                    &static_config.address)) {
                return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
            }
            break;
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX:
        {
            uint32_t prefix = 0U;

            if (!appliance_persistent_network_config_parse_decimal(
                    (const char *)value, value_length,
                    &prefix) ||
                prefix > NETWORK_IPV4_PREFIX_MAX) {
                return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
            }
            static_config.prefix_length = (uint8_t)prefix;
            break;
        }
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY:
            if (!appliance_persistent_network_config_parse_address(
                    (const char *)value, value_length,
                    &static_config.gateway)) {
                return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
            }
            break;
        case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS:
            if (!appliance_persistent_network_config_parse_address(
                    (const char *)value, value_length,
                    &static_config.dns)) {
                return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
            }
            break;
        default:
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
    }

    if (!seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_INTERFACE] ||
        !seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_MODE]) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }
    if (mode_length == 4U &&
        appliance_persistent_network_config_bytes_equal(
            mode, "DHCP", 4U)) {
        if (seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS]) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        model_status = appliance_network_config_publish_dhcp(
            &candidate, interface, interface_length);
    } else if (mode_length == 6U &&
               appliance_persistent_network_config_bytes_equal(
                   mode, "STATIC", 6U)) {
        if (!seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS] ||
            !seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX] ||
            !seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY] ||
            !seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS]) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        if (network_ipv4_config_validate(&static_config) !=
            NETWORK_IPV4_OK) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        model_status = appliance_network_config_publish_static(
            &candidate, interface, interface_length, &static_config);
    } else if (mode_length == 7U &&
               appliance_persistent_network_config_bytes_equal(
                   mode, "OFFLINE", 7U)) {
        if (seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_ADDRESS] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_PREFIX] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_GATEWAY] ||
            seen[APPLIANCE_PERSISTENT_NETWORK_CONFIG_FIELD_DNS]) {
            return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
        }
        model_status = appliance_network_config_publish_offline(
            &candidate, interface, interface_length);
    } else {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }
    if (model_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD;
    }

    config->values = candidate;
    return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

static enum appliance_persistent_network_config_status
appliance_persistent_network_config_commit(
    struct appliance_persistent_network_config *config,
    const struct appliance_network_config *candidate)
{
    enum config_journal_status journal_status;
    enum appliance_persistent_network_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config->source ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
        appliance_persistent_network_config_bytes_equal(
            &config->values, candidate, sizeof(*candidate))) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    }
    status = appliance_persistent_network_config_build_record(
        candidate, record, sizeof(record), &record_length);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return status;
    }
    journal_status = config_journal_append(
        &config->journal, record, record_length);
    status = appliance_persistent_network_config_map_journal(
        journal_status);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return status;
    }
    config->values = *candidate;
    config->source =
        APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED;
    return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

enum appliance_persistent_network_config_status
appliance_persistent_network_config_load(
    struct appliance_persistent_network_config *config,
    struct block_device *device)
{
    struct data_region region;
    enum data_region_status data_region_status;
    enum config_journal_status journal_status;
    enum appliance_persistent_network_config_status status;
    unsigned char record[APPLIANCE_PERSISTENT_NETWORK_CONFIG_RECORD_MAX];
    size_t record_length = 0U;

    if (config == NULL || device == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (config->initialized) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    data_region_status = data_region_discover(device, &region);
    status = appliance_persistent_network_config_map_data_region(
        data_region_status);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return status;
    }

    appliance_persistent_network_config_zero_bytes(
        &config->journal, sizeof(config->journal));
    appliance_persistent_network_config_zero_bytes(
        &config->values, sizeof(config->values));
    config->source = APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE;
    config->initialized = false;
    journal_status = config_journal_init(
        &config->journal, device,
        region.block_offset, region.block_count);
    status = appliance_persistent_network_config_map_journal(
        journal_status);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return status;
    }

    appliance_persistent_network_config_zero_bytes(
        record, sizeof(record));
    journal_status = config_journal_head(
        &config->journal, record, sizeof(record), &record_length);
    if (journal_status == CONFIG_JOURNAL_EMPTY) {
        status = appliance_persistent_network_config_publish_default(
            config,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING);
        if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
            return status;
        }
        config->initialized = true;
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    }
    status = appliance_persistent_network_config_map_journal(
        journal_status);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        return status;
    }

    status = appliance_persistent_network_config_parse_record(
        record, record_length, config);
    if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
        appliance_persistent_network_config_zero_bytes(
            &config->values, sizeof(config->values));
        status = appliance_persistent_network_config_publish_default(
            config,
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID);
        if (status != APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK) {
            return status;
        }
    } else {
        config->source =
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED;
    }
    config->initialized = true;
    return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
}

enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_dhcp(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length)
{
    struct appliance_network_config candidate;
    enum appliance_network_config_status model_status;

    if (config == NULL || interface == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    appliance_persistent_network_config_zero_bytes(
        &candidate, sizeof(candidate));
    model_status = appliance_network_config_publish_dhcp(
        &candidate, interface, interface_length);
    if (model_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG;
    }
    return appliance_persistent_network_config_commit(
        config, &candidate);
}

enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_offline(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length)
{
    struct appliance_network_config candidate;
    enum appliance_network_config_status model_status;

    if (config == NULL || interface == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    appliance_persistent_network_config_zero_bytes(
        &candidate, sizeof(candidate));
    model_status = appliance_network_config_publish_offline(
        &candidate, interface, interface_length);
    if (model_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG;
    }
    return appliance_persistent_network_config_commit(
        config, &candidate);
}

enum appliance_persistent_network_config_status
appliance_persistent_network_config_set_static(
    struct appliance_persistent_network_config *config,
    const char *interface,
    size_t interface_length,
    const struct network_ipv4_config *static_config)
{
    struct appliance_network_config candidate;
    enum appliance_network_config_status model_status;

    if (config == NULL || interface == NULL ||
        static_config == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    appliance_persistent_network_config_zero_bytes(
        &candidate, sizeof(candidate));
    model_status = appliance_network_config_publish_static(
        &candidate, interface, interface_length, static_config);
    if (model_status != APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG;
    }
    return appliance_persistent_network_config_commit(
        config, &candidate);
}

enum appliance_persistent_network_config_status
appliance_persistent_network_config_render(
    const struct appliance_persistent_network_config *config,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    enum appliance_network_config_status model_status;

    if (config == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL;
    }
    if (!config->initialized ||
        config->source == APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
    model_status = appliance_network_config_render(
        &config->values, output, capacity, output_length);
    switch (model_status) {
    case APPLIANCE_NETWORK_CONFIG_OK:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK;
    case APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL;
    default:
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE;
    }
}

const struct appliance_network_config *
appliance_persistent_network_config_current(
    const struct appliance_persistent_network_config *config)
{
    if (config == NULL ||
        !config->initialized ||
        config->source == APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE ||
        appliance_network_config_validate(&config->values) !=
            APPLIANCE_NETWORK_CONFIG_OK) {
        return NULL;
    }
    return &config->values;
}

enum appliance_persistent_network_config_source
appliance_persistent_network_config_source(
    const struct appliance_persistent_network_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE;
    }
    return config->source;
}

const char *appliance_persistent_network_config_source_string(
    enum appliance_persistent_network_config_source source)
{
    switch (source) {
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_NONE";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_INVALID";
    default:
        return NULL;
    }
}

const char *appliance_persistent_network_config_status_string(
    enum appliance_persistent_network_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG";
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL:
        return "APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
