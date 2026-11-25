#ifndef HFOC_HTTPS_HANDLER_H
#define HFOC_HTTPS_HANDLER_H

#include "http_parser.h"
#include "http_response.h"

#include <stdint.h>

#define HTTPS_HANDLER_VERSION 1U
/* ITEM-384: the root target serves the rendered panel document. */
#define HTTPS_HANDLER_ROOT_BODY_MAX 2048U
#define HTTPS_HANDLER_HEALTH_BODY_MAX 128U

enum https_handler_status {
    HTTPS_HANDLER_OK = 0,
    HTTPS_HANDLER_NULL = 1,
    HTTPS_HANDLER_BAD_STATE = 2,
    HTTPS_HANDLER_BAD_VERSION = 3,
    HTTPS_HANDLER_NOT_COMPLETE = 4,
    HTTPS_HANDLER_FRAMING_DENIED = 5,
    HTTPS_HANDLER_POLICY_DENIED = 6,
    HTTPS_HANDLER_UNROUTED = 7,
    HTTPS_HANDLER_RESPONSE_OVERFLOW = 8,
};

struct https_handler_result {
    uint32_t version;
    enum http_route_id route;
    enum http_method_kind method;
    struct http_response response;
};

enum https_handler_status https_handler_dispatch(
    const struct http_parser *parser,
    const struct http_parser_token *token,
    struct https_handler_result *result_out);
enum https_handler_status https_handler_encode(
    const struct https_handler_result *result,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written);
const char *https_handler_status_string(
    enum https_handler_status status);

#endif
