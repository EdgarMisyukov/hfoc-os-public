/*
 * ITEM-508 increment 10: host proof of the ACME client's HTTP exchange.
 *
 * The proof owns no socket and no TLS: it fixes URLs, request bodies and the
 * exact bytes a server answers with.  What it pins is that only an absolute
 * https URL is accepted and split into host, port and target; that a request
 * carries exactly the method, the host, the content type and the length the
 * item needs and nothing else, and that a request which does not fit is
 * refused whole; that a reply is read only when the caller received it whole,
 * with the headers the item acts on captured and unknown ones skipped; and
 * that a malformed status line, a folded header, a duplicate of a header the
 * item acts on, a chunked framing and a lying content length are refusals by
 * name rather than a shorter or differently framed answer.
 */

#include "appliance_acme_http.h"

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

static enum appliance_acme_http_status parse(const char *url,
                                             struct appliance_acme_http_target *target)
{
    return appliance_acme_http_parse_url(url, strlen(url), target);
}

static enum appliance_acme_http_status read_reply(
    const char *reply,
    struct appliance_acme_http_response *response)
{
    return appliance_acme_http_read_response(
        (const uint8_t *)reply, strlen(reply), response);
}

static void test_the_url(void)
{
    struct appliance_acme_http_target target;

    CHECK(parse("https://acme-v02.api.letsencrypt.org/directory", &target) ==
              APPLIANCE_ACME_HTTP_OK,
          "a directory URL is accepted");
    CHECK(strcmp(target.host, "acme-v02.api.letsencrypt.org") == 0 &&
              target.port == 443U &&
              strcmp(target.target, "/directory") == 0,
          "the host, the default port and the target are split");
    CHECK(!target.literal_ipv4,
          "a DNS name is not marked as a literal");
    CHECK(strstr(target.record, "host=acme-v02.api.letsencrypt.org") != NULL &&
              strstr(target.record, "port=443") != NULL &&
              strstr(target.record, "literal=no") != NULL,
          "the target publishes where the request goes");

    CHECK(parse("https://acme.example", &target) == APPLIANCE_ACME_HTTP_OK &&
              strcmp(target.target, "/") == 0,
          "a URL without a path asks for the root");

    CHECK(parse("https://127.0.0.1:14000/dir", &target) ==
              APPLIANCE_ACME_HTTP_OK,
          "the offline stub's literal address and port are accepted");
    CHECK(target.literal_ipv4 && target.port == 14000U &&
              strcmp(target.host, "127.0.0.1") == 0,
          "a dotted quad is marked as a literal and keeps its port");

    CHECK(parse("HTTPS://ACME.EXAMPLE/x", &target) == APPLIANCE_ACME_HTTP_OK,
          "the scheme is case-insensitive");
    CHECK(strcmp(target.host, "ACME.EXAMPLE") == 0,
          "the host is kept as written for SNI");

    CHECK(parse("https://acme.example/new-order?x=1&y=2", &target) ==
              APPLIANCE_ACME_HTTP_OK &&
              strcmp(target.target, "/new-order?x=1&y=2") == 0,
          "the query string stays part of the target");

    CHECK(parse("http://acme.example/directory", &target) ==
              APPLIANCE_ACME_HTTP_NOT_HTTPS,
          "a plaintext URL is refused by name");
    CHECK(parse("ftp://acme.example/directory", &target) ==
              APPLIANCE_ACME_HTTP_BAD_URL,
          "another scheme is not an ACME URL");
    CHECK(parse("https://", &target) == APPLIANCE_ACME_HTTP_BAD_HOST &&
              parse("https:///directory", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_HOST,
          "an empty authority is refused");
    CHECK(parse("https://user@acme.example/directory", &target) ==
              APPLIANCE_ACME_HTTP_BAD_URL,
          "userinfo is refused");
    CHECK(parse("https://[::1]/directory", &target) ==
              APPLIANCE_ACME_HTTP_BAD_HOST,
          "an IPv6 literal is refused by name");
    CHECK(parse("https://acme.example:0/dir", &target) ==
              APPLIANCE_ACME_HTTP_BAD_PORT &&
              parse("https://acme.example:65536/dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_PORT &&
              parse("https://acme.example:abc/dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_PORT &&
              parse("https://acme.example:/dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_PORT,
          "a port outside 1..65535 is refused");
    CHECK(parse("https://1.2.3.256/dir", &target) ==
              APPLIANCE_ACME_HTTP_BAD_HOST &&
              parse("https://1.2.3/dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_HOST &&
              parse("https://01.2.3.4/dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_HOST,
          "a dotted name that is not an address is refused");
    CHECK(parse("https://acme..example/dir", &target) ==
              APPLIANCE_ACME_HTTP_BAD_HOST &&
              parse("https://acme.example./dir", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_HOST,
          "an empty label is not a name");
    CHECK(parse("https://acme.example/dir#fragment", &target) ==
              APPLIANCE_ACME_HTTP_BAD_URL &&
              parse("https://acme.example/a b", &target) ==
                  APPLIANCE_ACME_HTTP_BAD_URL,
          "a fragment and a space in the target are refused");
    CHECK(appliance_acme_http_parse_url(NULL, 10U, &target) ==
              APPLIANCE_ACME_HTTP_NULL &&
              appliance_acme_http_parse_url("https://a/", 10U, NULL) ==
                  APPLIANCE_ACME_HTTP_NULL,
          "a NULL caller is refused");
}

static void test_the_request(void)
{
    struct appliance_acme_http_target target;
    struct appliance_acme_http_target stub;
    char request[APPLIANCE_ACME_HTTP_REQUEST_MAX];
    size_t length = 0U;
    const char body[] = "{\"protected\":\"x\"}";
    static const char expected[] =
        "POST /new-order HTTP/1.1\r\n"
        "Host: acme.example\r\n"
        "Content-Type: application/jose+json\r\n"
        "Content-Length: 17\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"protected\":\"x\"}";
    static const char expected_get[] =
        "GET /new-order HTTP/1.1\r\n"
        "Host: acme.example\r\n"
        "Connection: close\r\n"
        "\r\n";
    static const char expected_stub[] =
        "POST /dir HTTP/1.1\r\n"
        "Host: 127.0.0.1:14000\r\n"
        "Content-Type: application/jose+json\r\n"
        "Content-Length: 17\r\n"
        "Connection: close\r\n"
        "\r\n"
        "{\"protected\":\"x\"}";

    CHECK(parse("https://acme.example/new-order", &target) ==
              APPLIANCE_ACME_HTTP_OK,
          "the request target parses");
    CHECK(appliance_acme_http_build_request(
              "POST", 4U, &target, "application/jose+json", 21U,
              (const uint8_t *)body, sizeof(body) - 1U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_OK,
          "a POST request is built");
    CHECK(length == sizeof(expected) - 1U &&
              memcmp(request, expected, length) == 0,
          "the POST is exactly the bytes the item sends");
    CHECK(request[length] == '\0',
          "the request is terminated");

    CHECK(appliance_acme_http_build_request(
              "GET", 3U, &target, NULL, 0U, NULL, 0U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_OK,
          "a GET request is built");
    CHECK(length == sizeof(expected_get) - 1U &&
              memcmp(request, expected_get, length) == 0,
          "a GET carries no body, no content type and no length");

    CHECK(appliance_acme_http_build_request(
              "HEAD", 4U, &target, NULL, 0U, NULL, 0U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_OK &&
              strncmp(request, "HEAD /new-order HTTP/1.1\r\n", 25U) == 0,
          "a HEAD is built like a GET");

    CHECK(parse("https://127.0.0.1:14000/dir", &stub) ==
              APPLIANCE_ACME_HTTP_OK &&
              appliance_acme_http_build_request(
                  "POST", 4U, &stub, "application/jose+json", 21U,
                  (const uint8_t *)body, sizeof(body) - 1U, request,
                  sizeof(request), &length) == APPLIANCE_ACME_HTTP_OK,
          "a POST to the stub is built");
    CHECK(length == sizeof(expected_stub) - 1U &&
              memcmp(request, expected_stub, length) == 0,
          "a non-default port is named in the Host header");

    CHECK(appliance_acme_http_build_request(
              "PUT", 3U, &target, NULL, 0U, NULL, 0U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_BAD_METHOD,
          "a method the item does not send is refused");
    CHECK(appliance_acme_http_build_request(
              "post", 4U, &target, "application/jose+json", 21U,
              (const uint8_t *)body, sizeof(body) - 1U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_BAD_METHOD,
          "methods are case-sensitive: post is not POST");
    CHECK(appliance_acme_http_build_request(
              "POST", 4U, &target, NULL, 0U, (const uint8_t *)body,
              sizeof(body) - 1U, request, sizeof(request), &length) ==
              APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE,
          "a POST without a content type is refused");
    CHECK(appliance_acme_http_build_request(
              "POST", 4U, &target, "application/jose+json", 21U, NULL, 0U,
              request, sizeof(request), &length) ==
              APPLIANCE_ACME_HTTP_BAD_BODY,
          "an ACME POST is a JWS and cannot be empty");
    CHECK(appliance_acme_http_build_request(
              "GET", 3U, &target, NULL, 0U, (const uint8_t *)body,
              sizeof(body) - 1U, request, sizeof(request), &length) ==
              APPLIANCE_ACME_HTTP_BAD_BODY,
          "a GET with a body is refused");

    CHECK(appliance_acme_http_build_request(
              "POST", 4U, &target, "application/jose+json", 21U,
              (const uint8_t *)body, sizeof(body) - 1U, request, 10U,
              &length) == APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL,
          "a buffer too small is refused");
    CHECK(length == 0U && request[0] == '\0',
          "a refused request leaves nothing behind");
    CHECK(appliance_acme_http_build_request(
              NULL, 0U, &target, NULL, 0U, NULL, 0U, request,
              sizeof(request), &length) == APPLIANCE_ACME_HTTP_NULL,
          "a NULL method is refused");
}

static void test_the_reply(void)
{
    struct appliance_acme_http_response response;
    static const char reply[] =
        "HTTP/1.1 201 Created\r\n"
        "Replay-Nonce: nonce-1\r\n"
        "Location: https://acme.example/acct/7\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "X-Unknown: skipped\r\n"
        "\r\n"
        "{}";

    CHECK(read_reply(reply, &response) == APPLIANCE_ACME_HTTP_OK,
          "a complete reply is read");
    CHECK(response.status_code == 201U,
          "the status code is read");
    CHECK(response.has_nonce && strcmp(response.nonce, "nonce-1") == 0,
          "the replay nonce is captured");
    CHECK(response.has_location &&
              strcmp(response.location, "https://acme.example/acct/7") == 0,
          "the location is captured");
    CHECK(response.has_content_type &&
              strcmp(response.content_type, "application/json") == 0,
          "the content type is captured");
    CHECK(response.has_content_length && response.content_length == 2U &&
              response.body_length == 2U &&
              memcmp(response.body, "{}", 2U) == 0,
          "the body is read whole");
    CHECK(strstr(response.record, "status=201") != NULL &&
              strstr(response.record, "bytes=2") != NULL,
          "the reply publishes its status and size");
    CHECK(strstr(response.record, "{") == NULL &&
              strstr(response.record, "nonce-1") == NULL,
          "the record carries neither body nor nonce");

    CHECK(read_reply("HTTP/1.0 200 OK\r\nContent-Length: 2\r\n\r\n{}",
                     &response) == APPLIANCE_ACME_HTTP_OK &&
              response.status_code == 200U,
          "HTTP/1.0 is spoken");
    CHECK(read_reply("HTTP/1.1 200\r\n\r\n", &response) ==
              APPLIANCE_ACME_HTTP_OK &&
              response.status_code == 200U && response.body_length == 0U,
          "a reply without headers or body is read");
    CHECK(read_reply("HTTP/1.1 204 No Content\r\n\r\n", &response) ==
              APPLIANCE_ACME_HTTP_OK &&
              response.status_code == 204U,
          "a bare status line with a reason is read");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                     "\r\n{}",
                     &response) == APPLIANCE_ACME_HTTP_OK &&
              response.body_length == 2U,
          "a body without a content length is the rest of the reply");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nLocation:   https://a/b  \r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_OK &&
              strcmp(response.location, "https://a/b") == 0,
          "the optional whitespace around a value is trimmed");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nlOCATION: https://a/b\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_OK &&
              strcmp(response.location, "https://a/b") == 0,
          "header names are case-insensitive");

    CHECK(appliance_acme_http_status_is_success(200U) &&
              appliance_acme_http_status_is_success(299U) &&
              !appliance_acme_http_status_is_success(300U) &&
              !appliance_acme_http_status_is_success(199U),
          "the 2xx class is the item's success");
}

static void test_the_reply_refusals(void)
{
    struct appliance_acme_http_response response;

    CHECK(read_reply("", &response) == APPLIANCE_ACME_HTTP_TRUNCATED,
          "an empty reply is not read");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{",
                     &response) == APPLIANCE_ACME_HTTP_TRUNCATED,
          "a body shorter than promised is truncated");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{} ",
                     &response) == APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH,
          "a body longer than promised lies about its length");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Length: two\r\n\r\n{}",
                     &response) == APPLIANCE_ACME_HTTP_BAD_CONTENT_LENGTH &&
              read_reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                         "Content-Length: 2\r\n\r\n{}",
                         &response) ==
                  APPLIANCE_ACME_HTTP_DUPLICATE_HEADER,
          "a non-numeric and a duplicated content length are refused");

    CHECK(read_reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                     "2\r\n{}\r\n0\r\n\r\n",
                     &response) ==
              APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED,
          "a chunked framing is refused by name, not parsed as a body");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nLocation: https://a/b\r\n"
                     "Location: https://a/c\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_DUPLICATE_HEADER &&
              read_reply("HTTP/1.1 200 OK\r\nReplay-Nonce: a\r\n"
                         "Replay-Nonce: b\r\n\r\n",
                         &response) ==
                  APPLIANCE_ACME_HTTP_DUPLICATE_HEADER,
          "a header the item acts on is not answered twice");

    CHECK(read_reply("HTTP/1.1 20 OK\r\n\r\n", &response) ==
              APPLIANCE_ACME_HTTP_BAD_STATUS_LINE &&
              read_reply("HTTP/2 200\r\n\r\n", &response) ==
                  APPLIANCE_ACME_HTTP_BAD_STATUS_LINE &&
              read_reply("HTTP/1.2 200 OK\r\n\r\n", &response) ==
                  APPLIANCE_ACME_HTTP_BAD_STATUS_LINE &&
              read_reply("ICY 200 OK\r\n\r\n", &response) ==
                  APPLIANCE_ACME_HTTP_BAD_STATUS_LINE,
          "a status line that is not HTTP/1.x with three digits is refused");
    CHECK(read_reply("HTTP/1.1 200 OK\n\n", &response) ==
              APPLIANCE_ACME_HTTP_TRUNCATED,
          "a bare line feed is not a header terminator");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nBroken\r\n\r\n", &response) ==
              APPLIANCE_ACME_HTTP_BAD_HEADER,
          "a header without a colon is refused");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nLocation: https://a/b\r\n"
                     " continued\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_BAD_HEADER,
          "an obs-fold is refused");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nLocation: https://a/\x01b\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_BAD_HEADER,
          "a control byte in a value is refused");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nLocation: https://a/b\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_OK,
          "the fixture for the too-long cases is valid");
    CHECK(read_reply("HTTP/1.1 200 OK\r\nX-Unknown: skipped\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_OK,
          "an unknown header is skipped");
    CHECK(appliance_acme_http_read_response(NULL, 0U, &response) ==
              APPLIANCE_ACME_HTTP_NULL &&
              appliance_acme_http_read_response((const uint8_t *)"", 0U, NULL) ==
                  APPLIANCE_ACME_HTTP_NULL,
          "a NULL caller is refused");
}

static void test_the_bounds(void)
{
    struct appliance_acme_http_target target;
    struct appliance_acme_http_response response;
    char url[APPLIANCE_ACME_HTTP_URL_MAX + 2U];
    char reply[APPLIANCE_ACME_HTTP_RESPONSE_MAX + 64U];
    size_t position = 0U;

    /* A URL one byte over the bound. */
    memcpy(url, "https://acme.example/", 21U);
    position = 21U;
    while (position < APPLIANCE_ACME_HTTP_URL_MAX + 1U) {
        url[position++] = 'a';
    }
    url[position] = '\0';
    CHECK(parse(url, &target) == APPLIANCE_ACME_HTTP_BAD_URL,
          "a URL over the bound is refused");

    /* A target that fills the URL bound without crossing it is still read. */
    memcpy(url, "https://a/", 10U);
    position = 10U;
    while (position < APPLIANCE_ACME_HTTP_URL_MAX) {
        url[position++] = 'b';
    }
    url[position] = '\0';
    CHECK(appliance_acme_http_parse_url(url, position, &target) ==
              APPLIANCE_ACME_HTTP_OK &&
              target.target_length ==
                  APPLIANCE_ACME_HTTP_URL_MAX - 10U + 1U,
          "a target that fills the URL bound is read whole");

    /* A header block over the bound. */
    memcpy(reply, "HTTP/1.1 200 OK\r\n", 17U);
    position = 17U;
    while (position < APPLIANCE_ACME_HTTP_HEADER_BLOCK_MAX + 64U) {
        reply[position++] = 'x';
    }
    memcpy(&reply[position], "\r\n\r\n", 4U);
    position += 4U;
    CHECK(appliance_acme_http_read_response(
              (const uint8_t *)reply, position, &response) ==
              APPLIANCE_ACME_HTTP_HEADER_TOO_LONG,
          "a header block over the bound is refused");

    /* Too many headers. */
    position = 0U;
    memcpy(&reply[position], "HTTP/1.1 200 OK\r\n", 17U);
    position += 17U;
    for (uint32_t index = 0U;
         index < APPLIANCE_ACME_HTTP_HEADER_COUNT_MAX + 1U; ++index) {
        const int written = snprintf(
            &reply[position], 32U, "X-N-%u: v\r\n", (unsigned)index);

        position += (size_t)written;
    }
    memcpy(&reply[position], "\r\n", 2U);
    position += 2U;
    CHECK(appliance_acme_http_read_response(
              (const uint8_t *)reply, position, &response) ==
              APPLIANCE_ACME_HTTP_TOO_MANY_HEADERS,
          "more headers than the bound are refused");

    /* A body over the bound, with and without a declared length. */
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL,
          "a declared body over the bound is refused");
    /*
     * The longest length a header can spell is ten digits, and 4294967296 is
     * the first value that would wrap a 32-bit accumulator back to zero: the
     * bound must refuse it as it accumulates, never accept it as an empty body.
     */
    CHECK(read_reply("HTTP/1.1 200 OK\r\nContent-Length: 4294967296\r\n\r\n",
                     &response) == APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL,
          "a ten-digit content length is refused by the bound, not wrapped");
}

static void test_the_names(void)
{
    CHECK(strcmp(appliance_acme_http_status_string(APPLIANCE_ACME_HTTP_OK),
                 "ok") == 0 &&
              strcmp(appliance_acme_http_status_string(
                         APPLIANCE_ACME_HTTP_NOT_HTTPS), "not-https") == 0 &&
              strcmp(appliance_acme_http_status_string(
                         APPLIANCE_ACME_HTTP_DUPLICATE_HEADER),
                     "duplicate-header") == 0 &&
              strcmp(appliance_acme_http_status_string(
                         APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED),
                     "transfer-encoding-unsupported") == 0,
          "every refusal has its published name");
    CHECK(strcmp(appliance_acme_http_status_string(
                     (enum appliance_acme_http_status)99), "unknown") == 0,
          "an unknown status is named unknown");
}

int main(void)
{
    if (APPLIANCE_ACME_HTTP_VERSION != 1U ||
        APPLIANCE_ACME_HTTP_PORT_DEFAULT != 443U ||
        APPLIANCE_ACME_HTTP_REQUEST_MAX != 2816U ||
        APPLIANCE_ACME_HTTP_RESPONSE_MAX != 8192U ||
        APPLIANCE_ACME_HTTP_BODY_MAX != 6144U) {
        fprintf(stderr, "FAIL: the published constants changed\n");
        return 1;
    }
    test_the_url();
    test_the_request();
    test_the_reply();
    test_the_reply_refusals();
    test_the_bounds();
    test_the_names();

    if (failures != 0) {
        fprintf(stderr, "appliance acme http host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme http host test: OK\n");
    return 0;
}
