#include "https_handler.h"
#include "appliance_management_web_ui.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_HANDLER_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static const uint8_t https_handler_health_body[] =
    "{\"status\":\"ok\",\"route\":\"health\"}";

static bool https_handler_route_valid(enum http_route_id route)
{
    return route == HTTP_ROUTE_ROOT ||
           route == HTTP_ROUTE_HEALTH;
}

static bool https_handler_method_allowed(enum http_method_kind method)
{
    return method == HTTP_METHOD_GET ||
           method == HTTP_METHOD_HEAD;
}

static bool https_handler_result_valid(
    const struct https_handler_result *result)
{
    const struct http_response *response;

    if (result == NULL ||
        result->version != HTTPS_HANDLER_VERSION ||
        !https_handler_route_valid(result->route) ||
        !https_handler_method_allowed(result->method)) {
        return false;
    }
    response = &result->response;
    if (response->status != HTTP_STATUS_200_OK ||
        (result->route == HTTP_ROUTE_ROOT &&
         response->body_length >
             HTTPS_HANDLER_ROOT_BODY_MAX) ||
        (result->route == HTTP_ROUTE_HEALTH &&
         response->body_length >
             HTTPS_HANDLER_HEALTH_BODY_MAX)) {
        return false;
    }
    if (result->route == HTTP_ROUTE_ROOT) {
        const uint8_t *root_body =
            appliance_management_web_ui_shell_body();
        const uint32_t root_body_length =
            appliance_management_web_ui_shell_length();

        return response->content_type ==
                   HTTP_CONTENT_TYPE_HTML_UTF8 &&
               response->body == root_body &&
               response->body_length ==
                   root_body_length;
    }
    return response->content_type == HTTP_CONTENT_TYPE_JSON &&
           response->body == https_handler_health_body &&
           response->body_length ==
               (uint32_t)(sizeof(https_handler_health_body) - 1U);
}

enum https_handler_status https_handler_dispatch(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct https_handler_result *result_out)
{
    struct http_framing_result framing;
    struct http_policy_result policy;
    struct http_request_line line;
    enum http_route_id route = HTTP_ROUTE_NONE;
    enum http_parser_status parser_status;
    enum http_route_status route_status;

    if (parser == NULL || token == NULL || result_out == NULL) {
        return HTTPS_HANDLER_NULL;
    }
    memset(result_out, 0, sizeof(*result_out));
    if (!http_parser_published(parser)) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    if (http_parser_state(parser, token) !=
        HTTP_PARSER_STATE_COMPLETE) {
        return HTTPS_HANDLER_NOT_COMPLETE;
    }

    memset(&framing, 0, sizeof(framing));
    parser_status = http_framing_check(
        parser, token, &framing);
    if (parser_status != HTTP_PARSER_OK) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    if (framing.body_denied || framing.upgrade_denied) {
        return HTTPS_HANDLER_FRAMING_DENIED;
    }

    memset(&policy, 0, sizeof(policy));
    parser_status = http_policy_check(
        parser, token, &policy);
    if (parser_status != HTTP_PARSER_OK) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    if (!policy.allowed ||
        !https_handler_method_allowed(policy.method)) {
        return HTTPS_HANDLER_POLICY_DENIED;
    }

    parser_status = http_request_line_get(
        parser, token, &line);
    if (parser_status != HTTP_PARSER_OK) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    route_status = http_route_match_line(&line, &route);
    if (route_status != HTTP_ROUTE_OK ||
        !https_handler_route_valid(route)) {
        return HTTPS_HANDLER_UNROUTED;
    }

    result_out->version = HTTPS_HANDLER_VERSION;
    result_out->route = route;
    result_out->method = policy.method;
    result_out->response.status = HTTP_STATUS_200_OK;
    if (route == HTTP_ROUTE_ROOT) {
        result_out->response.content_type =
            HTTP_CONTENT_TYPE_HTML_UTF8;
        result_out->response.body =
            appliance_management_web_ui_shell_body();
        result_out->response.body_length =
            appliance_management_web_ui_shell_length();
    } else {
        result_out->response.content_type =
            HTTP_CONTENT_TYPE_JSON;
        result_out->response.body = https_handler_health_body;
        result_out->response.body_length =
            (uint32_t)(sizeof(https_handler_health_body) - 1U);
    }
    return HTTPS_HANDLER_OK;
}

enum https_handler_status https_handler_encode(
    const struct https_handler_result *result,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written)
{
    enum http_response_status response_status;
    uint32_t written_temp = 0U;

    if (result == NULL || output == NULL || written == NULL) {
        return HTTPS_HANDLER_NULL;
    }
    if (result->version != HTTPS_HANDLER_VERSION) {
        return HTTPS_HANDLER_BAD_VERSION;
    }
    if (!https_handler_result_valid(result)) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    if (result->method == HTTP_METHOD_HEAD) {
        response_status = http_response_encode_head(
            &result->response,
            output,
            output_capacity,
            &written_temp);
    } else {
        response_status = http_response_encode(
            &result->response,
            output,
            output_capacity,
            &written_temp);
    }
    if (response_status == HTTP_RESPONSE_BUFFER_TOO_SMALL) {
        return HTTPS_HANDLER_RESPONSE_OVERFLOW;
    }
    if (response_status != HTTP_RESPONSE_OK) {
        return HTTPS_HANDLER_BAD_STATE;
    }
    *written = written_temp;
    return HTTPS_HANDLER_OK;
}

const char *https_handler_status_string(
    enum https_handler_status status)
{
    switch (status) {
    case HTTPS_HANDLER_OK:
        return "HTTPS_HANDLER_OK";
    case HTTPS_HANDLER_NULL:
        return "HTTPS_HANDLER_NULL";
    case HTTPS_HANDLER_BAD_STATE:
        return "HTTPS_HANDLER_BAD_STATE";
    case HTTPS_HANDLER_BAD_VERSION:
        return "HTTPS_HANDLER_BAD_VERSION";
    case HTTPS_HANDLER_NOT_COMPLETE:
        return "HTTPS_HANDLER_NOT_COMPLETE";
    case HTTPS_HANDLER_FRAMING_DENIED:
        return "HTTPS_HANDLER_FRAMING_DENIED";
    case HTTPS_HANDLER_POLICY_DENIED:
        return "HTTPS_HANDLER_POLICY_DENIED";
    case HTTPS_HANDLER_UNROUTED:
        return "HTTPS_HANDLER_UNROUTED";
    case HTTPS_HANDLER_RESPONSE_OVERFLOW:
        return "HTTPS_HANDLER_RESPONSE_OVERFLOW";
    default:
        return NULL;
    }
}

#else

typedef int https_handler_translation_unit_guard;

#endif
