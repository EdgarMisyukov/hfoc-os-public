#include "http_error_matrix.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#if defined(HTTP_ERROR_MATRIX_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static bool http_error_matrix_parser_malformed(
    enum http_parser_status parser_status)
{
    switch (parser_status) {
    case HTTP_PARSER_REQUEST_LINE_INCOMPLETE:
    case HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER:
    case HTTP_PARSER_REQUEST_LINE_INVALID_SPACING:
    case HTTP_PARSER_REQUEST_LINE_INVALID_TARGET:
    case HTTP_PARSER_REQUEST_LINE_INVALID_VERSION:
    case HTTP_PARSER_HEADER_INCOMPLETE:
    case HTTP_PARSER_HEADER_INVALID_NAME:
    case HTTP_PARSER_HEADER_INVALID_VALUE:
    case HTTP_PARSER_HEADER_FOLDING:
    case HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION:
        return true;
    default:
        return false;
    }
}

const char *http_error_matrix_status_string(
    enum http_error_matrix_status status)
{
    switch (status) {
    case HTTP_ERROR_MATRIX_OK:
        return "HTTP_ERROR_MATRIX_OK";
    case HTTP_ERROR_MATRIX_NULL:
        return "HTTP_ERROR_MATRIX_NULL";
    case HTTP_ERROR_MATRIX_NOT_ERROR:
        return "HTTP_ERROR_MATRIX_NOT_ERROR";
    case HTTP_ERROR_MATRIX_UNMAPPED:
        return "HTTP_ERROR_MATRIX_UNMAPPED";
    default:
        return NULL;
    }
}

enum http_error_matrix_status http_error_matrix_map_parser(
    enum http_parser_status parser_status,
    enum http_status_code *http_status)
{
    if (http_status == NULL) {
        return HTTP_ERROR_MATRIX_NULL;
    }
    if (http_error_matrix_parser_malformed(parser_status)) {
        *http_status = HTTP_STATUS_400_BAD_REQUEST;
        return HTTP_ERROR_MATRIX_OK;
    }
    if (parser_status == HTTP_PARSER_REQUEST_LINE_LIMIT) {
        *http_status = HTTP_STATUS_414_URI_TOO_LONG;
        return HTTP_ERROR_MATRIX_OK;
    }
    if (parser_status == HTTP_PARSER_HEADERS_LIMIT) {
        *http_status = HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE;
        return HTTP_ERROR_MATRIX_OK;
    }
    return HTTP_ERROR_MATRIX_UNMAPPED;
}

enum http_error_matrix_status http_error_matrix_map_policy(
    enum http_policy_reason policy_reason,
    enum http_status_code *http_status)
{
    if (http_status == NULL) {
        return HTTP_ERROR_MATRIX_NULL;
    }
    switch (policy_reason) {
    case HTTP_POLICY_METHOD:
        *http_status = HTTP_STATUS_405_METHOD_NOT_ALLOWED;
        return HTTP_ERROR_MATRIX_OK;
    case HTTP_POLICY_VERSION:
    case HTTP_POLICY_MISSING_HOST:
    case HTTP_POLICY_EMPTY_HOST:
    case HTTP_POLICY_DUPLICATE_HOST:
    case HTTP_POLICY_INVALID_HOST:
        *http_status = HTTP_STATUS_400_BAD_REQUEST;
        return HTTP_ERROR_MATRIX_OK;
    case HTTP_POLICY_NONE:
        return HTTP_ERROR_MATRIX_NOT_ERROR;
    default:
        return HTTP_ERROR_MATRIX_UNMAPPED;
    }
}

enum http_error_matrix_status http_error_matrix_map_framing(
    const struct http_framing_result *framing,
    enum http_status_code *http_status)
{
    if (framing == NULL || http_status == NULL) {
        return HTTP_ERROR_MATRIX_NULL;
    }
    switch (framing->reason) {
    case HTTP_FRAMING_NONE:
        return HTTP_ERROR_MATRIX_NOT_ERROR;
    case HTTP_FRAMING_CONTENT_LENGTH:
    case HTTP_FRAMING_TRANSFER_ENCODING:
    case HTTP_FRAMING_EXPECT:
    case HTTP_FRAMING_UPGRADE:
    case HTTP_FRAMING_DUPLICATE:
    case HTTP_FRAMING_CONFLICT:
    case HTTP_FRAMING_INVALID:
        *http_status = HTTP_STATUS_413_PAYLOAD_TOO_LARGE;
        return HTTP_ERROR_MATRIX_OK;
    default:
        return HTTP_ERROR_MATRIX_UNMAPPED;
    }
}

enum http_error_matrix_status http_error_matrix_map_route(
    enum http_route_id route,
    enum http_status_code *http_status)
{
    if (http_status == NULL) {
        return HTTP_ERROR_MATRIX_NULL;
    }
    switch (route) {
    case HTTP_ROUTE_NONE:
        *http_status = HTTP_STATUS_404_NOT_FOUND;
        return HTTP_ERROR_MATRIX_OK;
    case HTTP_ROUTE_ROOT:
    case HTTP_ROUTE_HEALTH:
        return HTTP_ERROR_MATRIX_NOT_ERROR;
    default:
        return HTTP_ERROR_MATRIX_UNMAPPED;
    }
}

/* ------------------------------------------------------------ ITEM-498 --- */

const char *http_error_matrix_reason(enum http_status_code status)
{
    switch (status) {
    case HTTP_STATUS_400_BAD_REQUEST:
        return "bad_request";
    case HTTP_STATUS_401_UNAUTHORIZED:
        return "unauthorized";
    case HTTP_STATUS_403_FORBIDDEN:
        return "forbidden";
    case HTTP_STATUS_404_NOT_FOUND:
        return "not_found";
    case HTTP_STATUS_405_METHOD_NOT_ALLOWED:
        return "method_not_allowed";
    case HTTP_STATUS_413_PAYLOAD_TOO_LARGE:
        return "payload_too_large";
    case HTTP_STATUS_414_URI_TOO_LONG:
        return "uri_too_long";
    case HTTP_STATUS_429_TOO_MANY_REQUESTS:
        return "too_many_requests";
    case HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE:
        return "header_fields_too_large";
    case HTTP_STATUS_503_SERVICE_UNAVAILABLE:
        return "unavailable";
    default:
        return "error";
    }
}

enum http_error_body_family http_error_matrix_body_family(
    const uint8_t *target,
    size_t target_length)
{
    static const char panel[] = "/panel";
    static const char health[] = "/health";

    if (target == NULL || target_length == 0U) {
        return HTTP_ERROR_BODY_JSON;
    }
    if (target_length == 1U && target[0] == (uint8_t)'/') {
        return HTTP_ERROR_BODY_HTML;
    }
    if (target_length == sizeof(health) - 1U &&
        memcmp(target, health, sizeof(health) - 1U) == 0) {
        return HTTP_ERROR_BODY_HTML;
    }
    if (target_length >= sizeof(panel) - 1U &&
        memcmp(target, panel, sizeof(panel) - 1U) == 0 &&
        (target_length == sizeof(panel) - 1U ||
         target[sizeof(panel) - 1U] == (uint8_t)'/')) {
        return HTTP_ERROR_BODY_HTML;
    }
    return HTTP_ERROR_BODY_JSON;
}

/* The human phrase of a status, for the HTML document.  Kept here rather than
 * read from the response encoder so the body builders depend on nothing but
 * this header: the host proof links exactly this file. */
static const char *http_error_matrix_phrase(enum http_status_code status)
{
    switch (status) {
    case HTTP_STATUS_400_BAD_REQUEST:
        return "400 Bad Request";
    case HTTP_STATUS_401_UNAUTHORIZED:
        return "401 Unauthorized";
    case HTTP_STATUS_403_FORBIDDEN:
        return "403 Forbidden";
    case HTTP_STATUS_404_NOT_FOUND:
        return "404 Not Found";
    case HTTP_STATUS_405_METHOD_NOT_ALLOWED:
        return "405 Method Not Allowed";
    case HTTP_STATUS_413_PAYLOAD_TOO_LARGE:
        return "413 Payload Too Large";
    case HTTP_STATUS_414_URI_TOO_LONG:
        return "414 URI Too Long";
    case HTTP_STATUS_429_TOO_MANY_REQUESTS:
        return "429 Too Many Requests";
    case HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE:
        return "431 Request Header Fields Too Large";
    case HTTP_STATUS_503_SERVICE_UNAVAILABLE:
        return "503 Service Unavailable";
    default:
        return "Request refused";
    }
}

static bool http_error_matrix_write(
    uint8_t *buffer,
    size_t size,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    while (text[index] != '\0') {
        if (*offset + index + 1U >= size) {
            return false;
        }
        buffer[*offset + index] = (uint8_t)text[index];
        ++index;
    }
    *offset += index;
    buffer[*offset] = (uint8_t)'\0';
    return true;
}

static bool http_error_matrix_write_u64(
    uint8_t *buffer,
    size_t size,
    size_t *offset,
    uint64_t value)
{
    char digits[20U];
    size_t count = 0U;

    if (value == 0U) {
        digits[count++] = '0';
    }
    while (value != 0U) {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        if (*offset + 1U >= size) {
            return false;
        }
        buffer[(*offset)++] = (uint8_t)digits[--count];
    }
    buffer[*offset] = (uint8_t)'\0';
    return true;
}

size_t http_error_matrix_json_body(
    enum http_status_code status,
    uint8_t *buffer,
    size_t size)
{
    size_t offset = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = (uint8_t)'\0';
    if (!http_error_matrix_write(buffer, size, &offset, "{\"status\":") ||
        !http_error_matrix_write_u64(buffer, size, &offset,
                                     (uint64_t)status) ||
        !http_error_matrix_write(buffer, size, &offset, ",\"error\":\"") ||
        !http_error_matrix_write(buffer, size, &offset,
                                 http_error_matrix_reason(status)) ||
        !http_error_matrix_write(buffer, size, &offset, "\"}")) {
        buffer[0] = (uint8_t)'\0';
        return 0U;
    }
    return offset;
}

size_t http_error_matrix_html_body(
    enum http_status_code status,
    uint8_t *buffer,
    size_t size)
{
    const char *const phrase = http_error_matrix_phrase(status);
    size_t offset = 0U;

    if (buffer == NULL || size == 0U || phrase == NULL) {
        return 0U;
    }
    buffer[0] = (uint8_t)'\0';
    if (!http_error_matrix_write(
            buffer, size, &offset,
            "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
            "<title>HFOC Appliance management - ") ||
        !http_error_matrix_write(buffer, size, &offset, phrase) ||
        !http_error_matrix_write(buffer, size, &offset, "</title></head>\n<body><h1>") ||
        !http_error_matrix_write(buffer, size, &offset, phrase) ||
        !http_error_matrix_write(
            buffer, size, &offset,
            "</h1><p>The appliance refused this request: ") ||
        !http_error_matrix_write(buffer, size, &offset,
                                 http_error_matrix_reason(status)) ||
        !http_error_matrix_write(
            buffer, size, &offset,
            ".</p><p><a href=\"/panel\">Management sign-in</a></p>"
            "</body></html>\n")) {
        buffer[0] = (uint8_t)'\0';
        return 0U;
    }
    return offset;
}

#else

typedef int http_error_matrix_unused;

#endif
