#ifndef HFOC_HTTPS_DATA_FLOW_H
#define HFOC_HTTPS_DATA_FLOW_H

#include "https_connection.h"
#include "http_parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HTTPS_DATA_FLOW_VERSION 1U
#define HTTPS_DATA_FLOW_MAX_REQUEST_BYTES \
    (HTTP_PARSER_REQUEST_LINE_MAX + HTTP_PARSER_HEADERS_MAX)
#define HTTPS_DATA_FLOW_MAX_RESPONSE_BYTES \
    HTTPS_CONNECTION_RESPONSE_CAPACITY

enum https_data_flow_status {
    HTTPS_DATA_FLOW_OK = 0,
    HTTPS_DATA_FLOW_NULL = 1,
    HTTPS_DATA_FLOW_BAD_STATE = 2,
    HTTPS_DATA_FLOW_BAD_VERSION = 3,
    HTTPS_DATA_FLOW_NOT_PUBLISHED = 4,
    HTTPS_DATA_FLOW_STALE_GENERATION = 5,
    HTTPS_DATA_FLOW_INVALID_PROGRESS = 6,
    HTTPS_DATA_FLOW_PARSER_TERMINAL = 7,
    HTTPS_DATA_FLOW_PARSER_REJECTED = 8,
    HTTPS_DATA_FLOW_TLS_RETAINED = 9,
    HTTPS_DATA_FLOW_TLS_REJECTED = 10,
    HTTPS_DATA_FLOW_TLS_CLOSED = 11,
};

enum https_data_flow_tls_status {
    HTTPS_DATA_FLOW_SEND_ACCEPTED = 0,
    HTTPS_DATA_FLOW_SEND_RETAINED = 1,
    HTTPS_DATA_FLOW_SEND_CLOSED = 2,
    HTTPS_DATA_FLOW_SEND_REJECTED = 3,
};

typedef enum https_data_flow_tls_status
    (*https_data_flow_tls_send_fn)(
        void *context,
        const uint8_t *bytes,
        size_t length,
        size_t *bytes_accepted);

struct https_data_flow_config {
    uint32_t version;
    struct http_parser *parser;
    struct https_connection *connection;
    https_data_flow_tls_send_fn tls_send;
    void *tls_send_context;
};

struct https_data_flow {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct http_parser *parser;
    struct http_parser_token parser_token;
    struct https_connection *connection;
    https_data_flow_tls_send_fn tls_send;
    void *tls_send_context;
    uint32_t request_bytes;
    uint32_t response_sent;
    uint32_t retained_count;
    uint32_t rejected_count;
    uint32_t closed_count;
};

enum https_data_flow_status https_data_flow_init(
    struct https_data_flow *flow,
    const struct https_data_flow_config *config);
bool https_data_flow_published(
    const struct https_data_flow *flow);
uint32_t https_data_flow_generation(
    const struct https_data_flow *flow);
uint32_t https_data_flow_request_bytes(
    const struct https_data_flow *flow);
uint32_t https_data_flow_response_sent(
    const struct https_data_flow *flow);
uint32_t https_data_flow_retained_count(
    const struct https_data_flow *flow);
uint32_t https_data_flow_rejected_count(
    const struct https_data_flow *flow);
uint32_t https_data_flow_closed_count(
    const struct https_data_flow *flow);

enum https_data_flow_status https_data_flow_rx(
    struct https_data_flow *flow,
    const uint8_t *bytes,
    uint32_t length,
    uint32_t *consumed);
enum https_data_flow_status https_data_flow_tx(
    struct https_data_flow *flow,
    uint32_t response_length,
    uint32_t *bytes_accepted);
enum https_data_flow_status https_data_flow_release(
    struct https_data_flow *flow);

const char *https_data_flow_status_string(
    enum https_data_flow_status status);
const char *https_data_flow_tls_status_string(
    enum https_data_flow_tls_status status);

#endif
