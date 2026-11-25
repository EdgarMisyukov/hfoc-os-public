#include "appliance_production_acme_service.h"

/*
 * ITEM-508 increment 26: the ACME service ordinary boot owns.  The module is
 * transport-free: it binds the persisted identity as the account key, owns the
 * renewal policy and publishes one bounded record.  Nothing here opens a
 * socket, speaks HTTP or holds certificate material.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_SERVICE_HOST_TEST)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char appliance_production_acme_service_prefix[] =
    "[ACME-SERVICE] v=1 state=";
static const char appliance_production_acme_service_too_large[] =
    "[ACME-SERVICE] v=1 state=RECORD_TOO_LARGE";

/*
 * A bounded text builder: the record never grows past its buffer and never
 * truncates silently.  A build that would overflow is replaced whole by the
 * named fallback, so a reader never sees half a state.
 */
struct appliance_production_acme_service_text {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_production_acme_service_text_reset(
    struct appliance_production_acme_service_text *text,
    char *buffer,
    size_t capacity)
{
    text->buffer = buffer;
    text->capacity = capacity;
    text->length = 0U;
    text->overflow = false;
    if (capacity > 0U) {
        buffer[0] = '\0';
    }
}

static void appliance_production_acme_service_text_append(
    struct appliance_production_acme_service_text *text,
    const char *value)
{
    const size_t length = strlen(value);

    if (length == 0U || text->capacity == 0U) {
        return;
    }
    if (text->length + length + 1U > text->capacity) {
        text->overflow = true;
        return;
    }
    memcpy(&text->buffer[text->length], value, length);
    text->length += length;
    text->buffer[text->length] = '\0';
}

static void appliance_production_acme_service_text_append_u32(
    struct appliance_production_acme_service_text *text,
    uint32_t value)
{
    char digits[10];
    size_t count = 0U;

    if (value == 0U) {
        appliance_production_acme_service_text_append(text, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char one[2];

        one[0] = digits[count - 1U];
        one[1] = '\0';
        appliance_production_acme_service_text_append(text, one);
        --count;
    }
}

static void appliance_production_acme_service_publish(
    struct appliance_production_acme_service *service)
{
    struct appliance_production_acme_service_text text;

    appliance_production_acme_service_text_reset(
        &text, service->record, sizeof(service->record));
    appliance_production_acme_service_text_append(
        &text, appliance_production_acme_service_prefix);
    appliance_production_acme_service_text_append(
        &text,
        appliance_production_acme_service_state_string(service->state));
    switch (service->state) {
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND: {
        /*
         * The thumbprint comes from the service's own bound key, not from the
         * account module's single global record: a refusal of some other bind
         * must never rewrite the account name this service published.
         */
        const char *thumbprint =
            appliance_acme_key_thumbprint(&service->key);

        appliance_production_acme_service_text_append(
            &text, " account=bound thumbprint=");
        appliance_production_acme_service_text_append(
            &text, thumbprint != NULL ? thumbprint : "unavailable");
        appliance_production_acme_service_text_append(&text, " renewal=");
        appliance_production_acme_service_text_append(
            &text,
            appliance_acme_renewal_decision_string(service->last_decision));
        appliance_production_acme_service_text_append(&text, " expires=");
        appliance_production_acme_service_text_append_u32(
            &text, service->renewal.expires_at_epoch_seconds);
        appliance_production_acme_service_text_append(&text, " attempts=");
        appliance_production_acme_service_text_append_u32(
            &text, service->renewal.attempts);
        break;
    }
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY:
        appliance_production_acme_service_text_append(
            &text, " reason=identity_unavailable");
        break;
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED:
        appliance_production_acme_service_text_append(&text, " reason=");
        appliance_production_acme_service_text_append(
            &text,
            appliance_acme_account_status_string(service->account_status));
        break;
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_UNINITIALIZED:
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_NONE:
    /*
     * ITEM-508 increment 34: a disabled service keeps the record to the state
     * word; the durable control record and the panel already name the setting
     * and the pending request, and repeating them here would be a second
     * source of truth for one value.
     */
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_DISABLED:
    default:
        break;
    }
    if (text.overflow) {
        appliance_production_acme_service_text_reset(
            &text, service->record, sizeof(service->record));
        appliance_production_acme_service_text_append(
            &text, appliance_production_acme_service_too_large);
    }
}

/*
 * Bind when the certificate owner has a prepared identity.  An identity that
 * is not ready leaves the service waiting and a later poll retries; a bind
 * refusal leaves the account unbound and names the account status, so the
 * record never carries a thumbprint the last bind did not accept.
 */
static void appliance_production_acme_service_observe(
    struct appliance_production_acme_service *service,
    const struct appliance_production_certificate *identity)
{
    enum appliance_acme_account_status status;

    if (identity == NULL || !identity->initialized || !identity->available ||
        identity->key_length == 0U) {
        service->state =
            APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY;
        service->account_bound = false;
        service->account_status =
            APPLIANCE_ACME_ACCOUNT_IDENTITY_UNAVAILABLE;
        return;
    }
    ++service->bind_attempts;
    status = appliance_acme_account_bind(identity, &service->key);
    service->account_status = status;
    if (status == APPLIANCE_ACME_ACCOUNT_OK) {
        service->state = APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND;
        service->account_bound = true;
        return;
    }
    service->state = APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED;
    service->account_bound = false;
}

enum appliance_production_acme_service_status
appliance_production_acme_service_init(
    struct appliance_production_acme_service *service,
    const struct appliance_production_certificate *identity)
{
    if (service == NULL) {
        return APPLIANCE_PRODUCTION_ACME_SERVICE_NULL;
    }
    if (service->initialized) {
        return APPLIANCE_PRODUCTION_ACME_SERVICE_ALREADY_INITIALIZED;
    }
    memset(service, 0, sizeof(*service));
    service->version = APPLIANCE_PRODUCTION_ACME_SERVICE_VERSION;
    service->state = APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_UNINITIALIZED;
    service->account_status = APPLIANCE_ACME_ACCOUNT_NULL;
    service->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    appliance_acme_renewal_init(&service->renewal);
    service->initialized = true;
    appliance_production_acme_service_observe(service, identity);
    appliance_production_acme_service_publish(service);
    return APPLIANCE_PRODUCTION_ACME_SERVICE_OK;
}

enum appliance_production_acme_service_status
appliance_production_acme_service_poll(
    struct appliance_production_acme_service *service,
    const struct appliance_production_certificate *identity,
    uint32_t now_epoch_seconds)
{
    if (service == NULL) {
        return APPLIANCE_PRODUCTION_ACME_SERVICE_NULL;
    }
    if (!service->initialized) {
        return APPLIANCE_PRODUCTION_ACME_SERVICE_BAD_STATE;
    }
    ++service->polls;
    /*
     * ITEM-508 increment 34: the durable control decides whether the service
     * does anything at all.  A disabled service binds nothing and evaluates no
     * renewal; the control is read again on the next poll, so enabling it
     * takes effect without a reboot.
     */
    if (service->control_published && !service->enabled) {
        service->state = APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_DISABLED;
        service->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
        appliance_production_acme_service_publish(service);
        return APPLIANCE_PRODUCTION_ACME_SERVICE_OK;
    }
    if (!service->account_bound) {
        appliance_production_acme_service_observe(service, identity);
    }
    if (service->account_bound) {
        /*
         * A service that was disabled and enabled again still holds the bound
         * key; the state word follows the binding, not the last poll.
         */
        service->state = APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND;
        service->last_decision = appliance_acme_renewal_evaluate(
            &service->renewal, now_epoch_seconds);
    } else {
        service->last_decision = APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    }
    appliance_production_acme_service_publish(service);
    return APPLIANCE_PRODUCTION_ACME_SERVICE_OK;
}

void appliance_production_acme_service_set_control(
    struct appliance_production_acme_service *service,
    bool enabled,
    bool order_requested,
    uint32_t order_requested_at)
{
    if (service == NULL || !service->initialized) {
        return;
    }
    service->control_published = true;
    service->enabled = enabled;
    service->order_requested = order_requested;
    service->order_requested_at = order_requested_at;
}

bool appliance_production_acme_service_enabled(
    const struct appliance_production_acme_service *service)
{
    return service != NULL && service->initialized &&
           (!service->control_published || service->enabled);
}

bool appliance_production_acme_service_order_requested(
    const struct appliance_production_acme_service *service)
{
    return service != NULL && service->initialized && service->enabled &&
           service->order_requested;
}

enum appliance_acme_renewal_status
appliance_production_acme_service_certificate_obtained(
    struct appliance_production_acme_service *service,
    uint32_t now_epoch_seconds,
    uint32_t expires_at_epoch_seconds)
{
    enum appliance_acme_renewal_status status;

    if (service == NULL) {
        return APPLIANCE_ACME_RENEWAL_NULL;
    }
    if (!service->initialized) {
        return APPLIANCE_ACME_RENEWAL_BAD_STATE;
    }
    status = appliance_acme_renewal_certificate_obtained(
        &service->renewal, now_epoch_seconds, expires_at_epoch_seconds);
    /*
     * The policy moved (or refused to move) the expiry; the decision is
     * re-evaluated so the record the caller reads belongs to the state the
     * call left behind, not to the state before it.
     */
    service->last_decision = appliance_acme_renewal_evaluate(
        &service->renewal, now_epoch_seconds);
    appliance_production_acme_service_publish(service);
    return status;
}

struct appliance_acme_renewal *appliance_production_acme_service_renewal(
    struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return NULL;
    }
    return &service->renewal;
}

const char *appliance_production_acme_service_record(
    const struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return "uninitialized";
    }
    return service->record;
}

enum appliance_production_acme_service_state
appliance_production_acme_service_state(
    const struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_NONE;
    }
    return service->state;
}

bool appliance_production_acme_service_account_bound(
    const struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return false;
    }
    return service->account_bound;
}

enum appliance_acme_account_status
appliance_production_acme_service_account_status(
    const struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return APPLIANCE_ACME_ACCOUNT_NULL;
    }
    return service->account_status;
}

enum appliance_acme_renewal_decision
appliance_production_acme_service_renewal_decision(
    const struct appliance_production_acme_service *service)
{
    if (service == NULL || !service->initialized) {
        return APPLIANCE_ACME_RENEWAL_DECISION_NONE;
    }
    return service->last_decision;
}

const char *appliance_production_acme_service_state_string(
    enum appliance_production_acme_service_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_AWAITING_IDENTITY:
        return "AWAITING_IDENTITY";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_BOUND:
        return "BOUND";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_REFUSED:
        return "REFUSED";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_STATE_DISABLED:
        return "DISABLED";
    default:
        break;
    }
    return "UNKNOWN";
}

const char *appliance_production_acme_service_status_string(
    enum appliance_production_acme_service_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_ACME_SERVICE_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_ACME_SERVICE_ALREADY_INITIALIZED:
        return "ALREADY_INITIALIZED";
    default:
        break;
    }
    return "UNKNOWN";
}

#endif
