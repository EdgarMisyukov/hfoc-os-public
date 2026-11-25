#include "appliance_management_tls.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-375 management TLS lifecycle and key rotation.
 *
 * This module is allocation-free and metadata-only.  It never receives,
 * copies, stores or renders a private key or a certificate payload: the DER
 * bytes stay with the caller and the mbedTLS manifest boundary, and the
 * rendered record carries the fixed "secrets=NONE" marker.
 */

static const struct appliance_management_tls_parameters
    appliance_management_tls_frozen_policy = {
        APPLIANCE_MANAGEMENT_TLS_VERSION,
        APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13,
        APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256,
        APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_ECDHE_P256,
        APPLIANCE_MANAGEMENT_TLS_SIGNATURE_ECDSA_P256_SHA256,
        false,
        false,
};

static bool appliance_management_tls_protocol_valid(
    enum appliance_management_tls_protocol protocol)
{
    return protocol >= APPLIANCE_MANAGEMENT_TLS_PROTOCOL_NONE &&
           protocol <= APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13;
}

static bool appliance_management_tls_cipher_valid(
    enum appliance_management_tls_cipher cipher)
{
    return cipher >= APPLIANCE_MANAGEMENT_TLS_CIPHER_NONE &&
           cipher <=
               APPLIANCE_MANAGEMENT_TLS_CIPHER_CHACHA20_POLY1305_SHA256;
}

static bool appliance_management_tls_key_exchange_valid(
    enum appliance_management_tls_key_exchange key_exchange)
{
    return key_exchange >= APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_NONE &&
           key_exchange <=
               APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_ECDHE_P256;
}

static bool appliance_management_tls_signature_valid(
    enum appliance_management_tls_signature signature)
{
    return signature >= APPLIANCE_MANAGEMENT_TLS_SIGNATURE_NONE &&
           signature <=
               APPLIANCE_MANAGEMENT_TLS_SIGNATURE_ECDSA_P256_SHA256;
}

static bool appliance_management_tls_certificate_key_valid(
    enum appliance_management_tls_certificate_key key)
{
    return key >= APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_NONE &&
           key <= APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_RSA_2048;
}

static bool appliance_management_tls_state_valid(
    enum appliance_management_tls_state state)
{
    return state >= APPLIANCE_MANAGEMENT_TLS_STATE_NONE &&
           state <= APPLIANCE_MANAGEMENT_TLS_STATE_STAGED;
}

static bool appliance_management_tls_status_valid(
    enum appliance_management_tls_status status)
{
    return status >= APPLIANCE_MANAGEMENT_TLS_OK &&
           status <= APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL;
}

/*
 * The frozen policy accepts exactly one profile: TLS 1.3 with
 * TLS_AES_128_GCM_SHA256, ephemeral P-256 ECDHE and P-256 ECDSA/SHA-256.
 * Everything else is refused with the exact reason, so a legacy protocol, a
 * non-AEAD or weak cipher, a static key exchange, a weak signature,
 * compression and renegotiation can never be negotiated by accident.
 */
enum appliance_management_tls_policy_verdict
appliance_management_tls_policy_check(
    const struct appliance_management_tls_parameters *parameters)
{
    if (parameters == NULL ||
        parameters->version != APPLIANCE_MANAGEMENT_TLS_VERSION) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL;
    }
    if (!appliance_management_tls_protocol_valid(parameters->protocol)) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL;
    }
    if (parameters->protocol != APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13) {
        return parameters->protocol ==
                       APPLIANCE_MANAGEMENT_TLS_PROTOCOL_NONE
                   ? APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL
                   : APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL;
    }
    if (!appliance_management_tls_cipher_valid(parameters->cipher)) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_CIPHER;
    }
    if (parameters->cipher !=
        APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256) {
        return parameters->cipher == APPLIANCE_MANAGEMENT_TLS_CIPHER_NONE
                   ? APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_CIPHER
                   : APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER;
    }
    if (!appliance_management_tls_key_exchange_valid(
            parameters->key_exchange)) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_KEY_EXCHANGE;
    }
    if (parameters->key_exchange !=
        APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_ECDHE_P256) {
        return parameters->key_exchange ==
                       APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_NONE
                   ? APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_KEY_EXCHANGE
                   : APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE;
    }
    if (!appliance_management_tls_signature_valid(parameters->signature)) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_SIGNATURE;
    }
    if (parameters->signature !=
        APPLIANCE_MANAGEMENT_TLS_SIGNATURE_ECDSA_P256_SHA256) {
        return parameters->signature ==
                       APPLIANCE_MANAGEMENT_TLS_SIGNATURE_NONE
                   ? APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_SIGNATURE
                   : APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE;
    }
    if (parameters->compression_enabled) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_COMPRESSION_ENABLED;
    }
    if (parameters->renegotiation_enabled) {
        return APPLIANCE_MANAGEMENT_TLS_POLICY_RENEGOTIATION_ENABLED;
    }
    return APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED;
}

const struct appliance_management_tls_parameters *
appliance_management_tls_policy_frozen(void)
{
    return &appliance_management_tls_frozen_policy;
}

static bool appliance_management_tls_identity_field_valid(
    const char *field,
    uint8_t length,
    size_t maximum)
{
    size_t index;

    if (field == NULL || length == 0U || (size_t)length > maximum) {
        return false;
    }
    if (field[length] != '\0') {
        return false;
    }
    for (index = 0U; index < (size_t)length; ++index) {
        const char character = field[index];

        if ((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') ||
            (character >= '0' && character <= '9') ||
            character == '.' || character == '-') {
            continue;
        }
        return false;
    }
    return true;
}

static bool appliance_management_tls_fingerprint_present(
    const uint8_t fingerprint[APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH])
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH;
         ++index) {
        if (fingerprint[index] != 0U) {
            return true;
        }
    }
    return false;
}

static bool appliance_management_tls_certificate_key_allowed(
    enum appliance_management_tls_certificate_key key)
{
    return key == APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256 ||
           key == APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_RSA_2048;
}

/*
 * Structural validation only: the policy level of the certificate (key
 * strength) and the clock window are separate, separately reported reasons.
 */
static bool appliance_management_tls_certificate_shape_valid(
    const struct appliance_management_tls_certificate *certificate)
{
    return certificate != NULL &&
           certificate->version == APPLIANCE_MANAGEMENT_TLS_VERSION &&
           certificate->present &&
           appliance_management_tls_identity_field_valid(
               certificate->common_name,
               certificate->common_name_length,
               APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX) &&
           appliance_management_tls_identity_field_valid(
               certificate->subject_alt_name,
               certificate->subject_alt_name_length,
               APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX) &&
           certificate->issuer_length != 0U &&
           (size_t)certificate->issuer_length <=
               APPLIANCE_MANAGEMENT_TLS_ISSUER_MAX &&
           certificate->issuer[certificate->issuer_length] == '\0' &&
           appliance_management_tls_certificate_key_valid(
               certificate->key) &&
           certificate->key != APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_NONE &&
           certificate->not_after_epoch > certificate->not_before_epoch &&
           appliance_management_tls_fingerprint_present(
               certificate->fingerprint);
}

static bool appliance_management_tls_identity_matches(
    const struct appliance_management_tls_identity *identity,
    const struct appliance_management_tls_certificate *certificate)
{
    return identity->common_name_length ==
               certificate->common_name_length &&
           certificate->subject_alt_name_length ==
               identity->subject_alt_name_length &&
           memcmp(identity->common_name, certificate->common_name,
                  (size_t)identity->common_name_length) == 0 &&
           memcmp(identity->subject_alt_name,
                  certificate->subject_alt_name,
                  (size_t)identity->subject_alt_name_length) == 0;
}

static enum appliance_management_tls_status
appliance_management_tls_key_policy(
    const struct appliance_management_tls_certificate *certificate)
{
    return appliance_management_tls_certificate_key_allowed(
               certificate->key)
               ? APPLIANCE_MANAGEMENT_TLS_OK
               : APPLIANCE_MANAGEMENT_TLS_WEAK_CERTIFICATE_KEY;
}

static enum appliance_management_tls_status
appliance_management_tls_clock_policy(
    const struct appliance_management_tls_certificate *certificate,
    uint32_t now_epoch_seconds)
{
    if (now_epoch_seconds < certificate->not_before_epoch) {
        return APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID;
    }
    if (now_epoch_seconds >= certificate->not_after_epoch) {
        return APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_EXPIRED;
    }
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

static enum appliance_management_tls_status appliance_management_tls_admit(
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress)
{
    if (ingress->kind == APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL) {
        return APPLIANCE_MANAGEMENT_TLS_OK;
    }
    if (ingress->kind != APPLIANCE_MANAGEMENT_TLS_INGRESS_NETWORK) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_INGRESS;
    }
    switch (appliance_management_bind_admit(
        bind, ingress->management_interface, ingress->address)) {
    case APPLIANCE_MANAGEMENT_BIND_ALLOWED:
        return APPLIANCE_MANAGEMENT_TLS_OK;
    case APPLIANCE_MANAGEMENT_BIND_DENIED_PEER:
        return APPLIANCE_MANAGEMENT_TLS_DENIED_PEER;
    case APPLIANCE_MANAGEMENT_BIND_DENIED_OUTER:
        return APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER;
    case APPLIANCE_MANAGEMENT_BIND_DENIED_UNCONFIGURED:
    case APPLIANCE_MANAGEMENT_BIND_NULL:
    default:
        return APPLIANCE_MANAGEMENT_TLS_DENIED_UNCONFIGURED;
    }
}

/*
 * The ingress gate plus the operational state, in the order every operation
 * shares: null arguments, ingress version, ingress admission, state.  A
 * refused operation never mutates the record beyond the reject counter.
 */
static enum appliance_management_tls_status
appliance_management_tls_operation_gate(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress)
{
    enum appliance_management_tls_status status;

    if (tls == NULL || ingress == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    if (ingress->version != APPLIANCE_MANAGEMENT_TLS_VERSION) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_INGRESS;
    }
    status = appliance_management_tls_admit(bind, ingress);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        return status;
    }
    if (!tls->initialized) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }
    if (tls->version != APPLIANCE_MANAGEMENT_TLS_VERSION) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_VERSION;
    }
    if (appliance_management_tls_policy_check(&tls->policy) !=
        APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_POLICY;
    }
    if (!appliance_management_tls_state_valid(tls->state) ||
        tls->state == APPLIANCE_MANAGEMENT_TLS_STATE_NONE) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

static enum appliance_management_tls_status
appliance_management_tls_reject(
    struct appliance_management_tls *tls,
    enum appliance_management_tls_status status)
{
    if (tls != NULL) {
        tls->reject_count += 1U;
        tls->last_status = status;
    }
    return status;
}

enum appliance_management_tls_status appliance_management_tls_init(
    struct appliance_management_tls *tls)
{
    if (tls == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    memset(tls, 0, sizeof(*tls));
    tls->version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    tls->policy = appliance_management_tls_frozen_policy;
    tls->state = APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED;
    tls->last_status = APPLIANCE_MANAGEMENT_TLS_OK;
    tls->secrets_logged = true;
    tls->initialized = true;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status
appliance_management_tls_request_certificate(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    const char *common_name,
    size_t common_name_length,
    const char *subject_alt_name,
    size_t subject_alt_name_length,
    uint32_t now_epoch_seconds)
{
    enum appliance_management_tls_status status;
    uint32_t request_id;

    if (tls == NULL || ingress == NULL || common_name == NULL ||
        subject_alt_name == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    if (common_name_length == 0U ||
        common_name_length > APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX ||
        subject_alt_name_length == 0U ||
        subject_alt_name_length > APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX ||
        common_name_length > UINT8_MAX ||
        subject_alt_name_length > UINT8_MAX) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_BAD_IDENTITY);
    }
    status = appliance_management_tls_operation_gate(tls, bind, ingress);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        return status == APPLIANCE_MANAGEMENT_TLS_NULL
                   ? status
                   : appliance_management_tls_reject(tls, status);
    }
    if (tls->request.pending) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_REQUEST_PENDING);
    }

    memset(&tls->request, 0, sizeof(tls->request));
    tls->request.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    memcpy(tls->request.identity.common_name, common_name,
           common_name_length);
    tls->request.identity.common_name_length =
        (uint8_t)common_name_length;
    memcpy(tls->request.identity.subject_alt_name, subject_alt_name,
           subject_alt_name_length);
    tls->request.identity.subject_alt_name_length =
        (uint8_t)subject_alt_name_length;
    tls->request.identity.version = APPLIANCE_MANAGEMENT_TLS_VERSION;

    if (!appliance_management_tls_identity_field_valid(
            tls->request.identity.common_name,
            tls->request.identity.common_name_length,
            APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX) ||
        !appliance_management_tls_identity_field_valid(
            tls->request.identity.subject_alt_name,
            tls->request.identity.subject_alt_name_length,
            APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX)) {
        memset(&tls->request, 0, sizeof(tls->request));
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_BAD_IDENTITY);
    }

    request_id = tls->request_count + 1U;
    tls->request.pending = true;
    tls->request.request_id = request_id;
    tls->request.policy_generation = APPLIANCE_MANAGEMENT_TLS_VERSION;
    tls->request.created_at_epoch = now_epoch_seconds;
    tls->request_count = request_id;
    if (!tls->active.present) {
        tls->state = APPLIANCE_MANAGEMENT_TLS_STATE_AWAITING_CERTIFICATE;
    }
    tls->last_status = APPLIANCE_MANAGEMENT_TLS_OK;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status
appliance_management_tls_import_certificate(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    const struct appliance_management_tls_certificate *certificate,
    uint32_t now_epoch_seconds)
{
    enum appliance_management_tls_status status;

    if (tls == NULL || ingress == NULL || certificate == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    status = appliance_management_tls_operation_gate(tls, bind, ingress);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        return status == APPLIANCE_MANAGEMENT_TLS_NULL
                   ? status
                   : appliance_management_tls_reject(tls, status);
    }
    if (now_epoch_seconds == 0U) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_BAD_CLOCK);
    }
    if (!tls->request.pending) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_NO_PENDING_REQUEST);
    }
    if (!appliance_management_tls_certificate_shape_valid(certificate)) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_BAD_CERTIFICATE);
    }
    if (!appliance_management_tls_certificate_key_allowed(
            certificate->key)) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_WEAK_CERTIFICATE_KEY);
    }
    if (!appliance_management_tls_identity_matches(
            &tls->request.identity, certificate)) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_MISMATCH);
    }
    status = appliance_management_tls_clock_policy(certificate, now_epoch_seconds);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        return appliance_management_tls_reject(tls, status);
    }
    if (tls->active.present &&
        memcmp(tls->active.fingerprint, certificate->fingerprint,
               APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH) == 0) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE);
    }

    tls->request.pending = false;
    tls->import_count += 1U;
    tls->last_status = APPLIANCE_MANAGEMENT_TLS_OK;
    if (tls->active.present) {
        /*
         * Rotation: the new certificate is staged and the active
         * certificate keeps serving and stays ready until an explicit
         * promotion swaps the slot.
         */
        tls->staged = *certificate;
        tls->state = APPLIANCE_MANAGEMENT_TLS_STATE_STAGED;
        return APPLIANCE_MANAGEMENT_TLS_OK;
    }
    tls->active = *certificate;
    memset(&tls->staged, 0, sizeof(tls->staged));
    tls->state = APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status appliance_management_tls_promote(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    uint32_t now_epoch_seconds)
{
    enum appliance_management_tls_status status;
    bool was_active;

    if (tls == NULL || ingress == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    status = appliance_management_tls_operation_gate(tls, bind, ingress);
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        return status == APPLIANCE_MANAGEMENT_TLS_NULL
                   ? status
                   : appliance_management_tls_reject(tls, status);
    }
    if (now_epoch_seconds == 0U) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_BAD_CLOCK);
    }
    if (!tls->staged.present ||
        tls->state != APPLIANCE_MANAGEMENT_TLS_STATE_STAGED) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_NOT_STAGED);
    }
    status = appliance_management_tls_key_policy(&tls->staged);
    if (status == APPLIANCE_MANAGEMENT_TLS_OK) {
        status = appliance_management_tls_clock_policy(
            &tls->staged, now_epoch_seconds);
    }
    if (status != APPLIANCE_MANAGEMENT_TLS_OK) {
        /*
         * A certificate that is not yet valid may still be promoted once
         * its window opens, so it stays staged.  A permanently
         * unpromotable staged certificate is dropped and the listener
         * returns to serving the active certificate.
         */
        if (status !=
            APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID) {
            memset(&tls->staged, 0, sizeof(tls->staged));
            tls->state = tls->active.present
                             ? APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE
                             : APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED;
        }
        return appliance_management_tls_reject(tls, status);
    }

    was_active = tls->active.present;
    tls->active = tls->staged;
    memset(&tls->staged, 0, sizeof(tls->staged));
    tls->promote_count += 1U;
    if (was_active) {
        tls->rotation_count += 1U;
    }
    tls->state = APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE;
    tls->last_status = APPLIANCE_MANAGEMENT_TLS_OK;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status
appliance_management_tls_discard_staged(
    struct appliance_management_tls *tls)
{
    if (tls == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    if (!tls->initialized ||
        !appliance_management_tls_state_valid(tls->state)) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }
    if (!tls->staged.present ||
        tls->state != APPLIANCE_MANAGEMENT_TLS_STATE_STAGED) {
        return appliance_management_tls_reject(
            tls, APPLIANCE_MANAGEMENT_TLS_NOT_STAGED);
    }
    memset(&tls->staged, 0, sizeof(tls->staged));
    tls->request.pending = false;
    tls->state = tls->active.present
                     ? APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE
                     : APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED;
    tls->last_status = APPLIANCE_MANAGEMENT_TLS_OK;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

bool appliance_management_tls_certificate_ready(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds)
{
    if (tls == NULL || !tls->initialized ||
        tls->version != APPLIANCE_MANAGEMENT_TLS_VERSION) {
        return false;
    }
    /*
     * Epoch zero means the wall clock has not been validated yet; a
     * certificate is never published as ready against an unvalidated time.
     */
    if (now_epoch_seconds == 0U) {
        return false;
    }
    if (tls->state != APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
        tls->state != APPLIANCE_MANAGEMENT_TLS_STATE_STAGED) {
        return false;
    }
    if (!appliance_management_tls_certificate_shape_valid(&tls->active)) {
        return false;
    }
    if (!appliance_management_tls_certificate_key_allowed(
            tls->active.key)) {
        return false;
    }
    return appliance_management_tls_clock_policy(&tls->active, now_epoch_seconds) ==
           APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status appliance_management_tls_readiness(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds,
    bool *ready_out)
{
    if (tls == NULL || ready_out == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    *ready_out = appliance_management_tls_certificate_ready(tls, now_epoch_seconds);
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_status appliance_management_tls_snapshot(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds,
    struct appliance_management_tls_snapshot *snapshot_out)
{
    if (tls == NULL || snapshot_out == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    if (!tls->initialized ||
        !appliance_management_tls_state_valid(tls->state) ||
        tls->state == APPLIANCE_MANAGEMENT_TLS_STATE_NONE) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }
    if (tls->version != APPLIANCE_MANAGEMENT_TLS_VERSION) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_VERSION;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->published = true;
    snapshot_out->version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    snapshot_out->state = tls->state;
    snapshot_out->last_status = tls->last_status;
    snapshot_out->protocol = tls->policy.protocol;
    snapshot_out->cipher = tls->policy.cipher;
    snapshot_out->certificate_ready =
        appliance_management_tls_certificate_ready(tls, now_epoch_seconds);
    snapshot_out->request_pending = tls->request.pending;
    snapshot_out->staged_pending = tls->staged.present;
    snapshot_out->key_published = tls->active.present;
    snapshot_out->request_count = tls->request_count;
    snapshot_out->import_count = tls->import_count;
    snapshot_out->promote_count = tls->promote_count;
    snapshot_out->reject_count = tls->reject_count;
    snapshot_out->rotation_count = tls->rotation_count;
    snapshot_out->secrets_logged = tls->secrets_logged;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

static bool appliance_management_tls_append_text(
    char *rendered,
    size_t *offset,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        if (*offset >= APPLIANCE_MANAGEMENT_TLS_RENDER_MAX) {
            return false;
        }
        rendered[*offset] = text[index];
        *offset += 1U;
    }
    return true;
}

static bool appliance_management_tls_append_char(
    char *rendered,
    size_t *offset,
    char character)
{
    if (*offset >= APPLIANCE_MANAGEMENT_TLS_RENDER_MAX) {
        return false;
    }
    rendered[*offset] = character;
    *offset += 1U;
    return true;
}

static bool appliance_management_tls_append_u64(
    char *rendered,
    size_t *offset,
    uint64_t value)
{
    char digits[20];
    size_t count = 0U;

    do {
        digits[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    } while (value != 0U);
    while (count != 0U) {
        if (!appliance_management_tls_append_char(
                rendered, offset, digits[--count])) {
            return false;
        }
    }
    return true;
}

enum appliance_management_tls_status appliance_management_tls_render(
    const struct appliance_management_tls_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    char rendered[APPLIANCE_MANAGEMENT_TLS_RENDER_MAX];
    size_t offset = 0U;
    size_t index;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_MANAGEMENT_TLS_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_MANAGEMENT_TLS_VERSION ||
        !appliance_management_tls_state_valid(snapshot->state) ||
        snapshot->state == APPLIANCE_MANAGEMENT_TLS_STATE_NONE ||
        !appliance_management_tls_status_valid(snapshot->last_status)) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }
    if (!appliance_management_tls_protocol_valid(snapshot->protocol) ||
        !appliance_management_tls_cipher_valid(snapshot->cipher)) {
        return APPLIANCE_MANAGEMENT_TLS_BAD_STATE;
    }

    if (!appliance_management_tls_append_text(
            rendered, &offset, "management_tls v=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->version) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " state=") ||
        !appliance_management_tls_append_text(
            rendered, &offset,
            appliance_management_tls_state_string(snapshot->state)) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " protocol=") ||
        !appliance_management_tls_append_text(
            rendered, &offset,
            appliance_management_tls_protocol_string(
                snapshot->protocol)) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " cipher=") ||
        !appliance_management_tls_append_text(
            rendered, &offset,
            appliance_management_tls_cipher_string(snapshot->cipher)) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " certificate=") ||
        !appliance_management_tls_append_text(
            rendered, &offset,
            snapshot->certificate_ready ? "READY" : "UNAVAILABLE") ||
        !appliance_management_tls_append_text(
            rendered, &offset, " staged=") ||
        !appliance_management_tls_append_text(
            rendered, &offset,
            snapshot->staged_pending ? "PENDING" : "NONE") ||
        !appliance_management_tls_append_text(
            rendered, &offset, " request=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->request_count) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " import=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->import_count) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " promote=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->promote_count) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " rotate=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->rotation_count) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " reject=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, snapshot->reject_count) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " hsts=max-age=") ||
        !appliance_management_tls_append_u64(
            rendered, &offset, APPLIANCE_MANAGEMENT_TLS_HSTS_MAX_AGE) ||
        !appliance_management_tls_append_text(
            rendered, &offset, " secrets=NONE\n")) {
        return APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL;
    }
    if (capacity <= offset) {
        return APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < offset; ++index) {
        output[index] = rendered[index];
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_MANAGEMENT_TLS_OK;
}

enum appliance_management_tls_state appliance_management_tls_state(
    const struct appliance_management_tls *tls)
{
    if (tls == NULL || !tls->initialized ||
        tls->version != APPLIANCE_MANAGEMENT_TLS_VERSION ||
        !appliance_management_tls_state_valid(tls->state)) {
        return APPLIANCE_MANAGEMENT_TLS_STATE_NONE;
    }
    return tls->state;
}

const char *appliance_management_tls_state_string(
    enum appliance_management_tls_state state)
{
    switch (state) {
    case APPLIANCE_MANAGEMENT_TLS_STATE_NONE:
        return "NONE";
    case APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED:
        return "UNCONFIGURED";
    case APPLIANCE_MANAGEMENT_TLS_STATE_AWAITING_CERTIFICATE:
        return "AWAITING_CERTIFICATE";
    case APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE:
        return "ACTIVE";
    case APPLIANCE_MANAGEMENT_TLS_STATE_STAGED:
        return "STAGED";
    default:
        return NULL;
    }
}

const char *appliance_management_tls_status_string(
    enum appliance_management_tls_status status)
{
    switch (status) {
    case APPLIANCE_MANAGEMENT_TLS_OK:
        return "OK";
    case APPLIANCE_MANAGEMENT_TLS_NULL:
        return "NULL";
    case APPLIANCE_MANAGEMENT_TLS_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_MANAGEMENT_TLS_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_MANAGEMENT_TLS_BAD_POLICY:
        return "BAD_POLICY";
    case APPLIANCE_MANAGEMENT_TLS_BAD_IDENTITY:
        return "BAD_IDENTITY";
    case APPLIANCE_MANAGEMENT_TLS_BAD_INGRESS:
        return "BAD_INGRESS";
    case APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER:
        return "DENIED_OUTER";
    case APPLIANCE_MANAGEMENT_TLS_DENIED_PEER:
        return "DENIED_PEER";
    case APPLIANCE_MANAGEMENT_TLS_DENIED_UNCONFIGURED:
        return "DENIED_UNCONFIGURED";
    case APPLIANCE_MANAGEMENT_TLS_NO_PENDING_REQUEST:
        return "NO_PENDING_REQUEST";
    case APPLIANCE_MANAGEMENT_TLS_REQUEST_PENDING:
        return "REQUEST_PENDING";
    case APPLIANCE_MANAGEMENT_TLS_BAD_CERTIFICATE:
        return "BAD_CERTIFICATE";
    case APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_MISMATCH:
        return "CERTIFICATE_MISMATCH";
    case APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID:
        return "CERTIFICATE_NOT_YET_VALID";
    case APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_EXPIRED:
        return "CERTIFICATE_EXPIRED";
    case APPLIANCE_MANAGEMENT_TLS_WEAK_CERTIFICATE_KEY:
        return "WEAK_CERTIFICATE_KEY";
    case APPLIANCE_MANAGEMENT_TLS_BAD_CLOCK:
        return "BAD_CLOCK";
    case APPLIANCE_MANAGEMENT_TLS_NOT_STAGED:
        return "NOT_STAGED";
    case APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE:
        return "SAME_CERTIFICATE";
    case APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_management_tls_policy_verdict_string(
    enum appliance_management_tls_policy_verdict verdict)
{
    switch (verdict) {
    case APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED:
        return "ACCEPTED";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL:
        return "UNKNOWN_PROTOCOL";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL:
        return "WEAK_PROTOCOL";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_CIPHER:
        return "UNKNOWN_CIPHER";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER:
        return "WEAK_CIPHER";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_KEY_EXCHANGE:
        return "UNKNOWN_KEY_EXCHANGE";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE:
        return "WEAK_KEY_EXCHANGE";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_SIGNATURE:
        return "UNKNOWN_SIGNATURE";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE:
        return "WEAK_SIGNATURE";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_COMPRESSION_ENABLED:
        return "COMPRESSION_ENABLED";
    case APPLIANCE_MANAGEMENT_TLS_POLICY_RENEGOTIATION_ENABLED:
        return "RENEGOTIATION_ENABLED";
    default:
        return NULL;
    }
}

const char *appliance_management_tls_protocol_string(
    enum appliance_management_tls_protocol protocol)
{
    switch (protocol) {
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_NONE:
        return "NONE";
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_SSL3:
        return "SSL3";
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS10:
        return "TLS10";
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS11:
        return "TLS11";
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS12:
        return "TLS12";
    case APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13:
        return "TLS13";
    default:
        return NULL;
    }
}

const char *appliance_management_tls_cipher_string(
    enum appliance_management_tls_cipher cipher)
{
    switch (cipher) {
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_NONE:
        return "NONE";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_NULL:
        return "NULL";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_RC4_128_SHA:
        return "RC4_128_SHA";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_3DES_EDE_CBC_SHA:
        return "3DES_EDE_CBC_SHA";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_CBC_SHA:
        return "AES_128_CBC_SHA";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_256_CBC_SHA256:
        return "AES_256_CBC_SHA256";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256:
        return "AES_128_GCM_SHA256";
    case APPLIANCE_MANAGEMENT_TLS_CIPHER_CHACHA20_POLY1305_SHA256:
        return "CHACHA20_POLY1305_SHA256";
    default:
        return NULL;
    }
}

const char *appliance_management_tls_ingress_kind_string(
    enum appliance_management_tls_ingress_kind kind)
{
    switch (kind) {
    case APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL:
        return "LOCAL";
    case APPLIANCE_MANAGEMENT_TLS_INGRESS_NETWORK:
        return "NETWORK";
    default:
        return NULL;
    }
}
