#include "appliance_system_api.h"

#include "appliance_config_validation.h"
#include "appliance_identity.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t appliance_system_api_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"system\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";

static bool appliance_system_api_state_valid(
    enum appliance_system_api_state state)
{
    return state == APPLIANCE_SYSTEM_STATE_UNAVAILABLE ||
           state == APPLIANCE_SYSTEM_STATE_OK ||
           state == APPLIANCE_SYSTEM_STATE_DEGRADED ||
           state == APPLIANCE_SYSTEM_STATE_FAILED;
}

static bool appliance_system_api_config_source_valid(
    enum appliance_config_validation_source source)
{
    return source == APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED ||
           source == APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING ||
           source == APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID;
}

static const char *appliance_system_api_config_source_json(
    enum appliance_config_validation_source source);

static bool appliance_system_api_identity_matches(
    const struct appliance_system_api_snapshot *snapshot)
{
    struct appliance_identity identity;

    memset(&identity, 0, sizeof(identity));
    identity.version = APPLIANCE_IDENTITY_VERSION;
    memcpy(identity.machine_class, snapshot->machine_class,
           sizeof(identity.machine_class));
    memcpy(identity.product, snapshot->product,
           sizeof(identity.product));
    memcpy(identity.architecture, snapshot->architecture,
           sizeof(identity.architecture));
    memcpy(identity.platform, snapshot->platform,
           sizeof(identity.platform));
    memcpy(identity.hostname, snapshot->hostname,
           sizeof(identity.hostname));
    memcpy(identity.instance, snapshot->instance,
           sizeof(identity.instance));
    identity.published = true;
    return appliance_identity_validate(&identity) ==
           APPLIANCE_IDENTITY_OK;
}

static bool appliance_system_api_snapshot_valid(
    const struct appliance_system_api_snapshot *snapshot)
{
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_SYSTEM_API_VERSION ||
        !appliance_system_api_state_valid(snapshot->health_state) ||
        !appliance_system_api_state_valid(snapshot->config_state) ||
        snapshot->recovery_mode >
            APPLIANCE_SYSTEM_API_RECOVERY_MODE_MAX ||
        snapshot->active_process_count >
            APPLIANCE_SYSTEM_API_COUNT_MAX) {
        return false;
    }
    if (!appliance_system_api_identity_matches(snapshot)) {
        return false;
    }
    if (snapshot->config_state == APPLIANCE_SYSTEM_STATE_UNAVAILABLE) {
        return snapshot->config_source ==
                   APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE &&
               snapshot->config_hostname_length == 0U;
    }
    return appliance_system_api_config_source_valid(
               snapshot->config_source) &&
           snapshot->config_hostname_length != 0U &&
           snapshot->config_hostname_length <=
               APPLIANCE_SYSTEM_API_CONFIG_HOSTNAME_MAX &&
           appliance_config_validation_validate_hostname(
               snapshot->config_hostname,
               snapshot->config_hostname_length) ==
               APPLIANCE_CONFIG_VALIDATION_OK;
}

static bool appliance_system_api_exact_equal(
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

static bool appliance_system_api_copy_text(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index;

    for (index = 0U; source[index] != '\0'; ++index) {
        if (index >= capacity - 1U) {
            return false;
        }
        destination[index] = source[index];
    }
    destination[index] = '\0';
    return true;
}

static bool appliance_system_api_copy_hostname(
    struct appliance_system_api_snapshot *snapshot,
    const char *hostname,
    size_t hostname_length)
{
    size_t index;

    if (hostname_length == 0U ||
        hostname_length > APPLIANCE_SYSTEM_API_CONFIG_HOSTNAME_MAX) {
        return false;
    }
    for (index = 0U; index < hostname_length; ++index) {
        snapshot->config_hostname[index] = hostname[index];
    }
    snapshot->config_hostname[hostname_length] = '\0';
    snapshot->config_hostname_length = (uint8_t)hostname_length;
    return true;
}

enum appliance_system_api_status appliance_system_api_snapshot_init(
    struct appliance_system_api_snapshot *snapshot,
    const struct appliance_identity *identity,
    enum appliance_system_api_state health_state,
    uint64_t uptime_ticks,
    enum appliance_system_api_state config_state,
    enum appliance_config_validation_source config_source,
    const char *config_hostname,
    size_t config_hostname_length,
    uint32_t recovery_mode,
    uint32_t active_process_count)
{
    enum appliance_identity_status identity_status;

    if (snapshot == NULL || identity == NULL) {
        return APPLIANCE_SYSTEM_API_NULL;
    }
    if (snapshot->published) {
        return APPLIANCE_SYSTEM_API_BAD_STATE;
    }
    identity_status = appliance_identity_validate(identity);
    if (identity_status != APPLIANCE_IDENTITY_OK) {
        return APPLIANCE_SYSTEM_API_BAD_IDENTITY;
    }
    if (!appliance_system_api_state_valid(health_state)) {
        return APPLIANCE_SYSTEM_API_BAD_HEALTH;
    }
    if (!appliance_system_api_state_valid(config_state)) {
        return APPLIANCE_SYSTEM_API_BAD_CONFIG;
    }
    if (recovery_mode > APPLIANCE_SYSTEM_API_RECOVERY_MODE_MAX ||
        active_process_count > APPLIANCE_SYSTEM_API_COUNT_MAX) {
        return APPLIANCE_SYSTEM_API_BAD_STATE;
    }
    if (config_state == APPLIANCE_SYSTEM_STATE_UNAVAILABLE) {
        if (config_source !=
                APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE ||
            config_hostname != NULL ||
            config_hostname_length != 0U) {
            return APPLIANCE_SYSTEM_API_BAD_CONFIG;
        }
    } else {
        if (!appliance_system_api_config_source_valid(config_source) ||
            config_hostname == NULL ||
            appliance_config_validation_validate_hostname(
                config_hostname, config_hostname_length) !=
                APPLIANCE_CONFIG_VALIDATION_OK) {
            return APPLIANCE_SYSTEM_API_BAD_CONFIG;
        }
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (!appliance_system_api_copy_text(
            snapshot->machine_class,
            sizeof(snapshot->machine_class),
            identity->machine_class) ||
        !appliance_system_api_copy_text(
            snapshot->product,
            sizeof(snapshot->product),
            identity->product) ||
        !appliance_system_api_copy_text(
            snapshot->architecture,
            sizeof(snapshot->architecture),
            identity->architecture) ||
        !appliance_system_api_copy_text(
            snapshot->platform,
            sizeof(snapshot->platform),
            identity->platform) ||
        !appliance_system_api_copy_text(
            snapshot->hostname,
            sizeof(snapshot->hostname),
            identity->hostname) ||
        !appliance_system_api_copy_text(
            snapshot->instance,
            sizeof(snapshot->instance),
            identity->instance)) {
        return APPLIANCE_SYSTEM_API_BAD_IDENTITY;
    }
    if (config_state != APPLIANCE_SYSTEM_STATE_UNAVAILABLE &&
        !appliance_system_api_copy_hostname(
            snapshot, config_hostname, config_hostname_length)) {
        return APPLIANCE_SYSTEM_API_BAD_CONFIG;
    }
    snapshot->version = APPLIANCE_SYSTEM_API_VERSION;
    snapshot->health_state = health_state;
    snapshot->uptime_ticks = uptime_ticks;
    snapshot->config_state = config_state;
    snapshot->config_source = config_source;
    snapshot->recovery_mode = recovery_mode;
    snapshot->active_process_count = active_process_count;
    snapshot->published = true;
    return APPLIANCE_SYSTEM_API_OK;
}

static bool appliance_system_api_append_bytes(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const void *bytes,
    size_t length)
{
    const uint8_t *input = bytes;
    size_t index;

    if (*offset > capacity || length > capacity - *offset) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        output[*offset + index] = input[index];
    }
    *offset += length;
    return true;
}

static bool appliance_system_api_append_literal(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *literal)
{
    return appliance_system_api_append_bytes(
        output, capacity, offset, literal, strlen(literal));
}

static bool appliance_system_api_append_u64(
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
        if (!appliance_system_api_append_bytes(
                output, capacity, offset, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_system_api_append_json_key(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key)
{
    return appliance_system_api_append_literal(
               output, capacity, offset, ",") &&
           appliance_system_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_system_api_append_literal(
               output, capacity, offset, key) &&
           appliance_system_api_append_literal(
               output, capacity, offset, "\":");
}

static bool appliance_system_api_append_json_string_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const char *value)
{
    return appliance_system_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_system_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_system_api_append_literal(
               output, capacity, offset, value) &&
           appliance_system_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_system_api_append_json_u64_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    uint64_t value)
{
    return appliance_system_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_system_api_append_u64(
               output, capacity, offset, value);
}

static bool appliance_system_api_build_success(
    const struct appliance_system_api_snapshot *snapshot,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;
    const char *health =
        appliance_system_api_state_string(snapshot->health_state);
    const char *config_state =
        appliance_system_api_state_string(snapshot->config_state);
    const char *config_source =
        appliance_system_api_config_source_json(snapshot->config_source);

    if (health == NULL || config_state == NULL ||
        config_source == NULL) {
        return false;
    }
    if (!appliance_system_api_append_literal(
            output, capacity, &offset,
            "{\"api\":\"hfoc\",\"version\":1,\"name\":\"system\","
            "\"status\":\"ok\",\"auth\":\"unavailable\","
            "\"mutating\":false") ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "machine_class",
            snapshot->machine_class) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "product",
            snapshot->product) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "architecture",
            snapshot->architecture) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "platform",
            snapshot->platform) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "hostname",
            snapshot->hostname) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "instance",
            snapshot->instance) ||
        !appliance_system_api_append_json_u64_field(
            output, capacity, &offset, "uptime_ticks",
            snapshot->uptime_ticks) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "health", health) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "config_state",
            config_state) ||
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "config_source",
            config_source)) {
        return false;
    }
    if (snapshot->config_state != APPLIANCE_SYSTEM_STATE_UNAVAILABLE &&
        !appliance_system_api_append_json_string_field(
            output, capacity, &offset, "config_hostname",
            snapshot->config_hostname)) {
        return false;
    }
    if (!appliance_system_api_append_json_u64_field(
            output, capacity, &offset, "recovery_mode",
            snapshot->recovery_mode) ||
        !appliance_system_api_append_json_u64_field(
            output, capacity, &offset, "active_process_count",
            snapshot->active_process_count) ||
        !appliance_system_api_append_literal(
            output, capacity, &offset, "}")) {
        return false;
    }
    *output_length = offset;
    return true;
}

static void appliance_system_api_publish_static(
    struct appliance_system_api_result *result,
    enum http_status_code status,
    const uint8_t *body,
    uint32_t body_length)
{
    memset(result, 0, sizeof(*result));
    result->version = APPLIANCE_SYSTEM_API_VERSION;
    result->published = true;
    result->response.status = status;
    result->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result->response.body = body;
    result->response.body_length = body_length;
}

enum appliance_system_api_status appliance_system_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_system_api_snapshot *snapshot,
    struct appliance_system_api_result *result_out)
{
    uint8_t body[APPLIANCE_SYSTEM_API_BODY_MAX];
    size_t body_length = 0U;

    if (target == NULL || snapshot == NULL || result_out == NULL) {
        return APPLIANCE_SYSTEM_API_NULL;
    }
    if (!appliance_system_api_exact_equal(
            target,
            target_length,
            APPLIANCE_SYSTEM_API_PATH,
            APPLIANCE_SYSTEM_API_PATH_LENGTH)) {
        return APPLIANCE_SYSTEM_API_NOT_SYSTEM;
    }
    if (!appliance_system_api_snapshot_valid(snapshot)) {
        return APPLIANCE_SYSTEM_API_BAD_STATE;
    }
    if (method != HTTP_METHOD_GET &&
        method != HTTP_METHOD_HEAD) {
        appliance_system_api_publish_static(
            result_out,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            appliance_system_api_method_body,
            (uint32_t)(sizeof(appliance_system_api_method_body) - 1U));
        return APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED;
    }
    if (!appliance_system_api_build_success(
            snapshot,
            body,
            sizeof(body),
            &body_length)) {
        return APPLIANCE_SYSTEM_API_OVERFLOW;
    }
    memset(result_out, 0, sizeof(*result_out));
    memcpy(result_out->body, body, body_length);
    result_out->version = APPLIANCE_SYSTEM_API_VERSION;
    result_out->published = true;
    result_out->response.status = HTTP_STATUS_200_OK;
    result_out->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result_out->response.body = result_out->body;
    result_out->response.body_length = (uint32_t)body_length;
    return APPLIANCE_SYSTEM_API_OK;
}

static const char *appliance_system_api_config_source_json(
    enum appliance_config_validation_source source)
{
    switch (source) {
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_NONE:
        return "none";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED:
        return "committed";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING:
        return "default_missing";
    case APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID:
        return "default_invalid";
    default:
        return NULL;
    }
}

const char *appliance_system_api_status_string(
    enum appliance_system_api_status status)
{
    switch (status) {
    case APPLIANCE_SYSTEM_API_OK:
        return "APPLIANCE_SYSTEM_API_OK";
    case APPLIANCE_SYSTEM_API_NULL:
        return "APPLIANCE_SYSTEM_API_NULL";
    case APPLIANCE_SYSTEM_API_NOT_SYSTEM:
        return "APPLIANCE_SYSTEM_API_NOT_SYSTEM";
    case APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_SYSTEM_API_METHOD_NOT_ALLOWED";
    case APPLIANCE_SYSTEM_API_BAD_STATE:
        return "APPLIANCE_SYSTEM_API_BAD_STATE";
    case APPLIANCE_SYSTEM_API_BAD_VERSION:
        return "APPLIANCE_SYSTEM_API_BAD_VERSION";
    case APPLIANCE_SYSTEM_API_BAD_IDENTITY:
        return "APPLIANCE_SYSTEM_API_BAD_IDENTITY";
    case APPLIANCE_SYSTEM_API_BAD_CONFIG:
        return "APPLIANCE_SYSTEM_API_BAD_CONFIG";
    case APPLIANCE_SYSTEM_API_BAD_HEALTH:
        return "APPLIANCE_SYSTEM_API_BAD_HEALTH";
    case APPLIANCE_SYSTEM_API_OVERFLOW:
        return "APPLIANCE_SYSTEM_API_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_system_api_state_string(
    enum appliance_system_api_state state)
{
    switch (state) {
    case APPLIANCE_SYSTEM_STATE_UNAVAILABLE:
        return "unavailable";
    case APPLIANCE_SYSTEM_STATE_OK:
        return "ok";
    case APPLIANCE_SYSTEM_STATE_DEGRADED:
        return "degraded";
    case APPLIANCE_SYSTEM_STATE_FAILED:
        return "failed";
    default:
        return NULL;
    }
}
