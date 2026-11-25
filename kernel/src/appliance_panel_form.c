/*
 * ITEM-515: the panel's mutation forms, read by the serving path and by the
 * host proof that renders the page and derives the body from the drawn
 * controls.  See `appliance_panel_form.h` for the contract.
 */
#include "appliance_panel_form.h"

#include <string.h>

bool appliance_panel_form_field(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    const uint8_t **value_out,
    uint32_t *value_length_out)
{
    size_t name_length;
    uint32_t index = 0U;
    bool found = false;

    if (body == NULL || name == NULL || value_out == NULL ||
        value_length_out == NULL) {
        return false;
    }
    name_length = strlen(name);
    while (index < body_length) {
        uint32_t field_end = index;
        uint32_t separator = index;
        const uint8_t *value;
        uint32_t value_length;

        while (field_end < body_length &&
               body[field_end] != (uint8_t)'&') {
            ++field_end;
        }
        while (separator < field_end &&
               body[separator] != (uint8_t)'=') {
            ++separator;
        }
        if (separator >= field_end) {
            index = field_end + 1U;
            continue;
        }
        value = body + separator + 1U;
        value_length = field_end - separator - 1U;
        if ((size_t)(separator - index) == name_length &&
            memcmp(body + index, name, name_length) == 0 &&
            !found) {
            *value_out = value;
            *value_length_out = value_length;
            found = true;
        }
        index = field_end + 1U;
    }
    return found;
}

bool appliance_panel_form_flag(
    const uint8_t *body,
    uint32_t body_length,
    const char *name)
{
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;

    return appliance_panel_form_field(
               body, body_length, name, &value, &value_length) &&
           value_length == 1U && value[0] == (uint8_t)'1';
}

static bool appliance_panel_form_hex(
    uint8_t character,
    uint8_t *value_out)
{
    if (value_out == NULL) {
        return false;
    }
    if (character >= (uint8_t)'0' && character <= (uint8_t)'9') {
        *value_out = (uint8_t)(character - (uint8_t)'0');
        return true;
    }
    if (character >= (uint8_t)'a' && character <= (uint8_t)'f') {
        *value_out = (uint8_t)(character - (uint8_t)'a' + 10U);
        return true;
    }
    if (character >= (uint8_t)'A' && character <= (uint8_t)'F') {
        *value_out = (uint8_t)(character - (uint8_t)'A' + 10U);
        return true;
    }
    return false;
}

bool appliance_panel_form_decode(
    const uint8_t *value,
    uint32_t value_length,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length)
{
    uint32_t index;
    uint32_t written = 0U;

    if (value == NULL || output == NULL || output_length == NULL ||
        output_capacity == 0U) {
        return false;
    }
    for (index = 0U; index < value_length; ++index) {
        uint8_t character = value[index];

        if (character == (uint8_t)'+') {
            character = (uint8_t)' ';
        } else if (character == (uint8_t)'%') {
            uint8_t high = 0U;
            uint8_t low = 0U;

            if (index + 2U >= value_length ||
                !appliance_panel_form_hex(value[index + 1U], &high) ||
                !appliance_panel_form_hex(value[index + 2U], &low)) {
                return false;
            }
            character = (uint8_t)((high << 4U) | low);
            index += 2U;
        }
        if (written + 1U >= output_capacity) {
            return false;
        }
        output[written] = character;
        ++written;
    }
    output[written] = '\0';
    *output_length = written;
    return true;
}

bool appliance_panel_form_text(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *output_length)
{
    const uint8_t *value = NULL;
    uint32_t value_length = 0U;

    if (!appliance_panel_form_field(
            body, body_length, name, &value, &value_length)) {
        return false;
    }
    return appliance_panel_form_decode(
        value, value_length, output, output_capacity, output_length);
}

const char *appliance_panel_form_status_string(
    enum appliance_panel_form_status status)
{
    switch (status) {
    case APPLIANCE_PANEL_FORM_OK:
        return "OK";
    case APPLIANCE_PANEL_FORM_EMPTY_BODY:
        return "NETWORK_EMPTY_BODY";
    case APPLIANCE_PANEL_FORM_MODE_MISSING:
        return "NETWORK_MODE_MISSING";
    case APPLIANCE_PANEL_FORM_MODE_UNKNOWN:
        return "NETWORK_MODE_UNKNOWN";
    case APPLIANCE_PANEL_FORM_EXTRA_FIELD:
        return "NETWORK_EXTRA_FIELD";
    case APPLIANCE_PANEL_FORM_ADDRESS_MISSING:
        return "NETWORK_NO_ADDRESS";
    case APPLIANCE_PANEL_FORM_PREFIX_MISSING:
        return "NETWORK_NO_PREFIX";
    case APPLIANCE_PANEL_FORM_GATEWAY_MISSING:
        return "NETWORK_NO_GATEWAY";
    case APPLIANCE_PANEL_FORM_DNS_MISSING:
        return "NETWORK_NO_DNS";
    case APPLIANCE_PANEL_FORM_ADDRESS_INVALID:
        return "NETWORK_ADDRESS_INVALID";
    case APPLIANCE_PANEL_FORM_PREFIX_INVALID:
        return "NETWORK_PREFIX_INVALID";
    case APPLIANCE_PANEL_FORM_GATEWAY_INVALID:
        return "NETWORK_GATEWAY_INVALID";
    case APPLIANCE_PANEL_FORM_DNS_INVALID:
        return "NETWORK_DNS_INVALID";
    case APPLIANCE_PANEL_FORM_VALUE_INVALID:
        return "NETWORK_VALUE_INVALID";
    case APPLIANCE_PANEL_FORM_FIELD_MISSING:
        return "NETWORK_FIELD_MISSING";
    case APPLIANCE_PANEL_FORM_FIELD_DECODE:
        return "NETWORK_FIELD_DECODE";
    case APPLIANCE_PANEL_FORM_FIELD_TOO_LARGE:
        return "NETWORK_FIELD_TOO_LARGE";
    case APPLIANCE_PANEL_FORM_ACTION_UNKNOWN:
        return "NETWORK_ACTION_UNKNOWN";
    case APPLIANCE_PANEL_FORM_HOSTNAME_INVALID:
        return "NETWORK_HOSTNAME_INVALID";
    case APPLIANCE_PANEL_FORM_LANGUAGE_INVALID:
        return "NETWORK_LANGUAGE_INVALID";
    default:
        return "NETWORK_FIELD_MISSING";
    }
}

/*
 * The System page's field reader.  It keeps the stage's original shape: the
 * body is the one `name=value` pair and nothing else, so a body that carries a
 * second field, a second `=` or an empty value is not the drawn body.
 */
static bool appliance_panel_form_system_field(
    const uint8_t *body,
    uint32_t body_length,
    const char *name,
    const uint8_t **value_out,
    uint32_t *value_length_out)
{
    size_t name_length;

    if (body == NULL || name == NULL || value_out == NULL ||
        value_length_out == NULL) {
        return false;
    }
    name_length = strlen(name);
    if (name_length == 0U || body_length <= (uint32_t)name_length) {
        return false;
    }
    for (size_t index = 0U; index < name_length; ++index) {
        if (body[index] != (uint8_t)name[index]) {
            return false;
        }
    }
    if (body[name_length] != (uint8_t)'=') {
        return false;
    }
    for (uint32_t index = (uint32_t)name_length + 1U; index < body_length;
         ++index) {
        if (body[index] == (uint8_t)'&' || body[index] == (uint8_t)'=') {
            return false;
        }
    }
    *value_out = body + name_length + 1U;
    *value_length_out = body_length - (uint32_t)name_length - 1U;
    return *value_length_out != 0U;
}

enum appliance_panel_form_status appliance_panel_form_system_parse(
    const uint8_t *body,
    uint32_t body_length,
    enum appliance_panel_form_system_field *field_out,
    const uint8_t **value_out,
    uint32_t *value_length_out)
{
    if (field_out == NULL || value_out == NULL || value_length_out == NULL) {
        return APPLIANCE_PANEL_FORM_FIELD_MISSING;
    }
    *field_out = APPLIANCE_PANEL_FORM_SYSTEM_NONE;
    *value_out = NULL;
    *value_length_out = 0U;
    if (body == NULL || body_length == 0U) {
        return APPLIANCE_PANEL_FORM_EMPTY_BODY;
    }
    if (appliance_panel_form_system_field(
            body, body_length, "hostname", value_out, value_length_out)) {
        *field_out = APPLIANCE_PANEL_FORM_SYSTEM_HOSTNAME;
        return APPLIANCE_PANEL_FORM_OK;
    }
    if (appliance_panel_form_system_field(
            body, body_length, "language", value_out, value_length_out)) {
        *field_out = APPLIANCE_PANEL_FORM_SYSTEM_LANGUAGE;
        return APPLIANCE_PANEL_FORM_OK;
    }
    return APPLIANCE_PANEL_FORM_FIELD_MISSING;
}

enum appliance_panel_form_status appliance_panel_form_action_parse(
    const uint8_t *body,
    uint32_t body_length,
    uint8_t *action_out,
    uint32_t action_capacity,
    uint32_t *action_length_out)
{
    if (body == NULL || body_length == 0U) {
        return APPLIANCE_PANEL_FORM_EMPTY_BODY;
    }
    if (!appliance_panel_form_text(
            body, body_length, "action", action_out, action_capacity,
            action_length_out)) {
        return APPLIANCE_PANEL_FORM_FIELD_DECODE;
    }
    if (*action_length_out == 0U) {
        return APPLIANCE_PANEL_FORM_FIELD_MISSING;
    }
    return APPLIANCE_PANEL_FORM_OK;
}

/*
 * The Network page's stage form.  `mode` is the one required field; the DHCP
 * branch treats the four empty inputs a browser always submits as absent
 * values, and refuses a non-empty one as the stray field it is; the static
 * branch requires all four and names each missing or malformed value.
 */
enum appliance_panel_form_status appliance_panel_form_network_parse(
    const uint8_t *body,
    uint32_t body_length,
    struct appliance_panel_form_network *parsed_out)
{
    const uint8_t *mode = NULL;
    const uint8_t *address = NULL;
    const uint8_t *prefix = NULL;
    const uint8_t *gateway = NULL;
    const uint8_t *dns = NULL;
    uint32_t mode_length = 0U;
    uint32_t address_length = 0U;
    uint32_t prefix_length = 0U;
    uint32_t gateway_length = 0U;
    uint32_t dns_length = 0U;
    bool address_present;
    bool prefix_present;
    bool gateway_present;
    bool dns_present;
    char with_prefix[NETWORK_IPV4_LINE_MAX];
    enum network_ipv4_status network_status;

    if (parsed_out == NULL) {
        return APPLIANCE_PANEL_FORM_EMPTY_BODY;
    }
    memset(parsed_out, 0, sizeof(*parsed_out));
    if (body == NULL || body_length == 0U) {
        return APPLIANCE_PANEL_FORM_EMPTY_BODY;
    }
    if (!appliance_panel_form_field(
            body, body_length, "mode", &mode, &mode_length) ||
        mode_length == 0U) {
        return APPLIANCE_PANEL_FORM_MODE_MISSING;
    }
    address_present = appliance_panel_form_field(
        body, body_length, "address", &address, &address_length);
    prefix_present = appliance_panel_form_field(
        body, body_length, "prefix", &prefix, &prefix_length);
    gateway_present = appliance_panel_form_field(
        body, body_length, "gateway", &gateway, &gateway_length);
    dns_present = appliance_panel_form_field(
        body, body_length, "dns", &dns, &dns_length);
    if (mode_length == 4U && memcmp(mode, "dhcp", 4U) == 0) {
        /*
         * ITEM-515: the browser submits every drawn control, so the four
         * optional inputs arrive present and empty beside `mode=dhcp`.  An
         * empty value is the absence of a value; only a non-empty one is a
         * request that meant static and lost its mode.
         */
        if ((address_present && address_length != 0U) ||
            (prefix_present && prefix_length != 0U) ||
            (gateway_present && gateway_length != 0U) ||
            (dns_present && dns_length != 0U)) {
            return APPLIANCE_PANEL_FORM_EXTRA_FIELD;
        }
        parsed_out->mode = APPLIANCE_NETWORK_CONFIG_MODE_DHCP;
        return APPLIANCE_PANEL_FORM_OK;
    }
    if (mode_length != 6U || memcmp(mode, "static", 6U) != 0) {
        return APPLIANCE_PANEL_FORM_MODE_UNKNOWN;
    }
    if (!address_present || address_length == 0U) {
        return APPLIANCE_PANEL_FORM_ADDRESS_MISSING;
    }
    if (!prefix_present || prefix_length == 0U) {
        return APPLIANCE_PANEL_FORM_PREFIX_MISSING;
    }
    if (!gateway_present || gateway_length == 0U) {
        return APPLIANCE_PANEL_FORM_GATEWAY_MISSING;
    }
    if (!dns_present || dns_length == 0U) {
        return APPLIANCE_PANEL_FORM_DNS_MISSING;
    }
    /*
     * The model's builder owns the slash form; the panel's two fields are
     * joined here, so the browser never has to encode a `/` and the one
     * parser still validates the address and the prefix.
     */
    if ((size_t)address_length + 1U + (size_t)prefix_length + 1U >
        sizeof(with_prefix)) {
        return APPLIANCE_PANEL_FORM_VALUE_INVALID;
    }
    memcpy(with_prefix, address, address_length);
    with_prefix[address_length] = '/';
    memcpy(with_prefix + address_length + 1U, prefix, prefix_length);
    network_status = network_ipv4_config_from_parts(
        with_prefix, (size_t)address_length + 1U + (size_t)prefix_length,
        (const char *)gateway, gateway_length, (const char *)dns, dns_length,
        &parsed_out->static_config);
    if (network_status != NETWORK_IPV4_OK) {
        memset(parsed_out, 0, sizeof(*parsed_out));
        switch (network_status) {
        case NETWORK_IPV4_INVALID_ADDRESS:
            return APPLIANCE_PANEL_FORM_ADDRESS_INVALID;
        case NETWORK_IPV4_INVALID_PREFIX:
            return APPLIANCE_PANEL_FORM_PREFIX_INVALID;
        case NETWORK_IPV4_INVALID_ROUTE:
            return APPLIANCE_PANEL_FORM_GATEWAY_INVALID;
        case NETWORK_IPV4_INVALID_DNS:
            return APPLIANCE_PANEL_FORM_DNS_INVALID;
        default:
            return APPLIANCE_PANEL_FORM_VALUE_INVALID;
        }
    }
    if (network_ipv4_config_validate(&parsed_out->static_config) !=
        NETWORK_IPV4_OK) {
        memset(parsed_out, 0, sizeof(*parsed_out));
        return APPLIANCE_PANEL_FORM_VALUE_INVALID;
    }
    parsed_out->mode = APPLIANCE_NETWORK_CONFIG_MODE_STATIC;
    return APPLIANCE_PANEL_FORM_OK;
}
