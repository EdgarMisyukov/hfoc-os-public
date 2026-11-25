#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const uint8_t root_body[] = "<h1>hfoc</h1>";
static const uint8_t health_body[] =
    "{\"status\":\"ok\",\"route\":\"health\"}";

static const char root_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 13\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "<h1>hfoc</h1>";

static const char health_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 32\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "{\"status\":\"ok\",\"route\":\"health\"}";

static const char root_head_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 13\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n";

static const char health_head_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 32\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n";

static struct http_response make_response(
    enum http_status_code status,
    enum http_content_type content_type,
    const uint8_t *body,
    uint32_t body_length)
{
    struct http_response response;

    memset(&response, 0, sizeof(response));
    response.status = status;
    response.content_type = content_type;
    response.body = body;
    response.body_length = body_length;
    return response;
}

static void test_string_publication(void)
{
    CHECK(http_response_status_string(HTTP_RESPONSE_OK) != NULL,
          "response OK status string is defined");
    CHECK(http_response_status_string(HTTP_RESPONSE_NULL) != NULL,
          "response NULL status string is defined");
    CHECK(http_response_status_string(HTTP_RESPONSE_BAD_STATUS) != NULL,
          "response bad-status string is defined");
    CHECK(http_response_status_string(HTTP_RESPONSE_BAD_CONTENT_TYPE) != NULL,
          "response bad-content-type string is defined");
    CHECK(http_response_status_string(HTTP_RESPONSE_BODY_TOO_LARGE) != NULL,
          "response too-large string is defined");
    CHECK(http_response_status_string(HTTP_RESPONSE_BUFFER_TOO_SMALL) != NULL,
          "response short-buffer string is defined");
    CHECK(http_response_status_string(
              (enum http_response_status)99) == NULL,
          "unknown response status string is null");

    CHECK(http_status_code_string(HTTP_STATUS_200_OK) != NULL &&
              strcmp(http_status_code_string(HTTP_STATUS_200_OK),
                     "200 OK") == 0,
          "200 status string is exact");
    CHECK(http_status_code_string(HTTP_STATUS_400_BAD_REQUEST) != NULL &&
              strcmp(http_status_code_string(HTTP_STATUS_400_BAD_REQUEST),
                     "400 Bad Request") == 0,
          "400 status string is exact");
    CHECK(http_status_code_string(HTTP_STATUS_404_NOT_FOUND) != NULL &&
              strcmp(http_status_code_string(HTTP_STATUS_404_NOT_FOUND),
                     "404 Not Found") == 0,
          "404 status string is exact");
    CHECK(http_status_code_string(HTTP_STATUS_405_METHOD_NOT_ALLOWED) != NULL &&
              strcmp(
                  http_status_code_string(
                      HTTP_STATUS_405_METHOD_NOT_ALLOWED),
                  "405 Method Not Allowed") == 0,
          "405 status string is exact");
    CHECK(http_status_code_string(
              HTTP_STATUS_413_PAYLOAD_TOO_LARGE) != NULL &&
              strcmp(
                  http_status_code_string(
                      HTTP_STATUS_413_PAYLOAD_TOO_LARGE),
                  "413 Payload Too Large") == 0,
          "413 status string is exact");
    CHECK(http_status_code_string(HTTP_STATUS_414_URI_TOO_LONG) != NULL &&
              strcmp(http_status_code_string(HTTP_STATUS_414_URI_TOO_LONG),
                     "414 URI Too Long") == 0,
          "414 status string is exact");
    CHECK(http_status_code_string(
              HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE) != NULL &&
              strcmp(
                  http_status_code_string(
                      HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE),
                  "431 Request Header Fields Too Large") == 0,
          "431 status string is exact");
    CHECK(http_status_code_string((enum http_status_code)99) == NULL,
          "unknown status string is null");

    CHECK(http_content_type_string(HTTP_CONTENT_TYPE_HTML_UTF8) != NULL &&
              strcmp(
                  http_content_type_string(
                      HTTP_CONTENT_TYPE_HTML_UTF8),
                  "text/html; charset=utf-8") == 0,
          "HTML content-type string is exact");
    CHECK(http_content_type_string(HTTP_CONTENT_TYPE_JSON) != NULL &&
              strcmp(http_content_type_string(HTTP_CONTENT_TYPE_JSON),
                     "application/json") == 0,
          "JSON content-type string is exact");
    CHECK(http_content_type_string((enum http_content_type)99) == NULL,
          "unknown content-type string is null");
}

static void check_encode(
    const struct http_response *response,
    const uint8_t *expected,
    uint32_t expected_length,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t computed = http_response_length(response);
    uint32_t written = 0U;
    enum http_response_status status;

    CHECK(computed == expected_length,
          "response length matches fixture");
    memset(output, 0xA5, sizeof(output));
    status = http_response_encode(
        response, output, sizeof(output), &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == expected_length &&
              memcmp(output, expected, expected_length) == 0,
          message);
}

static void check_encode_head(
    const struct http_response *response,
    const uint8_t *expected,
    uint32_t expected_length,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t computed = http_response_head_length(response);
    uint32_t written = 0U;
    enum http_response_status status;

    CHECK(computed == expected_length,
          "HEAD response length matches fixture");
    memset(output, 0xA5, sizeof(output));
    status = http_response_encode_head(
        response, output, sizeof(output), &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == expected_length &&
              memcmp(output, expected, expected_length) == 0 &&
              output[expected_length] == 0xA5U,
          message);
}

static void test_exact_fixtures(void)
{
    struct http_response response;

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    check_encode(
        &response,
        (const uint8_t *)root_expected,
        (uint32_t)(sizeof(root_expected) - 1U),
        "root HTML response bytes are exact and deterministic");

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_JSON,
        health_body,
        (uint32_t)(sizeof(health_body) - 1U));
    check_encode(
        &response,
        (const uint8_t *)health_expected,
        (uint32_t)(sizeof(health_expected) - 1U),
        "health JSON response bytes are exact and deterministic");
}

static void test_head_exact_fixtures(void)
{
    struct http_response response;

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    check_encode_head(
        &response,
        (const uint8_t *)root_head_expected,
        (uint32_t)(sizeof(root_head_expected) - 1U),
        "root HEAD response bytes are exact with zero body bytes");

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_JSON,
        health_body,
        (uint32_t)(sizeof(health_body) - 1U));
    check_encode_head(
        &response,
        (const uint8_t *)health_head_expected,
        (uint32_t)(sizeof(health_head_expected) - 1U),
        "health HEAD response bytes are exact with zero body bytes");
}

static void test_head_get_equivalence(void)
{
    static const struct {
        const uint8_t *body;
        uint32_t body_length;
        const uint8_t *get_expected;
        uint32_t get_length;
        const uint8_t *head_expected;
        uint32_t head_length;
    } cases[] = {
        {
            root_body,
            (uint32_t)(sizeof(root_body) - 1U),
            (const uint8_t *)root_expected,
            (uint32_t)(sizeof(root_expected) - 1U),
            (const uint8_t *)root_head_expected,
            (uint32_t)(sizeof(root_head_expected) - 1U),
        },
        {
            health_body,
            (uint32_t)(sizeof(health_body) - 1U),
            (const uint8_t *)health_expected,
            (uint32_t)(sizeof(health_expected) - 1U),
            (const uint8_t *)health_head_expected,
            (uint32_t)(sizeof(health_head_expected) - 1U),
        },
    };
    uint32_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        struct http_response response = make_response(
            HTTP_STATUS_200_OK,
            HTTP_CONTENT_TYPE_HTML_UTF8,
            cases[index].body,
            cases[index].body_length);
        uint8_t get_output[HTTP_RESPONSE_BUFFER_MAX];
        uint8_t head_output[HTTP_RESPONSE_BUFFER_MAX];
        uint32_t get_written = 0U;
        uint32_t head_written = 0U;
        enum http_response_status get_status;
        enum http_response_status head_status;
        char message[96];

        if (index == 1U) {
            response.content_type = HTTP_CONTENT_TYPE_JSON;
        }
        get_status = http_response_encode(
            &response, get_output, sizeof(get_output), &get_written);
        head_status = http_response_encode_head(
            &response, head_output, sizeof(head_output), &head_written);
        CHECK(get_status == HTTP_RESPONSE_OK &&
                  head_status == HTTP_RESPONSE_OK &&
                  head_written == cases[index].head_length &&
                  get_written == cases[index].get_length &&
                  memcmp(
                      get_output,
                      head_output,
                      cases[index].head_length) == 0 &&
                  memcmp(
                      head_output,
                      cases[index].head_expected,
                      cases[index].head_length) == 0 &&
                  memcmp(
                      get_output,
                      cases[index].get_expected,
                      cases[index].get_length) == 0,
              "HEAD shares GET status and headers while suppressing body");
        snprintf(
            message,
            sizeof(message),
            "HEAD length is GET length minus body length for fixture %u",
            index);
        CHECK(http_response_head_length(&response) ==
                  http_response_length(&response) -
                      cases[index].body_length,
              message);
    }
}

static void test_head_zero_body_equivalence(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_JSON,
        NULL,
        0U);
    uint8_t get_output[HTTP_RESPONSE_BUFFER_MAX];
    uint8_t head_output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t get_written = 0U;
    uint32_t head_written = 0U;
    enum http_response_status get_status;
    enum http_response_status head_status;

    get_status = http_response_encode(
        &response, get_output, sizeof(get_output), &get_written);
    head_status = http_response_encode_head(
        &response, head_output, sizeof(head_output), &head_written);
    CHECK(http_response_head_length(&response) ==
              http_response_length(&response) &&
              get_status == HTTP_RESPONSE_OK &&
              head_status == HTTP_RESPONSE_OK &&
              get_written == head_written &&
              memcmp(get_output, head_output, get_written) == 0,
          "zero-body HEAD equals zero-body GET");
}

static void test_head_repeated_and_short_output_cursor(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint8_t repeated[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t head_length = http_response_head_length(&response);
    uint32_t written = UINT32_C(0x12345678);
    enum http_response_status status;

    memset(output, 0xA5, sizeof(output));
    status = http_response_encode_head(
        &response, output, head_length - 1U, &written);
    CHECK(status == HTTP_RESPONSE_BUFFER_TOO_SMALL &&
              written == UINT32_C(0x12345678),
          "one-byte-short HEAD output fails without written mutation");
    {
        uint32_t index;
        bool unchanged = true;

        for (index = 0U; index < sizeof(output); ++index) {
            if (output[index] != 0xA5U) {
                unchanged = false;
                break;
            }
        }
        CHECK(unchanged,
              "one-byte-short HEAD output fails without output mutation");
    }

    memset(output, 0xA5, sizeof(output));
    written = 0U;
    status = http_response_encode_head(
        &response, output, head_length, &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == head_length &&
              memcmp(
                  output,
                  root_head_expected,
                  sizeof(root_head_expected) - 1U) == 0 &&
              output[written] == 0xA5U,
          "exact-capacity HEAD succeeds after short failure");

    memset(repeated, 0xA5, sizeof(repeated));
    status = http_response_encode_head(
        &response,
        repeated,
        sizeof(repeated),
        &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == head_length &&
              memcmp(output, repeated, head_length) == 0,
          "repeated HEAD encoding is deterministic");
}

static void test_head_failure_contracts(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = UINT32_C(0x12345678);
    struct http_response invalid = response;
    uint32_t head_length = http_response_head_length(&response);

    memset(output, 0xA5, sizeof(output));
    CHECK(http_response_encode_head(
              NULL, output, sizeof(output), &written) ==
              HTTP_RESPONSE_NULL &&
              written == UINT32_C(0x12345678),
          "null HEAD response fails without mutation");
    CHECK(http_response_encode_head(
              &response, NULL, sizeof(output), &written) ==
              HTTP_RESPONSE_NULL,
          "null HEAD output fails");
    CHECK(http_response_encode_head(
              &response, output, sizeof(output), NULL) ==
              HTTP_RESPONSE_NULL,
          "null HEAD written fails");

    invalid.status = (enum http_status_code)99;
    CHECK(http_response_encode_head(
              &invalid, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BAD_STATUS,
          "unknown status HEAD fails");
    invalid = response;
    invalid.content_type = (enum http_content_type)99;
    CHECK(http_response_encode_head(
              &invalid, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BAD_CONTENT_TYPE,
          "unknown content type HEAD fails");

    invalid = response;
    invalid.body = NULL;
    invalid.body_length = 1U;
    CHECK(http_response_head_length(&invalid) == 0U &&
              http_response_encode_head(
                  &invalid, output, sizeof(output), &written) ==
                  HTTP_RESPONSE_NULL,
          "null HEAD body with nonzero length fails");

    invalid = response;
    invalid.body_length = HTTP_RESPONSE_BODY_MAX + 1U;
    CHECK(http_response_head_length(&invalid) == 0U &&
              http_response_encode_head(
                  &invalid, output, sizeof(output), &written) ==
                  HTTP_RESPONSE_BODY_TOO_LARGE,
          "oversized HEAD body fails");

    memset(output, 0xA5, sizeof(output));
    written = UINT32_C(0x12345678);
    CHECK(http_response_encode_head(
              &response, output, head_length - 1U, &written) ==
              HTTP_RESPONSE_BUFFER_TOO_SMALL &&
              written == UINT32_C(0x12345678),
          "short HEAD buffer fails without cursor mutation");
    {
        uint32_t index;
        bool unchanged = true;

        for (index = 0U; index < sizeof(output); ++index) {
            if (output[index] != 0xA5U) {
                unchanged = false;
                break;
            }
        }
        CHECK(unchanged,
              "short HEAD buffer fails without output mutation");
    }
}

static void test_status_lines_and_empty_body(void)
{
    static const struct {
        enum http_status_code status;
        const char *reason;
    } cases[] = {
        { HTTP_STATUS_200_OK, "200 OK" },
        { HTTP_STATUS_400_BAD_REQUEST, "400 Bad Request" },
        { HTTP_STATUS_404_NOT_FOUND, "404 Not Found" },
        { HTTP_STATUS_405_METHOD_NOT_ALLOWED, "405 Method Not Allowed" },
        { HTTP_STATUS_413_PAYLOAD_TOO_LARGE, "413 Payload Too Large" },
        { HTTP_STATUS_414_URI_TOO_LONG, "414 URI Too Long" },
        { HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE,
          "431 Request Header Fields Too Large" },
    };
    uint32_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        struct http_response response = make_response(
            cases[index].status,
            HTTP_CONTENT_TYPE_HTML_UTF8,
            NULL,
            0U);
        uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
        uint32_t written = 0U;
        uint32_t prefix_length =
            (uint32_t)strlen("HTTP/1.1 ") +
            (uint32_t)strlen(cases[index].reason);
        enum http_response_status status;

        CHECK(http_response_length(&response) > prefix_length,
              "empty response has positive encoded length");
        memset(output, 0, sizeof(output));
        status = http_response_encode(
            &response, output, sizeof(output), &written);
        CHECK(status == HTTP_RESPONSE_OK &&
                  memcmp(output, "HTTP/1.1 ", 9U) == 0 &&
                  memcmp(
                      output + 9U,
                      cases[index].reason,
                      strlen(cases[index].reason)) == 0 &&
                  output[9U + strlen(cases[index].reason)] == '\r' &&
                  output[9U + strlen(cases[index].reason) + 1U] == '\n',
              "empty response status line is exact");
        CHECK(written == http_response_length(&response),
              "empty response written length matches computed length");
    }
}

static const uint8_t *find_bytes(
    const uint8_t *haystack,
    uint32_t haystack_length,
    const void *needle,
    uint32_t needle_length)
{
    uint32_t index;

    if (haystack == NULL || needle == NULL ||
        needle_length == 0U || needle_length > haystack_length) {
        return NULL;
    }
    for (index = 0U; index + needle_length <= haystack_length; ++index) {
        if (memcmp(haystack + index, needle, needle_length) == 0) {
            return haystack + index;
        }
    }
    return NULL;
}

/*
 * ITEM-445 step 2: the two headers
 the browser sign-in answers with. A
 * response that carries neither is byte-identical to the pre-item head; a
 * response that carries them prints `Set-Cookie` and `Location` in that order
 * and refuses an over-long or dangling value.
 */
static void test_browser_sign_in_headers(void)
{
    static const uint8_t cookie[] =
        "hfoc_session=00112233445566778899aabbccddeeff"
        "00112233445566778899aabbccddeeff; HttpOnly; Secure; "
        "SameSite=Strict; Path=/; Max-Age=900";
    static const uint8_t location[] = "/panel/dashboard";
    struct http_response plain = make_response(
        HTTP_STATUS_303_SEE_OTHER, HTTP_CONTENT_TYPE_HTML_UTF8, NULL, 0U);
    struct http_response with_cookie = plain;
    struct http_response with_both = plain;
    struct http_response dangling = plain;
    struct http_response too_long = plain;
    struct http_response bad_status = plain;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint8_t reference[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;
    uint32_t reference_length = 0U;

    with_cookie.set_cookie = cookie;
    with_cookie.set_cookie_length = (uint32_t)(sizeof(cookie) - 1U);
    with_both = with_cookie;
    with_both.location = location;
    with_both.location_length = (uint32_t)(sizeof(location) - 1U);
    dangling.set_cookie_length = 8U;
    too_long.set_cookie = cookie;
    too_long.set_cookie_length = HTTP_RESPONSE_SET_COOKIE_MAX + 1U;
    bad_status.status = (enum http_status_code)299;

    CHECK(http_response_encode(
              &plain, reference, sizeof(reference), &reference_length) ==
              HTTP_RESPONSE_OK &&
              find_bytes(reference, reference_length, "Set-Cookie", 10U) == NULL &&
              find_bytes(reference, reference_length, "Location:", 9U) == NULL,
          "a response without the browser headers prints neither");
    memset(output, 0, sizeof(output));
    CHECK(http_response_encode(
              &with_cookie, output, sizeof(output), &written) ==
              HTTP_RESPONSE_OK &&
              find_bytes(output, written, "Set-Cookie: ", 12U) != NULL &&
              find_bytes(output, written, cookie, (uint32_t)(sizeof(cookie) - 1U)) != NULL &&
              find_bytes(output, written, "Location:", 9U) == NULL,
          "Set-Cookie is printed with its exact value");
    memset(output, 0, sizeof(output));
    CHECK(http_response_encode(
              &with_both, output, sizeof(output), &written) ==
              HTTP_RESPONSE_OK &&
              find_bytes(output, written, "Set-Cookie: ", 12U) != NULL &&
              find_bytes(output, written, "Location: /panel/dashboard", 26U) !=
                  NULL &&
              find_bytes(output, written, "HTTP/1.1 303 See Other", 21U) != NULL &&
              written == http_response_length(&with_both) &&
              find_bytes(output, written, "form-action 'self'", 18U) != NULL,
          "the sign-in answer prints 303, the cookie, the redirect and the "
          "form policy");
    CHECK(http_response_encode(
              &dangling, output, sizeof(output), &written) ==
              HTTP_RESPONSE_NULL,
          "a cookie length without a cookie value is refused");
    CHECK(http_response_encode(
              &too_long, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BODY_TOO_LARGE,
          "an over-long cookie value is refused");
    CHECK(http_response_encode(
              &bad_status, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BAD_STATUS,
          "an unknown status is still refused");
}

static void test_zero_length_and_max_body(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_JSON,
        NULL,
        0U);
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint8_t body[HTTP_RESPONSE_BODY_MAX];
    uint32_t written = 0U;
    uint32_t length;
    enum http_response_status status;

    memset(output, 0xA5, sizeof(output));
    status = http_response_encode(
        &response, output, sizeof(output), &written);
    length = http_response_length(&response);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == length &&
              length == (uint32_t)strlen(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: 0\r\n"
                  "Connection: close\r\n"
                  "Cache-Control: no-store\r\n"
                  "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
                  "Referrer-Policy: no-referrer\r\n"
                  "X-Content-Type-Options: nosniff\r\n"
                  "X-Frame-Options: DENY\r\n"
                  "\r\n"),
          "zero-length body encodes Content-Length 0 and no body bytes");

    memset(body, 'x', sizeof(body));
    response.body = body;
    response.body_length = sizeof(body);
    memset(output, 0xA5, sizeof(output));
    written = 0U;
    status = http_response_encode(
        &response, output, sizeof(output), &written);
    length = http_response_length(&response);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == length &&
              length == (uint32_t)strlen(
                  "HTTP/1.1 200 OK\r\n"
                  "Content-Type: application/json\r\n"
                  "Content-Length: 2048\r\n"
                  "Connection: close\r\n"
                  "Cache-Control: no-store\r\n"
                  "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
                  "Referrer-Policy: no-referrer\r\n"
                  "X-Content-Type-Options: nosniff\r\n"
                  "X-Frame-Options: DENY\r\n"
                  "\r\n") +
                  (uint32_t)sizeof(body) &&
              memcmp(
                  output + length - sizeof(body),
                  body,
                  sizeof(body)) == 0,
          "maximum body length encodes exact Content-Length and body");
}

static void test_capacity_contract(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t length = http_response_length(&response);
    uint32_t written = UINT32_C(0x12345678);
    enum http_response_status status;

    memset(output, 0xA5, sizeof(output));
    status = http_response_encode(
        &response, output, length - 1U, &written);
    CHECK(status == HTTP_RESPONSE_BUFFER_TOO_SMALL &&
              written == UINT32_C(0x12345678),
          "one-byte-short capacity fails without written mutation");
    {
        uint32_t index;
        bool unchanged = true;

        for (index = 0U; index < sizeof(output); ++index) {
            if (output[index] != 0xA5U) {
                unchanged = false;
                break;
            }
        }
        CHECK(unchanged,
              "one-byte-short capacity fails without output mutation");
    }

    memset(output, 0xA5, sizeof(output));
    written = 0U;
    status = http_response_encode(
        &response, output, length, &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == length &&
              memcmp(output, root_expected,
                     sizeof(root_expected) - 1U) == 0,
          "exact capacity succeeds with exact bytes");
}

static void test_failure_contracts(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = UINT32_C(0x12345678);
    struct http_response invalid = response;

    memset(output, 0xA5, sizeof(output));
    CHECK(http_response_encode(
              NULL, output, sizeof(output), &written) ==
              HTTP_RESPONSE_NULL &&
              written == UINT32_C(0x12345678),
          "null response fails without mutation");
    CHECK(http_response_encode(
              &response, NULL, sizeof(output), &written) ==
              HTTP_RESPONSE_NULL,
          "null output fails");
    CHECK(http_response_encode(
              &response, output, sizeof(output), NULL) ==
              HTTP_RESPONSE_NULL,
          "null written fails");

    invalid.status = (enum http_status_code)99;
    CHECK(http_response_encode(
              &invalid, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BAD_STATUS,
          "unknown status fails");
    invalid = response;
    invalid.content_type = (enum http_content_type)99;
    CHECK(http_response_encode(
              &invalid, output, sizeof(output), &written) ==
              HTTP_RESPONSE_BAD_CONTENT_TYPE,
          "unknown content type fails");

    invalid = response;
    invalid.body = NULL;
    invalid.body_length = 1U;
    CHECK(http_response_length(&invalid) == 0U &&
              http_response_encode(
                  &invalid, output, sizeof(output), &written) ==
                  HTTP_RESPONSE_NULL,
          "null body with nonzero length fails");

    invalid = response;
    invalid.body_length = HTTP_RESPONSE_BODY_MAX + 1U;
    CHECK(http_response_length(&invalid) == 0U &&
              http_response_encode(
                  &invalid, output, sizeof(output), &written) ==
                  HTTP_RESPONSE_BODY_TOO_LARGE,
          "oversized body fails");
}

static void test_determinism_and_no_request_echo(void)
{
    struct http_response response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    uint8_t first[HTTP_RESPONSE_BUFFER_MAX];
    uint8_t second[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t first_written = 0U;
    uint32_t second_written = 0U;
    enum http_response_status first_status;
    enum http_response_status second_status;

    first_status = http_response_encode(
        &response, first, sizeof(first), &first_written);
    second_status = http_response_encode(
        &response, second, sizeof(second), &second_written);
    CHECK(first_status == HTTP_RESPONSE_OK &&
              second_status == HTTP_RESPONSE_OK &&
              first_written == second_written &&
              memcmp(first, second, first_written) == 0,
          "repeated encoding is deterministic and side-effect free");

    {
        static const uint8_t hostile_body[] =
            "ignored\r\nX-Evil: injected";
        uint8_t encoded[HTTP_RESPONSE_BUFFER_MAX];
        uint32_t encoded_written = 0U;
        uint32_t index;
        uint32_t header_end = 0U;
        bool evil_in_headers = false;

        response.body = hostile_body;
        response.body_length =
            (uint32_t)(sizeof(hostile_body) - 1U);
        CHECK(http_response_encode(
                  &response,
                  encoded,
                  sizeof(encoded),
                  &encoded_written) == HTTP_RESPONSE_OK &&
                  encoded_written ==
                      http_response_length(&response),
              "body is opaque and does not change headers");

        for (index = 0U; index + 3U < encoded_written; ++index) {
            if (encoded[index] == '\r' &&
                encoded[index + 1U] == '\n' &&
                encoded[index + 2U] == '\r' &&
                encoded[index + 3U] == '\n') {
                header_end = index;
                break;
            }
        }
        for (index = 0U;
             index + 6U <= header_end;
             ++index) {
            if (memcmp(encoded + index, "X-Evil", 6U) == 0) {
                evil_in_headers = true;
                break;
            }
        }
        CHECK(header_end != 0U && !evil_in_headers,
              "CRLF in fixed body cannot inject a response header");
    }
}

int main(void)
{
    test_string_publication();
    test_exact_fixtures();
    test_head_exact_fixtures();
    test_head_get_equivalence();
    test_head_zero_body_equivalence();
    test_head_repeated_and_short_output_cursor();
    test_head_failure_contracts();
    test_status_lines_and_empty_body();
    test_browser_sign_in_headers();
    test_zero_length_and_max_body();
    test_capacity_contract();
    test_failure_contracts();
    test_determinism_and_no_request_echo();

    if (failures != 0) {
        fprintf(stderr, "HTTP response encoder fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP response encoder host tests: ALL OK\n");
    return 0;
}
