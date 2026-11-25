#ifndef HFOC_APPLIANCE_PRODUCTION_ACME_SERVICE_H
#define HFOC_APPLIANCE_PRODUCTION_ACME_SERVICE_H

#include "appliance_acme_account.h"
#include "appliance_acme_renewal.h"
#include "appliance_production_certificate.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 26: the ACME service ordinary boot owns.
 *
 * The whole ACME path before this increment is reachable only from the fixture
 * order driver and the host proofs: nothing on the ordinary boot path binds the
 * account or holds the renewal policy, so every boot would start with no
 * account and no notion of when the held certificate expires.  This module is
 * the one owner ordinary boot calls:
 *
 *   * the account key is the persisted appliance identity key (increment 24).
 *     `poll` binds it as soon as the certificate owner has a prepared identity
 *     and publishes the public RFC 7638 thumbprint - the name the CA knows the
 *     account by - never a key byte;
 *   * the renewal policy (increment 9) lives here, so the caller that verifies
 *     a chain feeds `certificate_obtained` into the same state the panel and
 *     the boot record read.  The policy itself is unchanged: renewal starts
 *     thirty days before expiry, a failed attempt waits six hours, five
 *     attempts exhaust the budget, and an unvalidated clock is refused;
 *   * one bounded record `[ACME-SERVICE] v=1 state=… account=… thumbprint=…
 *     renewal=… expires=… attempts=…` publishes the whole state.  The record
 *     carries a public thumbprint and policy numbers; it never carries a key,
 *     a nonce, a token or a certificate byte.
 *
 * The service is deliberately transport-free and socket-free: it binds, it
 * decides, it publishes.  Speaking to a CA, the HTTP-01 listener and the
 * order driver on the production path are the following increments, and a
 * certificate is not "renewed" here - only decided.
 *
 * Refusals are named, not hidden.  An identity that is not prepared yet leaves
 * the service `AWAITING_IDENTITY` and the next poll retries, because the
 * certificate owner prepares the identity asynchronously; an identity whose
 * key the JWS module rejects leaves it `REFUSED` with the account status, and
 * the retry is the next poll.  A refused bind always leaves the account
 * unbound: the service never half-binds, and it never publishes a thumbprint
 * that belongs to a key the last bind did not accept.
 *
 * The renewal policy is in memory, exactly as increment 9 defined it.  A
 * reboot therefore forgets a previously verified expiry until the certificate
 * owner or the order path feeds it again; persisting the policy is a separate
 * decision and is not fabricated here.
 */

#define APPLIANCE_PRODUCTION_ACME_SERVICE_VERSION 1U
/*
 * "[ACME-SERVICE] v=1 state=BOUND account=bound thumbprint=<43> renewal=<19>
 *  expires=<10> attempts=<10>" needs fewer than 224 bytes; the bound refuses
 * anything longer instead of truncating the record.
 */
#define APPLIANCE_PRODUCTION_ACME_SERVICE_RECORD_MAX 224U

enum appliance_production_acme_service_state {
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_UNINITIALIZED = 1,
    /* The identity is not prepared (yet); the next poll retries. */
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY = 2,
    /* The account key is bound and the thumbprint is published. */
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND = 3,
    /* The identity exists but the account key was refused by name. */
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED = 4,
    /*
     * ITEM-508 increment 34: the operator turned ACME off in the durable
     * control record.  A disabled service binds nothing, evaluates no renewal
     * and starts nothing; the record names the state and the poll keeps
     * reading the control so enabling it again takes effect on the next turn.
     */
    APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_DISABLED = 5,
};

enum appliance_production_acme_service_status {
    APPLIANCE_PRODUCTION_ACME_SERVICE_OK = 0,
    APPLIANCE_PRODUCTION_ACME_SERVICE_NULL = 1,
    APPLIANCE_PRODUCTION_ACME_SERVICE_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_ACME_SERVICE_ALREADY_INITIALIZED = 3,
};

struct appliance_production_acme_service {
    uint32_t version;
    bool initialized;
    enum appliance_production_acme_service_state state;
    bool account_bound;
    enum appliance_acme_account_status account_status;
    /*
     * The public half of the bound key: JWK and thumbprint.  The private
     * context lives inside the JWS module and is never copied here.
     */
    struct appliance_acme_key key;
    /* The one renewal policy the production ACME path owns. */
    struct appliance_acme_renewal renewal;
    enum appliance_acme_renewal_decision last_decision;
    /* Counters for the record and the panel; no material. */
    uint32_t bind_attempts;
    uint32_t polls;
    /*
     * ITEM-508 increment 34: the durable control as the caller published it.
     * `control_published` is false until the caller hands over the record
     * value; a service whose caller never does keeps the pre-increment
     * behavior (enabled) so the earlier proofs and a build without the record
     * are unchanged.  Production publishes the record every poll, and an
     * absent record there is the off state.
     */
    bool control_published;
    bool enabled;
    bool order_requested;
    uint32_t order_requested_at;
    char record[APPLIANCE_PRODUCTION_ACME_SERVICE_RECORD_MAX + 1U];
};

/*
 * Create the service over the certificate owner's identity view.  `identity`
 * may be NULL or unprepared: the service then waits, and `poll` binds when it
 * becomes available.  The identity object stays borrowed; the service copies
 * only the public JWK the JWS module derives.
 */
enum appliance_production_acme_service_status
appliance_production_acme_service_init(
    struct appliance_production_acme_service *service,
    const struct appliance_production_certificate *identity);

/*
 * One service step: bind the account if it is not bound yet, evaluate the
 * renewal policy against the validated instant and republish the record.
 * `now_epoch_seconds` is the validated wall clock; zero is accepted and the
 * policy refuses the decision with `bad-clock`, exactly as increment 9 says.
 */
enum appliance_production_acme_service_status
appliance_production_acme_service_poll(
    struct appliance_production_acme_service *service,
    const struct appliance_production_certificate *identity,
    uint32_t now_epoch_seconds);

/*
 * ITEM-508 increment 34: publish the durable control the service follows.
 * Called by the ordinary-boot composition with the value it read from the
 * ACME control record; a service that is disabled binds nothing and evaluates
 * no renewal, and enabling it again takes effect on the next poll.  The order
 * request is held, not executed: the order path that consumes it is the next
 * increment, and until then the control page and the
 * `[PRODUCTION-ACME-CONTROL]` record name it as requested rather than
 * pretending an order started.
 */
void appliance_production_acme_service_set_control(
    struct appliance_production_acme_service *service,
    bool enabled,
    bool order_requested,
    uint32_t order_requested_at);
bool appliance_production_acme_service_enabled(
    const struct appliance_production_acme_service *service);
bool appliance_production_acme_service_order_requested(
    const struct appliance_production_acme_service *service);

/*
 * The order path verified a chain: move the policy's expiry and republish.
 * This is the only call that moves the expiry, and it forwards to the policy
 * unchanged - an unvalidated clock or an expiry that does not outlive it is
 * refused by name and changes nothing.
 */
enum appliance_acme_renewal_status
appliance_production_acme_service_certificate_obtained(
    struct appliance_production_acme_service *service,
    uint32_t now_epoch_seconds,
    uint32_t expires_at_epoch_seconds);

/*
 * The policy the service owns, for the order path that starts and fails
 * attempts.  It is a policy: no key, no nonce, no token.
 */
struct appliance_acme_renewal *appliance_production_acme_service_renewal(
    struct appliance_production_acme_service *service);

const char *appliance_production_acme_service_record(
    const struct appliance_production_acme_service *service);
enum appliance_production_acme_service_state
appliance_production_acme_service_state(
    const struct appliance_production_acme_service *service);
bool appliance_production_acme_service_account_bound(
    const struct appliance_production_acme_service *service);
enum appliance_acme_account_status
appliance_production_acme_service_account_status(
    const struct appliance_production_acme_service *service);
enum appliance_acme_renewal_decision
appliance_production_acme_service_renewal_decision(
    const struct appliance_production_acme_service *service);

const char *appliance_production_acme_service_state_string(
    enum appliance_production_acme_service_state state);
const char *appliance_production_acme_service_status_string(
    enum appliance_production_acme_service_status status);

#endif
