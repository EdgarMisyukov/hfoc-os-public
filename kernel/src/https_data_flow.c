#include "https_data_flow.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HTTPS_DATA_FLOW_HOST_TEST)

static bool https_data_flow_generation_valid(uint32_t generation)
{
    return generation >= HTTPS_CONNECTION_GENERATION_MIN &&
           generation <= HTTPS_CONNECTION_GENERATION_MAX;
}

static bool https_data_flow_base_valid(
    const struct https_data_flow *flow)
{
    return flow != NULL &&
           flow->published &&
           flow->version == HTTPS_DATA_FLOW_VERSION &&
           https_data_flow_generation_valid(flow->generation) &&
           flow->parser != NULL &&
           flow->connection != NULL &&
           flow->tls_send != NULL &&
           flow->request_bytes <= HTTPS_DATA_FLOW_MAX_REQUEST_BYTES &&
           flow->response_sent <=
               HTTPS_DATA_FLOW_MAX_RESPONSE_BYTES;
}

static bool https_data_flow_parser_current(
    const struct https_data_flow *flow)
{
    const struct http_parser_token *token;
    const struct https_connection *connection;

    if (!https_data_flow_base_valid(flow) ||
        !http_parser_published(flow->parser) ||
        !https_connection_published(flow->connection)) {
        return false;
    }
    connection = flow->connection;
    token = https_connection_parser_token(connection);
    return token != NULL &&
           token->index == flow->parser_token.index &&
           token->generation == flow->parser_token.generation &&
           connection->generation == flow->generation &&
           http_parser_generation(flow->parser, token) ==
               flow->generation;
}

static bool https_data_flow_valid(
    const struct https_data_flow *flow)
{
    const struct https_connection *connection;

    if (!https_data_flow_parser_current(flow)) {
        return false;
    }
    connection = flow->connection;
    return flow->response_sent <= connection->response.offset &&
           connection->response.offset <=
               HTTPS_DATA_FLOW_MAX_RESPONSE_BYTES;
}

enum https_data_flow_status https_data_flow_init(
    struct https_data_flow *flow,
    const struct https_data_flow_config *config)
{
    const struct http_parser_token *token;

    if (flow == NULL || config == NULL ||
        config->parser == NULL ||
        config->connection == NULL ||
        config->tls_send == NULL) {
        return HTTPS_DATA_FLOW_NULL;
    }
    if (flow->published ||
        flow->version != 0U ||
        flow->generation != 0U ||
        flow->parser != NULL ||
        flow->connection != NULL ||
        flow->tls_send != NULL) {
        return HTTPS_DATA_FLOW_BAD_STATE;
    }
    if (config->version != HTTPS_DATA_FLOW_VERSION) {
        return HTTPS_DATA_FLOW_BAD_VERSION;
    }
    if (!http_parser_published(config->parser) ||
        !https_connection_published(config->connection)) {
        return HTTPS_DATA_FLOW_BAD_STATE;
    }

    token = https_connection_parser_token(config->connection);
    if (token == NULL ||
        http_parser_generation(config->parser, token) !=
            config->connection->generation) {
        return HTTPS_DATA_FLOW_BAD_STATE;
    }

    flow->version = HTTPS_DATA_FLOW_VERSION;
    flow->generation = config->connection->generation;
    flow->parser = config->parser;
    flow->parser_token = *token;
    flow->connection = config->connection;
    flow->tls_send = config->tls_send;
    flow->tls_send_context = config->tls_send_context;
    flow->published = true;
    return HTTPS_DATA_FLOW_OK;
}

bool https_data_flow_published(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow);
}

uint32_t https_data_flow_generation(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->generation
        : 0U;
}

uint32_t https_data_flow_request_bytes(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->request_bytes
        : 0U;
}

uint32_t https_data_flow_response_sent(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->response_sent
        : 0U;
}

uint32_t https_data_flow_retained_count(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->retained_count
        : 0U;
}

uint32_t https_data_flow_rejected_count(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->rejected_count
        : 0U;
}

uint32_t https_data_flow_closed_count(
    const struct https_data_flow *flow)
{
    return https_data_flow_valid(flow)
        ? flow->closed_count
        : 0U;
}

static enum https_data_flow_status
https_data_flow_validate_current(
    struct https_data_flow *flow)
{
    if (!https_data_flow_base_valid(flow)) {
        return HTTPS_DATA_FLOW_BAD_STATE;
    }
    if (!https_connection_published(flow->connection) ||
        !http_parser_published(flow->parser) ||
        flow->connection->generation != flow->generation ||
        !https_data_flow_parser_current(flow)) {
        return HTTPS_DATA_FLOW_STALE_GENERATION;
    }
    return HTTPS_DATA_FLOW_OK;
}

enum https_data_flow_status https_data_flow_rx(
    struct https_data_flow *flow,
    const uint8_t *bytes,
    uint32_t length,
    uint32_t *consumed)
{
    enum https_data_flow_status current;

    if (flow == NULL || consumed == NULL ||
        (length > 0U && bytes == NULL)) {
        return HTTPS_DATA_FLOW_NULL;
    }
    current = https_data_flow_validate_current(flow);
    if (current != HTTPS_DATA_FLOW_OK) {
        return current;
    }

    *consumed = 0U;
    while (*consumed < length) {
        enum http_parser_state state =
            http_parser_state(flow->parser, &flow->parser_token);
        enum http_parser_status status;
        bool complete = false;

        if (state == HTTP_PARSER_STATE_REJECTED ||
            http_request_line_error(flow->parser, &flow->parser_token) !=
                HTTP_PARSER_OK ||
            http_header_error(flow->parser, &flow->parser_token) !=
                HTTP_PARSER_OK) {
            return HTTPS_DATA_FLOW_PARSER_REJECTED;
        }
        if (state == HTTP_PARSER_STATE_COMPLETE) {
            return HTTPS_DATA_FLOW_PARSER_TERMINAL;
        }

        if (state == HTTP_PARSER_STATE_EMPTY ||
            state == HTTP_PARSER_STATE_REQUEST_LINE) {
            status = http_request_line_feed(
                flow->parser,
                &flow->parser_token,
                bytes + *consumed,
                1U,
                &complete);
            if (status == HTTP_PARSER_TERMINAL) {
                return HTTPS_DATA_FLOW_PARSER_TERMINAL;
            }
            ++*consumed;
            if (status != HTTP_PARSER_OK) {
                return HTTPS_DATA_FLOW_PARSER_REJECTED;
            }
            if (complete) {
                struct http_parser_progress progress;

                status = http_parser_progress(
                    flow->parser,
                    &flow->parser_token,
                    &progress);
                if (status != HTTP_PARSER_OK) {
                    return HTTPS_DATA_FLOW_BAD_STATE;
                }
                progress.header_offset =
                    progress.request_line_count;
                status = http_parser_advance(
                    flow->parser,
                    &flow->parser_token,
                    HTTP_PARSER_STATE_HEADERS,
                    &progress);
                if (status != HTTP_PARSER_OK) {
                    return HTTPS_DATA_FLOW_BAD_STATE;
                }
            }
        } else if (state == HTTP_PARSER_STATE_HEADERS) {
            status = http_header_feed(
                flow->parser,
                &flow->parser_token,
                bytes + *consumed,
                1U,
                &complete);
            if (status == HTTP_PARSER_TERMINAL) {
                return HTTPS_DATA_FLOW_PARSER_TERMINAL;
            }
            ++*consumed;
            if (status != HTTP_PARSER_OK) {
                return HTTPS_DATA_FLOW_PARSER_REJECTED;
            }
            if (complete) {
                flow->request_bytes += *consumed;
                return HTTPS_DATA_FLOW_OK;
            }
        } else {
            return HTTPS_DATA_FLOW_PARSER_TERMINAL;
        }
    }

    flow->request_bytes += *consumed;
    return HTTPS_DATA_FLOW_OK;
}

enum https_data_flow_status https_data_flow_tx(
    struct https_data_flow *flow,
    uint32_t response_length,
    uint32_t *bytes_accepted)
{
    const struct https_connection_response *response;
    enum https_data_flow_status current;
    enum https_data_flow_tls_status tls_status;
    uint32_t send_length;
    size_t accepted = 0U;

    if (flow == NULL || bytes_accepted == NULL) {
        return HTTPS_DATA_FLOW_NULL;
    }
    current = https_data_flow_validate_current(flow);
    if (current != HTTPS_DATA_FLOW_OK) {
        return current;
    }

    *bytes_accepted = 0U;
    response = &flow->connection->response;
    if (!response->active ||
        response->output == NULL ||
        flow->response_sent > response->offset) {
        return HTTPS_DATA_FLOW_BAD_STATE;
    }
    if (response_length > response->offset ||
        response_length < flow->response_sent) {
        return HTTPS_DATA_FLOW_INVALID_PROGRESS;
    }
    send_length = response_length - flow->response_sent;
    if (send_length == 0U) {
        return HTTPS_DATA_FLOW_OK;
    }

    tls_status = flow->tls_send(
        flow->tls_send_context,
        response->output + flow->response_sent,
        send_length,
        &accepted);
    switch (tls_status) {
    case HTTPS_DATA_FLOW_SEND_ACCEPTED:
        if (accepted == 0U || accepted > send_length) {
            return HTTPS_DATA_FLOW_INVALID_PROGRESS;
        }
        flow->response_sent += (uint32_t)accepted;
        *bytes_accepted = (uint32_t)accepted;
        return HTTPS_DATA_FLOW_OK;
    case HTTPS_DATA_FLOW_SEND_RETAINED:
        if (accepted != 0U) {
            return HTTPS_DATA_FLOW_INVALID_PROGRESS;
        }
        ++flow->retained_count;
        return HTTPS_DATA_FLOW_TLS_RETAINED;
    case HTTPS_DATA_FLOW_SEND_CLOSED:
        if (accepted != 0U) {
            return HTTPS_DATA_FLOW_INVALID_PROGRESS;
        }
        ++flow->closed_count;
        return HTTPS_DATA_FLOW_TLS_CLOSED;
    case HTTPS_DATA_FLOW_SEND_REJECTED:
        if (accepted != 0U) {
            return HTTPS_DATA_FLOW_INVALID_PROGRESS;
        }
        ++flow->rejected_count;
        return HTTPS_DATA_FLOW_TLS_REJECTED;
    default:
        return HTTPS_DATA_FLOW_INVALID_PROGRESS;
    }
}

enum https_data_flow_status https_data_flow_release(
    struct https_data_flow *flow)
{
    if (flow == NULL) {
        return HTTPS_DATA_FLOW_NULL;
    }
    memset(flow, 0, sizeof(*flow));
    return HTTPS_DATA_FLOW_OK;
}

const char *https_data_flow_status_string(
    enum https_data_flow_status status)
{
    switch (status) {
    case HTTPS_DATA_FLOW_OK:
        return "HTTPS_DATA_FLOW_OK";
    case HTTPS_DATA_FLOW_NULL:
        return "HTTPS_DATA_FLOW_NULL";
    case HTTPS_DATA_FLOW_BAD_STATE:
        return "HTTPS_DATA_FLOW_BAD_STATE";
    case HTTPS_DATA_FLOW_BAD_VERSION:
        return "HTTPS_DATA_FLOW_BAD_VERSION";
    case HTTPS_DATA_FLOW_NOT_PUBLISHED:
        return "HTTPS_DATA_FLOW_NOT_PUBLISHED";
    case HTTPS_DATA_FLOW_STALE_GENERATION:
        return "HTTPS_DATA_FLOW_STALE_GENERATION";
    case HTTPS_DATA_FLOW_INVALID_PROGRESS:
        return "HTTPS_DATA_FLOW_INVALID_PROGRESS";
    case HTTPS_DATA_FLOW_PARSER_TERMINAL:
        return "HTTPS_DATA_FLOW_PARSER_TERMINAL";
    case HTTPS_DATA_FLOW_PARSER_REJECTED:
        return "HTTPS_DATA_FLOW_PARSER_REJECTED";
    case HTTPS_DATA_FLOW_TLS_RETAINED:
        return "HTTPS_DATA_FLOW_TLS_RETAINED";
    case HTTPS_DATA_FLOW_TLS_REJECTED:
        return "HTTPS_DATA_FLOW_TLS_REJECTED";
    case HTTPS_DATA_FLOW_TLS_CLOSED:
        return "HTTPS_DATA_FLOW_TLS_CLOSED";
    default:
        return NULL;
    }
}

const char *https_data_flow_tls_status_string(
    enum https_data_flow_tls_status status)
{
    switch (status) {
    case HTTPS_DATA_FLOW_SEND_ACCEPTED:
        return "HTTPS_DATA_FLOW_SEND_ACCEPTED";
    case HTTPS_DATA_FLOW_SEND_RETAINED:
        return "HTTPS_DATA_FLOW_SEND_RETAINED";
    case HTTPS_DATA_FLOW_SEND_CLOSED:
        return "HTTPS_DATA_FLOW_SEND_CLOSED";
    case HTTPS_DATA_FLOW_SEND_REJECTED:
        return "HTTPS_DATA_FLOW_SEND_REJECTED";
    default:
        return NULL;
    }
}

#else

typedef int https_data_flow_translation_unit_guard;

#endif
