#include "appliance_network_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_network_config_zero_bytes(void *destination,
                                                size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_network_config_copy_bytes(void *destination,
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

static bool appliance_network_config_interface_char_allowed(
    unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9') ||
           value == (unsigned char)'-' ||
           value == (unsigned char)'_';
}

static bool appliance_network_config_interface_alnum(unsigned char value)
{
    return (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') ||
           (value >= (unsigned char)'0' &&
            value <= (unsigned char)'9');
}

static bool appliance_network_config_interface_valid(
    const char *interface,
    size_t interface_length)
{
    size_t index;

    if (interface == NULL ||
        interface_length == 0U ||
        interface_length > APPLIANCE_NETWORK_CONFIG_INTERFACE_MAX ||
        !appliance_network_config_interface_alnum(
            (unsigned char)interface[0]) ||
        !appliance_network_config_interface_alnum(
            (unsigned char)interface[interface_length - 1U])) {
        return false;
    }
    for (index = 0U; index < interface_length; ++index) {
        if (!appliance_network_config_interface_char_allowed(
                (unsigned char)interface[index])) {
            return false;
        }
    }
    return true;
}

static bool appliance_network_config_static_zero(
    const struct network_ipv4_config *config)
{
    const unsigned char *bytes =
        (const unsigned char *)config;
    size_t index;

    for (index = 0U; index < sizeof(*config); ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static void appliance_network_config_clear(
    struct appliance_network_config *config)
{
    appliance_network_config_zero_bytes(
        config, sizeof(*config));
}

static enum appliance_network_config_status
appliance_network_config_publish_common(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length,
    enum appliance_network_config_mode mode,
    const struct network_ipv4_config *static_config)
{
    if (config == NULL || interface == NULL) {
        return APPLIANCE_NETWORK_CONFIG_NULL;
    }
    if (!appliance_network_config_interface_valid(
            interface, interface_length)) {
        return APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE;
    }
    if (mode != APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
        mode != APPLIANCE_NETWORK_CONFIG_MODE_DHCP &&
        mode != APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE) {
        return APPLIANCE_NETWORK_CONFIG_INVALID_MODE;
    }
    if (mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        if (static_config == NULL) {
            return APPLIANCE_NETWORK_CONFIG_NULL;
        }
        if (network_ipv4_config_validate(static_config) !=
            NETWORK_IPV4_OK) {
            return APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG;
        }
    } else if (static_config != NULL) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }

    appliance_network_config_clear(config);
    config->version = APPLIANCE_NETWORK_CONFIG_VERSION;
    appliance_network_config_copy_bytes(
        config->interface, interface, interface_length);
    config->interface[interface_length] = '\0';
    config->interface_length = (uint8_t)interface_length;
    config->mode = mode;
    if (mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        config->static_config = *static_config;
        config->static_present = true;
    }
    config->initialized = true;
    return APPLIANCE_NETWORK_CONFIG_OK;
}

enum appliance_network_config_status
appliance_network_config_publish_default(
    struct appliance_network_config *config)
{
    static const char default_interface[] =
        APPLIANCE_NETWORK_CONFIG_DEFAULT_INTERFACE;

    return appliance_network_config_publish_common(
        config,
        default_interface,
        sizeof(default_interface) - 1U,
        APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
        NULL);
}

enum appliance_network_config_status
appliance_network_config_publish_dhcp(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length)
{
    return appliance_network_config_publish_common(
        config, interface, interface_length,
        APPLIANCE_NETWORK_CONFIG_MODE_DHCP, NULL);
}

enum appliance_network_config_status
appliance_network_config_publish_static(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length,
    const struct network_ipv4_config *static_config)
{
    return appliance_network_config_publish_common(
        config, interface, interface_length,
        APPLIANCE_NETWORK_CONFIG_MODE_STATIC, static_config);
}

enum appliance_network_config_status
appliance_network_config_publish_offline(
    struct appliance_network_config *config,
    const char *interface,
    size_t interface_length)
{
    return appliance_network_config_publish_common(
        config, interface, interface_length,
        APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE, NULL);
}

enum appliance_network_config_status
appliance_network_config_validate(
    const struct appliance_network_config *config)
{
    if (config == NULL) {
        return APPLIANCE_NETWORK_CONFIG_NULL;
    }
    if (!config->initialized) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }
    if (config->version != APPLIANCE_NETWORK_CONFIG_VERSION) {
        return APPLIANCE_NETWORK_CONFIG_BAD_VERSION;
    }
    if (!appliance_network_config_interface_valid(
            config->interface, config->interface_length)) {
        return APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE;
    }
    switch (config->mode) {
    case APPLIANCE_NETWORK_CONFIG_MODE_STATIC:
        if (!config->static_present) {
            return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
        }
        if (network_ipv4_config_validate(
                &config->static_config) != NETWORK_IPV4_OK) {
            return APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG;
        }
        return APPLIANCE_NETWORK_CONFIG_OK;
    case APPLIANCE_NETWORK_CONFIG_MODE_DHCP:
    case APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE:
        if (config->static_present ||
            !appliance_network_config_static_zero(
                &config->static_config)) {
            return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
        }
        return APPLIANCE_NETWORK_CONFIG_OK;
    default:
        return APPLIANCE_NETWORK_CONFIG_INVALID_MODE;
    }
}

static bool appliance_network_config_append_bytes(
    char *buffer,
    size_t capacity,
    size_t *position,
    const void *source,
    size_t length)
{
    const char *text = source;
    size_t index;

    if (*position > capacity ||
        length > capacity - *position) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        buffer[*position + index] = text[index];
    }
    *position += length;
    return true;
}

static bool appliance_network_config_append_string(
    char *buffer,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_network_config_append_bytes(
        buffer, capacity, position, text, length);
}

static bool appliance_network_config_append_u8(
    char *buffer,
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
        if (!appliance_network_config_append_bytes(
                buffer, capacity, position,
                digits + digit_count - 1U, 1U)) {
            return false;
        }
        --digit_count;
    }
    return true;
}

static bool appliance_network_config_append_address(
    char *buffer,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t octet;

    for (octet = 0U;
         octet < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet) {
        if (!appliance_network_config_append_u8(
                buffer, capacity, position,
                address->octets[octet]) ||
            (octet + 1U < NETWORK_IPV4_ADDRESS_OCTETS &&
             !appliance_network_config_append_bytes(
                 buffer, capacity, position, ".", 1U))) {
            return false;
        }
    }
    return true;
}

enum appliance_network_config_status
appliance_network_config_render(
    const struct appliance_network_config *config,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char buffer[APPLIANCE_NETWORK_CONFIG_OUTPUT_MAX];
    const struct network_ipv4_config *static_config;
    size_t position = 0U;

    if (config == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_NETWORK_CONFIG_NULL;
    }
    if (appliance_network_config_validate(config) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }

    if (!appliance_network_config_append_string(
            buffer, sizeof(buffer), &position,
            "network v=1 interface=") ||
        !appliance_network_config_append_bytes(
            buffer, sizeof(buffer), &position,
            config->interface, config->interface_length) ||
        !appliance_network_config_append_string(
            buffer, sizeof(buffer), &position, " mode=") ||
        !appliance_network_config_append_string(
            buffer, sizeof(buffer), &position,
            appliance_network_config_mode_string(config->mode))) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }

    if (config->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        static_config = &config->static_config;
        if (!appliance_network_config_append_string(
                buffer, sizeof(buffer), &position,
                " ipv4_address=") ||
            !appliance_network_config_append_address(
                buffer, sizeof(buffer), &position,
                &static_config->address) ||
            !appliance_network_config_append_string(
                buffer, sizeof(buffer), &position,
                " ipv4_prefix=") ||
            !appliance_network_config_append_u8(
                buffer, sizeof(buffer), &position,
                static_config->prefix_length) ||
            !appliance_network_config_append_string(
                buffer, sizeof(buffer), &position,
                " ipv4_gateway=") ||
            !appliance_network_config_append_address(
                buffer, sizeof(buffer), &position,
                &static_config->gateway) ||
            !appliance_network_config_append_string(
                buffer, sizeof(buffer), &position,
                " ipv4_dns=") ||
            !appliance_network_config_append_address(
                buffer, sizeof(buffer), &position,
                &static_config->dns)) {
            return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
        }
    }
    if (!appliance_network_config_append_bytes(
            buffer, sizeof(buffer), &position, "\n", 1U)) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }
    if (capacity < position + 1U) {
        return APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL;
    }

    appliance_network_config_copy_bytes(output, buffer, position);
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_NETWORK_CONFIG_OK;
}

enum appliance_network_config_status
appliance_network_config_interface(
    const struct appliance_network_config *config,
    char *interface,
    size_t capacity,
    size_t *interface_length)
{
    if (config == NULL || interface == NULL ||
        interface_length == NULL) {
        return APPLIANCE_NETWORK_CONFIG_NULL;
    }
    if (appliance_network_config_validate(config) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_CONFIG_BAD_STATE;
    }
    if (capacity < (size_t)config->interface_length + 1U) {
        return APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL;
    }
    appliance_network_config_copy_bytes(
        interface, config->interface,
        config->interface_length);
    interface[config->interface_length] = '\0';
    *interface_length = config->interface_length;
    return APPLIANCE_NETWORK_CONFIG_OK;
}

enum appliance_network_config_mode
appliance_network_config_mode(
    const struct appliance_network_config *config)
{
    if (config == NULL || !config->initialized) {
        return APPLIANCE_NETWORK_CONFIG_MODE_NONE;
    }
    return config->mode;
}

const struct network_ipv4_config *
appliance_network_config_static_config(
    const struct appliance_network_config *config)
{
    if (config == NULL ||
        appliance_network_config_validate(config) !=
            APPLIANCE_NETWORK_CONFIG_OK ||
        config->mode != APPLIANCE_NETWORK_CONFIG_MODE_STATIC ||
        !config->static_present) {
        return NULL;
    }
    return &config->static_config;
}

const char *appliance_network_config_mode_string(
    enum appliance_network_config_mode mode)
{
    switch (mode) {
    case APPLIANCE_NETWORK_CONFIG_MODE_NONE:
        return "NONE";
    case APPLIANCE_NETWORK_CONFIG_MODE_STATIC:
        return "STATIC";
    case APPLIANCE_NETWORK_CONFIG_MODE_DHCP:
        return "DHCP";
    case APPLIANCE_NETWORK_CONFIG_MODE_OFFLINE:
        return "OFFLINE";
    default:
        return NULL;
    }
}

const char *appliance_network_config_status_string(
    enum appliance_network_config_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_CONFIG_OK:
        return "APPLIANCE_NETWORK_CONFIG_OK";
    case APPLIANCE_NETWORK_CONFIG_NULL:
        return "APPLIANCE_NETWORK_CONFIG_NULL";
    case APPLIANCE_NETWORK_CONFIG_BAD_STATE:
        return "APPLIANCE_NETWORK_CONFIG_BAD_STATE";
    case APPLIANCE_NETWORK_CONFIG_BAD_VERSION:
        return "APPLIANCE_NETWORK_CONFIG_BAD_VERSION";
    case APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE:
        return "APPLIANCE_NETWORK_CONFIG_INVALID_INTERFACE";
    case APPLIANCE_NETWORK_CONFIG_INVALID_MODE:
        return "APPLIANCE_NETWORK_CONFIG_INVALID_MODE";
    case APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG:
        return "APPLIANCE_NETWORK_CONFIG_INVALID_STATIC_CONFIG";
    case APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL:
        return "APPLIANCE_NETWORK_CONFIG_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
