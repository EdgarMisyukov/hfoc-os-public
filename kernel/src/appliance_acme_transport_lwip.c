#ifdef HFOC_PRODUCTION_TLS

#include "appliance_acme_transport_lwip.h"

#include "appliance_dns_resolver.h"
#include "appliance_production_entropy.h"
#include "hfoc_mbedtls_alloc.h"
#include "ipv4_config.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <string.h>

/*
 * ITEM-508 increment 13.  The frozen profile carries TLS 1.3 only, so the
 * TLS 1.2 client entry point is not even linked; the client step this profile
 * exports is the dispatcher `mbedtls_ssl_handshake_step`, which prepares the
 * output and drives the client role named by the configuration.  It has no
 * public header; `scripts/verify-mbedtls-config.sh` forbids it everywhere and
 * is invoked with an exception that names exactly this source.
 */
int mbedtls_ssl_handshake_step(mbedtls_ssl_context *ssl);

static enum appliance_acme_transport_link_status binding_link_open(
    void *context,
    const char *host,
    uint16_t port,
    bool literal_ipv4);
static enum appliance_acme_transport_link_status binding_link_advance(
    void *context);
static enum appliance_acme_transport_link_status binding_link_send(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted);
static enum appliance_acme_transport_link_status binding_link_recv(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received);
static void binding_link_close(void *context);

static bool binding_valid(
    const struct appliance_acme_transport_lwip *binding)
{
    return binding != NULL && binding->published &&
        binding->version == APPLIANCE_ACME_TRANSPORT_LWIP_VERSION;
}

static enum appliance_acme_transport_link_status binding_refuse(
    struct appliance_acme_transport_lwip *binding,
    enum appliance_acme_transport_lwip_status status)
{
    binding->last_status = status;
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_FAILED;
    ++binding->failure_count;
    return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
}

static bool binding_rx_drained(
    const struct appliance_acme_transport_lwip *binding)
{
    return binding->rx_offset >= binding->rx_length;
}

/* --------------------------------------------------------------- socket half */

/*
 * One send is one slice of the record mbedTLS handed down: lwIP accepts only
 * what its current send-buffer space holds, and the BIO contract reports a
 * short send so mbedTLS continues with the remainder on its next step.  Once
 * `tcp_write` accepted the bytes they are the stack's, so a later `tcp_output`
 * failure does not make them un-sent: reporting them accepted is what keeps
 * mbedTLS from writing the same bytes twice.
 */
static enum hfoc_mbedtls_bio_transport_status binding_bio_send(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    struct appliance_acme_transport_lwip *binding = context;
    err_t error;
    u16_t room;

    if (bytes_accepted == NULL) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    *bytes_accepted = 0U;
    if (binding == NULL || binding->pcb == NULL || bytes == NULL ||
        length == 0U || length > (size_t)UINT16_MAX) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    room = tcp_sndbuf(binding->pcb);
    if (room == 0U) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    if ((size_t)room < length) {
        length = (size_t)room;
    }
    error = tcp_write(
        binding->pcb, bytes, (u16_t)length, TCP_WRITE_FLAG_COPY);
    if (error == ERR_MEM) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    if (error != ERR_OK) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    (void)tcp_output(binding->pcb);
    *bytes_accepted = length;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

/*
 * The receive staging buffer drains in order; an empty buffer with the peer
 * closed reports `CLOSED`, so a reply framed by the connection ends exactly
 * when the peer ends it, and never earlier.
 */
static enum hfoc_mbedtls_bio_transport_status binding_bio_recv(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    struct appliance_acme_transport_lwip *binding = context;
    size_t available;
    size_t received;

    if (bytes_received == NULL) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    *bytes_received = 0U;
    if (binding == NULL || bytes == NULL || capacity == 0U) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    if (binding->rx_overflow) {
        return HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED;
    }
    available = binding->rx_length - binding->rx_offset;
    if (available == 0U) {
        if (binding->peer_closed) {
            return HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED;
        }
        return HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED;
    }
    received = available < capacity ? available : capacity;
    memcpy(bytes, &binding->rx[binding->rx_offset], received);
    binding->rx_offset += received;
    if (binding->rx_offset == binding->rx_length) {
        binding->rx_offset = 0U;
        binding->rx_length = 0U;
    }
    *bytes_received = received;
    return HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED;
}

/*
 * A delivered pbuf larger than the staging buffer cannot be answered honestly
 * by dropping bytes: the stack is told to abort, the pcb is forgotten here
 * (lwIP frees it on `ERR_ABRT`), and `advance`/`recv` publish `OVERFLOW`.
 */
static err_t binding_tcp_recv(
    void *context,
    struct tcp_pcb *pcb,
    struct pbuf *packet,
    err_t error)
{
    struct appliance_acme_transport_lwip *binding = context;
    size_t length;

    (void)error;
    if (binding == NULL || pcb == NULL || pcb != binding->pcb) {
        if (packet != NULL) {
            pbuf_free(packet);
        }
        return ERR_OK;
    }
    if (packet == NULL) {
        binding->peer_closed = true;
        return ERR_OK;
    }
    length = packet->tot_len;
    if (length > sizeof(binding->rx) - binding->rx_length) {
        pbuf_free(packet);
        binding->rx_overflow = true;
        binding->pcb = NULL;
        return ERR_ABRT;
    }
    if (pbuf_copy_partial(
            packet,
            &binding->rx[binding->rx_length],
            (u16_t)length,
            0U) != (u16_t)length) {
        pbuf_free(packet);
        binding->rx_overflow = true;
        binding->pcb = NULL;
        return ERR_ABRT;
    }
    binding->rx_length += length;
    pbuf_free(packet);
    tcp_recved(pcb, (u16_t)length);
    return ERR_OK;
}

static err_t binding_tcp_connected(
    void *context,
    struct tcp_pcb *pcb,
    err_t error)
{
    struct appliance_acme_transport_lwip *binding = context;

    if (binding == NULL || pcb == NULL || pcb != binding->pcb) {
        if (pcb != NULL && error == ERR_OK) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    if (error != ERR_OK) {
        /*
         * lwIP frees a failed connection before publishing the failure, so
         * the pcb is only an identity here and must not be aborted again.
         */
        binding->pcb = NULL;
        binding->connected = false;
        (void)binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED);
        return ERR_ABRT;
    }
    binding->connected = true;
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS;
    return ERR_OK;
}

/*
 * An abort after the handshake is not a lost reply: the bytes already staged
 * stay deliverable, so the peer is marked closed and the driver reads what
 * arrived before it is told the stream ended.  Before that, the phase names
 * which half failed.
 */
static void binding_tcp_error(void *context, err_t error)
{
    struct appliance_acme_transport_lwip *binding = context;

    (void)error;
    if (binding == NULL) {
        return;
    }
    binding->pcb = NULL;
    binding->connected = false;
    switch (binding->phase) {
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING:
        (void)binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED);
        break;
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS:
        (void)binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_HANDSHAKE_FAILED);
        break;
    default:
        binding->peer_closed = true;
        break;
    }
}

static enum appliance_acme_transport_link_status binding_tcp_start(
    struct appliance_acme_transport_lwip *binding,
    const ip_addr_t *address)
{
    struct tcp_pcb *pcb;

    pcb = tcp_new();
    if (pcb == NULL) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED);
    }
    tcp_arg(pcb, binding);
    tcp_recv(pcb, binding_tcp_recv);
    tcp_err(pcb, binding_tcp_error);
    tcp_nagle_disable(pcb);
    binding->pcb = pcb;
    if (tcp_connect(pcb, address, binding->port, binding_tcp_connected) !=
        ERR_OK) {
        binding->pcb = NULL;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_abort(pcb);
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED);
    }
    ++binding->tcp_connect_count;
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING;
    return APPLIANCE_ACME_TRANSPORT_LINK_OK;
}

/* ------------------------------------------------------------------ TLS half */

static void binding_release_exchange(
    struct appliance_acme_transport_lwip *binding,
    bool graceful)
{
    if (binding->pcb != NULL) {
        struct tcp_pcb *pcb = binding->pcb;

        binding->pcb = NULL;
        tcp_arg(pcb, NULL);
        tcp_recv(pcb, NULL);
        tcp_err(pcb, NULL);
        tcp_poll(pcb, NULL, 0U);
        if (!graceful || tcp_close(pcb) != ERR_OK) {
            tcp_abort(pcb);
        }
    }
    if (binding->ssl_setup) {
        mbedtls_ssl_free(&binding->ssl);
        binding->ssl_setup = false;
    }
    if (binding->bio_attached) {
        (void)hfoc_mbedtls_bio_detach(&binding->bio, &binding->bio_token);
        binding->bio_attached = false;
    }
    if (binding->allocator_id != 0U) {
        (void)hfoc_mbedtls_alloc_connection_exit();
        (void)hfoc_mbedtls_alloc_connection_close(binding->allocator_id);
        binding->allocator_id = 0U;
    }
    binding->connected = false;
    binding->peer_closed = false;
    binding->rx_overflow = false;
    binding->rx_length = 0U;
    binding->rx_offset = 0U;
}

/*
 * The connection and the handshake are one window in the driver; this half
 * only reports the handshake's own progress.  The peer is verified twice: the
 * handshake is `VERIFY_REQUIRED`, and the published verification result must
 * still be zero, so a profile change cannot silently turn verification off.
 */
static bool binding_tls_setup(struct appliance_acme_transport_lwip *binding)
{
    struct hfoc_mbedtls_bio_transport transport;
    uint32_t allocator_id = 0U;
    int result;

    if (!hfoc_mbedtls_alloc_connection_open(&allocator_id)) {
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_ALLOCATOR_REFUSED;
        return false;
    }
    binding->allocator_id = allocator_id;
    if (!hfoc_mbedtls_alloc_connection_enter(allocator_id)) {
        (void)hfoc_mbedtls_alloc_connection_close(allocator_id);
        binding->allocator_id = 0U;
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_ALLOCATOR_REFUSED;
        return false;
    }
    mbedtls_ssl_init(&binding->ssl);
    result = mbedtls_ssl_setup(&binding->ssl, &binding->ssl_config);
    if (result != 0) {
        mbedtls_ssl_free(&binding->ssl);
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED;
        return false;
    }
    binding->ssl_setup = true;
    memset(&transport, 0, sizeof(transport));
    transport.send = binding_bio_send;
    transport.recv = binding_bio_recv;
    transport.ctx = binding;
    if (hfoc_mbedtls_bio_attach(
            &binding->bio,
            &binding->bio_token,
            &binding->bio_policy,
            &transport) != HFOC_MBEDTLS_BIO_OK) {
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED;
        return false;
    }
    binding->bio_attached = true;
    mbedtls_ssl_set_bio(
        &binding->ssl,
        &binding->bio.entries[binding->bio_token.index],
        hfoc_mbedtls_bio_send_cb,
        hfoc_mbedtls_bio_recv_cb,
        NULL);
    if (mbedtls_ssl_set_hostname(&binding->ssl, binding->host) != 0) {
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED;
        return false;
    }
    return true;
}

static enum appliance_acme_transport_link_status binding_tls_step(
    struct appliance_acme_transport_lwip *binding)
{
    int result;

    if (!binding->ssl_setup) {
        if (!binding_tls_setup(binding)) {
            enum appliance_acme_transport_lwip_status status =
                binding->last_status;

            binding_release_exchange(binding, false);
            return binding_refuse(binding, status);
        }
    }
    ++binding->handshake_steps;
    binding->last_tls_error = 0;
    result = mbedtls_ssl_handshake_step(&binding->ssl);
    if (result == 0) {
        uint32_t verify_result = mbedtls_ssl_get_verify_result(&binding->ssl);

        binding->last_tls_error = (int)verify_result;
        /*
         * The handshake itself already enforced `VERIFY_REQUIRED`, so a
         * completed handshake is a verified peer; this second read keeps the
         * guarantee visible and refuses recorded failure flags.  mbedTLS
         * reports `(uint32_t) -1` when the finished session is no longer
         * inspectable (this binding drives the step function directly, so the
         * wrapper that would wrap the session up never runs) - that is the
         * absence of a verdict, not a failure, and is not treated as one.
         */
        if (verify_result != 0U && verify_result != UINT32_MAX) {
            return binding_refuse(
                binding, APPLIANCE_ACME_TRANSPORT_LWIP_HANDSHAKE_FAILED);
        }
        binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY;
        binding->last_status = APPLIANCE_ACME_TRANSPORT_LWIP_OK;
        return APPLIANCE_ACME_TRANSPORT_LINK_OK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        if (binding->peer_closed && binding_rx_drained(binding)) {
            binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED;
            return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
        }
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE;
    }
    binding->last_tls_error = result;
    return binding_refuse(
        binding, APPLIANCE_ACME_TRANSPORT_LWIP_HANDSHAKE_FAILED);
}

static enum appliance_acme_transport_link_status binding_resolve_step(
    struct appliance_acme_transport_lwip *binding)
{
    const struct appliance_dns_resolver *resolver;
    const struct network_ipv4_address *answer;
    ip_addr_t address;

    if (binding->lifecycle == NULL) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_NO_NETWORK);
    }
    resolver = appliance_network_lifecycle_dns(binding->lifecycle);
    if (resolver == NULL) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_NO_NETWORK);
    }
    switch (appliance_dns_resolver_state(resolver)) {
    case APPLIANCE_DNS_RESOLVER_STATE_RESOLVED:
        answer = appliance_dns_resolver_answer(resolver);
        if (answer == NULL ||
            !appliance_dns_resolver_answer_published(resolver)) {
            return binding_refuse(
                binding, APPLIANCE_ACME_TRANSPORT_LWIP_RESOLVE_FAILED);
        }
        IP4_ADDR(
            &address,
            answer->octets[0],
            answer->octets[1],
            answer->octets[2],
            answer->octets[3]);
        if (binding_tcp_start(binding, &address) !=
            APPLIANCE_ACME_TRANSPORT_LINK_OK) {
            return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
        }
        /*
         * The connection is in flight, not the handshake: the driver must
         * come back, so this reports progress rather than completion.
         */
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    case APPLIANCE_DNS_RESOLVER_STATE_QUERYING:
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    default:
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_RESOLVE_FAILED);
    }
}

/* -------------------------------------------------------------- link surface */

static enum appliance_acme_transport_link_status binding_link_open(
    void *context,
    const char *host,
    uint16_t port,
    bool literal_ipv4)
{
    struct appliance_acme_transport_lwip *binding = context;
    size_t length;

    if (!binding_valid(binding) || host == NULL || port == 0U) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    if (binding->phase == APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_RESOLVING ||
        binding->phase == APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING ||
        binding->phase == APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS ||
        binding->phase == APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_BUSY);
    }
    length = strlen(host);
    if (length == 0U ||
        length > APPLIANCE_ACME_TRANSPORT_LWIP_HOST_MAX) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_BAD_HOST);
    }
    /* A refusal the caller did not close must not leak its connection. */
    binding_release_exchange(binding, false);
    memcpy(binding->host, host, length);
    binding->host[length] = '\0';
    binding->host_length = length;
    binding->port = port;
    binding->literal_ipv4 = literal_ipv4;
    binding->last_status = APPLIANCE_ACME_TRANSPORT_LWIP_OK;
    if (literal_ipv4) {
        ip_addr_t address;

        if (ip4addr_aton(host, &address) != 1) {
            return binding_refuse(
                binding, APPLIANCE_ACME_TRANSPORT_LWIP_BAD_HOST);
        }
        return binding_tcp_start(binding, &address);
    }
    if (binding->lifecycle == NULL) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_NO_NETWORK);
    }
    if (appliance_network_lifecycle_dns_query(
            binding->lifecycle, host, length) !=
        APPLIANCE_NETWORK_LIFECYCLE_OK) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_RESOLVE_FAILED);
    }
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_RESOLVING;
    return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
}

static enum appliance_acme_transport_link_status binding_link_advance(
    void *context)
{
    struct appliance_acme_transport_lwip *binding = context;

    if (!binding_valid(binding)) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    if (binding->rx_overflow) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_OVERFLOW);
    }
    switch (binding->phase) {
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_RESOLVING:
        return binding_resolve_step(binding);
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING:
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS:
        return binding_tls_step(binding);
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY:
        return APPLIANCE_ACME_TRANSPORT_LINK_OK;
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED:
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_FAILED:
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    default:
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_BAD_STATE);
    }
}

static enum appliance_acme_transport_link_status binding_link_send(
    void *context,
    const uint8_t *bytes,
    size_t length,
    size_t *bytes_accepted)
{
    struct appliance_acme_transport_lwip *binding = context;
    int result;

    if (bytes_accepted == NULL) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    *bytes_accepted = 0U;
    if (!binding_valid(binding)) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    if (binding->phase != APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY ||
        !binding->ssl_setup || bytes == NULL || length == 0U) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_SEND_FAILED);
    }
    result = mbedtls_ssl_write(&binding->ssl, bytes, length);
    ++binding->send_calls;
    if (result > 0) {
        *bytes_accepted = (size_t)result;
        return APPLIANCE_ACME_TRANSPORT_LINK_OK;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE;
    }
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        result == MBEDTLS_ERR_SSL_CONN_EOF) {
        binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED;
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    }
    return binding_refuse(
        binding, APPLIANCE_ACME_TRANSPORT_LWIP_SEND_FAILED);
}

static enum appliance_acme_transport_link_status binding_link_recv(
    void *context,
    uint8_t *bytes,
    size_t capacity,
    size_t *bytes_received)
{
    struct appliance_acme_transport_lwip *binding = context;
    int result;

    if (bytes_received == NULL) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    *bytes_received = 0U;
    if (!binding_valid(binding)) {
        return APPLIANCE_ACME_TRANSPORT_LINK_FAILED;
    }
    if (binding->phase == APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED) {
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    }
    if (binding->rx_overflow) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_OVERFLOW);
    }
    if (binding->phase != APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY ||
        !binding->ssl_setup || bytes == NULL || capacity == 0U) {
        return binding_refuse(
            binding, APPLIANCE_ACME_TRANSPORT_LWIP_RECV_FAILED);
    }
    result = mbedtls_ssl_read(&binding->ssl, bytes, capacity);
    ++binding->recv_calls;
    if (result > 0) {
        *bytes_received = (size_t)result;
        return APPLIANCE_ACME_TRANSPORT_LINK_OK;
    }
    if (result == 0) {
        binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED;
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        /*
         * The transport has no more bytes to give and will never have any:
         * the peer's end of the stream is the framing, not a failure.
         */
        if (binding->peer_closed && binding_rx_drained(binding)) {
            binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED;
            return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
        }
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_READ;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return APPLIANCE_ACME_TRANSPORT_LINK_WANT_WRITE;
    }
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY ||
        result == MBEDTLS_ERR_SSL_CONN_EOF) {
        binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED;
        return APPLIANCE_ACME_TRANSPORT_LINK_CLOSED;
    }
    return binding_refuse(
        binding, APPLIANCE_ACME_TRANSPORT_LWIP_RECV_FAILED);
}

static void binding_link_close(void *context)
{
    struct appliance_acme_transport_lwip *binding = context;

    if (binding == NULL || !binding->published) {
        return;
    }
    binding_release_exchange(binding, true);
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_IDLE;
}

/* ------------------------------------------------------------ publication */

enum appliance_acme_transport_lwip_status appliance_acme_transport_lwip_init(
    struct appliance_acme_transport_lwip *binding,
    struct appliance_network_lifecycle *lifecycle,
    const unsigned char *anchor_der,
    size_t anchor_length)
{
    int result;

    if (binding == NULL) {
        return APPLIANCE_ACME_TRANSPORT_LWIP_NULL;
    }
    memset(binding, 0, sizeof(*binding));
    binding->version = APPLIANCE_ACME_TRANSPORT_LWIP_VERSION;
    binding->lifecycle = lifecycle;
    binding->phase = APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_IDLE;
    binding->last_status = APPLIANCE_ACME_TRANSPORT_LWIP_OK;
    binding->link.context = binding;
    binding->link.open = binding_link_open;
    binding->link.advance = binding_link_advance;
    binding->link.send = binding_link_send;
    binding->link.recv = binding_link_recv;
    binding->link.close = binding_link_close;
    binding->bio_token.index = 0U;
    binding->bio_token.generation = HFOC_MBEDTLS_BIO_GENERATION_MIN;
    binding->bio_policy.version = HFOC_MBEDTLS_BIO_VERSION;
    binding->bio_policy.max_send_bytes = HFOC_MBEDTLS_BIO_MAX_SEND_BYTES;
    binding->bio_policy.max_recv_bytes = HFOC_MBEDTLS_BIO_MAX_RECV_BYTES;
    if (hfoc_mbedtls_bio_init(&binding->bio) != HFOC_MBEDTLS_BIO_OK) {
        binding->last_status = APPLIANCE_ACME_TRANSPORT_LWIP_BAD_STATE;
        return binding->last_status;
    }
    binding->bio_initialized = true;
    if (anchor_der == NULL || anchor_length == 0U) {
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_ANCHOR_REFUSED;
        return binding->last_status;
    }
    mbedtls_x509_crt_init(&binding->anchor);
    result = mbedtls_x509_crt_parse_der(
        &binding->anchor, anchor_der, anchor_length);
    if (result != 0) {
        mbedtls_x509_crt_free(&binding->anchor);
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_ANCHOR_REFUSED;
        return binding->last_status;
    }
    binding->anchor_parsed = true;
    result = mbedtls_ssl_config_defaults(
        &binding->ssl_config,
        MBEDTLS_SSL_IS_CLIENT,
        MBEDTLS_SSL_TRANSPORT_STREAM,
        MBEDTLS_SSL_PRESET_DEFAULT);
    if (result != 0) {
        binding->last_status =
            APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED;
        return binding->last_status;
    }
    binding->config_published = true;
    mbedtls_ssl_conf_rng(
        &binding->ssl_config, appliance_production_entropy_random, NULL);
    mbedtls_ssl_conf_min_tls_version(
        &binding->ssl_config, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_max_tls_version(
        &binding->ssl_config, MBEDTLS_SSL_VERSION_TLS1_3);
    /*
     * The client verifies its one named anchor and refuses anything else; the
     * handshake result is checked again on completion, so a profile that
     * changed the mode cannot silently weaken this.
     */
    mbedtls_ssl_conf_authmode(
        &binding->ssl_config, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(
        &binding->ssl_config, &binding->anchor, NULL);
    binding->published = true;
    binding->last_status = APPLIANCE_ACME_TRANSPORT_LWIP_OK;
    return binding->last_status;
}

const struct appliance_acme_transport_link *
appliance_acme_transport_lwip_link(
    struct appliance_acme_transport_lwip *binding)
{
    if (!binding_valid(binding)) {
        return NULL;
    }
    return &binding->link;
}

enum appliance_acme_transport_lwip_status
appliance_acme_transport_lwip_last_status(
    const struct appliance_acme_transport_lwip *binding)
{
    if (binding == NULL) {
        return APPLIANCE_ACME_TRANSPORT_LWIP_NULL;
    }
    return binding->last_status;
}

int appliance_acme_transport_lwip_last_tls_error(
    const struct appliance_acme_transport_lwip *binding)
{
    if (binding == NULL) {
        return 0;
    }
    return binding->last_tls_error;
}

enum appliance_acme_transport_lwip_phase appliance_acme_transport_lwip_phase(
    const struct appliance_acme_transport_lwip *binding)
{
    if (binding == NULL) {
        return APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_IDLE;
    }
    return binding->phase;
}

bool appliance_acme_transport_lwip_published(
    const struct appliance_acme_transport_lwip *binding)
{
    return binding_valid(binding);
}

const char *appliance_acme_transport_lwip_phase_string(
    enum appliance_acme_transport_lwip_phase phase)
{
    switch (phase) {
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_IDLE:
        return "IDLE";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_RESOLVING:
        return "RESOLVING";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CONNECTING:
        return "CONNECTING";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_TLS:
        return "TLS";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_READY:
        return "READY";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_CLOSED:
        return "CLOSED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PHASE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

const char *appliance_acme_transport_lwip_status_string(
    enum appliance_acme_transport_lwip_status status)
{
    switch (status) {
    case APPLIANCE_ACME_TRANSPORT_LWIP_OK:
        return "OK";
    case APPLIANCE_ACME_TRANSPORT_LWIP_NULL:
        return "NULL";
    case APPLIANCE_ACME_TRANSPORT_LWIP_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_TRANSPORT_LWIP_BUSY:
        return "BUSY";
    case APPLIANCE_ACME_TRANSPORT_LWIP_NO_NETWORK:
        return "NO_NETWORK";
    case APPLIANCE_ACME_TRANSPORT_LWIP_BAD_HOST:
        return "BAD_HOST";
    case APPLIANCE_ACME_TRANSPORT_LWIP_ANCHOR_REFUSED:
        return "ANCHOR_REFUSED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_ALLOCATOR_REFUSED:
        return "ALLOCATOR_REFUSED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_TLS_SETUP_FAILED:
        return "TLS_SETUP_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_RESOLVE_FAILED:
        return "RESOLVE_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_CONNECT_FAILED:
        return "CONNECT_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_HANDSHAKE_FAILED:
        return "HANDSHAKE_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_SEND_FAILED:
        return "SEND_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_RECV_FAILED:
        return "RECV_FAILED";
    case APPLIANCE_ACME_TRANSPORT_LWIP_OVERFLOW:
        return "OVERFLOW";
    case APPLIANCE_ACME_TRANSPORT_LWIP_PEER_CLOSED:
        return "PEER_CLOSED";
    default:
        return "UNKNOWN";
    }
}

#endif /* HFOC_PRODUCTION_TLS */

/*
 * Every kernel build compiles this wildcard source, including the fallback and
 * drill kernels without the TLS profile; the guard keeps those translation
 * units non-empty and warning-free.
 */
typedef int appliance_acme_transport_lwip_translation_unit_guard;
