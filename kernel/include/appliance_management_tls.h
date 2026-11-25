#ifndef HFOC_APPLIANCE_MANAGEMENT_TLS_H
#define HFOC_APPLIANCE_MANAGEMENT_TLS_H

#include "appliance_management_bind.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-375 management TLS lifecycle and key rotation.
 *
 * The management listener terminates TLS with the frozen mbedTLS profile
 * (server TLS 1.3, TLS_AES_128_GCM_SHA256, ephemeral P-256 ECDHE/ECDSA, DER
 * X.509).  This module owns the policy and the certificate lifecycle above
 * that profile:
 *
 *   - a machine-checked policy that refuses every legacy protocol, non-AEAD
 *     cipher, static key exchange, weak signature and compression or
 *     renegotiation, so "weak ciphers refused" is a decision, not a comment;
 *   - a bounded certificate request/import contract that validates a
 *     certificate against the pending request, the policy and the
 *     synchronized clock before it may serve;
 *   - rotation without downtime: a validated certificate is staged first, the
 *     active certificate keeps serving and stays ready through the staging
 *     window, and only an explicit promotion swaps the active slot;
 *   - the ITEM-374 management ingress binding, so a network certificate
 *     operation is admitted only when the binding admits it while the local
 *     console always works.
 *
 * The module is allocation-free and metadata-only: it never accepts, copies,
 * stores or renders a private key or a certificate payload, and the rendered
 * record carries fingerprints and the fixed "secrets=NONE" marker only.  The
 * DER bytes stay with the caller and the mbedTLS manifest boundary.
 */

#define APPLIANCE_MANAGEMENT_TLS_VERSION 1U
#define APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX 64U
#define APPLIANCE_MANAGEMENT_TLS_ISSUER_MAX 96U
#define APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH 32U
#define APPLIANCE_MANAGEMENT_TLS_RENDER_MAX 256U

/* The frozen V1 HSTS lifetime published by the management listener. */
#define APPLIANCE_MANAGEMENT_TLS_HSTS_MAX_AGE 31536000U
#define APPLIANCE_MANAGEMENT_TLS_HSTS \
    "Strict-Transport-Security: max-age=31536000"

/* Certificate key sizes the management listener accepts. */
#define APPLIANCE_MANAGEMENT_TLS_EC_P256_BITS 256U
#define APPLIANCE_MANAGEMENT_TLS_RSA_MIN_BITS 2048U

enum appliance_management_tls_protocol {
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_SSL3 = 1,
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS10 = 2,
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS11 = 3,
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS12 = 4,
    APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13 = 5,
};

enum appliance_management_tls_cipher {
    APPLIANCE_MANAGEMENT_TLS_CIPHER_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_NULL = 1,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_RC4_128_SHA = 2,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_3DES_EDE_CBC_SHA = 3,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_CBC_SHA = 4,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_256_CBC_SHA256 = 5,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256 = 6,
    APPLIANCE_MANAGEMENT_TLS_CIPHER_CHACHA20_POLY1305_SHA256 = 7,
};

enum appliance_management_tls_key_exchange {
    APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_NULL = 1,
    APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_RSA = 2,
    APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_DHE_1024 = 3,
    APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_ECDHE_P256 = 4,
};

enum appliance_management_tls_signature {
    APPLIANCE_MANAGEMENT_TLS_SIGNATURE_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_MD5 = 1,
    APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_SHA1 = 2,
    APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_PKCS1_SHA256 = 3,
    APPLIANCE_MANAGEMENT_TLS_SIGNATURE_ECDSA_P256_SHA256 = 4,
};

enum appliance_management_tls_certificate_key {
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256 = 1,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_RSA_1024 = 2,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_RSA_2048 = 3,
};

enum appliance_management_tls_policy_verdict {
    APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED = 0,
    APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL = 1,
    APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL = 2,
    APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_CIPHER = 3,
    APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER = 4,
    APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_KEY_EXCHANGE = 5,
    APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE = 6,
    APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_SIGNATURE = 7,
    APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE = 8,
    APPLIANCE_MANAGEMENT_TLS_POLICY_COMPRESSION_ENABLED = 9,
    APPLIANCE_MANAGEMENT_TLS_POLICY_RENEGOTIATION_ENABLED = 10,
};

enum appliance_management_tls_state {
    APPLIANCE_MANAGEMENT_TLS_STATE_NONE = 0,
    APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED = 1,
    APPLIANCE_MANAGEMENT_TLS_STATE_AWAITING_CERTIFICATE = 2,
    APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE = 3,
    APPLIANCE_MANAGEMENT_TLS_STATE_STAGED = 4,
};

enum appliance_management_tls_status {
    APPLIANCE_MANAGEMENT_TLS_OK = 0,
    APPLIANCE_MANAGEMENT_TLS_NULL = 1,
    APPLIANCE_MANAGEMENT_TLS_BAD_STATE = 2,
    APPLIANCE_MANAGEMENT_TLS_BAD_VERSION = 3,
    APPLIANCE_MANAGEMENT_TLS_BAD_POLICY = 4,
    APPLIANCE_MANAGEMENT_TLS_BAD_IDENTITY = 5,
    APPLIANCE_MANAGEMENT_TLS_BAD_INGRESS = 6,
    APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER = 7,
    APPLIANCE_MANAGEMENT_TLS_DENIED_PEER = 8,
    APPLIANCE_MANAGEMENT_TLS_DENIED_UNCONFIGURED = 9,
    APPLIANCE_MANAGEMENT_TLS_NO_PENDING_REQUEST = 10,
    APPLIANCE_MANAGEMENT_TLS_REQUEST_PENDING = 11,
    APPLIANCE_MANAGEMENT_TLS_BAD_CERTIFICATE = 12,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_MISMATCH = 13,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID = 14,
    APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_EXPIRED = 15,
    APPLIANCE_MANAGEMENT_TLS_WEAK_CERTIFICATE_KEY = 16,
    APPLIANCE_MANAGEMENT_TLS_BAD_CLOCK = 17,
    APPLIANCE_MANAGEMENT_TLS_NOT_STAGED = 18,
    APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE = 19,
    APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL = 20,
};

enum appliance_management_tls_ingress_kind {
    APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL = 0,
    APPLIANCE_MANAGEMENT_TLS_INGRESS_NETWORK = 1,
};

struct appliance_management_tls_ingress {
    uint32_t version;
    enum appliance_management_tls_ingress_kind kind;
    bool management_interface;
    uint8_t address[APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH];
};

struct appliance_management_tls_parameters {
    uint32_t version;
    enum appliance_management_tls_protocol protocol;
    enum appliance_management_tls_cipher cipher;
    enum appliance_management_tls_key_exchange key_exchange;
    enum appliance_management_tls_signature signature;
    bool compression_enabled;
    bool renegotiation_enabled;
};

struct appliance_management_tls_identity {
    uint32_t version;
    char common_name[APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX + 1U];
    uint8_t common_name_length;
    char subject_alt_name[APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX + 1U];
    uint8_t subject_alt_name_length;
};

struct appliance_management_tls_certificate {
    uint32_t version;
    bool present;
    char common_name[APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX + 1U];
    uint8_t common_name_length;
    char subject_alt_name[APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX + 1U];
    uint8_t subject_alt_name_length;
    char issuer[APPLIANCE_MANAGEMENT_TLS_ISSUER_MAX + 1U];
    uint8_t issuer_length;
    enum appliance_management_tls_certificate_key key;
    uint32_t not_before_epoch;
    uint32_t not_after_epoch;
    uint8_t fingerprint[APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH];
};

struct appliance_management_tls_request {
    uint32_t version;
    bool pending;
    uint32_t request_id;
    struct appliance_management_tls_identity identity;
    uint32_t policy_generation;
    uint32_t created_at_epoch;
};

struct appliance_management_tls {
    bool initialized;
    uint32_t version;
    enum appliance_management_tls_state state;
    enum appliance_management_tls_status last_status;
    struct appliance_management_tls_parameters policy;
    struct appliance_management_tls_request request;
    struct appliance_management_tls_certificate active;
    struct appliance_management_tls_certificate staged;
    uint32_t request_count;
    uint32_t import_count;
    uint32_t promote_count;
    uint32_t reject_count;
    uint32_t rotation_count;
    bool secrets_logged;
};

struct appliance_management_tls_snapshot {
    bool published;
    uint32_t version;
    enum appliance_management_tls_state state;
    enum appliance_management_tls_status last_status;
    enum appliance_management_tls_protocol protocol;
    enum appliance_management_tls_cipher cipher;
    bool certificate_ready;
    bool request_pending;
    bool staged_pending;
    bool key_published;
    uint32_t request_count;
    uint32_t import_count;
    uint32_t promote_count;
    uint32_t reject_count;
    uint32_t rotation_count;
    bool secrets_logged;
};

enum appliance_management_tls_policy_verdict
appliance_management_tls_policy_check(
    const struct appliance_management_tls_parameters *parameters);
const struct appliance_management_tls_parameters *
appliance_management_tls_policy_frozen(void);
enum appliance_management_tls_status appliance_management_tls_init(
    struct appliance_management_tls *tls);
enum appliance_management_tls_status
appliance_management_tls_request_certificate(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    const char *common_name,
    size_t common_name_length,
    const char *subject_alt_name,
    size_t subject_alt_name_length,
    uint32_t now_epoch_seconds);
enum appliance_management_tls_status
appliance_management_tls_import_certificate(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    const struct appliance_management_tls_certificate *certificate,
    uint32_t now_epoch_seconds);
enum appliance_management_tls_status appliance_management_tls_promote(
    struct appliance_management_tls *tls,
    const struct appliance_management_bind *bind,
    const struct appliance_management_tls_ingress *ingress,
    uint32_t now_epoch_seconds);
enum appliance_management_tls_status
appliance_management_tls_discard_staged(
    struct appliance_management_tls *tls);
enum appliance_management_tls_status
appliance_management_tls_readiness(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds,
    bool *ready_out);
enum appliance_management_tls_status appliance_management_tls_snapshot(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds,
    struct appliance_management_tls_snapshot *snapshot_out);
enum appliance_management_tls_status appliance_management_tls_render(
    const struct appliance_management_tls_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length);
enum appliance_management_tls_state appliance_management_tls_state(
    const struct appliance_management_tls *tls);
bool appliance_management_tls_certificate_ready(
    const struct appliance_management_tls *tls,
    uint32_t now_epoch_seconds);
const char *appliance_management_tls_state_string(
    enum appliance_management_tls_state state);
const char *appliance_management_tls_status_string(
    enum appliance_management_tls_status status);
const char *appliance_management_tls_policy_verdict_string(
    enum appliance_management_tls_policy_verdict verdict);
const char *appliance_management_tls_protocol_string(
    enum appliance_management_tls_protocol protocol);
const char *appliance_management_tls_cipher_string(
    enum appliance_management_tls_cipher cipher);
const char *appliance_management_tls_ingress_kind_string(
    enum appliance_management_tls_ingress_kind kind);

#endif
