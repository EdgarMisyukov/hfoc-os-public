#ifndef HFOC_LWIP_FORWARD_H
#define HFOC_LWIP_FORWARD_H

#include "forward_config.h"
#include "hfoc_lwip_netif.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-449: the relay owner.
 *
 * The declaration says what to forward and who may use it; this owner is the
 * only thing that acts on it.  It listens on the declared port, admits a
 * connection by source address *before* any outbound connection exists, dials
 * the declared target once, and relays bytes in both directions with
 * backpressure.  It owns the two PCBs of the one session the budget allows and
 * publishes what happened as counters and one event the caller renders.
 *
 * It is deliberately not the `hfoc_lwip_tcp` wrapper: that wrapper owns exactly
 * one listener, and the production serving listener already holds it.
 *
 * No TLS is terminated here: the bytes are opaque and the certificate belongs
 * to the client and the target.
 */
#define HFOC_LWIP_FORWARD_VERSION 1U
#define HFOC_LWIP_FORWARD_GENERATION_MIN 1U
#define HFOC_LWIP_FORWARD_GENERATION_MAX UINT32_C(0xFFFFFFFE)
/*
 * lwIP retries a refused pbuf chain whole. Size each direction for a receive
 * window plus one segment so a full callback can fit in an empty buffer,
 * including a chain assembled from out-of-order segments.
 */
#define HFOC_LWIP_FORWARD_BUFFER_MAX (TCP_WND + TCP_MSS)
#define HFOC_LWIP_FORWARD_IDLE_MS 30000U
#define HFOC_LWIP_FORWARD_LISTEN_BACKLOG 2U
/* tcp_poll's interval is in coarse timer units of 500 ms. */
#define HFOC_LWIP_FORWARD_POLL_INTERVAL 2U
#define HFOC_LWIP_FORWARD_RETRY_MS 2000U
#define HFOC_LWIP_FORWARD_VERDICT_MAX 24U

enum hfoc_lwip_forward_status {
    HFOC_LWIP_FORWARD_OK = 0,
    HFOC_LWIP_FORWARD_NULL = 1,
    HFOC_LWIP_FORWARD_BAD_STATE = 2,
    HFOC_LWIP_FORWARD_INVALID_VERSION = 3,
    HFOC_LWIP_FORWARD_INVALID_GENERATION = 4,
    HFOC_LWIP_FORWARD_NETIF_NOT_LIVE = 5,
    HFOC_LWIP_FORWARD_NOT_DECLARED = 6,
    HFOC_LWIP_FORWARD_INVALID_CONFIG = 7,
    HFOC_LWIP_FORWARD_LWIP_MEMORY = 8,
    HFOC_LWIP_FORWARD_BIND_FAILED = 9,
    HFOC_LWIP_FORWARD_PORT_IN_USE = 10,
    HFOC_LWIP_FORWARD_LISTEN_FAILED = 11,
    HFOC_LWIP_FORWARD_ALREADY_LISTENING = 12,
    HFOC_LWIP_FORWARD_NOT_LISTENING = 13,
    HFOC_LWIP_FORWARD_CONNECT_FAILED = 14,
};

/*
 * One event per thing the record has to say.  The caller takes it and renders
 * the line; the module never writes to a console or a serial port.
 */
enum hfoc_lwip_forward_event {
    HFOC_LWIP_FORWARD_EVENT_NONE = 0,
    HFOC_LWIP_FORWARD_EVENT_LISTENING = 1,
    HFOC_LWIP_FORWARD_EVENT_REFUSED_SOURCE = 2,
    HFOC_LWIP_FORWARD_EVENT_REFUSED_BUSY = 3,
    HFOC_LWIP_FORWARD_EVENT_SESSION_OK = 4,
    HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED = 5,
    HFOC_LWIP_FORWARD_EVENT_LISTEN_FAILED = 6,
};

/*
 * The per-PCB callback argument.  lwIP's error callback does not carry the
 * PCB, so the side is part of what the callback was given.
 */
struct hfoc_lwip_forward_link {
    struct hfoc_lwip_forward *owner;
    bool is_target;
};

struct hfoc_lwip_forward {
    uint32_t version;
    bool published;
    uint32_t generation;
    struct forward_config config;
    const struct hfoc_lwip_netif *netif;
    struct tcp_pcb *listen_pcb;
    bool listening;
    enum hfoc_lwip_forward_status status;
    uint32_t next_attempt_ms;
    /* the one session */
    struct hfoc_lwip_forward_link client_link;
    struct hfoc_lwip_forward_link target_link;
    struct tcp_pcb *client_pcb;
    struct tcp_pcb *target_pcb;
    /*
     * ITEM-644: lwIP's error callback reports ERR_ABRT both for the relay's
     * own tcp_abort() and for a PCB the stack removed on its own (for example
     * after TCP_SYNMAXRTX).  The callback must tell them apart: the first is
     * already handled, the second has to fail the surviving leg.
     */
    bool aborting_client;
    bool aborting_target;
    bool target_connected;
    bool client_eof;
    bool target_eof;
    bool target_shutdown;
    bool client_shutdown;
    uint32_t now_ms;
    uint8_t to_target[HFOC_LWIP_FORWARD_BUFFER_MAX];
    uint16_t to_target_length;
    uint16_t to_target_offset;
    uint8_t to_client[HFOC_LWIP_FORWARD_BUFFER_MAX];
    uint16_t to_client_length;
    uint16_t to_client_offset;
    uint32_t last_progress_ms;
    /* counters, cumulative for the life of the owner */
    uint32_t accepted;
    uint32_t refused_source;
    uint32_t refused_busy;
    uint32_t sessions;
    uint32_t completed;
    uint32_t aborted;
    uint32_t errors;
    uint32_t bytes_to_target;
    uint32_t bytes_to_client;
    uint8_t last_source[FORWARD_CONFIG_ADDRESS_LENGTH];
    bool source_known;
    enum hfoc_lwip_forward_event event;
    char verdict[HFOC_LWIP_FORWARD_VERDICT_MAX];
};

/* Preserve a declaration while the staged netif waits for link and IPv4. */
enum hfoc_lwip_forward_status hfoc_lwip_forward_init(
    struct hfoc_lwip_forward *forward,
    const struct hfoc_lwip_netif *netif,
    const struct forward_config *config,
    uint32_t generation);
/*
 * Bind and listen on the declared port.  A refused bind is a status the caller
 * publishes; `service` retries it, so a port still in TIME_WAIT or an address
 * that is not up yet is a delay, not a silent death.
 */
enum hfoc_lwip_forward_status hfoc_lwip_forward_listen(
    struct hfoc_lwip_forward *forward);
/*
 * The loop's turn: bring the listener up when the declaration says so, pump
 * both directions, and end a session that stopped making progress.  It never
 * blocks: a peer that cannot take bytes waits for its own window to open.
 */
enum hfoc_lwip_forward_status hfoc_lwip_forward_service(
    struct hfoc_lwip_forward *forward,
    uint32_t now_ms);
enum hfoc_lwip_forward_status hfoc_lwip_forward_shutdown(
    struct hfoc_lwip_forward *forward);
bool hfoc_lwip_forward_listening(
    const struct hfoc_lwip_forward *forward);
bool hfoc_lwip_forward_session_active(
    const struct hfoc_lwip_forward *forward);
enum hfoc_lwip_forward_event hfoc_lwip_forward_take_event(
    struct hfoc_lwip_forward *forward);
const struct forward_config *hfoc_lwip_forward_config(
    const struct hfoc_lwip_forward *forward);
const char *hfoc_lwip_forward_verdict(
    const struct hfoc_lwip_forward *forward);
const uint8_t *hfoc_lwip_forward_last_source(
    const struct hfoc_lwip_forward *forward);
bool hfoc_lwip_forward_source_known(
    const struct hfoc_lwip_forward *forward);
enum hfoc_lwip_forward_status hfoc_lwip_forward_status_of(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_accepted(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_refused_source(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_refused_busy(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_sessions(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_completed(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_aborted(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_errors(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_bytes_to_target(
    const struct hfoc_lwip_forward *forward);
uint32_t hfoc_lwip_forward_bytes_to_client(
    const struct hfoc_lwip_forward *forward);
const char *hfoc_lwip_forward_status_string(
    enum hfoc_lwip_forward_status status);
const char *hfoc_lwip_forward_event_string(
    enum hfoc_lwip_forward_event event);

#endif
