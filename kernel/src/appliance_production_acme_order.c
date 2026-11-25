#include "appliance_production_acme_order.h"

#include "appliance_acme_account.h"
#include "appliance_production_entropy.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * The driver is part of the TLS-serving path: every module it drives (the
 * flow, the JWS, the trust) is compiled only in the production TLS profile or
 * in its own host proof, so the other kernels compile an empty object instead
 * of a translation unit with unresolved references.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_ORDER_HOST_TEST)

/*
 * ITEM-508 increment 35: the order path ordinary boot runs.  The module is
 * pure state: it drives the proven flow over the injected transport link,
 * arms the injected listener from the parsed challenge, verifies the
 * downloaded chain against the injected anchor and adopts it through the
 * production certificate owner.  It opens no socket, allocates nothing and
 * reads no clock of its own: every instant and every millisecond comes from
 * the caller, so the whole path is drivable from a host proof.
 *
 * The state machine mirrors the fixture order driver
 * (`appliance_acme_order_qemu.c`), which stays the wire proof of the stub
 * recipe; this module is the same sequence without the fixture epoch, the
 * fixture anchor and the halt-on-failure.  A refusal becomes a named status
 * and one record, and the caller decides what to do next.
 */

const char *appliance_production_acme_order_stage_string(
    enum appliance_production_acme_order_stage stage)
{
    switch (stage) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY:
        return "DIRECTORY";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT:
        return "ACCOUNT";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER:
        return "ORDER";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION:
        return "AUTHORIZATION";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE:
        return "CHALLENGE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL:
        return "CHALLENGE_POLL";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE:
        return "FINALIZE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL:
        return "FINALIZE_POLL";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE:
        return "CERTIFICATE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE:
        return "DONE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE:
    default:
        return "NONE";
    }
}

static const char appliance_production_acme_order_prefix[] =
    "[PRODUCTION-ACME-ORDER] state=";
static const char appliance_production_acme_order_too_large[] =
    "[PRODUCTION-ACME-ORDER] state=RECORD_TOO_LARGE";

/*
 * A bounded text builder: the record never grows past its buffer and never
 * truncates silently.  A build that would overflow is replaced whole by the
 * named fallback, so a reader never sees half a state.
 */
struct appliance_production_acme_order_text {
    char *buffer;
    size_t capacity;
    size_t length;
    bool overflow;
};

static void appliance_production_acme_order_text_reset(
    struct appliance_production_acme_order_text *text,
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

static void appliance_production_acme_order_text_append(
    struct appliance_production_acme_order_text *text,
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
    memcpy(text->buffer + text->length, value, length);
    text->length += length;
    text->buffer[text->length] = '\0';
}

static void appliance_production_acme_order_text_append_u32(
    struct appliance_production_acme_order_text *text,
    uint32_t value)
{
    char digits[11];
    size_t count = 0U;

    if (value == 0U) {
        appliance_production_acme_order_text_append(text, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        char one[2];

        --count;
        one[0] = digits[count];
        one[1] = '\0';
        appliance_production_acme_order_text_append(text, one);
    }
}

static size_t appliance_production_acme_order_text_length(
    const char *value)
{
    size_t length = 0U;

    while (value[length] != '\0') {
        ++length;
    }
    return length;
}

static void appliance_production_acme_order_publish(
    struct appliance_production_acme_order *order,
    const char *state_word,
    const char *step_word,
    const char *detail)
{
    struct appliance_production_acme_order_text text;

    appliance_production_acme_order_text_reset(
        &text, order->record, sizeof(order->record));
    appliance_production_acme_order_text_append(
        &text, appliance_production_acme_order_prefix);
    appliance_production_acme_order_text_append(&text, state_word);
    if (step_word != NULL) {
        appliance_production_acme_order_text_append(&text, " step=");
        appliance_production_acme_order_text_append(&text, step_word);
    }
    if (detail != NULL) {
        appliance_production_acme_order_text_append(&text, " ");
        appliance_production_acme_order_text_append(&text, detail);
    }
    if (text.overflow) {
        appliance_production_acme_order_text_reset(
            &text, order->record, sizeof(order->record));
        appliance_production_acme_order_text_append(
            &text, appliance_production_acme_order_too_large);
    }
}

static void appliance_production_acme_order_publish_names(
    struct appliance_production_acme_order *order,
    const char *state_word,
    const char *step_word)
{
    struct appliance_production_acme_order_text text;

    appliance_production_acme_order_text_reset(
        &text, order->record, sizeof(order->record));
    appliance_production_acme_order_text_append(
        &text, appliance_production_acme_order_prefix);
    appliance_production_acme_order_text_append(&text, state_word);
    if (step_word != NULL) {
        appliance_production_acme_order_text_append(&text, " step=");
        appliance_production_acme_order_text_append(&text, step_word);
    }
    appliance_production_acme_order_text_append(&text, " names=");
    appliance_production_acme_order_text_append_u32(
        &text, order->identifier_count);
    if (text.overflow) {
        appliance_production_acme_order_text_reset(
            &text, order->record, sizeof(order->record));
        appliance_production_acme_order_text_append(
            &text, appliance_production_acme_order_too_large);
    }
}

static void appliance_production_acme_order_publish_failure(
    struct appliance_production_acme_order *order,
    enum appliance_production_acme_order_status status)
{
    struct appliance_production_acme_order_text text;

    order->state = APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED;
    order->last_status = status;
    if (order->listener_armed && order->listener.disarm != NULL) {
        order->listener.disarm(order->listener.context);
        order->listener_armed = false;
    }
    appliance_production_acme_order_text_reset(
        &text, order->record, sizeof(order->record));
    appliance_production_acme_order_text_append(
        &text, appliance_production_acme_order_prefix);
    appliance_production_acme_order_text_append(&text, "FAILED reason=");
    appliance_production_acme_order_text_append(
        &text, appliance_production_acme_order_status_string(status));
    appliance_production_acme_order_text_append(&text, " step=");
    appliance_production_acme_order_text_append(
        &text, appliance_acme_flow_step_string(order->flow.last_step));
    appliance_production_acme_order_text_append(&text, " flow=");
    appliance_production_acme_order_text_append(
        &text, appliance_acme_flow_status_string(order->flow.last_status));
    appliance_production_acme_order_text_append(&text, " transport=");
    appliance_production_acme_order_text_append(
        &text,
        appliance_acme_transport_status_string(order->transport.last_status));
    if (text.overflow) {
        appliance_production_acme_order_text_reset(
            &text, order->record, sizeof(order->record));
        appliance_production_acme_order_text_append(
            &text, appliance_production_acme_order_too_large);
    }
}

static void appliance_production_acme_order_publish_done(
    struct appliance_production_acme_order *order)
{
    struct appliance_production_acme_order_text text;

    order->state = APPLIANCE_PRODUCTION_ACME_ORDER_STATE_DONE;
    order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    if (order->listener_armed && order->listener.disarm != NULL) {
        order->listener.disarm(order->listener.context);
        order->listener_armed = false;
    }
    appliance_production_acme_order_text_reset(
        &text, order->record, sizeof(order->record));
    appliance_production_acme_order_text_append(
        &text, appliance_production_acme_order_prefix);
    appliance_production_acme_order_text_append(&text, "DONE step=CERTIFICATE");
    appliance_production_acme_order_text_append(&text, " members=");
    appliance_production_acme_order_text_append_u32(
        &text, order->certificate_members);
    appliance_production_acme_order_text_append(&text, " der=");
    appliance_production_acme_order_text_append_u32(
        &text, (uint32_t)order->der_length);
    appliance_production_acme_order_text_append(&text, " expires=");
    appliance_production_acme_order_text_append_u32(
        &text, order->leaf_not_after_epoch_seconds);
    if (text.overflow) {
        appliance_production_acme_order_text_reset(
            &text, order->record, sizeof(order->record));
        appliance_production_acme_order_text_append(
            &text, appliance_production_acme_order_too_large);
    }
}

/*
 * The length of one DER certificate: the outer SEQUENCE header plus its
 * contents.  The order path splits the verified chain into the leaf and the
 * rest for the certificate owner's record, so the length must come from the
 * bytes, not from an assumption.  Only the short form and the one- and
 * two-octet long forms are accepted; a certificate larger than either bound
 * is refused rather than guessed.
 */
static bool appliance_production_acme_order_der_length(
    const unsigned char *der,
    size_t length,
    size_t *member_length_out)
{
    size_t header = 2U;
    size_t content;

    if (der == NULL || member_length_out == NULL || length < 2U ||
        der[0] != 0x30U) {
        return false;
    }
    if (der[1] < 0x80U) {
        content = der[1];
    } else if (der[1] == 0x81U) {
        if (length < 3U) {
            return false;
        }
        content = der[2];
        header = 3U;
    } else if (der[1] == 0x82U) {
        if (length < 4U) {
            return false;
        }
        content = ((size_t)der[2] << 8U) | (size_t)der[3];
        header = 4U;
    } else {
        return false;
    }
    if (header + content > length) {
        return false;
    }
    *member_length_out = header + content;
    return true;
}

static void appliance_production_acme_order_remember_nonce(
    struct appliance_production_acme_order *order,
    const struct appliance_acme_http_response *response)
{
    if (!response->has_nonce || response->nonce_length == 0U ||
        response->nonce_length > APPLIANCE_PRODUCTION_ACME_ORDER_NONCE_MAX) {
        order->nonce_length = 0U;
        return;
    }
    memcpy(order->nonce, response->nonce, response->nonce_length);
    order->nonce[response->nonce_length] = '\0';
    order->nonce_length = response->nonce_length;
}

/*
 * Start one exchange over the injected link.  The transport is re-initialized
 * per exchange because `close` detaches the link; the binding itself is the
 * caller's and is reused.
 */
static enum appliance_production_acme_order_status
appliance_production_acme_order_begin(
    struct appliance_production_acme_order *order,
    const char *method,
    const char *url,
    const char *body,
    size_t body_length,
    uint32_t now_ms)
{
    enum appliance_acme_transport_status status;

    if (appliance_acme_transport_init(&order->transport, order->link) !=
        APPLIANCE_ACME_TRANSPORT_OK) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED;
    }
    status = appliance_acme_transport_begin(
        &order->transport, method,
        (uint32_t)appliance_production_acme_order_text_length(method), url,
        (uint32_t)appliance_production_acme_order_text_length(url),
        "application/jose+json",
        (uint32_t)appliance_production_acme_order_text_length(
            "application/jose+json"),
        (const uint8_t *)body, body_length, now_ms);
    if (status != APPLIANCE_ACME_TRANSPORT_OK &&
        status != APPLIANCE_ACME_TRANSPORT_PENDING) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED;
    }
    order->exchange_active = true;
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

/*
 * Arm the injected listener from the flow the driver is really driving: the
 * token and the key authorization come from the parsed `http-01` challenge,
 * never from a literal here.  A listener that is not up yet, or that refuses
 * the challenge, fails the order by name - the CA is about to validate, and
 * an answer that cannot be served must not be attempted.
 */
static enum appliance_production_acme_order_status
appliance_production_acme_order_arm_listener(
    struct appliance_production_acme_order *order)
{
    enum appliance_acme_challenge_status status;

    if (order->listener_armed) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    if (order->listener.arm_flow == NULL ||
        (order->listener.listening != NULL &&
         !order->listener.listening(order->listener.context))) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER;
    }
    status = order->listener.arm_flow(
        order->listener.context, &order->flow, &order->key,
        order->now_epoch_seconds);
    if (status != APPLIANCE_ACME_CHALLENGE_OK) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER;
    }
    order->listener_armed = true;
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

static enum appliance_production_acme_order_status
appliance_production_acme_order_flow_check(
    enum appliance_acme_flow_status status)
{
    return status == APPLIANCE_ACME_FLOW_OK
               ? APPLIANCE_PRODUCTION_ACME_ORDER_OK
               : APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
}

/*
 * Build the request for the current stage and open its exchange.  Every
 * request is built by the flow; this switch only decides which step comes
 * next and where it goes.
 */
static enum appliance_production_acme_order_status
appliance_production_acme_order_start_exchange(
    struct appliance_production_acme_order *order,
    enum appliance_production_acme_order_stage stage,
    uint32_t now_ms)
{
    enum appliance_acme_flow_status flow_status;
    enum appliance_production_acme_order_status status;

    if (order->nonce_length == 0U &&
        stage != APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
    }
    switch (stage) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY:
        return appliance_production_acme_order_begin(
            order, "GET", order->directory_url, NULL, 0U, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT:
        flow_status = appliance_acme_flow_begin_account(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            appliance_production_entropy_random, NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.directory.new_account.text,
            order->flow.request, order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER:
        flow_status = appliance_acme_flow_begin_order(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            appliance_production_entropy_random, NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.directory.new_order.text,
            order->flow.request, order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION:
        flow_status = appliance_acme_flow_begin_authorization(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            appliance_production_entropy_random, NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.order.authorizations[0].text,
            order->flow.request, order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL:
        flow_status =
            stage == APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE
                ? appliance_acme_flow_begin_challenge(
                      &order->flow, &order->key, order->nonce,
                      order->nonce_length, appliance_production_entropy_random,
                      NULL)
                : appliance_acme_flow_begin_challenge_poll(
                      &order->flow, &order->key, order->nonce,
                      order->nonce_length, appliance_production_entropy_random,
                      NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.authorization.http01.url.text,
            order->flow.request, order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE: {
        enum appliance_acme_jws_status csr_status =
            appliance_acme_key_write_csr(
                &order->key, order->flow.identifiers[0].name,
                appliance_production_entropy_random, NULL, order->csr,
                sizeof(order->csr), &order->csr_length);

        if (csr_status != APPLIANCE_ACME_JWS_OK) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
        }
        flow_status = appliance_acme_flow_begin_finalize(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            order->csr, order->csr_length, appliance_production_entropy_random,
            NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.order.finalize.text,
            order->flow.request, order->flow.request_length, now_ms);
    }
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL:
        flow_status = appliance_acme_flow_begin_order_poll(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            appliance_production_entropy_random, NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.order_url.text, order->flow.request,
            order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE:
        flow_status = appliance_acme_flow_begin_certificate(
            &order->flow, &order->key, order->nonce, order->nonce_length,
            appliance_production_entropy_random, NULL);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_begin(
            order, "POST", order->flow.certificate_url.text,
            order->flow.request, order->flow.request_length, now_ms);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE:
    default:
        return APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE;
    }
}

/*
 * The certificate step, with the two consequences the item requires: the
 * verified chain is adopted by the production certificate owner, and the
 * renewal policy moves to the leaf's own `notAfter`.  A refusal from either
 * fails the order; the chain is never adopted without the policy accepting
 * the same expiry.
 */
static enum appliance_production_acme_order_status
appliance_production_acme_order_finish_certificate(
    struct appliance_production_acme_order *order)
{
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    size_t leaf_length = 0U;
    enum appliance_acme_flow_status flow_status;
    enum appliance_production_certificate_status adopt_status;

    flow_status = appliance_acme_flow_certificate_result(
        &order->flow, &order->trust,
        (const char *)order->certificate_body,
        order->certificate_body_length, order->now_epoch_seconds, order->der,
        sizeof(order->der), &order->der_length, &order->certificate_members);
    if (flow_status != APPLIANCE_ACME_FLOW_OK) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED;
    }
    if (!appliance_production_acme_order_der_length(
            order->der, order->der_length, &leaf_length) ||
        leaf_length > APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED;
    }
    order->leaf_not_after_epoch_seconds =
        appliance_acme_trust_leaf_not_after(&order->trust);
    if (order->leaf_not_after_epoch_seconds == 0U ||
        order->leaf_not_after_epoch_seconds <= order->now_epoch_seconds) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED;
    }
    memset(fingerprint, 0, sizeof(fingerprint));
    adopt_status = appliance_production_certificate_adopt_acme(
        order->identity, order->der, (uint32_t)leaf_length,
        order->der + leaf_length, (uint32_t)(order->der_length - leaf_length),
        order->identity->key_der, order->identity->key_length, fingerprint);
    if (adopt_status != APPLIANCE_PRODUCTION_CERTIFICATE_OK) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_ADOPT_FAILED;
    }
    if (appliance_acme_renewal_certificate_obtained(
            order->renewal, order->now_epoch_seconds,
            order->leaf_not_after_epoch_seconds) !=
        APPLIANCE_ACME_RENEWAL_OK) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_RENEWAL_REFUSED;
    }
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

/*
 * Consume one complete reply.  The stage the request belonged to decides what
 * the reply means; a reply the flow refuses is a named failure, never a
 * shorter order.
 */
static enum appliance_production_acme_order_status
appliance_production_acme_order_handle_response(
    struct appliance_production_acme_order *order,
    enum appliance_production_acme_order_stage stage,
    const struct appliance_acme_http_response *response)
{
    enum appliance_acme_flow_status flow_status;
    enum appliance_production_acme_order_status status;

    if (!appliance_acme_http_status_is_success(response->status_code)) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
    }
    appliance_production_acme_order_remember_nonce(order, response);
    switch (stage) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY: {
        const char *names[
            APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX] = { NULL };
        size_t lengths[APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX];
        uint32_t index;

        flow_status = appliance_acme_flow_begin(
            &order->flow, (const char *)response->body,
            response->body_length, order->now_epoch_seconds);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        for (index = 0U; index < order->identifier_count; ++index) {
            names[index] = order->identifiers[index];
            lengths[index] = order->identifier_lengths[index];
        }
        flow_status = appliance_acme_flow_declare_identifiers(
            &order->flow, names, lengths, order->identifier_count);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT:
        if (!response->has_location || response->location_length == 0U) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
        }
        flow_status = appliance_acme_flow_account_result(
            &order->flow, (const char *)response->body,
            response->body_length, response->location,
            response->location_length);
        return appliance_production_acme_order_flow_check(flow_status);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER:
        if (!response->has_location || response->location_length == 0U) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
        }
        flow_status = appliance_acme_flow_order_result(
            &order->flow, (const char *)response->body,
            response->body_length, response->location,
            response->location_length);
        return appliance_production_acme_order_flow_check(flow_status);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION:
        flow_status = appliance_acme_flow_authorization_result(
            &order->flow, (const char *)response->body,
            response->body_length);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        return appliance_production_acme_order_arm_listener(order);
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL:
        flow_status = appliance_acme_flow_challenge_result(
            &order->flow, (const char *)response->body,
            response->body_length);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        if (order->flow.state == APPLIANCE_ACME_FLOW_STATE_READY) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        if (order->flow.state ==
                APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING &&
            stage == APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL:
        flow_status = appliance_acme_flow_finalize_result(
            &order->flow, (const char *)response->body,
            response->body_length);
        status = appliance_production_acme_order_flow_check(flow_status);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            return status;
        }
        if (order->flow.state ==
            APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        if (order->flow.state == APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING &&
            stage == APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE) {
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        return APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE:
        /*
         * The certificate body is consumed by the finish step, which owns the
         * trust verification and the two consequences; the reply is kept in
         * the transport's own buffer until then.
         */
        (void)response;
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE:
    default:
        return APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE;
    }
}

static enum appliance_production_acme_order_stage
appliance_production_acme_order_next_stage(
    const struct appliance_production_acme_order *order,
    enum appliance_production_acme_order_stage stage)
{
    switch (stage) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ACCOUNT:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_ORDER:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_AUTHORIZATION:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE:
        return order->flow.state == APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING
                   ? APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL
                   : APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CHALLENGE_POLL:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE:
        return order->flow.state == APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING
                   ? APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL
                   : APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_FINALIZE_POLL:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE;
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE:
    case APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE:
    default:
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DONE;
    }
}

enum appliance_production_acme_order_status
appliance_production_acme_order_tick(
    struct appliance_production_acme_order *order,
    uint32_t now_ms)
{
    enum appliance_acme_transport_status transport_status;
    enum appliance_production_acme_order_status status;
    struct appliance_acme_http_response response;

    if (order == NULL) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NULL;
    }
    if (!order->initialized ||
        order->state != APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING) {
        return order->initialized ? APPLIANCE_PRODUCTION_ACME_ORDER_OK
                                  : APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE;
    }
    if (now_ms == 0U) {
        /* No validated millisecond clock yet; the next turn retries. */
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    if (order->deadline_ms == 0U) {
        order->deadline_ms = now_ms + APPLIANCE_PRODUCTION_ACME_ORDER_DEADLINE_MS;
    } else if (now_ms > order->deadline_ms) {
        appliance_production_acme_order_publish_failure(
            order, APPLIANCE_PRODUCTION_ACME_ORDER_TIMEOUT);
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    if (!order->exchange_active) {
        status = appliance_production_acme_order_start_exchange(
            order, order->stage, now_ms);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            appliance_production_acme_order_publish_failure(order, status);
        }
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    transport_status = appliance_acme_transport_advance(&order->transport, now_ms);
    if (transport_status == APPLIANCE_ACME_TRANSPORT_PENDING) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    if (transport_status != APPLIANCE_ACME_TRANSPORT_OK) {
        appliance_production_acme_order_publish_failure(
            order, APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED);
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    if (appliance_acme_transport_take_response(&order->transport, &response) !=
        APPLIANCE_ACME_TRANSPORT_OK) {
        appliance_production_acme_order_publish_failure(
            order, APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED);
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    order->exchange_active = false;
    if (order->stage == APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_CERTIFICATE) {
        /*
         * The certificate body lives in the transport's reply until the
         * finish step reads it; keep the bytes the flow needs.
         */
        if (response.body_length > sizeof(order->certificate_body)) {
            appliance_production_acme_order_publish_failure(
                order, APPLIANCE_PRODUCTION_ACME_ORDER_BUFFER_TOO_SMALL);
            appliance_acme_transport_close(&order->transport);
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        memcpy(order->certificate_body, response.body, response.body_length);
        order->certificate_body_length = response.body_length;
        status = appliance_production_acme_order_finish_certificate(order);
        appliance_acme_transport_close(&order->transport);
        if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
            appliance_production_acme_order_publish_failure(order, status);
            return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
        }
        appliance_production_acme_order_publish_done(order);
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    status = appliance_production_acme_order_handle_response(
        order, order->stage, &response);
    appliance_acme_transport_close(&order->transport);
    if (status != APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
        appliance_production_acme_order_publish_failure(order, status);
        return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    }
    order->stage = appliance_production_acme_order_next_stage(
        order, order->stage);
    appliance_production_acme_order_publish_names(
        order, "RUNNING",
        appliance_production_acme_order_stage_string(order->stage));
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

void appliance_production_acme_order_init(
    struct appliance_production_acme_order *order)
{
    if (order == NULL) {
        return;
    }
    memset(order, 0, sizeof(*order));
    order->version = APPLIANCE_PRODUCTION_ACME_ORDER_VERSION;
    order->initialized = true;
    order->state = APPLIANCE_PRODUCTION_ACME_ORDER_STATE_IDLE;
    order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    order->stage = APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE;
    appliance_production_acme_order_publish(order, "IDLE", NULL, NULL);
}

enum appliance_production_acme_order_status
appliance_production_acme_order_configure(
    struct appliance_production_acme_order *order,
    const struct appliance_acme_transport_link *link,
    struct appliance_production_certificate *identity,
    struct appliance_acme_renewal *renewal,
    const struct appliance_production_acme_order_listener *listener,
    const unsigned char *anchor_der,
    size_t anchor_length,
    const char *directory_url)
{
    if (order == NULL) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NULL;
    }
    if (!order->initialized) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE;
    }
    if (link == NULL || identity == NULL || renewal == NULL ||
        anchor_der == NULL || anchor_length == 0U ||
        directory_url == NULL || directory_url[0] == '\0') {
        order->configured = false;
        order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_NULL;
        return order->last_status;
    }
    order->link = link;
    order->identity = identity;
    order->renewal = renewal;
    if (listener != NULL) {
        order->listener = *listener;
    } else {
        memset(&order->listener, 0, sizeof(order->listener));
    }
    order->anchor_der = anchor_der;
    order->anchor_length = anchor_length;
    order->directory_url = directory_url;
    order->configured = true;
    order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

enum appliance_production_acme_order_status
appliance_production_acme_order_start(
    struct appliance_production_acme_order *order,
    const char *const *names,
    const size_t *name_lengths,
    uint32_t count,
    uint32_t now_epoch_seconds)
{
    uint32_t index;

    if (order == NULL || names == NULL || name_lengths == NULL) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NULL;
    }
    if (!order->initialized || !order->configured) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE;
    }
    if (order->state == APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_BUSY;
    }
    if (count == 0U) {
        order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS;
        appliance_production_acme_order_publish(
            order, "REFUSED", NULL, "reason=NO_IDENTIFIERS");
        return order->last_status;
    }
    if (count > APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX) {
        order->last_status =
            APPLIANCE_PRODUCTION_ACME_ORDER_TOO_MANY_IDENTIFIERS;
        appliance_production_acme_order_publish(
            order, "REFUSED", NULL, "reason=TOO_MANY_IDENTIFIERS");
        return order->last_status;
    }
    if (now_epoch_seconds == 0U) {
        order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_BAD_CLOCK;
        appliance_production_acme_order_publish(
            order, "REFUSED", NULL, "reason=BAD_CLOCK");
        return order->last_status;
    }
    for (index = 0U; index < count; ++index) {
        if (names[index] == NULL || name_lengths[index] == 0U ||
            name_lengths[index] > APPLIANCE_ACME_NAME_MAX) {
            order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS;
            appliance_production_acme_order_publish(
                order, "REFUSED", NULL, "reason=NO_IDENTIFIERS");
            return order->last_status;
        }
        memcpy(order->identifiers[index], names[index], name_lengths[index]);
        order->identifiers[index][name_lengths[index]] = '\0';
        order->identifier_lengths[index] = name_lengths[index];
    }
    order->identifier_count = count;
    if (appliance_acme_account_bind(order->identity, &order->key) !=
        APPLIANCE_ACME_ACCOUNT_OK) {
        order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_KEY_REFUSED;
        appliance_production_acme_order_publish(
            order, "REFUSED", NULL, "reason=KEY_REFUSED");
        return order->last_status;
    }
    if (appliance_acme_trust_init(&order->trust, order->anchor_der,
                                  order->anchor_length) !=
        APPLIANCE_ACME_TRUST_OK) {
        order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_ANCHOR_REFUSED;
        appliance_production_acme_order_publish(
            order, "REFUSED", NULL, "reason=ANCHOR_REFUSED");
        return order->last_status;
    }
    order->now_epoch_seconds = now_epoch_seconds;
    order->deadline_ms = 0U;
    order->exchange_active = false;
    order->listener_armed = false;
    order->der_length = 0U;
    order->certificate_members = 0U;
    order->leaf_not_after_epoch_seconds = 0U;
    order->certificate_body_length = 0U;
    order->nonce_length = 0U;
    order->stage = APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_DIRECTORY;
    order->state = APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING;
    order->last_status = APPLIANCE_PRODUCTION_ACME_ORDER_OK;
    appliance_production_acme_order_publish_names(order, "RUNNING", "DIRECTORY");
    return APPLIANCE_PRODUCTION_ACME_ORDER_OK;
}

void appliance_production_acme_order_abort(
    struct appliance_production_acme_order *order)
{
    if (order == NULL || !order->initialized) {
        return;
    }
    if (order->state != APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING) {
        return;
    }
    if (order->exchange_active) {
        appliance_acme_transport_close(&order->transport);
        order->exchange_active = false;
    }
    appliance_production_acme_order_publish_failure(
        order, APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE);
}

enum appliance_production_acme_order_state
appliance_production_acme_order_state(
    const struct appliance_production_acme_order *order)
{
    if (order == NULL || !order->initialized) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_STATE_NONE;
    }
    return order->state;
}

enum appliance_production_acme_order_stage
appliance_production_acme_order_stage(
    const struct appliance_production_acme_order *order)
{
    if (order == NULL || !order->initialized) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_STAGE_NONE;
    }
    return order->stage;
}

enum appliance_production_acme_order_status
appliance_production_acme_order_last_status(
    const struct appliance_production_acme_order *order)
{
    if (order == NULL || !order->initialized) {
        return APPLIANCE_PRODUCTION_ACME_ORDER_NULL;
    }
    return order->last_status;
}

const char *appliance_production_acme_order_record(
    const struct appliance_production_acme_order *order)
{
    if (order == NULL || !order->initialized) {
        return "[PRODUCTION-ACME-ORDER] state=NONE";
    }
    return order->record;
}

uint32_t appliance_production_acme_order_certificate_members(
    const struct appliance_production_acme_order *order)
{
    return order != NULL && order->initialized ? order->certificate_members
                                               : 0U;
}

uint32_t appliance_production_acme_order_leaf_not_after(
    const struct appliance_production_acme_order *order)
{
    return order != NULL && order->initialized
               ? order->leaf_not_after_epoch_seconds
               : 0U;
}

const char *appliance_production_acme_order_state_string(
    enum appliance_production_acme_order_state state)
{
    switch (state) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_STATE_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STATE_IDLE:
        return "IDLE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING:
        return "RUNNING";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STATE_DONE:
        return "DONE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED:
        return "FAILED";
    }
    return "UNKNOWN";
}

const char *appliance_production_acme_order_status_string(
    enum appliance_production_acme_order_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_ACME_ORDER_OK:
        return "OK";
    case APPLIANCE_PRODUCTION_ACME_ORDER_NULL:
        return "NULL";
    case APPLIANCE_PRODUCTION_ACME_ORDER_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_PRODUCTION_ACME_ORDER_BAD_CLOCK:
        return "BAD_CLOCK";
    case APPLIANCE_PRODUCTION_ACME_ORDER_NO_IDENTIFIERS:
        return "NO_IDENTIFIERS";
    case APPLIANCE_PRODUCTION_ACME_ORDER_TOO_MANY_IDENTIFIERS:
        return "TOO_MANY_IDENTIFIERS";
    case APPLIANCE_PRODUCTION_ACME_ORDER_BUSY:
        return "BUSY";
    case APPLIANCE_PRODUCTION_ACME_ORDER_KEY_REFUSED:
        return "KEY_REFUSED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_ANCHOR_REFUSED:
        return "ANCHOR_REFUSED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_TRANSPORT_FAILED:
        return "TRANSPORT_FAILED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_FLOW_FAILED:
        return "FLOW_FAILED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_CHAIN_REFUSED:
        return "CHAIN_REFUSED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_NO_LISTENER:
        return "NO_LISTENER";
    case APPLIANCE_PRODUCTION_ACME_ORDER_ADOPT_FAILED:
        return "ADOPT_FAILED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_RENEWAL_REFUSED:
        return "RENEWAL_REFUSED";
    case APPLIANCE_PRODUCTION_ACME_ORDER_TIMEOUT:
        return "TIMEOUT";
    case APPLIANCE_PRODUCTION_ACME_ORDER_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    }
    return "UNKNOWN";
}

#else

/*
 * Every kernel build compiles this wildcard source, and only the TLS-serving
 * build defines the macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_production_acme_order_translation_unit_guard;

#endif
