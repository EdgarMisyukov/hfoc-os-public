#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_REQUEST_LINE_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

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

static bool line_fields_equal(
    const struct http_request_line *line,
    const char *method,
    const char *target,
    const char *version,
    uint32_t line_length)
{
    return line != NULL &&
           method != NULL &&
           target != NULL &&
           version != NULL &&
           line->method_length == strlen(method) &&
           memcmp(line->method, method, line->method_length) == 0 &&
           line->target_length == strlen(target) &&
           memcmp(line->target, target, line->target_length) == 0 &&
           line->version_length == strlen(version) &&
           memcmp(line->version, version, line->version_length) == 0 &&
           line->line_length == line_length;
}

static bool feed_split(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line,
    uint32_t step)
{
    uint32_t length = (uint32_t)strlen(line);
    uint32_t offset;

    for (offset = 0U; offset < length; offset += step) {
        uint32_t chunk = length - offset;
        bool complete = false;
        enum http_parser_status status;

        if (chunk > step) {
            chunk = step;
        }
        status = http_request_line_feed(
            parser,
            token,
            (const uint8_t *)(line + offset),
            chunk,
            &complete);
        if (status != HTTP_PARSER_OK) {
            return false;
        }
        if (offset + chunk >= length && !complete) {
            return false;
        }
        if (offset + chunk < length && complete) {
            return false;
        }
    }
    return true;
}

static void test_status_strings(void)
{
    CHECK(http_parser_status_string(
              HTTP_PARSER_REQUEST_LINE_INCOMPLETE) != NULL,
          "request-line incomplete status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER) != NULL,
          "request-line invalid-character status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_REQUEST_LINE_INVALID_SPACING) != NULL,
          "request-line invalid-spacing status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_REQUEST_LINE_INVALID_TARGET) != NULL,
          "request-line invalid-target status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_REQUEST_LINE_INVALID_VERSION) != NULL,
          "request-line invalid-version status string is defined");
}

static void test_preconditions_and_incomplete(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token wrong;
    struct http_request_line line;
    uint8_t byte = 'G';
    bool complete = false;

    CHECK(init_parser(&parser),
          "request-line fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "request-line fixture starts slot zero");
    CHECK(http_request_line_feed(
              &parser, &token, NULL, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "request-line feed rejects null bytes");
    CHECK(http_request_line_feed(
              &parser, &token, &byte, 1U, NULL) ==
              HTTP_PARSER_NULL,
          "request-line feed rejects null completion output");
    CHECK(http_request_line_feed(
              NULL, &token, &byte, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "request-line feed rejects null parser");
    CHECK(http_request_line_feed(
              &parser, NULL, &byte, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "request-line feed rejects null token");
    CHECK(http_request_line_error(&parser, &token) == HTTP_PARSER_OK,
          "incomplete request-line has no latched error");
    CHECK(http_request_line_get(
              &parser, &token, &line) ==
              HTTP_PARSER_REQUEST_LINE_INCOMPLETE,
          "request-line get rejects incomplete line");

    wrong = make_token(0U, 2U);
    CHECK(http_request_line_feed(
              &parser, &wrong, &byte, 1U, &complete) ==
              HTTP_PARSER_STALE_TOKEN,
          "request-line feed rejects stale generation");
    CHECK(http_request_line_get(
              &parser, &wrong, &line) ==
              HTTP_PARSER_STALE_TOKEN,
          "request-line get rejects stale generation");
    CHECK(http_request_line_error(
              &parser, &wrong) == HTTP_PARSER_STALE_TOKEN,
          "request-line error rejects stale generation");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_EMPTY,
          "stale request-line feed did not mutate slot");
}

static void test_exact_and_split_parsing(void)
{
    static const char *get_line = "GET / HTTP/1.1\r\n";
    static const char *head_line = "HEAD /health HTTP/1.1\r\n";
    static const char *post_line = "POST / HTTP/9.9\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line line;
    struct http_parser_progress progress;
    uint32_t step;

    CHECK(init_parser(&parser),
          "exact parsing fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "exact parsing fixture starts slot zero");
    CHECK(feed_split(
              &parser, &token, get_line, (uint32_t)strlen(get_line)),
          "exact GET request line parses in one feed");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK &&
              line_fields_equal(
                  &line, "GET", "/", "HTTP/1.1",
                  (uint32_t)strlen(get_line)),
          "exact GET request line exposes exact fields");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_progress(
                  &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.request_line_count ==
                  (uint32_t)strlen(get_line) &&
              progress.header_offset == 0U &&
              progress.header_count == 0U &&
              progress.total_count == (uint32_t)strlen(get_line),
          "exact GET request line publishes exact item 271 progress");

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "exact parsing fixture releases first slot");
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "exact parsing fixture restarts slot zero");
    for (step = 1U; step <= 5U; ++step) {
        CHECK(feed_split(&parser, &token, get_line, step),
              "GET request line parses across split step");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "split fixture releases slot between steps");
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "split fixture restarts slot between steps");
    }

    CHECK(feed_split(
              &parser, &token, head_line, (uint32_t)strlen(head_line)),
          "HEAD request line parses exactly");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK &&
              line_fields_equal(
                  &line, "HEAD", "/health", "HTTP/1.1",
                  (uint32_t)strlen(head_line)),
          "HEAD request line exposes exact fields");

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "HEAD fixture releases slot");
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "HEAD fixture restarts slot");
    CHECK(feed_split(
              &parser, &token, post_line, (uint32_t)strlen(post_line)),
          "syntax-only POST/HTTP 9.9 request line parses");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK &&
              line_fields_equal(
                  &line, "POST", "/", "HTTP/9.9",
                  (uint32_t)strlen(post_line)),
          "syntax-only parser leaves method/version policy to item 275");
}

static void test_control_and_spacing_rejection(void)
{
    static const char *cases[] = {
        "G\tT / HTTP/1.1\r\n",
        "GET /\x01 HTTP/1.1\r\n",
        "GET / HTTP/1.1\n",
        "GET /\x7f HTTP/1.1\r\n",
    };
    static const char *spacing_cases[] = {
        " GET / HTTP/1.1\r\n",
        "GET  / HTTP/1.1\r\n",
        "GET  HTTP/1.1\r\n",
        "GET /  HTTP/1.1\r\n",
        "GET / HTTP/1.1 \r\n",
        "GET / HTTP/1.1\rX",
    };
    enum http_parser_status expected[] = {
        HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
        HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
        HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
        HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
    };
    enum http_parser_status spacing_expected[] = {
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
        HTTP_PARSER_REQUEST_LINE_INVALID_SPACING,
    };
    struct http_parser parser;
    struct http_parser_token token;
    bool complete;
    uint32_t index;

    CHECK(init_parser(&parser),
          "character fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "character fixture starts slot zero");

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        complete = false;
        CHECK(http_request_line_feed(
                  &parser,
                  &token,
                  (const uint8_t *)cases[index],
                  (uint32_t)strlen(cases[index]),
                  &complete) == expected[index] &&
                  !complete,
              "request-line control byte rejects deterministically");
        CHECK(http_request_line_error(&parser, &token) ==
                  expected[index],
              "control-byte error latches for slot");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "character fixture releases slot after rejection");
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "character fixture restarts slot after rejection");
    }

    for (index = 0U;
         index < sizeof(spacing_cases) / sizeof(spacing_cases[0]);
         ++index) {
        complete = false;
        CHECK(http_request_line_feed(
                  &parser,
                  &token,
                  (const uint8_t *)spacing_cases[index],
                  (uint32_t)strlen(spacing_cases[index]),
                  &complete) == spacing_expected[index] &&
                  !complete,
              "request-line spacing error rejects deterministically");
        CHECK(http_request_line_error(&parser, &token) ==
                  spacing_expected[index],
              "spacing error latches for slot");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "spacing fixture releases slot after rejection");
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "spacing fixture restarts slot after rejection");
    }
}

static void test_target_and_version_rejection(void)
{
    static const char *target_cases[] = {
        "GET index HTTP/1.1\r\n",
        "GET /health#frag HTTP/1.1\r\n",
        "GET /\xC3\xA9 HTTP/1.1\r\n",
    };
    static const char *version_cases[] = {
        "GET / HTTP/1\r\n",
        "GET / HTTP/1.1x\r\n",
        "GET / HTTP/x.1\r\n",
        "GET / HTTP/1.11\r\n",
        "GET / HTTP/1..1\r\n",
        "GET / HTTP\r\n",
        "GET / HTTP/1\xC3.1\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line line;
    bool complete;
    uint32_t index;

    CHECK(init_parser(&parser),
          "target fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "target fixture starts slot zero");
    for (index = 0U;
         index < sizeof(target_cases) / sizeof(target_cases[0]);
         ++index) {
        complete = false;
        CHECK(http_request_line_feed(
                  &parser,
                  &token,
                  (const uint8_t *)target_cases[index],
                  (uint32_t)strlen(target_cases[index]),
                  &complete) ==
                  HTTP_PARSER_REQUEST_LINE_INVALID_TARGET &&
                  !complete,
              "invalid request target rejects deterministically");
        CHECK(http_request_line_get(
                  &parser, &token, &line) ==
                  HTTP_PARSER_REQUEST_LINE_INVALID_TARGET,
              "invalid request target getter rejects");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "target fixture releases slot after rejection");
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "target fixture restarts slot after rejection");
    }

    for (index = 0U;
         index < sizeof(version_cases) / sizeof(version_cases[0]);
         ++index) {
        complete = false;
        CHECK(http_request_line_feed(
                  &parser,
                  &token,
                  (const uint8_t *)version_cases[index],
                  (uint32_t)strlen(version_cases[index]),
                  &complete) ==
                  HTTP_PARSER_REQUEST_LINE_INVALID_VERSION &&
                  !complete,
              "invalid HTTP version rejects deterministically");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "version fixture releases slot after rejection");
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "version fixture restarts slot after rejection");
    }
}

static void test_limit_boundary(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line line;
    struct http_parser_progress progress;
    uint8_t exact[HTTP_PARSER_REQUEST_LINE_MAX];
    uint8_t overflow[HTTP_PARSER_REQUEST_LINE_MAX + 1U];
    bool complete;
    enum http_parser_status status;

    memcpy(exact, "GET /", 5U);
    memset(exact + 5U, 'a', 1008U);
    memcpy(exact + 1013U, " HTTP/1.1\r\n", 11U);

    CHECK(init_parser(&parser),
          "limit fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "limit fixture starts slot zero");
    complete = false;
    CHECK(http_request_line_feed(
              &parser,
              &token,
              exact,
              sizeof(exact),
              &complete) == HTTP_PARSER_OK &&
              complete,
          "1024-byte request line is accepted at exact boundary");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK &&
              line.method_length == 3U &&
              line.target_length == 1009U &&
              line.version_length == 8U &&
              line.line_length == HTTP_PARSER_REQUEST_LINE_MAX,
          "1024-byte request line exposes exact fields");
    CHECK(http_parser_progress(
              &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.request_line_count ==
                  HTTP_PARSER_REQUEST_LINE_MAX,
          "1024-byte request line publishes exact progress");

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "limit fixture releases exact-boundary slot");
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "limit fixture restarts overflow slot");

    memcpy(overflow, "GET /", 5U);
    memset(overflow + 5U, 'a', 1009U);
    memcpy(overflow + 1014U, " HTTP/1.1\r\n", 11U);
    complete = false;
    status = http_request_line_feed(
        &parser,
        &token,
        overflow,
        sizeof(overflow),
        &complete);
    CHECK(status == HTTP_PARSER_REQUEST_LINE_LIMIT && !complete,
          "1025-byte request line rejects as overflow");
    CHECK(http_request_line_error(&parser, &token) ==
              HTTP_PARSER_REQUEST_LINE_LIMIT,
          "request-line overflow latches limit error");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_progress(
                  &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.request_line_count ==
                  HTTP_PARSER_REQUEST_LINE_MAX,
          "overflow publishes accepted 1024-byte prefix progress");
    CHECK(http_parser_reject(
              &parser,
              &token,
              &(struct http_parser_progress){
                  0U,
                  HTTP_PARSER_REQUEST_LINE_MAX,
                  0U,
                  0U,
                  HTTP_PARSER_REQUEST_LINE_MAX
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_REJECTED,
          "overflow slot can publish exact REJECTED state");
}

static void test_error_lockout_and_isolation(void)
{
    static const char *bad = "G\tT / HTTP/1.1\r\n";
    static const char *good = "GET / HTTP/1.1\r\n";
    struct http_parser parser;
    struct http_parser_token first;
    struct http_parser_token second;
    struct http_parser_progress progress;
    struct http_request_line line;
    bool complete;

    CHECK(init_parser(&parser),
          "isolation fixture initializes parser");
    first = make_token(0U, 1U);
    second = make_token(1U, 2U);
    CHECK(http_parser_start(&parser, &first) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &second) == HTTP_PARSER_OK &&
              parser.active_count == 2U,
          "isolation fixture attaches two slots");

    complete = false;
    CHECK(http_request_line_feed(
              &parser,
              &first,
              (const uint8_t *)bad,
              (uint32_t)strlen(bad),
              &complete) ==
              HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
          "isolation fixture latches first slot failure");
    complete = false;
    CHECK(http_request_line_feed(
              &parser,
              &first,
              (const uint8_t *)bad,
              (uint32_t)strlen(bad),
              &complete) ==
              HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER &&
              !complete,
          "failed request-line slot rejects repeated feed");
    CHECK(http_request_line_get(
              &parser, &first, &line) ==
              HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER,
          "failed request-line getter stays latched");
    CHECK(http_parser_advance(
              &parser,
              &first,
              HTTP_PARSER_STATE_HEADERS,
              &(struct http_parser_progress){
                  0U, 1U, 1U, 0U, 1U
              }) == HTTP_PARSER_REQUEST_LINE_INVALID_CHARACTER &&
              http_parser_state(&parser, &first) ==
                  HTTP_PARSER_STATE_REQUEST_LINE,
          "failed request-line slot cannot advance");
    CHECK(http_parser_state(&parser, &first) ==
              HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_progress(
                  &parser, &first, &progress) == HTTP_PARSER_OK &&
              progress.request_line_count == 1U,
          "failed first slot publishes accepted prefix progress");

    CHECK(feed_split(
              &parser, &second, good, (uint32_t)strlen(good)),
          "unrelated second slot parses while first slot is failed");
    CHECK(http_parser_state(&parser, &second) ==
              HTTP_PARSER_STATE_REQUEST_LINE &&
              http_parser_active_count(&parser) == 2U,
          "failed slot does not disturb unrelated active slot");

    CHECK(http_parser_reject(
              &parser,
              &first,
              &(struct http_parser_progress){
                  0U, 1U, 0U, 0U, 1U
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &first) ==
                  HTTP_PARSER_STATE_REJECTED,
          "failed request-line slot publishes REJECTED explicitly");
    CHECK(http_request_line_feed(
              &parser,
              &first,
              (const uint8_t *)good,
              (uint32_t)strlen(good),
              &complete) == HTTP_PARSER_TERMINAL,
          "REJECTED request-line slot rejects further feed");

    CHECK(http_parser_release(&parser, &first) == HTTP_PARSER_OK &&
              parser.active_count == 1U,
          "release removes failed request-line slot");
    CHECK(http_parser_start(&parser, &first) == HTTP_PARSER_OK &&
              feed_split(
                  &parser,
                  &first,
                  good,
                  (uint32_t)strlen(good)),
          "released request-line slot parses after new generation");
}

int main(void)
{
    test_status_strings();
    test_preconditions_and_incomplete();
    test_exact_and_split_parsing();
    test_control_and_spacing_rejection();
    test_target_and_version_rejection();
    test_limit_boundary();
    test_error_lockout_and_isolation();

    if (failures != 0) {
        fprintf(stderr, "HTTP request line fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP request line host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_request_line_unused;

#endif
