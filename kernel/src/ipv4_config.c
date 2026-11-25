#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define NETWORK_IPV4_FIELD_COUNT 5U
/* The canonical text the builder composes and the parser consumes. */
#define NETWORK_IPV4_TEXT_MAX 256U

enum network_ipv4_field {
    NETWORK_IPV4_FIELD_VERSION = 0,
    NETWORK_IPV4_FIELD_ADDRESS = 1,
    NETWORK_IPV4_FIELD_PREFIX = 2,
    NETWORK_IPV4_FIELD_GATEWAY = 3,
    NETWORK_IPV4_FIELD_DNS = 4,
};

static const char *const network_ipv4_fields[
    NETWORK_IPV4_FIELD_COUNT] = {
    "schema_version",
    "ipv4_address",
    "ipv4_prefix",
    "ipv4_gateway",
    "ipv4_dns",
};

static bool network_ipv4_key_char_valid(unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'A' &&
            value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9') ||
           value == (unsigned char)'_';
}

static bool network_ipv4_parse_decimal(
    const char *value,
    size_t value_length,
    uint32_t *parsed_out)
{
    uint64_t parsed = 0U;
    size_t index;

    if (value_length == 0U ||
        (value_length > 1U && value[0] == '0')) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        const unsigned char byte =
            (unsigned char)value[index];

        if (byte < (unsigned char)'0' ||
            byte > (unsigned char)'9') {
            return false;
        }
        parsed = parsed * 10U +
                 (uint64_t)(byte - (unsigned char)'0');
    }
    if (parsed > UINT32_MAX) {
        return false;
    }
    *parsed_out = (uint32_t)parsed;
    return true;
}

static bool network_ipv4_address_nonzero(
    const struct network_ipv4_address *address)
{
    size_t index;

    for (index = 0U;
         index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++index) {
        if (address->octets[index] != 0U) {
            return true;
        }
    }
    return false;
}

enum network_ipv4_status network_ipv4_address_parse(
    const char *value,
    size_t value_length,
    struct network_ipv4_address *address_out)
{
    struct network_ipv4_address draft;
    size_t position = 0U;
    uint32_t octet_index;
    uint32_t octet_value;

    if (value == NULL || address_out == NULL) {
        return NETWORK_IPV4_NULL;
    }
    memset(&draft, 0, sizeof(draft));
    for (octet_index = 0U;
         octet_index < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet_index) {
        const size_t octet_start = position;
        size_t octet_length = 0U;

        while (position < value_length &&
               value[position] != '.') {
            ++position;
            ++octet_length;
        }
        if (octet_length == 0U ||
            octet_length > 3U ||
            !network_ipv4_parse_decimal(
                value + octet_start, octet_length,
                &octet_value) ||
            octet_value > 255U) {
            return NETWORK_IPV4_INVALID_ADDRESS;
        }
        draft.octets[octet_index] = (uint8_t)octet_value;
        if (octet_index + 1U <
            NETWORK_IPV4_ADDRESS_OCTETS) {
            if (position >= value_length ||
                value[position] != '.') {
                return NETWORK_IPV4_INVALID_ADDRESS;
            }
            ++position;
        }
    }
    if (value == NULL || address_out == NULL ||
        position != value_length ||
        !network_ipv4_address_nonzero(&draft)) {
        return NETWORK_IPV4_INVALID_ADDRESS;
    }
    *address_out = draft;
    return NETWORK_IPV4_OK;
}

static int network_ipv4_field_index(
    const char *key,
    size_t key_length)
{
    size_t field;

    for (field = 0U; field < NETWORK_IPV4_FIELD_COUNT; ++field) {
        const size_t name_length =
            strlen(network_ipv4_fields[field]);

        if (name_length == key_length &&
            memcmp(network_ipv4_fields[field], key,
                   key_length) == 0) {
            return (int)field;
        }
    }
    return -1;
}

static enum network_ipv4_status network_ipv4_parse_line(
    const char *line,
    size_t line_length,
    struct network_ipv4_config *draft,
    bool *seen)
{
    size_t equals_index = 0U;
    bool equals_found = false;
    size_t key_length;
    size_t value_length;
    size_t index;
    int field;
    uint32_t parsed_value;

    if (line_length == 0U ||
        line_length > NETWORK_IPV4_LINE_MAX) {
        return NETWORK_IPV4_MALFORMED_FIELD;
    }
    for (index = 0U; index < line_length; ++index) {
        if (line[index] == '=') {
            if (equals_found) {
                return NETWORK_IPV4_MALFORMED_FIELD;
            }
            equals_found = true;
            equals_index = index;
        }
    }
    if (!equals_found) {
        return NETWORK_IPV4_MALFORMED_FIELD;
    }

    key_length = equals_index;
    value_length = line_length - equals_index - 1U;
    if (key_length == 0U ||
        key_length > NETWORK_IPV4_KEY_MAX ||
        value_length == 0U ||
        value_length > NETWORK_IPV4_VALUE_MAX) {
        return NETWORK_IPV4_MALFORMED_FIELD;
    }
    for (index = 0U; index < key_length; ++index) {
        if (!network_ipv4_key_char_valid(
                (unsigned char)line[index])) {
            return NETWORK_IPV4_MALFORMED_FIELD;
        }
    }

    field = network_ipv4_field_index(line, key_length);
    if (field < 0) {
        return NETWORK_IPV4_UNKNOWN_FIELD;
    }
    if (seen[field]) {
        return NETWORK_IPV4_DUPLICATE_FIELD;
    }

    if (field == NETWORK_IPV4_FIELD_VERSION ||
        field == NETWORK_IPV4_FIELD_PREFIX) {
        if (!network_ipv4_parse_decimal(
                line + equals_index + 1U, value_length,
                &parsed_value)) {
            return field == NETWORK_IPV4_FIELD_VERSION
                ? NETWORK_IPV4_BAD_VERSION
                : NETWORK_IPV4_INVALID_PREFIX;
        }
        if (field == NETWORK_IPV4_FIELD_VERSION) {
            if (parsed_value != NETWORK_IPV4_CONFIG_VERSION) {
                return NETWORK_IPV4_BAD_VERSION;
            }
            draft->schema_version = parsed_value;
        } else {
            if (parsed_value > NETWORK_IPV4_PREFIX_MAX) {
                return NETWORK_IPV4_INVALID_PREFIX;
            }
            draft->prefix_length = (uint8_t)parsed_value;
        }
    } else {
        struct network_ipv4_address address;

        if (network_ipv4_address_parse(
                line + equals_index + 1U, value_length,
                &address) != NETWORK_IPV4_OK) {
            if (field == NETWORK_IPV4_FIELD_ADDRESS) {
                return NETWORK_IPV4_INVALID_ADDRESS;
            }
            if (field == NETWORK_IPV4_FIELD_GATEWAY) {
                return NETWORK_IPV4_INVALID_ROUTE;
            }
            return NETWORK_IPV4_INVALID_DNS;
        }
        if (field == NETWORK_IPV4_FIELD_ADDRESS) {
            draft->address = address;
        } else if (field == NETWORK_IPV4_FIELD_GATEWAY) {
            draft->gateway = address;
        } else {
            draft->dns = address;
        }
    }

    seen[field] = true;
    return NETWORK_IPV4_OK;
}

static uint32_t network_ipv4_address_to_u32(
    const struct network_ipv4_address *address)
{
    return ((uint32_t)address->octets[0] << 24U) |
           ((uint32_t)address->octets[1] << 16U) |
           ((uint32_t)address->octets[2] << 8U) |
           (uint32_t)address->octets[3];
}

static uint32_t network_ipv4_prefix_mask(uint8_t prefix_length)
{
    if (prefix_length == 0U) {
        return 0U;
    }
    return ~0U << (32U - (uint32_t)prefix_length);
}

static bool network_ipv4_route_valid(
    const struct network_ipv4_config *config)
{
    const uint32_t address = network_ipv4_address_to_u32(
        &config->address);
    const uint32_t gateway = network_ipv4_address_to_u32(
        &config->gateway);
    const uint32_t mask = network_ipv4_prefix_mask(
        config->prefix_length);
    const uint32_t network = address & mask;
    const uint32_t broadcast = network | ~mask;

    if ((gateway & mask) != network ||
        gateway == network ||
        gateway == broadcast ||
        gateway == address) {
        return false;
    }
    return true;
}

enum network_ipv4_status network_ipv4_config_validate(
    const struct network_ipv4_config *config)
{
    if (config == NULL) {
        return NETWORK_IPV4_NULL;
    }
    if (config->schema_version != NETWORK_IPV4_CONFIG_VERSION) {
        return NETWORK_IPV4_BAD_VERSION;
    }
    if (config->prefix_length > NETWORK_IPV4_PREFIX_MAX) {
        return NETWORK_IPV4_INVALID_PREFIX;
    }
    if (!network_ipv4_address_nonzero(&config->address)) {
        return NETWORK_IPV4_INVALID_ADDRESS;
    }
    if (!network_ipv4_address_nonzero(&config->gateway)) {
        return NETWORK_IPV4_INVALID_ROUTE;
    }
    if (!network_ipv4_address_nonzero(&config->dns)) {
        return NETWORK_IPV4_INVALID_DNS;
    }
    if (!network_ipv4_route_valid(config)) {
        return NETWORK_IPV4_INVALID_ROUTE;
    }
    return NETWORK_IPV4_OK;
}

enum network_ipv4_status network_ipv4_config_parse(
    const char *input,
    size_t input_length,
    struct network_ipv4_config *config_out)
{
    struct network_ipv4_config draft;
    bool seen[NETWORK_IPV4_FIELD_COUNT] = {false};
    size_t position = 0U;
    size_t field;

    if (input == NULL || config_out == NULL) {
        return NETWORK_IPV4_NULL;
    }
    if (input_length > NETWORK_IPV4_INPUT_MAX) {
        return NETWORK_IPV4_TOO_LONG;
    }
    memset(&draft, 0, sizeof(draft));

    while (position < input_length) {
        const size_t line_start = position;
        size_t line_length;
        enum network_ipv4_status status;

        while (position < input_length &&
               input[position] != '\n') {
            ++position;
        }
        line_length = position - line_start;
        status = network_ipv4_parse_line(
            input + line_start, line_length, &draft, seen);
        if (status != NETWORK_IPV4_OK) {
            return status;
        }
        if (position < input_length) {
            ++position;
        }
    }

    for (field = 0U; field < NETWORK_IPV4_FIELD_COUNT; ++field) {
        if (!seen[field]) {
            return NETWORK_IPV4_MISSING_FIELD;
        }
    }
    if (!network_ipv4_route_valid(&draft)) {
        return NETWORK_IPV4_INVALID_ROUTE;
    }

    *config_out = draft;
    return NETWORK_IPV4_OK;
}

enum network_ipv4_status network_ipv4_config_from_parts(
    const char *address_with_prefix,
    size_t address_length,
    const char *gateway,
    size_t gateway_length,
    const char *dns,
    size_t dns_length,
    struct network_ipv4_config *config_out)
{
    char text[NETWORK_IPV4_TEXT_MAX];
    size_t length = 0U;
    size_t slash_index = 0U;
    size_t index;
    bool slash_found = false;
    enum network_ipv4_status status;

    if (address_with_prefix == NULL || gateway == NULL || dns == NULL ||
        config_out == NULL) {
        return NETWORK_IPV4_NULL;
    }
    memset(config_out, 0, sizeof(*config_out));
    for (index = 0U; index < address_length; ++index) {
        if (address_with_prefix[index] == '/') {
            if (slash_found) {
                return NETWORK_IPV4_MALFORMED_FIELD;
            }
            slash_found = true;
            slash_index = index;
        }
    }
    if (!slash_found || slash_index == 0U ||
        slash_index + 1U >= address_length) {
        return NETWORK_IPV4_MALFORMED_FIELD;
    }
#define NETWORK_IPV4_TEXT_APPEND_VALUE(prefix, value, value_length) \
    do { \
        const char *prefix_text = (prefix); \
        size_t prefix_index; \
        for (prefix_index = 0U; prefix_text[prefix_index] != '\0'; \
             ++prefix_index) { \
            if (length + 1U >= sizeof(text)) { \
                return NETWORK_IPV4_TOO_LONG; \
            } \
            text[length++] = prefix_text[prefix_index]; \
        } \
        for (index = 0U; index < (value_length); ++index) { \
            if (length + 1U >= sizeof(text)) { \
                return NETWORK_IPV4_TOO_LONG; \
            } \
            text[length++] = (value)[index]; \
        } \
        if (length + 1U >= sizeof(text)) { \
            return NETWORK_IPV4_TOO_LONG; \
        } \
        text[length++] = '\n'; \
    } while (0)

    NETWORK_IPV4_TEXT_APPEND_VALUE("schema_version=", "1", 1U);
    NETWORK_IPV4_TEXT_APPEND_VALUE(
        "ipv4_address=", address_with_prefix, slash_index);
    NETWORK_IPV4_TEXT_APPEND_VALUE(
        "ipv4_prefix=", address_with_prefix + slash_index + 1U,
        address_length - slash_index - 1U);
    NETWORK_IPV4_TEXT_APPEND_VALUE("ipv4_gateway=", gateway, gateway_length);
    NETWORK_IPV4_TEXT_APPEND_VALUE("ipv4_dns=", dns, dns_length);
#undef NETWORK_IPV4_TEXT_APPEND_VALUE

    status = network_ipv4_config_parse(text, length, config_out);
    if (status != NETWORK_IPV4_OK) {
        memset(config_out, 0, sizeof(*config_out));
        return status;
    }
    status = network_ipv4_config_validate(config_out);
    if (status != NETWORK_IPV4_OK) {
        memset(config_out, 0, sizeof(*config_out));
        return status;
    }
    return NETWORK_IPV4_OK;
}

const char *network_ipv4_status_string(
    enum network_ipv4_status status)
{
    switch (status) {
    case NETWORK_IPV4_OK:
        return "NETWORK_IPV4_OK";
    case NETWORK_IPV4_NULL:
        return "NETWORK_IPV4_NULL";
    case NETWORK_IPV4_TOO_LONG:
        return "NETWORK_IPV4_TOO_LONG";
    case NETWORK_IPV4_MISSING_FIELD:
        return "NETWORK_IPV4_MISSING_FIELD";
    case NETWORK_IPV4_DUPLICATE_FIELD:
        return "NETWORK_IPV4_DUPLICATE_FIELD";
    case NETWORK_IPV4_UNKNOWN_FIELD:
        return "NETWORK_IPV4_UNKNOWN_FIELD";
    case NETWORK_IPV4_MALFORMED_FIELD:
        return "NETWORK_IPV4_MALFORMED_FIELD";
    case NETWORK_IPV4_BAD_VERSION:
        return "NETWORK_IPV4_BAD_VERSION";
    case NETWORK_IPV4_INVALID_ADDRESS:
        return "NETWORK_IPV4_INVALID_ADDRESS";
    case NETWORK_IPV4_INVALID_PREFIX:
        return "NETWORK_IPV4_INVALID_PREFIX";
    case NETWORK_IPV4_INVALID_ROUTE:
        return "NETWORK_IPV4_INVALID_ROUTE";
    case NETWORK_IPV4_INVALID_DNS:
        return "NETWORK_IPV4_INVALID_DNS";
    default:
        return NULL;
    }
}
