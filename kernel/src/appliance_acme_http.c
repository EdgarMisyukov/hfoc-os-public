#include "appliance_acme_http.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 10: the ACME client's HTTP exchange.  Nothing here opens
 * a socket, speaks TLS or allocates: the caller hands in the URL it is about
 * to fetch or the bytes a server answered, and gets back a bounded request or
 * a bounded reply.
 */

#define APPLIANCE_ACME_HTTP_VERSION_CURRENT 1U

/*
 * A bounded text builder: the request is assembled only after its exact length
 * is known, so a buffer that cannot hold it is refused before a byte is
 * written and never left holding half a request.
 */
struct appliance_acme_http_text {
    char *buffer;
    size_t capacity;
    size_t length;
};

static void appliance_acme_http_text_reset(
    struct appliance_acme_http_text *text,
    char *buffer,
    size_t capacity)
{
    text->buffer = buffer;
    text->capacity = capacity;
    text->length = 0U;
    if (buffer != NULL && capacity > 0U) {
        buffer[0] = '\0';
    }
}

static void appliance_acme_http_text_append_n(
    struct appliance_acme_http_text *text,
    const char *value,
    size_t length)
{
    if (length == 0U || text->length + length + 1U > text->capacity) {
        return;
    }
    memcpy(&text->buffer[text->length], value, length);
    text->length += length;
    text->buffer[text->length] = '\0';
}

static void appliance_acme_http_text_append(
    struct appliance_acme_http_text *text,
    const char *value)
{
    if (value != NULL) {
        appliance_acme_http_text_append_n(text, value, strlen(value));
    }
}

static void appliance_acme_http_text_append_u32(
    struct appliance_acme_http_text *text,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);
    while (count > 0U) {
        --count;
        appliance_acme_http_text_append_n(text, &digits[count], 1U);
    }
}

/* RFC 7230 token bytes: what a method and a header name may be spelled with. */
static bool appliance_acme_http_is_token_char(unsigned char value)
{
    if ((value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
        (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
        (value >= (unsigned char)'0' && value <= (unsigned char)'9')) {
        return true;
    }
    switch (value) {
    case '!':
    case '#':
    case '$':
    case '%':
    case '&':
    case '\'':
    case '*':
    case '+':
    case '-':
    case '.':
    case '^':
    case '_':
    case '`':
    case '|':
    case '~':
        return true;
    default:
        return false;
    }
}

static bool appliance_acme_http_is_digit(unsigned char value)
{
    return value >= (unsigned char)'0' && value <= (unsigned char)'9';
}

static bool appliance_acme_http_is_alpha(unsigned char value)
{
    return (value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
           (value >= (unsigned char)'a' && value <= (unsigned char)'z');
}

static unsigned char appliance_acme_http_lower(unsigned char value)
{
    if (value >= (unsigned char)'A' && value <= (unsigned char)'Z') {
        return (unsigned char)(value - (unsigned char)'A' + (unsigned char)'a');
    }
    return value;
}

static bool appliance_acme_http_method_is(
    const char *method,
    size_t method_length,
    const char *expected)
{
    const size_t expected_length = strlen(expected);

    return method_length == expected_length &&
           memcmp(method, expected, expected_length) == 0;
}

static bool appliance_acme_http_equals_ci(
    const char *left,
    size_t left_length,
    const char *right)
{
    const size_t right_length = strlen(right);

    if (left_length != right_length) {
        return false;
    }
    for (size_t index = 0U; index < left_length; ++index) {
        if (appliance_acme_http_lower((unsigned char)left[index]) !=
            appliance_acme_http_lower((unsigned char)right[index])) {
            return false;
        }
    }
    return true;
}

/*
 * A host is a DNS name or an IPv4 literal: letters, digits, `-` and `.`.  A
 * bare IPv6 authority is refused by name rather than guessed at, and a name
 * with an empty label or a leading/trailing dot is not a name this item ever
 * receives from a directory reply.
 */
static bool appliance_acme_http_host_valid(const char *host, size_t length)
{
    if (length == 0U || length > APPLIANCE_ACME_HTTP_HOST_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)host[index];

        if (!(appliance_acme_http_is_alpha(value) ||
              appliance_acme_http_is_digit(value) || value == (unsigned char)'-' ||
              value == (unsigned char)'.')) {
            return false;
        }
    }
    if (host[0] == (unsigned char)'.' || host[length - 1U] == (unsigned char)'.') {
        return false;
    }
    for (size_t index = 1U; index < length; ++index) {
        if (host[index] == (unsigned char)'.' &&
            host[index - 1U] == (unsigned char)'.') {
            return false;
        }
    }
    return true;
}

/* A dotted quad, each octet 0..255 and spelled without a leading zero. */
static bool appliance_acme_http_literal_ipv4(
    const char *host,
    size_t length,
    bool *literal_out)
{
    uint32_t octet = 0U;
    uint32_t value = 0U;
    uint32_t digits = 0U;

    *literal_out = false;
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char character = (unsigned char)host[index];

        if (appliance_acme_http_is_digit(character)) {
            if (digits == 3U) {
                return false;
            }
            value = value * 10U + (uint32_t)(character - (unsigned char)'0');
            ++digits;
            if (value > 255U) {
                return false;
            }
            continue;
        }
        if (character != (unsigned char)'.') {
            return false;
        }
        if (digits == 0U || (digits > 1U && host[index - digits] == '0')) {
            return false;
        }
        octet += 1U;
        digits = 0U;
        value = 0U;
    }
    if (octet != 3U || digits == 0U || (digits > 1U && host[length - digits] == '0')) {
        return false;
    }
    *literal_out = true;
    return true;
}

static enum appliance_acme_http_status appliance_acme_http_parse_port(
    const char *text,
    size_t length,
    uint16_t *port_out)
{
    uint32_t value = 0U;

    if (length == 0U || length > 5U) {
        return APPLIANCE_ACME_HTTP_BAD_PORT;
    }
    for (size_t index = 0U; index < length; ++index) {
        if (!appliance_acme_http_is_digit((unsigned char)text[index])) {
            return APPLIANCE_ACME_HTTP_BAD_PORT;
        }
        value = value * 10U + (uint32_t)(text[index] - '0');
    }
    if (value == 0U || value > 65535U) {
        return APPLIANCE_ACME_HTTP_BAD_PORT;
    }
    *port_out = (uint16_t)value;
    return APPLIANCE_ACME_HTTP_OK;
}

enum appliance_acme_http_status appliance_acme_http_parse_url(
    const char *url,
    size_t url_length,
    struct appliance_acme_http_target *target_out)
{
    const char *rest;
    size_t rest_length;
    size_t authority_length;
    size_t host_length;
    size_t target_length;
    enum appliance_acme_http_status status;

    if (url == NULL || target_out == NULL) {
        return APPLIANCE_ACME_HTTP_NULL;
    }
    memset(target_out, 0, sizeof(*target_out));
    if (url_length == 0U || url_length > APPLIANCE_ACME_HTTP_URL_MAX) {
        return APPLIANCE_ACME_HTTP_BAD_URL;
    }
    if (url_length < 8U ||
        !appliance_acme_http_equals_ci(url, 8U, "https://")) {
        return appliance_acme_http_equals_ci(url,
                                             url_length < 5U ? url_length : 5U,
                                             "http:")
                   ? APPLIANCE_ACME_HTTP_NOT_HTTPS
                   : APPLIANCE_ACME_HTTP_BAD_URL;
    }
    rest = &url[8];
    rest_length = url_length - 8U;
    authority_length = rest_length;
    for (size_t index = 0U; index < rest_length; ++index) {
        if (rest[index] == (unsigned char)'/') {
            authority_length = index;
            break;
        }
    }
    if (authority_length == 0U) {
        return APPLIANCE_ACME_HTTP_BAD_HOST;
    }
    for (size_t index = 0U; index < authority_length; ++index) {
        if (rest[index] == (unsigned char)'@') {
            return APPLIANCE_ACME_HTTP_BAD_URL;
        }
        if (rest[index] == (unsigned char)'[' || rest[index] == (unsigned char)']') {
            return APPLIANCE_ACME_HTTP_BAD_HOST;
        }
    }
    host_length = authority_length;
    target_out->port = APPLIANCE_ACME_HTTP_PORT_DEFAULT;
    for (size_t index = authority_length; index > 0U; --index) {
        if (rest[index - 1U] == (unsigned char)':') {
            status = appliance_acme_http_parse_port(
                &rest[index], authority_length - index, &target_out->port);
            if (status != APPLIANCE_ACME_HTTP_OK) {
                memset(target_out, 0, sizeof(*target_out));
                return status;
            }
            host_length = index - 1U;
            break;
        }
    }
    if (!appliance_acme_http_host_valid(rest, host_length)) {
        memset(target_out, 0, sizeof(*target_out));
        return APPLIANCE_ACME_HTTP_BAD_HOST;
    }
    if (!appliance_acme_http_literal_ipv4(
            rest, host_length, &target_out->literal_ipv4)) {
        /*
         * A name of digits and dots that is not a dotted quad is not a name
         * the item can resolve: refuse it instead of asking a resolver.
         */
        bool digits_and_dots = true;

        for (size_t index = 0U; index < host_length; ++index) {
            if (!appliance_acme_http_is_digit((unsigned char)rest[index]) &&
                rest[index] != (unsigned char)'.') {
                digits_and_dots = false;
                break;
            }
        }
        if (digits_and_dots) {
            memset(target_out, 0, sizeof(*target_out));
            return APPLIANCE_ACME_HTTP_BAD_HOST;
        }
    }
    memcpy(target_out->host, rest, host_length);
    target_out->host[host_length] = '\0';
    target_out->host_length = (uint32_t)host_length;

    target_length = rest_length - authority_length;
    if (target_length == 0U) {
        target_out->target[0] = '/';
        target_out->target[1] = '\0';
        target_out->target_length = 1U;
    } else {
        if (rest[authority_length] != (unsigned char)'/') {
            memset(target_out, 0, sizeof(*target_out));
            return APPLIANCE_ACME_HTTP_BAD_URL;
        }
        for (size_t index = authority_length; index < rest_length; ++index) {
            const unsigned char value = (unsigned char)rest[index];

            if (value <= 0x20U || value >= 0x7FU ||
                value == (unsigned char)'#') {
                memset(target_out, 0, sizeof(*target_out));
                return APPLIANCE_ACME_HTTP_BAD_URL;
            }
        }
        memcpy(target_out->target, &rest[authority_length], target_length);
        target_out->target[target_length] = '\0';
        target_out->target_length = (uint32_t)target_length;
    }

    {
        struct appliance_acme_http_text text;

        appliance_acme_http_text_reset(
            &text, target_out->record, sizeof(target_out->record));
        appliance_acme_http_text_append(&text, "[ACME-HTTP] target host=");
        appliance_acme_http_text_append_n(
            &text, target_out->host, target_out->host_length);
        appliance_acme_http_text_append(&text, " port=");
        appliance_acme_http_text_append_u32(&text, target_out->port);
        appliance_acme_http_text_append(
            &text, target_out->literal_ipv4 ? " literal=yes" : " literal=no");
    }
    return APPLIANCE_ACME_HTTP_OK;
}

enum appliance_acme_http_status appliance_acme_http_build_request(
    const char *method,
    size_t method_length,
    const struct appliance_acme_http_target *target,
    const char *content_type,
    size_t content_type_length,
    const uint8_t *body,
    size_t body_length,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    struct appliance_acme_http_text text;
    bool post;
    size_t needed;
    char number[10];
    size_t number_length;

    if (method == NULL || target == NULL || out == NULL || length_out == NULL) {
        return APPLIANCE_ACME_HTTP_NULL;
    }
    *length_out = 0U;
    if (capacity == 0U) {
        return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
    }
    out[0] = '\0';
    if (method_length == 0U || method_length > APPLIANCE_ACME_HTTP_METHOD_MAX) {
        return APPLIANCE_ACME_HTTP_BAD_METHOD;
    }
    for (size_t index = 0U; index < method_length; ++index) {
        if (!appliance_acme_http_is_token_char((unsigned char)method[index])) {
            return APPLIANCE_ACME_HTTP_BAD_METHOD;
        }
    }
    if (appliance_acme_http_method_is(method, method_length, "POST")) {
        post = true;
    } else if (appliance_acme_http_method_is(method, method_length, "GET") ||
               appliance_acme_http_method_is(method, method_length, "HEAD")) {
        post = false;
    } else {
        /*
         * HTTP methods are case-sensitive: `post` is not `POST`, and the item
         * sends exactly the three methods RFC 8555 uses.
         */
        return APPLIANCE_ACME_HTTP_BAD_METHOD;
    }
    if (target->host_length == 0U || target->target_length == 0U) {
        return APPLIANCE_ACME_HTTP_NULL;
    }
    if (post) {
        if (content_type == NULL || content_type_length == 0U ||
            content_type_length > APPLIANCE_ACME_HTTP_CONTENT_TYPE_MAX) {
            return APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE;
        }
        for (size_t index = 0U; index < content_type_length; ++index) {
            const unsigned char value = (unsigned char)content_type[index];

            if (value < 0x20U || value >= 0x7FU) {
                return APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE;
            }
        }
        if (body == NULL || body_length == 0U) {
            return APPLIANCE_ACME_HTTP_BAD_BODY;
        }
    } else {
        if (body_length != 0U) {
            return APPLIANCE_ACME_HTTP_BAD_BODY;
        }
    }
    if (body_length > APPLIANCE_ACME_HTTP_REQUEST_MAX) {
        return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
    }

    number_length = 0U;
    {
        uint32_t value = (uint32_t)body_length;

        do {
            number[number_length] = (char)('0' + (value % 10U));
            value /= 10U;
            ++number_length;
        } while (value != 0U);
    }

    needed = method_length + 1U + target->target_length + 1U +
             strlen("HTTP/1.1\r\n") + strlen("Host: ") + target->host_length +
             (target->port == APPLIANCE_ACME_HTTP_PORT_DEFAULT
                  ? 0U
                  : 1U + 5U) +
             strlen("\r\n") + strlen("Connection: close\r\n\r\n") +
             (post ? strlen("Content-Type: ") + content_type_length +
                         strlen("\r\nContent-Length: ") + number_length +
                         strlen("\r\n")
                   : 0U) +
             body_length;
    if (needed + 1U > capacity) {
        return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
    }

    appliance_acme_http_text_reset(&text, out, capacity);
    appliance_acme_http_text_append_n(&text, method, method_length);
    appliance_acme_http_text_append(&text, " ");
    appliance_acme_http_text_append_n(
        &text, target->target, target->target_length);
    appliance_acme_http_text_append(&text, " HTTP/1.1\r\nHost: ");
    appliance_acme_http_text_append_n(&text, target->host, target->host_length);
    if (target->port != APPLIANCE_ACME_HTTP_PORT_DEFAULT) {
        appliance_acme_http_text_append(&text, ":");
        appliance_acme_http_text_append_u32(&text, target->port);
    }
    appliance_acme_http_text_append(&text, "\r\n");
    if (post) {
        appliance_acme_http_text_append(&text, "Content-Type: ");
        appliance_acme_http_text_append_n(
            &text, content_type, content_type_length);
        appliance_acme_http_text_append(&text, "\r\nContent-Length: ");
        for (size_t index = number_length; index > 0U; --index) {
            appliance_acme_http_text_append_n(
                &text, &number[index - 1U], 1U);
        }
        appliance_acme_http_text_append(&text, "\r\n");
    }
    appliance_acme_http_text_append(&text, "Connection: close\r\n\r\n");
    if (body_length != 0U) {
        appliance_acme_http_text_append_n(
            &text, (const char *)body, body_length);
    }
    if (text.length != needed) {
        out[0] = '\0';
        return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
    }
    *length_out = text.length;
    return APPLIANCE_ACME_HTTP_OK;
}

/* The value of a header line, trimmed of the optional whitespace RFC 7230 allows. */
static void appliance_acme_http_trim_value(
    const char *value,
    size_t length,
    const char **trimmed_out,
    size_t *trimmed_length_out)
{
    size_t start = 0U;

    while (start < length &&
           (value[start] == (unsigned char)' ' ||
            value[start] == (unsigned char)'\t')) {
        ++start;
    }
    while (length > start &&
           (value[length - 1U] == (unsigned char)' ' ||
            value[length - 1U] == (unsigned char)'\t')) {
        --length;
    }
    *trimmed_out = &value[start];
    *trimmed_length_out = length - start;
}

static enum appliance_acme_http_status appliance_acme_http_read_header(
    const char *line,
    size_t length,
    struct appliance_acme_http_response *response)
{
    size_t name_length = 0U;
    const char *value;
    size_t value_length;

    if (length == 0U) {
        return APPLIANCE_ACME_HTTP_OK;
    }
    if (line[0] == (unsigned char)' ' || line[0] == (unsigned char)'\t') {
        return APPLIANCE_ACME_HTTP_BAD_HEADER;
    }
    while (name_length < length && line[name_length] != (unsigned char)':') {
        if (!appliance_acme_http_is_token_char((unsigned char)line[name_length])) {
            return APPLIANCE_ACME_HTTP_BAD_HEADER;
        }
        ++name_length;
    }
    if (name_length == 0U || name_length == length) {
        return APPLIANCE_ACME_HTTP_BAD_HEADER;
    }
    appliance_acme_http_trim_value(
        &line[name_length + 1U], length - name_length - 1U, &value,
        &value_length);
    for (size_t index = 0U; index < value_length; ++index) {
        const unsigned char character = (unsigned char)value[index];

        if (character < 0x20U || character >= 0x7FU) {
            return APPLIANCE_ACME_HTTP_BAD_HEADER;
        }
    }

    if (appliance_acme_http_equals_ci(line, name_length, "location")) {
        if (response->has_location) {
            return APPLIANCE_ACME_HTTP_DUPLICATE_HEADER;
        }
        if (value_length > APPLIANCE_ACME_HTTP_LOCATION_MAX) {
            return APPLIANCE_ACME_HTTP_HEADER_TOO_LONG;
        }
        memcpy(response->location, value, value_length);
        response->location[value_length] = '\0';
        response->location_length = (uint32_t)value_length;
        response->has_location = true;
        return APPLIANCE_ACME_HTTP_OK;
    }
    if (appliance_acme_http_equals_ci(line, name_length, "replay-nonce")) {
        if (response->has_nonce) {
            return APPLIANCE_ACME_HTTP_DUPLICATE_HEADER;
        }
        if (value_length > APPLIANCE_ACME_HTTP_NONCE_MAX) {
            return APPLIANCE_ACME_HTTP_HEADER_TOO_LONG;
        }
        memcpy(response->nonce, value, value_length);
        response->nonce[value_length] = '\0';
        response->nonce_length = (uint32_t)value_length;
        response->has_nonce = true;
        return APPLIANCE_ACME_HTTP_OK;
    }
    if (appliance_acme_http_equals_ci(line, name_length, "content-type")) {
        if (response->has_content_type) {
            return APPLIANCE_ACME_HTTP_DUPLICATE_HEADER;
        }
        if (value_length > APPLIANCE_ACME_HTTP_CONTENT_TYPE_MAX) {
            return APPLIANCE_ACME_HTTP_HEADER_TOO_LONG;
        }
        memcpy(response->content_type, value, value_length);
        response->content_type[value_length] = '\0';
        response->content_type_length = (uint32_t)value_length;
        response->has_content_type = true;
        return APPLIANCE_ACME_HTTP_OK;
    }
    if (appliance_acme_http_equals_ci(line, name_length, "content-length")) {
        uint32_t value_number = 0U;

        if (response->has_content_length) {
            return APPLIANCE_ACME_HTTP_DUPLICATE_HEADER;
        }
        if (value_length == 0U || value_length > 10U) {
            return APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH;
        }
        for (size_t index = 0U; index < value_length; ++index) {
            if (!appliance_acme_http_is_digit((unsigned char)value[index])) {
                return APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH;
            }
            value_number =
                value_number * 10U + (uint32_t)(value[index] - '0');
            /*
             * The bound is checked as the number accumulates, so a ten-digit
             * length cannot wrap a 32-bit accumulator back into a small one:
             * 4294967296 would otherwise be read as an empty body.
             */
            if (value_number > APPLIANCE_ACME_HTTP_BODY_MAX) {
                return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
            }
        }
        response->content_length = value_number;
        response->has_content_length = true;
        return APPLIANCE_ACME_HTTP_OK;
    }
    if (appliance_acme_http_equals_ci(line, name_length, "transfer-encoding")) {
        return APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED;
    }
    return APPLIANCE_ACME_HTTP_OK;
}

enum appliance_acme_http_status appliance_acme_http_read_response(
    const uint8_t *bytes,
    size_t length,
    struct appliance_acme_http_response *response_out)
{
    size_t head_length = 0U;
    size_t position = 0U;
    size_t status_line_end = 0U;
    uint32_t status_code = 0U;

    if (bytes == NULL || response_out == NULL) {
        return APPLIANCE_ACME_HTTP_NULL;
    }
    memset(response_out, 0, sizeof(*response_out));
    if (length == 0U) {
        return APPLIANCE_ACME_HTTP_TRUNCATED;
    }
    if (length > APPLIANCE_ACME_HTTP_RESPONSE_MAX) {
        return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
    }
    {
        bool terminated = false;

        for (size_t index = 0U; index + 3U < length; ++index) {
            if (bytes[index] == (unsigned char)'\r' &&
                bytes[index + 1U] == (unsigned char)'\n' &&
                bytes[index + 2U] == (unsigned char)'\r' &&
                bytes[index + 3U] == (unsigned char)'\n') {
                head_length = index;
                terminated = true;
                break;
            }
        }
        if (!terminated) {
            return APPLIANCE_ACME_HTTP_TRUNCATED;
        }
    }
    if (head_length > APPLIANCE_ACME_HTTP_HEADER_BLOCK_MAX) {
        return APPLIANCE_ACME_HTTP_HEADER_TOO_LONG;
    }

    /*
     * The status line: HTTP/1.x, a space, a three-digit code and an optional
     * reason phrase.  The head excludes the empty line, so a reply without
     * headers ends its status line with the empty line's own CRLF.
     */
    if (head_length < 12U ||
        memcmp(bytes, "HTTP/1.", 7U) != 0 ||
        (bytes[7] != (unsigned char)'0' && bytes[7] != (unsigned char)'1') ||
        bytes[8] != (unsigned char)' ' ||
        !appliance_acme_http_is_digit(bytes[9]) ||
        !appliance_acme_http_is_digit(bytes[10]) ||
        !appliance_acme_http_is_digit(bytes[11])) {
        return APPLIANCE_ACME_HTTP_BAD_STATUS_LINE;
    }
    status_code = (uint32_t)(bytes[9] - '0') * 100U +
                  (uint32_t)(bytes[10] - '0') * 10U +
                  (uint32_t)(bytes[11] - '0');
    if (head_length > 12U && bytes[12] == (unsigned char)' ') {
        size_t scan = 13U;

        while (scan < head_length && bytes[scan] != (unsigned char)'\r') {
            if (bytes[scan] < 0x20U || bytes[scan] >= 0x7FU) {
                return APPLIANCE_ACME_HTTP_BAD_STATUS_LINE;
            }
            ++scan;
        }
        status_line_end = scan;
    } else if (head_length == 12U) {
        status_line_end = 12U;
    } else {
        return APPLIANCE_ACME_HTTP_BAD_STATUS_LINE;
    }
    if (status_line_end == head_length) {
        position = head_length;
    } else if (status_line_end + 1U < head_length &&
               bytes[status_line_end] == (unsigned char)'\r' &&
               bytes[status_line_end + 1U] == (unsigned char)'\n') {
        position = status_line_end + 2U;
    } else {
        return APPLIANCE_ACME_HTTP_BAD_STATUS_LINE;
    }
    response_out->status_code = status_code;

    {
        uint32_t header_count = 0U;

        while (position < head_length) {
            size_t line_end = position;
            bool terminated = false;

            while (line_end < head_length &&
                   bytes[line_end] != (unsigned char)'\r') {
                ++line_end;
            }
            if (line_end < head_length) {
                if (line_end + 1U >= head_length ||
                    bytes[line_end + 1U] != (unsigned char)'\n') {
                    return APPLIANCE_ACME_HTTP_BAD_HEADER;
                }
                terminated = true;
            }
            if (header_count >= APPLIANCE_ACME_HTTP_HEADER_COUNT_MAX) {
                return APPLIANCE_ACME_HTTP_TOO_MANY_HEADERS;
            }
            ++header_count;
            {
                const enum appliance_acme_http_status status =
                    appliance_acme_http_read_header(
                        (const char *)&bytes[position], line_end - position,
                        response_out);

                if (status != APPLIANCE_ACME_HTTP_OK) {
                    memset(response_out, 0, sizeof(*response_out));
                    return status;
                }
            }
            position = terminated ? line_end + 2U : head_length;
        }
    }

    {
        const size_t body_offset = head_length + 4U;
        const size_t received = length - body_offset;

        if (response_out->has_content_length) {
            if (received < response_out->content_length) {
                memset(response_out, 0, sizeof(*response_out));
                return APPLIANCE_ACME_HTTP_TRUNCATED;
            }
            if (received > response_out->content_length) {
                memset(response_out, 0, sizeof(*response_out));
                return APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH;
            }
        } else if (received > APPLIANCE_ACME_HTTP_BODY_MAX) {
            memset(response_out, 0, sizeof(*response_out));
            return APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL;
        }
        if (received != 0U) {
            memcpy(response_out->body, &bytes[body_offset], received);
        }
        response_out->body_length = (uint32_t)received;
    }

    {
        struct appliance_acme_http_text text;

        appliance_acme_http_text_reset(
            &text, response_out->record, sizeof(response_out->record));
        appliance_acme_http_text_append(&text, "[ACME-HTTP] response status=");
        appliance_acme_http_text_append_u32(
            &text, response_out->status_code);
        appliance_acme_http_text_append(&text, " bytes=");
        appliance_acme_http_text_append_u32(&text, response_out->body_length);
    }
    return APPLIANCE_ACME_HTTP_OK;
}

bool appliance_acme_http_status_is_success(uint32_t status_code)
{
    return status_code >= 200U && status_code <= 299U;
}

const char *appliance_acme_http_status_string(
    enum appliance_acme_http_status status)
{
    switch (status) {
    case APPLIANCE_ACME_HTTP_OK:
        return "ok";
    case APPLIANCE_ACME_HTTP_NULL:
        return "null";
    case APPLIANCE_ACME_HTTP_BAD_URL:
        return "bad-url";
    case APPLIANCE_ACME_HTTP_NOT_HTTPS:
        return "not-https";
    case APPLIANCE_ACME_HTTP_BAD_HOST:
        return "bad-host";
    case APPLIANCE_ACME_HTTP_BAD_PORT:
        return "bad-port";
    case APPLIANCE_ACME_HTTP_BAD_METHOD:
        return "bad-method";
    case APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE:
        return "bad-content-type";
    case APPLIANCE_ACME_HTTP_BAD_BODY:
        return "bad-body";
    case APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    case APPLIANCE_ACME_HTTP_BAD_STATUS_LINE:
        return "bad-status-line";
    case APPLIANCE_ACME_HTTP_BAD_HEADER:
        return "bad-header";
    case APPLIANCE_ACME_HTTP_HEADER_TOO_LONG:
        return "header-too-long";
    case APPLIANCE_ACME_HTTP_TOO_MANY_HEADERS:
        return "too-many-headers";
    case APPLIANCE_ACME_HTTP_TRUNCATED:
        return "truncated";
    case APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH:
        return "bad-content-length";
    case APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED:
        return "transfer-encoding-unsupported";
    case APPLIANCE_ACME_HTTP_DUPLICATE_HEADER:
        return "duplicate-header";
    default:
        return "unknown";
    }
}
