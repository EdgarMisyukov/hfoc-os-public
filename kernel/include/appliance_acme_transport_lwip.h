#ifndef HFOC_APPLIANCE_ACME_TRANSPORT_LWIP_H
#define HFOC_APPLIANCE_ACME_TRANSPORT_LWIP_H


/*
 * The binding exists only in the TLS-serving profile: every other kernel
 * build (the fallback and drill kernels) compiles the wildcard sources
 * without the mbedTLS include path, so this header is deliberately empty
 * there instead of leaking an unavailable dependency.
 */
#ifdef HFOC_PRODUCTION_TLS

#include "appliance_acme_transport.h"
#include "appliance_network_lifecycle.h"
#include "hfoc_mbedtls_bio.h"

#include "lwip/tcp.h"

#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 13: the lwIP and mbedTLS binding of the transport link.
 *
 * `appliance_acme_transport` owns every decision of one ACME HTTP exchange:
 * the clock, the deadlines, the request, the reply and the refusals.  This
 * module is the other half the driver calls through - it moves bytes over a
 * real connection and a real TLS 1.3 client handshake, and it decides nothing
 * else.  Each of the five link operations maps the underlying result to one
 * link status:
 *
 *   * `open` resolves a name through the live network lifecycle's resolver
 *     (never a second resolver) or parses an IPv4 literal, then starts the
 *     connection;
 *   * `advance` drives the resolver, the connection and the client handshake,
 *     and reports `OK` only when the handshake verified the peer;
 *   * `send`/`recv` are one TLS record in and one TLS record out: a full
 *     record is never waited for, and a short result is reported as such;
 *   * `close` ends the exchange and never fails.
 *
 * The peer is verified against exactly one anchor, handed in by the caller
 * (`appliance_acme_trust_anchor_der_data()` in production, a fixture in a
 * proof); there is no other trust input.  The mbedTLS connection arena is the
 * shared one, and this module enters a slot for the lifetime of one exchange,
 * so an ACME order and an inbound management connection cannot silently share
 * or exhaust the arena's accounting.
 *
 * The binding publishes no record of its own: the driver above it already
 * records the exchange, and a second record would duplicate a decision this
 * module does not own.  Refusals are readable through `last_status`.
 */

#define APPLIANCE_ACME_TRANSPORT_LWIP_VERSION 1U
#define APPLIANCE_ACME_TRANSPORT_LWIP_HOST_MAX 253U
/*
 * The receive staging buffer.  lwIP hands the callback one delivered pbuf at a
 * time and the QEMU user-network link delivers segments the stack did not
 * choose, so the buffer is a full receive window plus one segment - the same
 * bound the facade relay uses, and larger than the BIO's per-call maximum.
 */
#define APPLIANCE_ACME_TRANSPORT_LWIP_RX_MAX (TCP_WND + TCP_MSS)

enum appliance_acme_transport_lwip_phase {
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_IDLE = 0,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_RESOLVING = 1,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING = 2,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS = 3,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY = 4,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED = 5,
    APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_FAILED = 6,
};

enum appliance_acme_transport_lwip_status {
    APPLIANCE_ACME_TRANSPORT_LWIP_OK = 0,
    APPLIANCE_ACME_TRANSPORT_LWIP_NULL = 1,
    APPLIANCE_ACME_TRANSPORT_LWIP_BAD_STATE = 2,
    APPLIANCE_ACME_TRANSPORT_LWIP_BUSY = 3,
    /* No published network lifecycle, or one without a usable resolver. */
    APPLIANCE_ACME_TRANSPORT_LWIP_NO_NETWORK = 4,
    APPLIANCE_ACME_TRANSPORT_LWIP_BAD_HOST = 5,
    /* The named anchor is missing or does not parse. */
    APPLIANCE_ACME_TRANSPORT_LWIP_ANCHOR_REFUSED = 6,
    APPLIANCE_ACME_TRANSPORT_LWIP_ALLOCATOR_REFUSED = 7,
    APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED = 8,
    /* The resolver refused the name, or answered with a failure. */
    APPLIANCE_ACME_TRANSPORT_LWIP_RESOLVE_FAILED = 9,
    APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED = 10,
    APPLIANCE_ACME_TRANSPORT_LWIP_HANDSHAKE_FAILED = 11,
    APPLIANCE_ACME_TRANSPORT_LWIP_SEND_FAILED = 12,
    APPLIANCE_ACME_TRANSPORT_LWIP_RECV_FAILED = 13,
    /* The peer sent more than the staging buffer holds, or the request is
     * larger than one TLS record and the caller asked for it whole. */
    APPLIANCE_ACME_TRANSPORT_LWIP_OVERFLOW = 14,
    APPLIANCE_ACME_TRANSPORT_LWIP_PEER_CLOSED = 15,
};

struct appliance_acme_transport_lwip {
    uint32_t version;
    bool published;
    struct appliance_acme_transport_link link;
    struct appliance_network_lifecycle *lifecycle;
    enum appliance_acme_transport_lwip_phase phase;
    enum appliance_acme_transport_lwip_status last_status;
    /*
     * ITEM-508 increment 15: the mbedTLS error behind the last refusal of the
     * handshake, kept so the driver's record can name it.  Zero means the
     * refusal carried no TLS error of its own; the value is an mbedTLS error
     * code, not a status of this module.
     */
    int last_tls_error;

    mbedtls_x509_crt anchor;
    bool anchor_parsed;
    mbedtls_ssl_config ssl_config;
    bool config_published;

    struct hfoc_mbedtls_bio bio;
    bool bio_initialized;
    bool bio_attached;
    struct hfoc_mbedtls_bio_policy bio_policy;
    struct hfoc_mbedtls_bio_token bio_token;
    uint32_t allocator_id;

    struct tcp_pcb *pcb;
    bool pcb_owned;
    bool connected;
    bool peer_closed;
    bool rx_overflow;
    uint8_t rx[APPLIANCE_ACME_TRANSPORT_LWIP_RX_MAX];
    size_t rx_length;
    size_t rx_offset;

    char host[APPLIANCE_ACME_TRANSPORT_LWIP_HOST_MAX + 1U];
    size_t host_length;
    uint16_t port;
    bool literal_ipv4;

    mbedtls_ssl_context ssl;
    bool ssl_setup;

    uint32_t tcp_connect_count;
    uint32_t handshake_steps;
    uint32_t send_calls;
    uint32_t recv_calls;
    uint32_t failure_count;
};

/*
 * Publish a binding over the live network lifecycle and the named anchor.
 * The lifecycle may be NULL only for a proof that never uses a name; a name
 * then fails closed.  The anchor is parsed once here, so an anchor the frozen
 * profile cannot parse is refused at publication and never at handshake time.
 */
enum appliance_acme_transport_lwip_status appliance_acme_transport_lwip_init(
    struct appliance_acme_transport_lwip *binding,
    struct appliance_network_lifecycle *lifecycle,
    const unsigned char *anchor_der,
    size_t anchor_length);

/* The link interface the driver is given; `context` is the binding. */
const struct appliance_acme_transport_link *
appliance_acme_transport_lwip_link(
    struct appliance_acme_transport_lwip *binding);

enum appliance_acme_transport_lwip_status
appliance_acme_transport_lwip_last_status(
    const struct appliance_acme_transport_lwip *binding);
/* The mbedTLS error behind the last handshake refusal; 0 when there was none. */
int appliance_acme_transport_lwip_last_tls_error(
    const struct appliance_acme_transport_lwip *binding);
enum appliance_acme_transport_lwip_phase appliance_acme_transport_lwip_phase(
    const struct appliance_acme_transport_lwip *binding);
bool appliance_acme_transport_lwip_published(
    const struct appliance_acme_transport_lwip *binding);

const char *appliance_acme_transport_lwip_phase_string(
    enum appliance_acme_transport_lwip_phase phase);
const char *appliance_acme_transport_lwip_status_string(
    enum appliance_acme_transport_lwip_status status);

#endif /* HFOC_PRODUCTION_TLS */

#endif
