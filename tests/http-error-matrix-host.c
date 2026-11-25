#include "http_error_matrix.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_ERROR_MATRIX_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const enum http_parser_status syntax_400[] = {
    HTTP_PARSER_REQUEST_LINE_INCOMPLETE,
    HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
    HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
    HTTP_PARSER_REQUEST_LINE_INVALID_TARGET,
    HTTP_PARSER_REQUEST_LINE_INVALID_VERSION,
    HTTP_PARSER_HEADER_INCOMPLETE,
    HTTP_PARSER_HEADER_INVALID_NAME,
    HTTP_PARSER_HEADER_INVALID_VALUE,
    HTTP_PARSER_HEADER_FOLDING,
    HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION,
};

static const enum http_parser_status internal_parser_statuses[] = {
    HTTP_PARSER_OK,
    HTTP_PARSER_NULL,
    HTTP_PARSER_BAD_STATE,
    HTTP_PARSER_BAD_TOKEN,
    HTTP_PARSER_STALE_TOKEN,
    HTTP_PARSER_NOT_ATTACHED,
    HTTP_PARSER_ALREADY_ATTACHED,
    HTTP_PARSER_INVALID_PREDECESSOR,
    HTTP_PARSER_INVALID_PROGRESS,
    HTTP_PARSER_TERMINAL,
    HTTP_PARSER_HEADER_OUT_OF_RANGE,
};

static void test_status_strings(void)
{
    CHECK(http_error_matrix_status_string(
              HTTP_ERROR_MATRIX_OK) != NULL &&
              strcmp(
                  http_error_matrix_status_string(
                      HTTP_ERROR_MATRIX_OK),
                  "HTTP_ERROR_MATRIX_OK") == 0,
          "matrix OK status string is exact");
    CHECK(http_error_matrix_status_string(
              HTTP_ERROR_MATRIX_NULL) != NULL &&
              strcmp(
                  http_error_matrix_status_string(
                      HTTP_ERROR_MATRIX_NULL),
                  "HTTP_ERROR_MATRIX_NULL") == 0,
          "matrix NULL status string is exact");
    CHECK(http_error_matrix_status_string(
              HTTP_ERROR_MATRIX_NOT_ERROR) != NULL &&
              strcmp(
                  http_error_matrix_status_string(
                      HTTP_ERROR_MATRIX_NOT_ERROR),
                  "HTTP_ERROR_MATRIX_NOT_ERROR") == 0,
          "matrix not-error status string is exact");
    CHECK(http_error_matrix_status_string(
              HTTP_ERROR_MATRIX_UNMAPPED) != NULL &&
              strcmp(
                  http_error_matrix_status_string(
                      HTTP_ERROR_MATRIX_UNMAPPED),
                  "HTTP_ERROR_MATRIX_UNMAPPED") == 0,
          "matrix unmapped status string is exact");
    CHECK(http_error_matrix_status_string(
              (enum http_error_matrix_status)99) == NULL,
          "unknown matrix status string is null");
}

static void test_parser_mapping(void)
{
    enum http_status_code status;
    uint32_t index;

    for (index = 0U;
         index < sizeof(syntax_400) / sizeof(syntax_400[0]);
         ++index) {
        status = HTTP_STATUS_404_NOT_FOUND;
        CHECK(http_error_matrix_map_parser(
                  syntax_400[index], &status) ==
                  HTTP_ERROR_MATRIX_OK &&
                  status == HTTP_STATUS_400_BAD_REQUEST,
              "malformed parser syntax maps to 400");
    }

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_REQUEST_LINE_LIMIT, &status) ==
              HTTP_ERROR_MATRIX_OK &&
              status == HTTP_STATUS_414_URI_TOO_LONG,
          "request-line overflow maps to 414");

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_HEADERS_LIMIT, &status) ==
              HTTP_ERROR_MATRIX_OK &&
              status == HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE,
          "header overflow maps to 431");

    for (index = 0U;
         index < sizeof(internal_parser_statuses) /
                     sizeof(internal_parser_statuses[0]);
         ++index) {
        status = HTTP_STATUS_404_NOT_FOUND;
        CHECK(http_error_matrix_map_parser(
                  internal_parser_statuses[index], &status) ==
                  HTTP_ERROR_MATRIX_UNMAPPED &&
                  status == HTTP_STATUS_404_NOT_FOUND,
              "internal parser status is unmapped without mutation");
    }

    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_BAD_STATE, NULL) ==
              HTTP_ERROR_MATRIX_NULL,
          "parser mapping rejects null status output");
}

static void test_policy_mapping(void)
{
    static const enum http_policy_reason bad_request_reasons[] = {
        HTTP_POLICY_VERSION,
        HTTP_POLICY_MISSING_HOST,
        HTTP_POLICY_EMPTY_HOST,
        HTTP_POLICY_DUPLICATE_HOST,
        HTTP_POLICY_INVALID_HOST,
    };
    enum http_status_code status;
    uint32_t index;

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_policy(
              HTTP_POLICY_METHOD, &status) ==
              HTTP_ERROR_MATRIX_OK &&
              status == HTTP_STATUS_405_METHOD_NOT_ALLOWED,
          "unsupported method maps to 405");

    for (index = 0U;
         index < sizeof(bad_request_reasons) /
                     sizeof(bad_request_reasons[0]);
         ++index) {
        status = HTTP_STATUS_404_NOT_FOUND;
        CHECK(http_error_matrix_map_policy(
                  bad_request_reasons[index], &status) ==
                  HTTP_ERROR_MATRIX_OK &&
                  status == HTTP_STATUS_400_BAD_REQUEST,
              "invalid version or Host policy maps to 400");
    }

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_policy(
              HTTP_POLICY_NONE, &status) ==
              HTTP_ERROR_MATRIX_NOT_ERROR &&
              status == HTTP_STATUS_404_NOT_FOUND,
          "policy none is not an error without output mutation");
    CHECK(http_error_matrix_map_policy(
              HTTP_POLICY_METHOD, NULL) ==
              HTTP_ERROR_MATRIX_NULL,
          "policy mapping rejects null status output");
}

static void make_framing_result(
    enum http_framing_kind reason,
    struct http_framing_result *result)
{
    memset(result, 0, sizeof(*result));
    result->reason = reason;
    switch (reason) {
    case HTTP_FRAMING_UPGRADE:
        result->upgrade_denied = true;
        break;
    case HTTP_FRAMING_NONE:
        break;
    default:
        result->body_denied = true;
        break;
    }
}

static void test_framing_mapping(void)
{
    static const enum http_framing_kind denied_kinds[] = {
        HTTP_FRAMING_CONTENT_LENGTH,
        HTTP_FRAMING_TRANSFER_ENCODING,
        HTTP_FRAMING_EXPECT,
        HTTP_FRAMING_UPGRADE,
        HTTP_FRAMING_DUPLICATE,
        HTTP_FRAMING_CONFLICT,
        HTTP_FRAMING_INVALID,
    };
    struct http_framing_result framing;
    enum http_status_code status;
    uint32_t index;

    for (index = 0U;
         index < sizeof(denied_kinds) / sizeof(denied_kinds[0]);
         ++index) {
        make_framing_result(denied_kinds[index], &framing);
        status = HTTP_STATUS_404_NOT_FOUND;
        CHECK(http_error_matrix_map_framing(
                  &framing, &status) == HTTP_ERROR_MATRIX_OK &&
                  status == HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
              "body or upgrade framing denial maps to 413");
    }

    make_framing_result(HTTP_FRAMING_NONE, &framing);
    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_framing(
              &framing, &status) ==
              HTTP_ERROR_MATRIX_NOT_ERROR &&
              status == HTTP_STATUS_404_NOT_FOUND,
          "framing none is not an error without output mutation");

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_framing(
              NULL, &status) == HTTP_ERROR_MATRIX_NULL &&
              status == HTTP_STATUS_404_NOT_FOUND,
          "framing mapping rejects null result without mutation");
    CHECK(http_error_matrix_map_framing(
              &framing, NULL) == HTTP_ERROR_MATRIX_NULL,
          "framing mapping rejects null status output");
}

static void test_route_mapping(void)
{
    enum http_status_code status;

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_route(
              HTTP_ROUTE_NONE, &status) ==
              HTTP_ERROR_MATRIX_OK &&
              status == HTTP_STATUS_404_NOT_FOUND,
          "unknown route maps to 404");

    status = HTTP_STATUS_413_PAYLOAD_TOO_LARGE;
    CHECK(http_error_matrix_map_route(
              HTTP_ROUTE_ROOT, &status) ==
              HTTP_ERROR_MATRIX_NOT_ERROR &&
              status == HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
          "root route is not an error without output mutation");
    status = HTTP_STATUS_413_PAYLOAD_TOO_LARGE;
    CHECK(http_error_matrix_map_route(
              HTTP_ROUTE_HEALTH, &status) ==
              HTTP_ERROR_MATRIX_NOT_ERROR &&
              status == HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
          "health route is not an error without output mutation");

    status = HTTP_STATUS_404_NOT_FOUND;
    CHECK(http_error_matrix_map_route(
              (enum http_route_id)99, &status) ==
              HTTP_ERROR_MATRIX_UNMAPPED &&
              status == HTTP_STATUS_404_NOT_FOUND,
          "invalid route is unmapped without mutation");
    CHECK(http_error_matrix_map_route(
              HTTP_ROUTE_NONE, NULL) ==
              HTTP_ERROR_MATRIX_NULL,
          "route mapping rejects null status output");
}

static void test_deterministic_repeat(void)
{
    enum http_status_code first = HTTP_STATUS_404_NOT_FOUND;
    enum http_status_code second = HTTP_STATUS_404_NOT_FOUND;

    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_REQUEST_LINE_INVALID_TARGET, &first) ==
              HTTP_ERROR_MATRIX_OK &&
              http_error_matrix_map_parser(
                  HTTP_PARSER_REQUEST_LINE_INVALID_TARGET, &second) ==
                  HTTP_ERROR_MATRIX_OK &&
              first == HTTP_STATUS_400_BAD_REQUEST &&
              second == HTTP_STATUS_400_BAD_REQUEST,
          "parser mapping is deterministic across repeated calls");
}

/*
 * ITEM-498: the body of a refusal is valid JSON naming its status and reason,
 * the browser targets get a readable HTML document, and a buffer that cannot
 * hold the body is refused instead of truncated.
 */
static void test_refusal_bodies(void)
{
    static const struct {
        enum http_status_code status;
        const char *reason;
    } reasons[] = {
        { HTTP_STATUS_400_BAD_REQUEST, "bad_request" },
        { HTTP_STATUS_401_UNAUTHORIZED, "unauthorized" },
        { HTTP_STATUS_403_FORBIDDEN, "forbidden" },
        { HTTP_STATUS_404_NOT_FOUND, "not_found" },
        { HTTP_STATUS_405_METHOD_NOT_ALLOWED, "method_not_allowed" },
        { HTTP_STATUS_413_PAYLOAD_TOO_LARGE, "payload_too_large" },
        { HTTP_STATUS_503_SERVICE_UNAVAILABLE, "unavailable" },
    };
    uint8_t body[HTTP_ERROR_BODY_MAX];
    char expected[64];
    size_t index;

    for (index = 0U; index < sizeof(reasons) / sizeof(reasons[0]); ++index) {
        size_t length = http_error_matrix_json_body(
            reasons[index].status, body, sizeof(body));

        CHECK(length != 0U, "the JSON refusal body is built");
        snprintf(expected, sizeof(expected), "{\"status\":%d,\"error\":\"%s\"}",
                 (int)reasons[index].status, reasons[index].reason);
        CHECK(strcmp((const char *)body, expected) == 0,
              "the JSON refusal body names its status and reason");
        CHECK(http_error_matrix_reason(reasons[index].status) ==
                  reasons[index].reason ||
                  strcmp(http_error_matrix_reason(reasons[index].status),
                         reasons[index].reason) == 0,
              "the reason word is the published one");
        length = http_error_matrix_html_body(
            reasons[index].status, body, sizeof(body));
        CHECK(length != 0U, "the HTML refusal body is built");
        CHECK(strstr((const char *)body, "<!doctype html>") ==
                  (char *)body,
              "the HTML refusal body is an HTML document");
        snprintf(expected, sizeof(expected), "%d ",
                 (int)reasons[index].status);
        CHECK(strstr((const char *)body, expected) != NULL,
              "the HTML refusal body names its status");
        CHECK(strstr((const char *)body, "href=\"/panel\"") != NULL,
              "the HTML refusal body offers the sign-in page");
    }
    /* A buffer that cannot hold the body is refused, never truncated. */
    CHECK(http_error_matrix_json_body(HTTP_STATUS_404_NOT_FOUND, body, 8U) ==
              0U,
          "a JSON body that does not fit is refused");
    CHECK(http_error_matrix_html_body(HTTP_STATUS_404_NOT_FOUND, body, 8U) ==
              0U,
          "an HTML body that does not fit is refused");

    /* The family decides the shape: the browser targets, and only they. */
    CHECK(http_error_matrix_body_family((const uint8_t *)"/", 1U) ==
              HTTP_ERROR_BODY_HTML,
          "/ is a browser target");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/health", 7U) ==
              HTTP_ERROR_BODY_HTML,
          "/health is a browser target");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/panel", 6U) ==
              HTTP_ERROR_BODY_HTML,
          "/panel is a browser target");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/panel/network",
                                        14U) == HTTP_ERROR_BODY_HTML,
          "a panel section is a browser target");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/api/v1/system/health",
                                        20U) == HTTP_ERROR_BODY_JSON,
          "an API target gets JSON");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/nope", 5U) ==
              HTTP_ERROR_BODY_JSON,
          "an unknown target gets JSON");
    CHECK(http_error_matrix_body_family((const uint8_t *)"/panelx", 7U) ==
              HTTP_ERROR_BODY_JSON,
          "a target that only starts like the panel is not one");
    CHECK(http_error_matrix_body_family(NULL, 0U) == HTTP_ERROR_BODY_JSON,
          "a refusal with no parsed target gets JSON");
}

int main(void)
{
    test_status_strings();
    test_parser_mapping();
    test_policy_mapping();
    test_framing_mapping();
    test_route_mapping();
    test_deterministic_repeat();
    test_refusal_bodies();

    if (failures != 0) {
        fprintf(stderr, "HTTP error matrix fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP error matrix host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_error_matrix_unused;

#endif
