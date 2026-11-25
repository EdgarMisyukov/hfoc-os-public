#include "hfoc_mbedtls_manifest.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef HFOC_MBEDTLS_MANIFEST_HOST_TEST

#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509.h"
#include "mbedtls/x509_crt.h"

static const char *const hfoc_mbedtls_manifest_status_names[] = {
    "OK",
    "NULL",
    "BAD_MAGIC",
    "BAD_VERSION",
    "BAD_ENTRY_COUNT",
    "BAD_PURPOSE",
    "BAD_ALGORITHM",
    "BAD_HOSTNAME_LENGTH",
    "BAD_HOSTNAME",
    "BAD_CERTIFICATE_LENGTH",
    "BAD_KEY_LENGTH",
    "BAD_TOTAL_LENGTH",
    "TRAILING_BYTES",
    "BAD_DIGEST",
    "CERTIFICATE_PARSE_FAILED",
    "CERTIFICATE_CHAIN_REJECTED",
    "CERTIFICATE_NOT_EC",
    "CERTIFICATE_NOT_P256",
    "CERTIFICATE_HOSTNAME_MISMATCH",
    "KEY_PARSE_FAILED",
    "KEY_NOT_EC",
    "KEY_NOT_P256",
    "KEY_MISMATCH",
};

static uint32_t hfoc_mbedtls_manifest_read_u32(
    const uint8_t *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint16_t hfoc_mbedtls_manifest_read_u16(
    const uint8_t *bytes)
{
    return (uint16_t)bytes[0] |
           ((uint16_t)bytes[1] << 8U);
}

static bool hfoc_mbedtls_manifest_san_matches(
    const mbedtls_x509_crt *crt)
{
    const mbedtls_x509_sequence *current;

    for (current = &crt->subject_alt_names;
         current != NULL;
         current = current->next) {
        mbedtls_x509_subject_alternative_name san;

        memset(&san, 0, sizeof(san));
        if (mbedtls_x509_parse_subject_alt_name(
                &current->buf, &san) == 0) {
            bool matches =
                san.type == MBEDTLS_X509_SAN_DNS_NAME &&
                san.san.unstructured_name.len ==
                    HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH &&
                memcmp(
                    san.san.unstructured_name.p,
                    HFOC_MBEDTLS_MANIFEST_HOSTNAME,
                    HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH) == 0;

            mbedtls_x509_free_subject_alt_name(&san);
            if (matches) {
                return true;
            }
        }
    }
    return false;
}

static bool hfoc_mbedtls_manifest_pk_is_p256(
    const mbedtls_pk_context *pk)
{
    mbedtls_pk_type_t type = mbedtls_pk_get_type(pk);
    mbedtls_ecp_keypair *ec;

    if (type != MBEDTLS_PK_ECKEY &&
        type != MBEDTLS_PK_ECDSA) {
        return false;
    }
    ec = mbedtls_pk_ec(*pk);
    return ec != NULL &&
           mbedtls_ecp_keypair_get_group_id(ec) ==
               MBEDTLS_ECP_DP_SECP256R1;
}

const char *hfoc_mbedtls_manifest_status_string(
    enum hfoc_mbedtls_manifest_status status)
{
    if ((unsigned int)status >
        (unsigned int)HFOC_MBEDTLS_MANIFEST_LAST_STATUS) {
        return NULL;
    }
    return hfoc_mbedtls_manifest_status_names[status];
}

enum hfoc_mbedtls_manifest_status hfoc_mbedtls_manifest_parse(
    struct hfoc_mbedtls_manifest *manifest,
    const void *manifest_bytes,
    uint32_t manifest_length)
{
    const uint8_t *bytes = manifest_bytes;
    uint16_t hostname_length;
    uint32_t purpose_id;
    uint32_t algorithm_id;
    uint32_t certificate_length;
    uint32_t key_length;
    uint32_t certificate_offset;
    uint32_t key_offset;
    uint32_t key_end;
    uint8_t digest[HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH];
    mbedtls_x509_crt certificate;
    mbedtls_pk_context key;
    unsigned char certificate_public[
        HFOC_MBEDTLS_MANIFEST_PUBLIC_KEY_MAX_BYTES];
    unsigned char key_public[
        HFOC_MBEDTLS_MANIFEST_PUBLIC_KEY_MAX_BYTES];
    int certificate_public_length;
    int key_public_length;
    enum hfoc_mbedtls_manifest_status status =
        HFOC_MBEDTLS_MANIFEST_OK;
    struct hfoc_mbedtls_manifest result;

    if (manifest == NULL || bytes == NULL) {
        return HFOC_MBEDTLS_MANIFEST_NULL;
    }
    if (manifest_length < HFOC_MBEDTLS_MANIFEST_HEADER_BYTES ||
        manifest_length > HFOC_MBEDTLS_MANIFEST_MAX_TOTAL_BYTES) {
        return HFOC_MBEDTLS_MANIFEST_BAD_TOTAL_LENGTH;
    }

    if (hfoc_mbedtls_manifest_read_u32(&bytes[0]) !=
        HFOC_MBEDTLS_MANIFEST_MAGIC) {
        return HFOC_MBEDTLS_MANIFEST_BAD_MAGIC;
    }
    if (hfoc_mbedtls_manifest_read_u32(&bytes[4]) !=
        HFOC_MBEDTLS_MANIFEST_VERSION) {
        return HFOC_MBEDTLS_MANIFEST_BAD_VERSION;
    }
    if (hfoc_mbedtls_manifest_read_u32(&bytes[8]) != 1U) {
        return HFOC_MBEDTLS_MANIFEST_BAD_ENTRY_COUNT;
    }
    purpose_id = hfoc_mbedtls_manifest_read_u32(&bytes[12]);
    if (purpose_id != HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE) {
        return HFOC_MBEDTLS_MANIFEST_BAD_PURPOSE;
    }
    algorithm_id = hfoc_mbedtls_manifest_read_u32(&bytes[16]);
    if (algorithm_id !=
        HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256) {
        return HFOC_MBEDTLS_MANIFEST_BAD_ALGORITHM;
    }
    hostname_length = hfoc_mbedtls_manifest_read_u16(&bytes[20]);
    if (hostname_length != HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH) {
        return HFOC_MBEDTLS_MANIFEST_BAD_HOSTNAME_LENGTH;
    }
    if (memcmp(
            &bytes[22],
            HFOC_MBEDTLS_MANIFEST_HOSTNAME,
            HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH) != 0) {
        return HFOC_MBEDTLS_MANIFEST_BAD_HOSTNAME;
    }

    certificate_length =
        hfoc_mbedtls_manifest_read_u32(&bytes[86]);
    key_length = hfoc_mbedtls_manifest_read_u32(&bytes[90]);
    if (certificate_length < HFOC_MBEDTLS_MANIFEST_CERTIFICATE_MIN ||
        certificate_length > HFOC_MBEDTLS_MANIFEST_CERTIFICATE_MAX) {
        return HFOC_MBEDTLS_MANIFEST_BAD_CERTIFICATE_LENGTH;
    }
    if (key_length < HFOC_MBEDTLS_MANIFEST_KEY_MIN ||
        key_length > HFOC_MBEDTLS_MANIFEST_KEY_MAX) {
        return HFOC_MBEDTLS_MANIFEST_BAD_KEY_LENGTH;
    }

    certificate_offset = HFOC_MBEDTLS_MANIFEST_HEADER_BYTES;
    key_offset = certificate_offset + certificate_length;
    key_end = key_offset + key_length;
    if (key_end != manifest_length) {
        return HFOC_MBEDTLS_MANIFEST_TRAILING_BYTES;
    }

    if (mbedtls_sha256(
            &bytes[certificate_offset],
            certificate_length,
            digest,
            0) != 0 ||
        memcmp(
            &bytes[94],
            digest,
            HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH) != 0) {
        return HFOC_MBEDTLS_MANIFEST_BAD_DIGEST;
    }

    mbedtls_x509_crt_init(&certificate);
    mbedtls_pk_init(&key);

    if (mbedtls_x509_crt_parse_der(
            &certificate,
            &bytes[certificate_offset],
            certificate_length) != 0) {
        status = HFOC_MBEDTLS_MANIFEST_CERTIFICATE_PARSE_FAILED;
        goto cleanup;
    }
    if (certificate.next != NULL) {
        status = HFOC_MBEDTLS_MANIFEST_CERTIFICATE_CHAIN_REJECTED;
        goto cleanup;
    }
    if (!hfoc_mbedtls_manifest_pk_is_p256(&certificate.pk)) {
        status = mbedtls_pk_get_type(&certificate.pk) ==
                     MBEDTLS_PK_ECKEY ||
                 mbedtls_pk_get_type(&certificate.pk) ==
                     MBEDTLS_PK_ECDSA ?
                     HFOC_MBEDTLS_MANIFEST_CERTIFICATE_NOT_P256 :
                     HFOC_MBEDTLS_MANIFEST_CERTIFICATE_NOT_EC;
        goto cleanup;
    }
    if (!hfoc_mbedtls_manifest_san_matches(&certificate)) {
        status =
            HFOC_MBEDTLS_MANIFEST_CERTIFICATE_HOSTNAME_MISMATCH;
        goto cleanup;
    }

    if (mbedtls_pk_parse_key(
            &key,
            &bytes[key_offset],
            key_length,
            NULL,
            0,
            NULL,
            NULL) != 0) {
        status = HFOC_MBEDTLS_MANIFEST_KEY_PARSE_FAILED;
        goto cleanup;
    }
    if (!hfoc_mbedtls_manifest_pk_is_p256(&key)) {
        status = mbedtls_pk_get_type(&key) == MBEDTLS_PK_ECKEY ||
                 mbedtls_pk_get_type(&key) == MBEDTLS_PK_ECDSA ?
                     HFOC_MBEDTLS_MANIFEST_KEY_NOT_P256 :
                     HFOC_MBEDTLS_MANIFEST_KEY_NOT_EC;
        goto cleanup;
    }

    certificate_public_length = mbedtls_pk_write_pubkey_der(
        &certificate.pk,
        certificate_public,
        sizeof(certificate_public));
    key_public_length = mbedtls_pk_write_pubkey_der(
        &key,
        key_public,
        sizeof(key_public));
    if (certificate_public_length <= 0 ||
        key_public_length <= 0 ||
        certificate_public_length != key_public_length ||
        memcmp(
            certificate_public +
                sizeof(certificate_public) -
                (size_t)certificate_public_length,
            key_public +
                sizeof(key_public) -
                (size_t)key_public_length,
            (size_t)certificate_public_length) != 0) {
        status = HFOC_MBEDTLS_MANIFEST_KEY_MISMATCH;
        goto cleanup;
    }

    memset(&result, 0, sizeof(result));
    result.published = true;
    result.version = HFOC_MBEDTLS_MANIFEST_VERSION;
    result.purpose_id = purpose_id;
    result.algorithm_id = algorithm_id;
    result.hostname_length = hostname_length;
    memcpy(
        result.hostname,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME,
        HFOC_MBEDTLS_MANIFEST_HOSTNAME_LENGTH);
    result.certificate = &bytes[certificate_offset];
    result.certificate_length = certificate_length;
    result.key = &bytes[key_offset];
    result.key_length = key_length;
    memcpy(
        result.certificate_digest,
        digest,
        HFOC_MBEDTLS_MANIFEST_SHA256_LENGTH);

cleanup:
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&certificate);
    if (status == HFOC_MBEDTLS_MANIFEST_OK) {
        *manifest = result;
    }
    return status;
}

#else

typedef int hfoc_mbedtls_manifest_production_empty;

#endif
