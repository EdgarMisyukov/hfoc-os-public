#ifndef HFOC_HTTPS_CONNECTION_H
#define HFOC_HTTPS_CONNECTION_H

#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_tls_handshake.h"
#include "http_parser.h"
#include "http_response.h"

#include <stdbool.h>
#include <stdint.h>

#define HTTPS_CONNECTION_VERSION 1U
#define HTTPS_CONNECTION_GENERATION_MIN 1U
#define HTTPS_CONNECTION_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HTTPS_CONNECTION_RESPONSE_CAPACITY HTTP_RESPONSE_BUFFER_MAX
#define HTTPS_CONNECTION_CAPACITY HFOC_LWIP_TCP_POOL_SLOT_COUNT

enum https_connection_status {
    HTTPS_CONNECTION_OK = 0,
    HTTPS_CONNECTION_NULL = 1,
    HTTPS_CONNECTION_BAD_STATE = 2,
    HTTPS_CONNECTION_BAD_VERSION = 3,
    HTTPS_CONNECTION_ALREADY_COMPOSED = 4,
    HTTPS_CONNECTION_NOT_COMPOSED = 5,
    HTTPS_CONNECTION_TCP_RESERVE_FAILED = 6,
    HTTPS_CONNECTION_TLS_RESERVE_FAILED = 7,
    HTTPS_CONNECTION_HTTP_RESERVE_FAILED = 8,
    HTTPS_CONNECTION_RESPONSE_INIT_FAILED = 9,
    HTTPS_CONNECTION_RESPONSE_BAD_CAPACITY = 10,
    HTTPS_CONNECTION_RESPONSE_OVERFLOW = 11,
    HTTPS_CONNECTION_RESPONSE_NOT_ACTIVE = 12,
    HTTPS_CONNECTION_RELEASE_FAILED = 13,
    HTTPS_CONNECTION_CAPACITY_FULL = 14,
};

struct https_connection_response {
    bool active;
    uint32_t version;
    uint32_t generation;
    uint8_t *output;
    uint32_t output_capacity;
    uint32_t offset;
};

struct https_connection_composer {
    bool published;
    uint32_t version;
    struct hfoc_lwip_tcp_pool *tcp_pool;
    struct hfoc_tls_handshake *tls_handshake;
    struct http_parser *http_parser;
};

struct https_connection_config {
    uint32_t version;
    uint8_t *response_output;
    uint32_t response_capacity;
};

struct https_connection {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct hfoc_lwip_tcp_pool_token tcp_token;
    struct hfoc_tls_handshake_token tls_token;
    struct http_parser_token parser_token;
    struct https_connection_response response;
};

enum https_connection_status https_connection_composer_init(
    struct https_connection_composer *composer,
    struct hfoc_lwip_tcp_pool *tcp_pool,
    struct hfoc_tls_handshake *tls_handshake,
    struct http_parser *http_parser);
bool https_connection_composer_published(
    const struct https_connection_composer *composer);

enum https_connection_status https_connection_compose(
    struct https_connection_composer *composer,
    const struct https_connection_config *config,
    struct https_connection *connection);
enum https_connection_status https_connection_release(
    struct https_connection_composer *composer,
    struct https_connection *connection);
uint32_t https_connection_capacity(
    const struct https_connection_composer *composer);
uint32_t https_connection_available(
    const struct https_connection_composer *composer);
bool https_connection_capacity_available(
    const struct https_connection_composer *composer);

bool https_connection_published(
    const struct https_connection *connection);
uint32_t https_connection_generation(
    const struct https_connection *connection);
const struct hfoc_lwip_tcp_pool_token *https_connection_tcp_token(
    const struct https_connection *connection);
const struct hfoc_tls_handshake_token *https_connection_tls_token(
    const struct https_connection *connection);
const struct http_parser_token *https_connection_parser_token(
    const struct https_connection *connection);

enum https_connection_status https_connection_response_init(
    struct https_connection_response *response,
    uint32_t generation,
    uint8_t *output,
    uint32_t output_capacity);
enum https_connection_status https_connection_response_advance(
    struct https_connection_response *response,
    uint32_t amount);
enum https_connection_status https_connection_response_clear(
    struct https_connection_response *response);

const char *https_connection_status_string(
    enum https_connection_status status);

#endif
