#include "https_connection.h"

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

struct fixture {
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_tls_handshake handshake;
    struct http_parser parser;
    struct https_connection_composer composer;
    uint8_t output[HTTPS_CONNECTION_RESPONSE_CAPACITY];
};

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

static void reset_fixture(struct fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    hfoc_lwip_alloc_shutdown();
    hfoc_mbedtls_alloc_shutdown();
    CHECK(hfoc_lwip_alloc_init(),
          "composition fixture starts fixed lwIP allocator");
    CHECK(hfoc_mbedtls_alloc_init(),
          "composition fixture starts mbedTLS allocator");
    CHECK(hfoc_lwip_tcp_pool_init(&fixture->pool) ==
              HFOC_LWIP_TCP_POOL_OK,
          "composition fixture starts four-slot TCP pool");
    CHECK(hfoc_tls_handshake_init(&fixture->handshake) ==
              HFOC_TLS_HANDSHAKE_OK,
          "composition fixture starts TLS handshake gate");
    CHECK(http_parser_init(&fixture->parser) == HTTP_PARSER_OK,
          "composition fixture starts HTTP parser gate");
    CHECK(https_connection_composer_init(
              &fixture->composer,
              &fixture->pool,
              &fixture->handshake,
              &fixture->parser) == HTTPS_CONNECTION_OK,
          "composition fixture publishes composer");
    CHECK(initial_counts(fixture),
          "composition fixture starts with exact initial counts");
}

static struct https_connection_config make_config(
    const struct fixture *fixture,
    uint32_t capacity)
{
    struct https_connection_config config;

    memset(&config, 0, sizeof(config));
    config.version = HTTPS_CONNECTION_VERSION;
    config.response_output =
        (uint8_t *)(uintptr_t)fixture->output;
    config.response_capacity = capacity;
    return config;
}

static struct https_connection zero_connection(void)
{
    struct https_connection connection;

    memset(&connection, 0, sizeof(connection));
    return connection;
}

static bool connection_unchanged(
    const struct https_connection *connection,
    const unsigned char *original)
{
    return memcmp(connection, original, sizeof(*connection)) == 0;
}

static bool shared_tokens(
    const struct https_connection *connection)
{
    const struct hfoc_lwip_tcp_pool_token *tcp =
        https_connection_tcp_token(connection);
    const struct hfoc_tls_handshake_token *tls =
        https_connection_tls_token(connection);
    const struct http_parser_token *parser =
        https_connection_parser_token(connection);

    return tcp != NULL &&
           tls != NULL &&
           parser != NULL &&
           tcp->index == tls->index &&
           tcp->index == parser->index &&
           tcp->generation == tls->generation &&
           tcp->generation == parser->generation &&
           tcp->generation == https_connection_generation(connection);
}

static void test_status_strings(void)
{
    static const char *strings[] = {
        "HTTPS_CONNECTION_OK",
        "HTTPS_CONNECTION_NULL",
        "HTTPS_CONNECTION_BAD_STATE",
        "HTTPS_CONNECTION_BAD_VERSION",
        "HTTPS_CONNECTION_ALREADY_COMPOSED",
        "HTTPS_CONNECTION_NOT_COMPOSED",
        "HTTPS_CONNECTION_TCP_RESERVE_FAILED",
        "HTTPS_CONNECTION_TLS_RESERVE_FAILED",
        "HTTPS_CONNECTION_HTTP_RESERVE_FAILED",
        "HTTPS_CONNECTION_RESPONSE_INIT_FAILED",
        "HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY",
        "HTTPS_CONNECTION_RESPONSE_OVERFLOW",
        "HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE",
        "HTTPS_CONNECTION_RELEASE_FAILED",
        "HTTPS_CONNECTION_CAPACITY_FULL",
    };
    size_t index;

    for (index = 0U; index < sizeof(strings) / sizeof(strings[0]);
         ++index) {
        const char *string = https_connection_status_string(
            (enum https_connection_status)index);

        CHECK(string != NULL &&
                  strcmp(string, strings[index]) == 0,
              "HTTPS connection status string is exact");
    }
    CHECK(https_connection_status_string(
              (enum https_connection_status)99U) == NULL,
          "HTTPS connection undefined status is rejected");
}

static void test_null_and_publication(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct https_connection connection;

    CHECK(https_connection_composer_init(NULL, NULL, NULL, NULL) ==
              HTTPS_CONNECTION_NULL,
          "composer init rejects null composer");
    CHECK(https_connection_composer_init(
              &fixture->composer, NULL, NULL, NULL) ==
              HTTPS_CONNECTION_NULL,
          "composer init rejects null TCP pool");
    CHECK(https_connection_composer_init(
              &fixture->composer,
              &fixture->pool,
              NULL,
              NULL) == HTTPS_CONNECTION_NULL,
          "composer init rejects null TLS handshake gate");
    CHECK(https_connection_composer_init(
              &fixture->composer,
              &fixture->pool,
              &fixture->handshake,
              NULL) == HTTPS_CONNECTION_NULL,
          "composer init rejects null HTTP parser gate");
    CHECK(!https_connection_composer_published(NULL),
          "composer published accessor rejects null composer");
    CHECK(https_connection_composer_published(&fixture->composer),
          "composer published accessor accepts published composer");

    CHECK(https_connection_compose(
              NULL, &config, &connection) == HTTPS_CONNECTION_NULL,
          "compose rejects null composer");
    CHECK(https_connection_compose(
              &fixture->composer, NULL, &connection) ==
              HTTPS_CONNECTION_NULL,
          "compose rejects null config");
    CHECK(https_connection_compose(
              &fixture->composer, &config, NULL) ==
              HTTPS_CONNECTION_NULL,
          "compose rejects null connection");
    CHECK(https_connection_release(
              NULL, &connection) == HTTPS_CONNECTION_NULL,
          "release rejects null composer");
    CHECK(https_connection_release(
              &fixture->composer, NULL) == HTTPS_CONNECTION_NULL,
          "release rejects null connection");

    connection = zero_connection();
    CHECK(!https_connection_published(NULL),
          "connection published accessor rejects null connection");
    CHECK(!https_connection_published(&connection),
          "zero connection is not published");
    CHECK(https_connection_generation(NULL) == 0U,
          "connection generation accessor rejects null connection");
    CHECK(https_connection_tcp_token(NULL) == NULL &&
              https_connection_tls_token(NULL) == NULL &&
              https_connection_parser_token(NULL) == NULL,
          "token accessors reject null connection");
    CHECK(https_connection_tcp_token(&connection) == NULL &&
              https_connection_tls_token(&connection) == NULL &&
              https_connection_parser_token(&connection) == NULL,
          "unpublished connection exposes no tokens");

    CHECK(https_connection_response_init(NULL, 1U, fixture->output,
              HTTPS_CONNECTION_RESPONSE_CAPACITY) ==
              HTTPS_CONNECTION_NULL,
          "response init rejects null response");
    CHECK(https_connection_response_advance(NULL, 1U) ==
              HTTPS_CONNECTION_NULL,
          "response advance rejects null response");
    CHECK(https_connection_response_clear(NULL) ==
              HTTPS_CONNECTION_NULL,
          "response clear rejects null response");
    CHECK(initial_counts(fixture),
          "null handling leaves initial counts unchanged");
}

static void test_capacity_accessors(struct fixture *fixture)
{
    struct https_connection_composer unpublished;

    memset(&unpublished, 0, sizeof(unpublished));
    CHECK(https_connection_capacity(NULL) == 0U,
          "capacity accessor rejects null composer");
    CHECK(https_connection_available(NULL) == 0U,
          "available accessor rejects null composer");
    CHECK(!https_connection_capacity_available(NULL),
          "capacity available accessor rejects null composer");
    CHECK(https_connection_capacity(&unpublished) == 0U,
          "capacity accessor rejects unpublished composer");
    CHECK(https_connection_available(&unpublished) == 0U,
          "available accessor rejects unpublished composer");
    CHECK(!https_connection_capacity_available(&unpublished),
          "capacity available accessor rejects unpublished composer");
    CHECK(https_connection_capacity(&fixture->composer) ==
              HTTPS_CONNECTION_CAPACITY,
          "published composer exposes fixed four-slot capacity");
    CHECK(https_connection_available(&fixture->composer) ==
              HTTPS_CONNECTION_CAPACITY,
          "published composer starts with four available slots");
    CHECK(https_connection_capacity_available(&fixture->composer),
          "published composer starts with admission available");
}

static void test_success_compose_and_release(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct https_connection connection = zero_connection();

    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_OK,
          "valid HTTPS connection composes");
    CHECK(https_connection_published(&connection) &&
              connection.version == HTTPS_CONNECTION_VERSION &&
              shared_tokens(&connection) &&
              connection.response.active &&
              connection.response.version == HTTPS_CONNECTION_VERSION &&
              connection.response.generation ==
                  https_connection_generation(&connection) &&
              connection.response.output == fixture->output &&
              connection.response.output_capacity ==
                  HTTPS_CONNECTION_RESPONSE_CAPACITY &&
              connection.response.offset == 0U,
          "composed connection publishes one shared generation and cursor");
    CHECK(composed_counts(fixture),
          "composed connection holds exactly one of each resource");

    CHECK(https_connection_response_advance(
              &connection.response, 100U) == HTTPS_CONNECTION_OK &&
              connection.response.offset == 100U,
          "response cursor advances within capacity");
    CHECK(https_connection_response_advance(
              &connection.response,
              HTTPS_CONNECTION_RESPONSE_CAPACITY - 100U) ==
              HTTPS_CONNECTION_OK &&
              connection.response.offset ==
                  HTTPS_CONNECTION_RESPONSE_CAPACITY,
          "response cursor advances to exact capacity");
    CHECK(https_connection_response_advance(
              &connection.response, 1U) ==
              HTTPS_CONNECTION_RESPONSE_OVERFLOW &&
              connection.response.offset ==
                  HTTPS_CONNECTION_RESPONSE_CAPACITY,
          "response cursor overflow fails without mutation");

    CHECK(https_connection_release(
              &fixture->composer, &connection) == HTTPS_CONNECTION_OK &&
              !https_connection_published(&connection) &&
              !connection.response.active,
          "composed connection releases and unpublishes");
    CHECK(initial_counts(fixture),
          "composed connection release restores exact initial counts");
    CHECK(https_connection_release(
              &fixture->composer, &connection) ==
              HTTPS_CONNECTION_NOT_COMPOSED,
          "second release is rejected");
    CHECK(initial_counts(fixture),
          "second release leaves initial counts unchanged");
}

static void test_fifth_connection_denial_and_recovery(
    struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct https_connection contexts[HTTPS_CONNECTION_CAPACITY];
    struct https_connection fifth = zero_connection();
    unsigned char original[sizeof(fifth)];
    uint32_t index;

    memset(contexts, 0, sizeof(contexts));
    for (index = 0U; index < HTTPS_CONNECTION_CAPACITY; ++index) {
        CHECK(https_connection_compose(
                  &fixture->composer, &config, &contexts[index]) ==
                  HTTPS_CONNECTION_OK,
              "four valid HTTPS connection contexts compose");
        CHECK(https_connection_published(&contexts[index]),
              "each composed context is published");
    }
    CHECK(https_connection_capacity(&fixture->composer) ==
              HTTPS_CONNECTION_CAPACITY &&
          https_connection_available(&fixture->composer) == 0U &&
          !https_connection_capacity_available(&fixture->composer),
          "four contexts consume every capacity slot");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) == 0U &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 4U &&
              hfoc_lwip_tcp_pool_published_count(&fixture->pool) == 0U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 4U &&
              http_parser_active_count(&fixture->parser) == 4U,
          "four contexts hold exactly four TCP, TLS, and parser resources");

    memcpy(original, &fifth, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &fifth) ==
              HTTPS_CONNECTION_CAPACITY_FULL &&
              connection_unchanged(&fifth, original),
          "fifth connection is rejected before any TLS allocation");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) == 0U &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 4U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 4U &&
              http_parser_active_count(&fixture->parser) == 4U,
          "fifth denial leaves all live contexts and gates unchanged");

    CHECK(https_connection_release(
              &fixture->composer, &contexts[0]) == HTTPS_CONNECTION_OK &&
              !https_connection_published(&contexts[0]),
          "one exact connection cleanup releases one slot");
    CHECK(https_connection_available(&fixture->composer) == 1U &&
              https_connection_capacity_available(&fixture->composer),
          "one cleanup restores exactly one admission slot");

    fifth = zero_connection();
    CHECK(https_connection_compose(
              &fixture->composer, &config, &fifth) ==
              HTTPS_CONNECTION_OK,
          "new connection composes after one cleanup");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) == 0U &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 4U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 4U &&
              http_parser_active_count(&fixture->parser) == 4U,
          "recovery holds exactly four live contexts");

    for (index = 1U; index < HTTPS_CONNECTION_CAPACITY; ++index) {
        CHECK(https_connection_release(
                  &fixture->composer, &contexts[index]) ==
                  HTTPS_CONNECTION_OK,
              "remaining original contexts release");
    }
    CHECK(https_connection_release(
              &fixture->composer, &fifth) == HTTPS_CONNECTION_OK,
          "recovery context releases");
    CHECK(initial_counts(fixture),
          "fifth denial and recovery restore exact initial counts");
}

static void test_tcp_stage_failure(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct hfoc_lwip_tcp_pool_token tokens[4];
    struct https_connection connection = zero_connection();
    unsigned char original[sizeof(connection)];
    uint32_t index;

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_reserve(
                  &fixture->pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK,
              "TCP failure fixture exhausts pool");
    }
    memcpy(original, &connection, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_CAPACITY_FULL &&
              connection_unchanged(&connection, original),
          "TCP exhaustion fails compose with explicit capacity full");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) == 0U &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 4U &&
              hfoc_lwip_tcp_pool_published_count(&fixture->pool) == 0U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 0U &&
              http_parser_active_count(&fixture->parser) == 0U &&
              https_connection_available(&fixture->composer) == 0U &&
              !https_connection_capacity_available(&fixture->composer),
          "TCP failure leaves pre-existing pool and other gates unchanged");

    for (index = 0U; index < HFOC_LWIP_TCP_POOL_SLOT_COUNT;
         ++index) {
        CHECK(hfoc_lwip_tcp_pool_release(
                  &fixture->pool, &tokens[index]) ==
                  HFOC_LWIP_TCP_POOL_OK,
              "TCP failure fixture releases exhausted reservations");
    }
    CHECK(initial_counts(fixture),
          "TCP failure cleanup restores initial counts");
}

static void test_tls_stage_failure(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct https_connection connection = zero_connection();
    unsigned char original[sizeof(connection)];

    hfoc_mbedtls_alloc_shutdown();
    memcpy(original, &connection, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_TLS_RESERVE_FAILED &&
              connection_unchanged(&connection, original),
          "TLS allocator failure fails compose without publishing");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) ==
              HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 0U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 0U &&
              http_parser_active_count(&fixture->parser) == 0U,
          "TLS stage failure releases reserved TCP slot in reverse order");

    CHECK(hfoc_mbedtls_alloc_init(),
          "TLS failure fixture restarts allocator");
    CHECK(initial_counts(fixture),
          "TLS failure cleanup leaves exact initial counts");
}

static void test_http_stage_failure(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct http_parser_token parser_tokens[4];
    struct https_connection connection = zero_connection();
    unsigned char original[sizeof(connection)];
    uint32_t index;

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        parser_tokens[index].index = index;
        parser_tokens[index].generation = index + 1U;
        CHECK(http_parser_start(
                  &fixture->parser, &parser_tokens[index]) ==
                  HTTP_PARSER_OK,
              "HTTP failure fixture pre-attaches parser slot");
    }
    memcpy(original, &connection, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_HTTP_RESERVE_FAILED &&
              connection_unchanged(&connection, original),
          "parser attachment failure fails compose without publishing");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) ==
              HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 0U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 0U &&
              http_parser_active_count(&fixture->parser) ==
                  HTTP_PARSER_SLOT_COUNT,
          "HTTP stage failure releases TLS then TCP and preserves parser");

    for (index = 0U; index < HTTP_PARSER_SLOT_COUNT; ++index) {
        CHECK(http_parser_release(
                  &fixture->parser, &parser_tokens[index]) ==
                  HTTP_PARSER_OK,
              "HTTP failure fixture releases pre-attached parser slots");
    }
    CHECK(initial_counts(fixture),
          "HTTP failure cleanup restores initial counts");
}

static void test_response_stage_failure(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY - 1U);
    struct https_connection connection = zero_connection();
    unsigned char original[sizeof(connection)];

    memcpy(original, &connection, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY &&
              connection_unchanged(&connection, original),
          "invalid response capacity fails compose without publishing");
    CHECK(hfoc_lwip_tcp_pool_free_count(&fixture->pool) ==
              HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
              hfoc_lwip_tcp_pool_reserved_count(&fixture->pool) == 0U &&
              hfoc_tls_handshake_active_count(&fixture->handshake) == 0U &&
              http_parser_active_count(&fixture->parser) == 0U,
          "response stage failure releases parser, TLS, then TCP");
    CHECK(initial_counts(fixture),
          "response failure cleanup restores exact initial counts");

    config.response_output = NULL;
    config.response_capacity = HTTPS_CONNECTION_RESPONSE_CAPACITY;
    memcpy(original, &connection, sizeof(original));
    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_RESPONSE_INIT_FAILED &&
              connection_unchanged(&connection, original),
          "null response output fails compose without publishing");
    CHECK(initial_counts(fixture),
          "null response output cleanup restores exact initial counts");
}

static void test_response_cursor_boundaries(struct fixture *fixture)
{
    struct https_connection_response response;

    memset(&response, 0, sizeof(response));
    CHECK(https_connection_response_advance(&response, 1U) ==
              HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE,
          "inactive response cursor rejects advance");
    CHECK(https_connection_response_init(
              &response,
              1U,
              fixture->output,
              HTTPS_CONNECTION_RESPONSE_CAPACITY - 1U) ==
              HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY &&
              !response.active,
          "response cursor rejects wrong capacity");
    CHECK(https_connection_response_init(
              &response,
              1U,
              fixture->output,
              HTTPS_CONNECTION_RESPONSE_CAPACITY) ==
              HTTPS_CONNECTION_OK &&
              response.active &&
              response.version == HTTPS_CONNECTION_VERSION &&
              response.generation == 1U &&
              response.output == fixture->output &&
              response.offset == 0U,
          "response cursor init publishes fixed capacity");
    CHECK(https_connection_response_init(
              &response,
              1U,
              fixture->output,
              HTTPS_CONNECTION_RESPONSE_CAPACITY) ==
              HTTPS_CONNECTION_BAD_STATE,
          "response cursor rejects double init");
    CHECK(https_connection_response_advance(
              &response, HTTPS_CONNECTION_RESPONSE_CAPACITY) ==
              HTTPS_CONNECTION_OK &&
              response.offset == HTTPS_CONNECTION_RESPONSE_CAPACITY,
          "response cursor advances to full capacity");
    CHECK(https_connection_response_advance(&response, 1U) ==
              HTTPS_CONNECTION_RESPONSE_OVERFLOW &&
              response.offset == HTTPS_CONNECTION_RESPONSE_CAPACITY,
          "response cursor overflow does not mutate offset");
    CHECK(https_connection_response_clear(&response) ==
              HTTPS_CONNECTION_OK &&
              !response.active &&
              response.offset == 0U &&
              response.output == NULL,
          "response cursor clear zeroizes cursor metadata");
    CHECK(https_connection_response_clear(&response) ==
              HTTPS_CONNECTION_OK &&
              !response.active,
          "response cursor clear is idempotent");
}

static void test_reuse_after_each_failure(struct fixture *fixture)
{
    struct https_connection_config config =
        make_config(fixture, HTTPS_CONNECTION_RESPONSE_CAPACITY);
    struct https_connection connection = zero_connection();

    CHECK(https_connection_compose(
              &fixture->composer, &config, &connection) ==
              HTTPS_CONNECTION_OK &&
              https_connection_release(
                  &fixture->composer, &connection) ==
                  HTTPS_CONNECTION_OK,
          "composition succeeds and releases after rollback fixtures");
    CHECK(initial_counts(fixture),
          "post-failure reuse restores exact initial counts");
}

int main(void)
{
    struct fixture fixture;

    reset_fixture(&fixture);
    test_status_strings();
    test_null_and_publication(&fixture);
    test_capacity_accessors(&fixture);
    test_success_compose_and_release(&fixture);
    test_fifth_connection_denial_and_recovery(&fixture);
    test_tcp_stage_failure(&fixture);
    test_tls_stage_failure(&fixture);
    test_http_stage_failure(&fixture);
    test_response_stage_failure(&fixture);
    test_response_cursor_boundaries(&fixture);
    test_reuse_after_each_failure(&fixture);

    if (failures != 0) {
        fprintf(stderr,
                "HTTPS connection composition host failures: %d\n",
                failures);
        return 1;
    }
    printf("HTTPS connection composition host test: ALL OK\n");
    return 0;
}

#endif
