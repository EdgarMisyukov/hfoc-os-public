#include "appliance_acme_jws.h"

/*
 * ITEM-508 increment 4: the JWS envelope, the account key and the two
 * encodings it needs.  The module is one of the TLS-profile kernel objects,
 * so it sees the frozen mbedTLS profile and nothing else.
 */
#if defined(HFOC_PRODUCTION_TLS) || \
    defined(HFOC_APPLIANCE_ACME_JWS_HOST_TEST)

#include "mbedtls/ecp.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_csr.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_ACME_KEY_VERSION 1U
#define APPLIANCE_ACME_P256_BYTES 32U
#define APPLIANCE_ACME_SIGNATURE_DER_MAX 80U

/*
 * One account key per appliance: the private context lives here, statically,
 * and `free` zeroizes it.
 */
static mbedtls_pk_context appliance_acme_key_context;
static bool appliance_acme_key_context_ready;

static const char appliance_acme_base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static int appliance_acme_base64_value(char character)
{
    for (int index = 0; index < 64; ++index) {
        if (appliance_acme_base64_alphabet[index] == character) {
            return index;
        }
    }
    return -1;
}

enum appliance_acme_base64_status appliance_acme_base64url_encode(
    const unsigned char *data,
    size_t length,
    char *out,
    size_t capacity,
    size_t *length_out)
{
    size_t needed;
    size_t position = 0U;
    size_t index = 0U;

    if (out == NULL || length_out == NULL || (data == NULL && length != 0U)) {
        return APPLIANCE_ACME_BASE64_NULL;
    }
    needed = ((length + 2U) / 3U) * 4U;
    needed -= length % 3U == 0U ? 0U : (length % 3U == 1U ? 2U : 1U);
    if (capacity <= needed) {
        return APPLIANCE_ACME_BASE64_BUFFER_TOO_SMALL;
    }
    while (index + 3U <= length) {
        const uint32_t chunk = ((uint32_t)data[index] << 16U) |
                               ((uint32_t)data[index + 1U] << 8U) |
                               (uint32_t)data[index + 2U];

        out[position++] = appliance_acme_base64_alphabet[(chunk >> 18U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[(chunk >> 12U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[(chunk >> 6U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[chunk & 63U];
        index += 3U;
    }
    if (length - index == 1U) {
        const uint32_t chunk = (uint32_t)data[index] << 16U;

        out[position++] = appliance_acme_base64_alphabet[(chunk >> 18U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[(chunk >> 12U) & 63U];
    } else if (length - index == 2U) {
        const uint32_t chunk = ((uint32_t)data[index] << 16U) |
                               ((uint32_t)data[index + 1U] << 8U);

        out[position++] = appliance_acme_base64_alphabet[(chunk >> 18U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[(chunk >> 12U) & 63U];
        out[position++] = appliance_acme_base64_alphabet[(chunk >> 6U) & 63U];
    }
    out[position] = '\0';
    *length_out = position;
    return APPLIANCE_ACME_BASE64_OK;
}

enum appliance_acme_base64_status appliance_acme_base64url_decode(
    const char *text,
    size_t length,
    unsigned char *out,
    size_t capacity,
    size_t *length_out)
{
    size_t position = 0U;
    size_t index = 0U;

    if (text == NULL || length_out == NULL ||
        (out == NULL && capacity != 0U)) {
        return APPLIANCE_ACME_BASE64_NULL;
    }
    if (length % 4U == 1U) {
        return APPLIANCE_ACME_BASE64_BAD_INPUT;
    }
    while (index < length) {
        const size_t remaining = length - index;
        const size_t chunk_length = remaining >= 4U ? 4U : remaining;
        uint32_t chunk = 0U;

        for (size_t offset = 0U; offset < chunk_length; ++offset) {
            const int value = appliance_acme_base64_value(text[index + offset]);

            if (value < 0) {
                return APPLIANCE_ACME_BASE64_BAD_INPUT;
            }
            chunk |= (uint32_t)value << (18U - 6U * (uint32_t)offset);
        }
        {
            const size_t produced = chunk_length == 4U ? 3U
                                    : chunk_length == 3U ? 2U
                                                         : 1U;

            if (position + produced > capacity) {
                return APPLIANCE_ACME_BASE64_BUFFER_TOO_SMALL;
            }
        }
        out[position++] = (unsigned char)((chunk >> 16U) & 0xFFU);
        if (chunk_length >= 3U) {
            out[position++] = (unsigned char)((chunk >> 8U) & 0xFFU);
        }
        if (chunk_length == 4U) {
            out[position++] = (unsigned char)(chunk & 0xFFU);
        }
        index += chunk_length;
    }
    *length_out = position;
    return APPLIANCE_ACME_BASE64_OK;
}

/*
 * The public key of the account, as the uncompressed point.  The profile
 * keeps mbedTLS's private structures closed, so the point is read from the
 * DER mbedTLS itself writes (`SubjectPublicKeyInfo`) and its shape is
 * validated: an EC key whose point is the 65-byte uncompressed form is a
 * P-256 key, because the profile enables no other curve with that point
 * size.
 */
static bool appliance_acme_key_public_point(
    const mbedtls_pk_context *key,
    unsigned char point[1U + 2U * APPLIANCE_ACME_P256_BYTES])
{
    unsigned char der[APPLIANCE_ACME_JWK_MAX];
    const unsigned char *cursor;
    size_t length;
    int written;

    if (mbedtls_pk_get_type(key) != MBEDTLS_PK_ECKEY) {
        return false;
    }
    memset(der, 0, sizeof(der));
    written = mbedtls_pk_write_pubkey_der(key, der, sizeof(der));
    if (written <= 0 || (size_t)written > sizeof(der)) {
        memset(der, 0, sizeof(der));
        return false;
    }
    length = (size_t)written;
    cursor = &der[sizeof(der) - length];
    for (size_t index = 0U; index + 4U <= length; ++index) {
        if (cursor[index] == 0x03U && cursor[index + 1U] == 0x42U &&
            cursor[index + 2U] == 0x00U && cursor[index + 3U] == 0x04U &&
            index + 4U + 2U * APPLIANCE_ACME_P256_BYTES == length) {
            memcpy(point, &cursor[index + 3U],
                   1U + 2U * APPLIANCE_ACME_P256_BYTES);
            memset(der, 0, sizeof(der));
            return true;
        }
    }
    memset(der, 0, sizeof(der));
    return false;
}

/*
 * The public JWK and the RFC 7638 thumbprint over its canonical form.  Both
 * are derived once, at bind time, so a request never re-derives them.
 */
static enum appliance_acme_jws_status appliance_acme_key_publish(
    struct appliance_acme_key *key)
{
    unsigned char point[1U + 2U * APPLIANCE_ACME_P256_BYTES];
    char x[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    char y[APPLIANCE_ACME_THUMBPRINT_MAX + 1U];
    size_t x_length = 0U;
    size_t y_length = 0U;
    size_t jwk_length = 0U;
    unsigned char digest[32U];
    const char *prefix = "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"";
    const char *middle = "\",\"y\":\"";
    const char *suffix = "\"}";

    if (!appliance_acme_key_public_point(
            &appliance_acme_key_context, point)) {
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    if (appliance_acme_base64url_encode(
            &point[1U], APPLIANCE_ACME_P256_BYTES, x, sizeof(x),
            &x_length) != APPLIANCE_ACME_BASE64_OK ||
        appliance_acme_base64url_encode(
            &point[1U + APPLIANCE_ACME_P256_BYTES],
            APPLIANCE_ACME_P256_BYTES, y, sizeof(y),
            &y_length) != APPLIANCE_ACME_BASE64_OK) {
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    jwk_length = strlen(prefix) + x_length + strlen(middle) + y_length +
                 strlen(suffix);
    if (jwk_length > APPLIANCE_ACME_JWK_MAX) {
        return APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL;
    }
    memcpy(key->jwk, prefix, strlen(prefix));
    memcpy(&key->jwk[strlen(prefix)], x, x_length);
    memcpy(&key->jwk[strlen(prefix) + x_length], middle, strlen(middle));
    memcpy(&key->jwk[strlen(prefix) + x_length + strlen(middle)], y,
           y_length);
    memcpy(&key->jwk[jwk_length - strlen(suffix)], suffix, strlen(suffix));
    key->jwk[jwk_length] = '\0';
    key->jwk_length = (uint32_t)jwk_length;

    /*
     * RFC 7638: the thumbprint is over the canonical JSON of the required
     * members - the exact bytes just built, which carry only those members
     * and no whitespace.
     */
    if (mbedtls_sha256((const unsigned char *)key->jwk, jwk_length,
                       digest, 0) != 0) {
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    if (appliance_acme_base64url_encode(
            digest, sizeof(digest), key->thumbprint,
            sizeof(key->thumbprint), &x_length) !=
            APPLIANCE_ACME_BASE64_OK ||
        x_length != APPLIANCE_ACME_THUMBPRINT_MAX) {
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    key->thumbprint_length = (uint32_t)x_length;
    memset(digest, 0, sizeof(digest));
    memset(x, 0, sizeof(x));
    memset(y, 0, sizeof(y));
    return APPLIANCE_ACME_JWS_OK;
}

void appliance_acme_key_free(struct appliance_acme_key *key)
{
    if (appliance_acme_key_context_ready) {
        mbedtls_pk_free(&appliance_acme_key_context);
        appliance_acme_key_context_ready = false;
    }
    if (key != NULL) {
        memset(key, 0, sizeof(*key));
    }
}

static enum appliance_acme_jws_status appliance_acme_key_bind(
    struct appliance_acme_key *key,
    const unsigned char *key_der,
    size_t key_length,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    if (key == NULL || key_der == NULL || key_length == 0U) {
        return APPLIANCE_ACME_JWS_NULL;
    }
    appliance_acme_key_free(key);
    memset(&appliance_acme_key_context, 0, sizeof(appliance_acme_key_context));
    mbedtls_pk_init(&appliance_acme_key_context);
    appliance_acme_key_context_ready = true;
    if (mbedtls_pk_parse_key(
            &appliance_acme_key_context, key_der, key_length, NULL, 0U,
            f_rng, rng_context) != 0 ||
        mbedtls_pk_get_type(&appliance_acme_key_context) !=
            MBEDTLS_PK_ECKEY) {
        appliance_acme_key_free(key);
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    key->version = APPLIANCE_ACME_KEY_VERSION;
    key->initialized = true;
    {
        const enum appliance_acme_jws_status status =
            appliance_acme_key_publish(key);

        if (status != APPLIANCE_ACME_JWS_OK) {
            appliance_acme_key_free(key);
            return status;
        }
    }
    return APPLIANCE_ACME_JWS_OK;
}

enum appliance_acme_jws_status appliance_acme_key_import(
    struct appliance_acme_key *key,
    const unsigned char *key_der,
    size_t key_length)
{
    return appliance_acme_key_bind(key, key_der, key_length, NULL, NULL);
}

enum appliance_acme_jws_status appliance_acme_key_generate(
    struct appliance_acme_key *key,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context)
{
    if (key == NULL || f_rng == NULL) {
        return APPLIANCE_ACME_JWS_NULL;
    }
    appliance_acme_key_free(key);
    memset(&appliance_acme_key_context, 0, sizeof(appliance_acme_key_context));
    mbedtls_pk_init(&appliance_acme_key_context);
    appliance_acme_key_context_ready = true;
    if (mbedtls_pk_setup(
            &appliance_acme_key_context,
            mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) != 0 ||
        mbedtls_ecp_gen_key(
            MBEDTLS_ECP_DP_SECP256R1,
            mbedtls_pk_ec(appliance_acme_key_context), f_rng,
            rng_context) != 0) {
        appliance_acme_key_free(key);
        return APPLIANCE_ACME_JWS_RANDOM_FAILED;
    }
    key->version = APPLIANCE_ACME_KEY_VERSION;
    key->initialized = true;
    {
        const enum appliance_acme_jws_status status =
            appliance_acme_key_publish(key);

        if (status != APPLIANCE_ACME_JWS_OK) {
            appliance_acme_key_free(key);
            return status;
        }
    }
    return APPLIANCE_ACME_JWS_OK;
}

const char *appliance_acme_key_jwk(const struct appliance_acme_key *key)
{
    if (key == NULL || !key->initialized) {
        return NULL;
    }
    return key->jwk;
}

const char *appliance_acme_key_thumbprint(const struct appliance_acme_key *key)
{
    if (key == NULL || !key->initialized) {
        return NULL;
    }
    return key->thumbprint;
}

/*
 * ITEM-508 increment 22: the subject of the CSR is one common name, bound to
 * the profile's own name rules.  The characters an X.509 name parser reads as
 * separators (`=`, `,`, `+`, `#`, `;`, `<`, `>`, `"`, `\`) and every control or
 * non-ASCII byte are refusals, so a declared name cannot rewrite the subject.
 */
static bool appliance_acme_csr_subject_valid(
    const char *common_name)
{
    size_t length = 0U;

    if (common_name == NULL) {
        return false;
    }
    while (common_name[length] != '\0') {
        const unsigned char value = (unsigned char)common_name[length];

        if (value <= 0x20U || value >= 0x7FU ||
            value == (unsigned char)',' || value == (unsigned char)'+' ||
            value == (unsigned char)'=' || value == (unsigned char)'<' ||
            value == (unsigned char)'>' || value == (unsigned char)'#' ||
            value == (unsigned char)';' || value == (unsigned char)'"' ||
            value == (unsigned char)'\\') {
            return false;
        }
        ++length;
    }
    return length > 0U && length <= APPLIANCE_ACME_CSR_CN_MAX;
}

enum appliance_acme_jws_status appliance_acme_key_write_csr(
    const struct appliance_acme_key *key,
    const char *common_name,
    int (*f_rng)(void *context, unsigned char *output, size_t length),
    void *rng_context,
    unsigned char *der,
    size_t capacity,
    size_t *length_out)
{
    static const char subject_prefix[] = "CN=";
    mbedtls_x509write_csr csr;
    char subject[APPLIANCE_ACME_CSR_CN_MAX + sizeof(subject_prefix)];
    size_t common_name_length = 0U;
    int result;

    if (key == NULL || common_name == NULL || der == NULL ||
        length_out == NULL || f_rng == NULL) {
        return APPLIANCE_ACME_JWS_NULL;
    }
    if (!key->initialized || !appliance_acme_key_context_ready) {
        return APPLIANCE_ACME_JWS_BAD_KEY;
    }
    if (!appliance_acme_csr_subject_valid(common_name)) {
        return APPLIANCE_ACME_JWS_BAD_INPUT;
    }
    /*
     * The bound is the module's: a smaller destination is a refusal before
     * mbedTLS is asked to write anything, so a caller can never receive a
     * partially written request.
     */
    if (capacity < APPLIANCE_ACME_CSR_MAX) {
        return APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL;
    }
    memcpy(subject, subject_prefix, sizeof(subject_prefix) - 1U);
    while (common_name[common_name_length] != '\0') {
        subject[sizeof(subject_prefix) - 1U + common_name_length] =
            common_name[common_name_length];
        ++common_name_length;
    }
    subject[sizeof(subject_prefix) - 1U + common_name_length] = '\0';

    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &appliance_acme_key_context);
    if (mbedtls_x509write_csr_set_subject_name(&csr, subject) != 0) {
        mbedtls_x509write_csr_free(&csr);
        return APPLIANCE_ACME_JWS_BAD_INPUT;
    }
    result = mbedtls_x509write_csr_der(
        &csr, der, capacity, f_rng, rng_context);
    mbedtls_x509write_csr_free(&csr);
    if (result < 0) {
        if (result == MBEDTLS_ERR_X509_BUFFER_TOO_SMALL) {
            return APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL;
        }
        if (result == MBEDTLS_ERR_ECP_RANDOM_FAILED) {
            return APPLIANCE_ACME_JWS_RANDOM_FAILED;
        }
        return APPLIANCE_ACME_JWS_CSR_FAILED;
    }
    /*
     * mbedTLS writes the DER at the end of the buffer and answers its length;
     * the caller gets the request at the start, exactly `length` bytes long.
     */
    memmove(der, &der[capacity - (size_t)result], (size_t)result);
    *length_out = (size_t)result;
    return APPLIANCE_ACME_JWS_OK;
}

/*
 * The raw R || S an ES256 JWS carries, converted from the DER mbedTLS writes.
 * A DER that is not exactly two INTEGERs of the curve size is a refusal: a
 * signature that is not ES256 must never be sent.
 */
static bool appliance_acme_signature_from_der(
    const unsigned char *der,
    size_t der_length,
    unsigned char signature[APPLIANCE_ACME_SIGNATURE_BYTES])
{
    size_t position = 0U;
    size_t integer;

    if (der == NULL || der_length < 8U || der[position++] != 0x30U) {
        return false;
    }
    {
        size_t length = der[position++];

        if ((length & 0x80U) != 0U) {
            const size_t octets = length & 0x7FU;

            if (octets != 1U || position + 1U > der_length) {
                return false;
            }
            length = der[position++];
        }
        if (position + length != der_length) {
            return false;
        }
    }
    memset(signature, 0, APPLIANCE_ACME_SIGNATURE_BYTES);
    for (integer = 0U; integer < 2U; ++integer) {
        size_t length;
        size_t offset = integer * APPLIANCE_ACME_P256_BYTES;

        if (position + 2U > der_length || der[position++] != 0x02U) {
            return false;
        }
        length = der[position++];
        if ((length & 0x80U) != 0U || position + length > der_length) {
            return false;
        }
        while (length > 1U && der[position] == 0x00U) {
            position += 1U;
            length -= 1U;
        }
        if (length > APPLIANCE_ACME_P256_BYTES) {
            return false;
        }
        memcpy(&signature[offset + APPLIANCE_ACME_P256_BYTES - length],
               &der[position], length);
        position += length;
    }
    return position == der_length;
}

static enum appliance_acme_jws_status appliance_acme_jws_append(
    char *out,
    size_t capacity,
    size_t *position,
    const char *text,
    size_t length)
{
    if (*position + length >= capacity) {
        return APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL;
    }
    memcpy(&out[*position], text, length);
    *position += length;
    out[*position] = '\0';
    return APPLIANCE_ACME_JWS_OK;
}

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
    size_t *length_out)
{
    static unsigned char signing_input[APPLIANCE_ACME_JWS_MAX];
    static char protected_encoded[APPLIANCE_ACME_JWS_MAX];
    static char payload_encoded[APPLIANCE_ACME_JWS_MAX];
    static char signature_encoded[APPLIANCE_ACME_SIGNATURE_BYTES * 2U];
    static unsigned char signature_der[APPLIANCE_ACME_SIGNATURE_DER_MAX];
    unsigned char signature[APPLIANCE_ACME_SIGNATURE_BYTES];
    unsigned char digest[32U];
    size_t protected_length_encoded = 0U;
    size_t payload_length_encoded = 0U;
    size_t signature_length_encoded = 0U;
    size_t signing_length = 0U;
    size_t signature_der_length = 0U;
    size_t position = 0U;
    enum appliance_acme_jws_status status;

    if (key == NULL || protected_json == NULL || out == NULL ||
        length_out == NULL || f_rng == NULL ||
        (payload == NULL && payload_length != 0U)) {
        return APPLIANCE_ACME_JWS_NULL;
    }
    if (!key->initialized || !appliance_acme_key_context_ready ||
        key->version != APPLIANCE_ACME_KEY_VERSION ||
        mbedtls_pk_get_type(&appliance_acme_key_context) !=
            MBEDTLS_PK_ECKEY) {
        return APPLIANCE_ACME_JWS_BAD_STATE;
    }
    if (protected_length == 0U ||
        protected_length > APPLIANCE_ACME_JWS_MAX / 2U ||
        payload_length > APPLIANCE_ACME_JWS_MAX / 2U) {
        return APPLIANCE_ACME_JWS_BAD_INPUT;
    }
    if (appliance_acme_base64url_encode(
            (const unsigned char *)protected_json, protected_length,
            protected_encoded, sizeof(protected_encoded),
            &protected_length_encoded) != APPLIANCE_ACME_BASE64_OK ||
        appliance_acme_base64url_encode(
            payload == NULL ? (const unsigned char *)"" : payload,
            payload_length, payload_encoded, sizeof(payload_encoded),
            &payload_length_encoded) != APPLIANCE_ACME_BASE64_OK) {
        return APPLIANCE_ACME_JWS_BAD_INPUT;
    }
    /*
     * The signing input is `protected || '.' || payload` over the *encoded*
     * text, exactly as RFC 7515 defines it.
     */
    if (protected_length_encoded + 1U + payload_length_encoded >
        sizeof(signing_input)) {
        return APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL;
    }
    memcpy(signing_input, protected_encoded, protected_length_encoded);
    signing_input[protected_length_encoded] = (unsigned char)'.';
    memcpy(&signing_input[protected_length_encoded + 1U], payload_encoded,
           payload_length_encoded);
    signing_length = protected_length_encoded + 1U + payload_length_encoded;

    if (mbedtls_md(
            mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), signing_input,
            signing_length, digest) != 0) {
        return APPLIANCE_ACME_JWS_SIGNATURE_FAILED;
    }
    if (mbedtls_pk_sign(
            &appliance_acme_key_context, MBEDTLS_MD_SHA256, digest,
            sizeof(digest), signature_der, sizeof(signature_der),
            &signature_der_length, f_rng, rng_context) != 0) {
        return APPLIANCE_ACME_JWS_SIGNATURE_FAILED;
    }
    memset(digest, 0, sizeof(digest));
    if (!appliance_acme_signature_from_der(
            signature_der, signature_der_length, signature)) {
        memset(signature_der, 0, sizeof(signature_der));
        return APPLIANCE_ACME_JWS_SIGNATURE_FAILED;
    }
    memset(signature_der, 0, sizeof(signature_der));
    if (appliance_acme_base64url_encode(
            signature, sizeof(signature), signature_encoded,
            sizeof(signature_encoded), &signature_length_encoded) !=
        APPLIANCE_ACME_BASE64_OK) {
        memset(signature, 0, sizeof(signature));
        return APPLIANCE_ACME_JWS_SIGNATURE_FAILED;
    }
    memset(signature, 0, sizeof(signature));

    static const char jws_prefix[] = "{\"protected\":\"";
    static const char jws_payload[] = "\",\"payload\":\"";
    static const char jws_signature[] = "\",\"signature\":\"";
    static const char jws_suffix[] = "\"}";

    status = appliance_acme_jws_append(
        out, capacity, &position, jws_prefix, sizeof(jws_prefix) - 1U);
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, protected_encoded,
            protected_length_encoded);
    }
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, jws_payload,
            sizeof(jws_payload) - 1U);
    }
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, payload_encoded, payload_length_encoded);
    }
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, jws_signature,
            sizeof(jws_signature) - 1U);
    }
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, signature_encoded,
            signature_length_encoded);
    }
    if (status == APPLIANCE_ACME_JWS_OK) {
        status = appliance_acme_jws_append(
            out, capacity, &position, jws_suffix, sizeof(jws_suffix) - 1U);
    }
    if (status != APPLIANCE_ACME_JWS_OK) {
        return status;
    }
    *length_out = position;
    return APPLIANCE_ACME_JWS_OK;
}

const char *appliance_acme_base64_status_string(
    enum appliance_acme_base64_status status)
{
    switch (status) {
    case APPLIANCE_ACME_BASE64_OK:
        return "OK";
    case APPLIANCE_ACME_BASE64_NULL:
        return "NULL";
    case APPLIANCE_ACME_BASE64_BAD_INPUT:
        return "BAD_INPUT";
    case APPLIANCE_ACME_BASE64_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        break;
    }
    return "UNKNOWN";
}

const char *appliance_acme_jws_status_string(
    enum appliance_acme_jws_status status)
{
    switch (status) {
    case APPLIANCE_ACME_JWS_OK:
        return "OK";
    case APPLIANCE_ACME_JWS_NULL:
        return "NULL";
    case APPLIANCE_ACME_JWS_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_ACME_JWS_BAD_KEY:
        return "BAD_KEY";
    case APPLIANCE_ACME_JWS_BAD_INPUT:
        return "BAD_INPUT";
    case APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case APPLIANCE_ACME_JWS_RANDOM_FAILED:
        return "RANDOM_FAILED";
    case APPLIANCE_ACME_JWS_SIGNATURE_FAILED:
        return "SIGNATURE_FAILED";
    case APPLIANCE_ACME_JWS_CSR_FAILED:
        return "CSR_FAILED";
    default:
        break;
    }
    return "UNKNOWN";
}

#endif /* HFOC_PRODUCTION_TLS || HFOC_APPLIANCE_ACME_JWS_HOST_TEST */
