#include "appliance_acme_transport.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 11: the ACME client's exchange driver.  Nothing here
 * opens a socket, speaks TLS or allocates: the link does the I/O, and this
 * module decides when, what and whether the answer is complete.
 */

#define APPLIANCE_ACME_TRANSPORT_VERSION_CURRENT 1U

struct appliance_acme_transport_text {
    char *buffer;
    size_t capacity;
    size_t length;
};

static void appliance_acme_transport_text_reset(
    struct appliance_acme_transport_text *text,
    char *buffer,
    size_t capacity)
{
    text->buffer = buffer;
    text->capacity = capacity;
    text->length = 0U;
    if (buffer != NULL && capacity > 0U) {
        buffer[0] = '\0';
    }
}

static void appliance_acme_transport_text_append_n(
    struct appliance_acme_transport_text *text,
    const char *value,
    size_t length)
{
    if (length == 0U || text->length + length + 1U > text->capacity) {
        return;
    }
    memcpy(&text->buffer[text->length], value, length);
    text->length += length;
    text->buffer[text->length] = '\0';
}

static void appliance_acme_transport_text_append(
    struct appliance_acme_transport_text *text,
    const char *value)
{
    if (value != NULL) {
        appliance_acme_transport_text_append_n(text, value, strlen(value));
    }
}

static void appliance_acme_transport_text_append_u32(
    struct appliance_acme_transport_text *text,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);
    while (count > 0U) {
        --count;
        appliance_acme_transport_text_append_n(text, &digits[count], 1U);
    }
}

static bool appliance_acme_transport_link_valid(
    const struct appliance_acme_transport_link *link)
{
    return link != NULL &&
           link->open != NULL &&
           link->advance != NULL &&
           link->send != NULL &&
           link->recv != NULL &&
           link->close != NULL;
}

bool appliance_acme_transport_published(
    const struct appliance_acme_transport *transport)
{
    return transport != NULL &&
           transport->version == APPLIANCE_ACME_TRANSPORT_VERSION &&
           appliance_acme_transport_link_valid(&transport->link);
}

enum appliance_acme_transport_status appliance_acme_transport_init(
    struct appliance_acme_transport *transport,
    const struct appliance_acme_transport_link *link)
{
    if (transport == NULL || link == NULL) {
        return APPLIANCE_ACME_TRANSPORT_NULL;
    }
    if (!appliance_acme_transport_link_valid(link)) {
        return APPLIANCE_ACME_TRANSPORT_BAD_LINK;
    }
    memset(transport, 0, sizeof(*transport));
    transport->version = APPLIANCE_ACME_TRANSPORT_VERSION_CURRENT;
    transport->state = APPLIANCE_ACME_TRANSPORT_STATE_FREE;
    transport->last_status = APPLIANCE_ACME_TRANSPORT_OK;
    transport->http_status = APPLIANCE_ACME_HTTP_OK;
    transport->link = *link;
    return APPLIANCE_ACME_TRANSPORT_OK;
}

static void appliance_acme_transport_publish_record(
    struct appliance_acme_transport *transport,
    const char *outcome)
{
    struct appliance_acme_transport_text text;

    appliance_acme_transport_text_reset(
        &text, transport->record, sizeof(transport->record));
    appliance_acme_transport_text_append(&text, "[ACME-TRANSPORT] outcome=");
    appliance_acme_transport_text_append(&text, outcome);
    appliance_acme_transport_text_append(&text, " status=");
    appliance_acme_transport_text_append(
        &text,
        appliance_acme_transport_status_string(transport->last_status));
    appliance_acme_transport_text_append(&text, " request=");
    appliance_acme_transport_text_append_u32(
        &text, (uint32_t)transport->request_length);
    appliance_acme_transport_text_append(&text, " reply=");
    appliance_acme_transport_text_append_u32(
        &text, (uint32_t)transport->reply_length);
}

/*
 * The one place the link is released.  A failure and a success both close it;
 * a second close is a no-op, so a caller may always call `close`.
 */
static void appliance_acme_transport_release_link(
    struct appliance_acme_transport *transport)
{
    if (transport->link_attached) {
        transport->link_attached = false;
        transport->link.close(transport->link.context);
    }
}

static enum appliance_acme_transport_status appliance_acme_transport_fail(
    struct appliance_acme_transport *transport,
    enum appliance_acme_transport_status status)
{
    transport->state = APPLIANCE_ACME_TRANSPORT_STATE_FAILED;
    transport->last_status = status;
    appliance_acme_transport_publish_record(transport, "failed");
    appliance_acme_transport_release_link(transport);
    return status;
}

static enum appliance_acme_transport_status appliance_acme_transport_fail_reply(
    struct appliance_acme_transport *transport,
    enum appliance_acme_http_status http_status)
{
    transport->http_status = http_status;
    return appliance_acme_transport_fail(
        transport, APPLIANCE_ACME_TRANSPORT_BAD_REPLY);
}

static enum appliance_acme_transport_status appliance_acme_transport_succeed(
    struct appliance_acme_transport *transport)
{
    transport->state = APPLIANCE_ACME_TRANSPORT_STATE_DONE;
    transport->last_status = APPLIANCE_ACME_TRANSPORT_OK;
    appliance_acme_transport_publish_record(transport, "done");
    appliance_acme_transport_release_link(transport);
    return APPLIANCE_ACME_TRANSPORT_OK;
}

static bool appliance_acme_transport_deadline_reached(
    uint32_t now_ms,
    uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

enum appliance_acme_transport_status appliance_acme_transport_begin(
    struct appliance_acme_transport *transport,
    const char *method,
    size_t method_length,
    const char *url,
    size_t url_length,
    const char *content_type,
    size_t content_type_length,
    const uint8_t *body,
    size_t body_length,
    uint32_t now_ms)
{
    enum appliance_acme_transport_link_status link_status;

    if (transport == NULL || method == NULL || url == NULL) {
        return APPLIANCE_ACME_TRANSPORT_NULL;
    }
    if (!appliance_acme_transport_published(transport)) {
        return APPLIANCE_ACME_TRANSPORT_BAD_LINK;
    }
    if (transport->state == APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING ||
        transport->state == APPLIANCE_ACME_TRANSPORT_STATE_SENDING ||
        transport->state == APPLIANCE_ACME_TRANSPORT_STATE_READING) {
        return APPLIANCE_ACME_TRANSPORT_BUSY;
    }
    if (now_ms == 0U) {
        return APPLIANCE_ACME_TRANSPORT_BAD_CLOCK;
    }
    transport->http_status = appliance_acme_http_parse_url(
        url, url_length, &transport->target);
    if (transport->http_status != APPLIANCE_ACME_HTTP_OK) {
        transport->last_status = APPLIANCE_ACME_TRANSPORT_BAD_URL;
        return transport->last_status;
    }
    transport->http_status = appliance_acme_http_build_request(
        method,
        method_length,
        &transport->target,
        content_type,
        content_type_length,
        body,
        body_length,
        transport->request,
        sizeof(transport->request),
        &transport->request_length);
    if (transport->http_status != APPLIANCE_ACME_HTTP_OK) {
        transport->last_status = APPLIANCE_ACME_TRANSPORT_BAD_REQUEST;
        return transport->last_status;
    }
    transport->request_offset = 0U;
    transport->reply_length = 0U;
    memset(&transport->response, 0, sizeof(transport->response));
    transport->handshake_steps = 0U;
    transport->send_calls = 0U;
    transport->recv_calls = 0U;
    transport->last_now_ms = now_ms;
    transport->deadline_ms = now_ms +
        APPLIANCE_ACME_TRANSPORT_CONNECT_DEADLINE_MS;
    transport->state = APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING;
    transport->link_attached = true;
    link_status = transport->link.open(
        transport->link.context,
        transport->target.host,
        transport->target.port,
        transport->target.literal_ipv4);
    if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_OK ||
        link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ ||
        link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE) {
        transport->last_status = APPLIANCE_ACME_TRANSPORT_PENDING;
        return transport->last_status;
    }
    if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_CLOSED) {
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_CLOSED);
    }
    return appliance_acme_transport_fail(
        transport, APPLIANCE_ACME_TRANSPORT_OPEN_FAILED);
}

/*
 * A body framed by the connection is the rest of the stream, so an `OK` parse
 * without a `Content-Length` is only complete once the peer has closed.  A
 * body framed by `Content-Length` is complete the moment that many bytes
 * arrived, and the parser knows it.
 */
static enum appliance_acme_transport_status appliance_acme_transport_parse(
    struct appliance_acme_transport *transport,
    bool closed,
    bool *complete_out)
{
    transport->http_status = appliance_acme_http_read_response(
        transport->reply, transport->reply_length, &transport->response);
    if (transport->http_status == APPLIANCE_ACME_HTTP_OK) {
        if (transport->response.has_content_length) {
            *complete_out = true;
            return APPLIANCE_ACME_TRANSPORT_OK;
        }
        if (closed) {
            *complete_out = true;
            return APPLIANCE_ACME_TRANSPORT_OK;
        }
        *complete_out = false;
        return APPLIANCE_ACME_TRANSPORT_PENDING;
    }
    if (transport->http_status == APPLIANCE_ACME_HTTP_TRUNCATED) {
        *complete_out = false;
        return closed ? APPLIANCE_ACME_TRANSPORT_BAD_REPLY
                      : APPLIANCE_ACME_TRANSPORT_PENDING;
    }
    *complete_out = false;
    return APPLIANCE_ACME_TRANSPORT_BAD_REPLY;
}

static enum appliance_acme_transport_status appliance_acme_transport_reading(
    struct appliance_acme_transport *transport,
    uint32_t now_ms)
{
    uint32_t call;

    for (call = 0U; call < APPLIANCE_ACME_TRANSPORT_RECV_BUDGET; ++call) {
        enum appliance_acme_transport_link_status link_status;
        enum appliance_acme_transport_status parse_status;
        bool complete = false;
        size_t received = 0U;

        if (appliance_acme_transport_deadline_reached(
                now_ms, transport->deadline_ms)) {
            return appliance_acme_transport_fail(
                transport, APPLIANCE_ACME_TRANSPORT_TIMEOUT);
        }
        link_status = transport->link.recv(
            transport->link.context,
            &transport->reply[transport->reply_length],
            sizeof(transport->reply) - transport->reply_length,
            &received);
        ++transport->recv_calls;
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_OK) {
            if (received > sizeof(transport->reply) - transport->reply_length) {
                return appliance_acme_transport_fail(
                    transport, APPLIANCE_ACME_TRANSPORT_RECV_FAILED);
            }
            transport->reply_length += received;
            parse_status = appliance_acme_transport_parse(
                transport, false, &complete);
            if (complete) {
                return appliance_acme_transport_succeed(transport);
            }
            if (parse_status == APPLIANCE_ACME_TRANSPORT_BAD_REPLY) {
                return appliance_acme_transport_fail_reply(
                    transport, transport->http_status);
            }
            if (transport->reply_length == sizeof(transport->reply)) {
                return appliance_acme_transport_fail(
                    transport, APPLIANCE_ACME_TRANSPORT_BUFFER_TOO_SMALL);
            }
            if (received == 0U) {
                return APPLIANCE_ACME_TRANSPORT_PENDING;
            }
            continue;
        }
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_CLOSED) {
            if (transport->reply_length == 0U) {
                return appliance_acme_transport_fail(
                    transport, APPLIANCE_ACME_TRANSPORT_CLOSED);
            }
            parse_status = appliance_acme_transport_parse(
                transport, true, &complete);
            if (complete) {
                return appliance_acme_transport_succeed(transport);
            }
            return appliance_acme_transport_fail_reply(
                transport, transport->http_status);
        }
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ ||
            link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE) {
            return APPLIANCE_ACME_TRANSPORT_PENDING;
        }
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_RECV_FAILED);
    }
    return APPLIANCE_ACME_TRANSPORT_PENDING;
}

static enum appliance_acme_transport_status appliance_acme_transport_sending(
    struct appliance_acme_transport *transport,
    uint32_t now_ms)
{
    uint32_t call;

    for (call = 0U; call < APPLIANCE_ACME_TRANSPORT_SEND_BUDGET; ++call) {
        enum appliance_acme_transport_link_status link_status;
        size_t remaining = transport->request_length -
                           transport->request_offset;
        size_t accepted = 0U;

        if (remaining == 0U) {
            transport->state = APPLIANCE_ACME_TRANSPORT_STATE_READING;
            transport->deadline_ms = now_ms +
                APPLIANCE_ACME_TRANSPORT_REPLY_DEADLINE_MS;
            return appliance_acme_transport_reading(transport, now_ms);
        }
        link_status = transport->link.send(
            transport->link.context,
            (const uint8_t *)&transport->request[transport->request_offset],
            remaining,
            &accepted);
        ++transport->send_calls;
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_OK) {
            if (accepted > remaining) {
                return appliance_acme_transport_fail(
                    transport, APPLIANCE_ACME_TRANSPORT_SEND_FAILED);
            }
            transport->request_offset += accepted;
            if (accepted == 0U) {
                return APPLIANCE_ACME_TRANSPORT_PENDING;
            }
            continue;
        }
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE ||
            link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ) {
            return APPLIANCE_ACME_TRANSPORT_PENDING;
        }
        if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_CLOSED) {
            return appliance_acme_transport_fail(
                transport, APPLIANCE_ACME_TRANSPORT_CLOSED);
        }
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_SEND_FAILED);
    }
    return APPLIANCE_ACME_TRANSPORT_PENDING;
}

static enum appliance_acme_transport_status appliance_acme_transport_connecting(
    struct appliance_acme_transport *transport,
    uint32_t now_ms)
{
    enum appliance_acme_transport_link_status link_status;

    if (appliance_acme_transport_deadline_reached(
            now_ms, transport->deadline_ms)) {
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_TIMEOUT);
    }
    ++transport->handshake_steps;
    link_status = transport->link.advance(transport->link.context);
    if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_OK) {
        transport->state = APPLIANCE_ACME_TRANSPORT_STATE_SENDING;
        return appliance_acme_transport_sending(transport, now_ms);
    }
    if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ ||
        link_status == APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE) {
        return APPLIANCE_ACME_TRANSPORT_PENDING;
    }
    if (link_status == APPLIANCE_ACME_TRANSPORT_LINK_CLOSED) {
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_CLOSED);
    }
    return appliance_acme_transport_fail(
        transport, APPLIANCE_ACME_TRANSPORT_HANDSHAKE_FAILED);
}

enum appliance_acme_transport_status appliance_acme_transport_advance(
    struct appliance_acme_transport *transport,
    uint32_t now_ms)
{
    if (transport == NULL) {
        return APPLIANCE_ACME_TRANSPORT_NULL;
    }
    if (!appliance_acme_transport_published(transport)) {
        return APPLIANCE_ACME_TRANSPORT_BAD_LINK;
    }
    /*
     * The state is read first: a call after the exchange finished must not
     * turn a complete reply into a clock refusal.
     */
    if (transport->state != APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING &&
        transport->state != APPLIANCE_ACME_TRANSPORT_STATE_SENDING &&
        transport->state != APPLIANCE_ACME_TRANSPORT_STATE_READING) {
        return APPLIANCE_ACME_TRANSPORT_BAD_STATE;
    }
    if (now_ms == 0U ||
        (transport->last_now_ms != 0U &&
         (int32_t)(now_ms - transport->last_now_ms) < 0)) {
        return appliance_acme_transport_fail(
            transport, APPLIANCE_ACME_TRANSPORT_BAD_CLOCK);
    }
    transport->last_now_ms = now_ms;
    switch (transport->state) {
    case APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING:
        return appliance_acme_transport_connecting(transport, now_ms);
    case APPLIANCE_ACME_TRANSPORT_STATE_SENDING:
        return appliance_acme_transport_sending(transport, now_ms);
    case APPLIANCE_ACME_TRANSPORT_STATE_READING:
        return appliance_acme_transport_reading(transport, now_ms);
    default:
        return APPLIANCE_ACME_TRANSPORT_BAD_STATE;
    }
}

enum appliance_acme_transport_status appliance_acme_transport_take_response(
    struct appliance_acme_transport *transport,
    struct appliance_acme_http_response *response_out)
{
    if (transport == NULL || response_out == NULL) {
        return APPLIANCE_ACME_TRANSPORT_NULL;
    }
    if (transport->state != APPLIANCE_ACME_TRANSPORT_STATE_DONE) {
        return APPLIANCE_ACME_TRANSPORT_BAD_STATE;
    }
    *response_out = transport->response;
    return APPLIANCE_ACME_TRANSPORT_OK;
}

void appliance_acme_transport_close(
    struct appliance_acme_transport *transport)
{
    if (transport == NULL) {
        return;
    }
    appliance_acme_transport_release_link(transport);
    transport->state = APPLIANCE_ACME_TRANSPORT_STATE_FREE;
}

const char *appliance_acme_transport_state_string(
    enum appliance_acme_transport_state state)
{
    switch (state) {
    case APPLIANCE_ACME_TRANSPORT_STATE_FREE:
        return "free";
    case APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING:
        return "connecting";
    case APPLIANCE_ACME_TRANSPORT_STATE_SENDING:
        return "sending";
    case APPLIANCE_ACME_TRANSPORT_STATE_READING:
        return "reading";
    case APPLIANCE_ACME_TRANSPORT_STATE_DONE:
        return "done";
    case APPLIANCE_ACME_TRANSPORT_STATE_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}

const char *appliance_acme_transport_status_string(
    enum appliance_acme_transport_status status)
{
    switch (status) {
    case APPLIANCE_ACME_TRANSPORT_OK:
        return "ok";
    case APPLIANCE_ACME_TRANSPORT_PENDING:
        return "pending";
    case APPLIANCE_ACME_TRANSPORT_NULL:
        return "null";
    case APPLIANCE_ACME_TRANSPORT_BAD_LINK:
        return "bad-link";
    case APPLIANCE_ACME_TRANSPORT_BAD_STATE:
        return "bad-state";
    case APPLIANCE_ACME_TRANSPORT_BUSY:
        return "busy";
    case APPLIANCE_ACME_TRANSPORT_BAD_CLOCK:
        return "bad-clock";
    case APPLIANCE_ACME_TRANSPORT_BAD_URL:
        return "bad-url";
    case APPLIANCE_ACME_TRANSPORT_BAD_REQUEST:
        return "bad-request";
    case APPLIANCE_ACME_TRANSPORT_BAD_REPLY:
        return "bad-reply";
    case APPLIANCE_ACME_TRANSPORT_OPEN_FAILED:
        return "open-failed";
    case APPLIANCE_ACME_TRANSPORT_HANDSHAKE_FAILED:
        return "handshake-failed";
    case APPLIANCE_ACME_TRANSPORT_SEND_FAILED:
        return "send-failed";
    case APPLIANCE_ACME_TRANSPORT_RECV_FAILED:
        return "recv-failed";
    case APPLIANCE_ACME_TRANSPORT_TIMEOUT:
        return "timeout";
    case APPLIANCE_ACME_TRANSPORT_CLOSED:
        return "closed";
    case APPLIANCE_ACME_TRANSPORT_BUFFER_TOO_SMALL:
        return "buffer-too-small";
    default:
        return "unknown";
    }
}
