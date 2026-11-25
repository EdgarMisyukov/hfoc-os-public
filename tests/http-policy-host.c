#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_POLICY_HOST_TEST)

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

static bool parse_request(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line,
    const char *headers)
{
    return feed_request_line(parser, token, line) &&
           feed_headers(parser, token, headers);
}

static bool parse_get(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *headers)
{
    return parse_request(
        parser, token, "GET / HTTP/1.1\r\n", headers);
}

static bool policy_result_equal(
    const struct http_policy_result *result,
    bool allowed,
    bool method_allowed,
    bool version_allowed,
    bool host_valid,
    enum http_method_kind method,
    enum http_policy_reason reason,
    uint32_t field_index)
{
    return result != NULL &&
           result->allowed == allowed &&
           result->method_allowed == method_allowed &&
           result->version_allowed == version_allowed &&
           result->host_valid == host_valid &&
           result->method == method &&
           result->reason == reason &&
           result->field_index == field_index;
}

static void test_kind_and_reason_strings(void)
{
    enum http_method_kind method;
    enum http_policy_reason reason;

    for (method = HTTP_METHOD_NONE;
         method <= HTTP_METHOD_POST;
         ++method) {
        CHECK(http_method_kind_string(method) != NULL,
              "method kind string is defined");
    }
    CHECK(http_method_kind_string(
              (enum http_method_kind)
                  (HTTP_METHOD_POST + 1U)) == NULL,
          "unknown method kind string is null");
    CHECK(http_method_kind_string(HTTP_METHOD_POST) != NULL &&
              strcmp(http_method_kind_string(HTTP_METHOD_POST),
                     "HTTP_METHOD_POST") == 0,
          "the appended method keeps its own name");
    for (reason = HTTP_POLICY_NONE;
         reason <= HTTP_POLICY_INVALID_HOST;
         ++reason) {
        CHECK(http_policy_reason_string(reason) != NULL,
              "policy reason string is defined");
    }
    CHECK(http_policy_reason_string(
              (enum http_policy_reason)
                  (HTTP_POLICY_INVALID_HOST + 1U)) == NULL,
          "unknown policy reason string is null");
}

static void test_preconditions_and_incomplete(void)
{
    static const char headers[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token stale;
    struct http_parser_progress before;
    struct http_parser_progress after;
    struct http_policy_result result;

    CHECK(init_parser(&parser),
          "policy fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "policy fixture starts slot zero");
    memset(&result, 0, sizeof(result));
    result.reason = HTTP_POLICY_INVALID_HOST;
    result.field_index = 99U;
    CHECK(http_policy_check(
              &parser, &token, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE &&
              result.reason == HTTP_POLICY_INVALID_HOST &&
              result.field_index == 99U,
          "policy check rejects missing request line without mutation");

    CHECK(feed_request_line(
              &parser, &token, "GET / HTTP/1.1\r\n"),
          "policy fixture completes request line");
    result.reason = HTTP_POLICY_INVALID_HOST;
    result.field_index = 99U;
    CHECK(http_policy_check(
              &parser, &token, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE &&
              result.reason == HTTP_POLICY_INVALID_HOST &&
              result.field_index == 99U,
          "policy check rejects incomplete header section");
    CHECK(http_policy_check(
              NULL, &token, &result) == HTTP_PARSER_NULL,
          "policy check rejects null parser");
    CHECK(http_policy_check(
              &parser, NULL, &result) == HTTP_PARSER_NULL,
          "policy check rejects null token");
    CHECK(http_policy_check(
              &parser, &token, NULL) == HTTP_PARSER_NULL,
          "policy check rejects null result");

    stale = make_token(0U, 2U);
    CHECK(http_policy_check(
              &parser, &stale, &result) == HTTP_PARSER_STALE_TOKEN,
          "policy check rejects stale generation");
    stale.index = HTTP_PARSER_SLOT_COUNT;
    stale.generation = token.generation;
    CHECK(http_policy_check(
              &parser, &stale, &result) == HTTP_PARSER_BAD_TOKEN,
          "policy check rejects invalid slot index");

    CHECK(feed_headers(&parser, &token, headers),
          "policy fixture completes valid headers");
    CHECK(http_parser_progress(
              &parser, &token, &before) == HTTP_PARSER_OK,
          "policy fixture reads progress before check");
    CHECK(http_policy_check(
              &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  true,
                  true,
                  true,
                  true,
                  HTTP_METHOD_GET,
                  HTTP_POLICY_NONE,
                  0U),
          "policy check accepts complete GET with valid Host");
    CHECK(http_parser_progress(
              &parser, &token, &after) == HTTP_PARSER_OK &&
              before.request_line_count ==
                  after.request_line_count &&
              before.header_count == after.header_count &&
              before.total_count == after.total_count,
          "policy check does not mutate parser progress");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_COMPLETE,
          "policy check leaves parser COMPLETE");
    CHECK(http_policy_check(
              &parser, &token, &result) == HTTP_PARSER_OK,
          "policy check remains callable after COMPLETE");
}

static void test_accepted_requests(void)
{
    static const char *get_headers[] = {
        "Host: hfoc.test\r\n\r\n",
        "Host: hfoc.test:443\r\n\r\n",
        "Accept: text/html\r\n"
        "Host: hfoc.test\r\n"
        "\r\n",
        "host: Hfoc.TEST\r\n\r\n",
        "Host: 10.0.2.15:443\r\n\r\n",
        "Host:   hfoc.test  \t\r\n\r\n",
    };
    static const char head_line[] = "HEAD /health HTTP/1.1\r\n";
    static const char head_headers[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_policy_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "accepted fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "accepted fixture starts slot zero");

    for (index = 0U;
         index < sizeof(get_headers) / sizeof(get_headers[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_get(&parser, &token, get_headers[index]) &&
                  http_policy_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  policy_result_equal(
                      &result,
                      true,
                      true,
                      true,
                      true,
                      HTTP_METHOD_GET,
                      HTTP_POLICY_NONE,
                      0U),
              "GET request with valid Host is accepted");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "accepted fixture restarts slot");
    }

    memset(&result, 0, sizeof(result));
    CHECK(parse_request(
              &parser, &token, head_line, head_headers) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  true,
                  true,
                  true,
                  true,
                  HTTP_METHOD_HEAD,
                  HTTP_POLICY_NONE,
                  0U),
          "HEAD request with valid Host is accepted");
}

static void test_method_rejection(void)
{
    /*
     * ITEM-445 step 2: `POST /` is the browser sign-in form and is accepted;
     * the unsupported-method corpus keeps every other spelling, and the
     * accepted/refused POST split has its own test below.
     */
    static const char *lines[] = {
        "PUT / HTTP/1.1\r\n",
        "DELETE / HTTP/1.1\r\n",
        "OPTIONS / HTTP/1.1\r\n",
        "PATCH / HTTP/1.1\r\n",
        "TRACE / HTTP/1.1\r\n",
        "CONNECT / HTTP/1.1\r\n",
        "get / HTTP/1.1\r\n",
        "GETT / HTTP/1.1\r\n",
    };
    static const char headers[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_policy_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "method fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "method fixture starts slot zero");

    for (index = 0U;
         index < sizeof(lines) / sizeof(lines[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_request(
                  &parser, &token, lines[index], headers) &&
                  http_policy_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  policy_result_equal(
                      &result,
                      false,
                      false,
                      true,
                      true,
                      HTTP_METHOD_UNSUPPORTED,
                      HTTP_POLICY_METHOD,
                      0U),
              "unsupported method is rejected before dispatch");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "method fixture restarts slot");
    }

    /* ITEM-445 step 2: POST is accepted for the form targets only. */
    {
        static const char *const accepted[] = {
            "POST / HTTP/1.1\r\n",
            "POST /panel/logout HTTP/1.1\r\n",
            /* ITEM-505: the network form and its scriptable entry. */
            "POST /panel/network HTTP/1.1\r\n",
            "POST /api/v1/network HTTP/1.1\r\n",
            /*
             * ITEM-505 increment 3: the hostname form and its scriptable
             * entry; the GET body of `/api/v1/system` keeps its read-only
             * meaning, the POST is the mutation.
             */
            "POST /panel/system HTTP/1.1\r\n",
            "POST /api/v1/system HTTP/1.1\r\n",
            /*
             * ITEM-506 increment 6: the credential-change form and its
             * scriptable entry.
             */
            "POST /panel/account HTTP/1.1\r\n",
            "POST /api/v1/system/account HTTP/1.1\r\n",
        };
        static const char *const refused[] = {
            "POST /panel HTTP/1.1\r\n",
            "POST /health HTTP/1.1\r\n",
            "POST /api/v1 HTTP/1.1\r\n",
            "POST /api/v1/network/status HTTP/1.1\r\n",
            "POST /api/v1/service HTTP/1.1\r\n",
        };

        for (index = 0U;
             index < sizeof(accepted) / sizeof(accepted[0]);
             ++index) {
            memset(&result, 0, sizeof(result));
            CHECK(parse_request(
                      &parser, &token, accepted[index], headers) &&
                      http_policy_check(
                          &parser, &token, &result) == HTTP_PARSER_OK &&
                      policy_result_equal(
                          &result,
                          true,
                          true,
                          true,
                          true,
                          HTTP_METHOD_POST,
                          HTTP_POLICY_NONE,
                          0U),
                  "the sign-in form target accepts POST");
            CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                      http_parser_start(&parser, &token) == HTTP_PARSER_OK,
                  "form target fixture restarts slot");
        }
        for (index = 0U;
             index < sizeof(refused) / sizeof(refused[0]);
             ++index) {
            memset(&result, 0, sizeof(result));
            CHECK(parse_request(
                      &parser, &token, refused[index], headers) &&
                      http_policy_check(
                          &parser, &token, &result) == HTTP_PARSER_OK &&
                      policy_result_equal(
                          &result,
                          false,
                          false,
                          true,
                          true,
                          HTTP_METHOD_POST,
                          HTTP_POLICY_METHOD,
                          0U),
                  "a non-form target still refuses POST");
            CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                      http_parser_start(&parser, &token) == HTTP_PARSER_OK,
                  "refused form fixture restarts slot");
        }
    }
}

static void test_version_rejection(void)
{
    static const char *lines[] = {
        "GET / HTTP/1.0\r\n",
        "GET / HTTP/0.9\r\n",
        "GET / HTTP/2.0\r\n",
        "GET / HTTP/9.9\r\n",
    };
    static const char headers[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_policy_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "version fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "version fixture starts slot zero");

    for (index = 0U;
         index < sizeof(lines) / sizeof(lines[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_request(
                  &parser, &token, lines[index], headers) &&
                  http_policy_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  policy_result_equal(
                      &result,
                      false,
                      true,
                      false,
                      true,
                      HTTP_METHOD_GET,
                      HTTP_POLICY_VERSION,
                      0U),
              "unsupported HTTP version is rejected before dispatch");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "version fixture restarts slot");
    }
}

static void test_host_rejection(void)
{
    static const char *cases[] = {
        "Accept: text/html\r\n\r\n",
        "Host:\r\n\r\n",
        "Host: \t\r\n\r\n",
        "Host: hfoc.test\r\n"
        "Host: hfoc.test\r\n"
        "\r\n",
        "Accept: text/html\r\n"
        "Host: hfoc.test\r\n"
        "host: hfoc.test\r\n"
        "\r\n",
        "Host: hfoc.test,evil\r\n\r\n",
        "Host: /evil\r\n\r\n",
        "Host: hfoc.test/path\r\n\r\n",
        "Host: hfoc.test?query\r\n\r\n",
        "Host: hfoc.test#frag\r\n\r\n",
        "Host: hfoc.test\\evil\r\n\r\n",
        "Host: hfoc test\r\n\r\n",
        "Host: hfoc.test:abc\r\n\r\n",
        "Host: hfoc.test:\r\n\r\n",
        "Host: :443\r\n\r\n",
        "Host: hfoc.test:443:extra\r\n\r\n",
    };
    enum http_policy_reason expected[] = {
        HTTP_POLICY_MISSING_HOST,
        HTTP_POLICY_EMPTY_HOST,
        HTTP_POLICY_EMPTY_HOST,
        HTTP_POLICY_DUPLICATE_HOST,
        HTTP_POLICY_DUPLICATE_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
        HTTP_POLICY_INVALID_HOST,
    };
    uint32_t expected_field[] = {
        0U,
        0U,
        0U,
        1U,
        2U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_policy_result result;
    uint32_t index;

    CHECK(init_parser(&parser),
          "host fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "host fixture starts slot zero");

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        memset(&result, 0, sizeof(result));
        CHECK(parse_get(&parser, &token, cases[index]) &&
                  http_policy_check(
                      &parser, &token, &result) == HTTP_PARSER_OK &&
                  policy_result_equal(
                      &result,
                      false,
                      true,
                      true,
                      false,
                      HTTP_METHOD_GET,
                      expected[index],
                      expected_field[index]),
              "missing, empty, duplicate, or invalid Host is rejected");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
                  http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "host fixture restarts slot");
    }
}

static void test_precedence_and_field_index(void)
{
    static const char missing_host[] =
        "Accept: text/html\r\n"
        "\r\n";
    static const char valid_host[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token token;
    struct http_policy_result result;

    CHECK(init_parser(&parser),
          "precedence fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "precedence fixture starts slot zero");

    memset(&result, 0, sizeof(result));
    CHECK(parse_request(
              &parser, &token, "PUT / HTTP/1.0\r\n", missing_host) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  false,
                  false,
                  false,
                  HTTP_METHOD_UNSUPPORTED,
                  HTTP_POLICY_METHOD,
                  0U),
          "unsupported method takes precedence over version and Host");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "precedence fixture restarts after method case");

    /*
     * ITEM-445 step 2: an accepted method on the form target does not mask a
     * refused version, so the version is the first refusal reported.
     */
    memset(&result, 0, sizeof(result));
    CHECK(parse_request(
              &parser, &token, "POST / HTTP/1.0\r\n", missing_host) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  true,
                  false,
                  false,
                  HTTP_METHOD_POST,
                  HTTP_POLICY_VERSION,
                  0U),
          "the form target reports a refused version before its Host");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "precedence fixture restarts after the form version case");

    memset(&result, 0, sizeof(result));
    CHECK(parse_request(
              &parser, &token, "GET / HTTP/1.0\r\n", missing_host) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  true,
                  false,
                  false,
                  HTTP_METHOD_GET,
                  HTTP_POLICY_VERSION,
                  0U),
          "unsupported version takes precedence over Host");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "precedence fixture restarts after version case");

    memset(&result, 0, sizeof(result));
    CHECK(parse_get(&parser, &token, missing_host) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  true,
                  true,
                  false,
                  HTTP_METHOD_GET,
                  HTTP_POLICY_MISSING_HOST,
                  0U),
          "missing Host is reported after method/version acceptance");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "precedence fixture restarts after Host case");

    memset(&result, 0, sizeof(result));
    CHECK(parse_request(
              &parser, &token, "PUT / HTTP/1.1\r\n", valid_host) &&
              http_policy_check(
                  &parser, &token, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  false,
                  true,
                  true,
                  HTTP_METHOD_UNSUPPORTED,
                  HTTP_POLICY_METHOD,
                  0U),
          "valid Host does not mask unsupported method");
}

static void test_isolation_and_restart(void)
{
    /* ITEM-445 step 2: POST on `/` is accepted now, so the denied slot uses
     * a method that is still unsupported. */
    static const char denied_line[] = "PUT / HTTP/1.1\r\n";
    static const char valid_headers[] =
        "Host: hfoc.test\r\n"
        "\r\n";
    struct http_parser parser;
    struct http_parser_token denied;
    struct http_parser_token accepted;
    struct http_policy_result result;
    struct http_parser_progress progress;
    uint32_t count = 0U;

    CHECK(init_parser(&parser),
          "isolation fixture initializes parser");
    denied = make_token(0U, 1U);
    accepted = make_token(1U, 2U);
    CHECK(http_parser_start(&parser, &denied) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &accepted) == HTTP_PARSER_OK &&
              parse_request(
                  &parser, &denied, denied_line, valid_headers) &&
              parse_get(&parser, &accepted, valid_headers),
          "isolation fixture prepares denied and accepted slots");

    memset(&result, 0, sizeof(result));
    CHECK(http_policy_check(
              &parser, &denied, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  false,
                  false,
                  true,
                  true,
                  HTTP_METHOD_UNSUPPORTED,
                  HTTP_POLICY_METHOD,
                  0U),
          "denied slot reports unsupported method");
    memset(&result, 0, sizeof(result));
    CHECK(http_policy_check(
              &parser, &accepted, &result) == HTTP_PARSER_OK &&
              policy_result_equal(
                  &result,
                  true,
                  true,
                  true,
                  true,
                  HTTP_METHOD_GET,
                  HTTP_POLICY_NONE,
                  0U),
          "accepted slot is unaffected by denied slot");
    CHECK(http_parser_active_count(&parser) == 2U,
          "policy isolation preserves both active slots");
    CHECK(http_parser_progress(
              &parser, &accepted, &progress) == HTTP_PARSER_OK &&
              progress.header_count ==
                  (uint32_t)strlen(valid_headers),
          "policy isolation preserves accepted header progress");
    CHECK(http_header_count(
              &parser, &accepted, &count) == HTTP_PARSER_OK &&
              count == 1U,
          "policy isolation preserves accepted header views");

    CHECK(http_parser_release(&parser, &accepted) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &accepted) == HTTP_PARSER_OK,
          "isolation fixture restarts accepted slot");
    CHECK(http_policy_check(
              &parser, &accepted, &result) ==
              HTTP_PARSER_HEADER_INCOMPLETE,
          "restarted slot requires complete headers again");
}

static void test_rejected_state(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress progress;
    struct http_policy_result result;
    bool complete = false;

    CHECK(init_parser(&parser),
          "rejected fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "rejected fixture starts slot zero");
    CHECK(http_request_line_feed(
              &parser,
              &token,
              (const uint8_t *)"GET / HTTP/1.1\r\n",
              16U,
              &complete) == HTTP_PARSER_OK &&
              complete,
          "rejected fixture completes request line");
    CHECK(http_parser_progress(
              &parser, &token, &progress) == HTTP_PARSER_OK,
          "rejected fixture reads request-line progress");
    CHECK(http_parser_reject(
              &parser,
              &token,
              &(struct http_parser_progress){
                  0U,
                  progress.request_line_count,
                  progress.request_line_count,
                  0U,
                  progress.request_line_count
              }) == HTTP_PARSER_OK &&
              http_parser_state(&parser, &token) ==
                  HTTP_PARSER_STATE_REJECTED,
          "rejected fixture publishes explicit REJECTED state");
    CHECK(http_policy_check(
              &parser, &token, &result) == HTTP_PARSER_TERMINAL,
          "policy check rejects terminal REJECTED slot");
}

int main(void)
{
    test_kind_and_reason_strings();
    test_preconditions_and_incomplete();
    test_accepted_requests();
    test_method_rejection();
    test_version_rejection();
    test_host_rejection();
    test_precedence_and_field_index();
    test_isolation_and_restart();
    test_rejected_state();

    if (failures != 0) {
        fprintf(stderr, "HTTP policy fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP policy host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_policy_unused;

#endif
