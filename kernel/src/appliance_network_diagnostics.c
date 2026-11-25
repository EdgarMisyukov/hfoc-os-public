#include "appliance_network_diagnostics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_network_diagnostics_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void appliance_network_diagnostics_copy_bytes(
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

static bool appliance_network_diagnostics_equal_bytes(
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

static bool appliance_network_diagnostics_reachability_valid(
    enum appliance_network_reachability reachability)
{
    return reachability == APPLIANCE_NETWORK_REACHABILITY_UNKNOWN ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_REACHABLE ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE;
}

static const char *appliance_network_diagnostics_link_string(
    enum network_link_state state)
{
    switch (state) {
    case NETWORK_LINK_DOWN:
        return "DOWN";
    case NETWORK_LINK_INITIALIZING:
        return "INITIALIZING";
    case NETWORK_LINK_UP:
        return "UP";
    case NETWORK_LINK_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

static const char *appliance_network_diagnostics_result_string(
    enum health_check_result_state result)
{
    switch (result) {
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return "UNKNOWN";
    case HEALTH_CHECK_RESULT_PASS:
        return "PASS";
    case HEALTH_CHECK_RESULT_WARN:
        return "WARN";
    case HEALTH_CHECK_RESULT_FAIL:
        return "FAIL";
    default:
        return NULL;
    }
}

static enum appliance_network_diagnostics_status
appliance_network_diagnostics_evaluate(
    struct appliance_network_diagnostics *diagnostics)
{
    enum network_link_state link = diagnostics->link_state;

    if (link == NETWORK_LINK_DOWN || link == NETWORK_LINK_FAILED) {
        diagnostics->result = HEALTH_CHECK_RESULT_FAIL;
        diagnostics->fault = APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK;
        return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
    }
    if (link == NETWORK_LINK_INITIALIZING) {
        diagnostics->result = HEALTH_CHECK_RESULT_UNKNOWN;
        diagnostics->fault = APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE;
        return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
    }
    if (link != NETWORK_LINK_UP) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK;
    }

    if (diagnostics->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
        if (!diagnostics->has_active ||
            !diagnostics->static_config_consistent) {
            diagnostics->result = HEALTH_CHECK_RESULT_FAIL;
            diagnostics->fault =
                APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG;
            return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
        }
    } else if (!diagnostics->has_active) {
        diagnostics->result = HEALTH_CHECK_RESULT_WARN;
        diagnostics->fault =
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS;
        return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
    }

    if (diagnostics->gateway_reachability ==
            APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE ||
        diagnostics->dns_reachability ==
            APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE) {
        diagnostics->result = HEALTH_CHECK_RESULT_FAIL;
        diagnostics->fault =
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY;
        return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
    }
    if (diagnostics->gateway_reachability ==
            APPLIANCE_NETWORK_REACHABILITY_UNKNOWN ||
        diagnostics->dns_reachability ==
            APPLIANCE_NETWORK_REACHABILITY_UNKNOWN) {
        diagnostics->result = HEALTH_CHECK_RESULT_WARN;
        diagnostics->fault =
            APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY;
        return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
    }

    diagnostics->result = HEALTH_CHECK_RESULT_PASS;
    diagnostics->fault = APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE;
    return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
}

enum appliance_network_diagnostics_status
appliance_network_diagnostics_publish(
    struct appliance_network_diagnostics *diagnostics,
    const struct appliance_network_config *desired,
    const struct network_link_state_record *link,
    const struct network_ipv4_config *active,
    enum appliance_network_reachability gateway_reachability,
    enum appliance_network_reachability dns_reachability)
{
    struct appliance_network_diagnostics candidate;
    enum appliance_network_diagnostics_status status;

    if (diagnostics == NULL || desired == NULL || link == NULL) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_NULL;
    }
    if (diagnostics->version != 0U &&
        diagnostics->version != APPLIANCE_NETWORK_DIAGNOSTICS_VERSION) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION;
    }
    if (appliance_network_config_validate(desired) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG;
    }
    if (network_link_state_validate(link) != NETWORK_LINK_OK) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK;
    }
    if (active != NULL &&
        network_ipv4_config_validate(active) != NETWORK_IPV4_OK) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE;
    }
    if (!appliance_network_diagnostics_reachability_valid(
            gateway_reachability) ||
        !appliance_network_diagnostics_reachability_valid(
            dns_reachability)) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY;
    }

    appliance_network_diagnostics_zero_bytes(
        &candidate, sizeof(candidate));
    candidate.version = APPLIANCE_NETWORK_DIAGNOSTICS_VERSION;
    candidate.published = true;
    appliance_network_diagnostics_copy_bytes(
        candidate.interface, desired->interface,
        desired->interface_length);
    candidate.interface[desired->interface_length] = '\0';
    candidate.interface_length = desired->interface_length;
    candidate.mode = desired->mode;
    candidate.device_id = link->device_id;
    candidate.link_state = link->state;
    candidate.link_generation = link->generation;
    if (active != NULL) {
        candidate.has_active = true;
        candidate.active = *active;
        candidate.static_config_consistent =
            desired->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC &&
            appliance_network_diagnostics_equal_bytes(
                active, &desired->static_config,
                sizeof(*active));
    }
    candidate.gateway_reachability = gateway_reachability;
    candidate.dns_reachability = dns_reachability;

    status = appliance_network_diagnostics_evaluate(&candidate);
    if (status != APPLIANCE_NETWORK_DIAGNOSTICS_OK) {
        return status;
    }

    *diagnostics = candidate;
    return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
}

static bool appliance_network_diagnostics_append_bytes(
    char *output,
    size_t capacity,
    size_t *position,
    const void *source,
    size_t length)
{
    const char *bytes = source;
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

static bool appliance_network_diagnostics_append_string(
    char *output,
    size_t capacity,
    size_t *position,
    const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return appliance_network_diagnostics_append_bytes(
        output, capacity, position, text, length);
}

static bool appliance_network_diagnostics_append_u8(
    char *output,
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
        if (!appliance_network_diagnostics_append_bytes(
                output, capacity, position,
                digits + digit_count - 1U, 1U)) {
            return false;
        }
        --digit_count;
    }
    return true;
}

static bool appliance_network_diagnostics_append_u32(
    char *output,
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
        if (!appliance_network_diagnostics_append_bytes(
                output, capacity, position,
                digits + digit_count - 1U, 1U)) {
            return false;
        }
        --digit_count;
    }
    return true;
}

static bool appliance_network_diagnostics_append_address(
    char *output,
    size_t capacity,
    size_t *position,
    const struct network_ipv4_address *address)
{
    size_t octet;

    for (octet = 0U;
         octet < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet) {
        if (!appliance_network_diagnostics_append_u8(
                output, capacity, position,
                address->octets[octet]) ||
            (octet + 1U < NETWORK_IPV4_ADDRESS_OCTETS &&
             !appliance_network_diagnostics_append_bytes(
                 output, capacity, position, ".", 1U))) {
            return false;
        }
    }
    return true;
}

enum appliance_network_diagnostics_status
appliance_network_diagnostics_render(
    const struct appliance_network_diagnostics *diagnostics,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    const char *mode_string;
    const char *link_string;
    const char *result_string;
    const char *fault_string;
    const char *gateway_string;
    const char *dns_string;
    size_t position = 0U;

    if (diagnostics == NULL || output == NULL ||
        output_length == NULL) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_NULL;
    }
    if (!diagnostics->published) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE;
    }
    if (diagnostics->version != APPLIANCE_NETWORK_DIAGNOSTICS_VERSION) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION;
    }
    mode_string = appliance_network_config_mode_string(
        diagnostics->mode);
    link_string = appliance_network_diagnostics_link_string(
        diagnostics->link_state);
    result_string = appliance_network_diagnostics_result_string(
        diagnostics->result);
    fault_string = appliance_network_diagnostics_fault_string(
        diagnostics->fault);
    gateway_string = appliance_network_reachability_string(
        diagnostics->gateway_reachability);
    dns_string = appliance_network_reachability_string(
        diagnostics->dns_reachability);
    if (mode_string == NULL || link_string == NULL ||
        result_string == NULL || fault_string == NULL ||
        gateway_string == NULL || dns_string == NULL) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE;
    }

    if (!appliance_network_diagnostics_append_string(
            output, capacity, &position, "network_diag v=1 interface=") ||
        !appliance_network_diagnostics_append_bytes(
            output, capacity, &position,
            diagnostics->interface, diagnostics->interface_length) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " mode=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, mode_string) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " device_id=") ||
        !appliance_network_diagnostics_append_u32(
            output, capacity, &position, diagnostics->device_id) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " link=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, link_string) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position,
            diagnostics->has_active ? " active=yes address=" :
                                      " active=no address=")) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW;
    }
    if (diagnostics->has_active) {
        if (!appliance_network_diagnostics_append_address(
                output, capacity, &position,
                &diagnostics->active.address) ||
            !appliance_network_diagnostics_append_string(
                output, capacity, &position, " prefix=") ||
            !appliance_network_diagnostics_append_u8(
                output, capacity, &position,
                diagnostics->active.prefix_length) ||
            !appliance_network_diagnostics_append_string(
                output, capacity, &position, " gateway=") ||
            !appliance_network_diagnostics_append_address(
                output, capacity, &position,
                &diagnostics->active.gateway) ||
            !appliance_network_diagnostics_append_string(
                output, capacity, &position, " dns=") ||
            !appliance_network_diagnostics_append_address(
                output, capacity, &position,
                &diagnostics->active.dns)) {
            return APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW;
        }
    } else {
        if (!appliance_network_diagnostics_append_string(
                output, capacity, &position, "none prefix=0 "
                "gateway=none dns=none")) {
            return APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW;
        }
    }

    if (!appliance_network_diagnostics_append_string(
            output, capacity, &position, " gateway_reach=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, gateway_string) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " dns_reach=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, dns_string) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " result=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, result_string) ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, " fault=") ||
        !appliance_network_diagnostics_append_string(
            output, capacity, &position, fault_string) ||
        !appliance_network_diagnostics_append_bytes(
            output, capacity, &position, "\n", 1U)) {
        return APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW;
    }
    output[position] = '\0';
    *output_length = position;
    return APPLIANCE_NETWORK_DIAGNOSTICS_OK;
}

const char *appliance_network_diagnostics_status_string(
    enum appliance_network_diagnostics_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_DIAGNOSTICS_OK:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_OK";
    case APPLIANCE_NETWORK_DIAGNOSTICS_NULL:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_NULL";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_VERSION";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_STATE";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_CONFIG";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_LINK";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_ACTIVE";
    case APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_BAD_REACHABILITY";
    case APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW:
        return "APPLIANCE_NETWORK_DIAGNOSTICS_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_network_reachability_string(
    enum appliance_network_reachability reachability)
{
    switch (reachability) {
    case APPLIANCE_NETWORK_REACHABILITY_UNKNOWN:
        return "UNKNOWN";
    case APPLIANCE_NETWORK_REACHABILITY_REACHABLE:
        return "REACHABLE";
    case APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE:
        return "UNREACHABLE";
    default:
        return NULL;
    }
}

const char *appliance_network_diagnostics_fault_string(
    enum appliance_network_diagnostics_fault fault)
{
    switch (fault) {
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE:
        return "NONE";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK:
        return "LINK";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS:
        return "ADDRESS";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG:
        return "CONFIG";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY:
        return "REACHABILITY";
    default:
        return NULL;
    }
}
