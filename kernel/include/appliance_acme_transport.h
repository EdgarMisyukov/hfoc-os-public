#ifndef HFOC_APPLIANCE_ACME_TRANSPORT_H
#define HFOC_APPLIANCE_ACME_TRANSPORT_H

#include "appliance_acme_http.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 11: one ACME HTTP exchange over a link.
 *
 * The order module produces one request at a time and consumes one reply at a
 * time; `appliance_acme_http` owns the bytes.  Between them sits the exchange
 * this module owns: it opens the link, drives the connection and the TLS
 * handshake to completion, sends the exact request, reads the reply whole and
 * proves the reply before handing it over.  The link itself is injected, so
 * every decision here - the clock refusal, the deadlines, the send and receive
 * boundaries, the refusals and the one bounded record - is provable on the
 * host with a scripted peer, and the lwIP and mbedTLS binding above it has
 * nothing left to decide.
 *
 * The rules the module keeps:
 *
 *   * nothing starts on an unset clock: `begin` refuses `now_ms == 0` before
 *     the link is touched, and a clock that runs backwards is a refusal, not a
 *     negative remainder;
 *   * the connection and the TLS handshake share one bounded window, the
 *     reply has its own; a window that closes is `TIMEOUT` with the record,
 *     never an open-ended wait;
 *   * the request is built by `appliance_acme_http` and sent whole - a short
 *     send is continued, never dropped - and no byte leaves before the link
 *     reports the handshake complete;
 *   * the reply is parsed only when the module has it whole: a body framed by
 *     `Content-Length` is complete when that many bytes arrived, a body framed
 *     by the connection is complete only when the link reports the peer
 *     closed, and a truncated reply is a refusal by name, never a shorter
 *     answer;
 *   * a refusal names whether it was the link, the request or the reply, and
 *     the reply's own `appliance_acme_http_status` is kept beside it;
 *   * one record, `[ACME-TRANSPORT] outcome=… status=… request=… reply=…`,
 *     carries no host, no URL query, no body, no nonce and no key.
 */

#define APPLIANCE_ACME_TRANSPORT_VERSION 1U
#define APPLIANCE_ACME_TRANSPORT_CONNECT_DEADLINE_MS 30000U
#define APPLIANCE_ACME_TRANSPORT_REPLY_DEADLINE_MS 60000U
#define APPLIANCE_ACME_TRANSPORT_SEND_BUDGET 16U
#define APPLIANCE_ACME_TRANSPORT_RECV_BUDGET 16U
#define APPLIANCE_ACME_TRANSPORT_RECORD_MAX 160U

enum appliance_acme_transport_state {
    APPLIANCE_ACME_TRANSPORT_STATE_FREE = 0,
    /* The link is open; the connection and the TLS handshake are in flight. */
    APPLIANCE_ACME_TRANSPORT_STATE_CONNECTING = 1,
    APPLIANCE_ACME_TRANSPORT_STATE_SENDING = 2,
    APPLIANCE_ACME_TRANSPORT_STATE_READING = 3,
    APPLIANCE_ACME_TRANSPORT_STATE_DONE = 4,
    APPLIANCE_ACME_TRANSPORT_STATE_FAILED = 5,
};

/*
 * What the link reports.  `WANT_READ`/`WANT_WRITE` mean "no progress this
 * call, call again when the socket or the TLS state says so"; they are never
 * errors.  `CLOSED` is the peer's end of the stream, which is framing, not a
 * failure, until a reply is known to be incomplete.
 */
enum appliance_acme_transport_link_status {
    APPLIANCE_ACME_TRANSPORT_LINK_OK = 0,
    APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ = 1,
    APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE = 2,
    APPLIANCE_ACME_TRANSPORT_LINK_CLOSED = 3,
    APPLIANCE_ACME_TRANSPORT_LINK_FAILED = 4,
};

enum appliance_acme_transport_status {
    APPLIANCE_ACME_TRANSPORT_OK = 0,
    /* The exchange is not finished; call `advance` again. */
    APPLIANCE_ACME_TRANSPORT_PENDING = 1,
    APPLIANCE_ACME_TRANSPORT_NULL = 2,
    /* An operation of the link interface is missing. */
    APPLIANCE_ACME_TRANSPORT_BAD_LINK = 3,
    APPLIANCE_ACME_TRANSPORT_BAD_STATE = 4,
    APPLIANCE_ACME_TRANSPORT_BUSY = 5,
    APPLIANCE_ACME_TRANSPORT_BAD_CLOCK = 6,
    /* The URL was refused; `http_status` names it exactly. */
    APPLIANCE_ACME_TRANSPORT_BAD_URL = 7,
    /* The request could not be built; `http_status` names it exactly. */
    APPLIANCE_ACME_TRANSPORT_BAD_REQUEST = 8,
    /* The reply was refused; `http_status` names it exactly. */
    APPLIANCE_ACME_TRANSPORT_BAD_REPLY = 9,
    APPLIANCE_ACME_TRANSPORT_OPEN_FAILED = 10,
    APPLIANCE_ACME_TRANSPORT_HANDSHAKE_FAILED = 11,
    APPLIANCE_ACME_TRANSPORT_SEND_FAILED = 12,
    APPLIANCE_ACME_TRANSPORT_RECV_FAILED = 13,
    APPLIANCE_ACME_TRANSPORT_TIMEOUT = 14,
    /* The peer closed before a complete reply arrived. */
    APPLIANCE_ACME_TRANSPORT_CLOSED = 15,
    APPLIANCE_ACME_TRANSPORT_BUFFER_TOO_SMALL = 16,
};

struct appliance_acme_transport_link {
    void *context;
    /* Start the connection; the handshake is driven by `advance` afterwards. */
    enum appliance_acme_transport_link_status (*open)(
        void *context,
        const char *host,
        uint16_t port,
        bool literal_ipv4);
    /* Drive the connection and the TLS handshake; `OK` means TLS is ready. */
    enum appliance_acme_transport_link_status (*advance)(void *context);
    enum appliance_acme_transport_link_status (*send)(
        void *context,
        const uint8_t *bytes,
        size_t length,
        size_t *bytes_accepted);
    enum appliance_acme_transport_link_status (*recv)(
        void *context,
        uint8_t *bytes,
        size_t capacity,
        size_t *bytes_received);
    void (*close)(void *context);
};

struct appliance_acme_transport {
    uint32_t version;
    enum appliance_acme_transport_state state;
    enum appliance_acme_transport_status last_status;
    /* The reply parser's own refusal, when the refusal came from the bytes. */
    enum appliance_acme_http_status http_status;
    struct appliance_acme_transport_link link;
    bool link_attached;
    struct appliance_acme_http_target target;
    char request[APPLIANCE_ACME_HTTP_REQUEST_MAX];
    size_t request_length;
    size_t request_offset;
    uint8_t reply[APPLIANCE_ACME_HTTP_RESPONSE_MAX];
    size_t reply_length;
    struct appliance_acme_http_response response;
    uint32_t deadline_ms;
    uint32_t last_now_ms;
    uint32_t handshake_steps;
    uint32_t send_calls;
    uint32_t recv_calls;
    char record[APPLIANCE_ACME_TRANSPORT_RECORD_MAX + 1U];
};

/* Publish a transport over a complete link interface. */
enum appliance_acme_transport_status appliance_acme_transport_init(
    struct appliance_acme_transport *transport,
    const struct appliance_acme_transport_link *link);

/*
 * Start one exchange: parse the URL, build the request and open the link.
 * The clock must be set (`now_ms != 0`), and the state must be `FREE`, `DONE`
 * or `FAILED`; anything else is `BUSY`.  Nothing is sent here - the first
 * `advance` drives the connection, the handshake and then the request.
 */
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
    uint32_t now_ms);

/*
 * Drive the exchange as far as the link allows.  Returns `OK` when the reply
 * is complete, `PENDING` when the link wants another turn, or a named refusal;
 * a refusal closes the link exactly once.
 */
enum appliance_acme_transport_status appliance_acme_transport_advance(
    struct appliance_acme_transport *transport,
    uint32_t now_ms);

/*
 * Copy the parsed reply out.  Only a `DONE` transport has one; a transport
 * that refused has nothing to hand over.
 */
enum appliance_acme_transport_status appliance_acme_transport_take_response(
    struct appliance_acme_transport *transport,
    struct appliance_acme_http_response *response_out);

/* Close the link if it is still open; idempotent, and never fails. */
void appliance_acme_transport_close(
    struct appliance_acme_transport *transport);

bool appliance_acme_transport_published(
    const struct appliance_acme_transport *transport);

const char *appliance_acme_transport_state_string(
    enum appliance_acme_transport_state state);
const char *appliance_acme_transport_status_string(
    enum appliance_acme_transport_status status);

#endif
