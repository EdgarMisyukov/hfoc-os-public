#include "appliance_acme_flow.h"

/*
 * ITEM-508 increment 5: the order's first three steps and the challenge the
 * HTTP-01 listener will answer.  The module is transport-free: it consumes
 * replies the caller fetched and produces request bodies the caller sends.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_FLOW_HOST_TEST)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "appliance_acme_trust.h"
#include "pem_decode.h"

#define APPLIANCE_ACME_FLOW_VERSION_CURRENT 1U


static bool appliance_acme_flow_url_valid(const char *text, size_t length)
{
    static const char scheme[] = "https://";

    if (text == NULL || length <= sizeof(scheme) - 1U ||
        length > APPLIANCE_ACME_URL_MAX ||
        memcmp(text, scheme, sizeof(scheme) - 1U) != 0) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value <= 0x20U || value >= 0x7FU) {
            return false;
        }
    }
    return true;
}

static bool appliance_acme_flow_nonce_valid(const char *text, size_t length)
{
    if (text == NULL || length == 0U ||
        length > APPLIANCE_ACME_FLOW_NONCE_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (!((value >= (unsigned char)'A' && value <= (unsigned char)'Z') ||
              (value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              value == (unsigned char)'-' || value == (unsigned char)'_')) {
            return false;
        }
    }
    return true;
}

static void appliance_acme_flow_set_url(
    struct appliance_acme_url *url,
    const char *text,
    size_t length)
{
    memset(url, 0, sizeof(*url));
    memcpy(url->text, text, length);
    url->text[length] = '\0';
    url->length = (uint32_t)length;
}

/*
 * One bounded record per step: the state, the step, the status and what the
 * caller can act on.  A nonce, a token and a key byte never appear here.
 */
static void appliance_acme_flow_publish(
    struct appliance_acme_flow *flow,
    enum appliance_acme_flow_step step)
{
    static const char prefix[] = "[ACME-FLOW] state=";
    static const char middle[] = " step=";
    static const char status_text[] = " status=";
    size_t position = 0U;

    memset(flow->record, 0, sizeof(flow->record));
    if (strlen(prefix) + strlen(middle) + strlen(status_text) + 32U >=
        sizeof(flow->record)) {
        return;
    }
    memcpy(&flow->record[position], prefix, strlen(prefix));
    position += strlen(prefix);
    {
        const char *state =
            appliance_acme_flow_state_string(flow->state);
        const size_t length = strlen(state);

        memcpy(&flow->record[position], state, length);
        position += length;
    }
    memcpy(&flow->record[position], middle, strlen(middle));
    position += strlen(middle);
    {
        const char *name = appliance_acme_flow_step_string(step);
        const size_t length = strlen(name);

        memcpy(&flow->record[position], name, length);
        position += length;
    }
    memcpy(&flow->record[position], status_text, strlen(status_text));
    position += strlen(status_text);
    {
        const char *status =
            appliance_acme_flow_status_string(flow->last_status);
        const size_t length = strlen(status);

        memcpy(&flow->record[position], status, length);
        position += length;
    }
    flow->record[position] = '\0';
}

/*
 * A step called out of order is a caller error, not a broken flow: the record
 * is published with the state the flow is still in and the flow stays usable,
 * so a caller that polled too early can simply poll again.  Only a reply the
 * CA sent that cannot be trusted fails the flow (`fail`).
 */
static enum appliance_acme_flow_status appliance_acme_flow_refuse(
    struct appliance_acme_flow *flow,
    enum appliance_acme_flow_step step,
    enum appliance_acme_flow_status status)
{
    if (flow != NULL) {
        flow->last_status = status;
        flow->last_step = step;
        appliance_acme_flow_publish(flow, step);
    }
    return status;
}

static enum appliance_acme_flow_status appliance_acme_flow_fail(
    struct appliance_acme_flow *flow,
    enum appliance_acme_flow_step step,
    enum appliance_acme_flow_status status)
{
    if (flow != NULL) {
        flow->state = APPLIANCE_ACME_FLOW_STATE_FAILED;
        flow->last_status = status;
        flow->last_step = step;
        appliance_acme_flow_publish(flow, step);
    }
    return status;
}

/*
 * Build one JWS request: the protected header carries the algorithm, the
 * nonce and the exact URL, plus either the public JWK (account creation) or
 * the account's `kid`.
 */
static enum appliance_acme_flow_status appliance_acme_flow_build(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    const char *url,
    size_t url_length,
    bool use_jwk,
    const char *payload,
    size_t payload_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    static char protected_header[APPLIANCE_ACME_FLOW_PROTECTED_MAX];
    static const char prefix[] = "{\"alg\":\"ES256\",\"nonce\":\"";
    static const char url_prefix[] = "\",\"url\":\"";
    static const char jwk_prefix[] = "\",\"jwk\":";
    static const char kid_prefix[] = "\",\"kid\":\"";
    size_t position = 0U;
    enum appliance_acme_jws_status status;

    if (key == NULL || payload == NULL || f_rng == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (!appliance_acme_flow_nonce_valid(nonce, nonce_length) ||
        !appliance_acme_flow_url_valid(url, url_length)) {
        return APPLIANCE_ACME_FLOW_BAD_REPLY;
    }
    memset(protected_header, 0, sizeof(protected_header));
    if (strlen(prefix) + nonce_length + strlen(url_prefix) + url_length +
            strlen(jwk_prefix) + strlen(appliance_acme_key_jwk(key)) + 4U >=
        sizeof(protected_header)) {
        return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
    }
    memcpy(&protected_header[position], prefix, strlen(prefix));
    position += strlen(prefix);
    memcpy(&protected_header[position], nonce, nonce_length);
    position += nonce_length;
    memcpy(&protected_header[position], url_prefix, strlen(url_prefix));
    position += strlen(url_prefix);
    memcpy(&protected_header[position], url, url_length);
    position += url_length;
    if (use_jwk) {
        memcpy(&protected_header[position], jwk_prefix, strlen(jwk_prefix));
        position += strlen(jwk_prefix);
        {
            const char *jwk = appliance_acme_key_jwk(key);
            const size_t length = strlen(jwk);

            memcpy(&protected_header[position], jwk, length);
            position += length;
        }
    } else {
        if (flow->account_url.length == 0U) {
            return APPLIANCE_ACME_FLOW_BAD_STATE;
        }
        memcpy(&protected_header[position], kid_prefix, strlen(kid_prefix));
        position += strlen(kid_prefix);
        memcpy(&protected_header[position], flow->account_url.text,
               flow->account_url.length);
        position += flow->account_url.length;
    }
    protected_header[position++] = (char)'}';
    protected_header[position] = '\0';

    {
        size_t request_length = 0U;

        status = appliance_acme_jws_sign(
            key, protected_header, position, (const unsigned char *)payload,
            payload_length, f_rng, rng_context, flow->request,
            sizeof(flow->request), &request_length);
        if (status != APPLIANCE_ACME_JWS_OK ||
            request_length > UINT32_MAX) {
            flow->request_length = 0U;
            return APPLIANCE_ACME_FLOW_BAD_REPLY;
        }
        flow->request_length = (uint32_t)request_length;
    }
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_begin(
    struct appliance_acme_flow *flow,
    const char *directory_json,
    size_t directory_length,
    uint32_t now_epoch_seconds)
{
    enum appliance_acme_message_status status;

    if (flow == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    memset(flow, 0, sizeof(*flow));
    flow->version = APPLIANCE_ACME_FLOW_VERSION_CURRENT;
    flow->initialized = true;
    flow->state = APPLIANCE_ACME_FLOW_STATE_AWAITING_CLOCK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_DIRECTORY;
    /*
     * The item's point 6, enforced first: an unvalidated clock starts no
     * order, and the refusal is published before the directory is read.
     */
    if (now_epoch_seconds == 0U) {
        flow->last_status = APPLIANCE_ACME_FLOW_BAD_CLOCK;
        appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_DIRECTORY);
        return APPLIANCE_ACME_FLOW_BAD_CLOCK;
    }
    if (directory_json == NULL || directory_length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_DIRECTORY,
            APPLIANCE_ACME_FLOW_NULL);
    }
    status = appliance_acme_parse_directory(
        directory_json, directory_length, &flow->directory);
    if (status != APPLIANCE_ACME_MESSAGE_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_DIRECTORY,
            APPLIANCE_ACME_FLOW_BAD_REPLY);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_READY;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_DIRECTORY);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_begin_account(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    static char payload[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    static const char prefix[] = "{\"termsOfServiceAgreed\":true,\"jwk\":";
    size_t position = 0U;
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_READY) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_key_jwk(key) == NULL) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
            APPLIANCE_ACME_FLOW_KEY_REQUIRED);
    }
    memset(payload, 0, sizeof(payload));
    memcpy(&payload[position], prefix, strlen(prefix));
    position += strlen(prefix);
    {
        const char *jwk = appliance_acme_key_jwk(key);
        const size_t length = strlen(jwk);

        if (position + length + 2U >= sizeof(payload)) {
            return appliance_acme_flow_fail(
                flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
                APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL);
        }
        memcpy(&payload[position], jwk, length);
        position += length;
    }
    payload[position++] = (char)'}';
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length, flow->directory.new_account.text,
        flow->directory.new_account.length, true, payload, position, f_rng,
        rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT, status);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_ACCOUNT;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_account_result(
    struct appliance_acme_flow *flow,
    const char *account_json,
    size_t account_length,
    const char *account_url,
    size_t account_url_length)
{
    enum appliance_acme_status account_status = APPLIANCE_ACME_STATUS_PENDING;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_parse_account(
            account_json, account_length, &account_status) !=
            APPLIANCE_ACME_MESSAGE_OK ||
        account_status != APPLIANCE_ACME_STATUS_VALID) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
            APPLIANCE_ACME_FLOW_BAD_STATUS);
    }
    if (!appliance_acme_flow_url_valid(account_url, account_url_length)) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT,
            APPLIANCE_ACME_FLOW_BAD_URL);
    }
    appliance_acme_flow_set_url(
        &flow->account_url, account_url, account_url_length);
    flow->state = APPLIANCE_ACME_FLOW_STATE_READY;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_ACCOUNT;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_ACCOUNT);
    return APPLIANCE_ACME_FLOW_OK;
}

static bool appliance_acme_flow_name_valid(
    const char *text,
    size_t length)
{
    size_t label = 0U;
    bool hyphen = false;

    if (text == NULL || length == 0U || length > APPLIANCE_ACME_NAME_MAX) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value == (unsigned char)'.') {
            if (label == 0U || hyphen) {
                return false;
            }
            label = 0U;
            continue;
        }
        if (!((value >= (unsigned char)'a' && value <= (unsigned char)'z') ||
              (value >= (unsigned char)'0' && value <= (unsigned char)'9') ||
              value == (unsigned char)'-')) {
            return false;
        }
        if (value == (unsigned char)'-' && label == 0U) {
            return false;
        }
        hyphen = value == (unsigned char)'-';
        ++label;
        if (label > 63U) {
            return false;
        }
    }
    return label != 0U && !hyphen;
}

enum appliance_acme_flow_status appliance_acme_flow_declare_identifiers(
    struct appliance_acme_flow *flow,
    const char *const *names,
    const size_t *name_lengths,
    uint32_t count)
{
    if (flow == NULL || names == NULL || name_lengths == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (!flow->initialized) {
        return APPLIANCE_ACME_FLOW_BAD_STATE;
    }
    if (count == 0U || count > APPLIANCE_ACME_FLOW_IDENTIFIER_MAX) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
            APPLIANCE_ACME_FLOW_TOO_MANY_IDENTIFIERS);
    }
    for (uint32_t index = 0U; index < count; ++index) {
        if (!appliance_acme_flow_name_valid(names[index],
                                            name_lengths[index])) {
            return appliance_acme_flow_fail(
                flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
                APPLIANCE_ACME_FLOW_BAD_REPLY);
        }
    }
    memset(flow->identifiers, 0, sizeof(flow->identifiers));
    for (uint32_t index = 0U; index < count; ++index) {
        memcpy(flow->identifiers[index].name, names[index],
               name_lengths[index]);
        flow->identifiers[index].name_length =
            (uint32_t)name_lengths[index];
    }
    flow->identifier_count = count;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    return APPLIANCE_ACME_FLOW_OK;
}

static enum appliance_acme_flow_status appliance_acme_flow_order_payload(
    struct appliance_acme_flow *flow,
    char *payload,
    size_t capacity,
    size_t *length_out)
{
    static const char prefix[] = "{\"identifiers\":[";
    static const char item_prefix[] = "{\"type\":\"dns\",\"value\":\"";
    size_t position = 0U;

    if (flow->identifier_count == 0U) {
        return APPLIANCE_ACME_FLOW_BAD_STATE;
    }
    memset(payload, 0, capacity);
    memcpy(&payload[position], prefix, strlen(prefix));
    position += strlen(prefix);
    for (uint32_t index = 0U; index < flow->identifier_count; ++index) {
        const struct appliance_acme_identifier *identifier =
            &flow->identifiers[index];

        if (index != 0U) {
            payload[position++] = (char)',';
        }
        if (position + strlen(item_prefix) + identifier->name_length + 4U >=
            capacity) {
            return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
        }
        memcpy(&payload[position], item_prefix, strlen(item_prefix));
        position += strlen(item_prefix);
        memcpy(&payload[position], identifier->name,
               identifier->name_length);
        position += identifier->name_length;
        memcpy(&payload[position], "\"}", 2U);
        position += 2U;
    }
    memcpy(&payload[position], "]}", 2U);
    position += 2U;
    *length_out = position;
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_begin_order(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    static char payload[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    size_t payload_length = 0U;
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_READY ||
        flow->account_url.length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_order_payload(
        flow, payload, sizeof(payload), &payload_length);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER, status);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length, flow->directory.new_order.text,
        flow->directory.new_order.length, false, payload, payload_length,
        f_rng, rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER, status);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_ORDER;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_ORDER);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_order_result(
    struct appliance_acme_flow *flow,
    const char *order_json,
    size_t order_length,
    const char *order_url,
    size_t order_url_length)
{
    struct appliance_acme_order order;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_parse_order(order_json, order_length, &order) !=
            APPLIANCE_ACME_MESSAGE_OK ||
        (order.status != APPLIANCE_ACME_STATUS_PENDING &&
         order.status != APPLIANCE_ACME_STATUS_READY) ||
        order.authorization_count == 0U) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
            APPLIANCE_ACME_FLOW_BAD_REPLY);
    }
    if (!appliance_acme_flow_url_valid(order_url, order_url_length)) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_ORDER,
            APPLIANCE_ACME_FLOW_BAD_URL);
    }
    memset(&flow->order_url, 0, sizeof(flow->order_url));
    memcpy(flow->order_url.text, order_url, order_url_length);
    flow->order_url.length = (uint32_t)order_url_length;
    flow->order = order;
    flow->state = APPLIANCE_ACME_FLOW_STATE_READY;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_ORDER;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_ORDER);
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * POST-as-GET: the payload of the JWS is the empty string, which RFC 8555
 * section 6.3 defines for a read of a resource the account owns.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_authorization(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_READY ||
        flow->order.authorization_count == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length,
        flow->order.authorizations[0].text,
        flow->order.authorizations[0].length, false, "", 0U, f_rng,
        rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION, status);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_AUTHORIZATION_PENDING;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION;
    appliance_acme_flow_publish(
        flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_authorization_result(
    struct appliance_acme_flow *flow,
    const char *authorization_json,
    size_t authorization_length)
{
    struct appliance_acme_authorization authorization;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_AUTHORIZATION_PENDING) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_parse_authorization(
            authorization_json, authorization_length, &authorization) !=
            APPLIANCE_ACME_MESSAGE_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION,
            APPLIANCE_ACME_FLOW_BAD_REPLY);
    }
    flow->authorization = authorization;
    flow->state = APPLIANCE_ACME_FLOW_STATE_READY;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION;
    appliance_acme_flow_publish(
        flow, APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION);
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * RFC 8555 section 8.1: the key authorization is the token, a dot, and the
 * base64url JWK thumbprint of the account key.  The HTTP-01 listener will
 * serve exactly this string for the challenge's token.
 */
enum appliance_acme_flow_status appliance_acme_flow_key_authorization(
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    const char *thumbprint;
    size_t total;

    if (flow == NULL || key == NULL || out == NULL || length_out == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (!flow->initialized || !flow->authorization.has_http01) {
        return APPLIANCE_ACME_FLOW_BAD_STATE;
    }
    thumbprint = appliance_acme_key_thumbprint(key);
    if (thumbprint == NULL) {
        return APPLIANCE_ACME_FLOW_KEY_REQUIRED;
    }
    total = (size_t)flow->authorization.http01.token_length + 1U +
            strlen(thumbprint);
    if (total >= capacity) {
        return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
    }
    memcpy(out, flow->authorization.http01.token,
           flow->authorization.http01.token_length);
    out[flow->authorization.http01.token_length] = '.';
    memcpy(&out[flow->authorization.http01.token_length + 1U], thumbprint,
           strlen(thumbprint));
    out[total] = '\0';
    *length_out = total;
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * ITEM-508 increment 30: the token of the parsed `http-01` challenge.  The
 * listener is armed with this token and with the key authorization above, so
 * both halves come from the same parsed authorization and neither is a caller
 * literal.  Nothing is copied when the caller's buffer is too small.
 */
enum appliance_acme_flow_status appliance_acme_flow_http01_token(
    const struct appliance_acme_flow *flow,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    size_t token_length;

    if (flow == NULL || out == NULL || length_out == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (!flow->initialized || !flow->authorization.has_http01) {
        return APPLIANCE_ACME_FLOW_BAD_STATE;
    }
    token_length = (size_t)flow->authorization.http01.token_length;
    if (token_length == 0U) {
        return APPLIANCE_ACME_FLOW_BAD_STATE;
    }
    if (token_length >= capacity) {
        return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
    }
    memcpy(out, flow->authorization.http01.token, token_length);
    out[token_length] = '\0';
    *length_out = token_length;
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * POST {} to the challenge: the empty JSON object tells the CA to validate.
 * RFC 8555 section 7.5.1 defines that body, so it is built here, not fetched.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_challenge(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_READY ||
        !flow->authorization.has_http01 ||
        flow->authorization.http01.url.length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length,
        flow->authorization.http01.url.text,
        flow->authorization.http01.url.length, false, "{}", 2U, f_rng,
        rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE, status);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_CHALLENGE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE);
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * ITEM-508 increment 18: a CA that answered `processing` is polled with
 * POST-as-GET to the same challenge URL - the empty payload and the account's
 * `kid` again (RFC 8555 section 7.5.1) - until `challenge_result` reads
 * `valid`.  A poll before the first answer or after it is out of order.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_challenge_poll(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING ||
        !flow->authorization.has_http01 ||
        flow->authorization.http01.url.length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length,
        flow->authorization.http01.url.text,
        flow->authorization.http01.url.length, false, "", 0U, f_rng,
        rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE, status);
    }
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_CHALLENGE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_challenge_result(
    struct appliance_acme_flow *flow,
    const char *challenge_json,
    size_t challenge_length)
{
    struct appliance_acme_challenge challenge;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_parse_challenge(
            challenge_json, challenge_length, &challenge) !=
            APPLIANCE_ACME_MESSAGE_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE,
            APPLIANCE_ACME_FLOW_BAD_REPLY);
    }
    if (challenge.status == APPLIANCE_ACME_STATUS_INVALID) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE,
            APPLIANCE_ACME_FLOW_BAD_STATUS);
    }
    /*
     * `pending` and `processing` mean the CA has not finished; the caller
     * polls the same URL again.  `valid` moves the order on to finalize.
     */
    flow->challenge = challenge;
    if (challenge.status == APPLIANCE_ACME_STATUS_VALID) {
        flow->state = APPLIANCE_ACME_FLOW_STATE_READY;
    }
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_CHALLENGE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CHALLENGE);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_begin_finalize(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    const unsigned char *csr,
    size_t csr_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    static const char payload_prefix[] = "{\"csr\":\"";
    static char payload[APPLIANCE_ACME_FLOW_PAYLOAD_MAX];
    enum appliance_acme_flow_status status;
    enum appliance_acme_base64_status base64_status;
    size_t csr_encoded_length = 0U;
    size_t payload_length = 0U;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_READY ||
        flow->order.finalize.length == 0U ||
        flow->challenge.status != APPLIANCE_ACME_STATUS_VALID) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    /*
     * ITEM-508 increment 22: a real CA needs the PKCS#10 in the finalize
     * payload, so an absent or oversized CSR is a named refusal - the flow
     * must never send `{}` in its place.
     */
    if (csr == NULL || csr_length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_CSR_REQUIRED);
    }
    if (csr_length > APPLIANCE_ACME_CSR_MAX) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_CSR_TOO_LARGE);
    }
    /*
     * `{"csr":"<unpadded base64url>"}` built here from the caller's DER.  The
     * buffer keeps room for the closing quote, the brace and the terminator,
     * so an encoder that runs out of room is a refusal, never a cut request.
     */
    memcpy(payload, payload_prefix, sizeof(payload_prefix) - 1U);
    base64_status = appliance_acme_base64url_encode(
        csr, csr_length, &payload[sizeof(payload_prefix) - 1U],
        sizeof(payload) - sizeof(payload_prefix) - 2U, &csr_encoded_length);
    if (base64_status != APPLIANCE_ACME_BASE64_OK) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_CSR_TOO_LARGE);
    }
    payload_length = sizeof(payload_prefix) - 1U + csr_encoded_length;
    payload[payload_length++] = (char)'"';
    payload[payload_length++] = (char)'}';
    payload[payload_length] = '\0';

    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length, flow->order.finalize.text,
        flow->order.finalize.length, false, payload, payload_length, f_rng,
        rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE, status);
    }
    flow->state = APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_FINALIZE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE);
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_finalize_result(
    struct appliance_acme_flow *flow,
    const char *order_json,
    size_t order_length)
{
    struct appliance_acme_order order;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING &&
        flow->state != APPLIANCE_ACME_FLOW_STATE_READY) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (appliance_acme_parse_order(order_json, order_length, &order) !=
            APPLIANCE_ACME_MESSAGE_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_BAD_REPLY);
    }
    flow->order = order;
    if (order.status == APPLIANCE_ACME_STATUS_VALID) {
        if (!order.has_certificate) {
            return appliance_acme_flow_fail(
                flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                APPLIANCE_ACME_FLOW_BAD_REPLY);
        }
        flow->certificate_url = order.certificate;
        flow->state = APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY;
        flow->last_step = APPLIANCE_ACME_FLOW_STEP_CERTIFICATE;
        flow->last_status = APPLIANCE_ACME_FLOW_OK;
        appliance_acme_flow_publish(
            flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE);
        return APPLIANCE_ACME_FLOW_OK;
    }
    if (order.status == APPLIANCE_ACME_STATUS_PENDING ||
        order.status == APPLIANCE_ACME_STATUS_PROCESSING ||
        order.status == APPLIANCE_ACME_STATUS_READY) {
        /*
         * The CA has not finished issuing; the caller polls the order again
         * and calls this function once more.
         */
        flow->last_status = APPLIANCE_ACME_FLOW_OK;
        flow->last_step = APPLIANCE_ACME_FLOW_STEP_FINALIZE;
        appliance_acme_flow_publish(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE);
        return APPLIANCE_ACME_FLOW_OK;
    }
    return appliance_acme_flow_fail(
        flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
        APPLIANCE_ACME_FLOW_BAD_STATUS);
}

/*
 * ITEM-508 increment 19: a finalize the CA answered as still pending is
 * polled at the order URL it named in its `Location` - POST-as-GET, the empty
 * payload and the account's `kid` (RFC 8555 section 7.4) - until
 * `finalize_result` reads a valid order with its certificate URL.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_order_poll(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING ||
        flow->order_url.length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length, flow->order_url.text,
        flow->order_url.length, false, "", 0U, f_rng, rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE, status);
    }
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_FINALIZE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_FINALIZE);
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * ITEM-508 increment 17: the certificate download is a POST-as-GET like the
 * authorization read - empty payload, the account's `kid` - so the last step
 * of the order travels through the same transport as every other step.  The
 * state stays `CERTIFICATE_READY`: it names "the URL is known", and only
 * `certificate_result` moves the flow to `CERTIFICATE_VERIFIED`.
 */
enum appliance_acme_flow_status appliance_acme_flow_begin_certificate(
    struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    const char *nonce,
    size_t nonce_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    enum appliance_acme_flow_status status;

    if (flow == NULL || !flow->initialized) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY ||
        flow->certificate_url.length == 0U) {
        return appliance_acme_flow_refuse(
            flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
            APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    status = appliance_acme_flow_build(
        flow, key, nonce, nonce_length, flow->certificate_url.text,
        flow->certificate_url.length, false, "", 0U, f_rng, rng_context);
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(
            flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE, status);
    }
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_CERTIFICATE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE);
    return APPLIANCE_ACME_FLOW_OK;
}

/*
 * ITEM-508 increment 8: the certificate chain the CA hands back is decoded and
 * trusted before it is published.  The body of a certificate download is a PEM
 * chain (`application/pem-certificate-chain`, RFC 8555 section 7.4.2), so the
 * step walks the armour, decodes each member into the caller's bounded DER
 * buffer and verifies the whole chain - leaf first - against the named anchor
 * and the first identifier the order declared.  A chain the appliance cannot
 * trust is a refusal, never a stored certificate.
 */

static bool appliance_acme_flow_find(
    const char *text,
    size_t length,
    size_t from,
    const char *needle,
    size_t *position_out)
{
    const size_t needle_length = strlen(needle);

    if (text == NULL || needle == NULL || position_out == NULL ||
        needle_length == 0U || from > length) {
        return false;
    }
    for (size_t index = from; index + needle_length <= length; ++index) {
        if (memcmp(&text[index], needle, needle_length) == 0) {
            *position_out = index;
            return true;
        }
    }
    return false;
}

static enum appliance_acme_flow_status appliance_acme_flow_chain_decode(
    const char *pem,
    size_t pem_length,
    unsigned char *der_out,
    size_t der_capacity,
    size_t *der_length_out,
    uint32_t *member_count_out)
{
    static const char begin_marker[] = "-----BEGIN CERTIFICATE-----";
    static const char end_marker[] = "-----END CERTIFICATE-----";
    size_t offset = 0U;
    size_t der_length = 0U;
    uint32_t count = 0U;

    if (pem == NULL || der_out == NULL || der_length_out == NULL ||
        member_count_out == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    for (;;) {
        size_t begin = 0U;
        size_t end = 0U;
        size_t written = 0U;
        enum pem_decode_status decoded;

        if (!appliance_acme_flow_find(pem, pem_length, offset, begin_marker,
                                      &begin)) {
            break;
        }
        if (!appliance_acme_flow_find(pem, pem_length, begin, end_marker,
                                      &end)) {
            return APPLIANCE_ACME_FLOW_BAD_CHAIN;
        }
        if (count >= APPLIANCE_ACME_TRUST_CHAIN_MEMBERS_MAX) {
            return APPLIANCE_ACME_FLOW_BAD_CHAIN;
        }
        if (der_length >= der_capacity) {
            return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
        }
        decoded = pem_decode_block(&pem[begin], pem_length - begin,
                                   "CERTIFICATE", &der_out[der_length],
                                   der_capacity - der_length, &written);
        if (decoded == PEM_DECODE_TOO_LARGE) {
            return APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL;
        }
        if (decoded != PEM_DECODE_OK) {
            return APPLIANCE_ACME_FLOW_BAD_CHAIN;
        }
        der_length += written;
        count += 1U;
        offset = end + (sizeof(end_marker) - 1U);
    }
    if (count == 0U) {
        return APPLIANCE_ACME_FLOW_BAD_CHAIN;
    }
    *der_length_out = der_length;
    *member_count_out = count;
    return APPLIANCE_ACME_FLOW_OK;
}

enum appliance_acme_flow_status appliance_acme_flow_certificate_result(
    struct appliance_acme_flow *flow,
    struct appliance_acme_trust *trust,
    const char *pem,
    size_t pem_length,
    uint32_t now_epoch_seconds,
    unsigned char *der_out,
    size_t der_capacity,
    size_t *der_length_out,
    uint32_t *member_count_out)
{
    enum appliance_acme_flow_status status;
    enum appliance_acme_trust_status trust_status;
    size_t der_length = 0U;
    uint32_t member_count = 0U;

    if (flow == NULL || !flow->initialized || trust == NULL ||
        pem == NULL || der_out == NULL || der_length_out == NULL ||
        member_count_out == NULL) {
        return APPLIANCE_ACME_FLOW_NULL;
    }
    *der_length_out = 0U;
    *member_count_out = 0U;
    if (flow->state != APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY ||
        flow->identifier_count == 0U) {
        return appliance_acme_flow_refuse(flow,
                                          APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                                          APPLIANCE_ACME_FLOW_BAD_STATE);
    }
    if (now_epoch_seconds == 0U) {
        return appliance_acme_flow_refuse(flow,
                                          APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                                          APPLIANCE_ACME_FLOW_BAD_CLOCK);
    }
    status = appliance_acme_flow_chain_decode(pem, pem_length, der_out,
                                              der_capacity, &der_length,
                                              &member_count);
    if (status == APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL) {
        /* The caller's buffer is a caller problem: the order may retry. */
        return appliance_acme_flow_refuse(flow,
                                          APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                                          status);
    }
    if (status != APPLIANCE_ACME_FLOW_OK) {
        return appliance_acme_flow_fail(flow,
                                        APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                                        status);
    }
    trust_status = appliance_acme_trust_verify(
        trust, der_out, der_length, flow->identifiers[0].name,
        flow->identifiers[0].name_length, now_epoch_seconds);
    if (trust_status != APPLIANCE_ACME_TRUST_OK) {
        return appliance_acme_flow_fail(flow,
                                        APPLIANCE_ACME_FLOW_STEP_CERTIFICATE,
                                        APPLIANCE_ACME_FLOW_UNTRUSTED);
    }
    *der_length_out = der_length;
    *member_count_out = member_count;
    flow->certificate_verified = true;
    flow->state = APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_VERIFIED;
    flow->last_status = APPLIANCE_ACME_FLOW_OK;
    flow->last_step = APPLIANCE_ACME_FLOW_STEP_CERTIFICATE;
    appliance_acme_flow_publish(flow, APPLIANCE_ACME_FLOW_STEP_CERTIFICATE);
    return APPLIANCE_ACME_FLOW_OK;
}

const char *appliance_acme_flow_state_string(
    enum appliance_acme_flow_state state)
{
    switch (state) {
    case APPLIANCE_ACME_FLOW_STATE_NONE:
        return "NONE";
    case APPLIANCE_ACME_FLOW_STATE_AWAITING_CLOCK:
        return "AWAITING_CLOCK";
    case APPLIANCE_ACME_FLOW_STATE_READY:
        return "READY";
    case APPLIANCE_ACME_FLOW_STATE_ACCOUNT_PENDING:
        return "ACCOUNT_PENDING";
    case APPLIANCE_ACME_FLOW_STATE_ORDER_PENDING:
        return "ORDER_PENDING";
    case APPLIANCE_ACME_FLOW_STATE_AUTHORIZATION_PENDING:
        return "AUTHORIZATION_PENDING";
    case APPLIANCE_ACME_FLOW_STATE_CHALLENGE_PENDING:
        return "CHALLENGE_PENDING";
    case APPLIANCE_ACME_FLOW_STATE_FINALIZE_PENDING:
        return "FINALIZE_PENDING";
    case APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_READY:
        return "CERTIFICATE_READY";
    case APPLIANCE_ACME_FLOW_STATE_CERTIFICATE_VERIFIED:
        return "CERTIFICATE_VERIFIED";
    case APPLIANCE_ACME_FLOW_STATE_FAILED:
        return "FAILED";
    default:
        break;
    }
    return "UNKNOWN";
}

const char *appliance_acme_flow_status_string(
    enum appliance_acme_flow_status status)
{
    switch (status) {
    case APPLIANCE_ACME_FLOW_OK:
        return "OK";
    case APPLIANCE_ACME_FLOW_NULL:
        return "NULL";
    case APPLIANCE_ACME_FLOW_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_FLOW_BAD_CLOCK:
        return "BAD_CLOCK";
    case APPLIANCE_ACME_FLOW_BAD_REPLY:
        return "BAD_REPLY";
    case APPLIANCE_ACME_FLOW_BAD_STATUS:
        return "BAD_STATUS";
    case APPLIANCE_ACME_FLOW_BAD_NONCE:
        return "BAD_NONCE";
    case APPLIANCE_ACME_FLOW_BAD_URL:
        return "BAD_URL";
    case APPLIANCE_ACME_FLOW_TOO_MANY_IDENTIFIERS:
        return "TOO_MANY_IDENTIFIERS";
    case APPLIANCE_ACME_FLOW_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case APPLIANCE_ACME_FLOW_KEY_REQUIRED:
        return "KEY_REQUIRED";
    case APPLIANCE_ACME_FLOW_BAD_CHAIN:
        return "BAD_CHAIN";
    case APPLIANCE_ACME_FLOW_UNTRUSTED:
        return "UNTRUSTED";
    case APPLIANCE_ACME_FLOW_CSR_REQUIRED:
        return "CSR_REQUIRED";
    case APPLIANCE_ACME_FLOW_CSR_TOO_LARGE:
        return "CSR_TOO_LARGE";
    default:
        break;
    }
    return "UNKNOWN";
}

const char *appliance_acme_flow_step_string(
    enum appliance_acme_flow_step step)
{
    switch (step) {
    case APPLIANCE_ACME_FLOW_STEP_NONE:
        return "NONE";
    case APPLIANCE_ACME_FLOW_STEP_DIRECTORY:
        return "DIRECTORY";
    case APPLIANCE_ACME_FLOW_STEP_ACCOUNT:
        return "ACCOUNT";
    case APPLIANCE_ACME_FLOW_STEP_ORDER:
        return "ORDER";
    case APPLIANCE_ACME_FLOW_STEP_AUTHORIZATION:
        return "AUTHORIZATION";
    case APPLIANCE_ACME_FLOW_STEP_KEY_AUTHORIZATION:
        return "KEY_AUTHORIZATION";
    case APPLIANCE_ACME_FLOW_STEP_CHALLENGE:
        return "CHALLENGE";
    case APPLIANCE_ACME_FLOW_STEP_FINALIZE:
        return "FINALIZE";
    case APPLIANCE_ACME_FLOW_STEP_CERTIFICATE:
        return "CERTIFICATE";
    default:
        break;
    }
    return "UNKNOWN";
}

#endif /* HFOC_PRODUCTION_TLS || HFOC_APPLIANCE_ACME_FLOW_HOST_TEST */
