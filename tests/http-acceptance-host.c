#include "http_error_matrix.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
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

static uint32_t random_state = UINT32_C(0x6D2B79F5);

static uint32_t next_random(void)
{
    uint32_t value = random_state;

    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    random_state = value;
    return value;
}

static struct http_parser_token make_token(
    uint32_t index,
    uint32_t generation)
{
    struct http_parser_token token;

    memset(&token, 0, sizeof(token));
    token.index = index;
    token.generation = generation;
    return token;
}

static bool init_parser(struct http_parser *parser)
{
    memset(parser, 0, sizeof(*parser));
    return http_parser_init(parser) == HTTP_PARSER_OK;
}

static bool feed_request_line_one_byte(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        struct http_parser_progress progress;
        bool complete = false;
        enum http_parser_status status;

        status = http_request_line_feed(
            parser,
            token,
            bytes + index,
            1U,
            &complete);
        if (status != HTTP_PARSER_OK ||
            (complete != (index + 1U == length))) {
            return false;
        }
        if (http_parser_progress(
                parser, token, &progress) != HTTP_PARSER_OK ||
            progress.request_line_count != index + 1U ||
            progress.total_count != index + 1U ||
            progress.request_line_count >
                HTTP_PARSER_REQUEST_LINE_MAX) {
            return false;
        }
    }
    return true;
}

static bool advance_to_headers(
    struct http_parser *parser,
    const struct http_parser_token *token)
{
    struct http_parser_progress progress;

    if (http_parser_progress(
            parser, token, &progress) != HTTP_PARSER_OK) {
        return false;
    }
    return http_parser_advance(
               parser,
               token,
               HTTP_PARSER_STATE_HEADERS,
               &(struct http_parser_progress){
                   0U,
                   progress.request_line_count,
                   progress.request_line_count,
                   0U,
                   progress.request_line_count
               }) == HTTP_PARSER_OK;
}

static bool feed_headers_one_byte(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *bytes,
    uint32_t length,
    uint32_t request_line_length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        struct http_parser_progress progress;
        bool complete = false;
        enum http_parser_status status;

        status = http_header_feed(
            parser,
            token,
            bytes + index,
            1U,
            &complete);
        if (status != HTTP_PARSER_OK ||
            (complete != (index + 1U == length))) {
            return false;
        }
        if (http_parser_progress(
                parser, token, &progress) != HTTP_PARSER_OK ||
            progress.header_offset != request_line_length ||
            progress.header_count != index + 1U ||
            progress.total_count != request_line_length + index + 1U ||
            progress.header_count > HTTP_PARSER_HEADERS_MAX) {
            return false;
        }
    }
    return true;
}

static bool feed_request_line_whole(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line,
    bool *complete)
{
    return http_request_line_feed(
               parser,
               token,
               (const uint8_t *)line,
               (uint32_t)strlen(line),
               complete) == HTTP_PARSER_OK;
}

static bool feed_headers_whole(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const uint8_t *headers,
    uint32_t length,
    bool *complete)
{
    return http_header_feed(
               parser,
               token,
               headers,
               length,
               complete) == HTTP_PARSER_OK;
}

static bool parse_valid_syntax(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line,
    const char *headers)
{
    bool complete = false;

    if (!feed_request_line_whole(
            parser, token, line, &complete) || !complete) {
        return false;
    }
    if (!advance_to_headers(parser, token)) {
        return false;
    }
    complete = false;
    if (!feed_headers_whole(
            parser,
            token,
            (const uint8_t *)headers,
            (uint32_t)strlen(headers),
            &complete) || !complete) {
        return false;
    }
    return http_parser_state(parser, token) ==
           HTTP_PARSER_STATE_COMPLETE;
}

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

static const uint8_t root_body[] = "<h1>hfoc</h1>";
static const uint8_t health_body[] =
    "{\"status\":\"ok\",\"route\":\"health\"}";
static const uint8_t rejected_body[] = "rejected";

static const char root_get_expected[] =
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

static const char health_get_expected[] =
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

static const char error_400_expected[] =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static const char error_404_expected[] =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static const char error_405_expected[] =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static const char error_413_expected[] =
    "HTTP/1.1 413 Payload Too Large\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static const char error_414_expected[] =
    "HTTP/1.1 414 URI Too Long\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static const char error_431_expected[] =
    "HTTP/1.1 431 Request Header Fields Too Large\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 8\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "rejected";

static bool check_exact_response(
    const struct http_response *response,
    const char *expected,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX + 8U];
    uint32_t written = 0U;
    uint32_t expected_length = (uint32_t)(strlen(expected));
    enum http_response_status status;

    memset(output, 0xA5, sizeof(output));
    status = http_response_encode(
        response, output, sizeof(output), &written);
    if (status != HTTP_RESPONSE_OK ||
        written != expected_length ||
        memcmp(output, expected, expected_length) != 0U ||
        output[expected_length] != 0xA5U) {
        CHECK(false, message);
        return false;
    }
    return true;
}

static bool check_exact_head_response(
    const struct http_response *response,
    const char *expected,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX + 8U];
    uint32_t written = 0U;
    uint32_t expected_length = (uint32_t)(strlen(expected));
    enum http_response_status status;

    memset(output, 0xA5, sizeof(output));
    status = http_response_encode_head(
        response, output, sizeof(output), &written);
    if (status != HTTP_RESPONSE_OK ||
        written != expected_length ||
        memcmp(output, expected, expected_length) != 0U ||
        output[expected_length] != 0xA5U) {
        CHECK(false, message);
        return false;
    }
    return true;
}

static void test_split_at_every_byte(void)
{
    static const struct {
        const char *line;
        const char *headers;
        enum http_method_kind method;
        enum http_route_id route;
        uint32_t header_count;
    } cases[] = {
        {
            "GET / HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_METHOD_GET,
            HTTP_ROUTE_ROOT,
            1U,
        },
        {
            "GET /health HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_METHOD_GET,
            HTTP_ROUTE_HEALTH,
            1U,
        },
        {
            "HEAD / HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_METHOD_HEAD,
            HTTP_ROUTE_ROOT,
            1U,
        },
        {
            "HEAD /health HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_METHOD_HEAD,
            HTTP_ROUTE_HEALTH,
            1U,
        },
    };
    struct http_parser parser;
    struct http_parser_token tokens[HTTP_PARSER_SLOT_COUNT];
    struct http_request_line line;
    struct http_policy_result policy;
    struct http_framing_result framing;
    struct http_parser_progress progress;
    enum http_route_id route;
    uint32_t count = 0U;
    uint32_t index;
    bool ok = true;

    CHECK(init_parser(&parser) &&
              http_parser_active_count(&parser) == 0U &&
              http_parser_request_line_max(&parser) ==
                  HTTP_PARSER_REQUEST_LINE_MAX &&
              http_parser_headers_max(&parser) ==
                  HTTP_PARSER_HEADERS_MAX,
          "acceptance fixture initializes empty parser descriptor");

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        tokens[index] = make_token(index, index + 1U);
        CHECK(http_parser_start(
                  &parser, &tokens[index]) == HTTP_PARSER_OK,
              "acceptance fixture attaches all four slots");
    }
    CHECK(http_parser_active_count(&parser) ==
              HTTP_PARSER_SLOT_COUNT,
          "acceptance fixture publishes four active parser slots");

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        uint32_t line_length =
            (uint32_t)strlen(cases[index].line);
        uint32_t header_length =
            (uint32_t)strlen(cases[index].headers);

        ok = feed_request_line_one_byte(
                 &parser,
                 &tokens[index],
                 (const uint8_t *)cases[index].line,
                 line_length) &&
             advance_to_headers(&parser, &tokens[index]) &&
             feed_headers_one_byte(
                 &parser,
                 &tokens[index],
                 (const uint8_t *)cases[index].headers,
                 header_length,
                 line_length);
        CHECK(ok,
              "fixed request parses one byte at a time in its own slot");
        if (!ok) {
            continue;
        }
        CHECK(http_parser_state(
                  &parser, &tokens[index]) ==
                  HTTP_PARSER_STATE_COMPLETE &&
                  http_parser_active_count(&parser) ==
                      HTTP_PARSER_SLOT_COUNT &&
                  http_parser_generation(
                      &parser, &tokens[index]) == index + 1U,
              "split fixture keeps COMPLETE state and ownership counters");
        CHECK(http_request_line_get(
                  &parser, &tokens[index], &line) == HTTP_PARSER_OK &&
                  line.line_length == line_length &&
                  line.method_length ==
                      (cases[index].method == HTTP_METHOD_GET
                           ? 3U
                           : 4U) &&
                  line.version_length == 8U,
              "split fixture exposes exact request-line fields");
        CHECK(http_parser_progress(
                  &parser, &tokens[index], &progress) == HTTP_PARSER_OK &&
                  progress.request_line_count == line_length &&
                  progress.header_offset == line_length &&
                  progress.header_count == header_length &&
                  progress.total_count == line_length + header_length,
              "split fixture publishes exact parser progress");
        memset(&framing, 0, sizeof(framing));
        memset(&policy, 0, sizeof(policy));
        route = HTTP_ROUTE_NONE;
        CHECK(http_framing_check(
                  &parser, &tokens[index], &framing) == HTTP_PARSER_OK &&
                  framing.reason == HTTP_FRAMING_NONE &&
                  http_policy_check(
                      &parser, &tokens[index], &policy) == HTTP_PARSER_OK &&
                  policy.allowed &&
                  policy.method == cases[index].method &&
                  policy.reason == HTTP_POLICY_NONE &&
                  http_route_match_line(&line, &route) == HTTP_ROUTE_OK &&
                  route == cases[index].route,
              "split fixture accepts framing, policy, and route evidence");
        CHECK(http_header_count(
                  &parser, &tokens[index], &count) == HTTP_PARSER_OK &&
                  count == cases[index].header_count,
              "split fixture exposes exact completed header count");
    }

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        CHECK(http_parser_release(
                  &parser, &tokens[index]) == HTTP_PARSER_OK,
              "split fixture releases parser slot");
    }
    CHECK(http_parser_active_count(&parser) == 0U,
          "split fixture returns parser ownership counter to zero");
}

static void test_request_line_malformed_corpus(void)
{
    static const struct {
        const char *line;
        enum http_parser_status status;
        enum http_status_code http_status;
    } cases[] = {
        {
            "G\tT / HTTP/1.1\r\n",
            HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET  / HTTP/1.1\r\n",
            HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET index HTTP/1.1\r\n",
            HTTP_PARSER_REQUEST_LINE_INVALID_TARGET,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET / HTTP/1..1\r\n",
            HTTP_PARSER_REQUEST_LINE_INVALID_VERSION,
            HTTP_STATUS_400_BAD_REQUEST,
        },
    };
    struct http_parser parser;
    struct http_parser_token token;
    uint32_t index;

    CHECK(init_parser(&parser),
          "malformed request-line corpus initializes parser");
    token = make_token(0U, 1U);

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        bool complete = false;
        enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;

        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "malformed request-line corpus starts slot");
        CHECK(http_request_line_feed(
                  &parser,
                  &token,
                  (const uint8_t *)cases[index].line,
                  (uint32_t)strlen(cases[index].line),
                  &complete) == cases[index].status &&
                  !complete,
              "malformed request-line corpus latches exact status");
        CHECK(http_request_line_error(
                  &parser, &token) == cases[index].status &&
                  http_error_matrix_map_parser(
                      cases[index].status, &mapped) ==
                      HTTP_ERROR_MATRIX_OK &&
                  mapped == cases[index].http_status,
              "malformed request-line corpus maps to exact HTTP status");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_active_count(&parser) == 0U,
              "malformed request-line corpus releases slot");
    }
}

static void test_request_line_overflow(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    uint8_t overflow[HTTP_PARSER_REQUEST_LINE_MAX + 1U];
    enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;
    bool complete = false;
    uint32_t index;

    memcpy(overflow, "GET /", 5U);
    for (index = 5U;
         index < HTTP_PARSER_REQUEST_LINE_MAX + 1U;
         ++index) {
        overflow[index] = (uint8_t)'a';
    }
    memcpy(
        overflow + HTTP_PARSER_REQUEST_LINE_MAX + 1U - 11U,
        " HTTP/1.1\r\n",
        11U);

    CHECK(init_parser(&parser),
          "request-line overflow fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "request-line overflow fixture starts slot");
    CHECK(http_request_line_feed(
              &parser,
              &token,
              overflow,
              sizeof(overflow),
              &complete) == HTTP_PARSER_REQUEST_LINE_LIMIT &&
              !complete,
          "request-line overflow latches limit status");
    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_REQUEST_LINE_LIMIT, &mapped) ==
              HTTP_ERROR_MATRIX_OK &&
              mapped == HTTP_STATUS_414_URI_TOO_LONG,
          "request-line overflow maps to 414");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "request-line overflow fixture releases slot");
}

static void test_header_malformed_corpus(void)
{
    static const struct {
        const char *headers;
        enum http_parser_status status;
        enum http_status_code http_status;
    } cases[] = {
        {
            "Bad Name: x\r\n\r\n",
            HTTP_PARSER_HEADER_INVALID_NAME,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "X: \x01\r\n\r\n",
            HTTP_PARSER_HEADER_INVALID_VALUE,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            " X: y\r\n\r\n",
            HTTP_PARSER_HEADER_FOLDING,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "X: y\n\r\n",
            HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION,
            HTTP_STATUS_400_BAD_REQUEST,
        },
    };
    struct http_parser parser;
    struct http_parser_token token;
    uint32_t index;

    CHECK(init_parser(&parser),
          "malformed header corpus initializes parser");
    token = make_token(0U, 1U);

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;
        bool complete = false;

        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  feed_request_line_whole(
                      &parser, &token, "GET / HTTP/1.1\r\n", &complete) &&
                  complete &&
                  advance_to_headers(&parser, &token),
              "malformed header corpus enters HEADERS state");
        complete = false;
        CHECK(http_header_feed(
                  &parser,
                  &token,
                  (const uint8_t *)cases[index].headers,
                  (uint32_t)strlen(cases[index].headers),
                  &complete) == cases[index].status &&
                  !complete,
              "malformed header corpus latches exact status");
        CHECK(http_header_error(
                  &parser, &token) == cases[index].status &&
                  http_error_matrix_map_parser(
                      cases[index].status, &mapped) ==
                      HTTP_ERROR_MATRIX_OK &&
                  mapped == cases[index].http_status,
              "malformed header corpus maps to exact HTTP status");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "malformed header corpus releases slot");
    }
}

static void test_header_overflow(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    uint8_t overflow[HTTP_PARSER_HEADERS_MAX + 1U];
    enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;
    bool complete = false;

    memset(overflow, 'A', sizeof(overflow));
    CHECK(init_parser(&parser),
          "header overflow fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line_whole(
                  &parser, &token, "GET / HTTP/1.1\r\n", &complete) &&
              complete &&
              advance_to_headers(&parser, &token),
          "header overflow fixture enters HEADERS state");
    complete = false;
    CHECK(http_header_feed(
              &parser,
              &token,
              overflow,
              sizeof(overflow),
              &complete) == HTTP_PARSER_HEADERS_LIMIT &&
              !complete,
          "header overflow latches limit status");
    CHECK(http_error_matrix_map_parser(
              HTTP_PARSER_HEADERS_LIMIT, &mapped) ==
              HTTP_ERROR_MATRIX_OK &&
              mapped == HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE,
          "header overflow maps to 431");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "header overflow fixture releases slot");
}

static void test_policy_malformed_corpus(void)
{
    static const struct {
        const char *line;
        const char *headers;
        enum http_policy_reason reason;
        enum http_status_code http_status;
    } cases[] = {
        {
            /*
             * ITEM-445 step 2: `POST /` is the browser sign-in form and is
             * accepted by the policy; the frozen 405 fixture moves to
             * `/panel`, which still refuses POST.
             */
            "POST /panel HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_POLICY_METHOD,
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
        },
        {
            "GET / HTTP/1.0\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_POLICY_VERSION,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET / HTTP/1.1\r\n",
            "Accept: text/html\r\n\r\n",
            HTTP_POLICY_MISSING_HOST,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET / HTTP/1.1\r\n",
            "Host:\r\n\r\n",
            HTTP_POLICY_EMPTY_HOST,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET / HTTP/1.1\r\n",
            "Host: hfoc.test\r\n"
            "Host: hfoc.test\r\n"
            "\r\n",
            HTTP_POLICY_DUPLICATE_HOST,
            HTTP_STATUS_400_BAD_REQUEST,
        },
        {
            "GET / HTTP/1.1\r\n",
            "Host: /evil\r\n\r\n",
            HTTP_POLICY_INVALID_HOST,
            HTTP_STATUS_400_BAD_REQUEST,
        },
    };
    struct http_parser parser;
    struct http_parser_token token;
    uint32_t index;

    CHECK(init_parser(&parser),
          "malformed policy corpus initializes parser");
    token = make_token(0U, 1U);

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        struct http_policy_result result;
        enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;

        memset(&result, 0, sizeof(result));
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  parse_valid_syntax(
                      &parser,
                      &token,
                      cases[index].line,
                      cases[index].headers) &&
                  http_policy_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  result.reason == cases[index].reason &&
                  http_error_matrix_map_policy(
                      cases[index].reason, &mapped) ==
                      HTTP_ERROR_MATRIX_OK &&
                  mapped == cases[index].http_status,
              "malformed policy corpus maps to exact HTTP status");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "malformed policy corpus releases slot");
    }

    /*
     * ITEM-445 step 2: the form targets accept POST, and only they.
     * ITEM-503 adds the panel certificate form and its API target to the
     * accepted set, ITEM-504 adds the routing form and its API target,
     * ITEM-505 adds the network form and its API target and its system
     * hostname pair, and ITEM-506 increment 6 adds the account form and its
     * scriptable target; every other target keeps refusing POST.
     */
    {
        static const char *const accepted[] = {
            "POST / HTTP/1.1\r\n",
            "POST /panel/logout HTTP/1.1\r\n",
            "POST /panel/security HTTP/1.1\r\n",
            "POST /api/v1/system/certificate HTTP/1.1\r\n",
            "POST /panel/routing HTTP/1.1\r\n",
            "POST /api/v1/system/routes HTTP/1.1\r\n",
            "POST /panel/network HTTP/1.1\r\n",
            "POST /api/v1/network HTTP/1.1\r\n",
            "POST /panel/system HTTP/1.1\r\n",
            "POST /api/v1/system HTTP/1.1\r\n",
            "POST /panel/account HTTP/1.1\r\n",
            "POST /api/v1/system/account HTTP/1.1\r\n",
        };
        static const char *const refused[] = {
            "POST /panel HTTP/1.1\r\n",
            "POST /health HTTP/1.1\r\n",
            "POST /api/v1 HTTP/1.1\r\n",
        };
        static const char headers[] = "Host: hfoc.test\r\n\r\n";

        for (index = 0U;
             index < sizeof(accepted) / sizeof(accepted[0]);
             ++index) {
            struct http_policy_result result;

            memset(&result, 0, sizeof(result));
            CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                      parse_valid_syntax(
                          &parser, &token, accepted[index], headers) &&
                      http_policy_check(
                          &parser, &token, &result) == HTTP_PARSER_OK &&
                      result.allowed && result.method_allowed &&
                      result.method == HTTP_METHOD_POST,
                  "the sign-in form target accepts POST");
            CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
                  "the sign-in form target releases its slot");
        }
        for (index = 0U;
             index < sizeof(refused) / sizeof(refused[0]);
             ++index) {
            struct http_policy_result result;

            memset(&result, 0, sizeof(result));
            CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                      parse_valid_syntax(
                          &parser, &token, refused[index], headers) &&
                      http_policy_check(
                          &parser, &token, &result) == HTTP_PARSER_OK &&
                      !result.allowed && !result.method_allowed &&
                      result.reason == HTTP_POLICY_METHOD,
                  "a non-form target still refuses POST");
            CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
                  "the non-form target releases its slot");
        }
    }
}

static void test_framing_malformed_corpus(void)
{
    static const struct {
        const char *headers;
        enum http_framing_kind reason;
    } cases[] = {
        {
            "Content-Length: 0\r\n\r\n",
            HTTP_FRAMING_CONTENT_LENGTH,
        },
        {
            "Transfer-Encoding: chunked\r\n\r\n",
            HTTP_FRAMING_TRANSFER_ENCODING,
        },
        {
            "Expect: 100-continue\r\n\r\n",
            HTTP_FRAMING_EXPECT,
        },
        {
            "Upgrade: h2c\r\n\r\n",
            HTTP_FRAMING_UPGRADE,
        },
        {
            "Content-Length: 0\r\n"
            "Content-Length: 0\r\n"
            "\r\n",
            HTTP_FRAMING_DUPLICATE,
        },
        {
            "Content-Length: 5\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n",
            HTTP_FRAMING_CONFLICT,
        },
        {
            "Content-Length: abc\r\n\r\n",
            HTTP_FRAMING_INVALID,
        },
    };
    struct http_parser parser;
    struct http_parser_token token;
    uint32_t index;

    CHECK(init_parser(&parser),
          "malformed framing corpus initializes parser");
    token = make_token(0U, 1U);

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        struct http_framing_result result;
        enum http_status_code mapped = HTTP_STATUS_404_NOT_FOUND;

        memset(&result, 0, sizeof(result));
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  parse_valid_syntax(
                      &parser,
                      &token,
                      "GET / HTTP/1.1\r\n",
                      cases[index].headers) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  result.reason == cases[index].reason &&
                  http_error_matrix_map_framing(
                      &result, &mapped) == HTTP_ERROR_MATRIX_OK &&
                  mapped == HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
              "malformed framing corpus maps to 413");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "malformed framing corpus releases slot");
    }
}

static void test_unknown_route(void)
{
    static const char line[] = "GET /missing HTTP/1.1\r\n";
    static const char headers[] = "Host: hfoc.test\r\n\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line request_line;
    struct http_policy_result policy;
    enum http_route_id route = HTTP_ROUTE_ROOT;
    enum http_status_code mapped = HTTP_STATUS_413_PAYLOAD_TOO_LARGE;

    CHECK(init_parser(&parser),
          "unknown route fixture initializes parser");
    token = make_token(0U, 1U);
    memset(&policy, 0, sizeof(policy));
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              parse_valid_syntax(&parser, &token, line, headers) &&
              http_policy_check(
                  &parser, &token, &policy) == HTTP_PARSER_OK &&
              policy.allowed &&
              http_request_line_get(
                  &parser, &token, &request_line) == HTTP_PARSER_OK &&
              http_route_match_line(
                  &request_line, &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_NONE &&
              http_error_matrix_map_route(
                  route, &mapped) == HTTP_ERROR_MATRIX_OK &&
              mapped == HTTP_STATUS_404_NOT_FOUND,
          "unknown route maps to 404");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "unknown route fixture releases slot");
}

static void test_exact_success_and_error_bytes(void)
{
    static const struct {
        enum http_status_code status;
        const char *expected;
    } errors[] = {
        {
            HTTP_STATUS_400_BAD_REQUEST,
            error_400_expected,
        },
        {
            HTTP_STATUS_404_NOT_FOUND,
            error_404_expected,
        },
        {
            HTTP_STATUS_405_METHOD_NOT_ALLOWED,
            error_405_expected,
        },
        {
            HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
            error_413_expected,
        },
        {
            HTTP_STATUS_414_URI_TOO_LONG,
            error_414_expected,
        },
        {
            HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE,
            error_431_expected,
        },
    };
    struct http_response response;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX + 8U];
    uint32_t written = 0xDEADBEEFU;
    uint32_t index;

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_HTML_UTF8,
        root_body,
        (uint32_t)(sizeof(root_body) - 1U));
    CHECK(check_exact_response(
              &response, root_get_expected, "root GET bytes are exact"),
          "root GET bytes are exact");
    CHECK(check_exact_head_response(
              &response,
              root_head_expected,
              "root HEAD bytes are exact with zero body"),
          "root HEAD bytes are exact with zero body");

    response = make_response(
        HTTP_STATUS_200_OK,
        HTTP_CONTENT_TYPE_JSON,
        health_body,
        (uint32_t)(sizeof(health_body) - 1U));
    CHECK(check_exact_response(
              &response,
              health_get_expected,
              "health GET bytes are exact"),
          "health GET bytes are exact");
    CHECK(check_exact_head_response(
              &response,
              health_head_expected,
              "health HEAD bytes are exact with zero body"),
          "health HEAD bytes are exact with zero body");

    response = make_response(
        HTTP_STATUS_400_BAD_REQUEST,
        HTTP_CONTENT_TYPE_JSON,
        rejected_body,
        (uint32_t)(sizeof(rejected_body) - 1U));
    memset(output, 0xA5, sizeof(output));
    CHECK(http_response_encode(
              &response,
              output,
              (uint32_t)(strlen(error_400_expected) - 1U),
              &written) == HTTP_RESPONSE_BUFFER_TOO_SMALL &&
              written == 0xDEADBEEFU,
          "one-byte-short error output is not mutated");

    for (index = 0U;
         index < sizeof(errors) / sizeof(errors[0]);
         ++index) {
        response = make_response(
            errors[index].status,
            HTTP_CONTENT_TYPE_JSON,
            rejected_body,
            (uint32_t)(sizeof(rejected_body) - 1U));
        CHECK(check_exact_response(
                  &response,
                  errors[index].expected,
                  "fixed error response bytes are exact"),
              "fixed error response bytes are exact");
    }
}

static bool fuzz_parser_iteration(uint32_t iteration)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    uint8_t line_bytes[64U];
    uint8_t header_bytes[128U];
    uint32_t line_length;
    uint32_t header_length;
    uint32_t offset = 0U;
    bool request_complete = false;
    bool header_complete = false;
    bool ok = true;

    line_length = 1U + next_random() % sizeof(line_bytes);
    header_length = 1U + next_random() % sizeof(header_bytes);
    for (offset = 0U; offset < line_length; ++offset) {
        line_bytes[offset] =
            (uint8_t)(next_random() % 0x80U);
    }
    for (offset = 0U; offset < header_length; ++offset) {
        header_bytes[offset] =
            (uint8_t)(next_random() % 0x80U);
    }

    if (!init_parser(&parser)) {
        return false;
    }
    token = make_token(
        (uint32_t)(iteration & 1U),
        (uint32_t)(1U + (iteration & 1U)));
    if (http_parser_start(&parser, &token) != HTTP_PARSER_OK ||
        http_parser_active_count(&parser) != 1U) {
        return false;
    }

    offset = 0U;
    while (offset < line_length && !request_complete) {
        uint32_t chunk = 1U + next_random() % 8U;
        bool complete = false;
        enum http_parser_status status;

        if (chunk > line_length - offset) {
            chunk = line_length - offset;
        }
        status = http_request_line_feed(
            &parser,
            &token,
            line_bytes + offset,
            chunk,
            &complete);
        if (status == HTTP_PARSER_OK) {
            if (complete) {
                request_complete = true;
                break;
            }
            offset += chunk;
        } else {
            if (http_parser_status_string(status) == NULL) {
                ok = false;
            }
            break;
        }
    }

    if (ok && request_complete) {
        if (!advance_to_headers(&parser, &token)) {
            ok = false;
        } else {
            offset = 0U;
            while (offset < header_length && !header_complete) {
                uint32_t chunk = 1U + next_random() % 8U;
                bool complete = false;
                enum http_parser_status status;

                if (chunk > header_length - offset) {
                    chunk = header_length - offset;
                }
                status = http_header_feed(
                    &parser,
                    &token,
                    header_bytes + offset,
                    chunk,
                    &complete);
                if (status == HTTP_PARSER_OK) {
                    if (complete) {
                        header_complete = true;
                        break;
                    }
                    offset += chunk;
                } else {
                    if (http_parser_status_string(status) == NULL) {
                        ok = false;
                    }
                    break;
                }
            }
        }
    }

    if (http_parser_progress(
            &parser, &token, &progress) != HTTP_PARSER_OK ||
        progress.request_line_count > HTTP_PARSER_REQUEST_LINE_MAX ||
        progress.header_count > HTTP_PARSER_HEADERS_MAX ||
        progress.total_count >
            HTTP_PARSER_REQUEST_LINE_MAX +
            HTTP_PARSER_HEADERS_MAX) {
        ok = false;
    }
    if (http_parser_active_count(&parser) != 1U) {
        ok = false;
    }
    if (http_parser_release(&parser, &token) != HTTP_PARSER_OK ||
        http_parser_active_count(&parser) != 0U) {
        ok = false;
    }
    return ok;
}

static void test_bounded_parser_fuzz(void)
{
    uint32_t iteration;
    uint32_t failed = 0U;

    for (iteration = 0U; iteration < 4096U; ++iteration) {
        if (!fuzz_parser_iteration(iteration)) {
            ++failed;
            if (failed <= 5U) {
                fprintf(
                    stderr,
                    "FAIL: bounded parser fuzz iteration %u\n",
                    iteration);
            }
        }
    }
    CHECK(failed == 0U,
          "bounded parser fuzz iterations preserve invariants");
}

static bool fuzz_encoder_iteration(uint32_t iteration)
{
    static const enum http_status_code statuses[] = {
        HTTP_STATUS_200_OK,
        HTTP_STATUS_400_BAD_REQUEST,
        HTTP_STATUS_404_NOT_FOUND,
        HTTP_STATUS_405_METHOD_NOT_ALLOWED,
        HTTP_STATUS_413_PAYLOAD_TOO_LARGE,
        HTTP_STATUS_414_URI_TOO_LONG,
        HTTP_STATUS_431_HEADER_FIELDS_TOO_LARGE,
    };
    static const enum http_content_type content_types[] = {
        HTTP_CONTENT_TYPE_HTML_UTF8,
        HTTP_CONTENT_TYPE_JSON,
    };
    static uint8_t body[HTTP_RESPONSE_BODY_MAX];
    struct http_response response;
    uint8_t expected[HTTP_RESPONSE_BUFFER_MAX + 8U];
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX + 8U];
    uint32_t body_length =
        next_random() % (HTTP_RESPONSE_BODY_MAX + 1U);
    uint32_t capacity;
    uint32_t expected_length;
    uint32_t written = 0xDEADBEEFU;
    uint32_t index;
    bool head = (iteration & 1U) != 0U;
    enum http_response_status status;

    for (index = 0U; index < body_length; ++index) {
        body[index] = (uint8_t)next_random();
    }
    response = make_response(
        statuses[next_random() %
                     (sizeof(statuses) / sizeof(statuses[0]))],
        content_types[next_random() %
                          (sizeof(content_types) /
                           sizeof(content_types[0]))],
        body_length == 0U ? NULL : body,
        body_length);
    expected_length = head
        ? http_response_head_length(&response)
        : http_response_length(&response);
    memset(expected, 0xA5, sizeof(expected));
    memset(output, 0xA5, sizeof(output));
    if (head) {
        status = http_response_encode_head(
            &response,
            expected,
            sizeof(expected),
            &written);
    } else {
        status = http_response_encode(
            &response,
            expected,
            sizeof(expected),
            &written);
    }
    if (status != HTTP_RESPONSE_OK ||
        written != expected_length ||
        expected[expected_length] != 0xA5U) {
        return false;
    }

    written = 0xDEADBEEFU;
    memset(output, 0xA5, sizeof(output));
    capacity = next_random() % (expected_length + 4U);
    if (head) {
        status = http_response_encode_head(
            &response,
            output,
            capacity,
            &written);
    } else {
        status = http_response_encode(
            &response,
            output,
            capacity,
            &written);
    }
    if (capacity >= expected_length) {
        return status == HTTP_RESPONSE_OK &&
               written == expected_length &&
               memcmp(output, expected, expected_length) == 0U &&
               output[expected_length] == 0xA5U;
    }
    if (status != HTTP_RESPONSE_BUFFER_TOO_SMALL ||
        written != 0xDEADBEEFU) {
        return false;
    }
    for (index = 0U; index < sizeof(output); ++index) {
        if (output[index] != 0xA5U) {
            return false;
        }
    }
    return true;
}

static void test_bounded_encoder_fuzz(void)
{
    uint32_t iteration;
    uint32_t failed = 0U;

    for (iteration = 0U; iteration < 1024U; ++iteration) {
        if (!fuzz_encoder_iteration(iteration)) {
            ++failed;
            if (failed <= 5U) {
                fprintf(
                    stderr,
                    "FAIL: bounded encoder fuzz iteration %u\n",
                    iteration);
            }
        }
    }
    CHECK(failed == 0U,
          "bounded encoder fuzz iterations preserve output ownership");
}

int main(void)
{
    test_split_at_every_byte();
    test_request_line_malformed_corpus();
    test_request_line_overflow();
    test_header_malformed_corpus();
    test_header_overflow();
    test_policy_malformed_corpus();
    test_framing_malformed_corpus();
    test_unknown_route();
    test_exact_success_and_error_bytes();
    test_bounded_parser_fuzz();
    test_bounded_encoder_fuzz();

    if (failures != 0) {
        fprintf(stderr, "HTTP parser/encoder acceptance failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP parser/encoder host acceptance: ALL OK\n");
    return 0;
}
