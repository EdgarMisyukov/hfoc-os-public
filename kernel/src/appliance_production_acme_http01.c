#include "appliance_production_acme_http01.h"

/*
 * ITEM-508 increment 28: the HTTP-01 listener.
 *
 * The decision is increment 7's module; this file only carries it over one
 * bounded TCP connection.  It reads exactly one request line per connection,
 * asks `appliance_acme_challenge_decide_line()` for the answer, writes exactly
 * one response and closes.  Nothing here opens a second path, redirects, reads
 * a file or keeps a token alive past the decision module's deadline.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_HTTP01_HOST_TEST)

#include "hfoc_lwip.h"
#include "hfoc_lwip_time.h"

#include "lwip/err.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char appliance_production_acme_http01_status_ok[] = "HTTP/1.1 200 OK\r\n";
static const char appliance_production_acme_http01_status_bad_request[] =
    "HTTP/1.1 400 Bad Request\r\n";
static const char appliance_production_acme_http01_status_not_found[] =
    "HTTP/1.1 404 Not Found\r\n";
static const char appliance_production_acme_http01_status_method[] =
    "HTTP/1.1 405 Method Not Allowed\r\n";
static const char appliance_production_acme_http01_headers[] =
    "Content-Type: text/plain\r\nConnection: close\r\n";
static const char appliance_production_acme_http01_length_prefix[] =
    "Content-Length: ";
static const char appliance_production_acme_http01_record_prefix[] =
    "[ACME-HTTP01] v=1 state=";

static const char *appliance_production_acme_http01_response_head(
    enum appliance_acme_challenge_status status)
{
    switch (status) {
    case APPLIANCE_ACME_CHALLENGE_OK:
        return appliance_production_acme_http01_status_ok;
    case APPLIANCE_ACME_CHALLENGE_METHOD_NOT_ALLOWED:
        return appliance_production_acme_http01_status_method;
    case APPLIANCE_ACME_CHALLENGE_BAD_REQUEST:
    case APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL:
        return appliance_production_acme_http01_status_bad_request;
    default:
        return appliance_production_acme_http01_status_not_found;
    }
}

static bool appliance_production_acme_http01_append(
    char *out,
    size_t capacity,
    size_t *length,
    const char *text,
    size_t text_length)
{
    if (*length + text_length > capacity) {
        return false;
    }
    if (text_length > 0U) {
        memcpy(&out[*length], text, text_length);
    }
    *length += text_length;
    return true;
}

static bool appliance_production_acme_http01_append_u32(
    char *out,
    size_t capacity,
    size_t *length,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        return appliance_production_acme_http01_append(
            out, capacity, length, "0", 1U);
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        if (!appliance_production_acme_http01_append(
                out, capacity, length, &digits[count], 1U)) {
            return false;
        }
    }
    return true;
}

static bool appliance_production_acme_http01_append_text(
    char *out,
    size_t capacity,
    size_t *length,
    const char *text)
{
    return appliance_production_acme_http01_append(
        out, capacity, length, text, strlen(text));
}

bool appliance_production_acme_http01_build_response(
    enum appliance_acme_challenge_status status,
    bool include_body,
    const char *body,
    size_t body_length,
    char *out,
    size_t capacity,
    size_t *out_length)
{
    size_t length = 0U;
    const char *head;

    if (out == NULL || out_length == NULL || capacity == 0U) {
        return false;
    }
    *out_length = 0U;
    head = appliance_production_acme_http01_response_head(status);
    if (status != APPLIANCE_ACME_CHALLENGE_OK) {
        body = NULL;
        body_length = 0U;
        include_body = false;
    } else if (include_body && (body == NULL || body_length == 0U)) {
        return false;
    }
    if (!appliance_production_acme_http01_append_text(
            out, capacity, &length, head) ||
        !appliance_production_acme_http01_append_text(
            out, capacity, &length, appliance_production_acme_http01_headers) ||
        !appliance_production_acme_http01_append_text(
            out, capacity, &length,
            appliance_production_acme_http01_length_prefix) ||
        !appliance_production_acme_http01_append_u32(
            out, capacity, &length, (uint32_t)body_length) ||
        !appliance_production_acme_http01_append_text(
            out, capacity, &length, "\r\n\r\n")) {
        return false;
    }
    if (include_body && body_length > 0U) {
        if (!appliance_production_acme_http01_append(
                out, capacity, &length, body, body_length)) {
            return false;
        }
    }
    *out_length = length;
    return true;
}

static void appliance_production_acme_http01_release(
    struct appliance_production_acme_http01_connection *connection,
    bool graceful);

static struct appliance_production_acme_http01 *
appliance_production_acme_http01_owner_of(
    struct appliance_production_acme_http01_connection *connection)
{
    return connection != NULL ? connection->owner : NULL;
}

static bool appliance_production_acme_http01_flush(
    struct appliance_production_acme_http01_connection *connection)
{
    struct appliance_production_acme_http01 *http01 =
        appliance_production_acme_http01_owner_of(connection);

    if (connection->pcb == NULL) {
        return true;
    }
    while (connection->response_offset < connection->response_length) {
        const size_t remaining =
            (size_t)(connection->response_length - connection->response_offset);
        const u16_t room = tcp_sndbuf(connection->pcb);
        size_t chunk = remaining;
        err_t error;

        if (room == 0U) {
            return false;
        }
        if ((size_t)room < chunk) {
            chunk = (size_t)room;
        }
        error = tcp_write(
            connection->pcb,
            &connection->response[connection->response_offset],
            (u16_t)chunk,
            TCP_WRITE_FLAG_COPY);
        if (error == ERR_MEM) {
            return false;
        }
        if (error != ERR_OK) {
            if (http01 != NULL) {
                ++http01->write_failure_count;
            }
            appliance_production_acme_http01_release(connection, false);
            return false;
        }
        connection->response_offset += (uint32_t)chunk;
    }
    if (tcp_output(connection->pcb) != ERR_OK) {
        if (http01 != NULL) {
            ++http01->write_failure_count;
        }
        appliance_production_acme_http01_release(connection, false);
        return false;
    }
    return true;
}

static void appliance_production_acme_http01_release(
    struct appliance_production_acme_http01_connection *connection,
    bool graceful)
{
    struct appliance_production_acme_http01 *http01;
    bool aborted = false;

    if (connection == NULL || !connection->active || connection->releasing) {
        return;
    }
    http01 = connection->owner;
    connection->releasing = true;
    if (connection->pcb != NULL) {
        if (graceful && !connection->peer_closed) {
            if (tcp_close(connection->pcb) != ERR_OK) {
                tcp_abort(connection->pcb);
                aborted = true;
            }
        } else {
            tcp_abort(connection->pcb);
            aborted = true;
        }
        connection->pcb = NULL;
    }
    memset(connection->line, 0, sizeof(connection->line));
    memset(connection->response, 0, sizeof(connection->response));
    connection->line_length = 0U;
    connection->response_length = 0U;
    connection->response_offset = 0U;
    connection->closing = false;
    connection->peer_closed = false;
    /*
     * A graceful close must not report ERR_ABRT to the caller: the pcb was
     * closed, not aborted, and lwIP frees it on the FIN path.  Only an abort
     * (or a close that failed and became one) owns the ERR_ABRT return.
     */
    connection->aborted = aborted;
    connection->active = false;
    connection->releasing = false;
    if (http01 != NULL) {
        if (http01->active_count != 0U) {
            --http01->active_count;
        }
        ++http01->close_count;
    }
}

static void appliance_production_acme_http01_respond(
    struct appliance_production_acme_http01_connection *connection,
    enum appliance_acme_challenge_status status,
    bool include_body,
    const char *body,
    size_t body_length)
{
    struct appliance_production_acme_http01 *http01 =
        appliance_production_acme_http01_owner_of(connection);
    size_t length = 0U;

    if (http01 == NULL || connection->pcb == NULL) {
        return;
    }
    if (!appliance_production_acme_http01_build_response(
            status, include_body, body, body_length, connection->response,
            sizeof(connection->response), &length)) {
        ++http01->write_failure_count;
        appliance_production_acme_http01_release(connection, false);
        return;
    }
    connection->response_length = (uint32_t)length;
    connection->response_offset = 0U;
    connection->closing = true;
    if (appliance_production_acme_http01_flush(connection)) {
        appliance_production_acme_http01_release(connection, true);
    }
}

/*
 * ITEM-508 increment 29: the byte count of one request line inside a received
 * segment.  `length` when the segment carries no line ending yet, so the
 * caller keeps the bytes and waits for the rest.
 */
size_t appliance_production_acme_http01_line_bytes(
    const char *bytes, size_t length)
{
    size_t index;

    if (bytes == NULL) {
        return 0U;
    }
    for (index = 0U; index < length; ++index) {
        if (bytes[index] == '\n') {
            return index + 1U;
        }
    }
    return length;
}

static bool appliance_production_acme_http01_line_complete(
    const struct appliance_production_acme_http01_connection *connection)
{
    uint32_t index;

    for (index = 0U; index < connection->line_length; ++index) {
        if (connection->line[index] == '\n') {
            return true;
        }
    }
    return false;
}

static enum appliance_acme_challenge_status
appliance_production_acme_http01_serve_line(
    struct appliance_production_acme_http01_connection *connection)
{
    struct appliance_production_acme_http01 *http01 =
        appliance_production_acme_http01_owner_of(connection);
    struct appliance_acme_challenge_request request;
    enum appliance_acme_challenge_status status;
    const char *body = NULL;
    size_t body_length = 0U;
    bool include_body = false;

    memset(&request, 0, sizeof(request));
    if (appliance_acme_challenge_read_request_line(
            connection->line, (size_t)connection->line_length,
            &request) == APPLIANCE_ACME_CHALLENGE_OK) {
        include_body = !(request.method_length == 4U &&
            memcmp(request.method, "HEAD", 4U) == 0);
    }
    status = appliance_acme_challenge_decide_line(
        &http01->challenge, connection->line,
        (size_t)connection->line_length, http01->now_epoch_seconds, &body,
        &body_length);
    if (status == APPLIANCE_ACME_CHALLENGE_OK && !include_body) {
        /*
         * The decision reports an empty body for HEAD, because HEAD carries
         * none.  The header still has to name the length a GET would return,
         * so the armed answer's own length is used; the wire carries no body.
         */
        body = NULL;
        body_length = (size_t)http01->challenge.answer_length;
    }
    if (status == APPLIANCE_ACME_CHALLENGE_OK) {
        ++http01->served_count;
    } else {
        ++http01->refused_count;
    }
    appliance_production_acme_http01_respond(
        connection, status, include_body, body, body_length);
    return status;
}

static err_t appliance_production_acme_http01_tcp_recv(
    void *context,
    struct tcp_pcb *pcb,
    struct pbuf *packet,
    err_t error)
{
    struct appliance_production_acme_http01_connection *connection = context;
    size_t received;
    size_t available;
    size_t copy;
    size_t line_bytes;
    bool overflow = false;

    (void)pcb;
    (void)error;
    if (connection == NULL || !connection->active) {
        if (packet != NULL) {
            pbuf_free(packet);
        }
        return ERR_OK;
    }
    if (packet == NULL) {
        connection->peer_closed = true;
        appliance_production_acme_http01_release(connection, false);
        return connection->aborted ? ERR_ABRT : ERR_OK;
    }
    received = packet->tot_len;
    available = sizeof(connection->line) - connection->line_length;
    copy = received;
    if (copy > available) {
        copy = available;
        overflow = true;
    }
    if (copy != 0U &&
        pbuf_copy_partial(
            packet, &connection->line[connection->line_length], (u16_t)copy,
            0U) != (u16_t)copy) {
        pbuf_free(packet);
        appliance_production_acme_http01_release(connection, false);
        return ERR_ABRT;
    }
    /*
     * ITEM-508 increment 29: exactly one request line reaches the decision.
     * The connection answers once and closes, so the bytes after the first
     * line ending are not this request's line; a real HTTP/1.1 client sends
     * its header lines in the same segment, and accumulating them put their
     * CRLFs into the line, where the parser refused the request as malformed
     * (`400`) before the armed answer could be reached - measured on the wire
     * proof's first run.
     */
    line_bytes = appliance_production_acme_http01_line_bytes(
        &connection->line[connection->line_length], copy);
    connection->line_length += (uint32_t)line_bytes;
    pbuf_free(packet);
    tcp_recved(pcb, (u16_t)received);
    connection->last_activity_ms = hfoc_lwip_time_now();
    if (appliance_production_acme_http01_line_complete(connection)) {
        (void)appliance_production_acme_http01_serve_line(connection);
        return connection->aborted ? ERR_ABRT : ERR_OK;
    }
    /*
     * No line ending in what fits: a segment the line buffer could not take
     * whole, or an exactly full line, is the named refusal - never a partial
     * line kept for the next segment, which would be parsed as a request the
     * client did not send.
     */
    if (overflow || connection->line_length == sizeof(connection->line)) {
        if (connection->owner != NULL) {
            ++connection->owner->refused_count;
        }
        appliance_production_acme_http01_respond(
            connection, APPLIANCE_ACME_CHALLENGE_BUFFER_TOO_SMALL, false,
            NULL, 0U);
        return connection->aborted ? ERR_ABRT : ERR_OK;
    }
    return ERR_OK;
}

static err_t appliance_production_acme_http01_tcp_sent(
    void *context,
    struct tcp_pcb *pcb,
    u16_t length)
{
    struct appliance_production_acme_http01_connection *connection = context;

    (void)pcb;
    (void)length;
    if (connection != NULL && connection->active && connection->closing) {
        if (appliance_production_acme_http01_flush(connection)) {
            appliance_production_acme_http01_release(connection, true);
        }
    }
    return ERR_OK;
}

static err_t appliance_production_acme_http01_tcp_poll(
    void *context,
    struct tcp_pcb *pcb)
{
    struct appliance_production_acme_http01_connection *connection = context;

    (void)pcb;
    if (connection != NULL && connection->active &&
        (uint32_t)(hfoc_lwip_time_now() - connection->last_activity_ms) >=
            APPLIANCE_PRODUCTION_ACME_HTTP01_IDLE_TIMEOUT_MS) {
        appliance_production_acme_http01_release(connection, false);
    }
    return ERR_OK;
}

static void appliance_production_acme_http01_tcp_error(
    void *context,
    err_t error)
{
    struct appliance_production_acme_http01_connection *connection = context;

    (void)error;
    if (connection != NULL) {
        appliance_production_acme_http01_release(connection, false);
    }
}

static err_t appliance_production_acme_http01_accept(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    struct appliance_production_acme_http01 *http01 = context;
    struct appliance_production_acme_http01_connection *connection = NULL;
    uint32_t index;

    (void)generation;
    if (http01 == NULL || pcb == NULL || http01->state !=
        APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING) {
        if (pcb != NULL) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    if (http01->active_count >=
        APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT;
         ++index) {
        if (!http01->connections[index].active) {
            connection = &http01->connections[index];
            break;
        }
    }
    if (connection == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    memset(connection, 0, sizeof(*connection));
    connection->owner = http01;
    connection->active = true;
    connection->pcb = pcb;
    connection->last_activity_ms = hfoc_lwip_time_now();
    tcp_arg(pcb, connection);
    tcp_recv(pcb, appliance_production_acme_http01_tcp_recv);
    tcp_sent(pcb, appliance_production_acme_http01_tcp_sent);
    tcp_poll(pcb, appliance_production_acme_http01_tcp_poll, 1U);
    tcp_err(pcb, appliance_production_acme_http01_tcp_error);
    ++http01->active_count;
    ++http01->accept_count;
    return ERR_OK;
}

/* ------------------------------------------------------------------- record */

static void appliance_production_acme_http01_append_record_text(
    char *out,
    size_t capacity,
    size_t *length,
    const char *text)
{
    const size_t text_length = strlen(text);

    if (*length + text_length > capacity) {
        return;
    }
    memcpy(&out[*length], text, text_length);
    *length += text_length;
    out[*length] = '\0';
}

static void appliance_production_acme_http01_append_record_u32(
    char *out,
    size_t capacity,
    size_t *length,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        appliance_production_acme_http01_append_record_text(
            out, capacity, length, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char one[2];

        --count;
        one[0] = digits[count];
        one[1] = '\0';
        appliance_production_acme_http01_append_record_text(
            out, capacity, length, one);
    }
}

const char *appliance_production_acme_http01_state_string(
    enum appliance_production_acme_http01_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING:
        return "LISTENING";
    case APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED:
        return "FAILED";
    case APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_UNINITIALIZED:
    default:
        return "UNINITIALIZED";
    }
}

const char *appliance_production_acme_http01_record(
    struct appliance_production_acme_http01 *http01)
{
    size_t length = 0U;

    if (http01 == NULL) {
        return NULL;
    }
    http01->record[0] = '\0';
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length,
        appliance_production_acme_http01_record_prefix);
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length,
        appliance_production_acme_http01_state_string(http01->state));
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length,
        http01->state == APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED
            ? " reason="
            : " armed=");
    if (http01->state == APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED) {
        appliance_production_acme_http01_append_record_text(
            http01->record, sizeof(http01->record), &length,
            http01->reason != NULL ? http01->reason : "none");
    } else {
        appliance_production_acme_http01_append_record_text(
            http01->record, sizeof(http01->record), &length,
            appliance_production_acme_http01_armed(http01) ? "1" : "0");
    }
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length, " port=");
    appliance_production_acme_http01_append_record_u32(
        http01->record, sizeof(http01->record), &length,
        APPLIANCE_PRODUCTION_ACME_HTTP01_PORT);
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length, " accepted=");
    appliance_production_acme_http01_append_record_u32(
        http01->record, sizeof(http01->record), &length, http01->accept_count);
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length, " served=");
    appliance_production_acme_http01_append_record_u32(
        http01->record, sizeof(http01->record), &length, http01->served_count);
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length, " refused=");
    appliance_production_acme_http01_append_record_u32(
        http01->record, sizeof(http01->record), &length, http01->refused_count);
    appliance_production_acme_http01_append_record_text(
        http01->record, sizeof(http01->record), &length, " closed=");
    appliance_production_acme_http01_append_record_u32(
        http01->record, sizeof(http01->record), &length, http01->close_count);
    return http01->record;
}

/* --------------------------------------------------------------------- API */

enum appliance_production_acme_http01_state
appliance_production_acme_http01_init(
    struct appliance_production_acme_http01 *http01,
    const struct hfoc_lwip_netif *live_netif,
    const struct network_ipv4_config *config)
{
    struct network_ipv4_port_policy port_policy;
    enum hfoc_lwip_tcp_status listener_status;

    if (http01 == NULL) {
        return APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED;
    }
    memset(http01, 0, sizeof(*http01));
    http01->version = APPLIANCE_PRODUCTION_ACME_HTTP01_VERSION;
    http01->state = APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED;
    http01->reason = "null";
    if (live_netif == NULL || config == NULL) {
        return http01->state;
    }
    memset(&port_policy, 0, sizeof(port_policy));
    port_policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    port_policy.port = APPLIANCE_PRODUCTION_ACME_HTTP01_PORT;
    memset(&http01->listener_callback, 0, sizeof(http01->listener_callback));
    http01->listener_callback.accept = appliance_production_acme_http01_accept;
    http01->listener_callback.context = http01;
    listener_status = hfoc_lwip_tcp_init(
        &http01->listener, live_netif, config, &port_policy,
        &http01->listener_callback, APPLIANCE_PRODUCTION_ACME_HTTP01_GENERATION);
    if (listener_status != HFOC_LWIP_TCP_OK) {
        http01->reason = "listener_init_failed";
        return http01->state;
    }
    listener_status = hfoc_lwip_tcp_bind(&http01->listener, &port_policy);
    if (listener_status != HFOC_LWIP_TCP_OK) {
        http01->reason = "listener_bind_failed";
        return http01->state;
    }
    listener_status = hfoc_lwip_tcp_listen(&http01->listener);
    if (listener_status != HFOC_LWIP_TCP_OK &&
        listener_status != HFOC_LWIP_TCP_ALREADY_LISTENING) {
        http01->reason = "listener_listen_failed";
        return http01->state;
    }
    http01->state = APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING;
    http01->reason = "none";
    return http01->state;
}

enum appliance_acme_challenge_status appliance_production_acme_http01_arm(
    struct appliance_production_acme_http01 *http01,
    const char *token,
    size_t token_length,
    const char *key_authorization,
    size_t key_authorization_length,
    uint32_t now_epoch_seconds)
{
    if (http01 == NULL) {
        return APPLIANCE_ACME_CHALLENGE_NULL;
    }
    if (http01->state != APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING) {
        return APPLIANCE_ACME_CHALLENGE_BAD_STATE;
    }
    http01->now_epoch_seconds = now_epoch_seconds;
    return appliance_acme_challenge_arm_token(
        &http01->challenge, token, token_length, key_authorization,
        key_authorization_length, now_epoch_seconds);
}

void appliance_production_acme_http01_disarm(
    struct appliance_production_acme_http01 *http01)
{
    uint32_t index;

    if (http01 == NULL) {
        return;
    }
    appliance_acme_challenge_disarm(&http01->challenge);
    for (index = 0U;
         index < APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT;
         ++index) {
        if (http01->connections[index].active) {
            appliance_production_acme_http01_release(
                &http01->connections[index], false);
        }
    }
}

void appliance_production_acme_http01_poll(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_epoch_seconds)
{
    if (http01 == NULL) {
        return;
    }
    http01->now_epoch_seconds = now_epoch_seconds;
}

bool appliance_production_acme_http01_listening(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL &&
        http01->state == APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING &&
        hfoc_lwip_tcp_listening(&http01->listener);
}

bool appliance_production_acme_http01_armed(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL && http01->challenge.armed;
}

enum appliance_production_acme_http01_state
appliance_production_acme_http01_state(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->state
                          : APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_UNINITIALIZED;
}

const char *appliance_production_acme_http01_reason(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->reason : "null";
}

uint32_t appliance_production_acme_http01_port(void)
{
    return APPLIANCE_PRODUCTION_ACME_HTTP01_PORT;
}

uint32_t appliance_production_acme_http01_accept_count(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->accept_count : 0U;
}

uint32_t appliance_production_acme_http01_served_count(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->served_count : 0U;
}

uint32_t appliance_production_acme_http01_refused_count(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->refused_count : 0U;
}

uint32_t appliance_production_acme_http01_active_count(
    const struct appliance_production_acme_http01 *http01)
{
    return http01 != NULL ? http01->active_count : 0U;
}

#else
typedef int appliance_production_acme_http01_translation_unit_guard;
#endif
