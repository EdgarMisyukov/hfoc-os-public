#include "forward_config.h"

/*
 * ITEM-449: the declaration's grammar, its validation and the access rule.
 *
 * Everything here is a pure function of bytes: the installer medium, the boot
 * command line, the machine's own menu and the relay all ask this one owner
 * what the declaration means.  Nothing in this file touches a device, an
 * address space or a socket, and nothing guesses: a malformed part is refused
 * with the status that names it, and a refused declaration is never applied
 * partially.
 */

static size_t forward_config_text_length(const char *text)
{
    size_t length = 0U;

    if (text == NULL) {
        return 0U;
    }
    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static bool forward_config_address_valid(
    const uint8_t address[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    if (address == NULL) {
        return false;
    }
    /*
     * A target or a client has to name one host: the unspecified address, the
     * limited broadcast, the loopback range and the multicast range are not
     * hosts a forward rule can mean, so they are refused as addresses.
     */
    if (address[0] == 0U && address[1] == 0U &&
        address[2] == 0U && address[3] == 0U) {
        return false;
    }
    if (address[0] == 255U && address[1] == 255U &&
        address[2] == 255U && address[3] == 255U) {
        return false;
    }
    if (address[0] == 127U) {
        return false;
    }
    if ((address[0] & 0xF0U) == 224U) {
        return false;
    }
    return true;
}

static bool forward_config_address_equal(
    const uint8_t left[FORWARD_CONFIG_ADDRESS_LENGTH],
    const uint8_t right[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    size_t index;

    for (index = 0U; index < FORWARD_CONFIG_ADDRESS_LENGTH; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static enum forward_config_status forward_config_parse_decimal(
    const char *text,
    size_t text_length,
    uint32_t minimum,
    uint32_t maximum,
    uint32_t *value_out)
{
    uint32_t value = 0U;
    size_t index;

    if (text == NULL || value_out == NULL || text_length == 0U) {
        return FORWARD_CONFIG_INVALID_PORT;
    }
    for (index = 0U; index < text_length; ++index) {
        const char digit = text[index];

        if (digit < '0' || digit > '9') {
            return FORWARD_CONFIG_INVALID_PORT;
        }
        value = value * 10U + (uint32_t)(digit - '0');
        if (value > maximum) {
            return FORWARD_CONFIG_INVALID_PORT;
        }
    }
    if (value < minimum) {
        return FORWARD_CONFIG_INVALID_PORT;
    }
    *value_out = value;
    return FORWARD_CONFIG_OK;
}

static enum forward_config_status forward_config_parse_address(
    const char *text,
    size_t text_length,
    uint8_t address_out[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    size_t offset = 0U;
    size_t octet = 0U;

    if (text == NULL || address_out == NULL || text_length == 0U) {
        return FORWARD_CONFIG_INVALID_ADDRESS;
    }
    while (octet < FORWARD_CONFIG_ADDRESS_LENGTH) {
        size_t digits = 0U;
        uint32_t value = 0U;

        while (offset + digits < text_length &&
               text[offset + digits] >= '0' &&
               text[offset + digits] <= '9') {
            value = value * 10U + (uint32_t)(text[offset + digits] - '0');
            ++digits;
            if (value > 255U) {
                return FORWARD_CONFIG_INVALID_ADDRESS;
            }
        }
        if (digits == 0U || digits > 3U) {
            return FORWARD_CONFIG_INVALID_ADDRESS;
        }
        /* `010` is not `10`: a leading zero is refused, never reinterpreted. */
        if (digits > 1U && text[offset] == '0') {
            return FORWARD_CONFIG_INVALID_ADDRESS;
        }
        address_out[octet] = (uint8_t)value;
        offset += digits;
        ++octet;
        if (octet == FORWARD_CONFIG_ADDRESS_LENGTH) {
            break;
        }
        if (offset >= text_length || text[offset] != '.') {
            return FORWARD_CONFIG_INVALID_ADDRESS;
        }
        ++offset;
    }
    if (offset != text_length) {
        return FORWARD_CONFIG_INVALID_ADDRESS;
    }
    if (!forward_config_address_valid(address_out)) {
        return FORWARD_CONFIG_INVALID_ADDRESS;
    }
    return FORWARD_CONFIG_OK;
}

static enum forward_config_status forward_config_parse_clients(
    const char *text,
    size_t text_length,
    struct forward_config *config)
{
    size_t offset = 0U;

    if (text == NULL || text_length == 0U) {
        return FORWARD_CONFIG_MISSING_FIELD;
    }
    while (offset < text_length) {
        size_t end = offset;
        uint8_t address[FORWARD_CONFIG_ADDRESS_LENGTH];
        uint32_t index;

        while (end < text_length && text[end] != ',') {
            ++end;
        }
        if (config->client_count == FORWARD_CONFIG_CLIENT_MAX) {
            return FORWARD_CONFIG_TOO_MANY_CLIENTS;
        }
        if (forward_config_parse_address(
                text + offset, end - offset, address) !=
            FORWARD_CONFIG_OK) {
            return FORWARD_CONFIG_INVALID_ADDRESS;
        }
        for (index = 0U; index < config->client_count; ++index) {
            if (forward_config_address_equal(
                    config->clients[index], address)) {
                return FORWARD_CONFIG_DUPLICATE_CLIENT;
            }
        }
        {
            size_t octet;

            for (octet = 0U; octet < FORWARD_CONFIG_ADDRESS_LENGTH;
                 ++octet) {
                config->clients[config->client_count][octet] =
                    address[octet];
            }
        }
        ++config->client_count;
        offset = end;
        if (offset < text_length) {
            /* Skip the comma; the segment after it is a client of its own. */
            ++offset;
            if (offset == text_length) {
                return FORWARD_CONFIG_INVALID_ADDRESS;
            }
        }
    }
    return FORWARD_CONFIG_OK;
}

static bool forward_config_parts_empty(
    const char *target,
    size_t target_length,
    const char *target_port,
    size_t target_port_length,
    const char *listen_port,
    size_t listen_port_length,
    const char *clients,
    size_t clients_length)
{
    return (target == NULL || target_length == 0U) &&
           (target_port == NULL || target_port_length == 0U) &&
           (listen_port == NULL || listen_port_length == 0U) &&
           (clients == NULL || clients_length == 0U);
}

static bool forward_config_port_valid(uint32_t port)
{
    return port >= 1U && port <= 65535U;
}

enum forward_config_status forward_config_from_parts(
    bool declared,
    bool enabled,
    const char *target,
    size_t target_length,
    const char *target_port,
    size_t target_port_length,
    const char *listen_port,
    size_t listen_port_length,
    const char *clients,
    size_t clients_length,
    struct forward_config *config_out)
{
    enum forward_config_status status;
    uint32_t target_port_value = 0U;
    uint32_t listen_port_value = 0U;

    if (config_out == NULL) {
        return FORWARD_CONFIG_NULL;
    }
    config_out->version = FORWARD_CONFIG_VERSION;
    config_out->declared = declared;
    config_out->enabled = declared && enabled;
    config_out->target_port = 0U;
    config_out->listen_port = 0U;
    config_out->client_count = 0U;
    {
        size_t index;

        for (index = 0U; index < FORWARD_CONFIG_ADDRESS_LENGTH; ++index) {
            config_out->target[index] = 0U;
        }
        for (index = 0U; index < FORWARD_CONFIG_CLIENT_MAX; ++index) {
            size_t octet;

            for (octet = 0U; octet < FORWARD_CONFIG_ADDRESS_LENGTH;
                 ++octet) {
                config_out->clients[index][octet] = 0U;
            }
        }
    }
    if (!declared) {
        if (!forward_config_parts_empty(
                target, target_length, target_port, target_port_length,
                listen_port, listen_port_length, clients, clients_length)) {
            return FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION;
        }
        return FORWARD_CONFIG_OK;
    }
    if (!enabled) {
        if (!forward_config_parts_empty(
                target, target_length, target_port, target_port_length,
                listen_port, listen_port_length, clients, clients_length)) {
            return FORWARD_CONFIG_VALUES_WITH_DISABLED;
        }
        return FORWARD_CONFIG_OK;
    }
    if (target == NULL || target_length == 0U ||
        target_port == NULL || target_port_length == 0U ||
        listen_port == NULL || listen_port_length == 0U ||
        clients == NULL || clients_length == 0U) {
        return FORWARD_CONFIG_MISSING_FIELD;
    }
    status = forward_config_parse_address(
        target, target_length, config_out->target);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    status = forward_config_parse_decimal(
        target_port, target_port_length, 1U, 65535U, &target_port_value);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    status = forward_config_parse_decimal(
        listen_port, listen_port_length, 1U, 65535U, &listen_port_value);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    if (listen_port_value == FORWARD_CONFIG_RESERVED_LISTEN_PORT) {
        return FORWARD_CONFIG_RESERVED_PORT;
    }
    status = forward_config_parse_clients(
        clients, clients_length, config_out);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    config_out->target_port = (uint16_t)target_port_value;
    config_out->listen_port = (uint16_t)listen_port_value;
    status = forward_config_validate(config_out);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    return FORWARD_CONFIG_OK;
}

enum forward_config_status forward_config_validate(
    const struct forward_config *config)
{
    uint32_t index;

    if (config == NULL) {
        return FORWARD_CONFIG_NULL;
    }
    if (config->version != FORWARD_CONFIG_VERSION) {
        return FORWARD_CONFIG_BAD_TEXT;
    }
    if (!config->declared) {
        if (config->enabled || config->client_count != 0U ||
            config->target_port != 0U || config->listen_port != 0U) {
            return FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION;
        }
        return FORWARD_CONFIG_OK;
    }
    if (!config->enabled) {
        if (config->client_count != 0U || config->target_port != 0U ||
            config->listen_port != 0U) {
            return FORWARD_CONFIG_VALUES_WITH_DISABLED;
        }
        return FORWARD_CONFIG_OK;
    }
    if (!forward_config_address_valid(config->target)) {
        return FORWARD_CONFIG_INVALID_ADDRESS;
    }
    if (!forward_config_port_valid(config->target_port) ||
        !forward_config_port_valid(config->listen_port)) {
        return FORWARD_CONFIG_INVALID_PORT;
    }
    if (config->listen_port == FORWARD_CONFIG_RESERVED_LISTEN_PORT) {
        return FORWARD_CONFIG_RESERVED_PORT;
    }
    if (config->client_count == 0U) {
        return FORWARD_CONFIG_MISSING_FIELD;
    }
    if (config->client_count > FORWARD_CONFIG_CLIENT_MAX) {
        return FORWARD_CONFIG_TOO_MANY_CLIENTS;
    }
    for (index = 0U; index < config->client_count; ++index) {
        uint32_t other;

        if (!forward_config_address_valid(config->clients[index])) {
            return FORWARD_CONFIG_INVALID_ADDRESS;
        }
        for (other = 0U; other < index; ++other) {
            if (forward_config_address_equal(
                    config->clients[index], config->clients[other])) {
                return FORWARD_CONFIG_DUPLICATE_CLIENT;
            }
        }
    }
    return FORWARD_CONFIG_OK;
}

enum forward_config_status forward_config_admit(
    const struct forward_config *config,
    const uint8_t source[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    enum forward_config_status status;
    uint32_t index;

    if (config == NULL || source == NULL) {
        return FORWARD_CONFIG_NULL;
    }
    status = forward_config_validate(config);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    if (!config->declared) {
        return FORWARD_CONFIG_DENIED_NO_DECLARATION;
    }
    if (!config->enabled) {
        return FORWARD_CONFIG_DENIED_DISABLED;
    }
    for (index = 0U; index < config->client_count; ++index) {
        if (forward_config_address_equal(config->clients[index], source)) {
            return FORWARD_CONFIG_ADMITTED;
        }
    }
    return FORWARD_CONFIG_DENIED_NOT_ALLOWED;
}

static bool forward_config_render_append(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text,
    size_t text_length)
{
    size_t index;

    if (*length > capacity || text_length > capacity - *length) {
        return false;
    }
    for (index = 0U; index < text_length; ++index) {
        buffer[*length + index] = text[index];
    }
    *length += text_length;
    return true;
}

static bool forward_config_render_decimal(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        return forward_config_render_append(buffer, capacity, length, "0", 1U);
    }
    while (value > 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        if (!forward_config_render_append(
                buffer, capacity, length, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool forward_config_render_address(
    char *buffer,
    size_t capacity,
    size_t *length,
    const uint8_t address[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    size_t octet;

    for (octet = 0U; octet < FORWARD_CONFIG_ADDRESS_LENGTH; ++octet) {
        if (octet > 0U &&
            !forward_config_render_append(buffer, capacity, length, ".", 1U)) {
            return false;
        }
        if (!forward_config_render_decimal(
                buffer, capacity, length, (uint32_t)address[octet])) {
            return false;
        }
    }
    return true;
}

enum forward_config_status forward_config_render(
    const struct forward_config *config,
    char *buffer,
    size_t capacity,
    size_t *length_out)
{
    size_t length = 0U;
    enum forward_config_status status;

    if (config == NULL || buffer == NULL || length_out == NULL) {
        return FORWARD_CONFIG_NULL;
    }
    *length_out = 0U;
    if (capacity == 0U) {
        return FORWARD_CONFIG_BUFFER_TOO_SMALL;
    }
    buffer[0] = '\0';
    status = forward_config_validate(config);
    if (status != FORWARD_CONFIG_OK) {
        return status;
    }
    if (!config->declared || !config->enabled) {
        const char *none = "target=none listen=none clients=0";

        if (!forward_config_render_append(
                buffer, capacity - 1U, &length, none,
                forward_config_text_length(none))) {
            return FORWARD_CONFIG_BUFFER_TOO_SMALL;
        }
        buffer[length] = '\0';
        *length_out = length;
        return FORWARD_CONFIG_OK;
    }
    if (!forward_config_render_append(
            buffer, capacity - 1U, &length, "target=", 7U) ||
        !forward_config_render_address(
            buffer, capacity - 1U, &length, config->target) ||
        !forward_config_render_append(buffer, capacity - 1U, &length, ":", 1U) ||
        !forward_config_render_decimal(
            buffer, capacity - 1U, &length,
            (uint32_t)config->target_port) ||
        !forward_config_render_append(
            buffer, capacity - 1U, &length, " listen=", 8U) ||
        !forward_config_render_decimal(
            buffer, capacity - 1U, &length,
            (uint32_t)config->listen_port) ||
        !forward_config_render_append(
            buffer, capacity - 1U, &length, " clients=", 9U) ||
        !forward_config_render_decimal(
            buffer, capacity - 1U, &length, config->client_count)) {
        return FORWARD_CONFIG_BUFFER_TOO_SMALL;
    }
    buffer[length] = '\0';
    *length_out = length;
    return FORWARD_CONFIG_OK;
}

bool forward_config_enabled(const struct forward_config *config)
{
    return config != NULL &&
           forward_config_validate(config) == FORWARD_CONFIG_OK &&
           config->declared &&
           config->enabled;
}

const char *forward_config_status_string(enum forward_config_status status)
{
    switch (status) {
    case FORWARD_CONFIG_OK:
        return "OK";
    case FORWARD_CONFIG_NULL:
        return "NULL";
    case FORWARD_CONFIG_BAD_TEXT:
        return "BAD_TEXT";
    case FORWARD_CONFIG_MISSING_FIELD:
        return "MISSING_FIELD";
    case FORWARD_CONFIG_INVALID_ADDRESS:
        return "INVALID_ADDRESS";
    case FORWARD_CONFIG_INVALID_PORT:
        return "INVALID_PORT";
    case FORWARD_CONFIG_TOO_MANY_CLIENTS:
        return "TOO_MANY_CLIENTS";
    case FORWARD_CONFIG_DUPLICATE_CLIENT:
        return "DUPLICATE_CLIENT";
    case FORWARD_CONFIG_RESERVED_PORT:
        return "RESERVED_PORT";
    case FORWARD_CONFIG_VALUES_WITHOUT_DECLARATION:
        return "VALUES_WITHOUT_DECLARATION";
    case FORWARD_CONFIG_VALUES_WITH_DISABLED:
        return "VALUES_WITH_DISABLED";
    case FORWARD_CONFIG_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case FORWARD_CONFIG_ADMITTED:
        return "ADMITTED";
    case FORWARD_CONFIG_DENIED_NO_DECLARATION:
        return "DENIED_NO_DECLARATION";
    case FORWARD_CONFIG_DENIED_DISABLED:
        return "DENIED_DISABLED";
    case FORWARD_CONFIG_DENIED_NOT_ALLOWED:
        return "DENIED_NOT_ALLOWED";
    default:
        return NULL;
    }
}
