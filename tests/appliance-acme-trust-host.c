/*
 * ITEM-508 increment 2: host proof of the ACME client role's trust decision.
 *
 * The proof drives every refusal path the module has without a network and
 * without the real ACME server: a fixture CA anchors a chained leaf, and the
 * shared ITEM-503 fixtures supply the wrong issuer, the wrong name and the
 * time windows.  The image's own anchor - ISRG Root X1 - is checked to be the
 * DER this build pinned by digest, and - since ITEM-508 increment 33 enabled
 * RSA verification at the owner's 2026-10-06 decision - it verifies the real
 * Let's Encrypt certificates that anchor really signed, embedded as DER
 * fixtures from the project's own certificate page.
 */

#include "appliance_acme_trust.h"

#include "appliance-acme-le-chain-fixtures.h"
#include "appliance-certificate-fixtures.h"
#include "hfoc_mbedtls_alloc.h"
#include "sha256.h"

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

static const char isrg_root_x1_sha256[] =
    "96bcec06264976f37460779acf28c5a7cfe8a3c0aae11a8ffcee05c0bddf08c6";

static void check_status(
    struct appliance_acme_trust *trust,
    enum appliance_acme_trust_status expected,
    const char *message)
{
    const enum appliance_acme_trust_status status =
        appliance_acme_trust_verify(
            trust, fixture_chained_certificate,
            sizeof(fixture_chained_certificate), "hfoc-appliance", 14U,
            FIXTURE_NOW);

    CHECK(status == expected, message);
    CHECK(trust->last_status == expected, "the refusal reason is published");
}

static void test_anchor_is_named_and_pinned(void)
{
    const unsigned char *anchor = appliance_acme_trust_anchor_der_data();
    const size_t length = appliance_acme_trust_anchor_der_length();
    unsigned char digest[SHA256_DIGEST_SIZE];
    char text[SHA256_DIGEST_SIZE * 2U + 1U];
    char byte[3U];
    size_t index;

    CHECK(anchor != NULL && length != 0U,
          "the image carries one named anchor");
    CHECK(sha256_compute(anchor, length, digest) == SHA256_OK,
          "the anchor's digest is computable");
    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        static const char digits[] = "0123456789abcdef";

        byte[0] = digits[(digest[index] >> 4) & 0x0FU];
        byte[1] = digits[digest[index] & 0x0FU];
        byte[2] = '\0';
        memcpy(&text[index * 2U], byte, 2U);
    }
    text[sizeof(text) - 1U] = '\0';
    CHECK(strcmp(text, isrg_root_x1_sha256) == 0,
          "the anchor is ISRG Root X1, byte for byte");
}

static void test_valid_chain_and_name_are_accepted(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "a fixture anchor binds");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_OK,
          "a chain to the named anchor for the presented name is accepted");
    CHECK(trust.verify_count == 1U && trust.refusal_count == 0U,
          "an accepted chain is counted, not refused");
}

static void test_wrong_anchor_is_refused(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_unrelated,
              sizeof(fixture_chain_unrelated)) == APPLIANCE_ACME_TRUST_OK,
          "the unrelated issuer binds as an anchor");
    check_status(&trust, APPLIANCE_ACME_TRUST_CHAIN_REJECTED,
                 "a chain to another root is refused");
    CHECK(trust.refusal_count == 1U && trust.verify_count == 0U,
          "the refusal is counted and no verification is claimed");
}

static void test_self_signed_leaf_is_refused(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the self-signed case");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_certificate, sizeof(fixture_certificate),
              "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_CHAIN_REJECTED,
          "a self-signed leaf is not trusted by a named anchor");
}

static void test_wrong_name_is_refused(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the wrong-name case");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), "absent.example", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_NAME_REJECTED,
          "a chain that verifies for another name is refused by name");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_multi_san_certificate,
              sizeof(fixture_multi_san_certificate), "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_OK,
          "one name of a multi-SAN certificate is enough");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_multi_san_certificate,
              sizeof(fixture_multi_san_certificate), "absent.example", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_NAME_REJECTED,
          "a name the certificate does not carry is refused by name");
}

static void test_unvalidated_clock_starts_nothing(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the clock case");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), "hfoc-appliance", 14U,
              0U) == APPLIANCE_ACME_TRUST_BAD_CLOCK,
          "an unvalidated clock refuses before the chain is read");
    CHECK(trust.verify_count == 0U,
          "the unvalidated clock starts no verification");
}

static void test_windows_are_walked(void)
{
    struct appliance_acme_trust trust;

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the window cases");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), "hfoc-appliance", 14U,
              FIXTURE_BEFORE) == APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED,
          "a chain that is not yet valid is refused by its window");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate), "hfoc-appliance", 14U,
              FIXTURE_AFTER) == APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED,
          "an expired chain is refused by its window");
}

static void test_malformed_inputs_are_refused(void)
{
    struct appliance_acme_trust trust;
    static const char *const bad_names[] = {
        "", "UPPER.example", "-leading.example", "trailing-.example",
        "two..dots", "trailing.example.", "wild*.example"
    };

    CHECK(appliance_acme_trust_init(
              &trust, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK,
          "the fixture anchor binds for the malformed cases");
    for (size_t index = 0U;
         index < sizeof(bad_names) / sizeof(bad_names[0]); ++index) {
        const char *name = bad_names[index];

        CHECK(appliance_acme_trust_verify(
                  &trust, fixture_chained_certificate,
                  sizeof(fixture_chained_certificate), name, strlen(name),
                  FIXTURE_NOW) == APPLIANCE_ACME_TRUST_BAD_NAME,
              "a name that is not an exact DNS name is refused by name");
    }
    CHECK(appliance_acme_trust_verify(
              &trust, NULL, 0U, "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_NULL,
          "a missing chain is a caller error");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate, 0U, "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED,
          "an empty chain is refused");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate, 100U, "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED,
          "a truncated chain is refused");
    CHECK(appliance_acme_trust_verify(
              &trust, fixture_chained_certificate,
              sizeof(fixture_chained_certificate) + 1U, "hfoc-appliance", 14U,
              FIXTURE_NOW) == APPLIANCE_ACME_TRUST_CHAIN_PARSE_FAILED,
          "a chain with a trailing byte is refused");
}

/*
 * ITEM-508 increment 33: the image's own anchor verifies the real Let's
 * Encrypt chain.  ISRG Root X1 is an RSA certificate and R10/R11 are RSA
 * intermediates; the owner's 2026-10-06 decision enabled RSA verification for
 * exactly this chain, so the anchor parses, the real certificates chain to it
 * with zero verification flags, and every pre-existing refusal still holds.
 * Before this increment the same call answered `ANCHOR_UNAVAILABLE`, which is
 * the finding this test replaces.
 */
static void test_the_image_anchor_verifies_the_real_chain(void)
{
    struct appliance_acme_trust trust;
    struct appliance_acme_trust second;

    CHECK(appliance_acme_trust_init(
              &trust, appliance_acme_trust_anchor_der_data(),
              appliance_acme_trust_anchor_der_length()) ==
              APPLIANCE_ACME_TRUST_OK,
          "the image anchor binds");
    CHECK(appliance_acme_trust_verify(
              &trust, letsencrypt_r10_der, sizeof(letsencrypt_r10_der),
              "r10", 3U, LETSENCRYPT_CHAIN_NOW) == APPLIANCE_ACME_TRUST_OK,
          "the real R10 chains to the image anchor with zero flags");
    CHECK(trust.verify_count == 1U && trust.refusal_count == 0U &&
              trust.last_flags == 0U,
          "the verified real chain is counted with no flags");
    /*
     * ITEM-508 increment 35: the verified leaf publishes its own `notAfter`,
     * which is the one expiry the renewal policy may be moved to.  R10's
     * certificate expires on 2027-03-12 23:59:59 UTC.
     */
    CHECK(appliance_acme_trust_leaf_not_after(&trust) ==
              UINT32_C(1804895999),
          "the verified leaf publishes its own notAfter");
    CHECK(appliance_acme_trust_verify(
              &trust, letsencrypt_r11_der, sizeof(letsencrypt_r11_der),
              "r11", 3U, LETSENCRYPT_CHAIN_NOW) == APPLIANCE_ACME_TRUST_OK,
          "the real R11 chains to the image anchor with zero flags");
    /*
     * The refusals did not move: the real certificate with a name the anchor
     * does not cover is refused by the name check, and outside its own window
     * it is refused by the window check - never silently accepted because the
     * signature verified.
     */
    CHECK(appliance_acme_trust_verify(
              &trust, letsencrypt_r10_der, sizeof(letsencrypt_r10_der),
              "hfoc-appliance", 14U,
              LETSENCRYPT_CHAIN_NOW) == APPLIANCE_ACME_TRUST_NAME_REJECTED,
          "the real chain with a name it does not cover is refused by name");
    CHECK(appliance_acme_trust_leaf_not_after(&trust) == 0U,
          "a refused chain publishes no leaf expiry");
    CHECK(appliance_acme_trust_verify(
              &trust, letsencrypt_r10_der, sizeof(letsencrypt_r10_der),
              "r10", 3U, FIXTURE_BEFORE) ==
              APPLIANCE_ACME_TRUST_CHAIN_WINDOW_REJECTED,
          "the real chain outside its window is refused by window");
    CHECK(appliance_acme_trust_init(
              &second, fixture_chain_ca,
              sizeof(fixture_chain_ca)) == APPLIANCE_ACME_TRUST_OK &&
              appliance_acme_trust_verify(
                  &second, letsencrypt_r10_der, sizeof(letsencrypt_r10_der),
                  "r10", 3U, LETSENCRYPT_CHAIN_NOW) ==
                  APPLIANCE_ACME_TRUST_CHAIN_REJECTED,
          "the real chain under a foreign anchor is rejected, not trusted");
    CHECK(appliance_acme_trust_leaf_not_after(&second) == 0U,
          "a foreign anchor publishes no leaf expiry");
}

int main(void)
{
    if (!hfoc_mbedtls_alloc_init()) {
        fprintf(stderr, "FAIL: the mbedTLS pools do not initialize\n");
        return 1;
    }
    test_anchor_is_named_and_pinned();
    test_valid_chain_and_name_are_accepted();
    test_wrong_anchor_is_refused();
    test_self_signed_leaf_is_refused();
    test_wrong_name_is_refused();
    test_unvalidated_clock_starts_nothing();
    test_windows_are_walked();
    test_malformed_inputs_are_refused();
    test_the_image_anchor_verifies_the_real_chain();

    if (failures != 0) {
        fprintf(stderr, "appliance acme trust host test: %d failure(s)\n",
                failures);
        return 1;
    }
    hfoc_mbedtls_alloc_shutdown();
    printf("appliance acme trust host test: OK\n");
    return 0;
}
