#ifndef HFOC_APPLIANCE_PRODUCTION_ACME_ORDER_H
#define HFOC_APPLIANCE_PRODUCTION_ACME_ORDER_H

#include "appliance_acme_challenge.h"
#include "appliance_acme_flow.h"
#include "appliance_acme_renewal.h"
#include "appliance_acme_transport.h"
#include "appliance_acme_trust.h"
#include "appliance_production_certificate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 35: the order path ordinary boot runs.
 *
 * Increments 1-33 built the ACME client piece by piece, but every one of them
 * is reachable only from the fixture family or the host proofs: nothing on
 * ordinary boot drives an order.  Increment 34 gave the operator a durable
 * request; this module is the path that consumes it.  It drives one complete
 * order - directory, account, order, authorization, `http-01` challenge, the
 * production listener armed from the parsed challenge, finalize and the
 * certificate download - and ends with the verified chain adopted by the
 * production certificate owner and the renewal policy moved to the leaf's own
 * expiry.
 *
 * The module is transport-agnostic and policy-free:
 *
 *   * the caller injects the transport link, so the production binding and a
 *     scripted host peer are the same code path; the module opens no socket
 *     and never touches lwIP;
 *   * the caller injects the listener, so the module never links the socket
 *     half of the challenge, and a host proof can watch the arming;
 *   * the caller injects the anchor, the identity (the account key) and the
 *     renewal policy; the module decides nothing about when an order is
 *     wanted.  It is started explicitly and runs one order to a terminal
 *     state; the caller owns attempt accounting and the durable request.
 *
 * Fail closed, not half-done: an unvalidated instant starts nothing, an order
 * without identifiers is refused by name, an order that outlives its backstop
 * fails instead of waiting forever, a challenge the listener cannot answer is
 * a named failure, a chain that does not verify against the named anchor is
 * never adopted, and a renewal that refuses the verified expiry fails the
 * order rather than reporting a success the policy did not accept.  Every
 * transition publishes one bounded record `[PRODUCTION-ACME-ORDER] state=…
 * step=… …`; the record carries no nonce, token, key or certificate byte, and
 * a buffer that cannot hold the whole record publishes the named fallback
 * instead of a truncation.
 */

#define APPLIANCE_PRODUCTION_ACME_ORDER_VERSION 1U
#define APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX \
    APPLIANCE_ACME_FLOW_IDENTIFIER_MAX
#define APPLIANCE_PRODUCTION_ACME_ORDER_DER_MAX \
    (APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX + \
     APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX)
#define APPLIANCE_PRODUCTION_ACME_ORDER_CSR_MAX APPLIANCE_ACME_CSR_MAX
#define APPLIANCE_PRODUCTION_ACME_ORDER_NONCE_MAX APPLIANCE_ACME_HTTP_NONCE_MAX
/* The same backstop the fixture order proof uses; the transport has its own
 * 30 s and 60 s windows inside it. */
#define APPLIANCE_PRODUCTION_ACME_ORDER_DEADLINE_MS 240000U
#define APPLIANCE_PRODUCTION_ACME_ORDER_RECORD_MAX 320U

enum appliance_production_acme_order_state {
    APPLIANCE_PRODUCTION_ACME_ORDER_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_ACME_ORDER_STATE_IDLE = 1,
    APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING = 2,
    APPLIANCE_PRODUCTION_ACME_ORDER_STATE_DONE = 3,
    APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED = 4,
};

enum appliance_production_acme_order_status {
    APPLIANCE_PRODUCTION_ACME_ORDER_OK = 0,
    APPLIANCE_PRODUCTION_ACME_ORDER_NULL = 1,
    APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE = 2,
    /* No validated wall clock: nothing starts. */
    APPLIANCE_PRODUCTION_ACME_ORDER_BAD_CLOCK = 3,
    APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS = 4,
    APPLIANCE_PRODUCTION_ACME_ORDER_TOO_MANY_IDENTIFIERS = 5,
    /* An order is already in flight; one at a time. */
    APPLIANCE_PRODUCTION_ACME_ORDER_BUSY = 6,
    APPLIANCE_PRODUCTION_ACME_ORDER_KEY_REFUSED = 7,
    APPLIANCE_PRODUCTION_ACME_ORDER_ANCHOR_REFUSED = 8,
    APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED = 9,
    APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED = 10,
    APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED = 11,
    /* The listener could not answer the challenge; the order fails by name. */
    APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER = 12,
    APPLIANCE_PRODUCTION_ACME_ORDER_ADOPT_FAILED = 13,
    APPLIANCE_PRODUCTION_ACME_ORDER_RENEWAL_REFUSED = 14,
    APPLIANCE_PRODUCTION_ACME_ORDER_TIMEOUT = 15,
    APPLIANCE_PRODUCTION_ACME_ORDER_BUFFER_TOO_SMALL = 16,
};

/*
 * The listener the driver arms from the parsed `http-01` challenge.  The
 * production caller binds the ITEM-508 HTTP-01 listener through these
 * callbacks; a host proof binds a stub.  `arm_flow` returns the challenge
 * module's own status, so a refusal is named here exactly as it is there.
 */
struct appliance_production_acme_order_listener {
    void *context;
    bool (*listening)(void *context);
    enum appliance_acme_challenge_status (*arm_flow)(
        void *context,
        const struct appliance_acme_flow *flow,
        const struct appliance_acme_key *key,
        uint32_t now_epoch_seconds);
    void (*disarm)(void *context);
};

/* The step the running order is on; the flow names it in the record too. */
enum appliance_production_acme_order_stage {
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE = 0,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY = 1,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT = 2,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER = 3,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION = 4,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE = 5,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL = 6,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE = 7,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL = 8,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE = 9,
    APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE = 10,
};

struct appliance_production_acme_order {
    uint32_t version;
    bool initialized;
    bool configured;
    enum appliance_production_acme_order_state state;
    enum appliance_production_acme_order_stage stage;
    enum appliance_production_acme_order_status last_status;
    struct appliance_acme_transport transport;
    struct appliance_acme_flow flow;
    struct appliance_acme_key key;
    struct appliance_acme_trust trust;
    const struct appliance_acme_transport_link *link;
    struct appliance_production_certificate *identity;
    struct appliance_acme_renewal *renewal;
    struct appliance_production_acme_order_listener listener;
    const unsigned char *anchor_der;
    size_t anchor_length;
    const char *directory_url;
    char identifiers[APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX]
                    [APPLIANCE_ACME_NAME_MAX + 1U];
    size_t identifier_lengths[APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX];
    uint32_t identifier_count;
    /* The validated instant the order started at; zero means none. */
    uint32_t now_epoch_seconds;
    uint32_t deadline_ms;
    bool exchange_active;
    bool listener_armed;
    unsigned char der[APPLIANCE_PRODUCTION_ACME_ORDER_DER_MAX];
    size_t der_length;
    uint32_t certificate_members;
    uint32_t leaf_not_after_epoch_seconds;
    /* The certificate reply's body, held for the finish step. */
    uint8_t certificate_body[APPLIANCE_ACME_HTTP_BODY_MAX];
    size_t certificate_body_length;
    unsigned char csr[APPLIANCE_PRODUCTION_ACME_ORDER_CSR_MAX];
    size_t csr_length;
    char nonce[APPLIANCE_PRODUCTION_ACME_ORDER_NONCE_MAX + 1U];
    uint32_t nonce_length;
    char record[APPLIANCE_PRODUCTION_ACME_ORDER_RECORD_MAX + 1U];
};

/* Prepare an unconfigured driver: IDLE, nothing borrowed yet. */
void appliance_production_acme_order_init(
    struct appliance_production_acme_order *order);

/*
 * Bind everything the driver borrows.  The pointers stay owned by the caller
 * and must outlive the order.  A NULL link, identity, renewal, anchor or
 * directory URL is refused by name and leaves the driver unconfigured.
 */
enum appliance_production_acme_order_status
appliance_production_acme_order_configure(
    struct appliance_production_acme_order *order,
    const struct appliance_acme_transport_link *link,
    struct appliance_production_certificate *identity,
    struct appliance_acme_renewal *renewal,
    const struct appliance_production_acme_order_listener *listener,
    const unsigned char *anchor_der,
    size_t anchor_length,
    const char *directory_url);

/*
 * Start one order for the exact identifiers the caller declares.  Refused
 * while another order is in flight, without identifiers, over the identifier
 * bound, on an unvalidated clock, or when the account key or the anchor is
 * refused.  The names are copied, so the caller's buffers may go away.
 */
enum appliance_production_acme_order_status
appliance_production_acme_order_start(
    struct appliance_production_acme_order *order,
    const char *const *names,
    const size_t *name_lengths,
    uint32_t count,
    uint32_t now_epoch_seconds);

/*
 * One non-blocking step.  Returns OK while the order runs or has finished; a
 * terminal failure is published in `state`/`last_status` and in the record,
 * and the caller reads it there instead of from this return value.
 */
enum appliance_production_acme_order_status
appliance_production_acme_order_tick(
    struct appliance_production_acme_order *order,
    uint32_t now_ms);

/* Drop a running order: the listener is disarmed and the state is FAILED. */
void appliance_production_acme_order_abort(
    struct appliance_production_acme_order *order);

enum appliance_production_acme_order_state
appliance_production_acme_order_state(
    const struct appliance_production_acme_order *order);
enum appliance_production_acme_order_stage
appliance_production_acme_order_stage(
    const struct appliance_production_acme_order *order);
enum appliance_production_acme_order_status
appliance_production_acme_order_last_status(
    const struct appliance_production_acme_order *order);
const char *appliance_production_acme_order_record(
    const struct appliance_production_acme_order *order);
/* The adopted chain's member count and the leaf expiry, for the caller. */
uint32_t appliance_production_acme_order_certificate_members(
    const struct appliance_production_acme_order *order);
uint32_t appliance_production_acme_order_leaf_not_after(
    const struct appliance_production_acme_order *order);

const char *appliance_production_acme_order_state_string(
    enum appliance_production_acme_order_state state);
const char *appliance_production_acme_order_stage_string(
    enum appliance_production_acme_order_stage stage);
const char *appliance_production_acme_order_status_string(
    enum appliance_production_acme_order_status status);

#endif
