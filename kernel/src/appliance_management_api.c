#include "appliance_management_api.h"

#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t appliance_management_api_version_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"name\":\"management\","
    "\"status\":\"ok\",\"auth\":\"unavailable\",\"mutating\":false}";
static const uint8_t appliance_management_api_not_found_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"error\":\"not_found\","
    "\"auth\":\"unavailable\",\"mutating\":false}";
static const uint8_t appliance_management_api_method_body[] =
    "{\"api\":\"hfoc\",\"version\":1,\"error\":\"method_not_allowed\","
    "\"auth\":\"unavailable\",\"mutating\":false}";

static bool appliance_management_api_exact_equal(
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

static bool appliance_management_api_has_prefix(
    const uint8_t *target,
    uint32_t target_length)
{
    if (target_length < APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH) {
        return false;
    }
    if (!appliance_management_api_exact_equal(
            target,
            APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH,
            APPLIANCE_MANAGEMENT_API_PREFIX,
            APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH)) {
        return false;
    }
    return target_length == APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH ||
           target[APPLIANCE_MANAGEMENT_API_PREFIX_LENGTH] == '/';
}

static void appliance_management_api_publish(
    struct appliance_management_api_result *result,
    enum appliance_management_api_route route,
    enum http_status_code http_status,
    const uint8_t *body,
    uint32_t body_length)
{
    result->version = APPLIANCE_MANAGEMENT_API_VERSION;
    result->published = true;
    result->route = route;
    result->response.status = http_status;
    result->response.content_type = HTTP_CONTENT_TYPE_JSON;
    result->response.body = body;
    result->response.body_length = body_length;
}

enum appliance_management_api_status appliance_management_api_dispatch(
    enum http_method_kind method,
    const uint8_t *target,
    uint32_t target_length,
    struct appliance_management_api_result *result_out)
{
    bool version_route;

    if (target == NULL || result_out == NULL) {
        return APPLIANCE_MANAGEMENT_API_NULL;
    }
    if (!appliance_management_api_has_prefix(target, target_length)) {
        return APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT;
    }

    version_route = appliance_management_api_exact_equal(
        target,
        target_length,
        APPLIANCE_MANAGEMENT_API_PATH,
        APPLIANCE_MANAGEMENT_API_PATH_LENGTH);
    if (!version_route) {
        appliance_management_api_publish(
            result_out,
            APPLIANCE_MANAGEMENT_API_ROUTE_NONE,
            HTTP_STATUS_404_NOT_FOUND,
            appliance_management_api_not_found_body,
            (uint32_t)(sizeof(appliance_management_api_not_found_body) - 1U));
        return APPLIANCE_MANAGEMENT_API_NOT_FOUND;
    }

    if (method == HTTP_METHOD_GET || method == HTTP_METHOD_HEAD) {
        appliance_management_api_publish(
            result_out,
            APPLIANCE_MANAGEMENT_API_ROUTE_VERSION,
            HTTP_STATUS_200_OK,
            appliance_management_api_version_body,
            (uint32_t)(sizeof(appliance_management_api_version_body) - 1U));
        return APPLIANCE_MANAGEMENT_API_OK;
    }

    appliance_management_api_publish(
        result_out,
        APPLIANCE_MANAGEMENT_API_ROUTE_VERSION,
        HTTP_STATUS_405_METHOD_NOT_ALLOWED,
        appliance_management_api_method_body,
        (uint32_t)(sizeof(appliance_management_api_method_body) - 1U));
    return APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED;
}

const char *appliance_management_api_status_string(
    enum appliance_management_api_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_API_OK:
        return "APPLIANCE_MANAGEMENT_API_OK";
    case APPLIANCE_MANAGEMENT_API_NULL:
        return "APPLIANCE_MANAGEMENT_API_NULL";
    case APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT:
        return "APPLIANCE_MANAGEMENT_API_NOT_MANAGEMENT";
    case APPLIANCE_MANAGEMENT_API_NOT_FOUND:
        return "APPLIANCE_MANAGEMENT_API_NOT_FOUND";
    case APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED:
        return "APPLIANCE_MANAGEMENT_API_METHOD_NOT_ALLOWED";
    default:
        return NULL;
    }
}

const char *appliance_management_api_route_string(
    enum appliance_management_api_route route)
{
    switch (route) {
    case APPLIANCE_MANAGEMENT_API_ROUTE_NONE:
        return "APPLIANCE_MANAGEMENT_API_ROUTE_NONE";
    case APPLIANCE_MANAGEMENT_API_ROUTE_VERSION:
        return "APPLIANCE_MANAGEMENT_API_ROUTE_VERSION";
    default:
        return NULL;
    }
}
