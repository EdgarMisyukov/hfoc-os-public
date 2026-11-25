#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_PARSER_HOST_TEST) && \
    defined(HTTP_REQUEST_LINE_HOST_TEST) && \
    defined(HTTP_HEADER_HOST_TEST) && \
    defined(HTTP_FRAMING_HOST_TEST)

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

static bool feed_request_line_text(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line)
{
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

static bool feed_request_line(
    struct http_parser *parser,
    const struct http_parser_token *token)
{
    return feed_request_line_text(parser, token, "GET / HTTP/1.1\r\n");
}

static bool feed_headers(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *headers)
{
    bool complete = false;

    if (http_header_feed(
            parser,
            token,
            (const uint8_t *)headers,
            (uint32_t)strlen(headers),
            &complete) != HTTP_PARSER_OK) {
        return false;
    }
    return complete;
}

static bool parse_headers(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *headers)
{
    return feed_request_line(parser, token) &&
           feed_headers(parser, token, headers);
}

static bool framing_result_equal(
    const struct http_framing_result *result,
    bool body_denied,
    bool upgrade_denied,
    enum http_framing_kind reason,
    uint32_t field_index)
{
    return result != NULL &&
           result->body_denied == body_denied &&
           result->upgrade_denied == upgrade_denied &&
           result->reason == reason &&
           result->field_index == field_index;
}

static void test_kind_strings(void)
{
    enum http_framing_kind kind;

    for (kind = HTTP_FRAMING_NONE;
         kind <= HTTP_FRAMING_INVALID;
         ++kind) {
        CHECK(http_framing_kind_string(kind) != NULL,
              "framing kind string is defined");
    }
    CHECK(http_framing_kind_string(
              (enum http_framing_kind)(HTTP_FRAMING_INVALID + 1U)) ==
              NULL,
          "unknown framing kind string is null");
}

static void test_preconditions_and_incomplete(void)
{
    static const char no_framing[] =
        "Host: hfoc.test\r\n"
        "Accept: text/html\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token stale;
    struct http_parser_progress before;
    struct http_parser_progress after;
    struct http_framing_result result;

    CHECK(init_parser(&parser),
          "framing fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "framing fixture starts slot zero");
    memset(&result, 0, sizeof(result));
    result.reason = HTTP_FRAMING_UPGRADE;
    result.field_index = 99U;
    CHECK(http_framing_check(
              &parser, &token, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE &&
              result.reason == HTTP_FRAMING_UPGRADE &&
              result.field_index == 99U,
          "framing check rejects missing request line without mutation");

    CHECK(feed_request_line(&parser, &token),
          "framing fixture completes request line");
    result.reason = HTTP_FRAMING_UPGRADE;
    result.field_index = 99U;
    CHECK(http_framing_check(
              &parser, &token, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE &&
              result.reason == HTTP_FRAMING_UPGRADE &&
              result.field_index == 99U,
          "framing check rejects incomplete header section");
    CHECK(http_framing_check(
              NULL, &token, &result) == HTTP_PARSER_NULL,
          "framing check rejects null parser");
    CHECK(http_framing_check(
              &parser, NULL, &result) == HTTP_PARSER_NULL,
          "framing check rejects null token");
    CHECK(http_framing_check(
              &parser, &token, NULL) == HTTP_PARSER_NULL,
          "framing check rejects null result");

    stale = make_token(0U, 2U);
    CHECK(http_framing_check(
              &parser, &stale, &result) == HTTP_PARSER_STALE_TOKEN,
          "framing check rejects stale generation");
    stale.index = HTTP_PARSER_SLOT_COUNT;
    stale.generation = token.generation;
    CHECK(http_framing_check(
              &parser, &stale, &result) == HTTP_PARSER_BAD_TOKEN,
          "framing check rejects invalid slot index");

    CHECK(feed_headers(
              &parser, &token, no_framing),
          "framing fixture completes no-framing headers");
    CHECK(http_parser_progress(
              &parser, &token, &before) == HTTP_PARSER_OK,
          "framing fixture reads progress before check");
    CHECK(http_framing_check(
              &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  false,
                  false,
                  HTTP_FRAMING_NONE,
                  0U),
          "framing check accepts complete no-framing request");
    CHECK(http_parser_progress(
              &parser, &token, &after) == HTTP_PARSER_OK &&
              before.request_line_count ==
                  after.request_line_count &&
              before.header_count == after.header_count &&
              before.total_count == after.total_count,
          "framing check does not mutate parser progress");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_COMPLETE,
          "framing check leaves parser COMPLETE");
    CHECK(http_framing_check(
              &parser, &token, &result) == HTTP_PARSER_OK,
          "framing check remains callable after COMPLETE");
}

static void test_accepted_no_framing(void)
{
    static const char *cases[] = {
        "\r\n",
        "Host: hfoc.test\r\n\r\n",
        "Accept: text/html\r\n"
        "Connection: close\r\n"
        "\r\n",
        "Content-Length-Extra: 0\r\n\r\n",
        "Transfer-Encoding-Extra: chunked\r\n\r\n",
        "X-Custom: value\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "accepted fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "accepted fixture starts slot zero");

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        result.reason = HTTP_FRAMING_INVALID;
        result.field_index = 99U;
        CHECK(parse_headers(
                  &parser, &token, cases[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      false,
                      false,
                      HTTP_FRAMING_NONE,
                      0U),
              "ordinary headers are accepted as no-framing");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "accepted fixture restarts slot");
    }
}

static void test_content_length_rejection(void)
{
    static const char *cases[] = {
        "Content-Length: 0\r\n\r\n",
        "content-length: 5\r\n\r\n",
        "Host: hfoc.test\r\n"
        "CoNtEnT-LenGtH: 999\r\n"
        "\r\n",
        "Content-Length:   123   \r\n\r\n",
    };
    static const char *invalid_cases[] = {
        "Content-Length:\r\n\r\n",
        "Content-Length: abc\r\n\r\n",
        "Content-Length: +1\r\n\r\n",
        "Content-Length: 12 34\r\n\r\n",
        "Host: hfoc.test\r\n"
        "Content-Length: abc\r\n"
        "\r\n",
        "Content-Length: abc\r\n"
        "Transfer-Encoding: chunked,\r\n"
        "\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "content-length fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "content-length fixture starts slot zero");

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_headers(
                  &parser, &token, cases[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      true,
                      false,
                      HTTP_FRAMING_CONTENT_LENGTH,
                      index == 2U ? 1U : 0U),
              "Content-Length request is denied before dispatch");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "content-length fixture restarts slot");
    }

    for (index = 0U;
         index < sizeof(invalid_cases) / sizeof(invalid_cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_headers(
                  &parser, &token, invalid_cases[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      true,
                      false,
                      HTTP_FRAMING_INVALID,
                      index == 4U ? 1U : 0U),
              "invalid Content-Length is rejected as invalid framing");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "invalid content-length fixture restarts slot");
    }
}

/*
 * ITEM-445 step 2: framing publishes the one bounded body the appliance
 * accepts. The sign-in form is a POST, so a POST Content-Length within the
 * bound is framed (not denied) and its number must be the real one; a GET body,
 * an oversized body and a value that would wrap uint32 all stay denied exactly
 * as before, so the accepted set cannot grow by accident.
 */
static void test_content_length_post_body(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;
    static const char *accepted[] = {
        "Content-Length: 0\r\n\r\n",
        "Content-Length: 1\r\n\r\n",
        "Content-Length: 8192\r\n\r\n",
        "Content-Length:   8192   \r\n\r\n",
    };
    static const uint32_t accepted_lengths[] = {0U, 1U, 8192U, 8192U};
    static const char *denied[] = {
        "Content-Length: 8193\r\n\r\n",
        "Content-Length: 4294967808\r\n\r\n",
        "Content-Length: 4294967296\r\n\r\n",
        "Content-Length: 99999999999999999999\r\n\r\n",
    };
    uint32_t index;

    for (index = 0U;
         index < sizeof(accepted) / sizeof(accepted[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(init_parser(&parser),
              "POST body fixture initializes parser");
        token = make_token(0U, 1U);
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  feed_request_line_text(
                      &parser, &token, "POST / HTTP/1.1\r\n") &&
                  feed_headers(&parser, &token, accepted[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK,
              "POST body fixture parses");
        CHECK(!result.body_denied &&
                  !result.upgrade_denied &&
                  result.body_present &&
                  result.content_length == accepted_lengths[index],
              "bounded POST body is framed with its exact length");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "POST body fixture releases slot");
    }

    for (index = 0U;
         index < sizeof(denied) / sizeof(denied[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(init_parser(&parser),
              "oversized POST body fixture initializes parser");
        token = make_token(0U, 1U);
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
                  feed_request_line_text(
                      &parser, &token, "POST / HTTP/1.1\r\n") &&
                  feed_headers(&parser, &token, denied[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      true,
                      false,
                      HTTP_FRAMING_CONTENT_LENGTH,
                      0U) &&
                  !result.body_present,
              "POST body outside the bound stays denied, unaliased");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "oversized POST body fixture releases slot");
    }

    /*
     * The same header on a GET is still denied: the method check, not the
     * number, is what keeps a body off every other route.
     */
    memset(&result, 0, sizeof(result));
    CHECK(init_parser(&parser),
          "GET body fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token) &&
              feed_headers(
                  &parser, &token, "Content-Length: 0\r\n\r\n") &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_CONTENT_LENGTH,
                  0U) &&
              !result.body_present,
          "GET with Content-Length is still denied");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "GET body fixture releases slot");
}

static void test_transfer_encoding_rejection(void)
{
    static const char *cases[] = {
        "Transfer-Encoding: chunked\r\n\r\n",
        "transfer-encoding: gzip, chunked\r\n\r\n",
        "Host: hfoc.test\r\n"
        "Transfer-Encoding: gzip , chunked\r\n"
        "\r\n",
    };
    static const char *invalid_cases[] = {
        "Transfer-Encoding:\r\n\r\n",
        "Transfer-Encoding: chunked,\r\n\r\n",
        "Transfer-Encoding: ,chunked\r\n\r\n",
        "Transfer-Encoding: chunked,,gzip\r\n\r\n",
        "Transfer-Encoding: chunked gzip\r\n\r\n",
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "transfer-encoding fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "transfer-encoding fixture starts slot zero");

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_headers(
                  &parser, &token, cases[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      true,
                      false,
                      HTTP_FRAMING_TRANSFER_ENCODING,
                      index < 2U ? 0U : 1U),
              "Transfer-Encoding request is denied before dispatch");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "transfer-encoding fixture restarts slot");
    }

    for (index = 0U;
         index < sizeof(invalid_cases) / sizeof(invalid_cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_headers(
                  &parser, &token, invalid_cases[index]) &&
                  http_framing_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  framing_result_equal(
                      &result,
                      true,
                      false,
                      HTTP_FRAMING_INVALID,
                      0U),
              "invalid Transfer-Encoding is rejected as invalid framing");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "invalid transfer-encoding fixture restarts slot");
    }
}

static void test_expect_and_upgrade_rejection(void)
{
    static const char expect_headers[] =
        "Expect: 100-continue\r\n\r\n";
    static const char upgrade_headers[] =
        "Upgrade: h2c\r\n\r\n";
    static const char both_headers[] =
        "Host: hfoc.test\r\n"
        "Expect: 100-continue\r\n"
        "Upgrade: h2c\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;

    CHECK(init_parser(&parser),
          "expect/upgrade fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "expect/upgrade fixture starts slot zero");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, expect_headers) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_EXPECT,
                  0U),
          "Expect request is denied as body framing");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "expect fixture restarts slot");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, upgrade_headers) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  false,
                  true,
                  HTTP_FRAMING_UPGRADE,
                  0U),
          "Upgrade request is denied before dispatch");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "upgrade fixture restarts slot");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, both_headers) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_EXPECT,
                  1U),
          "body framing takes precedence over Upgrade in field order");
}

static void test_duplicate_and_conflict_rejection(void)
{
    static const char duplicate_length[] =
        "Content-Length: 0\r\n"
        "Content-Length: 0\r\n"
        "\r\n";
    static const char duplicate_encoding[] =
        "Host: hfoc.test\r\n"
        "Transfer-Encoding: chunked\r\n"
        "transfer-encoding: chunked\r\n"
        "\r\n";
    static const char conflict[] =
        "Host: hfoc.test\r\n"
        "Content-Length: 5\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;

    CHECK(init_parser(&parser),
          "duplicate fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "duplicate fixture starts slot zero");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, duplicate_length) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_DUPLICATE,
                  1U),
          "duplicate Content-Length is rejected even with equal values");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "duplicate length fixture restarts slot");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, duplicate_encoding) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_DUPLICATE,
                  2U),
          "duplicate Transfer-Encoding is rejected");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "duplicate encoding fixture restarts slot");

    memset(&result, 0, sizeof(result));
    CHECK(parse_headers(
              &parser, &token, conflict) &&
              http_framing_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_CONFLICT,
                  2U),
          "Content-Length plus Transfer-Encoding conflict is rejected");
}

static void test_isolation_and_restart(void)
{
    static const char denied_headers[] =
        "Content-Length: 5\r\n\r\n";
    static const char accepted_headers[] =
        "Host: hfoc.test\r\n\r\n";
    struct http_parser parser;
    struct http_parser_token denied;
    struct http_parser_token accepted;
    struct http_framing_result result;
    struct http_parser_progress progress;
    uint32_t count = 0U;

    CHECK(init_parser(&parser),
          "isolation fixture initializes parser");
    denied = make_token(0U, 1U);
    accepted = make_token(1U, 2U);
    CHECK(http_parser_start(&parser, &denied) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &accepted) == HTTP_PARSER_OK &&
              parse_headers(
                  &parser, &denied, denied_headers) &&
              parse_headers(
                  &parser, &accepted, accepted_headers),
          "isolation fixture prepares denied and accepted slots");

    memset(&result, 0, sizeof(result));
    CHECK(http_framing_check(
              &parser, &denied, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  true,
                  false,
                  HTTP_FRAMING_CONTENT_LENGTH,
                  0U),
          "denied slot reports Content-Length framing");
    memset(&result, 0, sizeof(result));
    CHECK(http_framing_check(
              &parser, &accepted, &result) == HTTP_PARSER_OK &&
              framing_result_equal(
                  &result,
                  false,
                  false,
                  HTTP_FRAMING_NONE,
                  0U),
          "accepted slot is unaffected by denied slot");
    CHECK(http_parser_active_count(&parser) == 2U,
          "framing isolation preserves both active slots");
    CHECK(http_parser_progress(
              &parser, &accepted, &progress) == HTTP_PARSER_OK &&
              progress.header_count ==
                  (uint32_t)strlen(accepted_headers),
          "framing isolation preserves accepted header progress");
    CHECK(http_header_count(
              &parser, &accepted, &count) == HTTP_PARSER_OK &&
              count == 1U,
          "framing isolation preserves accepted header views");

    CHECK(http_parser_release(&parser, &accepted) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &accepted) == HTTP_PARSER_OK,
          "isolation fixture restarts accepted slot");
    CHECK(http_framing_check(
              &parser, &accepted, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE,
          "restarted slot requires complete headers again");
}

static void test_rejected_state(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_framing_result result;

    CHECK(init_parser(&parser),
          "rejected fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              feed_request_line(&parser, &token),
          "rejected fixture enters HEADERS state");
    CHECK(http_parser_reject(
              &parser,
              &token,
              &(struct http_parser_progress){
                  0U, 16U, 16U, 0U, 16U
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_REJECTED,
          "rejected fixture publishes explicit REJECTED state");
    CHECK(http_framing_check(
              &parser, &token, &result) == HTTP_PARSER_TERMINAL,
          "framing check rejects terminal REJECTED slot");
}

int main(void)
{
    test_kind_strings();
    test_preconditions_and_incomplete();
    test_accepted_no_framing();
    test_content_length_rejection();
    test_content_length_post_body();
    test_transfer_encoding_rejection();
    test_expect_and_upgrade_rejection();
    test_duplicate_and_conflict_rejection();
    test_isolation_and_restart();
    test_rejected_state();

    if (failures != 0) {
        fprintf(stderr, "HTTP framing fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP framing host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_framing_unused;

#endif
