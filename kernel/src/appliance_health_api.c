#include "appliance_health_api.h"

#include "appliance_health.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t appliance_health_api_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"health\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";

static bool appliance_health_api_output_valid(
    const struct appliance_health_output *health)
{
    const char *state;
    const char *domain;
    const char *fault;

    if (health == NULL ||
        !health->published ||
        health->version != APPLIANCE_HEALTH_VERSION) {
        return false;
    }
    state = appliance_health_state_string(health->state);
    domain = appliance_health_domain_string(
        health->first_fault_domain);
    fault = appliance_health_fault_string(health->first_fault);
    if (state == NULL || domain == NULL || fault == NULL) {
        return false;
    }
    if (health->first_fault_domain != APPLIANCE_HEALTH_DOMAIN_NONE &&
        health->first_fault == APPLIANCE_HEALTH_FAULT_NONE) {
        return false;
    }
    if (health->first_fault_domain == APPLIANCE_HEALTH_DOMAIN_NONE &&
        health->first_fault != APPLIANCE_HEALTH_FAULT_NONE) {
        return false;
    }
    if (health->state == APPLIANCE_HEALTH_STATE_OK &&
        !health->healthy) {
        return false;
    }
    if (health->state != APPLIANCE_HEALTH_STATE_OK &&
        health->healthy) {
        return false;
    }
    return true;
}

static bool appliance_health_api_snapshot_valid(
    const struct appliance_health_api_snapshot *snapshot)
{
    return snapshot != NULL &&
           snapshot->published &&
           snapshot->version == APPLIANCE_HEALTH_API_VERSION &&
           appliance_health_api_output_valid(&snapshot->health);
}

static bool appliance_health_api_exact_equal(
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

static bool appliance_health_api_append_bytes(
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

static bool appliance_health_api_append_literal(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *literal)
{
    return appliance_health_api_append_bytes(
        output,
        capacity,
        offset,
        literal,
        strlen(literal));
}

static bool appliance_health_api_append_json_key(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key)
{
    return appliance_health_api_append_literal(
               output, capacity, offset, ",") &&
           appliance_health_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_health_api_append_literal(
               output, capacity, offset, key) &&
           appliance_health_api_append_literal(
               output, capacity, offset, "\":");
}

static bool appliance_health_api_append_json_string_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const char *value)
{
    return appliance_health_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_health_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_health_api_append_literal(
               output, capacity, offset, value) &&
           appliance_health_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_health_api_append_json_bool_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    bool value)
{
    return appliance_health_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_health_api_append_literal(
               output,
               capacity,
               offset,
               value ? "true" : "false");
}

static bool appliance_health_api_build_success(
    const struct appliance_health_api_snapshot *snapshot,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    const char *state = appliance_health_state_string(
        snapshot->health.state);
    const char *domain = appliance_health_domain_string(
        snapshot->health.first_fault_domain);
    const char *fault = appliance_health_fault_string(
        snapshot->health.first_fault);
    size_t offset = 0U;

    if (state == NULL || domain == NULL || fault == NULL) {
        return false;
    }
    if (!appliance_health_api_append_literal(
            output, capacity, &offset,
            "{\"api\":\"hfoc\",\"version\":1,\"name\":\"health\","
            "\"status\":\"ok\",\"auth\":\"unavailable\","
            "\"mutating\":false") ||
        !appliance_health_api_append_json_string_field(
            output, capacity, &offset, "state", state) ||
        !appliance_health_api_append_json_bool_field(
            output, capacity, &offset, "healthy",
            snapshot->health.healthy) ||
        !appliance_health_api_append_json_bool_field(
            output, capacity, &offset, "critical",
            snapshot->health.critical) ||
        !appliance_health_api_append_json_string_field(
            output, capacity, &offset,
            "first_fault_domain", domain) ||
        !appliance_health_api_append_json_string_field(
            output, capacity, &offset, "first_fault", fault) ||
        !appliance_health_api_append_literal(
            output, capacity, &offset, "}")) {
        return false;
    }
    *output_length = offset;
    return true;
}

static void appliance_health_api_publish_static(
    struct appliance_health_api_result *result,
    enum http_status_code status,
    const uint8_t *body,
    uint32_t body_length)
{
    memset(result, 0, sizeof(*result));
    result->version = APPLIANCE_HEALTH_API_VERSION;
    result->published = true;
    result->response.status = status;
    result->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result->response.body = body;
    result->response.body_length = body_length;
}

enum appliance_health_api_status appliance_health_api_snapshot_init(
    struct appliance_health_api_snapshot *snapshot,
    const struct appliance_health_input *input)
{
    struct appliance_health_output output;

    if (snapshot == NULL || input == NULL) {
        return APPLIANCE_HEALTH_API_NULL;
    }
    if (snapshot->published) {
        return APPLIANCE_HEALTH_API_BAD_STATE;
    }
    memset(&output, 0, sizeof(output));
    if (appliance_health_evaluate(input, &output) !=
        APPLIANCE_HEALTH_STATUS_OK) {
        return APPLIANCE_HEALTH_API_BAD_HEALTH;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = APPLIANCE_HEALTH_API_VERSION;
    snapshot->health = output;
    snapshot->published = true;
    return APPLIANCE_HEALTH_API_OK;
}

enum appliance_health_api_status appliance_health_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_health_api_snapshot *snapshot,
    struct appliance_health_api_result *result_out)
{
    uint8_t body[APPLIANCE_HEALTH_API_BODY_MAX];
    size_t body_length = 0U;

    if (target == NULL || snapshot == NULL || result_out == NULL) {
        return APPLIANCE_HEALTH_API_NULL;
    }
    if (!appliance_health_api_exact_equal(
            target,
            target_length,
            APPLIANCE_HEALTH_API_PATH,
            APPLIANCE_HEALTH_API_PATH_LENGTH)) {
        return APPLIANCE_HEALTH_API_NOT_HEALTH;
    }
    if (!appliance_health_api_snapshot_valid(snapshot)) {
        return APPLIANCE_HEALTH_API_BAD_STATE;
    }
    if (method != HTTP_METHOD_GET &&
        method != HTTP_METHOD_HEAD) {
        appliance_health_api_publish_static(
            result_out,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            appliance_health_api_method_body,
            (uint32_t)(
                sizeof(appliance_health_api_method_body) - 1U));
        return APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED;
    }
    if (!appliance_health_api_build_success(
            snapshot,
            body,
            sizeof(body),
            &body_length)) {
        return APPLIANCE_HEALTH_API_OVERFLOW;
    }

    memset(result_out, 0, sizeof(*result_out));
    memcpy(result_out->body, body, body_length);
    result_out->version = APPLIANCE_HEALTH_API_VERSION;
    result_out->published = true;
    result_out->response.status = HTTP_STATUS_200_OK;
    result_out->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result_out->response.body = result_out->body;
    result_out->response.body_length = (uint32_t)body_length;
    return APPLIANCE_HEALTH_API_OK;
}

const char *appliance_health_api_status_string(
    enum appliance_health_api_status status)
{
    switch (status) {
    case APPLIANCE_HEALTH_API_OK:
        return "APPLIANCE_HEALTH_API_OK";
    case APPLIANCE_HEALTH_API_NULL:
        return "APPLIANCE_HEALTH_API_NULL";
    case APPLIANCE_HEALTH_API_NOT_HEALTH:
        return "APPLIANCE_HEALTH_API_NOT_HEALTH";
    case APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_HEALTH_API_METHOD_NOT_ALLOWED";
    case APPLIANCE_HEALTH_API_BAD_STATE:
        return "APPLIANCE_HEALTH_API_BAD_STATE";
    case APPLIANCE_HEALTH_API_BAD_VERSION:
        return "APPLIANCE_HEALTH_API_BAD_VERSION";
    case APPLIANCE_HEALTH_API_BAD_HEALTH:
        return "APPLIANCE_HEALTH_API_BAD_HEALTH";
    case APPLIANCE_HEALTH_API_OVERFLOW:
        return "APPLIANCE_HEALTH_API_OVERFLOW";
    default:
        return NULL;
    }
}
