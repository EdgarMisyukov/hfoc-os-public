#ifndef HFOC_APPLIANCE_ACME_JWS_H
#define HFOC_APPLIANCE_ACME_JWS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-508 increment 4: the request envelope an ACME account speaks with.
 *
 * Every ACME request is a JWS (RFC 8555 section 6.2) whose payload proves the
 * appliance holds the account key: the protected header names the algorithm
 * and the nonce, the signing input is `protected || '.' || payload`, and the
 * signature is the raw 64-byte `R || S` pair of ES256, base64url-encoded.
 * This module owns exactly that envelope and the two encodings it needs -
 * base64url, which the profile deliberately does not provide (`MBEDTLS_BASE64_C`
 * is off), and the JWK thumbprint of RFC 7638 - and nothing above it:
 *
 *   * the account key is ES256 (P-256 + SHA-256).  RSA stays disabled in the
 *     profile; the item's account key is the one this module generates or
 *     imports, and it refuses anything that is not an EC P-256 key;
 *   * the signature is converted from the DER mbedTLS writes to the raw R || S
 *     JWS expects, and a DER that is not exactly two integers of the curve
 *     size is a refusal - a signature that is not ES256 must never be sent;
 *   * base64url is unpadded and strict in both directions: an encoder input of
 *     any length round-trips, and a decoder refuses padding, the standard
 *     alphabet's `+` and `/`, a stray character and a length modulo four of
 *     one;
 *   * every output buffer is the caller's and every length is bounded, so a
 *     caller cannot be surprised by a bigger request than it reserved.
 *
 * The thumbprint is the account's stable name at the CA (RFC 7638 over the
 * canonical `{"crv","kty","x","y"}`), and the module computes it once per key
 * so a request never re-derives it silently.
 */

#define APPLIANCE_ACME_JWS_MAX 4096U
/* The raw ES256 signature a JWS carries: R || S, 32 bytes each. */
#define APPLIANCE_ACME_SIGNATURE_BYTES 64U
/* base64url of a SHA-256 digest: 43 characters, plus a terminator. */
#define APPLIANCE_ACME_THUMBPRINT_MAX 43U
#define APPLIANCE_ACME_JWK_MAX 256U
/*
 * ITEM-508 increment 22: the PKCS#10 DER one ACME finalize carries.  A P-256
 * CSR is a little over 300 bytes; the bound is the module's, not mbedTLS's, so
 * an oversized request is refused instead of silently truncated.
 */
#define APPLIANCE_ACME_CSR_MAX 512U
/* A CN is bounded by RFC 5280 and by the subject parser this module feeds. */
#define APPLIANCE_ACME_CSR_CN_MAX 64U

enum appliance_acme_base64_status {
    APPLIANCE_ACME_BASE64_OK = 0,
    APPLIANCE_ACME_BASE64_NULL = 1,
    APPLIANCE_ACME_BASE64_BAD_INPUT = 2,
    APPLIANCE_ACME_BASE64_BUFFER_TOO_SMALL = 3,
};

enum appliance_acme_jws_status {
    APPLIANCE_ACME_JWS_OK = 0,
    APPLIANCE_ACME_JWS_NULL = 1,
    APPLIANCE_ACME_JWS_BAD_STATE = 2,
    APPLIANCE_ACME_JWS_BAD_KEY = 3,
    APPLIANCE_ACME_JWS_BAD_INPUT = 4,
    APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL = 5,
    APPLIANCE_ACME_JWS_RANDOM_FAILED = 6,
    APPLIANCE_ACME_JWS_SIGNATURE_FAILED = 7,
    APPLIANCE_ACME_JWS_CSR_FAILED = 8,
};

/*
 * The account key.  The appliance has exactly one, so the private context
 * lives inside the module (statically, like the trust anchor's caches) and
 * this structure carries what a caller reads: the public JWK and its RFC 7638
 * thumbprint.  `free` zeroizes the private half.
 */
struct appliance_acme_key {
    bool initialized;
    uint32_t version;
    char thumbprint[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    uint32_t thumbprint_length;
    char jwk[APPLIANCE_ACME_JWK_MAX + 1U];
    uint32_t jwk_length;
};

/* Unpadded base64url, the JWS alphabet. */
enum appliance_acme_base64_status appliance_acme_base64url_encode(
    const unsigned char *data,
    size_t length,
    char *out,
    size_t capacity,
    size_t *length_out);
enum appliance_acme_base64_status appliance_acme_base64url_decode(
    const char *text,
    size_t length,
    unsigned char *out,
    size_t capacity,
    size_t *length_out);

/*
 * Bind an account key.  Both calls refuse a key that is not EC P-256 and
 * derive the public JWK and the RFC 7638 thumbprint at once, because every
 * request needs both.
 */
enum appliance_acme_jws_status appliance_acme_key_import(
    struct appliance_acme_key *key,
    const unsigned char *key_der,
    size_t key_length);
enum appliance_acme_jws_status appliance_acme_key_generate(
    struct appliance_acme_key *key,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context);
void appliance_acme_key_free(struct appliance_acme_key *key);

const char *appliance_acme_key_jwk(const struct appliance_acme_key *key);
const char *appliance_acme_key_thumbprint(const struct appliance_acme_key *key);

/*
 * ITEM-508 increment 22: write the PKCS#10 request the finalize sends
 * (RFC 8555 section 7.4).  The private context lives inside this module and
 * there is deliberately no accessor for it, so the CSR is written here: the
 * subject is exactly `CN=<common_name>`, the digest is SHA-256, the key is the
 * bound account key, and the caller receives the bounded DER only.  An
 * uninitialized key, an empty or over-long subject and a buffer below
 * `APPLIANCE_ACME_CSR_MAX` are refusals, never a truncated request.
 */
enum appliance_acme_jws_status appliance_acme_key_write_csr(
    const struct appliance_acme_key *key,
    const char *common_name,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context,
    unsigned char *der,
    size_t capacity,
    size_t *length_out);

/*
 * One flattened JWS: `{"protected":"..","payload":"..","signature":".."}`.
 * An empty payload is legal (POST-as-GET) and is encoded as the empty string.
 */
enum appliance_acme_jws_status appliance_acme_jws_sign(
    const struct appliance_acme_key *key,
    const char *protected_json,
    size_t protected_length,
    const unsigned char *payload,
    size_t payload_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context,
    char *out,
    size_t capacity,
    size_t *length_out);

const char *appliance_acme_base64_status_string(
    enum appliance_acme_base64_status status);
const char *appliance_acme_jws_status_string(
    enum appliance_acme_jws_status status);

#endif
