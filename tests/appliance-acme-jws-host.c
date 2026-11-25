/*
 * ITEM-508 increment 4: host proof of the request envelope.
 *
 * The proof binds the shared ITEM-503 P-256 fixture key, checks its public
 * JWK and its RFC 7638 thumbprint against values computed independently
 * (OpenSSL for the point, Python's json/hashlib for the canonical form), then
 * signs a protected header and a payload and requires the signature to verify
 * over `protected.payload` - both with mbedTLS and through the raw R || S
 * conversion the JWS carries.  It also drives base64url's refusals.  There is
 * no network and no ACME server.
 *
 * ITEM-508 increment 22 adds the PKCS#10 writer: the same key writes a CSR
 * whose DER has the PKCS#10 shape, carries the ordered common name, and is
 * refused for an uninitialized key, an empty or over-long subject and a
 * destination below the module's bound.  `--write-csr <path>` writes that
 * request to a file so the build can hand it to `openssl req -verify`, which
 * verifies its self-signature independently of this proof.
 */

#include "appliance_acme_jws.h"

#include "appliance-certificate-fixtures.h"
#include "hfoc_mbedtls_alloc.h"

#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

/* Independently computed: OpenSSL printed the point, Python the canonical
 * JSON and its SHA-256. */
static const char fixture_chained_thumbprint[] =
    "3ysHIvd-KKLESSZogF4Qw7dQqZjtIvjQ36NVdQDqe_o";
static const char fixture_chained_jwk[] =
    "{\"crv\":\"P-256\",\"kty\":\"EC\","
    "\"x\":\"ZT7RQFVpc20HsRgb9Hg7HVG8xdmbcoltJ_daerR32Q0\","
    "\"y\":\"HzG4hNy_wDqhuJSb2hbcVOhaUqEz-iOz-5b2PzfVoXM\"}";

/*
 * Rebuild the DER mbedTLS verifies from the raw R || S the JWS carries.  That
 * the conversion is exactly reversible is part of the proof: the envelope must
 * carry raw ES256, not the DER the library writes.
 */
static size_t raw_signature_to_der(
    const unsigned char raw[64U],
    unsigned char der[80U])
{
    unsigned char body[72U];
    size_t body_length = 0U;

    for (size_t half = 0U; half < 2U; ++half) {
        const unsigned char *value = &raw[half * 32U];
        size_t length = 32U;
        size_t offset = 0U;

        while (length > 1U && value[offset] == 0x00U) {
            offset += 1U;
            length -= 1U;
        }
        body[body_length++] = 0x02U;
        if ((value[offset] & 0x80U) != 0U) {
            body[body_length++] = (unsigned char)(length + 1U);
            body[body_length++] = 0x00U;
        } else {
            body[body_length++] = (unsigned char)length;
        }
        memcpy(&body[body_length], &value[offset], length);
        body_length += length;
    }
    der[0] = 0x30U;
    der[1] = (unsigned char)body_length;
    memcpy(&der[2], body, body_length);
    return body_length + 2U;
}

static void test_key_and_encodings(void)
{
    struct appliance_acme_key key;
    unsigned char decoded[APPLIANCE_ACME_JWS_MAX];
    size_t decoded_length = 0U;
    char encoded[APPLIANCE_ACME_JWS_MAX];
    size_t encoded_length = 0U;
    static const unsigned char round_trip[] = { 0x00U, 0x01U, 0xFEU, 0xFFU };

    CHECK(appliance_acme_key_import(
              &key, fixture_chained_key,
              sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK,
          "the shared fixture P-256 key imports");
    CHECK(strcmp(appliance_acme_key_thumbprint(&key),
                 fixture_chained_thumbprint) == 0,
          "the RFC 7638 thumbprint matches the independent value");
    CHECK(strcmp(appliance_acme_key_jwk(&key), fixture_chained_jwk) == 0,
          "the public JWK is the canonical form");

    CHECK(appliance_acme_base64url_encode(
              round_trip, sizeof(round_trip), encoded, sizeof(encoded),
              &encoded_length) == APPLIANCE_ACME_BASE64_OK &&
              strcmp(encoded, "AAH-_w") == 0,
          "base64url uses its own alphabet and no padding");
    CHECK(appliance_acme_base64url_decode(
              encoded, encoded_length, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_OK &&
              decoded_length == sizeof(round_trip) &&
              memcmp(decoded, round_trip, sizeof(round_trip)) == 0,
          "base64url round-trips");
    CHECK(appliance_acme_base64url_decode(
              "a+b/", 4U, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_BAD_INPUT,
          "the standard alphabet is refused");
    CHECK(appliance_acme_base64url_decode(
              "abcd=", 5U, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_BAD_INPUT,
          "padding is refused");
    CHECK(appliance_acme_base64url_decode(
              "a", 1U, decoded, sizeof(decoded),
              &decoded_length) == APPLIANCE_ACME_BASE64_BAD_INPUT,
          "a one-character tail is refused");
}

static void test_jws_signature(void)
{
    static const char protected_json[] =
        "{\"alg\":\"ES256\",\"nonce\":\"abc\","
        "\"url\":\"https://acme.example/new-order\"}";
    static const unsigned char payload[] =
        "{\"identifiers\":[{\"type\":\"dns\",\"value\":\"example.test\"}]}";
    struct appliance_acme_key key;
    char jws[APPLIANCE_ACME_JWS_MAX];
    char protected_b64[APPLIANCE_ACME_JWS_MAX];
    char payload_b64[APPLIANCE_ACME_JWS_MAX];
    char signature_b64[APPLIANCE_ACME_JWS_MAX];
    unsigned char signature[APPLIANCE_ACME_SIGNATURE_BYTES];
    unsigned char signing_input[APPLIANCE_ACME_JWS_MAX];
    unsigned char digest[32U];
    size_t jws_length = 0U;
    size_t protected_length = 0U;
    size_t payload_length = 0U;
    size_t signature_length = 0U;
    size_t signing_length = 0U;
    size_t decoded_length = 0U;

    CHECK(appliance_acme_key_import(
              &key, fixture_chained_key,
              sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK,
          "the fixture key binds for signing");
    CHECK(appliance_acme_jws_sign(
              &key, protected_json, strlen(protected_json), payload,
              sizeof(payload) - 1U, fixture_rng, NULL, jws, sizeof(jws),
              &jws_length) == APPLIANCE_ACME_JWS_OK,
          "a flattened JWS is produced");
    CHECK(strncmp(jws, "{\"protected\":\"", 14U) == 0 &&
              jws[jws_length - 2U] == '"' && jws[jws_length - 1U] == '}',
          "the JWS has the flattened JSON shape");
    CHECK(appliance_acme_base64url_encode(
              (const unsigned char *)protected_json, strlen(protected_json),
              protected_b64, sizeof(protected_b64),
              &protected_length) == APPLIANCE_ACME_BASE64_OK &&
              appliance_acme_base64url_encode(
                  payload, sizeof(payload) - 1U, payload_b64,
                  sizeof(payload_b64), &payload_length) ==
                  APPLIANCE_ACME_BASE64_OK,
          "the proof re-encodes the signing input");
    memcpy(signing_input, protected_b64, protected_length);
    signing_input[protected_length] = (unsigned char)'.';
    memcpy(&signing_input[protected_length + 1U], payload_b64,
           payload_length);
    signing_length = protected_length + 1U + payload_length;
    CHECK(mbedtls_sha256(signing_input, signing_length, digest, 0) == 0,
          "the signing input hashes");
    {
        const char *marker = "\",\"signature\":\"";
        const char *start = strstr(jws, marker);
        const char *end =
            start == NULL ? NULL : strchr(start + strlen(marker), '"');

        CHECK(start != NULL && end != NULL,
              "the JWS carries a signature member");
        if (start != NULL && end != NULL) {
            signature_length = (size_t)(end - (start + strlen(marker)));
            memcpy(signature_b64, start + strlen(marker), signature_length);
            signature_b64[signature_length] = '\0';
        }
    }
    CHECK(signature_length != 0U &&
              appliance_acme_base64url_decode(
                  signature_b64, signature_length, signature,
                  sizeof(signature),
                  &decoded_length) == APPLIANCE_ACME_BASE64_OK,
          "the signature decodes");
    CHECK(decoded_length == APPLIANCE_ACME_SIGNATURE_BYTES,
          "the ES256 signature is the raw 64-byte R || S");
    {
        mbedtls_pk_context context;
        unsigned char der_signature[80U];
        const size_t der_length =
            raw_signature_to_der(signature, der_signature);

        mbedtls_pk_init(&context);
        CHECK(mbedtls_pk_parse_key(
                  &context, fixture_chained_key,
                  sizeof(fixture_chained_key), NULL, 0U, fixture_rng,
                  NULL) == 0,
              "the fixture key parses for verification");
        CHECK(mbedtls_pk_verify(
                  &context, MBEDTLS_MD_SHA256, digest, sizeof(digest),
                  der_signature, der_length) == 0,
              "the signature verifies over protected.payload");
        mbedtls_pk_free(&context);
    }
}

/*
 * ITEM-508 increment 22: the PKCS#10 the finalize carries.  The DER is
 * checked for the shape a CSR must have (SEQUENCE, then INTEGER version 0,
 * the exact declared length and the subject's common name), while the
 * signature itself is verified independently by `openssl req -verify` on the
 * file `--write-csr` produces.
 */
static size_t csr_der_content_length(const unsigned char *der, size_t length)
{
    size_t header;
    size_t content;

    if (length < 2U || der[0] != 0x30U) {
        return 0U;
    }
    if ((der[1] & 0x80U) == 0U) {
        content = der[1];
        header = 2U;
    } else {
        const size_t octets = der[1] & 0x7FU;

        if (octets == 0U || octets > 4U || length < 2U + octets) {
            return 0U;
        }
        content = 0U;
        for (size_t index = 0U; index < octets; ++index) {
            content = (content << 8U) | der[2U + index];
        }
        header = 2U + octets;
    }
    return header + content == length ? content : 0U;
}

static bool csr_contains_text(
    const unsigned char *der,
    size_t der_length,
    const char *text)
{
    const size_t text_length = strlen(text);

    if (text_length == 0U || der_length < text_length) {
        return false;
    }
    for (size_t offset = 0U; offset + text_length <= der_length; ++offset) {
        if (memcmp(&der[offset], text, text_length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_csr_writer(void)
{
    struct appliance_acme_key key;
    struct appliance_acme_key unbound;
    unsigned char der[APPLIANCE_ACME_CSR_MAX];
    size_t der_length = 0U;
    char over_long[APPLIANCE_ACME_CSR_CN_MAX + 2U];

    memset(&unbound, 0, sizeof(unbound));
    memset(over_long, (int)'a', sizeof(over_long) - 1U);
    over_long[sizeof(over_long) - 1U] = '\0';

    CHECK(appliance_acme_key_write_csr(
              &unbound, "hfoc-appliance", fixture_rng, NULL, der,
              sizeof(der), &der_length) == APPLIANCE_ACME_JWS_BAD_KEY,
          "an unbound key cannot write a CSR");

    CHECK(appliance_acme_key_import(
              &key, fixture_chained_key,
              sizeof(fixture_chained_key)) == APPLIANCE_ACME_JWS_OK,
          "the fixture key binds for the CSR");
    CHECK(appliance_acme_key_write_csr(
              &key, "hfoc-appliance", fixture_rng, NULL, der, sizeof(der),
              &der_length) == APPLIANCE_ACME_JWS_OK &&
              der_length > 100U &&
              csr_der_content_length(der, der_length) != 0U,
          "the CSR is a bounded DER SEQUENCE");
    CHECK(csr_contains_text(der, der_length, "hfoc-appliance"),
          "the CSR subject carries the ordered common name");
    CHECK(appliance_acme_key_write_csr(
              &key, "", fixture_rng, NULL, der, sizeof(der),
              &der_length) == APPLIANCE_ACME_JWS_BAD_INPUT,
          "an empty subject is refused");
    CHECK(appliance_acme_key_write_csr(
              &key, "hfoc,appliance", fixture_rng, NULL, der, sizeof(der),
              &der_length) == APPLIANCE_ACME_JWS_BAD_INPUT,
          "a subject that could rewrite the name is refused");
    CHECK(appliance_acme_key_write_csr(
              &key, over_long, fixture_rng, NULL, der, sizeof(der),
              &der_length) == APPLIANCE_ACME_JWS_BAD_INPUT,
          "a subject over the CN bound is refused");
    CHECK(appliance_acme_key_write_csr(
              &key, "hfoc-appliance", fixture_rng, NULL, der, 64U,
              &der_length) == APPLIANCE_ACME_JWS_BUFFER_TOO_SMALL,
          "a destination below the bound is refused before writing");
}

static int write_csr_request(
    const char *path)
{
    struct appliance_acme_key key;
    unsigned char der[APPLIANCE_ACME_CSR_MAX];
    size_t der_length = 0U;
    FILE *handle;

    if (appliance_acme_key_import(
            &key, fixture_chained_key,
            sizeof(fixture_chained_key)) != APPLIANCE_ACME_JWS_OK ||
        appliance_acme_key_write_csr(
            &key, "hfoc-appliance", fixture_rng, NULL, der, sizeof(der),
            &der_length) != APPLIANCE_ACME_JWS_OK) {
        fprintf(stderr, "FAIL: the fixture CSR is not written\n");
        return 1;
    }
    handle = fopen(path, "wb");
    if (handle == NULL) {
        fprintf(stderr, "FAIL: cannot open %s\n", path);
        return 1;
    }
    if (fwrite(der, 1U, der_length, handle) != der_length) {
        fprintf(stderr, "FAIL: cannot write %s\n", path);
        fclose(handle);
        return 1;
    }
    fclose(handle);
    printf("wrote the fixture CSR (%zu bytes) to %s\n", der_length, path);
    return 0;
}

int main(int argc, char **argv)
{
    int status;

    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: the mbedTLS pools do not initialize\n");
        return 1;
    }
    if (argc == 3 && strcmp(argv[1], "--write-csr") == 0) {
        status = write_csr_request(argv[2]);
    } else if (argc == 1) {
        test_key_and_encodings();
        test_jws_signature();
        test_csr_writer();
        status = failures == 0 ? 0 : 1;
        if (failures != 0) {
            fprintf(stderr, "appliance acme jws host test: %d failure(s)\n",
                    failures);
        } else {
            printf("appliance acme jws host test: OK\n");
        }
    } else {
        fprintf(stderr, "usage: %s [--write-csr <path>]\n", argv[0]);
        status = 1;
    }
    appliance_acme_key_free(NULL);
    hfoc_mbedtls_alloc_shutdown();
    return status;
}
