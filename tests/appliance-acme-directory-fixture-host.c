/*
 * ITEM-508 increment 14: host proof of the offline ACME stub's identity.
 *
 * The planned QEMU directory proof dials the host stub as the IPv4 literal
 * `10.0.2.2` and the frozen TLS client verifies the peer's name.  Before this
 * fixture existed, every chain the tree carried named only the DNS SAN
 * `hfoc-appliance`, so the handshake the proof depends on could not succeed -
 * the increment would have proven a refusal, not an exchange.  This proof
 * pins the fixture that closes that gap and pins the exact verification the
 * binding performs:
 *
 *   * the CA and the leaf are the bytes the generator recorded, checked by
 *     SHA-256, so the host stub and the guest anchor cannot drift apart;
 *   * the leaf verifies against its own CA for the literal `10.0.2.2` with a
 *     clean flag word - the same `cn` the binding hands to
 *     `mbedtls_ssl_set_hostname`, matched through the leaf's iPAddress SAN;
 *   * the same leaf is refused for a different literal and against an
 *     unrelated CA from the shared ITEM-503 fixtures, so the acceptance above
 *     is not "any chain verifies";
 *   * the leaf's private key parses and its public half is byte-identical to
 *     the certificate's, which is what lets the host stub serve this leaf;
 *   * the DNS SAN is still `hfoc-appliance`, so the same fixture can also
 *     serve the planned DNS-named variation without a second chain.
 *
 * No kernel module is linked here beyond mbedTLS: the proof is about the
 * bytes the proof will use, not about a decision the appliance owns.
 */

#include "appliance-certificate-fixtures.h"
#include "appliance-acme-directory-fixtures.h"
#include "hfoc_mbedtls_alloc.h"
#include "sha256.h"

#include "mbedtls/pk.h"
#include "mbedtls/x509_crt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
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

#define DIRECTORY_FIXTURE_CA_SHA256 \
    "3378d4a885b59df3248bfadd8fea0dbfa7407d0699e1645a89a14ee4548df0f8"
#define DIRECTORY_FIXTURE_LEAF_SHA256 \
    "03dd89be84805299fca33dbb6c5e7ba9c2b965b0cf15056e1188a2a122dc3c7f"

static void digest_to_text(
    const unsigned char *bytes,
    size_t length,
    char *text)
{
    static const char digits[] = "0123456789abcdef";
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t index;

    if (sha256_compute(bytes, length, digest) != SHA256_OK) {
        text[0] = '\0';
        return;
    }
    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        text[index * 2U] = digits[(digest[index] >> 4) & 0x0FU];
        text[index * 2U + 1U] = digits[digest[index] & 0x0FU];
    }
    text[SHA256_DIGEST_SIZE * 2U] = '\0';
}

static void test_material_is_pinned(void)
{
    char text[SHA256_DIGEST_SIZE * 2U + 1U];

    digest_to_text(
        directory_fixture_ca, sizeof(directory_fixture_ca), text);
    CHECK(strcmp(text, DIRECTORY_FIXTURE_CA_SHA256) == 0,
          "the fixture CA is the generator's bytes");
    digest_to_text(
        directory_fixture_leaf, sizeof(directory_fixture_leaf), text);
    CHECK(strcmp(text, DIRECTORY_FIXTURE_LEAF_SHA256) == 0,
          "the fixture leaf is the generator's bytes");
    CHECK(sizeof(directory_fixture_leaf_key) == 121U,
          "the fixture leaf key is one SEC1 P-256 key");
}

static bool verify(
    const unsigned char *chain,
    size_t chain_length,
    const unsigned char *anchor,
    size_t anchor_length,
    const char *name,
    uint32_t *flags_out)
{
    mbedtls_x509_crt leaf;
    mbedtls_x509_crt ca;
    uint32_t flags = 0U;
    int result;

    mbedtls_x509_crt_init(&leaf);
    mbedtls_x509_crt_init(&ca);
    if (mbedtls_x509_crt_parse_der(&ca, anchor, anchor_length) != 0 ||
        mbedtls_x509_crt_parse_der(&leaf, chain, chain_length) != 0) {
        mbedtls_x509_crt_free(&leaf);
        mbedtls_x509_crt_free(&ca);
        return false;
    }
    result = mbedtls_x509_crt_verify(
        &leaf, &ca, NULL, name, &flags, NULL, NULL);
    mbedtls_x509_crt_free(&leaf);
    mbedtls_x509_crt_free(&ca);
    if (flags_out != NULL) {
        *flags_out = flags;
    }
    return result == 0 && flags == 0U;
}

static void test_the_literal_name_verifies(void)
{
    uint32_t flags = UINT32_MAX;

    CHECK(verify(
              directory_fixture_leaf, sizeof(directory_fixture_leaf),
              directory_fixture_ca, sizeof(directory_fixture_ca),
              "10.0.2.2", &flags),
          "the leaf verifies for the literal the proof dials");
    CHECK(flags == 0U, "the literal acceptance carries no warning flag");
    CHECK(verify(
              directory_fixture_leaf, sizeof(directory_fixture_leaf),
              directory_fixture_ca, sizeof(directory_fixture_ca),
              "hfoc-appliance", &flags),
          "the same leaf verifies for its DNS name");
}

static void test_other_names_are_refused(void)
{
    uint32_t flags = 0U;

    CHECK(!verify(
              directory_fixture_leaf, sizeof(directory_fixture_leaf),
              directory_fixture_ca, sizeof(directory_fixture_ca),
              "10.0.2.3", &flags),
          "another literal does not verify against this leaf");
    CHECK((flags & MBEDTLS_X509_BADCERT_CN_MISMATCH) != 0U,
          "the refusal names the name mismatch");
    CHECK(!verify(
              directory_fixture_leaf, sizeof(directory_fixture_leaf),
              directory_fixture_ca, sizeof(directory_fixture_ca),
              "elsewhere.example", &flags),
          "another DNS name does not verify against this leaf");
}

static void test_another_anchor_is_refused(void)
{
    CHECK(!verify(
              directory_fixture_leaf, sizeof(directory_fixture_leaf),
              fixture_chain_unrelated, sizeof(fixture_chain_unrelated),
              "10.0.2.2", NULL),
          "an unrelated CA does not verify this leaf");
    CHECK(!verify(
              fixture_chained_certificate,
              sizeof(fixture_chained_certificate),
              directory_fixture_ca, sizeof(directory_fixture_ca),
              "hfoc-appliance", NULL),
          "this CA does not verify the shared fixture leaf");
}

static void test_the_leaf_key_matches_the_certificate(void)
{
    mbedtls_x509_crt leaf;
    mbedtls_pk_context key;
    unsigned char certificate_public[256];
    unsigned char key_public[256];
    size_t certificate_length;
    size_t key_length;
    bool parsed = false;

    mbedtls_x509_crt_init(&leaf);
    mbedtls_pk_init(&key);
    if (mbedtls_x509_crt_parse_der(
            &leaf, directory_fixture_leaf,
            sizeof(directory_fixture_leaf)) == 0 &&
        mbedtls_pk_parse_key(
            &key, directory_fixture_leaf_key,
            sizeof(directory_fixture_leaf_key), NULL, 0U, NULL, NULL) == 0) {
        parsed = true;
    }
    CHECK(parsed, "the leaf and its key both parse");
    if (parsed) {
        certificate_length = (size_t)mbedtls_pk_write_pubkey_der(
            &leaf.pk, certificate_public, sizeof(certificate_public));
        key_length = (size_t)mbedtls_pk_write_pubkey_der(
            &key, key_public, sizeof(key_public));
        CHECK(certificate_length != 0U && key_length != 0U &&
                  certificate_length == key_length &&
                  memcmp(
                      certificate_public + sizeof(certificate_public) -
                          certificate_length,
                      key_public + sizeof(key_public) - key_length,
                      certificate_length) == 0,
              "the key's public half is the certificate's public key");
    }
    mbedtls_pk_free(&key);
    mbedtls_x509_crt_free(&leaf);
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: the mbedTLS pools do not initialize\n");
        return 1;
    }
    test_material_is_pinned();
    test_the_literal_name_verifies();
    test_other_names_are_refused();
    test_another_anchor_is_refused();
    test_the_leaf_key_matches_the_certificate();

    if (failures != 0) {
        fprintf(stderr,
                "appliance acme directory fixture host test: %d failure(s)\n",
                failures);
        return 1;
    }
    hfoc_mbedtls_alloc_shutdown();
    printf("appliance acme directory fixture host test: OK\n");
    return 0;
}
