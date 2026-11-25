#include "https_connection.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_CONNECTION_HOST_TEST)

static bool https_connection_generation_valid(uint32_t generation)
{
    return generation >= HTTPS_CONNECTION_GENERATION_MIN &&
           generation <= HTTPS_CONNECTION_GENERATION_MAX;
}

static bool https_connection_response_valid(
    const struct https_connection_response *response)
{
    return response != NULL &&
           response->active &&
           response->version == HTTPS_CONNECTION_VERSION &&
           https_connection_generation_valid(response->generation) &&
           response->output != NULL &&
           response->output_capacity ==
               HTTPS_CONNECTION_RESPONSE_CAPACITY &&
           response->offset <= response->output_capacity;
}

static bool https_connection_composer_valid(
    const struct https_connection_composer *composer)
{
    return composer != NULL &&
           composer->published &&
           composer->version == HTTPS_CONNECTION_VERSION &&
           composer->tcp_pool != NULL &&
           composer->tls_handshake != NULL &&
           composer->http_parser != NULL &&
           hfoc_lwip_tcp_pool_published(composer->tcp_pool) &&
           hfoc_tls_handshake_published(composer->tls_handshake) &&
           http_parser_published(composer->http_parser);
}

static bool https_connection_valid(
    const struct https_connection *connection)
{
    return connection != NULL &&
           connection->published &&
           connection->version == HTTPS_CONNECTION_VERSION &&
           https_connection_generation_valid(connection->generation) &&
           connection->tcp_token.index <
               HFOC_LWIP_TCP_POOL_SLOT_COUNT &&
           connection->tcp_token.generation ==
               connection->generation &&
           connection->tls_token.index == connection->tcp_token.index &&
           connection->tls_token.generation == connection->generation &&
           connection->parser_token.index == connection->tcp_token.index &&
           connection->parser_token.generation ==
               connection->generation &&
           https_connection_response_valid(&connection->response) &&
           connection->response.generation == connection->generation;
}

static void https_connection_cleanup_partial(
    struct https_connection_composer *composer,
    struct https_connection *pending,
    bool response_owned,
    bool parser_owned,
    bool tls_owned,
    bool tcp_owned)
{
    if (response_owned) {
        (void)https_connection_response_clear(&pending->response);
    }
    if (parser_owned) {
        (void)http_parser_release(
            composer->http_parser, &pending->parser_token);
    }
    if (tls_owned) {
        (void)hfoc_tls_handshake_release(
            composer->tls_handshake, &pending->tls_token);
    }
    if (tcp_owned) {
        (void)hfoc_lwip_tcp_pool_release(
            composer->tcp_pool, &pending->tcp_token);
    }
}

enum https_connection_status https_connection_composer_init(
    struct https_connection_composer *composer,
    struct hfoc_lwip_tcp_pool *tcp_pool,
    struct hfoc_tls_handshake *tls_handshake,
    struct http_parser *http_parser)
{
    if (composer == NULL ||
        tcp_pool == NULL ||
        tls_handshake == NULL ||
        http_parser == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    if (composer->published ||
        composer->version != 0U ||
        composer->tcp_pool != NULL ||
        composer->tls_handshake != NULL ||
        composer->http_parser != NULL) {
        return HTTPS_CONNECTION_BAD_STATE;
    }
    if (!hfoc_lwip_tcp_pool_published(tcp_pool) ||
        !hfoc_tls_handshake_published(tls_handshake) ||
        !http_parser_published(http_parser)) {
        return HTTPS_CONNECTION_BAD_STATE;
    }

    memset(composer, 0, sizeof(*composer));
    composer->version = HTTPS_CONNECTION_VERSION;
    composer->tcp_pool = tcp_pool;
    composer->tls_handshake = tls_handshake;
    composer->http_parser = http_parser;
    composer->published = true;
    return HTTPS_CONNECTION_OK;
}

bool https_connection_composer_published(
    const struct https_connection_composer *composer)
{
    return https_connection_composer_valid(composer);
}

uint32_t https_connection_capacity(
    const struct https_connection_composer *composer)
{
    return https_connection_composer_valid(composer)
        ? HTTPS_CONNECTION_CAPACITY
        : 0U;
}

uint32_t https_connection_available(
    const struct https_connection_composer *composer)
{
    if (!https_connection_composer_valid(composer)) {
        return 0U;
    }
    return hfoc_lwip_tcp_pool_free_count(composer->tcp_pool);
}

bool https_connection_capacity_available(
    const struct https_connection_composer *composer)
{
    return https_connection_available(composer) > 0U;
}

enum https_connection_status https_connection_compose(
    struct https_connection_composer *composer,
    const struct https_connection_config *config,
    struct https_connection *connection)
{
    struct https_connection pending;
    enum hfoc_lwip_tcp_pool_status tcp_status;
    enum hfoc_tls_handshake_status tls_status;
    enum http_parser_status parser_status;
    enum https_connection_status response_status;

    if (composer == NULL || config == NULL || connection == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    if (!https_connection_composer_valid(composer)) {
        return HTTPS_CONNECTION_BAD_STATE;
    }
    if (connection->published) {
        return HTTPS_CONNECTION_ALREADY_COMPOSED;
    }
    if (config->version != HTTPS_CONNECTION_VERSION) {
        return HTTPS_CONNECTION_BAD_VERSION;
    }
    if (config->response_output == NULL) {
        return HTTPS_CONNECTION_RESPONSE_INIT_FAILED;
    }
    if (config->response_capacity !=
        HTTPS_CONNECTION_RESPONSE_CAPACITY) {
        return HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY;
    }
    if (!https_connection_capacity_available(composer)) {
        return HTTPS_CONNECTION_CAPACITY_FULL;
    }

    memset(&pending, 0, sizeof(pending));
    tcp_status = hfoc_lwip_tcp_pool_reserve(
        composer->tcp_pool, &pending.tcp_token);
    if (tcp_status != HFOC_LWIP_TCP_POOL_OK) {
        return HTTPS_CONNECTION_TCP_RESERVE_FAILED;
    }

    pending.tls_token.index = pending.tcp_token.index;
    pending.tls_token.generation = pending.tcp_token.generation;
    tls_status = hfoc_tls_handshake_reserve(
        composer->tls_handshake, &pending.tls_token);
    if (tls_status != HFOC_TLS_HANDSHAKE_OK) {
        https_connection_cleanup_partial(
            composer, &pending, false, false, false, true);
        return HTTPS_CONNECTION_TLS_RESERVE_FAILED;
    }

    pending.parser_token.index = pending.tcp_token.index;
    pending.parser_token.generation = pending.tcp_token.generation;
    parser_status = http_parser_start(
        composer->http_parser, &pending.parser_token);
    if (parser_status != HTTP_PARSER_OK) {
        https_connection_cleanup_partial(
            composer, &pending, false, false, true, true);
        return HTTPS_CONNECTION_HTTP_RESERVE_FAILED;
    }

    response_status = https_connection_response_init(
        &pending.response,
        pending.tcp_token.generation,
        config->response_output,
        config->response_capacity);
    if (response_status != HTTPS_CONNECTION_OK) {
        https_connection_cleanup_partial(
            composer, &pending, false, true, true, true);
        return response_status == HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY
            ? HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY
            : HTTPS_CONNECTION_RESPONSE_INIT_FAILED;
    }

    pending.version = HTTPS_CONNECTION_VERSION;
    pending.generation = pending.tcp_token.generation;
    pending.published = true;
    *connection = pending;
    return HTTPS_CONNECTION_OK;
}

enum https_connection_status https_connection_release(
    struct https_connection_composer *composer,
    struct https_connection *connection)
{
    bool failed = false;

    if (composer == NULL || connection == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    if (!https_connection_composer_valid(composer)) {
        return HTTPS_CONNECTION_BAD_STATE;
    }
    if (!https_connection_valid(connection)) {
        return HTTPS_CONNECTION_NOT_COMPOSED;
    }

    (void)https_connection_response_clear(&connection->response);
    if (http_parser_release(
            composer->http_parser, &connection->parser_token) !=
        HTTP_PARSER_OK) {
        failed = true;
    }
    if (hfoc_tls_handshake_release(
            composer->tls_handshake, &connection->tls_token) !=
        HFOC_TLS_HANDSHAKE_OK) {
        failed = true;
    }
    if (hfoc_lwip_tcp_pool_release(
            composer->tcp_pool, &connection->tcp_token) !=
        HFOC_LWIP_TCP_POOL_OK) {
        failed = true;
    }
    if (!failed) {
        memset(connection, 0, sizeof(*connection));
        return HTTPS_CONNECTION_OK;
    }
    return HTTPS_CONNECTION_RELEASE_FAILED;
}

bool https_connection_published(
    const struct https_connection *connection)
{
    return https_connection_valid(connection);
}

uint32_t https_connection_generation(
    const struct https_connection *connection)
{
    return https_connection_valid(connection)
        ? connection->generation
        : 0U;
}

const struct hfoc_lwip_tcp_pool_token *https_connection_tcp_token(
    const struct https_connection *connection)
{
    return https_connection_valid(connection)
        ? &connection->tcp_token
        : NULL;
}

const struct hfoc_tls_handshake_token *https_connection_tls_token(
    const struct https_connection *connection)
{
    return https_connection_valid(connection)
        ? &connection->tls_token
        : NULL;
}

const struct http_parser_token *https_connection_parser_token(
    const struct https_connection *connection)
{
    return https_connection_valid(connection)
        ? &connection->parser_token
        : NULL;
}

enum https_connection_status https_connection_response_init(
    struct https_connection_response *response,
    uint32_t generation,
    uint8_t *output,
    uint32_t output_capacity)
{
    if (response == NULL || output == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    if (response->active) {
        return HTTPS_CONNECTION_BAD_STATE;
    }
    if (!https_connection_generation_valid(generation)) {
        return HTTPS_CONNECTION_BAD_STATE;
    }
    if (output_capacity != HTTPS_CONNECTION_RESPONSE_CAPACITY) {
        return HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY;
    }

    memset(response, 0, sizeof(*response));
    response->version = HTTPS_CONNECTION_VERSION;
    response->generation = generation;
    response->output = output;
    response->output_capacity = output_capacity;
    response->offset = 0U;
    response->active = true;
    return HTTPS_CONNECTION_OK;
}

enum https_connection_status https_connection_response_advance(
    struct https_connection_response *response,
    uint32_t amount)
{
    if (response == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    if (!https_connection_response_valid(response)) {
        return HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE;
    }
    if (amount > response->output_capacity - response->offset) {
        return HTTPS_CONNECTION_RESPONSE_OVERFLOW;
    }
    response->offset += amount;
    return HTTPS_CONNECTION_OK;
}

enum https_connection_status https_connection_response_clear(
    struct https_connection_response *response)
{
    if (response == NULL) {
        return HTTPS_CONNECTION_NULL;
    }
    memset(response, 0, sizeof(*response));
    return HTTPS_CONNECTION_OK;
}

const char *https_connection_status_string(
    enum https_connection_status status)
{
    switch (status) {
    case HTTPS_CONNECTION_OK:
        return "HTTPS_CONNECTION_OK";
    case HTTPS_CONNECTION_NULL:
        return "HTTPS_CONNECTION_NULL";
    case HTTPS_CONNECTION_BAD_STATE:
        return "HTTPS_CONNECTION_BAD_STATE";
    case HTTPS_CONNECTION_BAD_VERSION:
        return "HTTPS_CONNECTION_BAD_VERSION";
    case HTTPS_CONNECTION_ALREADY_COMPOSED:
        return "HTTPS_CONNECTION_ALREADY_COMPOSED";
    case HTTPS_CONNECTION_NOT_COMPOSED:
        return "HTTPS_CONNECTION_NOT_COMPOSED";
    case HTTPS_CONNECTION_TCP_RESERVE_FAILED:
        return "HTTPS_CONNECTION_TCP_RESERVE_FAILED";
    case HTTPS_CONNECTION_TLS_RESERVE_FAILED:
        return "HTTPS_CONNECTION_TLS_RESERVE_FAILED";
    case HTTPS_CONNECTION_HTTP_RESERVE_FAILED:
        return "HTTPS_CONNECTION_HTTP_RESERVE_FAILED";
    case HTTPS_CONNECTION_RESPONSE_INIT_FAILED:
        return "HTTPS_CONNECTION_RESPONSE_INIT_FAILED";
    case HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY:
        return "HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY";
    case HTTPS_CONNECTION_RESPONSE_OVERFLOW:
        return "HTTPS_CONNECTION_RESPONSE_OVERFLOW";
    case HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE:
        return "HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE";
    case HTTPS_CONNECTION_RELEASE_FAILED:
        return "HTTPS_CONNECTION_RELEASE_FAILED";
    case HTTPS_CONNECTION_CAPACITY_FULL:
        return "HTTPS_CONNECTION_CAPACITY_FULL";
    default:
        return NULL;
    }
}

#else

typedef int https_connection_translation_unit_guard;

#endif
