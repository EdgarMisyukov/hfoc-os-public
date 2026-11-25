#include "appliance_acme_renewal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 9: the renewal policy.  Nothing here talks to a CA, opens
 * a socket or holds material: the caller hands in a validated instant and gets
 * back the decision, and every accepted call publishes one bounded record.
 */

#define APPLIANCE_ACME_RENEWAL_VERSION_CURRENT 1U

/*
 * A bounded text builder: the record never grows past its buffer and never
 * truncates silently, so a caller that reads it reads a complete record.
 */
struct appliance_acme_renewal_text {
    char *buffer;
    size_t capacity;
    size_t length;
};

static void appliance_acme_renewal_text_reset(
    struct appliance_acme_renewal_text *text,
    char *buffer,
    size_t capacity)
{
    text->buffer = buffer;
    text->capacity = capacity;
    text->length = 0U;
    if (capacity > 0U) {
        buffer[0] = '\0';
    }
}

static void appliance_acme_renewal_text_append(
    struct appliance_acme_renewal_text *text,
    const char *value)
{
    const size_t length = strlen(value);

    if (length == 0U || text->capacity == 0U) {
        return;
    }
    if (text->length + length + 1U > text->capacity) {
        return;
    }
    memcpy(&text->buffer[text->length], value, length);
    text->length += length;
    text->buffer[text->length] = '\0';
}

static void appliance_acme_renewal_text_append_u32(
    struct appliance_acme_renewal_text *text,
    uint32_t value)
{
    char digits[11];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    } while (value != 0U);
    while (count > 0U) {
        --count;
        if (text->capacity > 0U && text->length + 2U <= text->capacity) {
            text->buffer[text->length] = digits[count];
            ++text->length;
            text->buffer[text->length] = '\0';
        }
    }
}

/*
 * The one shape every accepted call publishes.  It names the fact, the expiry
 * the decision runs on, the attempt count and the next attempt instant; it
 * never carries a key, a nonce, a token or a certificate byte.
 */
static void appliance_acme_renewal_publish(
    struct appliance_acme_renewal *renewal,
    const char *fact)
{
    struct appliance_acme_renewal_text text;

    appliance_acme_renewal_text_reset(
        &text, renewal->record, sizeof(renewal->record));
    appliance_acme_renewal_text_append(&text, "[ACME-RENEWAL] ");
    appliance_acme_renewal_text_append(&text, fact);
    appliance_acme_renewal_text_append(&text, " expires=");
    appliance_acme_renewal_text_append_u32(
        &text, renewal->expires_at_epoch_seconds);
    appliance_acme_renewal_text_append(&text, " attempts=");
    appliance_acme_renewal_text_append_u32(&text, renewal->attempts);
    appliance_acme_renewal_text_append(&text, " next=");
    appliance_acme_renewal_text_append_u32(
        &text, renewal->next_attempt_epoch_seconds);
}

static void appliance_acme_renewal_publish_decision(
    struct appliance_acme_renewal *renewal)
{
    struct appliance_acme_renewal_text text;

    appliance_acme_renewal_text_reset(
        &text, renewal->record, sizeof(renewal->record));
    appliance_acme_renewal_text_append(&text, "[ACME-RENEWAL] decision=");
    appliance_acme_renewal_text_append(
        &text,
        appliance_acme_renewal_decision_string(renewal->last_decision));
    appliance_acme_renewal_text_append(&text, " expires=");
    appliance_acme_renewal_text_append_u32(
        &text, renewal->expires_at_epoch_seconds);
    appliance_acme_renewal_text_append(&text, " attempts=");
    appliance_acme_renewal_text_append_u32(&text, renewal->attempts);
    appliance_acme_renewal_text_append(&text, " next=");
    appliance_acme_renewal_text_append_u32(
        &text, renewal->next_attempt_epoch_seconds);
}

static uint32_t appliance_acme_renewal_add_saturating(
    uint32_t left,
    uint32_t right)
{
    if (left > UINT32_MAX - right) {
        return UINT32_MAX;
    }
    return left + right;
}

/*
 * The decision itself, without publishing: it reads the state and the instant
 * and changes nothing, so every entry point runs the same policy.
 */
static enum appliance_acme_renewal_decision appliance_acme_renewal_decide(
    const struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds,
    uint32_t *next_attempt_out)
{
    uint32_t next_attempt = 0U;

    if (!renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    }
    if (now_epoch_seconds == 0U) {
        return APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK;
    }
    if (renewal->expires_at_epoch_seconds == 0U) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE;
    }
    if (renewal->attempts >= APPLIANCE_ACME_RENEWAL_ATTEMPT_MAX) {
        return APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED;
    }
    if (renewal->last_outcome == APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED &&
        renewal->last_attempt_epoch_seconds != 0U) {
        if (now_epoch_seconds < renewal->last_attempt_epoch_seconds) {
            return APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK;
        }
        next_attempt = appliance_acme_renewal_add_saturating(
            renewal->last_attempt_epoch_seconds,
            APPLIANCE_ACME_RENEWAL_RETRY_SECONDS);
        if (now_epoch_seconds - renewal->last_attempt_epoch_seconds <
            APPLIANCE_ACME_RENEWAL_RETRY_SECONDS) {
            *next_attempt_out = next_attempt;
            return APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER;
        }
    }
    if (renewal->expires_at_epoch_seconds > now_epoch_seconds &&
        renewal->expires_at_epoch_seconds - now_epoch_seconds >
            APPLIANCE_ACME_RENEWAL_WINDOW_SECONDS) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED;
    }
    return APPLIANCE_ACME_RENEWAL_DECISION_DUE;
}

void appliance_acme_renewal_init(struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL) {
        return;
    }
    memset(renewal, 0, sizeof(*renewal));
    renewal->version = APPLIANCE_ACME_RENEWAL_VERSION_CURRENT;
    renewal->initialized = true;
    renewal->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    renewal->last_outcome = APPLIANCE_ACME_RENEWAL_OUTCOME_NONE;
}

void appliance_acme_renewal_reset(struct appliance_acme_renewal *renewal)
{
    appliance_acme_renewal_init(renewal);
}

enum appliance_acme_renewal_decision appliance_acme_renewal_evaluate(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds)
{
    uint32_t next_attempt = 0U;
    enum appliance_acme_renewal_decision decision;

    if (renewal == NULL || !renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    }
    decision = appliance_acme_renewal_decide(
        renewal, now_epoch_seconds, &next_attempt);
    renewal->evaluation_count += 1U;
    renewal->last_decision = decision;
    renewal->next_attempt_epoch_seconds = next_attempt;
    appliance_acme_renewal_publish_decision(renewal);
    return decision;
}

enum appliance_acme_renewal_status appliance_acme_renewal_attempt_started(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds)
{
    enum appliance_acme_renewal_decision decision;

    if (renewal == NULL) {
        return APPLIANCE_ACME_RENEWAL_NULL;
    }
    if (!renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    }
    /*
     * One order at a time: an attempt that started and has not finished yet
     * is already asking the CA, and a caller that polls must not multiply it.
     * The clock keeps its precedence: an unvalidated instant is refused by
     * `evaluate` as `BAD_CLOCK` below, even while an attempt is in flight.
     */
    if (now_epoch_seconds != 0U &&
        renewal->last_outcome == APPLIANCE_ACME_RENEWAL_OUTCOME_NONE &&
        renewal->attempts != 0U &&
        renewal->last_attempt_epoch_seconds != 0U) {
        return APPLIANCE_ACME_RENEWAL_NOT_DUE;
    }
    decision = appliance_acme_renewal_evaluate(renewal, now_epoch_seconds);
    switch (decision) {
    case APPLIANCE_ACME_RENEWAL_DECISION_DUE:
        break;
    case APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK:
        return APPLIANCE_ACME_RENEWAL_BAD_CLOCK;
    case APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE:
        return APPLIANCE_ACME_RENEWAL_NO_CERTIFICATE;
    case APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED:
        return APPLIANCE_ACME_RENEWAL_ATTEMPTS_EXHAUSTED;
    case APPLIANCE_ACME_RENEWAL_DECISION_NONE:
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    case APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED:
    case APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER:
    default:
        return APPLIANCE_ACME_RENEWAL_NOT_DUE;
    }
    renewal->attempts += 1U;
    renewal->last_attempt_epoch_seconds = now_epoch_seconds;
    renewal->last_outcome = APPLIANCE_ACME_RENEWAL_OUTCOME_NONE;
    renewal->next_attempt_epoch_seconds = 0U;
    appliance_acme_renewal_publish(renewal, "attempt=started");
    return APPLIANCE_ACME_RENEWAL_OK;
}

enum appliance_acme_renewal_status appliance_acme_renewal_attempt_failed(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds)
{
    if (renewal == NULL) {
        return APPLIANCE_ACME_RENEWAL_NULL;
    }
    if (!renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    }
    if (now_epoch_seconds == 0U ||
        (renewal->last_attempt_epoch_seconds != 0U &&
         now_epoch_seconds < renewal->last_attempt_epoch_seconds)) {
        return APPLIANCE_ACME_RENEWAL_BAD_CLOCK;
    }
    if (renewal->last_outcome != APPLIANCE_ACME_RENEWAL_OUTCOME_NONE) {
        return APPLIANCE_ACME_RENEWAL_NOT_ATTEMPTED;
    }
    renewal->last_outcome = APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED;
    renewal->last_attempt_epoch_seconds = now_epoch_seconds;
    renewal->next_attempt_epoch_seconds =
        appliance_acme_renewal_add_saturating(
            now_epoch_seconds, APPLIANCE_ACME_RENEWAL_RETRY_SECONDS);
    renewal->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER;
    appliance_acme_renewal_publish(renewal, "attempt=failed");
    return APPLIANCE_ACME_RENEWAL_OK;
}

enum appliance_acme_renewal_status appliance_acme_renewal_certificate_obtained(
    struct appliance_acme_renewal *renewal,
    uint32_t now_epoch_seconds,
    uint32_t expires_at_epoch_seconds)
{
    if (renewal == NULL) {
        return APPLIANCE_ACME_RENEWAL_NULL;
    }
    if (!renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    }
    if (now_epoch_seconds == 0U) {
        return APPLIANCE_ACME_RENEWAL_BAD_CLOCK;
    }
    if (expires_at_epoch_seconds == 0U ||
        expires_at_epoch_seconds <= now_epoch_seconds) {
        return APPLIANCE_ACME_RENEWAL_BAD_VALUE;
    }
    renewal->expires_at_epoch_seconds = expires_at_epoch_seconds;
    renewal->attempts = 0U;
    renewal->last_attempt_epoch_seconds = now_epoch_seconds;
    renewal->last_outcome = APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED;
    renewal->next_attempt_epoch_seconds = 0U;
    /*
     * A success is not a decision: whether the new certificate is due again is
     * computed by `evaluate`, and a short-lived one may well be due at once.
     */
    renewal->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    appliance_acme_renewal_publish(renewal, "attempt=succeeded");
    return APPLIANCE_ACME_RENEWAL_OK;
}

enum appliance_acme_renewal_status appliance_acme_renewal_attempts_reset(
    struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL) {
        return APPLIANCE_ACME_RENEWAL_NULL;
    }
    if (!renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    }
    renewal->attempts = 0U;
    renewal->last_attempt_epoch_seconds = 0U;
    renewal->last_outcome = APPLIANCE_ACME_RENEWAL_OUTCOME_NONE;
    renewal->next_attempt_epoch_seconds = 0U;
    renewal->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    appliance_acme_renewal_publish(renewal, "attempts=reset");
    return APPLIANCE_ACME_RENEWAL_OK;
}

const char *appliance_acme_renewal_record(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL) {
        return "";
    }
    return renewal->record;
}

enum appliance_acme_renewal_decision appliance_acme_renewal_last_decision(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    }
    return renewal->last_decision;
}

enum appliance_acme_renewal_outcome appliance_acme_renewal_last_outcome(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return APPLIANCE_ACME_RENEWAL_OUTCOME_NONE;
    }
    return renewal->last_outcome;
}

uint32_t appliance_acme_renewal_expires_at(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return 0U;
    }
    return renewal->expires_at_epoch_seconds;
}

uint32_t appliance_acme_renewal_attempts(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return 0U;
    }
    return renewal->attempts;
}

uint32_t appliance_acme_renewal_last_attempt_epoch_seconds(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return 0U;
    }
    return renewal->last_attempt_epoch_seconds;
}

uint32_t appliance_acme_renewal_next_attempt_epoch_seconds(
    const struct appliance_acme_renewal *renewal)
{
    if (renewal == NULL || !renewal->initialized) {
        return 0U;
    }
    return renewal->next_attempt_epoch_seconds;
}

const char *appliance_acme_renewal_decision_string(
    enum appliance_acme_renewal_decision decision)
{
    switch (decision) {
    case APPLIANCE_ACME_RENEWAL_DECISION_NONE:
        return "none";
    case APPLIANCE_ACME_RENEWAL_DECISION_BAD_CLOCK:
        return "bad-clock";
    case APPLIANCE_ACME_RENEWAL_DECISION_NO_CERTIFICATE:
        return "no-certificate";
    case APPLIANCE_ACME_RENEWAL_DECISION_NOT_NEEDED:
        return "not-needed";
    case APPLIANCE_ACME_RENEWAL_DECISION_DUE:
        return "due";
    case APPLIANCE_ACME_RENEWAL_DECISION_RETRY_AFTER:
        return "retry-after";
    case APPLIANCE_ACME_RENEWAL_DECISION_ATTEMPTS_EXHAUSTED:
        return "attempts-exhausted";
    default:
        return "unknown";
    }
}

const char *appliance_acme_renewal_status_string(
    enum appliance_acme_renewal_status status)
{
    switch (status) {
    case APPLIANCE_ACME_RENEWAL_OK:
        return "ok";
    case APPLIANCE_ACME_RENEWAL_NULL:
        return "null";
    case APPLIANCE_ACME_RENEWAL_BAD_STATE:
        return "bad-state";
    case APPLIANCE_ACME_RENEWAL_BAD_CLOCK:
        return "bad-clock";
    case APPLIANCE_ACME_RENEWAL_NOT_DUE:
        return "not-due";
    case APPLIANCE_ACME_RENEWAL_NOT_ATTEMPTED:
        return "not-attempted";
    case APPLIANCE_ACME_RENEWAL_ATTEMPTS_EXHAUSTED:
        return "attempts-exhausted";
    case APPLIANCE_ACME_RENEWAL_BAD_VALUE:
        return "bad-value";
    case APPLIANCE_ACME_RENEWAL_NO_CERTIFICATE:
        return "no-certificate";
    default:
        return "unknown";
    }
}

const char *appliance_acme_renewal_outcome_string(
    enum appliance_acme_renewal_outcome outcome)
{
    switch (outcome) {
    case APPLIANCE_ACME_RENEWAL_OUTCOME_NONE:
        return "none";
    case APPLIANCE_ACME_RENEWAL_OUTCOME_SUCCEEDED:
        return "succeeded";
    case APPLIANCE_ACME_RENEWAL_OUTCOME_FAILED:
        return "failed";
    default:
        return "unknown";
    }
}
