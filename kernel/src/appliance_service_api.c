#include "appliance_service_api.h"

#include "appliance_service_manager.h"
#include "health_check.h"
#include "http_parser.h"
#include "http_response.h"
#include "service_health.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const uint8_t appliance_service_api_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"service\","
    "\"error\":\"method_not_allowed\",\"auth\":\"unavailable\","
    "\"mutating\":false}";

static bool appliance_service_api_state_valid(
    enum appliance_service_manager_state state)
{
    return state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED ||
           state == APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED;
}

static bool appliance_service_api_policy_valid(
    enum appliance_service_manager_startup_policy policy)
{
    return policy == APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK ||
           policy == APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE;
}

static bool appliance_service_api_health_valid(
    enum health_check_result_state result)
{
    return result == HEALTH_CHECK_RESULT_UNKNOWN ||
           result == HEALTH_CHECK_RESULT_PASS ||
           result == HEALTH_CHECK_RESULT_WARN ||
           result == HEALTH_CHECK_RESULT_FAIL;
}

static bool appliance_service_api_fault_valid(
    enum service_health_fault fault)
{
    return fault == SERVICE_HEALTH_FAULT_NONE ||
           fault == SERVICE_HEALTH_FAULT_READINESS ||
           fault == SERVICE_HEALTH_FAULT_RESTART ||
           fault == SERVICE_HEALTH_FAULT_DEPENDENCY;
}

static bool appliance_service_api_health_coherent(
    enum health_check_result_state result,
    enum service_health_fault fault)
{
    if (result == HEALTH_CHECK_RESULT_PASS ||
        result == HEALTH_CHECK_RESULT_UNKNOWN) {
        return fault == SERVICE_HEALTH_FAULT_NONE;
    }
    if (result == HEALTH_CHECK_RESULT_WARN) {
        return fault == SERVICE_HEALTH_FAULT_DEPENDENCY;
    }
    return fault == SERVICE_HEALTH_FAULT_READINESS ||
           fault == SERVICE_HEALTH_FAULT_RESTART;
}

static bool appliance_service_api_manager_valid(
    const struct appliance_service_manager_snapshot *manager)
{
    uint32_t active_count;

    if (manager == NULL ||
        !manager->published ||
        manager->version != APPLIANCE_SERVICE_MANAGER_VERSION ||
        !appliance_service_api_state_valid(manager->state) ||
        !appliance_service_api_policy_valid(manager->startup_policy) ||
        manager->configured_count > APPLIANCE_SERVICE_MANAGER_CAPACITY ||
        manager->active_count > SERVICE_SUPERVISOR_CAPACITY ||
        manager->ready_count > manager->active_count ||
        manager->pending_count > manager->active_count ||
        manager->failed_count > manager->active_count ||
        manager->exhausted_count > manager->failed_count ||
        !appliance_service_api_health_valid(manager->health_result) ||
        !appliance_service_api_fault_valid(manager->health_fault) ||
        !appliance_service_api_health_coherent(
            manager->health_result,
            manager->health_fault)) {
        return false;
    }
    if (manager->active_count !=
        manager->ready_count +
            manager->pending_count +
            manager->failed_count) {
        return false;
    }
    active_count = manager->active_count;
    if (manager->failed_count > 0U &&
        manager->health_result != HEALTH_CHECK_RESULT_FAIL) {
        return false;
    }
    if (manager->failed_count == 0U &&
        manager->pending_count == 0U &&
        active_count > 0U &&
        manager->ready_count == active_count &&
        manager->health_result == HEALTH_CHECK_RESULT_UNKNOWN) {
        return false;
    }
    return true;
}

static bool appliance_service_api_exact_equal(
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

static bool appliance_service_api_append_bytes(
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

static bool appliance_service_api_append_literal(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *literal)
{
    return appliance_service_api_append_bytes(
        output,
        capacity,
        offset,
        literal,
        strlen(literal));
}

static bool appliance_service_api_append_u64(
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
        if (!appliance_service_api_append_bytes(
                output,
                capacity,
                offset,
                &digits[count],
                1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_service_api_append_json_key(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key)
{
    return appliance_service_api_append_literal(
               output, capacity, offset, ",") &&
           appliance_service_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_service_api_append_literal(
               output, capacity, offset, key) &&
           appliance_service_api_append_literal(
               output, capacity, offset, "\":");
}

static bool appliance_service_api_append_json_string_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    const char *value)
{
    return appliance_service_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_service_api_append_literal(
               output, capacity, offset, "\"") &&
           appliance_service_api_append_literal(
               output, capacity, offset, value) &&
           appliance_service_api_append_literal(
               output, capacity, offset, "\"");
}

static bool appliance_service_api_append_json_u64_field(
    uint8_t *output,
    size_t capacity,
    size_t *offset,
    const char *key,
    uint64_t value)
{
    return appliance_service_api_append_json_key(
               output, capacity, offset, key) &&
           appliance_service_api_append_u64(
               output, capacity, offset, value);
}

static const char *appliance_service_api_state_json(
    enum appliance_service_manager_state state)
{
    switch (state) {
    case APPLIANCE_SERVICE_MANAGER_STATE_STOPPED:
        return "stopped";
    case APPLIANCE_SERVICE_MANAGER_STATE_RUNNING:
        return "running";
    case APPLIANCE_SERVICE_MANAGER_STATE_START_FAILED:
        return "start_failed";
    case APPLIANCE_SERVICE_MANAGER_STATE_STOP_FAILED:
        return "stop_failed";
    default:
        return NULL;
    }
}

static const char *appliance_service_api_policy_json(
    enum appliance_service_manager_startup_policy policy)
{
    switch (policy) {
    case APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK:
        return "rollback";
    case APPLIANCE_SERVICE_MANAGER_STARTUP_PRESERVE:
        return "preserve";
    default:
        return NULL;
    }
}

static const char *appliance_service_api_health_json(
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

static const char *appliance_service_api_fault_json(
    enum service_health_fault fault)
{
    switch (fault) {
    case SERVICE_HEALTH_FAULT_NONE:
        return "none";
    case SERVICE_HEALTH_FAULT_READINESS:
        return "readiness";
    case SERVICE_HEALTH_FAULT_RESTART:
        return "restart";
    case SERVICE_HEALTH_FAULT_DEPENDENCY:
        return "dependency";
    default:
        return NULL;
    }
}

static bool appliance_service_api_build_success(
    const struct appliance_service_manager_snapshot *manager,
    uint8_t *output,
    size_t capacity,
    size_t *output_length)
{
    const char *state =
        appliance_service_api_state_json(manager->state);
    const char *policy =
        appliance_service_api_policy_json(manager->startup_policy);
    const char *health =
        appliance_service_api_health_json(manager->health_result);
    const char *fault =
        appliance_service_api_fault_json(manager->health_fault);
    size_t offset = 0U;

    if (state == NULL || policy == NULL ||
        health == NULL || fault == NULL) {
        return false;
    }
    if (!appliance_service_api_append_literal(
            output,
            capacity,
            &offset,
            "{\"api\":\"hfoc\",\"version\":1,\"name\":\"service\","
            "\"status\":\"ok\",\"auth\":\"unavailable\","
            "\"mutating\":false") ||
        !appliance_service_api_append_json_string_field(
            output, capacity, &offset, "state", state) ||
        !appliance_service_api_append_json_string_field(
            output, capacity, &offset, "policy", policy) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "generation",
            manager->generation) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "configured_count",
            manager->configured_count) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "active_count",
            manager->active_count) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "ready_count",
            manager->ready_count) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "pending_count",
            manager->pending_count) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "failed_count",
            manager->failed_count) ||
        !appliance_service_api_append_json_u64_field(
            output, capacity, &offset, "exhausted_count",
            manager->exhausted_count) ||
        !appliance_service_api_append_json_string_field(
            output, capacity, &offset, "health", health) ||
        !appliance_service_api_append_json_string_field(
            output, capacity, &offset, "fault", fault) ||
        !appliance_service_api_append_literal(
            output, capacity, &offset, "}")) {
        return false;
    }
    *output_length = offset;
    return true;
}

static void appliance_service_api_publish_static(
    struct appliance_service_api_result *result,
    enum http_status_code status,
    const uint8_t *body,
    uint32_t body_length)
{
    memset(result, 0, sizeof(*result));
    result->version = APPLIANCE_SERVICE_API_VERSION;
    result->published = true;
    result->route = APPLIANCE_SERVICE_API_ROUTE_STATUS;
    result->response.status = status;
    result->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result->response.body = body;
    result->response.body_length = body_length;
}

enum appliance_service_api_status appliance_service_api_snapshot_init(
    struct appliance_service_api_snapshot *snapshot,
    const struct appliance_service_manager_snapshot *manager)
{
    if (snapshot == NULL || manager == NULL) {
        return APPLIANCE_SERVICE_API_NULL;
    }
    if (snapshot->published) {
        return APPLIANCE_SERVICE_API_BAD_STATE;
    }
    if (!appliance_service_api_manager_valid(manager)) {
        return APPLIANCE_SERVICE_API_BAD_MANAGER;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    snapshot->version = APPLIANCE_SERVICE_API_VERSION;
    snapshot->manager = *manager;
    snapshot->published = true;
    return APPLIANCE_SERVICE_API_OK;
}

enum appliance_service_api_status appliance_service_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    const struct appliance_service_api_snapshot *snapshot,
    struct appliance_service_api_result *result_out)
{
    uint8_t body[APPLIANCE_SERVICE_API_BODY_MAX];
    size_t body_length = 0U;

    if (target == NULL || snapshot == NULL || result_out == NULL) {
        return APPLIANCE_SERVICE_API_NULL;
    }
    if (!appliance_service_api_exact_equal(
            target,
            target_length,
            APPLIANCE_SERVICE_API_PATH,
            APPLIANCE_SERVICE_API_PATH_LENGTH)) {
        return APPLIANCE_SERVICE_API_NOT_SERVICE;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_SERVICE_API_VERSION ||
        !appliance_service_api_manager_valid(&snapshot->manager)) {
        return APPLIANCE_SERVICE_API_BAD_STATE;
    }
    if (method != HTTP_METHOD_GET &&
        method != HTTP_METHOD_HEAD) {
        appliance_service_api_publish_static(
            result_out,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            appliance_service_api_method_body,
            (uint32_t)(
                sizeof(appliance_service_api_method_body) - 1U));
        return APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED;
    }
    if (!appliance_service_api_build_success(
            &snapshot->manager,
            body,
            sizeof(body),
            &body_length)) {
        return APPLIANCE_SERVICE_API_OVERFLOW;
    }

    memset(result_out, 0, sizeof(*result_out));
    memcpy(result_out->body, body, body_length);
    result_out->version = APPLIANCE_SERVICE_API_VERSION;
    result_out->published = true;
    result_out->route = APPLIANCE_SERVICE_API_ROUTE_STATUS;
    result_out->response.status = HTTP_STATUS_200_OK;
    result_out->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result_out->response.body = result_out->body;
    result_out->response.body_length = (uint32_t)body_length;
    return APPLIANCE_SERVICE_API_OK;
}

const char *appliance_service_api_status_string(
    enum appliance_service_api_status status)
{
    switch (status) {
    case APPLIANCE_SERVICE_API_OK:
        return "APPLIANCE_SERVICE_API_OK";
    case APPLIANCE_SERVICE_API_NULL:
        return "APPLIANCE_SERVICE_API_NULL";
    case APPLIANCE_SERVICE_API_NOT_SERVICE:
        return "APPLIANCE_SERVICE_API_NOT_SERVICE";
    case APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_SERVICE_API_METHOD_NOT_ALLOWED";
    case APPLIANCE_SERVICE_API_BAD_STATE:
        return "APPLIANCE_SERVICE_API_BAD_STATE";
    case APPLIANCE_SERVICE_API_BAD_VERSION:
        return "APPLIANCE_SERVICE_API_BAD_VERSION";
    case APPLIANCE_SERVICE_API_BAD_MANAGER:
        return "APPLIANCE_SERVICE_API_BAD_MANAGER";
    case APPLIANCE_SERVICE_API_OVERFLOW:
        return "APPLIANCE_SERVICE_API_OVERFLOW";
    default:
        return NULL;
    }
}

const char *appliance_service_api_route_string(
    enum appliance_service_api_route route)
{
    switch (route) {
    case APPLIANCE_SERVICE_API_ROUTE_NONE:
        return "APPLIANCE_SERVICE_API_ROUTE_NONE";
    case APPLIANCE_SERVICE_API_ROUTE_STATUS:
        return "APPLIANCE_SERVICE_API_ROUTE_STATUS";
    default:
        return NULL;
    }
}
