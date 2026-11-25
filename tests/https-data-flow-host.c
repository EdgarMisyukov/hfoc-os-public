#include "https_connection.h"
#include "https_data_flow.h"

#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_mbedtls_alloc.h"
#include "hfoc_tls_handshake.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HTTPS_CONNECTION_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

u32_t sys_now(void)
{
    return 1000U;
}

struct tls_sink {
    uint8_t data[HTTPS_CONNECTION_RESPONSE_CAPACITY];
    uint32_t length;
    uint32_t calls;
    size_t accepted_per_call;
    enum https_data_flow_tls_status outcome;
};

struct fixture {
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_tls_handshake handshake;
    struct http_parser parser;
    struct https_connection_composer composer;
    struct https_connection connection;
    struct https_data_flow flow;
    struct tls_sink sink;
    uint8_t output[HTTPS_CONNECTION_RESPONSE_CAPACITY];
};

static const uint8_t request_bytes[] =
    "GET / HTTP/1.1\r\nHost: hfoc.test\r\n\r\n";
static const uint32_t request_length =
    (uint32_t)(sizeof(request_bytes) - 1U);

static bool initial_counts(const struct fixture *fixture)
{
    return hfoc_lwip_tcp_pool_free_count(&fixture->pool) ==
               HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
           hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 0U &&
           hfoc_lwip_tcp_pool_published_count(&fixture->pool) == 0U &&
           hfoc_tls_handshake_active_count(&fixture->handshake) == 0U &&
           http_parser_active_count(&fixture->parser) == 0U;
}

static bool composed_counts(const struct fixture *fixture)
{
    return hfoc_lwip_tcp_pool_free_count(&fixture->pool) ==
               HFOC_LWIP_TCP_POOL_SLOT_COUNT - 1U &&
           hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 1U &&
           hfoc_lwip_tcp_pool_published_count(&fixture->pool) == 0U &&
           hfoc_tls_handshake_active_count(&fixture->handshake) == 1U &&
           http_parser_active_count(&fixture->parser) == 1U;
}

static enum https_data_flow_tls_status tls_send_cb(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    struct tls_sink *sink = context;
    size_t accepted;

    if (sink == NULL || bytes == NULL || bytes_accepted == NULL) {
        return HTTPS_DATA_FLOW_SEND_REJECTED;
    }
    *bytes_accepted = 0U;
    ++sink->calls;
    if (sink->outcome != HTTPS_DATA_FLOW_SEND_ACCEPTED) {
        return sink->outcome;
    }
    accepted = sink->accepted_per_call;
    if (accepted == 0U) {
        return HTTPS_DATA_FLOW_SEND_REJECTED;
    }
    if (accepted > length) {
        accepted = length;
    }
    if (sink->length + (uint32_t)accepted >
        HTTPS_CONNECTION_RESPONSE_CAPACITY) {
        return HTTPS_DATA_FLOW_SEND_REJECTED;
    }
    memcpy(sink->data + sink->length, bytes, accepted);
    sink->length += (uint32_t)accepted;
    *bytes_accepted = accepted;
    return HTTPS_DATA_FLOW_SEND_ACCEPTED;
}

static void reset_fixture(struct fixture *fixture)
{
    struct https_connection_config config;

    memset(fixture, 0, sizeof(*fixture));
    hfoc_lwip_alloc_shutdown();
    hfoc_mbedtls_alloc_shutdown();
    CHECK(hfoc_lwip_alloc_init(),
          "data-flow fixture starts fixed lwIP allocator");
    CHECK(hfoc_mbedtls_alloc_init(),
          "data-flow fixture starts mbedTLS allocator");
    CHECK(hfoc_lwip_tcp_pool_init(&fixture->pool) ==
              HFOC_LWIP_TCP_POOL_OK,
          "data-flow fixture starts four-slot TCP pool");
    CHECK(hfoc_tls_handshake_init(&fixture->handshake) ==
              HFOC_TLS_HANDSHAKE_OK,
          "data-flow fixture starts TLS handshake gate");
    CHECK(http_parser_init(&fixture->parser) == HTTP_PARSER_OK,
          "data-flow fixture starts HTTP parser gate");
    CHECK(https_connection_composer_init(
              &fixture->composer,
              &fixture->pool,
              &fixture->handshake,
              &fixture->parser) == HTTPS_CONNECTION_OK,
          "data-flow fixture publishes composer");

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_CONNECTION_VERSION;
    config.response_output = fixture->output;
    config.response_capacity = HTTPS_CONNECTION_RESPONSE_CAPACITY;
    CHECK(https_connection_compose(
              &fixture->composer,
              &config,
              &fixture->connection) == HTTPS_CONNECTION_OK,
          "data-flow fixture composes one HTTPS connection");
    CHECK(composed_counts(fixture),
          "data-flow fixture holds exactly one of each resource");
}

static struct https_data_flow_config make_flow_config(
    const struct fixture *fixture)
{
    struct https_data_flow_config config;

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_DATA_FLOW_VERSION;
    config.parser = (struct http_parser *)(uintptr_t)&fixture->parser;
    config.connection =
        (struct https_connection *)(uintptr_t)&fixture->connection;
    config.tls_send = tls_send_cb;
    config.tls_send_context =
        (void *)(uintptr_t)&fixture->sink;
    return config;
}

static enum http_parser_state parser_state(
    const struct fixture *fixture)
{
    return http_parser_state(
        &fixture->parser,
        https_connection_parser_token(&fixture->connection));
}

static void init_flow(struct fixture *fixture)
{
    struct https_data_flow_config config = make_flow_config(fixture);

    CHECK(https_data_flow_init(&fixture->flow, &config) ==
              HTTPS_DATA_FLOW_OK &&
              https_data_flow_published(&fixture->flow) &&
              https_data_flow_generation(&fixture->flow) ==
                  https_connection_generation(&fixture->connection),
          "valid data-flow adapter publishes current generation");
}

static void fill_response(
    struct fixture *fixture,
    uint32_t length)
{
    uint32_t index;

    for (index = 0U; index < length; ++index) {
        fixture->output[index] = (uint8_t)('A' + (index % 26U));
    }
    CHECK(https_connection_response_advance(
              &fixture->connection.response, length) ==
              HTTPS_CONNECTION_OK &&
              fixture->connection.response.offset == length,
          "response fixture advances committed response length");
}

static void test_status_strings(void)
{
    static const char *flow_strings[] = {
        "HTTPS_DATA_FLOW_OK",
        "HTTPS_DATA_FLOW_NULL",
        "HTTPS_DATA_FLOW_BAD_STATE",
        "HTTPS_DATA_FLOW_BAD_VERSION",
        "HTTPS_DATA_FLOW_NOT_PUBLISHED",
        "HTTPS_DATA_FLOW_STALE_GENERATION",
        "HTTPS_DATA_FLOW_INVALID_PROGRESS",
        "HTTPS_DATA_FLOW_PARSER_TERMINAL",
        "HTTPS_DATA_FLOW_PARSER_REJECTED",
        "HTTPS_DATA_FLOW_TLS_RETAINED",
        "HTTPS_DATA_FLOW_TLS_REJECTED",
        "HTTPS_DATA_FLOW_TLS_CLOSED",
    };
    static const char *tls_strings[] = {
        "HTTPS_DATA_FLOW_SEND_ACCEPTED",
        "HTTPS_DATA_FLOW_SEND_RETAINED",
        "HTTPS_DATA_FLOW_SEND_CLOSED",
        "HTTPS_DATA_FLOW_SEND_REJECTED",
    };
    size_t index;

    for (index = 0U;
         index < sizeof(flow_strings) / sizeof(flow_strings[0]);
         ++index) {
        const char *string = https_data_flow_status_string(
            (enum https_data_flow_status)index);

        CHECK(string != NULL &&
                  strcmp(string, flow_strings[index]) == 0,
              "HTTPS data-flow status string is exact");
    }
    CHECK(https_data_flow_status_string(
              (enum https_data_flow_status)99U) == NULL,
          "HTTPS data-flow undefined status is rejected");

    for (index = 0U;
         index < sizeof(tls_strings) / sizeof(tls_strings[0]);
         ++index) {
        const char *string = https_data_flow_tls_status_string(
            (enum https_data_flow_tls_status)index);

        CHECK(string != NULL &&
                  strcmp(string, tls_strings[index]) == 0,
              "HTTPS data-flow TLS status string is exact");
    }
    CHECK(https_data_flow_tls_status_string(
              (enum https_data_flow_tls_status)99U) == NULL,
          "HTTPS data-flow undefined TLS status is rejected");
}

static void test_null_and_init(struct fixture *fixture)
{
    struct https_data_flow_config config = make_flow_config(fixture);
    struct https_data_flow_config bad = config;
    struct https_data_flow flow;

    memset(&flow, 0, sizeof(flow));
    CHECK(https_data_flow_init(NULL, &config) ==
              HTTPS_DATA_FLOW_NULL,
          "data-flow init rejects null flow");
    CHECK(https_data_flow_init(&flow, NULL) ==
              HTTPS_DATA_FLOW_NULL,
          "data-flow init rejects null config");
    CHECK(https_data_flow_rx(NULL, request_bytes, 1U, NULL) ==
              HTTPS_DATA_FLOW_NULL,
          "data-flow rx rejects null flow");
    CHECK(https_data_flow_tx(NULL, 1U, NULL) ==
              HTTPS_DATA_FLOW_NULL,
          "data-flow tx rejects null flow");
    CHECK(https_data_flow_release(NULL) == HTTPS_DATA_FLOW_NULL,
          "data-flow release rejects null flow");
    CHECK(!https_data_flow_published(NULL) &&
              https_data_flow_generation(NULL) == 0U &&
              https_data_flow_request_bytes(NULL) == 0U &&
              https_data_flow_response_sent(NULL) == 0U &&
              https_data_flow_retained_count(NULL) == 0U &&
              https_data_flow_rejected_count(NULL) == 0U &&
              https_data_flow_closed_count(NULL) == 0U,
          "data-flow accessors reject null flow");

    bad.parser = NULL;
    CHECK(https_data_flow_init(&flow, &bad) == HTTPS_DATA_FLOW_NULL,
          "data-flow init rejects null parser");
    bad = make_flow_config(fixture);
    bad.connection = NULL;
    CHECK(https_data_flow_init(&flow, &bad) == HTTPS_DATA_FLOW_NULL,
          "data-flow init rejects null connection");
    bad = make_flow_config(fixture);
    bad.tls_send = NULL;
    CHECK(https_data_flow_init(&flow, &bad) == HTTPS_DATA_FLOW_NULL,
          "data-flow init rejects null TLS send callback");
    bad = make_flow_config(fixture);
    bad.version = 99U;
    CHECK(https_data_flow_init(&flow, &bad) ==
              HTTPS_DATA_FLOW_BAD_VERSION,
          "data-flow init rejects bad version");

    init_flow(fixture);
    CHECK(https_data_flow_init(
              &fixture->flow, &config) == HTTPS_DATA_FLOW_BAD_STATE,
          "data-flow init rejects already published flow");
    CHECK(https_data_flow_release(&fixture->flow) ==
              HTTPS_DATA_FLOW_OK &&
              !https_data_flow_published(&fixture->flow) &&
              https_data_flow_generation(&fixture->flow) == 0U,
          "data-flow release clears flow publication");
    CHECK(https_data_flow_release(&fixture->flow) ==
              HTTPS_DATA_FLOW_OK,
          "data-flow release is idempotent");
    CHECK(composed_counts(fixture),
          "data-flow init/release does not change connection counts");
}

static void test_incremental_request(struct fixture *fixture)
{
    uint32_t offset;
    uint32_t consumed = 0U;
    uint32_t total = 0U;

    init_flow(fixture);
    for (offset = 0U; offset < request_length; ++offset) {
        CHECK(https_data_flow_rx(
                  &fixture->flow,
                  request_bytes + offset,
                  1U,
                  &consumed) == HTTPS_DATA_FLOW_OK &&
                  consumed == 1U,
              "split-at-every-byte request advances exactly one byte");
        total += consumed;
    }
    CHECK(total == request_length &&
              https_data_flow_request_bytes(&fixture->flow) ==
                  request_length &&
              parser_state(fixture) == HTTP_PARSER_STATE_COMPLETE,
          "split-at-every-byte request reaches parser complete");
}

static void test_one_chunk_and_terminal(struct fixture *fixture)
{
    uint32_t consumed = 0U;
    uint32_t extra_consumed = 0U;
    uint8_t extra = 'X';

    init_flow(fixture);
    CHECK(https_data_flow_rx(
              &fixture->flow,
              request_bytes,
              request_length,
              &consumed) == HTTPS_DATA_FLOW_OK &&
              consumed == request_length &&
              parser_state(fixture) == HTTP_PARSER_STATE_COMPLETE,
          "one-chunk authenticated request reaches parser complete");
    CHECK(https_data_flow_rx(
              &fixture->flow,
              &extra,
              1U,
              &extra_consumed) == HTTPS_DATA_FLOW_PARSER_TERMINAL &&
              extra_consumed == 0U &&
              parser_state(fixture) == HTTP_PARSER_STATE_COMPLETE,
          "extra plaintext after complete request is rejected as terminal");
}

static void test_malformed_rejection(struct fixture *fixture)
{
    static const uint8_t malformed[] = "BAD\n";
    static const uint32_t malformed_length =
        (uint32_t)(sizeof(malformed) - 1U);
    uint32_t consumed = 0U;
    uint32_t retry_consumed = 0U;
    uint8_t retry = 'X';

    init_flow(fixture);
    CHECK(https_data_flow_rx(
              &fixture->flow,
              malformed,
              malformed_length,
              &consumed) == HTTPS_DATA_FLOW_PARSER_REJECTED &&
              consumed == malformed_length,
          "malformed authenticated plaintext rejects parser");
    CHECK(https_data_flow_rx(
              &fixture->flow,
              &retry,
              1U,
              &retry_consumed) == HTTPS_DATA_FLOW_PARSER_REJECTED &&
              retry_consumed == 0U,
          "rejected parser does not consume retry bytes");
}

static void test_short_progress_tx(struct fixture *fixture)
{
    static const uint32_t response_length = 39U;
    uint32_t bytes_accepted;
    uint32_t total = 0U;

    init_flow(fixture);
    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_ACCEPTED;
    fixture->sink.accepted_per_call = 3U;
    fill_response(fixture, response_length);

    while (total < response_length) {
        CHECK(https_data_flow_tx(
                  &fixture->flow,
                  response_length,
                  &bytes_accepted) == HTTPS_DATA_FLOW_OK &&
                  bytes_accepted > 0U &&
                  bytes_accepted <= 3U,
              "TLS short progress returns positive bounded accepted bytes");
        total += bytes_accepted;
    }
    CHECK(total == response_length &&
              https_data_flow_response_sent(&fixture->flow) ==
                  response_length &&
              fixture->sink.length == response_length &&
              memcmp(fixture->sink.data,
                     fixture->output,
                     response_length) == 0,
          "TLS short progress sends exact prefix without duplication");
}

static void test_retained_tx(struct fixture *fixture)
{
    static const uint32_t response_length = 10U;
    uint32_t bytes_accepted = 0U;

    init_flow(fixture);
    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_RETAINED;
    fixture->sink.accepted_per_call = 0U;
    fill_response(fixture, response_length);
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_TLS_RETAINED &&
              bytes_accepted == 0U &&
              https_data_flow_response_sent(&fixture->flow) == 0U &&
              https_data_flow_retained_count(&fixture->flow) == 1U &&
              fixture->sink.length == 0U,
          "TLS retained backpressure leaves response_sent unchanged");

    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_ACCEPTED;
    fixture->sink.accepted_per_call = 4U;
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_OK &&
              bytes_accepted == 4U &&
              https_data_flow_response_sent(&fixture->flow) == 4U,
          "TLS backpressure retry resumes at exact offset");
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_OK &&
              bytes_accepted == 4U &&
              https_data_flow_response_sent(&fixture->flow) == 8U,
          "TLS backpressure retry continues without skip");
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_OK &&
              bytes_accepted == 2U &&
              https_data_flow_response_sent(&fixture->flow) ==
                  response_length &&
              fixture->sink.length == response_length &&
              memcmp(fixture->sink.data,
                     fixture->output,
                     response_length) == 0,
          "TLS backpressure retry completes exact response");
}

static void test_rejected_and_closed_tx(struct fixture *fixture)
{
    static const uint32_t response_length = 8U;
    uint32_t bytes_accepted = 0U;

    init_flow(fixture);
    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_REJECTED;
    fixture->sink.accepted_per_call = 0U;
    fill_response(fixture, response_length);
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_TLS_REJECTED &&
              bytes_accepted == 0U &&
              https_data_flow_response_sent(&fixture->flow) == 0U &&
              https_data_flow_rejected_count(&fixture->flow) == 1U &&
              fixture->sink.length == 0U,
          "TLS rejected response leaves response_sent unchanged");

    CHECK(https_data_flow_release(&fixture->flow) ==
              HTTPS_DATA_FLOW_OK &&
              https_connection_release(
                  &fixture->composer,
                  &fixture->connection) == HTTPS_CONNECTION_OK &&
              initial_counts(fixture),
          "rejected fixture releases flow and connection");
    reset_fixture(fixture);
    init_flow(fixture);
    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_CLOSED;
    fixture->sink.accepted_per_call = 0U;
    fill_response(fixture, response_length);
    CHECK(https_data_flow_tx(
              &fixture->flow,
              response_length,
              &bytes_accepted) == HTTPS_DATA_FLOW_TLS_CLOSED &&
              bytes_accepted == 0U &&
              https_data_flow_response_sent(&fixture->flow) == 0U &&
              https_data_flow_closed_count(&fixture->flow) == 1U &&
              fixture->sink.length == 0U,
          "TLS closed response leaves response_sent unchanged");
}

static void test_invalid_response_length(struct fixture *fixture)
{
    uint32_t bytes_accepted = 0U;

    init_flow(fixture);
    fixture->sink.outcome = HTTPS_DATA_FLOW_SEND_ACCEPTED;
    fixture->sink.accepted_per_call = 1U;
    fill_response(fixture, 5U);
    CHECK(https_data_flow_tx(
              &fixture->flow,
              6U,
              &bytes_accepted) == HTTPS_DATA_FLOW_INVALID_PROGRESS &&
              bytes_accepted == 0U &&
              https_data_flow_response_sent(&fixture->flow) == 0U &&
              fixture->sink.calls == 0U,
          "invalid response length fails before TLS sink call");
}

static void test_stale_generation(struct fixture *fixture)
{
    struct https_connection_config config;
    uint32_t consumed = 0U;
    uint32_t bytes_accepted = 0U;
    uint32_t old_generation;

    init_flow(fixture);
    old_generation = https_connection_generation(&fixture->connection);
    fill_response(fixture, 4U);
    CHECK(https_connection_release(
              &fixture->composer,
              &fixture->connection) == HTTPS_CONNECTION_OK,
          "stale fixture releases old connection");

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_CONNECTION_VERSION;
    config.response_output = fixture->output;
    config.response_capacity = HTTPS_CONNECTION_RESPONSE_CAPACITY;
    CHECK(https_connection_compose(
              &fixture->composer,
              &config,
              &fixture->connection) == HTTPS_CONNECTION_OK &&
              https_connection_generation(&fixture->connection) !=
                  old_generation,
          "stale fixture composes a new connection generation");

    CHECK(https_data_flow_rx(
              &fixture->flow,
              request_bytes,
              request_length,
              &consumed) == HTTPS_DATA_FLOW_STALE_GENERATION &&
              consumed == 0U &&
              parser_state(fixture) == HTTP_PARSER_STATE_EMPTY,
          "stale connection generation rejects old flow without parser mutation");
    CHECK(https_data_flow_tx(
              &fixture->flow,
              4U,
              &bytes_accepted) == HTTPS_DATA_FLOW_STALE_GENERATION &&
              bytes_accepted == 0U &&
              fixture->connection.response.offset == 0U &&
              fixture->sink.length == 0U,
          "stale connection generation rejects old flow without response mutation");
}

int main(void)
{
    struct fixture fixture;

    reset_fixture(&fixture);
    test_status_strings();
    test_null_and_init(&fixture);
    test_incremental_request(&fixture);

    reset_fixture(&fixture);
    test_one_chunk_and_terminal(&fixture);

    reset_fixture(&fixture);
    test_malformed_rejection(&fixture);

    reset_fixture(&fixture);
    test_short_progress_tx(&fixture);

    reset_fixture(&fixture);
    test_retained_tx(&fixture);

    reset_fixture(&fixture);
    test_rejected_and_closed_tx(&fixture);

    reset_fixture(&fixture);
    test_invalid_response_length(&fixture);

    reset_fixture(&fixture);
    test_stale_generation(&fixture);

    if (failures != 0) {
        fprintf(stderr,
                "HTTPS TLS-to-HTTP data flow host failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTPS TLS-to-HTTP data flow host test: ALL OK\n");
    return 0;
}

#endif
