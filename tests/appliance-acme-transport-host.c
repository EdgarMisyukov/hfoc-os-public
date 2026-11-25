/*
 * ITEM-508 increment 11: host proof of the ACME exchange driver.
 *
 * The proof owns no socket and no TLS: the link is a scripted peer that
 * reports connection states, accepts a bounded number of bytes per send and
 * hands the reply over in fragments.  What is pinned is that nothing starts
 * before the clock is set, that the request leaves only after the link reports
 * the handshake complete and then leaves whole, that a reply framed by
 * `Content-Length` is complete when those bytes arrived while one framed by
 * the connection waits for the close, that a truncated or malformed reply is
 * a refusal by name, that deadlines and a backwards clock are refusals, and
 * that the one record carries neither the host, the body nor the nonce.
 */

#include "appliance_acme_transport.h"

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

struct fake_link {
    uint32_t open_calls;
    uint32_t advance_calls;
    uint32_t send_calls;
    uint32_t recv_calls;
    uint32_t close_calls;
    enum appliance_acme_transport_link_status open_result;
    enum appliance_acme_transport_link_status advance_result;
    enum appliance_acme_transport_link_status send_result;
    enum appliance_acme_transport_link_status recv_result;
    size_t send_limit;
    size_t recv_limit;
    enum appliance_acme_transport_link_status recv_when_empty;
    char opened_host[APPLIANCE_ACME_HTTP_HOST_MAX + 1U];
    uint16_t opened_port;
    bool opened_literal;
    uint8_t sent[APPLIANCE_ACME_HTTP_REQUEST_MAX];
    size_t sent_length;
    uint8_t reply[APPLIANCE_ACME_HTTP_RESPONSE_MAX];
    size_t reply_length;
    size_t reply_offset;
};

static struct fake_link fake;

static enum appliance_acme_transport_link_status fake_open(
    void *context,
    const char *host,
    uint16_t port,
    bool literal_ipv4)
{
    struct fake_link *link = context;

    ++link->open_calls;
    if (host != NULL) {
        snprintf(link->opened_host, sizeof(link->opened_host), "%s", host);
    }
    link->opened_port = port;
    link->opened_literal = literal_ipv4;
    return link->open_result;
}

static enum appliance_acme_transport_link_status fake_advance(void *context)
{
    struct fake_link *link = context;

    ++link->advance_calls;
    return link->advance_result;
}

static enum appliance_acme_transport_link_status fake_send(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    struct fake_link *link = context;
    size_t accepted = length;

    ++link->send_calls;
    *bytes_accepted = 0U;
    if (link->send_result != APPLIANCE_ACME_TRANSPORT_LINK_OK) {
        return link->send_result;
    }
    if (link->send_limit != 0U && accepted > link->send_limit) {
        accepted = link->send_limit;
    }
    if (accepted > sizeof(link->sent) - link->sent_length) {
        accepted = sizeof(link->sent) - link->sent_length;
    }
    memcpy(&link->sent[link->sent_length], bytes, accepted);
    link->sent_length += accepted;
    *bytes_accepted = accepted;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static enum appliance_acme_transport_link_status fake_recv(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    struct fake_link *link = context;
    size_t available = link->reply_length - link->reply_offset;
    size_t received = available;

    ++link->recv_calls;
    *bytes_received = 0U;
    if (link->recv_result != APPLIANCE_ACME_TRANSPORT_LINK_OK) {
        return link->recv_result;
    }
    if (available == 0U) {
        return link->recv_when_empty;
    }
    if (link->recv_limit != 0U && received > link->recv_limit) {
        received = link->recv_limit;
    }
    if (received > capacity) {
        received = capacity;
    }
    memcpy(bytes, &link->reply[link->reply_offset], received);
    link->reply_offset += received;
    *bytes_received = received;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

static void fake_close(void *context)
{
    struct fake_link *link = context;

    ++link->close_calls;
}

static struct appliance_acme_transport_link fake_interface(void)
{
    struct appliance_acme_transport_link link;

    link.context = &fake;
    link.open = fake_open;
    link.advance = fake_advance;
    link.send = fake_send;
    link.recv = fake_recv;
    link.close = fake_close;
    return link;
}

static void fake_reset(void)
{
    memset(&fake, 0, sizeof(fake));
    fake.open_result = APPLIANCE_ACME_TRANSPORT_LINK_OK;
    fake.advance_result = APPLIANCE_ACME_TRANSPORT_LINK_OK;
    fake.send_result = APPLIANCE_ACME_TRANSPORT_LINK_OK;
    fake.recv_result = APPLIANCE_ACME_TRANSPORT_LINK_OK;
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
}

static void fake_reply(const char *reply)
{
    fake.reply_length = strlen(reply);
    fake.reply_offset = 0U;
    memcpy(fake.reply, reply, fake.reply_length);
}

static const char request_method[] = "POST";
static const char request_url[] = "https://acme.example/new-order";
static const char request_type[] = "application/jose+json";
static const char request_body[] = "{\"protected\":\"x\"}";
#define TEST_NOW 1000U

static enum appliance_acme_transport_status start(
    struct appliance_acme_transport *transport)
{
    return appliance_acme_transport_begin(
        transport,
        request_method,
        sizeof(request_method) - 1U,
        request_url,
        sizeof(request_url) - 1U,
        request_type,
        sizeof(request_type) - 1U,
        (const uint8_t *)request_body,
        sizeof(request_body) - 1U,
        TEST_NOW);
}

/*
 * Start and then let the scripted link finish the handshake: `advance` with
 * the link reporting OK sends the request and reads the reply.
 */
static enum appliance_acme_transport_status start_and_advance(
    struct appliance_acme_transport *transport)
{
    const enum appliance_acme_transport_status status = start(transport);

    if (status != APPLIANCE_ACME_TRANSPORT_PENDING) {
        return status;
    }
    return appliance_acme_transport_advance(transport, TEST_NOW + 1U);
}

static void test_the_interface(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    CHECK(appliance_acme_transport_init(NULL, &link) ==
              APPLIANCE_ACME_TRANSPORT_NULL,
          "a NULL transport is refused");
    CHECK(appliance_acme_transport_init(&transport, NULL) ==
              APPLIANCE_ACME_TRANSPORT_NULL,
          "a NULL link is refused");
    link.send = NULL;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_BAD_LINK &&
              fake.open_calls == 0U,
          "an incomplete link interface is refused");

    link = fake_interface();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              appliance_acme_transport_published(&transport) &&
              transport.state == APPLIANCE_ACME_TRANSPORT_STATE_FREE,
          "a complete link interface publishes a transport");
    CHECK(strcmp(appliance_acme_transport_state_string(
                     APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING),
                 "connecting") == 0 &&
              strcmp(appliance_acme_transport_state_string(
                         APPLIANCE_ACME_TRANSPORT_STATE_DONE),
                     "done") == 0,
          "every state has its published name");
}

static void test_the_refusals_before_the_link(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the refusal cases");

    CHECK(appliance_acme_transport_begin(
              &transport, NULL, 0U, request_url, sizeof(request_url) - 1U,
              NULL, 0U, NULL, 0U, TEST_NOW) == APPLIANCE_ACME_TRANSPORT_NULL &&
              fake.open_calls == 0U,
          "a NULL method is refused before the link");
    CHECK(appliance_acme_transport_begin(
              &transport, request_method, sizeof(request_method) - 1U,
              request_url, sizeof(request_url) - 1U, request_type,
              sizeof(request_type) - 1U, (const uint8_t *)request_body,
              sizeof(request_body) - 1U, 0U) ==
              APPLIANCE_ACME_TRANSPORT_BAD_CLOCK &&
              fake.open_calls == 0U,
          "an unset clock is refused before the link");
    CHECK(appliance_acme_transport_begin(
              &transport, request_method, sizeof(request_method) - 1U,
              "http://acme.example/dir", 22U, request_type,
              sizeof(request_type) - 1U, (const uint8_t *)request_body,
              sizeof(request_body) - 1U, TEST_NOW) ==
              APPLIANCE_ACME_TRANSPORT_BAD_URL &&
              transport.http_status == APPLIANCE_ACME_HTTP_NOT_HTTPS &&
              fake.open_calls == 0U,
          "a plaintext URL is refused by the HTTP name");
    CHECK(appliance_acme_transport_begin(
              &transport, request_method, sizeof(request_method) - 1U,
              request_url, sizeof(request_url) - 1U, NULL, 0U,
              (const uint8_t *)request_body, sizeof(request_body) - 1U,
              TEST_NOW) == APPLIANCE_ACME_TRANSPORT_BAD_REQUEST &&
              transport.http_status == APPLIANCE_ACME_HTTP_BAD_CONTENT_TYPE &&
              fake.open_calls == 0U,
          "a request that cannot be built is refused before the link");

    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING &&
              fake.open_calls == 1U &&
              strcmp(fake.opened_host, "acme.example") == 0 &&
              fake.opened_port == 443U && !fake.opened_literal,
          "an accepted exchange opens the parsed host");
    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_BUSY &&
              fake.open_calls == 1U,
          "a second exchange while one is in flight is refused");
}

static void test_open_failure(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();
    struct appliance_acme_http_response response;

    fake_reset();
    fake.open_result = APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the open failure");
    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_OPEN_FAILED &&
              transport.state == APPLIANCE_ACME_TRANSPORT_STATE_FAILED &&
              fake.close_calls == 1U,
          "a refused connection fails and releases the link once");
    CHECK(strstr(transport.record, "outcome=failed") != NULL &&
              strstr(transport.record, "status=open-failed") != NULL,
          "the failure publishes its record");
    CHECK(appliance_acme_transport_take_response(&transport, &response) ==
              APPLIANCE_ACME_TRANSPORT_BAD_STATE,
          "a failed exchange has no reply to hand over");
}

static void test_connect_timeout_and_clock(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK && start(&transport) ==
              APPLIANCE_ACME_TRANSPORT_PENDING,
          "the transport is published for the deadline cases");
    CHECK(appliance_acme_transport_advance(
              &transport,
              TEST_NOW + APPLIANCE_ACME_TRANSPORT_CONNECT_DEADLINE_MS + 1U) ==
              APPLIANCE_ACME_TRANSPORT_TIMEOUT &&
              fake.advance_calls == 0U,
          "a connect window that closed is a timeout before the link is asked");
    CHECK(strstr(transport.record, "status=timeout") != NULL,
          "the timeout publishes its record");

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK && start(&transport) ==
              APPLIANCE_ACME_TRANSPORT_PENDING,
          "a fresh transport for the backwards clock");
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW - 1U) ==
              APPLIANCE_ACME_TRANSPORT_BAD_CLOCK &&
              fake.advance_calls == 0U,
          "a clock that runs backwards is refused, not subtracted");
}

static void test_the_request_leaves_whole(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();
    struct appliance_acme_http_target target;
    char expected[APPLIANCE_ACME_HTTP_REQUEST_MAX];
    size_t expected_length = 0U;

    fake_reset();
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    fake.send_limit = 7U;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the short-send case");
    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING,
          "the exchange starts");

    fake.advance_result = APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 1U) ==
              APPLIANCE_ACME_TRANSPORT_PENDING &&
              fake.sent_length == 0U,
          "no request byte leaves before the handshake completes");
    fake.advance_result = APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE;
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 2U) ==
              APPLIANCE_ACME_TRANSPORT_PENDING &&
              fake.sent_length == 0U,
          "a handshake that wants to write still sends nothing");

    fake.advance_result = APPLIANCE_ACME_TRANSPORT_LINK_OK;
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 3U) ==
              APPLIANCE_ACME_TRANSPORT_PENDING,
          "a short send leaves the exchange pending");
    CHECK(fake.sent_length ==
              APPLIANCE_ACME_TRANSPORT_SEND_BUDGET * 7U,
          "each turn accepts the bounded number of bytes the link takes");
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 4U) ==
              APPLIANCE_ACME_TRANSPORT_PENDING,
          "the remaining bytes are sent on the next turn");

    CHECK(appliance_acme_http_parse_url(
              request_url, sizeof(request_url) - 1U, &target) ==
              APPLIANCE_ACME_HTTP_OK &&
              appliance_acme_http_build_request(
                  request_method, sizeof(request_method) - 1U, &target,
                  request_type, sizeof(request_type) - 1U,
                  (const uint8_t *)request_body, sizeof(request_body) - 1U,
                  expected, sizeof(expected), &expected_length) ==
              APPLIANCE_ACME_HTTP_OK,
          "the reference request is built by the HTTP module");
    CHECK(fake.sent_length == expected_length &&
              memcmp(fake.sent, expected, expected_length) == 0,
          "the bytes the link received are exactly the HTTP request");
}

static void test_send_and_receive_failures(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    fake.send_result = APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_SEND_FAILED &&
              fake.close_calls == 1U,
          "a send the link refuses fails the exchange");

    fake_reset();
    fake.send_result = APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_CLOSED,
          "a peer that closes during the send is named closed");

    fake_reset();
    fake.recv_result = APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_RECV_FAILED,
          "a receive the link refuses fails the exchange");

    fake_reset();
    fake.advance_result = APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_HANDSHAKE_FAILED,
          "a handshake the link refuses fails the exchange");
}

static void test_the_reply_whole(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();
    struct appliance_acme_http_response response;
    static const char reply[] =
        "HTTP/1.1 201 Created\r\n"
        "Replay-Nonce: nonce-abc\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "\r\n"
        "{}";

    fake_reset();
    fake.recv_limit = 5U;
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the fragmented reply");
    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING,
          "the exchange starts");
    fake_reply(reply);
    {
        enum appliance_acme_transport_status status =
            APPLIANCE_ACME_TRANSPORT_PENDING;
        uint32_t turns = 0U;

        while (status == APPLIANCE_ACME_TRANSPORT_PENDING &&
               turns < 64U) {
            status = appliance_acme_transport_advance(
                &transport, TEST_NOW + 10U + turns);
            ++turns;
        }
        CHECK(status == APPLIANCE_ACME_TRANSPORT_OK,
              "a reply that arrives in fragments is read whole");
        CHECK(turns > 1U,
              "the fragmented reply took more than one turn");
    }
    CHECK(appliance_acme_transport_take_response(&transport, &response) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              response.status_code == 201U &&
              response.has_nonce &&
              strcmp(response.nonce, "nonce-abc") == 0 &&
              response.body_length == 2U &&
              memcmp(response.body, "{}", 2U) == 0,
          "the complete reply is handed over with its status, nonce and body");
    CHECK(strstr(transport.record, "outcome=done") != NULL &&
              strstr(transport.record, "status=ok") != NULL &&
              strstr(transport.record, "nonce-abc") == NULL &&
              strstr(transport.record, "acme.example") == NULL &&
              strstr(transport.record, "{}") == NULL &&
              strlen(transport.record) <
                  APPLIANCE_ACME_TRANSPORT_RECORD_MAX,
          "the record names the outcome and carries neither nonce, host nor body");
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 200U) ==
              APPLIANCE_ACME_TRANSPORT_BAD_STATE &&
              appliance_acme_transport_advance(&transport, TEST_NOW - 1U) ==
                  APPLIANCE_ACME_TRANSPORT_BAD_STATE &&
              appliance_acme_transport_take_response(
                  &transport, &response) == APPLIANCE_ACME_TRANSPORT_OK,
          "a finished exchange is not advanced again, even by a clock that went back");
    CHECK(fake.close_calls == 1U,
          "the link is released when the reply is complete");
}

static void test_the_connection_framing(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();
    struct appliance_acme_http_response response;

    /* Content-Length completes without waiting for the peer to close. */
    fake_reset();
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the framed reply");
    fake_reply("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\n{}");
    CHECK(start_and_advance(&transport) == APPLIANCE_ACME_TRANSPORT_OK,
          "a Content-Length reply completes when its bytes arrived");

    /* No Content-Length: the body is the rest of the connection. */
    fake_reset();
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the connection-framed reply");
    fake_reply("HTTP/1.1 200 OK\r\n\r\n{}");
    CHECK(start_and_advance(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING,
          "a reply framed by the connection waits while it is open");
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    CHECK(appliance_acme_transport_advance(&transport, TEST_NOW + 2U) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              appliance_acme_transport_take_response(
                  &transport, &response) == APPLIANCE_ACME_TRANSPORT_OK &&
              response.body_length == 2U,
          "the same reply completes when the peer closes");
}

static void test_the_reply_refusals(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the reply refusals");
    fake_reply("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n{}");
    CHECK(start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_BAD_REPLY &&
              transport.http_status == APPLIANCE_ACME_HTTP_TRUNCATED,
          "a reply shorter than it promised is truncated, not handed over");

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
               "2\r\n{}\r\n0\r\n\r\n");
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_BAD_REPLY &&
              transport.http_status ==
                  APPLIANCE_ACME_HTTP_TRANSFER_ENCODING_UNSUPPORTED,
          "a chunked framing is refused by its HTTP name");

    fake_reset();
    fake_reply("HTTP/1.1 200 OK\r\nContent-Length: 999999\r\n\r\n");
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_BAD_REPLY &&
              transport.http_status ==
                  APPLIANCE_ACME_HTTP_BUFFER_TOO_SMALL,
          "a declared body over the bound is refused by its HTTP name");

    fake_reset();
    fake_reply("ICY 200 OK\r\n\r\n");
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_BAD_REPLY &&
              transport.http_status == APPLIANCE_ACME_HTTP_BAD_STATUS_LINE,
          "a status line that is not HTTP is refused by its HTTP name");

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK,
          "the transport is published for the bound case");
    fake.reply_length = sizeof(fake.reply);
    memset(fake.reply, 'x', sizeof(fake.reply));
    fake.reply_offset = 0U;
    CHECK(start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_BUFFER_TOO_SMALL,
          "a reply that fills the buffer without a head is refused");

    fake_reset();
    fake.recv_when_empty = APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start_and_advance(&transport) ==
              APPLIANCE_ACME_TRANSPORT_CLOSED,
          "a peer that closes with no reply byte is named closed");
}

static void test_close_is_idempotent(void)
{
    struct appliance_acme_transport transport;
    struct appliance_acme_transport_link link = fake_interface();

    fake_reset();
    CHECK(appliance_acme_transport_init(&transport, &link) ==
              APPLIANCE_ACME_TRANSPORT_OK &&
              start(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING,
          "an exchange is in flight");
    appliance_acme_transport_close(&transport);
    appliance_acme_transport_close(&transport);
    CHECK(fake.close_calls == 1U &&
              transport.state == APPLIANCE_ACME_TRANSPORT_STATE_FREE,
          "closing twice releases the link once");
    CHECK(start(&transport) == APPLIANCE_ACME_TRANSPORT_PENDING &&
              fake.close_calls == 1U,
          "a closed transport starts the next exchange");
}

static void test_the_names(void)
{
    CHECK(strcmp(appliance_acme_transport_status_string(
                     APPLIANCE_ACME_TRANSPORT_PENDING), "pending") == 0 &&
              strcmp(appliance_acme_transport_status_string(
                         APPLIANCE_ACME_TRANSPORT_TIMEOUT), "timeout") == 0 &&
              strcmp(appliance_acme_transport_status_string(
                         APPLIANCE_ACME_TRANSPORT_BAD_REPLY),
                     "bad-reply") == 0 &&
              strcmp(appliance_acme_transport_status_string(
                         APPLIANCE_ACME_TRANSPORT_BUFFER_TOO_SMALL),
                     "buffer-too-small") == 0,
          "every refusal has its published name");
    CHECK(strcmp(appliance_acme_transport_status_string(
                     (enum appliance_acme_transport_status)99),
                 "unknown") == 0,
          "an unknown status is named unknown");
    CHECK(strcmp(appliance_acme_transport_state_string(
                     (enum appliance_acme_transport_state)99),
                 "unknown") == 0,
          "an unknown state is named unknown");
}

int main(void)
{
    if (APPLIANCE_ACME_TRANSPORT_VERSION != 1U ||
        APPLIANCE_ACME_TRANSPORT_CONNECT_DEADLINE_MS != 30000U ||
        APPLIANCE_ACME_TRANSPORT_REPLY_DEADLINE_MS != 60000U ||
        APPLIANCE_ACME_TRANSPORT_SEND_BUDGET != 16U ||
        APPLIANCE_ACME_TRANSPORT_RECV_BUDGET != 16U) {
        fprintf(stderr, "FAIL: the published constants changed\n");
        return 1;
    }

    test_the_interface();
    test_the_refusals_before_the_link();
    test_open_failure();
    test_connect_timeout_and_clock();
    test_the_request_leaves_whole();
    test_send_and_receive_failures();
    test_the_reply_whole();
    test_the_connection_framing();
    test_the_reply_refusals();
    test_close_is_idempotent();
    test_the_names();

    if (failures != 0) {
        fprintf(stderr, "appliance acme transport host test: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("appliance acme transport host test: OK\n");
    return 0;
}
