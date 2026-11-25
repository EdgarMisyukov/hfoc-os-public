#ifndef HFOC_APPLIANCE_ACME_FLOW_H
#define HFOC_APPLIANCE_ACME_FLOW_H

#include "appliance_acme_jws.h"
#include "appliance_acme_message.h"
#include "appliance_acme_trust.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 5: the ACME order as a sequence of named steps.
 *
 * This is the client's spine, and it is deliberately transport-free: every
 * function either consumes a reply the caller already fetched or produces the
 * exact request body the caller must send, so the whole flow is provable on
 * the host with fixed data and no network.  The transport (and the HTTP-01
 * listener that answers the challenge) is the next increment.
 *
 * The item's rule that an unvalidated clock starts nothing is enforced here,
 * first: `begin` refuses with `BAD_CLOCK` and publishes a record before any
 * request is built, so "the order never started" is visible in the journal
 * rather than inferred from a missing request.
 *
 * Every request is a JWS (RFC 8555 section 6.2): the protected header carries
 * `alg`, the anti-replay `nonce` and the exact `url`, and either the public
 * JWK (the account is being created) or the account's `kid`.  The payload is
 * JSON built by this module from the values the caller supplied, never copied
 * from the wire.  Every step publishes one bounded record that names the step,
 * the state and the reason - never a nonce, a token or a key byte.
 */

#define APPLIANCE_ACME_FLOW_VERSION 1U
#define APPLIANCE_ACME_FLOW_REQUEST_MAX 2048U
#define APPLIANCE_ACME_FLOW_PAYLOAD_MAX 1024U
#define APPLIANCE_ACME_FLOW_PROTECTED_MAX 512U
#define APPLIANCE_ACME_FLOW_RECORD_MAX 320U
#define APPLIANCE_ACME_FLOW_IDENTIFIER_MAX APPLIANCE_ACME_IDENTIFIER_MAX
#define APPLIANCE_ACME_FLOW_NONCE_MAX 128U

enum appliance_acme_flow_state {
    APPLIANCE_ACME_FLOW_STATE_NONE = 0,
    /* No validated clock: the item starts nothing. */
    APPLIANCE_ACME_FLOW_STATE_AWAITING_CLOCK = 1,
    APPLIANCE_ACME_FLOW_STATE_READY = 2,
    APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING = 3,
    APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING = 4,
    APPLIANCE_ACME_FLOW_STATE_AUTHORIZATION_PENDING = 5,
    APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING = 6,
    APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING = 7,
    APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY = 8,
    APPLIANCE_ACME_FLOW_STATE_FAILED = 9,
    /* The downloaded chain verified against the named anchor. */
    APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_VERIFIED = 10,
};

enum appliance_acme_flow_status {
    APPLIANCE_ACME_FLOW_OK = 0,
    APPLIANCE_ACME_FLOW_NULL = 1,
    APPLIANCE_ACME_FLOW_BAD_STATE = 2,
    APPLIANCE_ACME_FLOW_BAD_CLOCK = 3,
    APPLIANCE_ACME_FLOW_BAD_REPLY = 4,
    APPLIANCE_ACME_FLOW_BAD_STATUS = 5,
    APPLIANCE_ACME_FLOW_BAD_NONCE = 6,
    APPLIANCE_ACME_FLOW_BAD_URL = 7,
    APPLIANCE_ACME_FLOW_TOO_MANY_IDENTIFIERS = 8,
    APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL = 9,
    APPLIANCE_ACME_FLOW_KEY_REQUIRED = 10,
    /* The certificate body is not a bounded chain of certificates. */
    APPLIANCE_ACME_FLOW_BAD_CHAIN = 11,
    /* The chain does not verify against the anchor for this name. */
    APPLIANCE_ACME_FLOW_UNTRUSTED = 12,
    /* ITEM-508 increment 22: a finalize without a CSR is never sent. */
    APPLIANCE_ACME_FLOW_CSR_REQUIRED = 13,
    APPLIANCE_ACME_FLOW_CSR_TOO_LARGE = 14,
};

enum appliance_acme_flow_step {
    APPLIANCE_ACME_FLOW_STEP_NONE = 0,
    APPLIANCE_ACME_FLOW_STEP_DIRECTORY = 1,
    APPLIANCE_ACME_FLOW_STEP_ACCOUNT = 2,
    APPLIANCE_ACME_FLOW_STEP_ORDER = 3,
    APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION = 4,
    APPLIANCE_ACME_FLOW_STEP_KEY_AUTHORIZATION = 5,
    APPLIANCE_ACME_FLOW_STEP_CHALLENGE = 6,
    APPLIANCE_ACME_FLOW_STEP_FINALIZE = 7,
    APPLIANCE_ACME_FLOW_STEP_CERTIFICATE = 8,
};

struct appliance_acme_flow {
    uint32_t version;
    bool initialized;
    enum appliance_acme_flow_state state;
    enum appliance_acme_flow_status last_status;
    enum appliance_acme_flow_step last_step;
    struct appliance_acme_directory directory;
    struct appliance_acme_url account_url;
    /*
     * ITEM-508 increment 19: the order URL is the `Location` of the newOrder
     * reply; a `processing` finalize polls it with POST-as-GET, so the caller
     * hands it over with the order body.
     */
    struct appliance_acme_url order_url;
    struct appliance_acme_order order;
    struct appliance_acme_authorization authorization;
    struct appliance_acme_challenge challenge;
    struct appliance_acme_url certificate_url;
    bool certificate_verified;
    struct appliance_acme_identifier identifiers[APPLIANCE_ACME_FLOW_IDENTIFIER_MAX];
    uint32_t identifier_count;
    /* The request the caller must send next, and the step it belongs to. */
    char request[APPLIANCE_ACME_FLOW_REQUEST_MAX];
    uint32_t request_length;
    /* A bounded publication of the last step; never a nonce, token or key. */
    char record[APPLIANCE_ACME_FLOW_RECORD_MAX];
};

/*
 * Start the flow.  `now_epoch_seconds` is the validated wall clock: zero
 * refuses the whole order with `BAD_CLOCK` before the directory is even read.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin(
    struct appliance_acme_flow *flow,
    const char *directory_json,
    size_t directory_length,
    uint32_t now_epoch_seconds);

/* Build the newAccount request.  The payload carries the public JWK. */
enum appliance_acme_flow_status appliance_acme_flow_begin_account(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/* Consume the account reply and the Location header the CA answered with. */
enum appliance_acme_flow_status appliance_acme_flow_account_result(
    struct appliance_acme_flow *flow,
    const char *account_json,
    size_t account_length,
    const char *account_url,
    size_t account_url_length);

/*
 * Declare the exact DNS names this order is for.  The names are copied and
 * bounded; the payload of newOrder is built from these copies, never from a
 * wire reply.
 */
enum appliance_acme_flow_status appliance_acme_flow_declare_identifiers(
    struct appliance_acme_flow *flow,
    const char *const *names,
    const size_t *name_lengths,
    uint32_t count);

/* Build the newOrder request for the identifiers the caller declared. */
enum appliance_acme_flow_status appliance_acme_flow_begin_order(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/*
 * Consume the order reply and the `Location` header the CA answered with; the
 * URL is kept for the finalize poll.
 */
enum appliance_acme_flow_status appliance_acme_flow_order_result(
    struct appliance_acme_flow *flow,
    const char *order_json,
    size_t order_length,
    const char *order_url,
    size_t order_url_length);

/*
 * Poll an order the finalize answered as still pending: POST-as-GET to the
 * stored order URL (empty payload, the account's `kid`, RFC 8555 section
 * 7.4).  Only `FINALIZE_PENDING` may poll; `finalize_result` reads the answer.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_order_poll(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/* POST-as-GET to the authorization the order named. */
enum appliance_acme_flow_status appliance_acme_flow_begin_authorization(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

enum appliance_acme_flow_status appliance_acme_flow_authorization_result(
    struct appliance_acme_flow *flow,
    const char *authorization_json,
    size_t authorization_length);

/* The key authorization the HTTP-01 answer serves (RFC 8555 section 8.1). */
enum appliance_acme_flow_status appliance_acme_flow_key_authorization(
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    char *out,
    size_t capacity,
    size_t *length_out);

/*
 * ITEM-508 increment 30: the token of the parsed `http-01` challenge - the URL
 * segment the listener's answer has to name.  `key_authorization` returns the
 * whole answer; arming the listener needs the token beside it, and the token
 * is the flow's own parsed value, never one a caller supplied.  A flow that is
 * not initialized or carries no `http-01`, and an empty token, are `BAD_STATE`;
 * a token that does not fit the caller's buffer is `BUFFER_TOO_SMALL` and
 * nothing is copied.
 */
enum appliance_acme_flow_status appliance_acme_flow_http01_token(
    const struct appliance_acme_flow *flow,
    char *out,
    size_t capacity,
    size_t *length_out);

/* POST {} to the http-01 challenge: the CA is told to start validating. */
enum appliance_acme_flow_status appliance_acme_flow_begin_challenge(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/*
 * Poll a challenge the CA answered `processing`: POST-as-GET to the same
 * challenge URL (empty payload, the account's `kid`, RFC 8555 section 7.5.1).
 * Only `CHALLENGE_PENDING` may poll; the state stays pending until
 * `challenge_result` reads a `valid` answer.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_challenge_poll(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

enum appliance_acme_flow_status appliance_acme_flow_challenge_result(
    struct appliance_acme_flow *flow,
    const char *challenge_json,
    size_t challenge_length);

/*
 * POST the finalize URL once the challenge has been answered.  ITEM-508
 * increment 22: the payload is `{"csr":"<unpadded base64url PKCS#10>"}` built
 * here from the caller's DER (RFC 8555 section 7.4); a missing or oversized
 * CSR is a named refusal and nothing is sent in its place.  The payload is
 * bounded by `APPLIANCE_ACME_FLOW_PAYLOAD_MAX` and never truncated.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_finalize(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    const unsigned char *csr,
    size_t csr_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/* The finalized order: valid, with the certificate URL it names. */
enum appliance_acme_flow_status appliance_acme_flow_finalize_result(
    struct appliance_acme_flow *flow,
    const char *order_json,
    size_t order_length);

/*
 * POST-as-GET to the certificate URL the finalized order named.  The request
 * is built exactly like the other POST-as-GET reads (empty payload, the
 * account's `kid`), so the caller downloads the chain through the same
 * transport as every other step; the state stays `CERTIFICATE_READY` until
 * the body arrives at `certificate_result`.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_certificate(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);

/*
 * The certificate step: decode the PEM chain the CA returned behind the
 * certificate URL, verify it against the anchor the caller bound and the
 * first identifier the order declared, and publish the DER chain for the
 * caller to store.  A chain the appliance cannot trust fails the order; a
 * caller buffer too small and an unvalidated clock leave it usable.
 */
enum appliance_acme_flow_status appliance_acme_flow_certificate_result(
    struct appliance_acme_flow *flow,
    struct appliance_acme_trust *trust,
    const char *pem,
    size_t pem_length,
    uint32_t now_epoch_seconds,
    unsigned char *der_out,
    size_t der_capacity,
    size_t *der_length_out,
    uint32_t *member_count_out);

const char *appliance_acme_flow_state_string(
    enum appliance_acme_flow_state state);
const char *appliance_acme_flow_status_string(
    enum appliance_acme_flow_status status);
const char *appliance_acme_flow_step_string(
    enum appliance_acme_flow_step step);

#endif
