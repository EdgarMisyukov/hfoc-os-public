#include "hfoc_lwip_forward.h"

/*
 * ITEM-449: the one relay.  Compiled into the production kernel (the lwIP
 * sources only exist in that profile); the host arm stays available for a
 * future proof, exactly as the listener wrapper does.
 */
#if defined(HFOC_PRODUCTION_NETWORK) || defined(HFOC_LWIP_FORWARD_HOST_TEST)

#include "lwip/ip4_addr.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

_Static_assert(HFOC_LWIP_FORWARD_BUFFER_MAX <= UINT16_MAX,
               "forward buffer must fit its cursor");

static bool hfoc_lwip_forward_generation_valid(uint32_t generation)
{
    return generation >= HFOC_LWIP_FORWARD_GENERATION_MIN &&
           generation <= HFOC_LWIP_FORWARD_GENERATION_MAX;
}

static bool hfoc_lwip_forward_valid(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL &&
           forward->published &&
           forward->version == HFOC_LWIP_FORWARD_VERSION &&
           hfoc_lwip_forward_generation_valid(forward->generation) &&
           hfoc_lwip_netif_published(forward->netif);
}

static void hfoc_lwip_forward_set_verdict(
    struct hfoc_lwip_forward *forward,
    const char *verdict)
{
    size_t index = 0U;

    if (verdict == NULL) {
        forward->verdict[0] = '\0';
        return;
    }
    while (index + 1U < sizeof(forward->verdict) &&
           verdict[index] != '\0') {
        forward->verdict[index] = verdict[index];
        ++index;
    }
    forward->verdict[index] = '\0';
}

static void hfoc_lwip_forward_set_source(
    struct hfoc_lwip_forward *forward,
    const uint8_t source[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    size_t index;

    for (index = 0U; index < FORWARD_CONFIG_ADDRESS_LENGTH; ++index) {
        forward->last_source[index] = source[index];
    }
    forward->source_known = true;
}

static void hfoc_lwip_forward_source_of(
    const struct tcp_pcb *pcb,
    uint8_t source[FORWARD_CONFIG_ADDRESS_LENGTH])
{
    const ip4_addr_t *remote = ip_2_ip4(&pcb->remote_ip);

    source[0] = ip4_addr1(remote);
    source[1] = ip4_addr2(remote);
    source[2] = ip4_addr3(remote);
    source[3] = ip4_addr4(remote);
}

static void hfoc_lwip_forward_clear_session(
    struct hfoc_lwip_forward *forward)
{
    forward->client_pcb = NULL;
    forward->target_pcb = NULL;
    forward->target_connected = false;
    forward->client_eof = false;
    forward->target_eof = false;
    forward->target_shutdown = false;
    forward->client_shutdown = false;
    forward->to_target_length = 0U;
    forward->to_target_offset = 0U;
    forward->to_client_length = 0U;
    forward->to_client_offset = 0U;
}

static bool hfoc_lwip_forward_drained(
    const struct hfoc_lwip_forward *forward)
{
    return forward->to_target_offset == forward->to_target_length &&
           forward->to_client_offset == forward->to_client_length;
}

static void hfoc_lwip_forward_abort_pcb(
    struct hfoc_lwip_forward *forward,
    struct tcp_pcb **pcb,
    bool is_target)
{
    if (*pcb != NULL) {
        /*
         * ITEM-644: tcp_abort() runs the PCB's error callback synchronously.
         * Mark this leg first so that callback knows the session is already
         * ending here and does not fail it a second time.
         */
        if (is_target) {
            forward->aborting_target = true;
        } else {
            forward->aborting_client = true;
        }
        tcp_abort(*pcb);
        if (is_target) {
            forward->aborting_target = false;
        } else {
            forward->aborting_client = false;
        }
        *pcb = NULL;
    }
}

/*
 * End the session with a named verdict.  Both PCBs go, because a relay that
 * lost one half cannot serve the other: the peer would wait forever for bytes
 * that will never come.
 */
static void hfoc_lwip_forward_fail(
    struct hfoc_lwip_forward *forward,
    const char *verdict)
{
    hfoc_lwip_forward_abort_pcb(forward, &forward->client_pcb, false);
    hfoc_lwip_forward_abort_pcb(forward, &forward->target_pcb, true);
    hfoc_lwip_forward_set_verdict(forward, verdict);
    ++forward->aborted;
    forward->event = HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED;
    hfoc_lwip_forward_clear_session(forward);
}

static void hfoc_lwip_forward_finish(
    struct hfoc_lwip_forward *forward,
    const char *verdict)
{
    if (forward->client_pcb != NULL) {
        if (tcp_close(forward->client_pcb) != ERR_OK) {
            hfoc_lwip_forward_abort_pcb(
                forward, &forward->client_pcb, false);
        } else {
            forward->client_pcb = NULL;
        }
    }
    if (forward->target_pcb != NULL) {
        if (tcp_close(forward->target_pcb) != ERR_OK) {
            hfoc_lwip_forward_abort_pcb(
                forward, &forward->target_pcb, true);
        } else {
            forward->target_pcb = NULL;
        }
    }
    hfoc_lwip_forward_set_verdict(forward, verdict);
    ++forward->completed;
    forward->event = HFOC_LWIP_FORWARD_EVENT_SESSION_OK;
    hfoc_lwip_forward_clear_session(forward);
}

/*
 * Move whatever the peer's window can take.  A partial write is normal: the
 * offset records what went, and `tcp_sent`/`tcp_poll` call back here when the
 * window opens again.
 */
static void hfoc_lwip_forward_pump_direction(
    struct tcp_pcb *peer,
    uint8_t *buffer,
    uint16_t *length,
    uint16_t *offset,
    uint32_t *bytes,
    uint32_t now_ms,
    bool *progress)
{
    uint16_t pending;
    uint16_t space;

    if (peer == NULL) {
        return;
    }
    if (*offset >= *length) {
        *length = 0U;
        *offset = 0U;
        return;
    }
    pending = (uint16_t)(*length - *offset);
    space = (uint16_t)tcp_sndbuf(peer);
    if (space > pending) {
        space = pending;
    }
    if (space == 0U) {
        return;
    }
    if (tcp_write(peer, buffer + *offset, space,
                  TCP_WRITE_FLAG_COPY) != ERR_OK) {
        return;
    }
    *offset = (uint16_t)(*offset + space);
    *bytes += space;
    *progress = true;
    (void)tcp_output(peer);
    (void)now_ms;
}

static void hfoc_lwip_forward_pump(struct hfoc_lwip_forward *forward)
{
    bool progress = false;

    if (forward->target_connected) {
        hfoc_lwip_forward_pump_direction(
            forward->target_pcb, forward->to_target,
            &forward->to_target_length, &forward->to_target_offset,
            &forward->bytes_to_target, forward->now_ms, &progress);
    }
    hfoc_lwip_forward_pump_direction(
        forward->client_pcb, forward->to_client,
        &forward->to_client_length, &forward->to_client_offset,
        &forward->bytes_to_client, forward->now_ms, &progress);
    if (progress) {
        forward->last_progress_ms = forward->now_ms;
    }
}

/*
 * A side that finished sending has nothing more to say: half-close the other
 * side so its peer reads the end of the stream, and close the session once
 * both directions ended and nothing is left in the buffers.
 */
static void hfoc_lwip_forward_settle(struct hfoc_lwip_forward *forward)
{
    hfoc_lwip_forward_pump(forward);
    if (forward->client_pcb == NULL || forward->target_pcb == NULL) {
        return;
    }
    if (forward->client_eof && forward->target_connected &&
        !forward->target_shutdown &&
        forward->to_target_offset == forward->to_target_length) {
        forward->target_shutdown = true;
        (void)tcp_shutdown(forward->target_pcb, 0, 1);
    }
    if (forward->target_eof && !forward->client_shutdown &&
        forward->to_client_offset == forward->to_client_length) {
        forward->client_shutdown = true;
        (void)tcp_shutdown(forward->client_pcb, 0, 1);
    }
    if (forward->client_eof && forward->target_eof &&
        hfoc_lwip_forward_drained(forward)) {
        hfoc_lwip_forward_finish(forward, "OK");
    }
}

static err_t hfoc_lwip_forward_connected(
    void *arg,
    struct tcp_pcb *pcb,
    err_t err)
{
    struct hfoc_lwip_forward_link *link = arg;
    struct hfoc_lwip_forward *forward;

    if (link == NULL || link->owner == NULL) {
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    forward = link->owner;
    if (err != ERR_OK || pcb == NULL) {
        /*
         * ITEM-644: on a failed connect lwIP owns the PCB and has already
         * removed it.  Aborting it here would be a use-after-free; only the
         * accepted client still belongs to the relay.
         */
        if (forward->target_pcb == pcb) {
            forward->target_pcb = NULL;
        }
        hfoc_lwip_forward_fail(forward, "CONNECT_FAILED");
        return ERR_ABRT;
    }
    if (forward->target_pcb != pcb) {
        /* A late callback for a leg the relay no longer owns. */
        return ERR_OK;
    }
    forward->target_connected = true;
    forward->last_progress_ms = forward->now_ms;
    hfoc_lwip_forward_settle(forward);
    return ERR_OK;
}

/*
 * Copy one received pbuf chain into the direction's buffer, or refuse it
 * whole: a partially copied chain would be acknowledged twice when lwIP
 * re-delivers it.
 */
static err_t hfoc_lwip_forward_receive(
    struct hfoc_lwip_forward *forward,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    uint8_t *buffer,
    uint16_t *length,
    uint16_t *offset,
    bool *eof)
{
    uint16_t used;

    if (p == NULL) {
        *eof = true;
        hfoc_lwip_forward_settle(forward);
        return ERR_OK;
    }
    used = (uint16_t)(*length - *offset);
    if (p->tot_len > HFOC_LWIP_FORWARD_BUFFER_MAX - used) {
        /*
         * No room: do not acknowledge and do not free.  lwIP keeps the pbuf
         * and calls back when the window moves, which is the backpressure the
         * bounded budget buys.
         */
        return ERR_MEM;
    }
    if (*offset > 0U && used > 0U) {
        memmove(buffer, buffer + *offset, used);
    }
    *offset = 0U;
    *length = used;
    (void)pbuf_copy_partial(p, buffer + *length, p->tot_len, 0U);
    *length = (uint16_t)(*length + p->tot_len);
    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    hfoc_lwip_forward_pump(forward);
    return ERR_OK;
}

static err_t hfoc_lwip_forward_receive_client(
    void *arg,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err)
{
    struct hfoc_lwip_forward_link *link = arg;

    if (link == NULL || link->owner == NULL) {
        return ERR_OK;
    }
    if (err != ERR_OK) {
        hfoc_lwip_forward_fail(link->owner, "CLIENT_ERROR");
        return ERR_ABRT;
    }
    if (link->owner->client_pcb != pcb) {
        return ERR_OK;
    }
    return hfoc_lwip_forward_receive(
        link->owner, pcb, p, link->owner->to_target,
        &link->owner->to_target_length, &link->owner->to_target_offset,
        &link->owner->client_eof);
}

static err_t hfoc_lwip_forward_receive_target(
    void *arg,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err)
{
    struct hfoc_lwip_forward_link *link = arg;

    if (link == NULL || link->owner == NULL) {
        return ERR_OK;
    }
    if (err != ERR_OK) {
        hfoc_lwip_forward_fail(link->owner, "TARGET_ERROR");
        return ERR_ABRT;
    }
    if (link->owner->target_pcb != pcb) {
        return ERR_OK;
    }
    return hfoc_lwip_forward_receive(
        link->owner, pcb, p, link->owner->to_client,
        &link->owner->to_client_length, &link->owner->to_client_offset,
        &link->owner->target_eof);
}

static err_t hfoc_lwip_forward_sent(
    void *arg,
    struct tcp_pcb *pcb,
    u16_t length)
{
    struct hfoc_lwip_forward_link *link = arg;

    (void)pcb;
    (void)length;
    if (link != NULL && link->owner != NULL) {
        hfoc_lwip_forward_pump(link->owner);
    }
    return ERR_OK;
}

static err_t hfoc_lwip_forward_poll(
    void *arg,
    struct tcp_pcb *pcb)
{
    struct hfoc_lwip_forward_link *link = arg;

    (void)pcb;
    if (link == NULL || link->owner == NULL) {
        return ERR_OK;
    }
    /*
     * The poll callback re-drives a pump that a closed window stalled; the
     * idle deadline itself is the service call's, so it sees the loop's clock.
     */
    hfoc_lwip_forward_pump(link->owner);
    return ERR_OK;
}

static void hfoc_lwip_forward_error(void *arg, err_t err)
{
    struct hfoc_lwip_forward_link *link = arg;
    struct hfoc_lwip_forward *forward;

    if (link == NULL || link->owner == NULL) {
        return;
    }
    forward = link->owner;
    if (link->is_target) {
        forward->target_pcb = NULL;
    } else {
        forward->client_pcb = NULL;
    }
    if (err == ERR_ABRT && (link->is_target ? forward->aborting_target
                                           : forward->aborting_client)) {
        /*
         * Our own abort: the session is already over and its record written.
         */
        return;
    }
    /*
     * ITEM-644: an ERR_ABRT that we did not initiate is the stack removing a
     * PCB on its own, e.g. after TCP_SYNMAXRTX.  The other leg must not be
     * left attached to a half-session that can never complete.
     */
    ++forward->errors;
    hfoc_lwip_forward_fail(
        forward, link->is_target ? "TARGET_ABORTED" : "CLIENT_ABORTED");
}

static err_t hfoc_lwip_forward_accept(
    void *arg,
    struct tcp_pcb *pcb,
    err_t err)
{
    struct hfoc_lwip_forward *forward = arg;
    uint8_t source[FORWARD_CONFIG_ADDRESS_LENGTH];
    ip_addr_t target_address;
    struct tcp_pcb *target;
    enum forward_config_status admission;

    if (forward == NULL || !forward->listening || err != ERR_OK ||
        pcb == NULL) {
        if (pcb != NULL) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    hfoc_lwip_forward_source_of(pcb, source);
    admission = forward_config_admit(&forward->config, source);
    if (admission != FORWARD_CONFIG_ADMITTED) {
        /*
         * The access rule is enforced here, before the appliance dials
         * anything: a denied client cannot use the machine as a way out.
         */
        ++forward->refused_source;
        hfoc_lwip_forward_set_source(forward, source);
        hfoc_lwip_forward_set_verdict(forward, "SOURCE_NOT_ALLOWED");
        forward->event = HFOC_LWIP_FORWARD_EVENT_REFUSED_SOURCE;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    if (forward->client_pcb != NULL || forward->target_pcb != NULL) {
        ++forward->refused_busy;
        hfoc_lwip_forward_set_source(forward, source);
        hfoc_lwip_forward_set_verdict(forward, "BUSY");
        forward->event = HFOC_LWIP_FORWARD_EVENT_REFUSED_BUSY;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    target = tcp_new();
    if (target == NULL) {
        hfoc_lwip_forward_set_source(forward, source);
        hfoc_lwip_forward_set_verdict(forward, "NO_MEMORY");
        forward->event = HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED;
        ++forward->aborted;
        tcp_abort(pcb);
        return ERR_ABRT;
    }
    ++forward->accepted;
    ++forward->sessions;
    forward->client_pcb = pcb;
    forward->target_pcb = target;
    forward->target_connected = false;
    forward->client_eof = false;
    forward->target_eof = false;
    forward->target_shutdown = false;
    forward->client_shutdown = false;
    forward->to_target_length = 0U;
    forward->to_target_offset = 0U;
    forward->to_client_length = 0U;
    forward->to_client_offset = 0U;
    forward->last_progress_ms = forward->now_ms;
    hfoc_lwip_forward_set_source(forward, source);
    hfoc_lwip_forward_set_verdict(forward, "OPEN");
    forward->client_link.owner = forward;
    forward->client_link.is_target = false;
    forward->target_link.owner = forward;
    forward->target_link.is_target = true;
    tcp_arg(pcb, &forward->client_link);
    tcp_recv(pcb, hfoc_lwip_forward_receive_client);
    tcp_sent(pcb, hfoc_lwip_forward_sent);
    tcp_err(pcb, hfoc_lwip_forward_error);
    tcp_poll(pcb, hfoc_lwip_forward_poll,
             (u8_t)HFOC_LWIP_FORWARD_POLL_INTERVAL);
    tcp_nagle_disable(pcb);
    tcp_arg(target, &forward->target_link);
    tcp_recv(target, hfoc_lwip_forward_receive_target);
    tcp_sent(target, hfoc_lwip_forward_sent);
    tcp_err(target, hfoc_lwip_forward_error);
    tcp_poll(target, hfoc_lwip_forward_poll,
             (u8_t)HFOC_LWIP_FORWARD_POLL_INTERVAL);
    tcp_nagle_disable(target);
    IP4_ADDR(
        &target_address,
        forward->config.target[0], forward->config.target[1],
        forward->config.target[2], forward->config.target[3]);
    if (tcp_connect(target, &target_address,
                    forward->config.target_port,
                    hfoc_lwip_forward_connected) != ERR_OK) {
        hfoc_lwip_forward_fail(forward, "CONNECT_FAILED");
        return ERR_ABRT;
    }
    return ERR_OK;
}

enum hfoc_lwip_forward_status hfoc_lwip_forward_init(
    struct hfoc_lwip_forward *forward,
    const struct hfoc_lwip_netif *netif,
    const struct forward_config *config,
    uint32_t generation)
{
    if (forward == NULL || netif == NULL || config == NULL) {
        return HFOC_LWIP_FORWARD_NULL;
    }
    if (forward->published || forward->version != 0U) {
        return HFOC_LWIP_FORWARD_BAD_STATE;
    }
    if (!hfoc_lwip_forward_generation_valid(generation)) {
        return HFOC_LWIP_FORWARD_INVALID_GENERATION;
    }
    if (!hfoc_lwip_netif_live(netif) && !hfoc_lwip_netif_staged(netif)) {
        return HFOC_LWIP_FORWARD_NETIF_NOT_LIVE;
    }
    if (forward_config_validate(config) != FORWARD_CONFIG_OK) {
        return HFOC_LWIP_FORWARD_INVALID_CONFIG;
    }
    memset(forward, 0, sizeof(*forward));
    forward->version = HFOC_LWIP_FORWARD_VERSION;
    forward->generation = generation;
    forward->config = *config;
    forward->netif = netif;
    forward->published = true;
    forward->now_ms = 0U;
    forward->last_progress_ms = 0U;
    forward->next_attempt_ms = 0U;
    forward->event = HFOC_LWIP_FORWARD_EVENT_NONE;
    hfoc_lwip_forward_set_verdict(forward, "NONE");
    forward->status = forward_config_enabled(config)
        ? HFOC_LWIP_FORWARD_OK
        : HFOC_LWIP_FORWARD_NOT_DECLARED;
    return HFOC_LWIP_FORWARD_OK;
}

enum hfoc_lwip_forward_status hfoc_lwip_forward_listen(
    struct hfoc_lwip_forward *forward)
{
    struct tcp_pcb *pcb;
    struct tcp_pcb *listen_pcb;
    err_t bind_err;
    err_t listen_err = ERR_OK;

    if (forward == NULL) {
        return HFOC_LWIP_FORWARD_NULL;
    }
    if (!hfoc_lwip_forward_valid(forward)) {
        return HFOC_LWIP_FORWARD_BAD_STATE;
    }
    if (forward->listening) {
        return HFOC_LWIP_FORWARD_ALREADY_LISTENING;
    }
    if (!forward_config_enabled(&forward->config)) {
        return HFOC_LWIP_FORWARD_NOT_DECLARED;
    }
    if (!hfoc_lwip_netif_addressed(forward->netif)) {
        forward->status = HFOC_LWIP_FORWARD_NETIF_NOT_LIVE;
        return forward->status;
    }
    pcb = tcp_new();
    if (pcb == NULL) {
        forward->status = HFOC_LWIP_FORWARD_LWIP_MEMORY;
        return forward->status;
    }
    bind_err = tcp_bind(pcb, IP_ANY_TYPE, forward->config.listen_port);
    if (bind_err != ERR_OK) {
        tcp_abort(pcb);
        forward->status = bind_err == ERR_USE
            ? HFOC_LWIP_FORWARD_PORT_IN_USE
            : HFOC_LWIP_FORWARD_BIND_FAILED;
        return forward->status;
    }
    listen_pcb = tcp_listen_with_backlog_and_err(
        pcb, (u8_t)HFOC_LWIP_FORWARD_LISTEN_BACKLOG, &listen_err);
    if (listen_pcb == NULL) {
        forward->status = listen_err == ERR_MEM
            ? HFOC_LWIP_FORWARD_LWIP_MEMORY
            : (listen_err == ERR_USE
                   ? HFOC_LWIP_FORWARD_PORT_IN_USE
                   : HFOC_LWIP_FORWARD_LISTEN_FAILED);
        return forward->status;
    }
    forward->listen_pcb = listen_pcb;
    forward->listening = true;
    forward->status = HFOC_LWIP_FORWARD_OK;
    forward->event = HFOC_LWIP_FORWARD_EVENT_LISTENING;
    tcp_arg(listen_pcb, forward);
    tcp_accept(listen_pcb, hfoc_lwip_forward_accept);
    return HFOC_LWIP_FORWARD_OK;
}

enum hfoc_lwip_forward_status hfoc_lwip_forward_service(
    struct hfoc_lwip_forward *forward,
    uint32_t now_ms)
{
    if (forward == NULL) {
        return HFOC_LWIP_FORWARD_NULL;
    }
    if (!hfoc_lwip_forward_valid(forward)) {
        return HFOC_LWIP_FORWARD_BAD_STATE;
    }
    forward->now_ms = now_ms;
    if (!forward_config_enabled(&forward->config)) {
        return HFOC_LWIP_FORWARD_NOT_DECLARED;
    }
    if (!forward->listening) {
        enum hfoc_lwip_forward_status status;

        if ((int32_t)(now_ms - forward->next_attempt_ms) < 0) {
            return forward->status;
        }
        status = hfoc_lwip_forward_listen(forward);
        if (status == HFOC_LWIP_FORWARD_NETIF_NOT_LIVE) {
            return status;
        }
        forward->next_attempt_ms = now_ms + HFOC_LWIP_FORWARD_RETRY_MS;
        if (status != HFOC_LWIP_FORWARD_OK) {
            forward->event = HFOC_LWIP_FORWARD_EVENT_LISTEN_FAILED;
            return forward->status;
        }
    }
    if (forward->client_pcb != NULL || forward->target_pcb != NULL) {
        const uint32_t idle =
            forward->last_progress_ms + HFOC_LWIP_FORWARD_IDLE_MS;

        /*
         * ITEM-644: the deadline belongs to the session, not to a pair of
         * pointers.  If one leg was removed by the stack, the surviving leg
         * must still be failed instead of holding the one relay slot.
         */
        if (idle < forward->last_progress_ms ||
            (int32_t)(now_ms - idle) >= 0) {
            hfoc_lwip_forward_fail(forward, "IDLE");
            return HFOC_LWIP_FORWARD_OK;
        }
        if (forward->client_pcb != NULL && forward->target_pcb != NULL) {
            hfoc_lwip_forward_settle(forward);
        }
    }
    return HFOC_LWIP_FORWARD_OK;
}

enum hfoc_lwip_forward_status hfoc_lwip_forward_shutdown(
    struct hfoc_lwip_forward *forward)
{
    if (forward == NULL) {
        return HFOC_LWIP_FORWARD_NULL;
    }
    if (!forward->published) {
        return HFOC_LWIP_FORWARD_BAD_STATE;
    }
    hfoc_lwip_forward_abort_pcb(forward, &forward->client_pcb, false);
    hfoc_lwip_forward_abort_pcb(forward, &forward->target_pcb, true);
    if (forward->listen_pcb != NULL) {
        if (tcp_close(forward->listen_pcb) != ERR_OK) {
            tcp_abort(forward->listen_pcb);
        }
        forward->listen_pcb = NULL;
    }
    memset(forward, 0, sizeof(*forward));
    return HFOC_LWIP_FORWARD_OK;
}

bool hfoc_lwip_forward_listening(const struct hfoc_lwip_forward *forward)
{
    return forward != NULL &&
           forward->published &&
           forward->listening &&
           forward->listen_pcb != NULL &&
           forward->listen_pcb->state == LISTEN;
}

bool hfoc_lwip_forward_session_active(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published &&
           (forward->client_pcb != NULL || forward->target_pcb != NULL);
}

enum hfoc_lwip_forward_event hfoc_lwip_forward_take_event(
    struct hfoc_lwip_forward *forward)
{
    enum hfoc_lwip_forward_event event;

    if (forward == NULL) {
        return HFOC_LWIP_FORWARD_EVENT_NONE;
    }
    event = forward->event;
    forward->event = HFOC_LWIP_FORWARD_EVENT_NONE;
    return event;
}

const struct forward_config *hfoc_lwip_forward_config(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published ? &forward->config : NULL;
}

const char *hfoc_lwip_forward_verdict(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published ? forward->verdict : NULL;
}

const uint8_t *hfoc_lwip_forward_last_source(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published ? forward->last_source
                                                 : NULL;
}

bool hfoc_lwip_forward_source_known(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published && forward->source_known;
}

enum hfoc_lwip_forward_status hfoc_lwip_forward_status_of(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL && forward->published
        ? forward->status
        : HFOC_LWIP_FORWARD_NULL;
}

uint32_t hfoc_lwip_forward_accepted(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->accepted : 0U;
}

uint32_t hfoc_lwip_forward_refused_source(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->refused_source : 0U;
}

uint32_t hfoc_lwip_forward_refused_busy(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->refused_busy : 0U;
}

uint32_t hfoc_lwip_forward_sessions(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->sessions : 0U;
}

uint32_t hfoc_lwip_forward_completed(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->completed : 0U;
}

uint32_t hfoc_lwip_forward_aborted(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->aborted : 0U;
}

uint32_t hfoc_lwip_forward_errors(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->errors : 0U;
}

uint32_t hfoc_lwip_forward_bytes_to_target(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->bytes_to_target : 0U;
}

uint32_t hfoc_lwip_forward_bytes_to_client(
    const struct hfoc_lwip_forward *forward)
{
    return forward != NULL ? forward->bytes_to_client : 0U;
}

const char *hfoc_lwip_forward_status_string(
    enum hfoc_lwip_forward_status status)
{
    switch (status) {
    case HFOC_LWIP_FORWARD_OK:
        return "OK";
    case HFOC_LWIP_FORWARD_NULL:
        return "NULL";
    case HFOC_LWIP_FORWARD_BAD_STATE:
        return "BAD_STATE";
    case HFOC_LWIP_FORWARD_INVALID_VERSION:
        return "INVALID_VERSION";
    case HFOC_LWIP_FORWARD_INVALID_GENERATION:
        return "INVALID_GENERATION";
    case HFOC_LWIP_FORWARD_NETIF_NOT_LIVE:
        return "NETIF_NOT_LIVE";
    case HFOC_LWIP_FORWARD_NOT_DECLARED:
        return "NOT_DECLARED";
    case HFOC_LWIP_FORWARD_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case HFOC_LWIP_FORWARD_LWIP_MEMORY:
        return "LWIP_MEMORY";
    case HFOC_LWIP_FORWARD_BIND_FAILED:
        return "BIND_FAILED";
    case HFOC_LWIP_FORWARD_PORT_IN_USE:
        return "PORT_IN_USE";
    case HFOC_LWIP_FORWARD_LISTEN_FAILED:
        return "LISTEN_FAILED";
    case HFOC_LWIP_FORWARD_ALREADY_LISTENING:
        return "ALREADY_LISTENING";
    case HFOC_LWIP_FORWARD_NOT_LISTENING:
        return "NOT_LISTENING";
    case HFOC_LWIP_FORWARD_CONNECT_FAILED:
        return "CONNECT_FAILED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_forward_event_string(
    enum hfoc_lwip_forward_event event)
{
    switch (event) {
    case HFOC_LWIP_FORWARD_EVENT_NONE:
        return "NONE";
    case HFOC_LWIP_FORWARD_EVENT_LISTENING:
        return "LISTENING";
    case HFOC_LWIP_FORWARD_EVENT_REFUSED_SOURCE:
        return "REFUSED_SOURCE";
    case HFOC_LWIP_FORWARD_EVENT_REFUSED_BUSY:
        return "REFUSED_BUSY";
    case HFOC_LWIP_FORWARD_EVENT_SESSION_OK:
        return "SESSION_OK";
    case HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED:
        return "SESSION_FAILED";
    case HFOC_LWIP_FORWARD_EVENT_LISTEN_FAILED:
        return "LISTEN_FAILED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_forward_not_compiled;

#endif
