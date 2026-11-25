#include "http_response.h"
#include "appliance_management_tls.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTP_RESPONSE_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

static const char http_response_status_prefix[] = "HTTP/1.1 ";
static const char http_response_content_type_prefix[] =
    "Content-Type: ";
static const char http_response_content_length_prefix[] =
    "Content-Length: ";
static const char http_response_set_cookie_prefix[] = "Set-Cookie: ";
static const char http_response_location_prefix[] = "Location: ";
/*
 * ITEM-375: the management listener terminates TLS, so every response it
 * serves carries HSTS.  ITEM-384 adds the Content-Security-Policy of the
 * rendered panel.  Every header is part of the fixed head, which keeps it on
 * error responses too and keeps it out of reach of a handler bug.
 */
static const char http_response_fixed_headers[] =
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    APPLIANCE_MANAGEMENT_TLS_HSTS "\r\n"
    HTTP_RESPONSE_CONTENT_SECURITY_POLICY "\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n";
static const char http_response_crlf[] = "\r\n";

static const char http_response_status_200[] = "200 OK";
static const char http_response_status_303[] = "303 See Other";
static const char http_response_status_400[] = "400 Bad Request";
static const char http_response_status_401[] = "401 Unauthorized";
static const char http_response_status_403[] = "403 Forbidden";
static const char http_response_status_404[] = "404 Not Found";
static const char http_response_status_405[] =
    "405 Method Not Allowed";
static const char http_response_status_413[] =
    "413 Payload Too Large";
static const char http_response_status_414[] = "414 URI Too Long";
static const char http_response_status_429[] =
    "429 Too Many Requests";
static const char http_response_status_431[] =
    "431 Request Header Fields Too Large";
static const char http_response_status_503[] =
    "503 Service Unavailable";

static const char http_response_type_html[] =
    "text/html; charset=utf-8";
static const char http_response_type_json[] = "application/json";

const char *http_status_code_string(
    enum http_status_code status)
{
    switch (status) {
    case HTTP_STATUS_200_OK:
        return http_response_status_200;
    case HTTP_STATUS_303_SEE_OTHER:
        return http_response_status_303;
    case HTTP_STATUS_400_BAD_REQUEST:
        return http_response_status_400;
    case HTTP_STATUS_401_UNAUTHORIZED:
        return http_response_status_401;
    case HTTP_STATUS_403_FORBIDDEN:
        return http_response_status_403;
    case HTTP_STATUS_404_NOT_FOUND:
        return http_response_status_404;
    case HTTP_STATUS_405_METHOD_NOT_ALLOWED:
        return http_response_status_405;
    case HTTP_STATUS_413_PAYLOAD_TOO_LARGE:
        return http_response_status_413;
    case HTTP_STATUS_414_URI_TOO_LONG:
        return http_response_status_414;
    case HTTP_STATUS_429_TOO_MANY_REQUESTS:
        return http_response_status_429;
    case HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE:
        return http_response_status_431;
    case HTTP_STATUS_503_SERVICE_UNAVAILABLE:
        return http_response_status_503;
    default:
        return NULL;
    }
}

const char *http_content_type_string(
    enum http_content_type content_type)
{
    switch (content_type) {
    case HTTP_CONTENT_TYPE_HTML_UTF8:
        return http_response_type_html;
    case HTTP_CONTENT_TYPE_JSON:
        return http_response_type_json;
    case HTTP_CONTENT_TYPE_PEM:
        return "application/x-pem-file";
    default:
        return NULL;
    }
}

const char *http_response_status_string(
    enum http_response_status status)
{
    switch (status) {
    case HTTP_RESPONSE_OK:
        return "HTTP_RESPONSE_OK";
    case HTTP_RESPONSE_NULL:
        return "HTTP_RESPONSE_NULL";
    case HTTP_RESPONSE_BAD_STATUS:
        return "HTTP_RESPONSE_BAD_STATUS";
    case HTTP_RESPONSE_BAD_CONTENT_TYPE:
        return "HTTP_RESPONSE_BAD_CONTENT_TYPE";
    case HTTP_RESPONSE_BODY_TOO_LARGE:
        return "HTTP_RESPONSE_BODY_TOO_LARGE";
    case HTTP_RESPONSE_BUFFER_TOO_SMALL:
        return "HTTP_RESPONSE_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

static bool http_response_valid(
    const struct http_response *response)
{
    return response != NULL &&
           http_status_code_string(response->status) != NULL &&
           http_content_type_string(response->content_type) != NULL &&
           (response->body != NULL ||
            response->body_length == 0U) &&
           response->body_length <= HTTP_RESPONSE_BODY_MAX &&
           (response->set_cookie != NULL ||
            response->set_cookie_length == 0U) &&
           response->set_cookie_length <= HTTP_RESPONSE_SET_COOKIE_MAX &&
           (response->location != NULL ||
            response->location_length == 0U) &&
           response->location_length <= HTTP_RESPONSE_LOCATION_MAX;
}

static enum http_response_status http_response_failure_status(
    const struct http_response *response)
{
    if (!http_response_valid(response)) {
        if (http_status_code_string(response->status) == NULL) {
            return HTTP_RESPONSE_BAD_STATUS;
        }
        if (http_content_type_string(response->content_type) == NULL) {
            return HTTP_RESPONSE_BAD_CONTENT_TYPE;
        }
        if ((response->body == NULL &&
             response->body_length != 0U) ||
            (response->set_cookie == NULL &&
             response->set_cookie_length != 0U) ||
            (response->location == NULL &&
             response->location_length != 0U)) {
            return HTTP_RESPONSE_NULL;
        }
        return HTTP_RESPONSE_BODY_TOO_LARGE;
    }
    return HTTP_RESPONSE_OK;
}

static void http_response_append(
    uint8_t *output,
    uint32_t *offset,
    const uint8_t *bytes,
    uint32_t length)
{
    memcpy(output + *offset, bytes, length);
    *offset += length;
}

static void http_response_append_cstr(
    uint8_t *output,
    uint32_t *offset,
    const char *text)
{
    http_response_append(
        output,
        offset,
        (const uint8_t *)text,
        (uint32_t)(strlen(text)));
}

static void http_response_write_decimal(
    uint8_t *output,
    uint32_t *offset,
    uint32_t value)
{
    uint8_t digits[10];
    uint32_t count = 0U;

    do {
        digits[count++] =
            (uint8_t)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);

    while (count != 0U) {
        output[(*offset)++] = digits[--count];
    }
}

static uint32_t http_response_build_header(
    const struct http_response *response,
    uint8_t *header)
{
    uint32_t offset = 0U;

    http_response_append_cstr(
        header, &offset, http_response_status_prefix);
    http_response_append_cstr(
        header,
        &offset,
        http_status_code_string(response->status));
    http_response_append_cstr(
        header, &offset, http_response_crlf);
    http_response_append_cstr(
        header, &offset, http_response_content_type_prefix);
    http_response_append_cstr(
        header,
        &offset,
        http_content_type_string(response->content_type));
    http_response_append_cstr(
        header, &offset, http_response_crlf);
    http_response_append_cstr(
        header, &offset, http_response_content_length_prefix);
    http_response_write_decimal(
        header, &offset, response->body_length);
    http_response_append_cstr(
        header, &offset, http_response_crlf);
    /*
     * ITEM-445 step 2: the two headers the browser sign-in adds, printed only
     * when the response carries them, so every pre-item head is unchanged.
     */
    if (response->set_cookie_length != 0U) {
        http_response_append_cstr(
            header, &offset, http_response_set_cookie_prefix);
        http_response_append(
            header,
            &offset,
            response->set_cookie,
            response->set_cookie_length);
        http_response_append_cstr(
            header, &offset, http_response_crlf);
    }
    if (response->location_length != 0U) {
        http_response_append_cstr(
            header, &offset, http_response_location_prefix);
        http_response_append(
            header,
            &offset,
            response->location,
            response->location_length);
        http_response_append_cstr(
            header, &offset, http_response_crlf);
    }
    http_response_append_cstr(
        header, &offset, http_response_fixed_headers);
    return offset;
}

static uint32_t http_response_header_length(
    const struct http_response *response)
{
    uint8_t header[HTTP_RESPONSE_BUFFER_MAX];

    return http_response_build_header(response, header);
}

uint32_t http_response_length(
    const struct http_response *response)
{
    if (!http_response_valid(response)) {
        return 0U;
    }
    return http_response_header_length(response) +
           response->body_length;
}

uint32_t http_response_head_length(
    const struct http_response *response)
{
    if (!http_response_valid(response)) {
        return 0U;
    }
    return http_response_header_length(response);
}

enum http_response_status http_response_encode(
    const struct http_response *response,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written)
{
    uint8_t header[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t header_length;
    uint32_t total_length;
    enum http_response_status failure_status;

    if (response == NULL ||
        output == NULL ||
        written == NULL) {
        return HTTP_RESPONSE_NULL;
    }
    failure_status = http_response_failure_status(response);
    if (failure_status != HTTP_RESPONSE_OK) {
        return failure_status;
    }

    header_length = http_response_build_header(response, header);
    total_length = header_length + response->body_length;
    if (output_capacity < total_length) {
        return HTTP_RESPONSE_BUFFER_TOO_SMALL;
    }

    memcpy(output, header, header_length);
    if (response->body_length != 0U) {
        memcpy(
            output + header_length,
            response->body,
            response->body_length);
    }
    *written = total_length;
    return HTTP_RESPONSE_OK;
}

enum http_response_status http_response_encode_head(
    const struct http_response *response,
    uint8_t *output,
    uint32_t output_capacity,
    uint32_t *written)
{
    uint8_t header[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t header_length;
    enum http_response_status failure_status;

    if (response == NULL ||
        output == NULL ||
        written == NULL) {
        return HTTP_RESPONSE_NULL;
    }
    failure_status = http_response_failure_status(response);
    if (failure_status != HTTP_RESPONSE_OK) {
        return failure_status;
    }

    header_length = http_response_build_header(response, header);
    if (output_capacity < header_length) {
        return HTTP_RESPONSE_BUFFER_TOO_SMALL;
    }

    memcpy(output, header, header_length);
    *written = header_length;
    return HTTP_RESPONSE_OK;
}

#else

typedef int http_response_unused;

#endif
