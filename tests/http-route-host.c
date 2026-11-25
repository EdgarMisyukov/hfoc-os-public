#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(HTTP_ROUTES_HOST_TEST)

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

static bool parse_request_line(
    struct http_parser *parser,
    const struct http_parser_token *token,
    const char *line)
{
    bool complete = false;

    return http_request_line_feed(
               parser,
               token,
               (const uint8_t *)line,
               (uint32_t)strlen(line),
               &complete) == HTTP_PARSER_OK &&
           complete;
}

static void test_table_and_strings(void)
{
    const struct http_route_descriptor *routes = NULL;
    uint32_t count = 0U;

    CHECK(http_route_table(&routes, &count) == HTTP_ROUTE_OK &&
              routes != NULL &&
              count == HTTP_ROUTE_COUNT,
          "route table publishes exactly the counted descriptors");
    CHECK(routes[0].id == HTTP_ROUTE_ROOT &&
              routes[0].target_length == HTTP_ROUTE_ROOT_LENGTH &&
              routes[0].target != NULL &&
              routes[0].target[0] == '/' &&
              routes[0].target[HTTP_ROUTE_ROOT_LENGTH] == '\0',
          "first route identity is exact root slash");
    CHECK(routes[1].id == HTTP_ROUTE_HEALTH &&
              routes[1].target_length == HTTP_ROUTE_HEALTH_LENGTH &&
              routes[1].target != NULL &&
              memcmp(
                  routes[1].target,
                  "/health",
                  HTTP_ROUTE_HEALTH_LENGTH) == 0 &&
              routes[1].target[HTTP_ROUTE_HEALTH_LENGTH] == '\0',
          "second route identity is exact health path");
    CHECK(routes[2].id == HTTP_ROUTE_CERTIFICATE &&
              routes[2].target_length == HTTP_ROUTE_CERTIFICATE_LENGTH &&
              routes[2].target != NULL &&
              memcmp(
                  routes[2].target,
                  "/hfoc-ca.crt",
                  HTTP_ROUTE_CERTIFICATE_LENGTH) == 0 &&
              routes[2].target[HTTP_ROUTE_CERTIFICATE_LENGTH] == '\0',
          "third route identity is the public certificate document");
    CHECK(http_route_id_string(HTTP_ROUTE_NONE) != NULL,
          "route none string is defined");
    CHECK(http_route_id_string(HTTP_ROUTE_ROOT) != NULL,
          "route root string is defined");
    CHECK(http_route_id_string(HTTP_ROUTE_HEALTH) != NULL,
          "route health string is defined");
    /* ITEM-503: the certificate route is the third one; anything past the
     * count is still unknown. */
    CHECK(http_route_id_string(HTTP_ROUTE_CERTIFICATE) != NULL,
          "route certificate string is defined");
    CHECK(http_route_id_string(
              (enum http_route_id)(HTTP_ROUTE_COUNT + 1U)) == NULL,
          "unknown route string is null");
    CHECK(http_route_table(NULL, &count) == HTTP_ROUTE_NULL,
          "route table rejects null descriptor output");
    CHECK(http_route_table(&routes, NULL) == HTTP_ROUTE_NULL,
          "route table rejects null count output");
}

static void test_exact_matches(void)
{
    enum http_route_id route = HTTP_ROUTE_NONE;
    const uint8_t root[] = "/";
    const uint8_t health[] = "/health";
    const uint8_t certificate[] = "/hfoc-ca.crt";

    CHECK(http_route_match(
              root,
              sizeof(root) - 1U,
              &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_ROOT,
          "route match resolves exact root target");
    route = HTTP_ROUTE_NONE;
    CHECK(http_route_match(
              health,
              sizeof(health) - 1U,
              &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_HEALTH,
          "route match resolves exact health target");
    route = HTTP_ROUTE_NONE;
    CHECK(http_route_match(
              certificate,
              sizeof(certificate) - 1U,
              &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_CERTIFICATE,
          "route match resolves the public certificate document");
    route = HTTP_ROUTE_NONE;
    CHECK(http_route_match(
              root,
              sizeof(root) - 1U,
              &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_ROOT &&
              http_route_match(
                  root,
                  sizeof(root) - 1U,
                  &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_ROOT,
          "route match is deterministic and side-effect free");
}

static void test_unknown_and_no_normalization(void)
{
    static const char *cases[] = {
        "",
        "/ ",
        " /",
        "//",
        "/health/",
        "/Health",
        "/HEALTH",
        "health",
        "*",
        "/*",
        "/health/*",
        "/health?x",
        "/?x",
        "/health#x",
        "/#",
        "/../health",
        "/health/..",
        "/%2e%2e/health",
        "http://hfoc.test/health",
        "/health?x=1",
        "/health//",
        "/health/.",
        "/health/%2e",
    };
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line line;
    enum http_route_id route;
    uint32_t index;

    for (index = 0U;
         index < sizeof(cases) / sizeof(cases[0]);
         ++index) {
        route = HTTP_ROUTE_ROOT;
        CHECK(http_route_match(
                  (const uint8_t *)cases[index],
                  (uint32_t)strlen(cases[index]),
                  &route) == HTTP_ROUTE_OK &&
                  route == HTTP_ROUTE_NONE,
              "unknown, query, wildcard, or traversal target is not routed");
    }

    CHECK(init_parser(&parser),
          "route fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "route fixture starts slot zero");
    CHECK(parse_request_line(
              &parser, &token, "GET /health?x HTTP/1.1\r\n"),
          "route fixture parses query-bearing request line");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK,
          "route fixture reads query-bearing request line");
    route = HTTP_ROUTE_ROOT;
    CHECK(http_route_match_line(&line, &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_NONE,
          "route match does not strip query from request target");
}

static void test_preconditions_and_request_line_integration(void)
{
    struct http_parser parser;
    struct http_parser_token token;
    struct http_request_line line;
    struct http_request_line bad_line;
    enum http_route_id route = HTTP_ROUTE_ROOT;
    const uint8_t target[] = "/";

    CHECK(http_route_match(
              NULL, 1U, &route) == HTTP_ROUTE_NULL &&
              route == HTTP_ROUTE_ROOT,
          "route match rejects null target without mutation");
    CHECK(http_route_match(
              target, sizeof(target) - 1U, NULL) == HTTP_ROUTE_NULL,
          "route match rejects null route output");
    CHECK(http_route_match_line(NULL, &route) == HTTP_ROUTE_NULL,
          "route line match rejects null line");
    memset(&bad_line, 0, sizeof(bad_line));
    CHECK(http_route_match_line(&bad_line, &route) ==
              HTTP_ROUTE_NULL,
          "route line match rejects null target view");

    CHECK(init_parser(&parser),
          "integration fixture initializes parser");
    token = make_token(0U, 1U);
    CHECK(http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "integration fixture starts slot zero");
    CHECK(parse_request_line(
              &parser, &token, "GET / HTTP/1.1\r\n"),
          "integration fixture parses root request line");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK,
          "integration fixture reads root request line");
    route = HTTP_ROUTE_NONE;
    CHECK(http_route_match_line(&line, &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_ROOT,
          "route line match resolves root from item 272 fields");
    CHECK(http_parser_state(&parser, &token) ==
              HTTP_PARSER_STATE_REQUEST_LINE,
          "route line match does not mutate parser state");

    CHECK(http_parser_release(&parser, &token) == HTTP_PARSER_OK &&
              http_parser_start(&parser, &token) == HTTP_PARSER_OK,
          "integration fixture restarts slot");
    CHECK(parse_request_line(
              &parser, &token, "HEAD /health HTTP/1.1\r\n"),
          "integration fixture parses health request line");
    CHECK(http_request_line_get(
              &parser, &token, &line) == HTTP_PARSER_OK,
          "integration fixture reads health request line");
    route = HTTP_ROUTE_NONE;
    CHECK(http_route_match_line(&line, &route) == HTTP_ROUTE_OK &&
              route == HTTP_ROUTE_HEALTH,
          "route line match resolves health from item 272 fields");
}

int main(void)
{
    test_table_and_strings();
    test_exact_matches();
    test_unknown_and_no_normalization();
    test_preconditions_and_request_line_integration();

    if (failures != 0) {
        fprintf(stderr, "HTTP route fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTP route host tests: ALL OK\n");
    return 0;
}

#else

typedef int http_route_unused;

#endif
