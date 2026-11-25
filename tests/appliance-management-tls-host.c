/*
 * ITEM-375 focused host proof: the management TLS policy, the certificate
 * request/import contract, rotation without downtime, the fail-closed
 * management ingress gate and the HSTS publication.
 */

#ifdef HFOC_APPLIANCE_MANAGEMENT_TLS_HOST_TEST

#include "appliance_management_bind.h"
#include "appliance_management_tls.h"
#include "http_response.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;
static int checks;

#define CHECK(condition, message)                                        \
    do {                                                                 \
        ++checks;                                                        \
        if (!(condition)) {                                              \
            ++failures;                                                  \
            fprintf(stderr, "FAIL: %s\n", (message));                    \
        }                                                                \
    } while (0)

#define FINGERPRINT_A 0xA1U
#define FINGERPRINT_B 0xB2U
#define FINGERPRINT_C 0xC3U

static const uint32_t host_now = 1000000U;
static const uint32_t host_valid_from = 900000U;
static const uint32_t host_valid_to = 2000000U;

static void host_fill_fingerprint(
    uint8_t fingerprint[APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH],
    uint8_t value)
{
    size_t index;

    for (index = 0U;
         index < APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH;
         ++index) {
        fingerprint[index] = (uint8_t)(value + (uint8_t)index);
    }
}

static struct appliance_management_tls_parameters host_frozen_policy(void)
{
    const struct appliance_management_tls_parameters *frozen =
        appliance_management_tls_policy_frozen();
    struct appliance_management_tls_parameters parameters;

    memset(&parameters, 0, sizeof(parameters));
    if (frozen != NULL) {
        parameters = *frozen;
    }
    return parameters;
}

static struct appliance_management_tls_certificate host_certificate(
    const char *common_name,
    const char *subject_alt_name,
    uint8_t fingerprint_value,
    enum appliance_management_tls_certificate_key key,
    uint32_t not_before_epoch,
    uint32_t not_after_epoch)
{
    struct appliance_management_tls_certificate certificate;

    memset(&certificate, 0, sizeof(certificate));
    certificate.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    certificate.present = true;
    (void)snprintf(
        certificate.common_name, sizeof(certificate.common_name), "%s",
        common_name);
    certificate.common_name_length = (uint8_t)strlen(common_name);
    (void)snprintf(
        certificate.subject_alt_name,
        sizeof(certificate.subject_alt_name), "%s", subject_alt_name);
    certificate.subject_alt_name_length =
        (uint8_t)strlen(subject_alt_name);
    (void)snprintf(
        certificate.issuer, sizeof(certificate.issuer), "%s",
        "HFOC-APPLIANCE-V1-CA");
    certificate.issuer_length = (uint8_t)strlen(certificate.issuer);
    certificate.key = key;
    certificate.not_before_epoch = not_before_epoch;
    certificate.not_after_epoch = not_after_epoch;
    host_fill_fingerprint(certificate.fingerprint, fingerprint_value);
    return certificate;
}

static struct appliance_management_tls_ingress host_local_ingress(void)
{
    struct appliance_management_tls_ingress ingress;

    memset(&ingress, 0, sizeof(ingress));
    ingress.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    ingress.kind = APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL;
    return ingress;
}

static struct appliance_management_tls_ingress host_network_ingress(
    bool management_interface,
    uint8_t last_octet)
{
    struct appliance_management_tls_ingress ingress;

    memset(&ingress, 0, sizeof(ingress));
    ingress.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    ingress.kind = APPLIANCE_MANAGEMENT_TLS_INGRESS_NETWORK;
    ingress.management_interface = management_interface;
    ingress.address[0] = 192U;
    ingress.address[1] = 168U;
    ingress.address[2] = 1U;
    ingress.address[3] = last_octet;
    return ingress;
}

static struct appliance_management_bind host_bind(
    enum appliance_management_bind_scope scope)
{
    struct appliance_management_bind bind;
    const uint8_t peer[APPLIANCE_MANAGEMENT_BIND_ADDRESS_LENGTH] = {
        192U, 168U, 1U, 10U,
    };

    memset(&bind, 0, sizeof(bind));
    switch (scope) {
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY:
        CHECK(appliance_management_bind_publish_local_only(&bind) ==
                  APPLIANCE_MANAGEMENT_BIND_OK,
              "local-only binding publishes");
        break;
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE:
        CHECK(appliance_management_bind_publish_management_interface(
                  &bind, "eth0", 4U, &peer, 1U) ==
                  APPLIANCE_MANAGEMENT_BIND_OK,
              "management-interface binding publishes");
        break;
    case APPLIANCE_MANAGEMENT_BIND_SCOPE_OUTER_INTERFACE:
        CHECK(appliance_management_bind_publish_outer_interface(&bind) ==
                  APPLIANCE_MANAGEMENT_BIND_OK,
              "outer-interface binding publishes");
        break;
    default:
        break;
    }
    return bind;
}

/* 1. The frozen policy accepts exactly one profile and refuses the rest. */
static void host_policy_refuses_weak_suites(void)
{
    struct appliance_management_tls_parameters parameters =
        host_frozen_policy();

    CHECK(appliance_management_tls_policy_frozen() != NULL,
          "the frozen policy is published");
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED,
          "the frozen profile is accepted by its own policy");
    CHECK(parameters.protocol == APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13 &&
              parameters.cipher ==
                  APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256 &&
              parameters.key_exchange ==
                  APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_ECDHE_P256 &&
              parameters.signature ==
                  APPLIANCE_MANAGEMENT_TLS_SIGNATURE_ECDSA_P256_SHA256,
          "the frozen profile is TLS 1.3 with AES-128-GCM and P-256");

    parameters = host_frozen_policy();
    parameters.protocol = APPLIANCE_MANAGEMENT_TLS_PROTOCOL_SSL3;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL,
          "SSL 3.0 is refused as a weak protocol");
    parameters.protocol = APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS10;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL,
          "TLS 1.0 is refused as a weak protocol");
    parameters.protocol = APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS11;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL,
          "TLS 1.1 is refused as a weak protocol");
    parameters.protocol = APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS12;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_PROTOCOL,
          "TLS 1.2 is refused outside the frozen profile");
    parameters.protocol = APPLIANCE_MANAGEMENT_TLS_PROTOCOL_NONE;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL,
          "an absent protocol is refused as unknown");

    parameters = host_frozen_policy();
    parameters.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_NULL;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "the NULL cipher is refused");
    parameters.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_RC4_128_SHA;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "RC4 is refused");
    parameters.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_3DES_EDE_CBC_SHA;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "3DES is refused");
    parameters.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_CBC_SHA;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "AES-CBC-SHA1 is refused as a non-AEAD cipher");
    parameters.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_256_CBC_SHA256;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "AES-CBC-SHA256 is refused outside the frozen profile");
    parameters.cipher =
        APPLIANCE_MANAGEMENT_TLS_CIPHER_CHACHA20_POLY1305_SHA256;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER,
          "an AEAD cipher outside the frozen profile is refused");

    parameters = host_frozen_policy();
    parameters.key_exchange = APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_NULL;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE,
          "a null key exchange is refused");
    parameters.key_exchange = APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_RSA;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE,
          "static RSA key exchange is refused");
    parameters.key_exchange = APPLIANCE_MANAGEMENT_TLS_KEY_EXCHANGE_DHE_1024;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_KEY_EXCHANGE,
          "1024-bit DHE is refused");

    parameters = host_frozen_policy();
    parameters.signature = APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_MD5;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE,
          "RSA-MD5 is refused");
    parameters.signature = APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_SHA1;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE,
          "RSA-SHA1 is refused");
    parameters.signature =
        APPLIANCE_MANAGEMENT_TLS_SIGNATURE_RSA_PKCS1_SHA256;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_SIGNATURE,
          "RSA-PKCS1-SHA256 is refused outside the frozen profile");

    parameters = host_frozen_policy();
    parameters.compression_enabled = true;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_COMPRESSION_ENABLED,
          "TLS compression is refused");
    parameters = host_frozen_policy();
    parameters.renegotiation_enabled = true;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_RENEGOTIATION_ENABLED,
          "renegotiation is refused");
    CHECK(appliance_management_tls_policy_check(NULL) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL,
          "a null policy request is refused");

    parameters = host_frozen_policy();
    parameters.version = APPLIANCE_MANAGEMENT_TLS_VERSION + 1U;
    CHECK(appliance_management_tls_policy_check(&parameters) ==
              APPLIANCE_MANAGEMENT_TLS_POLICY_UNKNOWN_PROTOCOL,
          "an unknown policy version is refused");

    CHECK(strcmp(appliance_management_tls_policy_verdict_string(
                     APPLIANCE_MANAGEMENT_TLS_POLICY_ACCEPTED),
                 "ACCEPTED") == 0 &&
              strcmp(appliance_management_tls_policy_verdict_string(
                         APPLIANCE_MANAGEMENT_TLS_POLICY_WEAK_CIPHER),
                     "WEAK_CIPHER") == 0,
          "policy verdict strings are exact");
    CHECK(strcmp(appliance_management_tls_protocol_string(
                     APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13),
                 "TLS13") == 0 &&
              strcmp(appliance_management_tls_cipher_string(
                         APPLIANCE_MANAGEMENT_TLS_CIPHER_NULL),
                     "NULL") == 0,
          "protocol and cipher strings are exact");
}

/* 2. Request, import, activation and refusal. */
static void host_request_import_and_readiness(void)
{
    struct appliance_management_tls tls;
    struct appliance_management_tls_ingress local = host_local_ingress();
    struct appliance_management_tls_certificate certificate;
    struct appliance_management_tls_snapshot snapshot;
    bool ready = true;

    CHECK(appliance_management_tls_init(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the lifecycle initializes");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED,
          "an initialized lifecycle is unconfigured");
    CHECK(!appliance_management_tls_certificate_ready(&tls, host_now),
          "an unconfigured lifecycle is not certificate ready");

    /* An import without a pending request is refused. */
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_NO_PENDING_REQUEST,
          "an import without a pending request is refused");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_UNCONFIGURED,
          "a refused import leaves the lifecycle unconfigured");
    CHECK(tls.reject_count == 1U,
          "a refused import is counted as a rejection");

    /* A request derives the identity from the appliance hostname. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the local console may request a certificate");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_AWAITING_CERTIFICATE &&
              tls.request.pending && tls.request.request_id == 1U &&
              tls.request.policy_generation ==
                  APPLIANCE_MANAGEMENT_TLS_VERSION,
          "the request is pending against the frozen policy generation");
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "other-host", 10U, "other-host", 10U,
              host_now) == APPLIANCE_MANAGEMENT_TLS_REQUEST_PENDING,
          "a second request while one is pending is refused");
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "", 0U, "hfoc-appliance", 14U,
              host_now) == APPLIANCE_MANAGEMENT_TLS_BAD_IDENTITY,
          "an empty identity is refused");

    /* A certificate that does not answer the pending request is refused. */
    certificate = host_certificate(
        "other-host", "other-host",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_MISMATCH,
          "a certificate for another name is refused");

    /* An expired or not-yet-valid certificate is refused. */
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_now - 1U);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_EXPIRED,
          "an expired certificate is refused");
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_now + 1U, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID,
          "a not-yet-valid certificate is refused");
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_to, host_valid_from);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_BAD_CERTIFICATE,
          "an inverted validity window is refused");

    /* A weak certificate key is refused by the policy. */
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_RSA_1024,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_WEAK_CERTIFICATE_KEY,
          "a 1024-bit RSA certificate key is refused");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_AWAITING_CERTIFICATE,
          "every refusal leaves the lifecycle awaiting the certificate");

    /* A valid certificate activates the listener. */
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "a valid certificate is imported");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE,
          "an imported certificate activates the listener");
    CHECK(!tls.request.pending && tls.import_count == 1U &&
              tls.reject_count == 8U,
          "a successful import consumes the request and counts exactly");

    ready = false;
    CHECK(appliance_management_tls_readiness(&tls, host_now, &ready) ==
              APPLIANCE_MANAGEMENT_TLS_OK && ready,
          "an active certificate publishes certificate readiness");
    CHECK(appliance_management_tls_certificate_ready(&tls, host_now),
          "the readiness accessor agrees with the record");
    CHECK(!appliance_management_tls_certificate_ready(
              &tls, host_valid_to),
          "readiness ends when the certificate expires");
    CHECK(!appliance_management_tls_certificate_ready(&tls, 0U),
          "readiness never publishes against an unvalidated clock");
    CHECK(appliance_management_tls_readiness(&tls, host_now, NULL) ==
              APPLIANCE_MANAGEMENT_TLS_NULL,
          "readiness refuses a null output");

    /* An unsynchronized clock never validates a certificate. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "a rotation request is accepted before the clock check");
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, 0U) ==
              APPLIANCE_MANAGEMENT_TLS_BAD_CLOCK,
          "an import against an unsynchronized clock is refused");
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE,
          "the refused import left the pending request intact");
    CHECK(appliance_management_tls_discard_staged(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_NOT_STAGED,
          "an unsynchronized import staged nothing");

    /* A corrupted record is refused instead of trusted. */
    tls.version = APPLIANCE_MANAGEMENT_TLS_VERSION + 1U;
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_BAD_VERSION,
          "a record with an unknown version is refused");
    tls.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    tls.policy.cipher = APPLIANCE_MANAGEMENT_TLS_CIPHER_RC4_128_SHA;
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_BAD_POLICY,
          "a record whose policy weakened is refused");
    tls.policy = host_frozen_policy();

    CHECK(appliance_management_tls_snapshot(
              &tls, host_now, &snapshot) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the lifecycle snapshots");
    CHECK(snapshot.published && snapshot.certificate_ready &&
              snapshot.state == APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
              snapshot.protocol ==
                  APPLIANCE_MANAGEMENT_TLS_PROTOCOL_TLS13 &&
              snapshot.cipher ==
                  APPLIANCE_MANAGEMENT_TLS_CIPHER_AES_128_GCM_SHA256 &&
              snapshot.key_published && !snapshot.staged_pending &&
              snapshot.secrets_logged,
          "the snapshot carries the frozen profile and honest readiness");
}

/* 3. Rotation without downtime. */
static void host_rotation_without_downtime(void)
{
    struct appliance_management_tls tls;
    struct appliance_management_tls_ingress local = host_local_ingress();
    struct appliance_management_tls_certificate first;
    struct appliance_management_tls_certificate second;
    struct appliance_management_tls_certificate noop;
    uint8_t active_fingerprint[
        APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH];
    uint8_t staged_fingerprint[
        APPLIANCE_MANAGEMENT_TLS_FINGERPRINT_LENGTH];

    CHECK(appliance_management_tls_init(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the rotation fixture initializes");
    first = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK &&
              appliance_management_tls_import_certificate(
                  &tls, NULL, &local, &first, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the first certificate becomes active");
    memcpy(active_fingerprint, tls.active.fingerprint,
           sizeof(active_fingerprint));

    CHECK(appliance_management_tls_promote(
              &tls, NULL, &local, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_NOT_STAGED,
          "promotion without a staged certificate is refused");

    /* A rotation request must not disturb the serving certificate. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now + 1U) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the active listener accepts a rotation request");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
              appliance_management_tls_certificate_ready(&tls, host_now) &&
              memcmp(tls.active.fingerprint, active_fingerprint,
                     sizeof(active_fingerprint)) == 0,
          "a rotation request keeps the active certificate serving");

    /* Same certificate is not a rotation. */
    noop = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &noop, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE,
          "re-importing the serving certificate is refused");

    /* A different certificate is staged, not promoted. */
    second = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_B, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from + 1U, host_valid_to + 1U);
    memcpy(staged_fingerprint, second.fingerprint,
           sizeof(staged_fingerprint));
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &second, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the rotation certificate is staged");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_STAGED &&
              tls.staged.present && tls.active.present &&
              memcmp(tls.active.fingerprint, active_fingerprint,
                     sizeof(active_fingerprint)) == 0 &&
              memcmp(tls.staged.fingerprint, staged_fingerprint,
                     sizeof(staged_fingerprint)) == 0,
          "staging keeps the old certificate active");
    CHECK(appliance_management_tls_certificate_ready(&tls, host_now),
          "the listener stays ready while the rotation is staged");

    /* A staged certificate that expires before promotion is dropped. */
    CHECK(appliance_management_tls_promote(
              &tls, NULL, &local, host_valid_to + 2U) ==
              APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_EXPIRED,
          "a staged certificate that expired before promotion is refused");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
              !tls.staged.present &&
              memcmp(tls.active.fingerprint, active_fingerprint,
                     sizeof(active_fingerprint)) == 0 &&
              appliance_management_tls_certificate_ready(&tls, host_now),
          "a refused promotion leaves the old certificate serving");
    CHECK(tls.request_count == 2U,
          "a refused promotion does not fabricate a new request");

    /* A staged certificate that is not yet valid stays staged. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the operator requests the rotation again");
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &second, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK &&
              appliance_management_tls_promote(
                  &tls, NULL, &local, host_valid_from) ==
              APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_NOT_YET_VALID,
          "a staged certificate that is not yet valid is refused");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_STAGED &&
              tls.staged.present &&
              memcmp(tls.active.fingerprint, active_fingerprint,
                     sizeof(active_fingerprint)) == 0 &&
              appliance_management_tls_certificate_ready(&tls, host_now),
          "a not-yet-valid promotion keeps the rotation staged and the "
          "active certificate serving");

    CHECK(appliance_management_tls_promote(
              &tls, NULL, &local, host_now + 2U) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the staged certificate promotes");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
              !tls.staged.present &&
              memcmp(tls.active.fingerprint, staged_fingerprint,
                     sizeof(staged_fingerprint)) == 0 &&
              appliance_management_tls_certificate_ready(&tls, host_now),
          "the promoted certificate serves without an interruption");
    CHECK(tls.rotation_count == 1U && tls.promote_count == 1U &&
              tls.import_count == 3U,
          "rotation, promotion and import are counted exactly");

    /* Discarding a staged rotation keeps the serving certificate. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "a second rotation request is accepted");
    second = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_C, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &second, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK &&
              appliance_management_tls_state(&tls) ==
                  APPLIANCE_MANAGEMENT_TLS_STATE_STAGED,
          "a second staged rotation is published");
    CHECK(appliance_management_tls_discard_staged(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "a staged rotation is discarded");
    CHECK(appliance_management_tls_state(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_STATE_ACTIVE &&
              !tls.staged.present && !tls.request.pending &&
              memcmp(tls.active.fingerprint, staged_fingerprint,
                     sizeof(staged_fingerprint)) == 0 &&
              appliance_management_tls_certificate_ready(&tls, host_now),
          "discarding a staged rotation keeps the active certificate");
    CHECK(appliance_management_tls_discard_staged(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_NOT_STAGED,
          "discarding without a staged certificate is refused");
    CHECK(tls.rotation_count == 1U,
          "a discarded rotation does not increment the rotation count");
}

/* 4. The ITEM-374 ingress binding gates every network operation. */
static void host_ingress_binding(void)
{
    struct appliance_management_tls tls;
    struct appliance_management_tls_ingress local = host_local_ingress();
    struct appliance_management_tls_ingress network =
        host_network_ingress(true, 10U);
    struct appliance_management_tls_ingress outer =
        host_network_ingress(false, 10U);
    struct appliance_management_tls_ingress unknown_peer =
        host_network_ingress(true, 77U);
    struct appliance_management_bind local_only =
        host_bind(APPLIANCE_MANAGEMENT_BIND_SCOPE_LOCAL_ONLY);
    struct appliance_management_bind management =
        host_bind(APPLIANCE_MANAGEMENT_BIND_SCOPE_MANAGEMENT_INTERFACE);
    struct appliance_management_tls_certificate certificate;

    CHECK(appliance_management_tls_init(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the ingress fixture initializes");
    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);

    CHECK(appliance_management_tls_request_certificate(
              &tls, &local_only, &network, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER,
          "a local-only binding denies a network certificate request");
    CHECK(appliance_management_tls_request_certificate(
              &tls, &management, &outer, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER,
          "the management binding denies the outer interface");
    CHECK(appliance_management_tls_request_certificate(
              &tls, &management, &unknown_peer, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_PEER,
          "the management binding denies an unknown peer");
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &network, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_UNCONFIGURED,
          "an unpublished binding denies a network request");
    CHECK(!tls.request.pending && tls.request_count == 0U &&
              tls.reject_count == 4U,
          "a denied request never becomes pending");

    CHECK(appliance_management_tls_request_certificate(
              &tls, &management, &network, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the management interface may request a certificate");
    CHECK(appliance_management_tls_import_certificate(
              &tls, &management, &outer, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER,
          "an import from the outer interface is denied");
    CHECK(appliance_management_tls_import_certificate(
              &tls, &management, &network, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "an import from the management interface is admitted");
    CHECK(appliance_management_tls_promote(
              &tls, &local_only, &network, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER,
          "promotion from a denied interface is refused");

    /* The local console always works, with or without a binding. */
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the local console works without any binding");
    CHECK(appliance_management_tls_import_certificate(
              &tls, NULL, &local, &certificate, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_SAME_CERTIFICATE,
          "the local console import reaches the rotation policy");
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, NULL, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_NULL,
          "a null ingress is refused");
    CHECK(appliance_management_tls_init(NULL) ==
              APPLIANCE_MANAGEMENT_TLS_NULL &&
              appliance_management_tls_promote(
                  NULL, NULL, &local, host_now) ==
                  APPLIANCE_MANAGEMENT_TLS_NULL,
          "the lifecycle refuses null objects");
    CHECK(strcmp(appliance_management_tls_ingress_kind_string(
                     APPLIANCE_MANAGEMENT_TLS_INGRESS_NETWORK),
                 "NETWORK") == 0 &&
              strcmp(appliance_management_tls_state_string(
                         APPLIANCE_MANAGEMENT_TLS_STATE_STAGED),
                     "STAGED") == 0 &&
              strcmp(appliance_management_tls_status_string(
                         APPLIANCE_MANAGEMENT_TLS_DENIED_OUTER),
                     "DENIED_OUTER") == 0,
          "ingress, state and status strings are exact");
}

/* 5. The rendered record is bounded and never carries secret material. */
static void host_render_record(void)
{
    struct appliance_management_tls tls;
    struct appliance_management_tls_ingress local = host_local_ingress();
    struct appliance_management_tls_certificate certificate;
    struct appliance_management_tls_snapshot snapshot;
    char rendered[APPLIANCE_MANAGEMENT_TLS_RENDER_MAX];
    char small[8];
    size_t length = 0U;

    CHECK(appliance_management_tls_init(&tls) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the render fixture initializes");
    CHECK(appliance_management_tls_snapshot(&tls, host_now, &snapshot) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the unconfigured lifecycle snapshots");
    CHECK(appliance_management_tls_render(
              &snapshot, rendered, sizeof(rendered), &length) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the unconfigured record renders");
    CHECK(strstr(rendered, "management_tls v=1 state=UNCONFIGURED") !=
              NULL &&
              strstr(rendered, "certificate=UNAVAILABLE") != NULL &&
              strstr(rendered, "secrets=NONE") != NULL,
          "the unconfigured record is exact");

    certificate = host_certificate(
        "hfoc-appliance", "hfoc-appliance",
        FINGERPRINT_A, APPLIANCE_MANAGEMENT_TLS_CERTIFICATE_KEY_EC_P256,
        host_valid_from, host_valid_to);
    CHECK(appliance_management_tls_request_certificate(
              &tls, NULL, &local, "hfoc-appliance", 14U,
              "hfoc-appliance", 14U, host_now) ==
              APPLIANCE_MANAGEMENT_TLS_OK &&
              appliance_management_tls_import_certificate(
                  &tls, NULL, &local, &certificate, host_now) ==
                  APPLIANCE_MANAGEMENT_TLS_OK &&
              appliance_management_tls_snapshot(
                  &tls, host_now, &snapshot) ==
                  APPLIANCE_MANAGEMENT_TLS_OK,
          "the active lifecycle snapshots");
    CHECK(appliance_management_tls_render(
              &snapshot, rendered, sizeof(rendered), &length) ==
              APPLIANCE_MANAGEMENT_TLS_OK,
          "the active record renders");
    CHECK(strstr(rendered, "state=ACTIVE") != NULL &&
              strstr(rendered, "certificate=READY") != NULL &&
              strstr(rendered, "protocol=TLS13") != NULL &&
              strstr(rendered, "cipher=AES_128_GCM_SHA256") != NULL &&
              strstr(rendered, "hsts=max-age=31536000") != NULL &&
              strstr(rendered, "secrets=NONE") != NULL,
          "the active record publishes the policy, HSTS and readiness");
    CHECK(strstr(rendered, "PRIVATE") == NULL &&
              strstr(rendered, "BEGIN") == NULL &&
              strstr(rendered, "password") == NULL &&
              strstr(rendered, "hfoc-appliance") == NULL,
          "the record never renders identity or secret material");
    CHECK(length > 0U && memchr(rendered, '\0', length) == NULL &&
              rendered[length] == '\0',
          "the record is a bounded NUL-terminated string");
    CHECK(length > 0U && length < sizeof(rendered),
          "the rendered length is bounded");

    CHECK(appliance_management_tls_render(
              &snapshot, small, sizeof(small), &length) ==
              APPLIANCE_MANAGEMENT_TLS_BUFFER_TOO_SMALL,
          "a short buffer is refused");
    CHECK(appliance_management_tls_render(
              &snapshot, NULL, sizeof(rendered), &length) ==
              APPLIANCE_MANAGEMENT_TLS_NULL,
          "a null buffer is refused");
    snapshot.state = APPLIANCE_MANAGEMENT_TLS_STATE_NONE;
    CHECK(appliance_management_tls_render(
              &snapshot, rendered, sizeof(rendered), &length) ==
              APPLIANCE_MANAGEMENT_TLS_BAD_STATE,
          "an unpublished snapshot is refused");
}

/* 6. The management listener publishes HSTS on every response. */
static void host_hsts_header(void)
{
    struct http_response response;
    uint8_t encoded[HTTP_RESPONSE_BUFFER_MAX];
    uint32_t written = 0U;
    const char *hsts = "Strict-Transport-Security: max-age=31536000\r\n";
    size_t index;

    memset(&response, 0, sizeof(response));
    response.status = HTTP_STATUS_200_OK;
    response.content_type = HTTP_CONTENT_TYPE_JSON;
    response.body = (const uint8_t *)"{}";
    response.body_length = 2U;

    CHECK(http_response_encode(
              &response, encoded, sizeof(encoded), &written) ==
              HTTP_RESPONSE_OK,
          "a management response encodes");
    for (index = 0U; index + strlen(hsts) <= written; ++index) {
        if (memcmp(encoded + index, hsts, strlen(hsts)) == 0) {
            break;
        }
    }
    CHECK(index + strlen(hsts) <= written,
          "the encoded response carries the HSTS header");
    CHECK(http_response_head_length(&response) == written - 2U,
          "the HSTS header is part of the fixed head length");
    CHECK(strcmp(APPLIANCE_MANAGEMENT_TLS_HSTS,
                 "Strict-Transport-Security: max-age=31536000") == 0,
          "the published HSTS constant is exact");
}

int main(void)
{
    host_policy_refuses_weak_suites();
    host_request_import_and_readiness();
    host_rotation_without_downtime();
    host_ingress_binding();
    host_render_record();
    host_hsts_header();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance management TLS host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance management TLS host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_management_tls_host_translation_unit_marker;
#endif
