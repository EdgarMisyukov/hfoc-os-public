#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_PARSER_HOST_TEST) && \
    defined(HTTP_REQUEST_LINE_HOST_TEST) && \
    defined(HTTP_HEADER_HOST_TEST)

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

static bool feed_request_line(
    struct http_parser *parser,
    const struct http_parser_token *token)
{
    static const char line[] = "GET / HTTP/1.1\r\n";
    struct http_parser_progress progress;
    bool complete = false;

    if (http_request_line_feed(
            parser,
            token,
            (const uint8_t *)line,
            (uint32_t)strlen(line),
            &complete) != HTTP_PARSER_OK ||
        !complete) {
        return false;
    }
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

static bool header_field_equal(
    const struct http_header_field *field,
    const char *name,
    const char *value)
{
    return field != NULL &&
           name != NULL &&
           value != NULL &&
           field->name_length == strlen(name) &&
           memcmp(field->name, name, field->name_length) == 0 &&
           field->value_length == strlen(value) &&
           memcmp(field->value, value, field->value_length) == 0;
}

static bool feed_split(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *headers,
    uint32_t step)
{
    uint32_t length = (uint32_t)strlen(headers);
    uint32_t offset;

    for (offset = 0U; offset < length; offset += step) {
        uint32_t chunk = length - offset;
        bool complete = false;
        enum http_parser_status status;

        if (chunk > step) {
            chunk = step;
        }
        status = http_header_feed(
            parser,
            token,
            (const uint8_t *)(headers + offset),
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
              HTTP_PARSER_HEADER_INCOMPLETE) != NULL,
          "header incomplete status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_HEADER_INVALID_NAME) != NULL,
          "header invalid-name status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_HEADER_INVALID_VALUE) != NULL,
          "header invalid-value status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_HEADER_FOLDING) != NULL,
          "header folding status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION) != NULL,
          "header incomplete-termination status string is defined");
    CHECK(http_parser_status_string(
              HTTP_PARSER_HEADER_OUT_OF_RANGE) != NULL,
          "header out-of-range status string is defined");
}

static void test_preconditions_and_incomplete(void)
{
    static const char headers[] = "Host: hfoc.test\r\n\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token wrong;
    struct http_header_field field;
    uint8_t byte = 'H';
    uint32_t count = 99U;
    bool complete = false;

    CHECK(init_parser(&parser),
          "header fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "header fixture starts slot zero");
    CHECK(http_header_feed(
              &parser, &token, &byte, 1U, &complete) ==
              HTTP_PARSER_REQUEST_LINE_INCOMPLETE,
          "header feed rejects missing request line");

    CHECK(feed_request_line(&parser, &token),
          "header fixture completes request line and HEADERS state");
    CHECK(http_header_feed(
              &parser, &token, NULL, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "header feed rejects null bytes");
    CHECK(http_header_feed(
              &parser, &token, &byte, 1U, NULL) ==
              HTTP_PARSER_NULL,
          "header feed rejects null completion output");
    CHECK(http_header_feed(
              NULL, &token, &byte, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "header feed rejects null parser");
    CHECK(http_header_feed(
              &parser, NULL, &byte, 1U, &complete) ==
              HTTP_PARSER_NULL,
          "header feed rejects null token");
    CHECK(http_header_error(&parser, &token) == HTTP_PARSER_OK,
          "incomplete header section has no latched error");
    CHECK(http_header_count(
              &parser, &token, &count) ==
              HTTP_PARSER_HEADER_INCOMPLETE &&
              count == 99U,
          "header count rejects incomplete section");
    CHECK(http_header_field(
              &parser, &token, 0U, &field) ==
              HTTP_PARSER_HEADER_INCOMPLETE,
          "header field rejects incomplete section");

    wrong = make_token(0U, 2U);
    CHECK(http_header_feed(
              &parser, &wrong, &byte, 1U, &complete) ==
              HTTP_PARSER_STALE_TOKEN,
          "header feed rejects stale generation");
    CHECK(http_header_count(
              &parser, &wrong, &count) ==
              HTTP_PARSER_STALE_TOKEN,
          "header count rejects stale generation");
    CHECK(http_header_field(
              &parser, &wrong, 0U, &field) ==
              HTTP_PARSER_STALE_TOKEN,
          "header field rejects stale generation");
    CHECK(http_header_error(
              &parser, &wrong) == HTTP_PARSER_STALE_TOKEN,
          "header error rejects stale generation");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_HEADERS,
          "stale header feed did not mutate slot");

    complete = false;
    CHECK(http_header_feed(
              &parser,
              &token,
              (const uint8_t *)headers,
              (uint32_t)strlen(headers),
              &complete) == HTTP_PARSER_OK &&
              complete,
          "header fixture completes exact header section");
    CHECK(http_header_feed(
              &parser, &token, &byte, 1U, &complete) ==
              HTTP_PARSER_TERMINAL,
          "COMPLETE header slot rejects second feed");
}

static void test_exact_and_split_parsing(void)
{
    static const char headers[] =
        "Host: hfoc.test\r\n"
        "Accept: text/html\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    struct http_header_field field;
    uint32_t count = 0U;
    uint32_t step;

    CHECK(init_parser(&parser),
          "exact header fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "exact header fixture starts slot zero");
    CHECK(feed_request_line(&parser, &token),
          "exact header fixture enters HEADERS state");
    CHECK(feed_split(
              &parser, &token, headers, (uint32_t)strlen(headers)),
          "exact header section parses in one feed");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_COMPLETE,
          "exact header section publishes COMPLETE");
    CHECK(http_header_count(
              &parser, &token, &count) == HTTP_PARSER_OK &&
              count == 2U,
          "exact header section exposes two fields");
    CHECK(http_header_field(
              &parser, &token, 0U, &field) == HTTP_PARSER_OK &&
              header_field_equal(
                  &field, "Host", " hfoc.test"),
          "exact header field zero exposes exact name/value");
    CHECK(http_header_field(
              &parser, &token, 1U, &field) == HTTP_PARSER_OK &&
              header_field_equal(
                  &field, "Accept", " text/html"),
          "exact header field one exposes exact name/value");
    CHECK(http_header_field(
              &parser, &token, 2U, &field) ==
              HTTP_PARSER_HEADER_OUT_OF_RANGE,
          "header field getter rejects out-of-range index");
    CHECK(http_parser_progress(
              &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.request_line_count == 16U &&
              progress.header_offset == 16U &&
              progress.header_count ==
                  (uint32_t)strlen(headers) &&
              progress.total_count ==
                  16U + (uint32_t)strlen(headers),
          "exact header section publishes exact item 271 progress");

    for (step = 1U; step <= 5U; ++step) {
        count = 0U;
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  feed_request_line(&parser, &token) &&
                  feed_split(&parser, &token, headers, step) &&
                  http_header_count(
                      &parser, &token, &count) == HTTP_PARSER_OK &&
                  count == 2U,
              "header section parses across split step");
    }
}

static void test_empty_and_duplicate_fields(void)
{
    static const char empty_headers[] = "\r\n";
    static const char duplicate_headers[] =
        "X-A: 1\r\n"
        "X-A:2\r\n"
        "X-Empty:\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    struct http_header_field field;
    uint32_t count = 0U;

    CHECK(init_parser(&parser),
          "empty header fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token),
          "empty header fixture enters HEADERS state");
    CHECK(feed_split(
              &parser, &token, empty_headers,
              (uint32_t)strlen(empty_headers)),
          "empty header section parses");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_COMPLETE &&
              http_header_count(
                  &parser, &token, &count) == HTTP_PARSER_OK &&
              count == 0U,
          "empty header section exposes zero fields");
    CHECK(http_parser_progress(
              &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.header_count == 2U &&
              progress.total_count == 18U,
          "empty header section counts only terminator bytes");

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token),
          "duplicate header fixture enters HEADERS state");
    CHECK(feed_split(
              &parser, &token, duplicate_headers,
              (uint32_t)strlen(duplicate_headers)),
          "duplicate and empty-value header section parses");
    CHECK(http_header_count(
              &parser, &token, &count) == HTTP_PARSER_OK &&
              count == 3U &&
              http_header_field(
                  &parser, &token, 0U, &field) == HTTP_PARSER_OK &&
              header_field_equal(&field, "X-A", " 1") &&
              http_header_field(
                  &parser, &token, 1U, &field) == HTTP_PARSER_OK &&
              header_field_equal(&field, "X-A", "2") &&
              http_header_field(
                  &parser, &token, 2U, &field) == HTTP_PARSER_OK &&
              header_field_equal(&field, "X-Empty", ""),
          "duplicate names and empty values remain exact fields");
}

static void test_folding_rejection(void)
{
    static const char *cases[] = {
        " Host: x\r\n\r\n",
        "Host: x\r\n value\r\n\r\n",
        "Host: x\r\n\tvalue\r\n\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    bool complete;
    uint32_t index;

    CHECK(init_parser(&parser),
          "folding fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "folding fixture starts slot zero");

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        complete = false;
        CHECK(feed_request_line(&parser, &token),
              "folding fixture enters HEADERS state");
        CHECK(http_header_feed(
                  &parser,
                  &token,
                  (const uint8_t *)cases[index],
                  (uint32_t)strlen(cases[index]),
                  &complete) == HTTP_PARSER_HEADER_FOLDING &&
                  !complete,
              "obsolete header folding rejects deterministically");
        CHECK(http_header_error(&parser, &token) ==
                  HTTP_PARSER_HEADER_FOLDING,
              "header folding error latches for slot");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "folding fixture restarts slot after rejection");
    }
}

static void test_invalid_name_and_value_rejection(void)
{
    static const char *name_cases[] = {
        "Ho st: x\r\n\r\n",
        "Host x\r\n\r\n",
        ": x\r\n\r\n",
        "H@st: x\r\n\r\n",
    };
    static const char *value_cases[] = {
        "Host: x\x01y\r\n\r\n",
        "Host: x\x7fy\r\n\r\n",
        "Host: \xC3\xA9\r\n\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    bool complete;
    uint32_t index;

    CHECK(init_parser(&parser),
          "name fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "name fixture starts slot zero");
    for (index = 0U; index < sizeof(name_cases) / sizeof(name_cases[0]);
         ++index) {
        complete = false;
        CHECK(feed_request_line(&parser, &token) &&
                  http_header_feed(
                      &parser,
                      &token,
                      (const uint8_t *)name_cases[index],
                      (uint32_t)strlen(name_cases[index]),
                      &complete) ==
                  HTTP_PARSER_HEADER_INVALID_NAME &&
                  !complete,
              "invalid header name rejects deterministically");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "name fixture restarts slot after rejection");
    }

    for (index = 0U; index < sizeof(value_cases) / sizeof(value_cases[0]);
         ++index) {
        complete = false;
        CHECK(feed_request_line(&parser, &token) &&
                  http_header_feed(
                      &parser,
                      &token,
                      (const uint8_t *)value_cases[index],
                      (uint32_t)strlen(value_cases[index]),
                      &complete) ==
                  HTTP_PARSER_HEADER_INVALID_VALUE &&
                  !complete,
              "invalid header value rejects deterministically");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "value fixture restarts slot after rejection");
    }
}

static void test_incomplete_termination_rejection(void)
{
    static const char *cases[] = {
        "Host: x\n\r\n",
        "Host: x\rX\r\n",
    };
    static const char incomplete[] = "Host: x\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    bool complete;
    uint32_t index;

    CHECK(init_parser(&parser),
          "termination fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "termination fixture starts slot zero");

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        complete = false;
        CHECK(feed_request_line(&parser, &token) &&
                  http_header_feed(
                      &parser,
                      &token,
                      (const uint8_t *)cases[index],
                      (uint32_t)strlen(cases[index]),
                      &complete) ==
                  HTTP_PARSER_HEADER_INCOMPLETE_TERMINATION &&
                  !complete,
              "incomplete header termination rejects deterministically");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "termination fixture restarts slot after rejection");
    }

    complete = false;
    CHECK(feed_request_line(&parser, &token) &&
              http_header_feed(
                  &parser,
                  &token,
                  (const uint8_t *)incomplete,
                  (uint32_t)strlen(incomplete),
                  &complete) == HTTP_PARSER_OK &&
              !complete &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_HEADERS,
          "incomplete but well-formed header field waits for terminator");
}

static void test_limit_boundary(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    struct http_header_field field;
    uint8_t exact[HTTP_PARSER_HEADERS_MAX];
    uint8_t overflow[HTTP_PARSER_HEADERS_MAX + 1U];
    uint32_t count = 0U;
    bool complete;
    enum http_parser_status status;

    memcpy(exact, "A:", 2U);
    memset(exact + 2U, 'a', HTTP_PARSER_HEADERS_MAX - 6U);
    memcpy(exact + HTTP_PARSER_HEADERS_MAX - 4U, "\r\n\r\n", 4U);

    CHECK(init_parser(&parser),
          "header limit fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token),
          "header limit fixture enters HEADERS state");
    complete = false;
    CHECK(http_header_feed(
              &parser,
              &token,
              exact,
              sizeof(exact),
              &complete) == HTTP_PARSER_OK &&
              complete,
          "4096-byte header section is accepted at exact boundary");
    CHECK(http_header_count(
              &parser, &token, &count) == HTTP_PARSER_OK &&
              count == 1U &&
              http_header_field(
                  &parser, &token, 0U, &field) == HTTP_PARSER_OK &&
              field.value_length == HTTP_PARSER_HEADERS_MAX - 6U,
          "4096-byte header section exposes exact value length");
    CHECK(http_parser_progress(
              &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.header_count == HTTP_PARSER_HEADERS_MAX,
          "4096-byte header section publishes exact progress");

    memcpy(overflow, "A:", 2U);
    memset(overflow + 2U, 'a', HTTP_PARSER_HEADERS_MAX - 5U);
    memcpy(overflow + HTTP_PARSER_HEADERS_MAX - 3U, "\r\n\r\n", 4U);

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token),
          "header limit fixture restarts overflow slot");
    complete = false;
    status = http_header_feed(
        &parser,
        &token,
        overflow,
        sizeof(overflow),
        &complete);
    CHECK(status == HTTP_PARSER_HEADERS_LIMIT && !complete,
          "4097-byte header section rejects as overflow");
    CHECK(http_header_error(&parser, &token) ==
              HTTP_PARSER_HEADERS_LIMIT,
          "header overflow latches limit error");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress(
                  &parser, &token, &progress) == HTTP_PARSER_OK &&
              progress.header_count == HTTP_PARSER_HEADERS_MAX,
          "header overflow publishes accepted 4096-byte prefix progress");
    CHECK(http_parser_reject(
              &parser,
              &token,
              &(struct http_parser_progress){
                  0U,
                  16U,
                  16U,
                  HTTP_PARSER_HEADERS_MAX,
                  16U + HTTP_PARSER_HEADERS_MAX
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_REJECTED,
          "header overflow slot can publish exact REJECTED state");
}

static void test_error_lockout_and_isolation(void)
{
    static const char bad[] = "Host: x\r\n value\r\n\r\n";
    static const char good[] = "Host: hfoc.test\r\n\r\n";
    struct http_parser parser;
    struct http_parser_token first;
    struct http_parser_token second;
    struct http_parser_progress progress;
    uint32_t count = 0U;
    bool complete;

    CHECK(init_parser(&parser),
          "header isolation fixture initializes parser");
    first = make_token(0U, 1U);
    second = make_token(1U, 2U);
    CHECK(http_parser_start(&parser, &first) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &second) == HTTP_PARSER_OK &&
              parser.active_count == 2U &&
              feed_request_line(&parser, &first) &&
              feed_request_line(&parser, &second),
          "header isolation fixture attaches two prepared slots");

    complete = false;
    CHECK(http_header_feed(
              &parser,
              &first,
              (const uint8_t *)bad,
              (uint32_t)strlen(bad),
              &complete) == HTTP_PARSER_HEADER_FOLDING &&
              !complete,
          "header isolation fixture latches first slot failure");
    complete = false;
    CHECK(http_header_feed(
              &parser,
              &first,
              (const uint8_t *)bad,
              (uint32_t)strlen(bad),
              &complete) == HTTP_PARSER_HEADER_FOLDING &&
              !complete,
          "failed header slot rejects repeated feed");
    CHECK(http_header_count(
              &parser, &first, &count) ==
              HTTP_PARSER_HEADER_FOLDING,
          "failed header count stays latched");
    CHECK(http_parser_state(&parser, &first) ==
              HTTP_PARSER_STATE_HEADERS &&
              http_parser_progress(
                  &parser, &first, &progress) == HTTP_PARSER_OK &&
              progress.header_count == 9U,
          "failed first header slot publishes accepted prefix progress");

    CHECK(feed_split(
              &parser, &second, good, (uint32_t)strlen(good)),
          "unrelated second header slot parses while first is failed");
    CHECK(http_parser_state(&parser, &second) ==
              HTTP_PARSER_STATE_COMPLETE &&
              http_parser_active_count(&parser) == 2U,
          "failed header slot does not disturb unrelated active slot");

    CHECK(http_parser_reject(
              &parser,
              &first,
              &(struct http_parser_progress){
                  0U, 16U, 16U, 9U, 25U
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &first) ==
                  HTTP_PARSER_STATE_REJECTED,
          "failed header slot publishes REJECTED explicitly");
    CHECK(http_header_feed(
              &parser,
              &first,
              (const uint8_t *)good,
              (uint32_t)strlen(good),
              &complete) == HTTP_PARSER_TERMINAL,
          "REJECTED header slot rejects further feed");

    CHECK(http_parser_release(&parser, &first) == HTTP_PARSER_OK &&
              parser.active_count == 1U,
          "release removes failed header slot");
    CHECK(http_parser_start(&parser, &first) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &first) &&
              feed_split(
                  &parser, &first, good, (uint32_t)strlen(good)),
          "released header slot parses after new generation");
}

int main(void)
{
    test_status_strings();
    test_preconditions_and_incomplete();
    test_exact_and_split_parsing();
    test_empty_and_duplicate_fields();
    test_folding_rejection();
    test_invalid_name_and_value_rejection();
    test_incomplete_termination_rejection();
    test_limit_boundary();
    test_error_lockout_and_isolation();

    if (failures != 0) {
        fprintf(stderr, "HTTP header fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP header host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_header_unused;

#endif
