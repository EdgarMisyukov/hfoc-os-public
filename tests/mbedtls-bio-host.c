#include "hfoc_mbedtls_bio.h"

#include "mbedtls/ssl.h"

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

enum sink_mode {
    SINK_MODE_FULL = 0,
    SINK_MODE_SHORT = 1,
    SINK_MODE_RETAIN = 2,
    SINK_MODE_CLOSED = 3,
    SINK_MODE_REJECT = 4,
};

struct sink {
    enum sink_mode write_mode;
    enum sink_mode read_mode;
    size_t short_write;
    size_t short_read;
    uint8_t inbound[32];
    size_t inbound_length;
    size_t inbound_offset;
    uint8_t accepted_log[64];
    size_t accepted_length;
    size_t write_calls;
    size_t read_calls;
};

static enum hfoc_mbedtls_bio_transport_status
sink_send(void *opaque,
          const uint8_t *bytes,
          size_t length,
          size_t *bytes_accepted)
{
    struct sink *sink = opaque;
    size_t accepted;

    *bytes_accepted = 0U;
    ++sink->write_calls;
    if (sink->write_mode == SINK_MODE_RETAIN) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    if (sink->write_mode == SINK_MODE_CLOSED) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED;
    }
    if (sink->write_mode == SINK_MODE_REJECT) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }

    accepted = length;
    if (sink->write_mode == SINK_MODE_SHORT &&
        sink->short_write < accepted) {
        accepted = sink->short_write;
    }
    if (accepted == 0U || accepted > length) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    if (accepted > sizeof(sink->accepted_log) -
            sink->accepted_length) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    memcpy(
        sink->accepted_log + sink->accepted_length,
        bytes,
        accepted);
    sink->accepted_length += accepted;
    *bytes_accepted = accepted;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

static enum hfoc_mbedtls_bio_transport_status
sink_recv(void *opaque,
          uint8_t *bytes,
          size_t capacity,
          size_t *bytes_received)
{
    struct sink *sink = opaque;
    size_t available;
    size_t received;

    *bytes_received = 0U;
    ++sink->read_calls;
    available = sink->inbound_length - sink->inbound_offset;
    if (available == 0U) {
        if (sink->read_mode == SINK_MODE_CLOSED) {
            return HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED;
        }
        if (sink->read_mode == SINK_MODE_REJECT) {
            return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
        }
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }

    received = available;
    if (received > capacity) {
        received = capacity;
    }
    if (sink->read_mode == SINK_MODE_SHORT &&
        sink->short_read < received) {
        received = sink->short_read;
    }
    if (received == 0U) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    memcpy(
        bytes,
        sink->inbound + sink->inbound_offset,
        received);
    sink->inbound_offset += received;
    *bytes_received = received;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

static void init_sink(struct sink *sink)
{
    memset(sink, 0, sizeof(*sink));
    sink->write_mode = SINK_MODE_FULL;
    sink->read_mode = SINK_MODE_FULL;
    sink->short_write = 1U;
    sink->short_read = 1U;
}

static bool init_bio(struct hfoc_mbedtls_bio *bio)
{
    memset(bio, 0, sizeof(*bio));
    return hfoc_mbedtls_bio_init(bio) ==
        HFOC_MBEDTLS_BIO_OK;
}

static struct hfoc_mbedtls_bio_policy make_policy(
    uint32_t max_send_bytes,
    uint32_t max_recv_bytes)
{
    struct hfoc_mbedtls_bio_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_MBEDTLS_BIO_VERSION;
    policy.max_send_bytes = max_send_bytes;
    policy.max_recv_bytes = max_recv_bytes;
    return policy;
}

static struct hfoc_mbedtls_bio_transport make_transport(
    struct sink *sink)
{
    struct hfoc_mbedtls_bio_transport transport;

    memset(&transport, 0, sizeof(transport));
    transport.send = sink_send;
    transport.recv = sink_recv;
    transport.ctx = sink;
    return transport;
}

static void test_strings_and_init(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_token token;

    CHECK(hfoc_mbedtls_bio_state_string(
              HFOC_MBEDTLS_BIO_STATE_FREE) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  HFOC_MBEDTLS_BIO_STATE_ATTACHED) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  HFOC_MBEDTLS_BIO_STATE_WANT_READ) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  HFOC_MBEDTLS_BIO_STATE_WANT_WRITE) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  HFOC_MBEDTLS_BIO_STATE_CLOSED) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  HFOC_MBEDTLS_BIO_STATE_FAILED) != NULL &&
              hfoc_mbedtls_bio_state_string(
                  (enum hfoc_mbedtls_bio_state)99U) == NULL,
          "BIO state strings cover known and unknown values");
    CHECK(hfoc_mbedtls_bio_transport_status_string(
              HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED) != NULL &&
              hfoc_mbedtls_bio_transport_status_string(
                  HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED) != NULL &&
              hfoc_mbedtls_bio_transport_status_string(
                  HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED) != NULL &&
              hfoc_mbedtls_bio_transport_status_string(
                  HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED) != NULL &&
              hfoc_mbedtls_bio_transport_status_string(
                  (enum hfoc_mbedtls_bio_transport_status)99U) == NULL,
          "BIO transport strings cover known and unknown values");
    CHECK(hfoc_mbedtls_bio_status_string(
              HFOC_MBEDTLS_BIO_OK) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_NULL) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_BAD_STATE) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_BAD_TOKEN) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_STALE_TOKEN) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_ALREADY_ATTACHED) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_INVALID_POLICY) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_INVALID_TRANSPORT) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  HFOC_MBEDTLS_BIO_NOT_ATTACHED) != NULL &&
              hfoc_mbedtls_bio_status_string(
                  (enum hfoc_mbedtls_bio_status)99U) == NULL,
          "BIO status strings cover known and unknown values");

    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    memset(&bio, 0, sizeof(bio));
    CHECK(!hfoc_mbedtls_bio_published(&bio) &&
              hfoc_mbedtls_bio_active_count(&bio) == 0U &&
              hfoc_mbedtls_bio_state(&bio, &token) ==
                  HFOC_MBEDTLS_BIO_STATE_FREE &&
              hfoc_mbedtls_bio_generation(&bio, &token) == 0U,
          "unpublished BIO accessors fail closed");
    CHECK(init_bio(&bio),
          "valid BIO init publishes fixed empty state");
    CHECK(hfoc_mbedtls_bio_init(&bio) ==
              HFOC_MBEDTLS_BIO_BAD_STATE,
          "published BIO rejects double init");
    CHECK(bio.published &&
              bio.version == HFOC_MBEDTLS_BIO_VERSION &&
              bio.active_count == 0U &&
              hfoc_mbedtls_bio_published(&bio),
          "published BIO exposes exact initial state");
}

static void test_attach_validation(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy policy =
        make_policy(64U, 64U);
    struct hfoc_mbedtls_bio_policy bad_policy;
    struct hfoc_mbedtls_bio_token token;
    struct hfoc_mbedtls_bio_token invalid_token;
    struct sink sink;
    struct hfoc_mbedtls_bio_transport transport;
    struct hfoc_mbedtls_bio_transport bad_transport;

    init_sink(&sink);
    transport = make_transport(&sink);

    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    CHECK(hfoc_mbedtls_bio_attach(
              NULL, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_NULL,
          "BIO attach rejects null gate");
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, NULL, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_NULL,
          "BIO attach rejects null token");
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, NULL, &transport) ==
              HFOC_MBEDTLS_BIO_NULL,
          "BIO attach rejects null policy");
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, NULL) ==
              HFOC_MBEDTLS_BIO_NULL,
          "BIO attach rejects null transport");

    memset(&bio, 0, sizeof(bio));
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_BAD_STATE,
          "BIO attach rejects unpublished gate");

    init_bio(&bio);
    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 99U;
    invalid_token.generation = 1U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &invalid_token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_BAD_TOKEN,
          "BIO attach rejects invalid token index");
    memset(&invalid_token, 0, sizeof(invalid_token));
    invalid_token.index = 0U;
    invalid_token.generation = 0U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &invalid_token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_BAD_TOKEN,
          "BIO attach rejects invalid token generation");

    bad_policy = policy;
    bad_policy.version = 2U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &bad_policy, &transport) ==
              HFOC_MBEDTLS_BIO_INVALID_POLICY &&
              bio.active_count == 0U,
          "BIO attach rejects bad policy version");
    bad_policy = policy;
    bad_policy.max_send_bytes = 0U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &bad_policy, &transport) ==
              HFOC_MBEDTLS_BIO_INVALID_POLICY,
          "BIO attach rejects zero send limit");
    bad_policy = policy;
    bad_policy.max_send_bytes = 4097U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &bad_policy, &transport) ==
              HFOC_MBEDTLS_BIO_INVALID_POLICY,
          "BIO attach rejects over-limit send bytes");
    bad_policy = policy;
    bad_policy.max_recv_bytes = 0U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &bad_policy, &transport) ==
              HFOC_MBEDTLS_BIO_INVALID_POLICY,
          "BIO attach rejects zero recv limit");
    bad_policy = policy;
    bad_policy.max_recv_bytes = 4097U;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &bad_policy, &transport) ==
              HFOC_MBEDTLS_BIO_INVALID_POLICY,
          "BIO attach rejects over-limit recv bytes");

    bad_transport = transport;
    bad_transport.send = NULL;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &bad_transport) ==
              HFOC_MBEDTLS_BIO_INVALID_TRANSPORT,
          "BIO attach rejects missing send callback");
    bad_transport = transport;
    bad_transport.recv = NULL;
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &bad_transport) ==
              HFOC_MBEDTLS_BIO_INVALID_TRANSPORT,
          "BIO attach rejects missing recv callback");

    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_OK &&
              bio.active_count == 1U &&
              bio.entries[token.index].active &&
              hfoc_mbedtls_bio_state(&bio, &token) ==
                  HFOC_MBEDTLS_BIO_STATE_ATTACHED,
          "BIO attach publishes one active slot");
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_ALREADY_ATTACHED &&
              bio.active_count == 1U,
          "BIO attach rejects duplicate attachment");
    CHECK(hfoc_mbedtls_bio_detach(&bio, &token) ==
              HFOC_MBEDTLS_BIO_OK &&
              bio.active_count == 0U &&
              !bio.entries[token.index].active &&
              hfoc_mbedtls_bio_state(&bio, &token) ==
                  HFOC_MBEDTLS_BIO_STATE_FREE,
          "BIO detach releases the active slot");
}

static void test_send_retained_and_short_progress(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy policy =
        make_policy(4096U, 4096U);
    struct hfoc_mbedtls_bio_token token;
    struct sink sink;
    struct hfoc_mbedtls_bio_transport transport;
    uint8_t bytes[8] = { 0x10, 0x20, 0x30, 0x40,
                         0x50, 0x60, 0x70, 0x80 };
    int result;

    init_bio(&bio);
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    init_sink(&sink);
    transport = make_transport(&sink);
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_OK,
          "send fixture attaches BIO slot");

    sink.write_mode = SINK_MODE_RETAIN;
    result = hfoc_mbedtls_bio_send_cb(&bio.entries[0], bytes, 8U);
    CHECK(result == MBEDTLS_ERR_SSL_WANT_WRITE &&
              sink.write_calls == 1U &&
              sink.accepted_length == 0U &&
              bio.entries[0].want_write_count == 1U &&
              hfoc_mbedtls_bio_state(&bio, &token) ==
                  HFOC_MBEDTLS_BIO_STATE_WANT_WRITE,
          "retained send maps to WANT_WRITE without consuming bytes");

    sink.write_mode = SINK_MODE_SHORT;
    sink.short_write = 1U;
    result = hfoc_mbedtls_bio_send_cb(&bio.entries[0], bytes, 8U);
    CHECK(result == 1 &&
              sink.accepted_length == 1U &&
              sink.accepted_log[0] == bytes[0] &&
              bio.entries[0].send_bytes == 1U &&
              bio.entries[0].short_send_count == 1U,
          "short accepted send returns exact positive progress");

    sink.write_mode = SINK_MODE_FULL;
    result = hfoc_mbedtls_bio_send_cb(
        &bio.entries[0], bytes + 1, 7U);
    CHECK(result == 7 &&
              sink.accepted_length == 8U &&
              memcmp(sink.accepted_log, bytes, 8U) == 0 &&
              bio.entries[0].send_bytes == 8U,
          "remainder send completes without skipping or duplicating bytes");
}

static void test_recv_retained_and_short_progress(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy policy =
        make_policy(4096U, 4096U);
    struct hfoc_mbedtls_bio_token token;
    struct sink sink;
    struct hfoc_mbedtls_bio_transport transport;
    uint8_t output[8] = { 0 };
    int result;

    init_bio(&bio);
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    init_sink(&sink);
    transport = make_transport(&sink);
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_OK,
          "recv fixture attaches BIO slot");

    sink.read_mode = SINK_MODE_RETAIN;
    result = hfoc_mbedtls_bio_recv_cb(&bio.entries[0], output, 8U);
    CHECK(result == MBEDTLS_ERR_SSL_WANT_READ &&
              sink.read_calls == 1U &&
              output[0] == 0U &&
              bio.entries[0].want_read_count == 1U &&
              hfoc_mbedtls_bio_state(&bio, &token) ==
                  HFOC_MBEDTLS_BIO_STATE_WANT_READ,
          "retained recv maps to WANT_READ without writing output");

    sink.inbound[0] = 0xA1;
    sink.inbound[1] = 0xB2;
    sink.inbound_length = 2U;
    sink.read_mode = SINK_MODE_SHORT;
    sink.short_read = 1U;
    result = hfoc_mbedtls_bio_recv_cb(&bio.entries[0], output, 8U);
    CHECK(result == 1 &&
              output[0] == 0xA1 &&
              sink.inbound_offset == 1U &&
              bio.entries[0].recv_bytes == 1U &&
              bio.entries[0].short_recv_count == 1U,
          "short accepted recv returns exact positive progress");

    result = hfoc_mbedtls_bio_recv_cb(
        &bio.entries[0], output + 1, 7U);
    CHECK(result == 1 &&
              output[1] == 0xB2 &&
              sink.inbound_offset == 2U &&
              bio.entries[0].recv_bytes == 2U,
          "remainder recv returns the retained byte exactly once");
}

static void test_send_recv_reject_close(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy policy =
        make_policy(64U, 64U);
    struct hfoc_mbedtls_bio_token token;
    struct sink sink;
    struct hfoc_mbedtls_bio_transport transport;
    uint8_t bytes[4] = { 1, 2, 3, 4 };
    uint8_t output[4] = { 0 };
    int result;

    init_bio(&bio);
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    init_sink(&sink);
    transport = make_transport(&sink);
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_OK,
          "reject/close fixture attaches BIO slot");

    CHECK(hfoc_mbedtls_bio_send_cb(NULL, bytes, 4U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "send callback rejects null context");
    CHECK(hfoc_mbedtls_bio_send_cb(
              &bio.entries[0], NULL, 4U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "send callback rejects null buffer");
    CHECK(hfoc_mbedtls_bio_send_cb(
              &bio.entries[0], bytes, 0U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "send callback rejects zero length");
    CHECK(hfoc_mbedtls_bio_send_cb(
              &bio.entries[0], bytes, 65U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "send callback rejects policy overflow");
    CHECK(hfoc_mbedtls_bio_recv_cb(NULL, output, 4U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "recv callback rejects null context");
    CHECK(hfoc_mbedtls_bio_recv_cb(
              &bio.entries[0], NULL, 4U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "recv callback rejects null buffer");
    CHECK(hfoc_mbedtls_bio_recv_cb(
              &bio.entries[0], output, 0U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "recv callback rejects zero capacity");
    CHECK(hfoc_mbedtls_bio_recv_cb(
              &bio.entries[0], output, 65U) ==
              MBEDTLS_ERR_SSL_BAD_INPUT_DATA,
          "recv callback rejects policy overflow");

    sink.write_mode = SINK_MODE_CLOSED;
    result = hfoc_mbedtls_bio_send_cb(
        &bio.entries[0], bytes, 4U);
    CHECK(result != 0 &&
              result != MBEDTLS_ERR_SSL_WANT_WRITE &&
              bio.entries[0].closed_count == 1U,
          "closed send maps to stable negative error");

    sink.write_mode = SINK_MODE_REJECT;
    result = hfoc_mbedtls_bio_send_cb(
        &bio.entries[0], bytes, 4U);
    CHECK(result != 0 &&
              result != MBEDTLS_ERR_SSL_WANT_WRITE &&
              bio.entries[0].rejected_count == 1U,
          "rejected send maps to stable negative error");

    sink.read_mode = SINK_MODE_CLOSED;
    result = hfoc_mbedtls_bio_recv_cb(
        &bio.entries[0], output, 4U);
    CHECK(result == 0 &&
              bio.entries[0].closed_count == 2U,
          "closed recv maps to mbedTLS EOF zero");

    sink.read_mode = SINK_MODE_REJECT;
    result = hfoc_mbedtls_bio_recv_cb(
        &bio.entries[0], output, 4U);
    CHECK(result != 0 &&
              result != MBEDTLS_ERR_SSL_WANT_READ &&
              bio.entries[0].rejected_count == 2U,
          "rejected recv maps to stable negative error");
}

static void test_signature_compatibility(void)
{
    mbedtls_ssl_send_t *send_fn = hfoc_mbedtls_bio_send_cb;
    mbedtls_ssl_recv_t *recv_fn = hfoc_mbedtls_bio_recv_cb;

    CHECK(send_fn == hfoc_mbedtls_bio_send_cb &&
              recv_fn == hfoc_mbedtls_bio_recv_cb,
          "BIO callbacks match mbedTLS send/recv signatures");
}

static void test_detach_zeroization(void)
{
    struct hfoc_mbedtls_bio bio;
    struct hfoc_mbedtls_bio_policy policy =
        make_policy(64U, 64U);
    struct hfoc_mbedtls_bio_token token;
    struct sink sink;
    struct hfoc_mbedtls_bio_transport transport;

    init_bio(&bio);
    memset(&token, 0, sizeof(token));
    token.index = 0U;
    token.generation = 1U;
    init_sink(&sink);
    transport = make_transport(&sink);
    CHECK(hfoc_mbedtls_bio_attach(
              &bio, &token, &policy, &transport) ==
              HFOC_MBEDTLS_BIO_OK,
          "zeroization fixture attaches BIO slot");
    CHECK(hfoc_mbedtls_bio_send_cb(
              &bio.entries[0], (const unsigned char *)"AB", 2U) == 2,
          "zeroization fixture sends before detach");

    CHECK(hfoc_mbedtls_bio_detach(&bio, &token) ==
              HFOC_MBEDTLS_BIO_OK,
          "zeroization fixture detaches BIO slot");
    CHECK(bio.active_count == 0U &&
              !bio.entries[0].active &&
              bio.entries[0].generation == 0U &&
              bio.entries[0].send_bytes == 0U &&
              bio.entries[0].send_calls == 0U &&
              bio.entries[0].recv_calls == 0U &&
              bio.entries[0].want_read_count == 0U &&
              bio.entries[0].want_write_count == 0U &&
              bio.entries[0].short_send_count == 0U &&
              bio.entries[0].short_recv_count == 0U &&
              bio.entries[0].rejected_count == 0U &&
              bio.entries[0].closed_count == 0U,
          "BIO detach zeroizes slot ownership and counters");
}

int main(void)
{
    test_strings_and_init();
    test_attach_validation();
    test_send_retained_and_short_progress();
    test_recv_retained_and_short_progress();
    test_send_recv_reject_close();
    test_signature_compatibility();
    test_detach_zeroization();

    if (failures != 0) {
        fprintf(
            stderr,
            "mbedTLS BIO transport host tests: %d FAILURES\n",
            failures);
        return 1;
    }
    puts("mbedTLS BIO transport host tests: ALL OK");
    return 0;
}
