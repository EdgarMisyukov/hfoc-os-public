/*
 * ITEM-508 increment 7: host proof of the HTTP-01 answer decision.
 *
 * The proof owns no socket and no clock: it arms a token at a fixed epoch and
 * decides fixed request lines.  What it pins is that the listener answers the
 * one path the item allows and nothing else - the exact well-known path for
 * the armed token, GET with the key authorization as the body, HEAD with no
 * body, a token that is not the armed one refused, a query string refused, a
 * method that is not GET or HEAD refused, a token whose 900 s lifetime has
 * passed refused, and every request after disarm refused.
 */

#include "appliance_acme_challenge.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, ...) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: "); \
        fprintf(stderr, __VA_ARGS__); \
        fprintf(stderr, "\n"); \
        ++failures; \
    } else { \
        printf("PASS: "); \
        printf(__VA_ARGS__); \
        printf("\n"); \
    } \
} while (0)

#define PREFIX APPLIANCE_ACME_CHALLENGE_PREFIX
#define PATH_CAPACITY (sizeof(PREFIX) + 64U)

static const char token[] = "tok3n-ABC_123";
static const char key_authorization[] =
    "tok3n-ABC_123.LPJNl1Ec0Y7RZ5F8yfKd4QoJcQo0Y6Vf3PjcK1sWq3A";

static enum appliance_acme_challenge_status arm_fixture(
    struct appliance_acme_challenge_arm *arm)
{
    return appliance_acme_challenge_arm_token(
        arm, token, sizeof(token) - 1U, key_authorization,
        sizeof(key_authorization) - 1U, 1000U);
}

static enum appliance_acme_challenge_status decide_exact_at(
    struct appliance_acme_challenge_arm *arm,
    const char *method,
    uint32_t now,
    const char **body,
    size_t *body_length)
{
    char path[PATH_CAPACITY];

    snprintf(path, sizeof(path), "%s%s", PREFIX, token);
    return appliance_acme_challenge_decide(
        arm, method, strlen(method), path, strlen(path), now, body,
        body_length);
}

static void test_arming_validation(void)
{
    struct appliance_acme_challenge_arm arm;

    CHECK(appliance_acme_challenge_arm_token(
              NULL, token, sizeof(token) - 1U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null arm is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, NULL, 0U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null token is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token) - 1U, NULL, 0U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null key authorization is refused");

    CHECK(appliance_acme_challenge_arm_token(
              &arm, "", 0U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_TOKEN,
          "an empty token is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, "tok3n/ABC", 9U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_TOKEN,
          "a token outside the base64url alphabet is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, "tok3n.ABC", 9U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_TOKEN,
          "a token with a dot is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, "tok3n+ABC", 9U, key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_TOKEN,
          "a token with base64 padding is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token), key_authorization,
              sizeof(key_authorization) - 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_TOKEN,
          "a token longer than its bound is refused");

    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token) - 1U, key_authorization, 0U,
              1000U) == APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "an empty key authorization is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token) - 1U, key_authorization,
              APPLIANCE_ACME_CHALLENGE_ANSWER_MAX + 1U, 1000U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "a key authorization longer than its bound is refused");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token) - 1U, key_authorization,
              sizeof(key_authorization) - 1U, 0U) ==
              APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "an unvalidated clock refuses to arm");
    CHECK(appliance_acme_challenge_arm_token(
              &arm, token, sizeof(token) - 1U, key_authorization,
              sizeof(key_authorization) - 1U, UINT32_MAX) ==
              APPLIANCE_ACME_CHALLENGE_BAD_STATE,
          "a clock whose lifetime would wrap the deadline refuses to arm");
}

static void test_the_answer(void)
{
    struct appliance_acme_challenge_arm arm;
    const char *body = NULL;
    size_t body_length = 0U;
    char path[PATH_CAPACITY];

    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms at a validated clock");
    CHECK(arm.armed && arm.token_length == sizeof(token) - 1U &&
              arm.answer_length == sizeof(key_authorization) - 1U &&
              arm.armed_at_epoch_seconds == 1000U &&
              arm.deadline_epoch_seconds ==
                  1000U + APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS,
          "arming records the token, the answer and the deadline");
    CHECK(strcmp(arm.token, token) == 0 &&
              strcmp(arm.answer, key_authorization) == 0,
          "arming stores the exact token and key authorization");
    CHECK(arm.answer[arm.answer_length] == '\0',
          "the stored answer stays terminated");

    snprintf(path, sizeof(path), "%s%s", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK,
          "a GET of the exact path is answered");
    CHECK(body != NULL && body_length == sizeof(key_authorization) - 1U &&
              memcmp(body, key_authorization,
                     sizeof(key_authorization) - 1U) == 0,
          "the body is exactly the key authorization");
    CHECK(arm.served_count == 1U, "the answered GET is counted");

    body = NULL;
    body_length = 0U;
    CHECK(appliance_acme_challenge_decide(
              &arm, "HEAD", 4U, path, strlen(path), 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK,
          "a HEAD of the exact path is answered");
    CHECK(body == NULL && body_length == 0U,
          "HEAD answers the headers with no body");
    CHECK(arm.served_count == 2U, "the answered HEAD is counted");

    /*
     * The whole lifetime is servable up to and including the deadline second;
     * one second later the token is stale.
     */
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1000U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK,
          "the deadline second itself is still served");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path),
              arm.deadline_epoch_seconds + 1U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_EXPIRED,
          "a token past its lifetime is expired");
    CHECK(body == NULL && body_length == 0U,
          "an expired token answers no body");

    /*
     * Re-arming replaces the token: the answer of the old path is not served
     * any more, and the old token is gone with it.
     */
    CHECK(appliance_acme_challenge_arm_token(
              &arm, "next-token", 10U, "next-token.thumbprint", 21U,
              2000U) == APPLIANCE_ACME_CHALLENGE_OK,
          "a second token arms over the first");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 2001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "the replaced token's path is not found");
    CHECK(strcmp(arm.token, "next-token") == 0 &&
              strcmp(arm.answer, "next-token.thumbprint") == 0,
          "re-arming replaces the token and the answer");
}

static void test_exact_path(void)
{
    struct appliance_acme_challenge_arm arm;
    const char *body = (const char *)&arm;
    size_t body_length = 99U;
    char path[PATH_CAPACITY];

    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms for the path proof");

    snprintf(path, sizeof(path), "%s%s", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK,
          "the well-known path is served before the refusals");
    CHECK(body_length == sizeof(key_authorization) - 1U,
          "the served body has the key authorization's length");

    snprintf(path, sizeof(path), "%s%s/", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "a trailing slash is not the exact path");
    snprintf(path, sizeof(path), "%s%s?x=1", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "a query string is not the exact path");
    snprintf(path, sizeof(path), "%s%s#frag", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "a fragment is not the exact path");
    CHECK(body == NULL && body_length == 0U,
          "a refused path answers no body");
    snprintf(path, sizeof(path), "%s%s", PREFIX, "tok3n-ABC_124");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "another token is not served");
    snprintf(path, sizeof(path), "%s", PREFIX);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "the prefix alone is not served");
    snprintf(path, sizeof(path), "%stok3n-abc_123", PREFIX);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "the token comparison does not fold case");
    snprintf(path, sizeof(path), "/.well-known/acme-challenges/%s", token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "a lookalike prefix is not served");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, "", 0U, 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "an empty path is not served");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, NULL, 0U, 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "a null path is not served");

    snprintf(path, sizeof(path), "%s%s", PREFIX, token);
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 0U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_EXPIRED,
          "an unvalidated clock at the request is expired");
    CHECK(arm.served_count == 1U,
          "only the one served request was counted");
}

static void test_methods(void)
{
    static const struct {
        const char *method;
    } methods[] = {{"POST"}, {"PUT"}, {"DELETE"}, {"OPTIONS"}};
    struct appliance_acme_challenge_arm arm;
    const char *body = NULL;
    size_t body_length = 0U;

    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms for the method proof");

    for (size_t index = 0U; index < sizeof(methods) / sizeof(methods[0]);
         ++index) {
        CHECK(decide_exact_at(&arm, methods[index].method, 1001U, &body,
                              &body_length) ==
                  APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
              "the method %s is not allowed", methods[index].method);
        CHECK(body == NULL && body_length == 0U,
              "the refused %s answers no body", methods[index].method);
    }
    CHECK(decide_exact_at(&arm, "get", 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
          "the method comparison does not fold case");
    CHECK(decide_exact_at(&arm, "GETX", 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
          "a method with a suffix is not GET");
    CHECK(decide_exact_at(&arm, "HEADX", 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
          "a method with a suffix is not HEAD");
    CHECK(appliance_acme_challenge_decide(
              &arm, NULL, 0U, "/", 1U, 1001U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
          "a null method is not allowed");
    CHECK(arm.served_count == 0U,
          "a refused request is not counted as served");
}

static void test_the_request_line(void)
{
    struct appliance_acme_challenge_request request;
    static const char good[] =
        "GET /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.1\r\n";
    static const char good_lf[] =
        "HEAD /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.0\n";
    static const char *const malformed[] = {
        "",
        "GET",
        "GET ",
        "GET /x",
        "GET /x ",
        " GET /x HTTP/1.1",
        "GET  /x HTTP/1.1",
        "GET\t/x HTTP/1.1",
        "GET x HTTP/1.1",
        "GET * HTTP/1.1",
        "GET /x HTTP/2.0",
        "GET /x HTTP/1.2",
        "GET /x HTTP/1.1 extra",
        "GET /x HTTP/1.1\r",
        "GET /\x01x HTTP/1.1",
        "GET /x HTTP/1.1\x7f",
        "GET _x HTTP/1.1",
        "PROPFINDXYZABCDE /x HTTP/1.1",
    };

    CHECK(appliance_acme_challenge_read_request_line(
              good, sizeof(good) - 1U, &request) ==
              APPLIANCE_ACME_CHALLENGE_OK,
          "a well-formed request line parses");
    CHECK(strcmp(request.method, "GET") == 0 &&
              request.method_length == 3U &&
              strcmp(request.path, "/.well-known/acme-challenge/tok3n-ABC_123")
                  == 0 &&
              request.path_length ==
                  sizeof("/.well-known/acme-challenge/tok3n-ABC_123") - 1U,
          "the parsed method and target are exact");

    CHECK(appliance_acme_challenge_read_request_line(
              good_lf, sizeof(good_lf) - 1U, &request) ==
              APPLIANCE_ACME_CHALLENGE_OK &&
              strcmp(request.method, "HEAD") == 0,
          "a line ending in a bare LF and HTTP/1.0 parses");

    for (size_t index = 0U;
         index < sizeof(malformed) / sizeof(malformed[0]); ++index) {
        CHECK(appliance_acme_challenge_read_request_line(
                  malformed[index], strlen(malformed[index]), &request) ==
                  APPLIANCE_ACME_CHALLENGE_BAD_REQUEST,
              "the malformed request line %zu is refused", index);
    }
    CHECK(appliance_acme_challenge_read_request_line(
              NULL, 0U, &request) == APPLIANCE_ACME_CHALLENGE_NULL,
          "a null request line is refused");
    CHECK(appliance_acme_challenge_read_request_line(
              good, sizeof(good) - 1U, NULL) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null request output is refused");
    CHECK(appliance_acme_challenge_read_request_line(
              "GET /x HTTP/1.1", 14U, &request) ==
              APPLIANCE_ACME_CHALLENGE_BAD_REQUEST,
          "a truncated request line is refused");

    /* The longest target the listener speaks fits exactly, one byte more does
     * not, and a refused target is never partly copied. */
    {
        char line[APPLIANCE_ACME_CHALLENGE_PATH_MAX + 32U];
        char target[APPLIANCE_ACME_CHALLENGE_PATH_MAX + 2U];

        memset(target, 'a', sizeof(target));
        target[0] = '/';
        target[APPLIANCE_ACME_CHALLENGE_PATH_MAX] = '\0';
        snprintf(line, sizeof(line), "GET %s HTTP/1.1", target);
        CHECK(appliance_acme_challenge_read_request_line(
                  line, strlen(line), &request) ==
                  APPLIANCE_ACME_CHALLENGE_OK &&
                  request.path_length ==
                      APPLIANCE_ACME_CHALLENGE_PATH_MAX &&
                  request.path[APPLIANCE_ACME_CHALLENGE_PATH_MAX] == '\0',
              "the longest target fits exactly");

        target[APPLIANCE_ACME_CHALLENGE_PATH_MAX] = 'a';
        target[APPLIANCE_ACME_CHALLENGE_PATH_MAX + 1U] = '\0';
        snprintf(line, sizeof(line), "GET %s HTTP/1.1", target);
        CHECK(appliance_acme_challenge_read_request_line(
                  line, strlen(line), &request) ==
                  APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL,
              "a target one byte longer than the buffer is refused");
        CHECK(request.path_length == 0U && request.method_length == 0U &&
                  request.path[0] == '\0',
              "a refused target is not partly copied");
    }
}

static void test_the_decision_from_the_line(void)
{
    struct appliance_acme_challenge_arm arm;
    const char *body = NULL;
    size_t body_length = 0U;
    static const char good[] =
        "GET /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.1\r\n";
    static const char good_head[] =
        "HEAD /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.1\r\n";
    static const char other_token[] =
        "GET /.well-known/acme-challenge/tok3n-ABC_124 HTTP/1.1\r\n";
    static const char with_query[] =
        "GET /.well-known/acme-challenge/tok3n-ABC_123?a=b HTTP/1.1\r\n";
    static const char post[] =
        "POST /.well-known/acme-challenge/tok3n-ABC_123 HTTP/1.1\r\n";
    static const char malformed[] = "GARBAGE\r\n";

    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms for the line proof");

    CHECK(appliance_acme_challenge_decide_line(
              &arm, good, sizeof(good) - 1U, 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK &&
              body != NULL && body_length == sizeof(key_authorization) - 1U &&
              memcmp(body, key_authorization, body_length) == 0,
          "the listener answers the request line with the key authorization");
    CHECK(appliance_acme_challenge_decide_line(
              &arm, good_head, sizeof(good_head) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_OK &&
              body == NULL && body_length == 0U,
          "the listener answers a HEAD line with no body");
    CHECK(appliance_acme_challenge_decide_line(
              &arm, other_token, sizeof(other_token) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "the listener refuses a line for another token");
    CHECK(appliance_acme_challenge_decide_line(
              &arm, with_query, sizeof(with_query) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NOT_FOUND,
          "the listener refuses a line with a query string");
    CHECK(appliance_acme_challenge_decide_line(
              &arm, post, sizeof(post) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED,
          "the listener refuses a POST line");
    body = (const char *)&arm;
    body_length = 77U;
    CHECK(appliance_acme_challenge_decide_line(
              &arm, malformed, sizeof(malformed) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_BAD_REQUEST &&
              body == NULL && body_length == 0U,
          "a malformed line never reaches the decision");
    CHECK(appliance_acme_challenge_decide_line(
              &arm, NULL, 0U, 1002U, &body, &body_length) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null line never reaches the decision");

    appliance_acme_challenge_disarm(&arm);
    CHECK(appliance_acme_challenge_decide_line(
              &arm, good, sizeof(good) - 1U, 1002U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE,
          "a disarmed listener refuses the request line");
}

static void test_arming_state(void)
{
    struct appliance_acme_challenge_arm arm;
    const char *body = NULL;
    size_t body_length = 0U;
    char path[PATH_CAPACITY];

    snprintf(path, sizeof(path), "%s%s", PREFIX, token);
    memset(&arm, 0, sizeof(arm));
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE,
          "an unarmed listener answers no challenge");

    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms before disarm");
    appliance_acme_challenge_disarm(&arm);
    CHECK(!arm.armed && arm.token_length == 0U && arm.answer_length == 0U &&
              arm.served_count == 0U,
          "disarm clears the token, the answer and the count");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE,
          "a disarmed listener answers no challenge even for the old path");
    appliance_acme_challenge_disarm(&arm);
    appliance_acme_challenge_disarm(NULL);
    CHECK(true, "disarm is idempotent and accepts null");

    CHECK(appliance_acme_challenge_decide(
              NULL, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NULL,
          "a null arm refuses the decision");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, NULL,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NULL,
          "a null body output refuses the decision");
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body, NULL) ==
              APPLIANCE_ACME_CHALLENGE_NULL,
          "a null length output refuses the decision");

    /* An armed structure written by another version is not a challenge. */
    CHECK(arm_fixture(&arm) == APPLIANCE_ACME_CHALLENGE_OK,
          "the token arms again for the version proof");
    arm.version = APPLIANCE_ACME_CHALLENGE_VERSION + 1U;
    CHECK(appliance_acme_challenge_decide(
              &arm, "GET", 3U, path, strlen(path), 1001U, &body,
              &body_length) == APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE,
          "an armed structure from another version is not a challenge");
}

static void test_the_names(void)
{
    static const struct {
        enum appliance_acme_challenge_status status;
        const char *name;
    } names[] = {
        {APPLIANCE_ACME_CHALLENGE_OK, "OK"},
        {APPLIANCE_ACME_CHALLENGE_NULL, "NULL"},
        {APPLIANCE_ACME_CHALLENGE_BAD_STATE, "BAD_STATE"},
        {APPLIANCE_ACME_CHALLENGE_BAD_TOKEN, "BAD_TOKEN"},
        {APPLIANCE_ACME_CHALLENGE_NO_CHALLENGE, "NO_CHALLENGE"},
        {APPLIANCE_ACME_CHALLENGE_NOT_FOUND, "NOT_FOUND"},
        {APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED, "METHOD_NOT_ALLOWED"},
        {APPLIANCE_ACME_CHALLENGE_EXPIRED, "EXPIRED"},
        {APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL, "BUFFER_TOO_SMALL"},
        {APPLIANCE_ACME_CHALLENGE_BAD_REQUEST, "BAD_REQUEST"},
    };

    for (size_t index = 0U; index < sizeof(names) / sizeof(names[0]);
         ++index) {
        CHECK(strcmp(appliance_acme_challenge_status_string(names[index].status),
                     names[index].name) == 0,
              "the status %s has its name", names[index].name);
    }
    CHECK(strcmp(appliance_acme_challenge_status_string(
                     (enum appliance_acme_challenge_status)99),
                 "UNKNOWN") == 0,
          "an unknown status is named UNKNOWN");
}

int main(void)
{
    if (APPLIANCE_ACME_CHALLENGE_VERSION != 1U ||
        strcmp(APPLIANCE_ACME_CHALLENGE_PREFIX,
               "/.well-known/acme-challenge/") != 0 ||
        APPLIANCE_ACME_CHALLENGE_LIFETIME_SECONDS != 900U) {
        fprintf(stderr, "FAIL: the published constants changed\n");
        return 1;
    }
    test_arming_validation();
    test_the_answer();
    test_exact_path();
    test_methods();
    test_the_request_line();
    test_the_decision_from_the_line();
    test_arming_state();
    test_the_names();

    if (failures != 0) {
        fprintf(stderr, "appliance acme challenge host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme challenge host test: OK\n");
    return 0;
}
