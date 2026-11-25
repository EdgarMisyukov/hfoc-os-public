#include "http_parser.h"
#include "http_response.h"
#include "https_handler.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTPS_HANDLER_HOST_TEST)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const uint8_t root_body[] =
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>HFOC Appliance</title>\n"
    "<style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:60rem;line-height:1.45}nav a{margin-right:.6rem}code{background:#eee;padding:0 .2rem}.state{font-weight:700}.notice{color:#a00;font-weight:700}footer{margin-top:2rem;color:#555;font-size:.9rem}</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>HFOC Appliance</h1>\n"
    "<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>\n"
    "<main>\n"
    "<h2>Management sign-in</h2>\n"
    "<p class=\"state\">SIGNED OUT</p>\n"
    "<form method=\"post\" action=\"/\">\n"
    "<p><label>Login <input type=\"text\" name=\"username\" autocomplete=\"username\"></label></p>\n"
    "<p><label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\"></label></p>\n"
    "<p><button type=\"submit\">Sign in</button></p>\n"
    "</form>\n"
    "<p>Set the account on the machine console with <code>credentials</code>, then sign in here with the same login and password.</p>\n"
    "<p>The panel requires an administrator session. Sign in above with the login and password set on the local console, or present the issued token in the <code>Authorization: Bearer</code> request header. A sign-in answers one <code>HttpOnly</code>, <code>Secure</code>, <code>SameSite=Strict</code> session cookie with a short lifetime. The panel loads no browser script and no external asset.</p>\n"
    "<p>Session: none</p>\n"
    "<p>Management API discovery: <code>/api/v1</code></p>\n"
    "</main>\n"
    "<footer>HFOC Appliance management panel</footer>\n"
    "</body>\n"
    "</html>\n";
static const uint8_t health_body[] =
    "{\"status\":\"ok\",\"route\":\"health\"}";

static const char root_get_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 1611\r\n"
    "Connection: close\r\n"
    "Cache-Control: no-store\r\n"
    "Strict-Transport-Security: max-age=31536000\r\n"
    "Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline'; base-uri 'none'; form-action 'self'; frame-ancestors 'none'\r\n"
    "Referrer-Policy: no-referrer\r\n"
    "X-Content-Type-Options: nosniff\r\n"
    "X-Frame-Options: DENY\r\n"
    "\r\n"
    "<!doctype html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>HFOC Appliance</title>\n"
    "<style>body{font-family:system-ui,sans-serif;margin:2rem;max-width:60rem;line-height:1.45}nav a{margin-right:.6rem}code{background:#eee;padding:0 .2rem}.state{font-weight:700}.notice{color:#a00;font-weight:700}footer{margin-top:2rem;color:#555;font-size:.9rem}</style>\n"
    "</head>\n"
    "<body>\n"
    "<h1>HFOC Appliance</h1>\n"
    "<nav>Dashboard Network Services Storage System Security Diagnostics Updates Recovery Routing Account Certificates Help</nav>\n"
    "<main>\n"
    "<h2>Management sign-in</h2>\n"
    "<p class=\"state\">SIGNED OUT</p>\n"
    "<form method=\"post\" action=\"/\">\n"
    "<p><label>Login <input type=\"text\" name=\"username\" autocomplete=\"username\"></label></p>\n"
    "<p><label>Password <input type=\"password\" name=\"password\" autocomplete=\"current-password\"></label></p>\n"
    "<p><button type=\"submit\">Sign in</button></p>\n"
    "</form>\n"
    "<p>Set the account on the machine console with <code>credentials</code>, then sign in here with the same login and password.</p>\n"
    "<p>The panel requires an administrator session. Sign in above with the login and password set on the local console, or present the issued token in the <code>Authorization: Bearer</code> request header. A sign-in answers one <code>HttpOnly</code>, <code>Secure</code>, <code>SameSite=Strict</code> session cookie with a short lifetime. The panel loads no browser script and no external asset.</p>\n"
    "<p>Session: none</p>\n"
    "<p>Management API discovery: <code>/api/v1</code></p>\n"
    "</main>\n"
    "<footer>HFOC Appliance management panel</footer>\n"
    "</body>\n"
    "</html>\n";

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

static const char root_head_expected[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/html; charset=utf-8\r\n"
    "Content-Length: 1611\r\n"
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

static bool parse_complete(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *request_line,
    const char *headers)
{
    struct http_parser_progress progress;
    bool complete = false;

    if (http_request_line_feed(
            parser,
            token,
            (const uint8_t *)request_line,
            (uint32_t)strlen(request_line),
            &complete) != HTTP_PARSER_OK ||
        !complete ||
        http_parser_progress(
            parser, token, &progress) != HTTP_PARSER_OK) {
        return false;
    }
    if (http_parser_advance(
            parser,
            token,
            HTTP_PARSER_STATE_HEADERS,
            &(struct http_parser_progress){
                0U,
                progress.request_line_count,
                progress.request_line_count,
                0U,
                progress.request_line_count
            }) != HTTP_PARSER_OK) {
        return false;
    }
    complete = false;
    if (http_header_feed(
            parser,
            token,
            (const uint8_t *)headers,
            (uint32_t)strlen(headers),
            &complete) != HTTP_PARSER_OK ||
        !complete) {
        return false;
    }
    return http_parser_state(parser, token) ==
           HTTP_PARSER_STATE_COMPLETE;
}

static void check_exact_response(
    const struct http_response *response,
    const uint8_t *expected,
    uint32_t expected_length,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;
    enum http_response_status status;

    status = http_response_encode(
        response,
        output,
        sizeof(output),
        &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == expected_length &&
              memcmp(output, expected, expected_length) == 0,
          message);
}

static void check_exact_head(
    const struct http_response *response,
    const uint8_t *expected,
    uint32_t expected_length,
    const char *message)
{
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;
    enum http_response_status status;

    status = http_response_encode_head(
        response,
        output,
        sizeof(output),
        &written);
    CHECK(status == HTTP_RESPONSE_OK &&
              written == expected_length &&
              memcmp(output, expected, expected_length) == 0,
          message);
}

static void test_status_strings(void)
{
    static const char *strings[] = {
        "HTTPS_HANDLER_OK",
        "HTTPS_HANDLER_NULL",
        "HTTPS_HANDLER_BAD_STATE",
        "HTTPS_HANDLER_BAD_VERSION",
        "HTTPS_HANDLER_NOT_COMPLETE",
        "HTTPS_HANDLER_FRAMING_DENIED",
        "HTTPS_HANDLER_POLICY_DENIED",
        "HTTPS_HANDLER_UNROUTED",
        "HTTPS_HANDLER_RESPONSE_OVERFLOW",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(strings) / sizeof(strings[0]);
         ++index) {
        const char *string = https_handler_status_string(
            (enum https_handler_status)index);

        CHECK(string != NULL &&
                  strcmp(string, strings[index]) == 0,
              "HTTPS handler status string is exact");
    }
    CHECK(https_handler_status_string(
              (enum https_handler_status)99U) == NULL,
          "HTTPS handler undefined status is rejected");
}

static void test_null_and_version(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct https_handler_result result;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 123U;

    memset(&result, 0, sizeof(result));
    memset(&parser, 0, sizeof(parser));
    token = make_token(0U, 1U);

    CHECK(https_handler_dispatch(
              NULL, &token, &result) == HTTPS_HANDLER_NULL,
          "handler dispatch rejects null parser");
    CHECK(https_handler_dispatch(
              &parser, NULL, &result) == HTTPS_HANDLER_NULL,
          "handler dispatch rejects null token");
    CHECK(https_handler_dispatch(
              &parser, &token, NULL) == HTTPS_HANDLER_NULL,
          "handler dispatch rejects null result");
    CHECK(https_handler_encode(
              NULL,
              output,
              sizeof(output),
              &written) == HTTPS_HANDLER_NULL,
          "handler encode rejects null result");
    CHECK(https_handler_encode(
              &result,
              NULL,
              sizeof(output),
              &written) == HTTPS_HANDLER_NULL,
          "handler encode rejects null output");
    CHECK(https_handler_encode(
              &result,
              output,
              sizeof(output),
              NULL) == HTTPS_HANDLER_NULL,
          "handler encode rejects null written");

    result.version = 99U;
    CHECK(https_handler_encode(
              &result,
              output,
              sizeof(output),
              &written) == HTTPS_HANDLER_BAD_VERSION,
          "handler encode rejects bad result version");
}

static void test_valid_dispatch(void)
{
    static const struct {
        const char *request_line;
        const char *headers;
        enum http_route_id route;
        enum http_method_kind method;
        const uint8_t *body;
        uint32_t body_length;
        enum http_content_type content_type;
        const char *get_expected;
        const char *head_expected;
        const char *message;
    } cases[] = {
        {
            "GET / HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_ROUTE_ROOT,
            HTTP_METHOD_GET,
            root_body,
            (uint32_t)(sizeof(root_body) - 1U),
            HTTP_CONTENT_TYPE_HTML_UTF8,
            root_get_expected,
            root_head_expected,
            "root GET handler response bytes are exact",
        },
        {
            "HEAD / HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_ROUTE_ROOT,
            HTTP_METHOD_HEAD,
            root_body,
            (uint32_t)(sizeof(root_body) - 1U),
            HTTP_CONTENT_TYPE_HTML_UTF8,
            root_get_expected,
            root_head_expected,
            "root HEAD handler response bytes are exact",
        },
        {
            "GET /health HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_ROUTE_HEALTH,
            HTTP_METHOD_GET,
            health_body,
            (uint32_t)(sizeof(health_body) - 1U),
            HTTP_CONTENT_TYPE_JSON,
            health_get_expected,
            health_head_expected,
            "health GET handler response bytes are exact",
        },
        {
            "HEAD /health HTTP/1.1\r\n",
            "Host: hfoc.test\r\n\r\n",
            HTTP_ROUTE_HEALTH,
            HTTP_METHOD_HEAD,
            health_body,
            (uint32_t)(sizeof(health_body) - 1U),
            HTTP_CONTENT_TYPE_JSON,
            health_get_expected,
            health_head_expected,
            "health HEAD handler response bytes are exact",
        },
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_progress before;
    struct http_parser_progress after;
    struct https_handler_result result;
    uint32_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        CHECK(init_parser(&parser),
              "handler fixture initializes parser");
        token = make_token(0U, 1U);
        CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
              "handler fixture starts parser slot");
        CHECK(parse_complete(
                  &parser,
                  &token,
                  cases[index].request_line,
                  cases[index].headers),
              "handler fixture parses complete request");
        CHECK(http_parser_progress(
                  &parser, &token, &before) == HTTP_PARSER_OK,
              "handler fixture reads progress before dispatch");
        memset(&result, 0, sizeof(result));
        CHECK(https_handler_dispatch(
                  &parser, &token, &result) == HTTPS_HANDLER_OK &&
                  result.version == HTTPS_HANDLER_VERSION &&
                  result.route == cases[index].route &&
                  result.method == cases[index].method &&
                  result.response.status == HTTP_STATUS_200_OK &&
                  result.response.content_type ==
                      cases[index].content_type &&
                  result.response.body != NULL &&
                  result.response.body_length ==
                      cases[index].body_length &&
                  memcmp(
                      result.response.body,
                      cases[index].body,
                      cases[index].body_length) == 0,
              "valid dispatch publishes fixed route, method, and body");
        CHECK(http_parser_progress(
                  &parser, &token, &after) == HTTP_PARSER_OK &&
                  before.request_line_count ==
                      after.request_line_count &&
                  before.header_count == after.header_count &&
                  before.total_count == after.total_count &&
                  http_parser_state(&parser, &token) ==
                      HTTP_PARSER_STATE_COMPLETE,
              "handler dispatch does not mutate parser progress");

        check_exact_response(
            &result.response,
            (const uint8_t *)cases[index].get_expected,
            (uint32_t)(strlen(cases[index].get_expected)),
            cases[index].message);
        check_exact_head(
            &result.response,
            (const uint8_t *)cases[index].head_expected,
            (uint32_t)(strlen(cases[index].head_expected)),
            "HEAD handler response has zero body bytes");

        memset(&result, 0, sizeof(result));
        CHECK(https_handler_dispatch(
                  &parser, &token, &result) == HTTPS_HANDLER_OK &&
                  result.route == cases[index].route,
              "handler dispatch is deterministic and side-effect free");
        CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
              "handler fixture releases parser slot");
    }
}

static void test_rejections(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_parser_token stale;
    struct https_handler_result result;

    CHECK(init_parser(&parser),
          "handler rejection fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "handler rejection fixture starts parser slot");
    memset(&result, 0, sizeof(result));
    CHECK(https_handler_dispatch(
              &parser, &token, &result) ==
              HTTPS_HANDLER_NOT_COMPLETE &&
              result.route == HTTP_ROUTE_NONE,
          "incomplete request does not publish handler result");

    CHECK(parse_complete(
              &parser,
              &token,
              "GET / HTTP/1.1\r\n",
              "Host: hfoc.test\r\nContent-Length: 0\r\n\r\n"),
          "handler rejection fixture parses body-bearing request");
    memset(&result, 0, sizeof(result));
    CHECK(https_handler_dispatch(
              &parser, &token, &result) ==
              HTTPS_HANDLER_FRAMING_DENIED &&
              result.route == HTTP_ROUTE_NONE,
          "body-bearing request is denied before handler dispatch");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "framing rejection fixture releases parser slot");

    CHECK(init_parser(&parser),
          "policy rejection fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              parse_complete(
                  &parser,
                  &token,
                  "POST / HTTP/1.1\r\n",
                  "Host: hfoc.test\r\n\r\n"),
          "policy rejection fixture parses unsupported method");
    memset(&result, 0, sizeof(result));
    CHECK(https_handler_dispatch(
              &parser, &token, &result) ==
              HTTPS_HANDLER_POLICY_DENIED &&
              result.route == HTTP_ROUTE_NONE,
          "unsupported method is denied before handler dispatch");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "policy rejection fixture releases parser slot");

    CHECK(init_parser(&parser),
          "route rejection fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              parse_complete(
                  &parser,
                  &token,
                  "GET /missing HTTP/1.1\r\n",
                  "Host: hfoc.test\r\n\r\n"),
          "route rejection fixture parses unknown route");
    memset(&result, 0, sizeof(result));
    CHECK(https_handler_dispatch(
              &parser, &token, &result) == HTTPS_HANDLER_UNROUTED &&
              result.route == HTTP_ROUTE_NONE,
          "unknown route does not publish a handler result");

    stale = make_token(0U, 2U);
    CHECK(https_handler_dispatch(
              &parser, &stale, &result) ==
              HTTPS_HANDLER_NOT_COMPLETE &&
              result.route == HTTP_ROUTE_NONE,
          "stale token does not publish a handler result");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "route rejection fixture releases parser slot");
}

static void test_encode_overflow(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct https_handler_result result;
    uint8_t output[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 99U;
    enum https_handler_status encode_status;

    CHECK(init_parser(&parser),
          "encode overflow fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK &&
              parse_complete(
                  &parser,
                  &token,
                  "GET / HTTP/1.1\r\n",
                  "Host: hfoc.test\r\n\r\n"),
          "encode overflow fixture parses root request");
    memset(&result, 0, sizeof(result));
    CHECK(https_handler_dispatch(
              &parser, &token, &result) == HTTPS_HANDLER_OK,
          "encode overflow fixture dispatches root request");
    encode_status = https_handler_encode(
        &result,
        output,
        (uint32_t)(strlen(root_head_expected) - 1U),
        &written);
    CHECK(encode_status == HTTPS_HANDLER_RESPONSE_OVERFLOW &&
              written == 99U,
          "short handler output is rejected without mutation");
    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK,
          "encode overflow fixture releases parser slot");
}

int main(void)
{
    test_status_strings();
    test_null_and_version();
    test_valid_dispatch();
    test_rejections();
    test_encode_overflow();

    if (failures != 0) {
        fprintf(stderr,
                "HTTPS root/health handler host failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTPS root/health handler host test: ALL OK\n");
    return 0;
}

#else

typedef int https_handler_unused;

#endif
