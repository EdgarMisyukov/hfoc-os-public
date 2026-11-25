#ifndef HFOC_APPLIANCE_PRODUCTION_ACME_HTTP01_H
#define HFOC_APPLIANCE_PRODUCTION_ACME_HTTP01_H

#include "appliance_acme_challenge.h"
#include "appliance_acme_flow.h"
#include "hfoc_lwip_netif.h"
#include "hfoc_lwip_tcp.h"
#include "network_ipv4_profile.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-508 increment 28: the socket half of the HTTP-01 answer.
 *
 * The decision already exists and is host-proven (increment 7): exactly
 * `/.well-known/acme-challenge/<token>` for the armed token, GET and HEAD only,
 * one answer consumed, nothing else answered, no redirect.  This module owns
 * the transport that decision never had: one `hfoc_lwip_tcp` listener on the
 * port the ACME server fetches (80), a bounded connection array, and the
 * writing of the exact response bytes.
 *
 * It decides nothing itself.  Every request line goes through
 * `appliance_acme_challenge_decide_line()`, and every byte written comes from
 * `appliance_production_acme_http01_build_response()`, so the same answer the
 * host proof pins is the answer the wire carries.
 *
 * The listener comes up **unarmed**: until the order path arms a token, every
 * request is the decision module's named refusal (`NO_CHALLENGE`), and the
 * production record says `armed=0`.
 */

#define APPLIANCE_PRODUCTION_ACME_HTTP01_VERSION 1U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_PORT 80U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT 2U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_LINE_MAX 640U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_RESPONSE_MAX 768U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_IDLE_TIMEOUT_MS 10000U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_RECORD_MAX 192U
#define APPLIANCE_PRODUCTION_ACME_HTTP01_GENERATION 1U

enum appliance_production_acme_http01_state {
    APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_UNINITIALIZED = 0,
    APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_LISTENING = 1,
    APPLIANCE_PRODUCTION_ACME_HTTP01_STATE_FAILED = 2,
};

struct appliance_production_acme_http01_connection {
    struct appliance_production_acme_http01 *owner;
    bool active;
    bool releasing;
    bool closing;
    bool peer_closed;
    bool aborted;
    struct tcp_pcb *pcb;
    char line[APPLIANCE_PRODUCTION_ACME_HTTP01_LINE_MAX];
    uint32_t line_length;
    char response[APPLIANCE_PRODUCTION_ACME_HTTP01_RESPONSE_MAX];
    uint32_t response_length;
    uint32_t response_offset;
    uint32_t last_activity_ms;
};

struct appliance_production_acme_http01 {
    uint32_t version;
    enum appliance_production_acme_http01_state state;
    uint32_t now_epoch_seconds;
    const char *reason;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_tcp_callback listener_callback;
    struct appliance_acme_challenge_arm challenge;
    struct appliance_production_acme_http01_connection
        connections[APPLIANCE_PRODUCTION_ACME_HTTP01_CONNECTION_COUNT];
    uint32_t active_count;
    uint32_t accept_count;
    uint32_t served_count;
    uint32_t refused_count;
    uint32_t write_failure_count;
    uint32_t close_count;
    char record[APPLIANCE_PRODUCTION_ACME_HTTP01_RECORD_MAX + 1U];
};

/*
 * Build the exact response for one decision.  `body` is the key authorization
 * `decide_line` returned; `include_body` is false for HEAD and for every
 * refusal.  Only `OK` is a 200; `METHOD_NOT_ALLOWED` is 405, the malformed and
 * oversized request lines are 400, and every other named refusal is 404.  The
 * Content-Length always names the body length, also for HEAD, and the
 * connection always closes.
 *
 * Returns false (and writes nothing) when the response does not fit the
 * caller's buffer, so an oversized answer is refused by name instead of being
 * truncated on the wire.
 */
bool appliance_production_acme_http01_build_response(
    enum appliance_acme_challenge_status status,
    bool include_body,
    const char *body,
    size_t body_length,
    char *out,
    size_t capacity,
    size_t *out_length);

/*
 * ITEM-508 increment 29: how many bytes of one received segment belong to the
 * request line.  A connection answers exactly one line and closes, so the
 * bytes after the first line ending are not this request's line and must not
 * reach the decision: a real HTTP/1.1 client sends its header lines in the
 * same segment, and carrying their CRLFs into the line made every such request
 * a malformed `400` on the wire (measured on the first wire-proof run).
 * Returns the length up to and including the first `\n`, or `length` when the
 * segment carries no line ending yet (the line is then still incomplete).
 */
size_t appliance_production_acme_http01_line_bytes(
    const char *bytes, size_t length);

enum appliance_production_acme_http01_state
appliance_production_acme_http01_init(
    struct appliance_production_acme_http01 *http01,
    const struct hfoc_lwip_netif *live_netif,
    const struct network_ipv4_config *config);

/*
 * Arm the one token the order path is answering for, or close it.  Arming
 * before the listener exists is `BAD_STATE`; the decision module owns every
 * other refusal.
 */
enum appliance_acme_challenge_status appliance_production_acme_http01_arm(
    struct appliance_production_acme_http01 *http01,
    const char *token,
    size_t token_length,
    const char *key_authorization,
    size_t key_authorization_length,
    uint32_t now_epoch_seconds);
void appliance_production_acme_http01_disarm(
    struct appliance_production_acme_http01 *http01);

/*
 * ITEM-508 increment 30: arm from the order path.  The token comes from the
 * flow's parsed `http-01` challenge and the answer from
 * `appliance_acme_flow_key_authorization()`, so neither is a caller literal
 * and the wire can only ever carry the key authorization of the challenge the
 * flow is actually answering.  This function decides nothing itself: the flow
 * refusals are named and mapped (`NULL` -> `NULL`, `BAD_STATE`/`KEY_REQUIRED`
 * -> `BAD_STATE`, `BUFFER_TOO_SMALL` -> `BUFFER_TOO_SMALL`), and every other
 * refusal is the decision module's own.  A refused arm leaves any previously
 * armed token exactly as it was.
 */
enum appliance_acme_challenge_status
appliance_production_acme_http01_arm_flow(
    struct appliance_production_acme_http01 *http01,
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    uint32_t now_epoch_seconds);

/*
 * Carry the validated wall clock the kernel loop owns.  The decision module
 * needs it to expire an armed token; without a poll the listener would answer
 * for a token whose 900 s window is long over.
 */
void appliance_production_acme_http01_poll(
    struct appliance_production_acme_http01 *http01,
    uint32_t now_epoch_seconds);

bool appliance_production_acme_http01_listening(
    const struct appliance_production_acme_http01 *http01);
bool appliance_production_acme_http01_armed(
    const struct appliance_production_acme_http01 *http01);
enum appliance_production_acme_http01_state
appliance_production_acme_http01_state(
    const struct appliance_production_acme_http01 *http01);
const char *appliance_production_acme_http01_reason(
    const struct appliance_production_acme_http01 *http01);
uint32_t appliance_production_acme_http01_port(void);
uint32_t appliance_production_acme_http01_accept_count(
    const struct appliance_production_acme_http01 *http01);
uint32_t appliance_production_acme_http01_served_count(
    const struct appliance_production_acme_http01 *http01);
uint32_t appliance_production_acme_http01_refused_count(
    const struct appliance_production_acme_http01 *http01);
uint32_t appliance_production_acme_http01_active_count(
    const struct appliance_production_acme_http01 *http01);

/* One bounded record, refreshed on every publish. */
const char *appliance_production_acme_http01_record(
    struct appliance_production_acme_http01 *http01);
const char *appliance_production_acme_http01_state_string(
    enum appliance_production_acme_http01_state state);

#endif
