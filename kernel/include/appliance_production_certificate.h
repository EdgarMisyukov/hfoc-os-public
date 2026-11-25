#ifndef HFOC_APPLIANCE_PRODUCTION_CERTIFICATE_H
#define HFOC_APPLIANCE_PRODUCTION_CERTIFICATE_H

#include "appliance_state_loader.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-386 ordinary-boot management certificate.
 *
 * Before ITEM-386 a freshly installed production appliance had no certificate
 * at all: the only certificate in the tree was the ITEM-270 QEMU test secret,
 * and `scripts/verify-mbedtls-test-secret.sh --production` fails the build if
 * it reaches the production ELF.  The management TLS lifecycle therefore
 * stayed AWAITING_CERTIFICATE forever and the panel was unreachable out of the
 * box.
 *
 * This module gives ordinary boot a real certificate:
 *
 *   1. read the persisted certificate record from the state volume's reserved
 *      CERTIFICATE store (namespace CERTIFICATE, store 1);
 *   2. when the record is absent, generate a per-appliance self-signed P-256
 *      certificate with mbedTLS (P-256 key + X.509 v3 certificate, SHA-256,
 *      CN and SAN from the persisted hostname) and write it back to the same
 *      store, so the identity survives the next boot;
 *   3. hand the DER bytes to the caller, which loads them through the ITEM-375
 *      management TLS lifecycle and serves them from the listener.
 *
 * Honest limits, published rather than hidden:
 *
 *   - the certificate is SELF-SIGNED, so a browser shows a warning until an
 *     operator imports an appliance certificate; the listener says so in its
 *     `[PRODUCTION-SERVING]` record;
 *   - the certificate is per-appliance, generated from the appliance's own
 *     hardware entropy, and never shared between appliances;
 *   - the private key is written to the appliance's own state volume in the
 *     record below and is never rendered, logged, hashed into a marker or
 *     copied outside the store; when the volume is sealed the module refuses
 *     the plaintext write exactly as the durable update record does;
 *   - when the state volume is unavailable the appliance still serves, with a
 *     per-boot generated certificate, and publishes DEGRADED with the reason.
 */

#define APPLIANCE_PRODUCTION_CERTIFICATE_VERSION 1U
#define APPLIANCE_PRODUCTION_CERTIFICATE_MAGIC UINT32_C(0x54524543)

#define APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX 1024U
#define APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX 512U
#define APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX 64U
/*
 * ITEM-503: an operator certificate may carry its issuing chain, so the
 * record grew from two blocks (leaf plus key) to twelve: the v2 header plus
 * 1024 leaf + 4096 chain + 512 key.  The CERTIFICATE store holds 64 blocks,
 * so the record still fits with room to spare, and a v1 record (two blocks,
 * leaf plus key) keeps being read.
 */
#define APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX 4096U
#define APPLIANCE_PRODUCTION_CERTIFICATE_ISSUER_MAX 128U
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS 12U
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BYTES \
    (APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_BLOCKS * \
     APPLIANCE_STATE_BLOCK_SIZE)
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION 1U
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_VERSION_2 2U
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_HEADER_BYTES 16U
#define APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_HEADER_2_BYTES 24U
/*
 * Record v2 provenance: 0 = generated here, 1 = imported by an operator,
 * 2 = issued through ACME and verified against the named anchor (ITEM-508
 * increment 23).  Any other value is a corrupt record, not "generated":
 * provenance is what the serving record and the panel tell the operator.
 */
#define APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_GENERATED 0U
#define APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_OPERATOR 1U
#define APPLIANCE_PRODUCTION_CERTIFICATE_SOURCE_ACME 2U
#define APPLIANCE_PRODUCTION_CERTIFICATE_VALIDITY_LENGTH 15U
#define APPLIANCE_PRODUCTION_CERTIFICATE_SERIAL_BYTES 16U
#define APPLIANCE_PRODUCTION_CERTIFICATE_LIFETIME_SECONDS \
    UINT32_C(315360000)
/* Documented fallback window used only while no wall clock is validated. */
#define APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_BEFORE \
    "20260101000000"
#define APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_AFTER \
    "20360101000000"
/*
 * ITEM-404: the same window in epoch seconds, because the management TLS
 * lifecycle validates a certificate against a numeric wall clock.  The two
 * forms are asserted to name the same instant by the host proof.
 */
#define APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_BEFORE_EPOCH \
    UINT32_C(1767225600)
#define APPLIANCE_PRODUCTION_CERTIFICATE_FALLBACK_NOT_AFTER_EPOCH \
    UINT32_C(2082758400)

enum appliance_production_certificate_state {
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_NONE = 0,
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNINITIALIZED = 1,
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY = 2,
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_READY_EPHEMERAL = 3,
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_UNAVAILABLE = 4,
    APPLIANCE_PRODUCTION_CERTIFICATE_STATE_FAILED = 5,
};

enum appliance_production_certificate_status {
    APPLIANCE_PRODUCTION_CERTIFICATE_OK = 0,
    APPLIANCE_PRODUCTION_CERTIFICATE_NULL = 1,
    APPLIANCE_PRODUCTION_CERTIFICATE_BAD_STATE = 2,
    APPLIANCE_PRODUCTION_CERTIFICATE_STORE_UNAVAILABLE = 3,
    APPLIANCE_PRODUCTION_CERTIFICATE_STORE_SEALED = 4,
    APPLIANCE_PRODUCTION_CERTIFICATE_STORE_READ_FAILED = 5,
    APPLIANCE_PRODUCTION_CERTIFICATE_STORE_WRITE_FAILED = 6,
    APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_CORRUPT = 7,
    APPLIANCE_PRODUCTION_CERTIFICATE_ENTROPY_UNAVAILABLE = 8,
    APPLIANCE_PRODUCTION_CERTIFICATE_KEY_GENERATION_FAILED = 9,
    APPLIANCE_PRODUCTION_CERTIFICATE_CERTIFICATE_WRITE_FAILED = 10,
    APPLIANCE_PRODUCTION_CERTIFICATE_HOSTNAME_UNAVAILABLE = 11,
    APPLIANCE_PRODUCTION_CERTIFICATE_RECORD_TOO_LARGE = 12,
};

struct appliance_production_certificate {
    bool initialized;
    uint32_t version;
    enum appliance_production_certificate_state state;
    enum appliance_production_certificate_status last_status;
    bool available;
    bool generated;
    bool loaded_from_store;
    bool persisted;
    /* ITEM-503: the record says the certificate came from an operator. */
    bool imported;
    /*
     * ITEM-508 increment 23: the record says the chain was issued through
     * ACME and verified against the named anchor by this appliance.  It is a
     * separate flag from `imported`, because "the operator uploaded it" and
     * "the CA issued it to this appliance" are different statements, and the
     * serving record must not merge them.
     */
    bool acme_issued;
    bool store_bound;
    bool fallback_validity;
    struct appliance_state_block_device store;
    char common_name[
        APPLIANCE_PRODUCTION_CERTIFICATE_COMMON_NAME_MAX + 1U];
    uint32_t common_name_length;
    uint8_t certificate_der[
        APPLIANCE_PRODUCTION_CERTIFICATE_DER_MAX];
    uint32_t certificate_length;
    uint8_t key_der[APPLIANCE_PRODUCTION_CERTIFICATE_KEY_DER_MAX];
    uint32_t key_length;
    /*
     * ITEM-503: the issuing chain of an imported operator certificate
     * (concatenated DER, empty for the generated self-signed certificate).
     * It is persisted in record v2 and handed to the TLS handshake next to
     * the leaf, so a client that does not already trust the issuer can build
     * the path.
     */
    uint8_t chain_der[APPLIANCE_PRODUCTION_CERTIFICATE_CHAIN_DER_MAX];
    uint32_t chain_length;
    /*
     * ITEM-503: the issuer name of the served certificate, as the DER says.
     * An imported certificate must not be described as "HFOC self-signed" in
     * the lifecycle record, so serving publishes the parsed issuer.
     */
    char issuer[APPLIANCE_PRODUCTION_CERTIFICATE_ISSUER_MAX + 1U];
    uint32_t issuer_length;
    /*
     * ITEM-404: the lifecycle metadata of the DER that is actually served.
     * The window is the one written into the certificate (fallback window
     * when no wall clock was validated, otherwise the generated window) and
     * the fingerprint is SHA-256 over the DER.  The management TLS lifecycle
     * refuses a certificate without a window and a fingerprint, so serving
     * publishes these instead of a structure the lifecycle must reject.
     */
    bool metadata_ready;
    uint32_t not_before_epoch;
    uint32_t not_after_epoch;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];
    /* Raw mbedTLS result of the last failed generation step, for diagnostics. */
    int32_t last_result;
    uint32_t generation_count;
    uint32_t load_count;
    uint32_t store_write_count;
    /* ITEM-503: how many operator records this module wrote. */
    uint32_t import_count;
    /* ITEM-508 increment 23: how many ACME-issued records it wrote. */
    uint32_t acme_count;
};

enum appliance_production_certificate_status
appliance_production_certificate_prepare(
    struct appliance_production_certificate *certificate,
    struct appliance_state_loader *loader,
    uint32_t now_epoch_seconds);

/*
 * ITEM-503: persist an operator certificate, its optional chain and its key
 * as record v2.  The caller has already validated the bytes with
 * `appliance_certificate_import_validate`; this function writes, flushes and
 * reads the record back, and refuses (writing nothing) when the store is
 * unavailable, sealed, or the record would not fit.  The active in-memory
 * certificate is deliberately not replaced: the listener's TLS configuration
 * is built once, so the import applies on the next boot and the caller
 * publishes that honestly.
 */
enum appliance_production_certificate_status
appliance_production_certificate_adopt(
    struct appliance_production_certificate *certificate,
    const uint8_t *certificate_der,
    uint32_t certificate_length,
    const uint8_t *chain_der,
    uint32_t chain_length,
    const uint8_t *key_der,
    uint32_t key_length,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE]);

/*
 * ITEM-508 increment 23: persist the chain this appliance obtained through
 * ACME and verified against the named anchor.  The record, the bounds, the
 * flush and the read-back discipline are exactly the operator path; only the
 * provenance differs (`SOURCE_ACME`), so the serving record and the panel can
 * tell a generated self-signed identity, an operator upload and an issued
 * chain apart.  The caller passes the identity key whose public half the CSR
 * carried; the active in-memory certificate is deliberately not replaced, for
 * the same reason as `adopt` — it applies on the next boot.
 */
enum appliance_production_certificate_status
appliance_production_certificate_adopt_acme(
    struct appliance_production_certificate *certificate,
    const uint8_t *certificate_der,
    uint32_t certificate_length,
    const uint8_t *chain_der,
    uint32_t chain_length,
    const uint8_t *key_der,
    uint32_t key_length,
    uint8_t fingerprint_out[SHA256_DIGEST_SIZE]);

bool appliance_production_certificate_available(
    const struct appliance_production_certificate *certificate);
enum appliance_production_certificate_state
appliance_production_certificate_state(
    const struct appliance_production_certificate *certificate);
const char *appliance_production_certificate_state_string(
    enum appliance_production_certificate_state state);
const char *appliance_production_certificate_status_string(
    enum appliance_production_certificate_status status);

#endif
