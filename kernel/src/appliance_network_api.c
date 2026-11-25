#include "appliance_network_api.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t appliance_network_api_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";

static bool appliance_network_api_config_source_valid(
    enum appliance_network_api_config_source source)
{
    return source == APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED ||
           source == APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING ||
           source == APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_INVALID;
}

static bool appliance_network_api_bytes_zero(
    const void *bytes,
    size_t length)
{
    const unsigned char *value = bytes;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (value[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool appliance_network_api_reachability_valid(
    enum appliance_network_reachability reachability)
{
    return reachability == APPLIANCE_NETWORK_REACHABILITY_UNKNOWN ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_REACHABLE ||
           reachability == APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE;
}

static bool appliance_network_api_health_result_valid(
    enum health_check_result_state result)
{
    return result == HEALTH_CHECK_RESULT_UNKNOWN ||
           result == HEALTH_CHECK_RESULT_PASS ||
           result == HEALTH_CHECK_RESULT_WARN ||
           result == HEALTH_CHECK_RESULT_FAIL;
}

static bool appliance_network_api_fault_valid(
    enum appliance_network_diagnostics_fault fault)
{
    return fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE ||
           fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK ||
           fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS ||
           fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG ||
           fault == APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY;
}

static bool appliance_network_api_diagnostics_valid(
    const struct appliance_network_config *desired,
    const struct appliance_network_diagnostics *diagnostics)
{
    if (desired == NULL || diagnostics == NULL ||
        !diagnostics->published ||
        diagnostics->version != APPLIANCE_NETWORK_DIAGNOSTICS_VERSION) {
        return false;
    }
    if (diagnostics->interface_length != desired->interface_length ||
        diagnostics->mode != desired->mode ||
        memcmp(diagnostics->interface,
               desired->interface,
               desired->interface_length) != 0) {
        return false;
    }
    if (diagnostics->device_id < NETWORK_DEVICE_ID_MIN ||
        diagnostics->device_id > NETWORK_DEVICE_ID_MAX ||
        diagnostics->link_generation < NETWORK_LINK_GENERATION_MIN ||
        diagnostics->link_generation > NETWORK_LINK_GENERATION_MAX ||
        (unsigned int)diagnostics->link_state >=
            NETWORK_LINK_STATE_COUNT) {
        return false;
    }
    if (!appliance_network_api_reachability_valid(
            diagnostics->gateway_reachability) ||
        !appliance_network_api_reachability_valid(
            diagnostics->dns_reachability) ||
        !appliance_network_api_health_result_valid(
            diagnostics->result) ||
        !appliance_network_api_fault_valid(diagnostics->fault)) {
        return false;
    }
    if (diagnostics->has_active) {
        if (network_ipv4_config_validate(&diagnostics->active) !=
            NETWORK_IPV4_OK) {
            return false;
        }
    } else if (!appliance_network_api_bytes_zero(
                   &diagnostics->active,
                   sizeof(diagnostics->active))) {
        return false;
    }
    return true;
}

static bool appliance_network_api_snapshot_valid(
    const struct appliance_network_api_snapshot *snapshot)
{
    if (snapshot == NULL || !snapshot->published ||
        snapshot->version != APPLIANCE_NETWORK_API_VERSION) {
        return false;
    }
    if (snapshot->config_state !=
            APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE &&
        snapshot->config_state != APPLIANCE_NETWORK_API_CONFIG_OK) {
        return false;
    }
    if (snapshot->config_state ==
        APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE) {
        return snapshot->config_source ==
                   APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE &&
               !snapshot->desired.initialized &&
               !snapshot->diagnostics.published;
    }
    if (!appliance_network_api_config_source_valid(
            snapshot->config_source) ||
        !snapshot->desired.initialized ||
        appliance_network_config_validate(&snapshot->desired) !=
            APPLIANCE_NETWORK_CONFIG_OK) {
        return false;
    }
    if (snapshot->diagnostics.published &&
        !appliance_network_api_diagnostics_valid(
            &snapshot->desired,
            &snapshot->diagnostics)) {
        return false;
    }
    return true;
}

static bool appliance_network_api_exact_equal(
    const uint8_t *target,
    uint32_t target_length,
    const char *expected,
    uint32_t expected_length)
{
    uint32_t index;

    if (target_length != expected_length) {
        return false;
    }
    for (index = 0U; index < target_length; ++index) {
        if (target[index] != (uint8_t)expected[index]) {
            return false;
        }
    }
    return true;
}

enum appliance_network_api_status appliance_network_api_snapshot_init(
    struct appliance_network_api_snapshot *snapshot,
    const struct appliance_network_config *desired,
    enum appliance_network_api_config_source config_source,
    const struct appliance_network_diagnostics *diagnostics)
{
    if (snapshot == NULL) {
        return APPLIANCE_NETWORK_API_NULL;
    }
    if (snapshot->published) {
        return APPLIANCE_NETWORK_API_BAD_STATE;
    }
    if (desired == NULL) {
        if (config_source != APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE ||
            diagnostics != NULL) {
            return APPLIANCE_NETWORK_API_BAD_CONFIG;
        }
        memset(snapshot, 0, sizeof(*snapshot));
        snapshot->version = APPLIANCE_NETWORK_API_VERSION;
        snapshot->config_state =
            APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE;
        snapshot->config_source =
            APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE;
        snapshot->published = true;
        return APPLIANCE_NETWORK_API_OK;
    }
    if (!appliance_network_api_config_source_valid(config_source)) {
        return APPLIANCE_NETWORK_API_BAD_CONFIG;
    }
    if (appliance_network_config_validate(desired) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_API_BAD_CONFIG;
    }
    if (diagnostics != NULL &&
        !appliance_network_api_diagnostics_valid(
            desired, diagnostics)) {
        return APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = APPLIANCE_NETWORK_API_VERSION;
    snapshot->config_state = APPLIANCE_NETWORK_API_CONFIG_OK;
    snapshot->config_source = config_source;
    snapshot->desired = *desired;
    if (diagnostics != NULL) {
        snapshot->diagnostics = *diagnostics;
    }
    snapshot->published = true;
    return APPLIANCE_NETWORK_API_OK;
}

static bool appliance_network_api_append_bytes(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const void *bytes,
    size_t length)
{
    const uint8_t *source = bytes;
    size_t index;

    if (*offset > capacity || length > capacity - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = source[index];
    }
    *offset += length;
    return true;
}

static bool appliance_network_api_append_literal(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *literal)
{
    return appliance_network_api_append_bytes(
        output, capacity, offset, literal, strlen(literal));
}

static bool appliance_network_api_append_u64(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count != 0U) {
        --count;
        if (!appliance_network_api_append_bytes(
                output, capacity, offset, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_network_api_append_json_key(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key)
{
    return appliance_network_api_append_literal(
               output, capacity, offset, ",") &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_network_api_append_literal(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\":");
}

static bool appliance_network_api_append_json_string_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const char *value)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_network_api_append_literal(
               output, capacity, offset, value) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_network_api_append_json_u64_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    uint64_t value)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_u64(
               output, capacity, offset, value);
}

static bool appliance_network_api_append_json_bool_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    bool value)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output,
               capacity,
               offset,
               value ? "true" : "false");
}

static bool appliance_network_api_append_address(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const struct network_ipv4_address *address)
{
    size_t octet;

    for (octet = 0U;
         octet < NETWORK_IPV4_ADDRESS_OCTETS;
         ++octet) {
        if (!appliance_network_api_append_u64(
                output,
                capacity,
                offset,
                address->octets[octet]) ||
            (octet + 1U < NETWORK_IPV4_ADDRESS_OCTETS &&
             !appliance_network_api_append_literal(
                 output, capacity, offset, "."))) {
            return false;
        }
    }
    return true;
}

static bool appliance_network_api_append_json_address_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const struct network_ipv4_address *address)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_network_api_append_address(
               output, capacity, offset, address) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_network_api_append_interface_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const struct appliance_network_config *config)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_network_api_append_bytes(
               output,
               capacity,
               offset,
               config->interface,
               config->interface_length) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_network_api_append_interface_text(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const char *value)
{
    return appliance_network_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_network_api_append_literal(
               output, capacity, offset, value) &&
           appliance_network_api_append_literal(
               output, capacity, offset, "\"");
}

static const char *appliance_network_api_mode_json(
    enum appliance_network_config_mode mode)
{
    switch (mode) {
    case APPLIANCE_NETWORK_CONFIG_MODE_NONE:
        return "none";
    case APPLIANCE_NETWORK_CONFIG_MODE_STATIC:
        return "static";
    case APPLIANCE_NETWORK_CONFIG_MODE_DHCP:
        return "dhcp";
    default:
        return NULL;
    }
}

static const char *appliance_network_api_link_json(
    enum network_link_state state)
{
    switch (state) {
    case NETWORK_LINK_DOWN:
        return "down";
    case NETWORK_LINK_INITIALIZING:
        return "initializing";
    case NETWORK_LINK_UP:
        return "up";
    case NETWORK_LINK_FAILED:
        return "failed";
    default:
        return NULL;
    }
}

static const char *appliance_network_api_reachability_json(
    enum appliance_network_reachability reachability)
{
    switch (reachability) {
    case APPLIANCE_NETWORK_REACHABILITY_UNKNOWN:
        return "unknown";
    case APPLIANCE_NETWORK_REACHABILITY_REACHABLE:
        return "reachable";
    case APPLIANCE_NETWORK_REACHABILITY_UNREACHABLE:
        return "unreachable";
    default:
        return NULL;
    }
}

static const char *appliance_network_api_health_json(
    enum health_check_result_state result)
{
    switch (result) {
    case HEALTH_CHECK_RESULT_UNKNOWN:
        return "unknown";
    case HEALTH_CHECK_RESULT_PASS:
        return "pass";
    case HEALTH_CHECK_RESULT_WARN:
        return "warn";
    case HEALTH_CHECK_RESULT_FAIL:
        return "fail";
    default:
        return NULL;
    }
}

static const char *appliance_network_api_fault_json(
    enum appliance_network_diagnostics_fault fault)
{
    switch (fault) {
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_NONE:
        return "none";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_LINK:
        return "link";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_ADDRESS:
        return "address";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_CONFIG:
        return "config";
    case APPLIANCE_NETWORK_DIAGNOSTICS_FAULT_REACHABILITY:
        return "reachability";
    default:
        return NULL;
    }
}

static bool appliance_network_api_build_config_body(
    const struct appliance_network_api_snapshot *snapshot,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    const struct appliance_network_config *desired = &snapshot->desired;
    const char *config_state =
        appliance_network_api_config_state_string(
            snapshot->config_state);
    const char *config_source =
        appliance_network_api_config_source_string(
            snapshot->config_source);
    const char *mode;
    size_t offset = 0U;

    if (config_state == NULL || config_source == NULL) {
        return false;
    }
    if (!appliance_network_api_append_literal(
            output, capacity, &offset,
            "{\"api\":\"hfoc\",\"version\":1,\"name\":\"network\","
            "\"status\":\"ok\",\"auth\":\"unavailable\","
            "\"mutating\":false") ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "config_state", config_state) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "config_source", config_source)) {
        return false;
    }
    if (snapshot->config_state == APPLIANCE_NETWORK_API_CONFIG_OK) {
        mode = appliance_network_api_mode_json(desired->mode);
        if (mode == NULL ||
            !appliance_network_api_append_interface_field(
                output, capacity, &offset,
                "interface", desired) ||
            !appliance_network_api_append_json_string_field(
                output, capacity, &offset,
                "mode", mode)) {
            return false;
        }
        if (desired->mode == APPLIANCE_NETWORK_CONFIG_MODE_STATIC) {
            const struct network_ipv4_config *static_config =
                &desired->static_config;

            if (!appliance_network_api_append_json_address_field(
                    output, capacity, &offset,
                    "ipv4_address", &static_config->address) ||
                !appliance_network_api_append_json_u64_field(
                    output, capacity, &offset,
                    "ipv4_prefix",
                    static_config->prefix_length) ||
                !appliance_network_api_append_json_address_field(
                    output, capacity, &offset,
                    "ipv4_gateway", &static_config->gateway) ||
                !appliance_network_api_append_json_address_field(
                    output, capacity, &offset,
                    "ipv4_dns", &static_config->dns)) {
                return false;
            }
        }
    }
    if (!appliance_network_api_append_literal(
            output, capacity, &offset, "}")) {
        return false;
    }
    *output_length = offset;
    return true;
}

static bool appliance_network_api_build_status_body(
    const struct appliance_network_api_snapshot *snapshot,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    const struct appliance_network_config *desired =
        snapshot->config_state == APPLIANCE_NETWORK_API_CONFIG_OK
            ? &snapshot->desired
            : NULL;
    const struct appliance_network_diagnostics *diagnostics =
        snapshot->diagnostics.published ? &snapshot->diagnostics : NULL;
    const char *config_state =
        appliance_network_api_config_state_string(
            snapshot->config_state);
    const char *mode = desired != NULL
        ? appliance_network_api_mode_json(desired->mode)
        : "none";
    const char *link = diagnostics != NULL
        ? appliance_network_api_link_json(diagnostics->link_state)
        : "none";
    const char *gateway_reachability = diagnostics != NULL
        ? appliance_network_api_reachability_json(
              diagnostics->gateway_reachability)
        : "unknown";
    const char *dns_reachability = diagnostics != NULL
        ? appliance_network_api_reachability_json(
              diagnostics->dns_reachability)
        : "unknown";
    const char *health = diagnostics != NULL
        ? appliance_network_api_health_json(diagnostics->result)
        : "unavailable";
    const char *fault = diagnostics != NULL
        ? appliance_network_api_fault_json(diagnostics->fault)
        : "none";
    size_t offset = 0U;
    bool active = diagnostics != NULL && diagnostics->has_active;
    const char *diagnostics_state =
        diagnostics != NULL ? "available" : "unavailable";
    const char *none = "none";
    const struct network_ipv4_address *active_address = NULL;
    uint8_t active_prefix = 0U;

    if (config_state == NULL || mode == NULL || link == NULL ||
        gateway_reachability == NULL || dns_reachability == NULL ||
        health == NULL || fault == NULL) {
        return false;
    }
    if (active) {
        active_address = &diagnostics->active.address;
        active_prefix = diagnostics->active.prefix_length;
    }
    if (!appliance_network_api_append_literal(
            output, capacity, &offset,
            "{\"api\":\"hfoc\",\"version\":1,"
            "\"name\":\"network-status\",\"status\":\"ok\","
            "\"auth\":\"unavailable\",\"mutating\":false") ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "config_state", config_state) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "diagnostics", diagnostics_state) ||
        !appliance_network_api_append_interface_text(
            output, capacity, &offset,
            "interface",
            desired != NULL ? (const char *)desired->interface : none) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "mode", mode) ||
        !appliance_network_api_append_json_bool_field(
            output, capacity, &offset,
            "active", active)) {
        return false;
    }
    if (active_address != NULL) {
        const struct network_ipv4_config *active_config =
            &diagnostics->active;

        if (!appliance_network_api_append_json_address_field(
                output, capacity, &offset,
                "ipv4_address", &active_config->address) ||
            !appliance_network_api_append_json_u64_field(
                output, capacity, &offset,
                "ipv4_prefix", active_config->prefix_length) ||
            !appliance_network_api_append_json_address_field(
                output, capacity, &offset,
                "ipv4_gateway", &active_config->gateway) ||
            !appliance_network_api_append_json_address_field(
                output, capacity, &offset,
                "ipv4_dns", &active_config->dns)) {
            return false;
        }
    } else {
        const char *address = none;

        if (!appliance_network_api_append_json_string_field(
                output, capacity, &offset,
                "ipv4_address", address) ||
            !appliance_network_api_append_json_u64_field(
                output, capacity, &offset,
                "ipv4_prefix", active_prefix) ||
            !appliance_network_api_append_json_string_field(
                output, capacity, &offset,
                "ipv4_gateway", address) ||
            !appliance_network_api_append_json_string_field(
                output, capacity, &offset,
                "ipv4_dns", address)) {
            return false;
        }
    }
    if (!appliance_network_api_append_json_u64_field(
            output, capacity, &offset,
            "device_id",
            diagnostics != NULL ? diagnostics->device_id : 0U) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "link", link) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "gateway_reachability", gateway_reachability) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "dns_reachability", dns_reachability) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "health", health) ||
        !appliance_network_api_append_json_string_field(
            output, capacity, &offset,
            "fault", fault) ||
        !appliance_network_api_append_literal(
            output, capacity, &offset, "}")) {
        return false;
    }
    *output_length = offset;
    return true;
}

static void appliance_network_api_publish_static(
    struct appliance_network_api_result *result,
    enum appliance_network_api_route route,
    enum http_status_code status,
    const uint8_t *body,
    uint32_t body_length)
{
    memset(result, 0, sizeof(*result));
    result->version = APPLIANCE_NETWORK_API_VERSION;
    result->published = true;
    result->route = route;
    result->response.status = status;
    result->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result->response.body = body;
    result->response.body_length = body_length;
}

enum appliance_network_api_status appliance_network_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_network_api_snapshot *snapshot,
    struct appliance_network_api_result *result_out)
{
    enum appliance_network_api_route route;
    uint8_t body[APPLIANCE_NETWORK_API_BODY_MAX];
    size_t body_length = 0U;

    if (target == NULL || snapshot == NULL || result_out == NULL) {
        return APPLIANCE_NETWORK_API_NULL;
    }
    if (appliance_network_api_exact_equal(
            target,
            target_length,
            APPLIANCE_NETWORK_API_CONFIG_PATH,
            APPLIANCE_NETWORK_API_CONFIG_PATH_LENGTH)) {
        route = APPLIANCE_NETWORK_API_ROUTE_CONFIG;
    } else if (appliance_network_api_exact_equal(
                   target,
                   target_length,
                   APPLIANCE_NETWORK_API_STATUS_PATH,
                   APPLIANCE_NETWORK_API_STATUS_PATH_LENGTH)) {
        route = APPLIANCE_NETWORK_API_ROUTE_STATUS;
    } else {
        return APPLIANCE_NETWORK_API_NOT_NETWORK;
    }
    if (!appliance_network_api_snapshot_valid(snapshot)) {
        return APPLIANCE_NETWORK_API_BAD_STATE;
    }
    if (method != HTTP_METHOD_GET &&
        method != HTTP_METHOD_HEAD) {
        appliance_network_api_publish_static(
            result_out,
            route,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            appliance_network_api_method_body,
            (uint32_t)(sizeof(appliance_network_api_method_body) - 1U));
        return APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED;
    }
    if (route == APPLIANCE_NETWORK_API_ROUTE_CONFIG) {
        if (!appliance_network_api_build_config_body(
                snapshot,
                body,
                sizeof(body),
                &body_length)) {
            return APPLIANCE_NETWORK_API_OVERFLOW;
        }
    } else {
        if (!appliance_network_api_build_status_body(
                snapshot,
                body,
                sizeof(body),
                &body_length)) {
            return APPLIANCE_NETWORK_API_OVERFLOW;
        }
    }

    memset(result_out, 0, sizeof(*result_out));
    memcpy(result_out->body, body, body_length);
    result_out->version = APPLIANCE_NETWORK_API_VERSION;
    result_out->published = true;
    result_out->route = route;
    result_out->response.status = HTTP_STATUS_200_OK;
    result_out->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result_out->response.body = result_out->body;
    result_out->response.body_length = (uint32_t)body_length;
    return APPLIANCE_NETWORK_API_OK;
}

const char *appliance_network_api_status_string(
    enum appliance_network_api_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_API_OK:
        return "APPLIANCE_NETWORK_API_OK";
    case APPLIANCE_NETWORK_API_NULL:
        return "APPLIANCE_NETWORK_API_NULL";
    case APPLIANCE_NETWORK_API_NOT_NETWORK:
        return "APPLIANCE_NETWORK_API_NOT_NETWORK";
    case APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_NETWORK_API_METHOD_NOT_ALLOWED";
    case APPLIANCE_NETWORK_API_BAD_STATE:
        return "APPLIANCE_NETWORK_API_BAD_STATE";
    case APPLIANCE_NETWORK_API_BAD_VERSION:
        return "APPLIANCE_NETWORK_API_BAD_VERSION";
    case APPLIANCE_NETWORK_API_BAD_CONFIG:
        return "APPLIANCE_NETWORK_API_BAD_CONFIG";
    case APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS:
        return "APPLIANCE_NETWORK_API_BAD_DIAGNOSTICS";
    case APPLIANCE_NETWORK_API_OVERFLOW:
        return "APPLIANCE_NETWORK_API_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_network_api_route_string(
    enum appliance_network_api_route route)
{
    switch (route) {
    case APPLIANCE_NETWORK_API_ROUTE_NONE:
        return "APPLIANCE_NETWORK_API_ROUTE_NONE";
    case APPLIANCE_NETWORK_API_ROUTE_CONFIG:
        return "APPLIANCE_NETWORK_API_ROUTE_CONFIG";
    case APPLIANCE_NETWORK_API_ROUTE_STATUS:
        return "APPLIANCE_NETWORK_API_ROUTE_STATUS";
    default:
        return NULL;
    }
}

const char *appliance_network_api_config_state_string(
    enum appliance_network_api_config_state state)
{
    switch (state) {
    case APPLIANCE_NETWORK_API_CONFIG_UNAVAILABLE:
        return "unavailable";
    case APPLIANCE_NETWORK_API_CONFIG_OK:
        return "ok";
    default:
        return NULL;
    }
}

const char *appliance_network_api_config_source_string(
    enum appliance_network_api_config_source source)
{
    switch (source) {
    case APPLIANCE_NETWORK_API_CONFIG_SOURCE_NONE:
        return "none";
    case APPLIANCE_NETWORK_API_CONFIG_SOURCE_COMMITTED:
        return "committed";
    case APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_MISSING:
        return "default_missing";
    case APPLIANCE_NETWORK_API_CONFIG_SOURCE_DEFAULT_INVALID:
        return "default_invalid";
    default:
        return NULL;
    }
}
