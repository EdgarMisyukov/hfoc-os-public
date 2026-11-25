#ifndef HFOC_APPLIANCE_ACME_RENEWAL_H
#define HFOC_APPLIANCE_ACME_RENEWAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 9: when the appliance renews, and when it stops.
 *
 * The order exists so the appliance holds a certificate it obtained itself.
 * A certificate expires, and the appliance has to ask for the next one before
 * that happens - while it still has a working identity - but it must not ask
 * forever: an unreachable CA, a refused order and a wrong name would otherwise
 * turn into an unbounded stream of requests.  This module owns exactly that
 * decision and nothing above it:
 *
 *   * the instant the verified certificate stops being valid is the only clock
 *     the decision runs on.  It is set by `certificate_obtained`, which the
 *     caller invokes after the increment 8 step verified a chain, so a
 *     certificate that was merely downloaded never moves it;
 *   * renewal starts inside a thirty-day window before expiry, never earlier,
 *     so a fresh certificate is not re-requested on every boot;
 *   * an unvalidated clock (zero) refuses the whole decision with `BAD_CLOCK`
 *     before any window is computed - the item starts nothing without a
 *     validated wall clock, and a clock that moved backwards since the last
 *     attempt is not a validated clock either;
 *   * an appliance with no certificate yet has nothing to renew: the decision
 *     is `NO_CERTIFICATE` and the first issuance remains a different path,
 *     which keeps "renewal" from silently becoming "enrolment";
 *   * a failed attempt is retried no sooner than six hours later, and at most
 *     five attempts are made before the decision becomes
 *     `ATTEMPTS_EXHAUSTED`: at that point the appliance stops asking and the
 *     operator has to act.  `attempts_reset` is that action;
 *   * a certificate that verified successfully clears the attempt counter and
 *     moves the expiry, so the next decision runs on the new certificate.
 *
 * Every accepted call publishes one bounded record naming the fact, the
 * expiry, the attempt count and the next attempt instant; a refusal returns its
 * named status and leaves the state - and the record - as it was.  No record,
 * and no field of this structure, ever carries a key, a nonce, a token or a
 * certificate byte: the module is a policy, and it holds no material.
 */

#define APPLIANCE_ACME_RENEWAL_VERSION 1U
/* Renew thirty days before expiry; the item's own margin. */
#define APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS UINT32_C(2592000)
/* One failed attempt is retried no sooner than six hours later. */
#define APPLIANCE_ACME_RENEWAL_RETRY_SECONDS UINT32_C(21600)
/* Bounded attempts since the last success; then the operator is required. */
#define APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX 5U
#define APPLIANCE_ACME_RENEWAL_RECORD_MAX 160U

enum appliance_acme_renewal_outcome {
    /* No attempt has finished since the last success. */
    APPLIANCE_ACME_RENEWAL_OUTCOME_NONE = 0,
    APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED = 1,
    APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED = 2,
};

enum appliance_acme_renewal_decision {
    /* NULL or uninitialized: the caller has no renewal state to decide on. */
    APPLIANCE_ACME_RENEWAL_DECISION_NONE = 0,
    /* The clock handed in is not a validated wall clock. */
    APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK = 1,
    /* No verified certificate yet: this is an enrolment, not a renewal. */
    APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE = 2,
    APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED = 3,
    APPLIANCE_ACME_RENEWAL_DECISION_DUE = 4,
    /* A failed attempt is still inside its retry window. */
    APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER = 5,
    /* The attempt budget is spent; the operator has to act. */
    APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED = 6,
};

enum appliance_acme_renewal_status {
    APPLIANCE_ACME_RENEWAL_OK = 0,
    APPLIANCE_ACME_RENEWAL_NULL = 1,
    APPLIANCE_ACME_RENEWAL_BAD_STATE = 2,
    APPLIANCE_ACME_RENEWAL_BAD_CLOCK = 3,
    /* The decision is not DUE, so no attempt may start. */
    APPLIANCE_ACME_RENEWAL_NOT_DUE = 4,
    /* No attempt was started, so none can be finished. */
    APPLIANCE_ACME_RENEWAL_NOT_ATTEMPTED = 5,
    APPLIANCE_ACME_RENEWAL_ATTEMPTS_EXHAUSTED = 6,
    /* An expiry that does not outlive the instant it was verified. */
    APPLIANCE_ACME_RENEWAL_BAD_VALUE = 7,
    APPLIANCE_ACME_RENEWAL_NO_CERTIFICATE = 8,
};

struct appliance_acme_renewal {
    uint32_t version;
    bool initialized;
    /* The instant the verified certificate stops being valid; 0 = none yet. */
    uint32_t expires_at_epoch_seconds;
    /* Attempts since the last success. */
    uint32_t attempts;
    /* When the last attempt started (or ended, for the retry window); 0 = none. */
    uint32_t last_attempt_epoch_seconds;
    enum appliance_acme_renewal_outcome last_outcome;
    uint32_t evaluation_count;
    enum appliance_acme_renewal_decision last_decision;
    /* The instant a RETRY_AFTER decision waits for; 0 when there is none. */
    uint32_t next_attempt_epoch_seconds;
    /* One bounded publication of the last call; never any key or secret. */
    char record[APPLIANCE_ACME_RENEWAL_RECORD_MAX + 1U];
};

/* An appliance with no certificate and no attempts: the empty state. */
void appliance_acme_renewal_init(struct appliance_acme_renewal *renewal);

/*
 * The certificate step verified and published a chain: this is the one call
 * that moves the expiry.  `now_epoch_seconds` must be validated and the new
 * expiry must outlive it; a success clears the attempt counter and records the
 * success, so the next decision runs against the certificate just obtained.
 */
enum appliance_acme_renewal_status appliance_acme_renewal_certificate_obtained(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds,
    uint32_t expires_at_epoch_seconds);

/*
 * The decision, and the record that publishes it.  This call changes nothing
 * but the counters and the published record; it is safe to call on every boot
 * and on every panel read.
 */
enum appliance_acme_renewal_decision appliance_acme_renewal_evaluate(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds);

/*
 * Start one attempt.  It runs only when the decision is `DUE`: a certificate
 * that is still fresh (`NOT_NEEDED`), a retry window that has not elapsed
 * (`RETRY_AFTER`) and a spent budget (`ATTEMPTS_EXHAUSTED`) all refuse with
 * `NOT_DUE`/`ATTEMPTS_EXHAUSTED` and count nothing, so a caller that polls
 * cannot multiply attempts.
 */
enum appliance_acme_renewal_status appliance_acme_renewal_attempt_started(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds);

/*
 * End the started attempt as failed.  The retry window runs from this instant,
 * so a caller reports the failure when it happened, not when it started.
 */
enum appliance_acme_renewal_status appliance_acme_renewal_attempt_failed(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds);

/*
 * The operator's action after `ATTEMPTS_EXHAUSTED`: clear the counter and the
 * failure so a new decision may be DUE again.  The known expiry is kept - the
 * appliance still has whatever certificate it had.
 */
enum appliance_acme_renewal_status appliance_acme_renewal_attempts_reset(
    struct appliance_acme_renewal *renewal);

/* The empty state again: no certificate, no attempts, no record. */
void appliance_acme_renewal_reset(struct appliance_acme_renewal *renewal);

const char *appliance_acme_renewal_record(
    const struct appliance_acme_renewal *renewal);
enum appliance_acme_renewal_decision appliance_acme_renewal_last_decision(
    const struct appliance_acme_renewal *renewal);
enum appliance_acme_renewal_outcome appliance_acme_renewal_last_outcome(
    const struct appliance_acme_renewal *renewal);
uint32_t appliance_acme_renewal_expires_at(
    const struct appliance_acme_renewal *renewal);
uint32_t appliance_acme_renewal_attempts(
    const struct appliance_acme_renewal *renewal);
uint32_t appliance_acme_renewal_last_attempt_epoch_seconds(
    const struct appliance_acme_renewal *renewal);
uint32_t appliance_acme_renewal_next_attempt_epoch_seconds(
    const struct appliance_acme_renewal *renewal);

const char *appliance_acme_renewal_decision_string(
    enum appliance_acme_renewal_decision decision);
const char *appliance_acme_renewal_status_string(
    enum appliance_acme_renewal_status status);
const char *appliance_acme_renewal_outcome_string(
    enum appliance_acme_renewal_outcome outcome);

#endif
