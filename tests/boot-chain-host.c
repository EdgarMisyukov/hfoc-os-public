#ifdef HFOC_BOOT_CHAIN_HOST_TEST

#include "boot-chain-vector.h"
#include "boot_chain.h"
#include "boot_measurements.h"
#include "image_signature.h"
#include "key_use.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "trust_anchor.h"
#include "verification_policy.h"

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

#define BOOT_CHAIN_TEST_CAPACITY 512U

static void boot_chain_host_components(
    unsigned char boot_payload[64],
    unsigned char loader_config[85],
    unsigned char kernel[256],
    struct boot_chain_component components[3])
{
    struct boot_chain_component vector[3];

    memcpy(boot_payload, boot_chain_vector_boot_payload, 64U);
    memcpy(loader_config, boot_chain_vector_loader_config, 85U);
    memcpy(kernel, boot_chain_vector_kernel, 256U);
    boot_chain_vector_components(vector);
    components[0].kind = vector[0].kind;
    components[0].data = boot_payload;
    components[0].length = vector[0].length;
    components[1].kind = vector[1].kind;
    components[1].data = loader_config;
    components[1].length = vector[1].length;
    components[2].kind = vector[2].kind;
    components[2].data = kernel;
    components[2].length = vector[2].length;
}

static void boot_chain_host_producer_and_verify(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    struct boot_chain_verify_report report;
    struct boot_measurements measurements;
    struct boot_chain_component components[3];
    unsigned char boot_payload[64];
    unsigned char loader_config[85];
    unsigned char kernel[256];
    unsigned char envelope[BOOT_CHAIN_TEST_CAPACITY];
    unsigned char digest[SHA256_DIGEST_SIZE];
    size_t envelope_length = 0U;

    boot_chain_host_components(boot_payload, loader_config, kernel,
                               components);

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK,
          "the declared trust set binds for the boot chain");
    CHECK(image_signature_policy_init(&store, &policy, 1U) ==
              IMAGE_SIGNATURE_OK,
          "the boot-chain policy is built from the trust set");

    CHECK(boot_chain_chain_digest(BOOT_CHAIN_VECTOR_RELEASE_ID,
                                  IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                                  components, 3U, digest) == BOOT_CHAIN_OK,
          "the producer computes the chain digest");
    CHECK(memcmp(digest, boot_chain_vector_chain_digest, SHA256_DIGEST_SIZE) == 0,
          "the kernel manifest format matches the signed vector");

    CHECK(boot_chain_manifest_seal(
              &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
              boot_chain_vector_dev_signature, sizeof(boot_chain_vector_dev_signature), envelope,
              sizeof(envelope), &envelope_length) == BOOT_CHAIN_OK,
          "the manifest seals into the signed envelope");
    CHECK(envelope_length > BOOT_CHAIN_MANIFEST_SIZE,
          "the sealed envelope carries the manifest and the envelope seal");

    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            envelope, envelope_length, components, 3U,
                            &report) == BOOT_CHAIN_OK,
          "the signed boot chain verifies over the actual bytes");
    CHECK(boot_chain_report_is_trusted(&report),
          "the verified report is trusted");
    CHECK(report.envelope_verified && report.signature_verified &&
              report.release_verified && report.components_verified,
          "every verification stage reports true");
    CHECK(report.release_id == BOOT_CHAIN_VECTOR_RELEASE_ID &&
              report.signer_id ==
                  IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID &&
              report.algorithm == TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
          "the report carries the signed release identity");
    CHECK(report.component_count == 3U &&
              report.components[0].kind ==
                  BOOT_CHAIN_COMPONENT_BOOT_PAYLOAD &&
              report.components[2].kind == BOOT_CHAIN_COMPONENT_KERNEL &&
              report.components[0].matched &&
              report.components[1].matched &&
              report.components[2].matched,
          "the report carries the ordered component table");
    CHECK(memcmp(report.chain_digest, boot_chain_vector_chain_digest,
                 SHA256_DIGEST_SIZE) == 0,
          "the report carries the signed chain digest");

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "the register set resets");
    CHECK(boot_chain_measure(&report, &measurements) == BOOT_CHAIN_OK,
          "a trusted report fills the register set");
    CHECK(boot_measurements_is_published(&measurements),
          "the register set publishes after the chain verified");
    CHECK(boot_measurements_log_count(&measurements) == 4U,
          "the chain and every component are logged");
    CHECK(boot_measurements_read(&measurements, BOOT_CHAIN_CHAIN_REGISTER,
                                 digest) == BOOT_MEASUREMENTS_OK,
          "the chain register reads back");
    CHECK(boot_measurements_read(&measurements,
                                 BOOT_CHAIN_COMPONENT_KERNEL,
                                 digest) == BOOT_MEASUREMENTS_OK,
          "the kernel register reads back");
    CHECK(boot_measurements_aggregate(&measurements, digest) ==
              BOOT_MEASUREMENTS_OK,
          "the measurement aggregate reads back");

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
              boot_chain_measure(&report, &measurements) == BOOT_CHAIN_OK &&
              boot_measurements_aggregate(&measurements, digest) ==
                  BOOT_MEASUREMENTS_OK,
          "measuring the same chain republishes the aggregate");
    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "the register set resets before the tamper case");
    components[2].length = 255U;
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            envelope, envelope_length, components, 3U,
                            &report) ==
              BOOT_CHAIN_COMPONENT_LENGTH_MISMATCH,
          "a truncated component is refused");
    CHECK(!boot_chain_report_is_trusted(&report),
          "a refused chain leaves no trusted report");
    CHECK(boot_chain_measure(&report, &measurements) ==
              BOOT_CHAIN_MANIFEST_INVALID,
          "an untrusted report cannot fill the register set");
    CHECK(!boot_measurements_is_published(&measurements),
          "a refused chain publishes no measurements");
    components[2].length = 256U;

    kernel[0] ^= 0x01U;
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            envelope, envelope_length, components, 3U,
                            &report) ==
              BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH,
          "a tampered component is refused");
    CHECK(!report.envelope_verified || !report.signature_verified ||
              !report.components_verified,
          "the refused report does not claim verified components");
    kernel[0] ^= 0x01U;

    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID + 1U,
                            envelope, envelope_length, components, 3U,
                            &report) == BOOT_CHAIN_RELEASE_MISMATCH,
          "a foreign release identity is refused");
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            envelope, envelope_length, components, 2U,
                            &report) == BOOT_CHAIN_COMPONENT_MISSING,
          "a missing component is refused");
    {
        struct boot_chain_component extra[BOOT_CHAIN_COMPONENT_CAPACITY + 1U];

        memcpy(extra, components, sizeof(components));
        extra[3] = components[2];
        CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                                envelope, envelope_length, extra,
                                BOOT_CHAIN_COMPONENT_CAPACITY + 1U,
                                &report) == BOOT_CHAIN_COMPONENT_LIMIT,
              "a component count beyond the signed manifest is refused");
    }
}

static void boot_chain_host_rejections(void)
{
    struct image_signature_store store;
    struct image_signature_store release_only;
    struct verification_policy policy;
    struct verification_policy release_policy;
    struct boot_chain_verify_report report;
    struct boot_chain_component components[3];
    unsigned char boot_payload[64];
    unsigned char loader_config[85];
    unsigned char kernel[256];
    unsigned char envelope[BOOT_CHAIN_TEST_CAPACITY];
    unsigned char tampered[BOOT_CHAIN_TEST_CAPACITY];
    size_t envelope_length = 0U;

    boot_chain_host_components(boot_payload, loader_config, kernel,
                               components);
    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK &&
              image_signature_store_declared(&release_only, true) ==
                  IMAGE_SIGNATURE_OK,
          "both trust sets bind for the rejection matrix");
    CHECK(image_signature_policy_init(&store, &policy, 1U) ==
              IMAGE_SIGNATURE_OK,
          "the rejection policy builds");
    CHECK(image_signature_policy_init(&release_only, &release_policy, 1U) ==
              IMAGE_SIGNATURE_OK,
          "the release-only policy builds");

    CHECK(boot_chain_manifest_seal(
              &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
              boot_chain_vector_dev_signature, sizeof(boot_chain_vector_dev_signature), envelope,
              sizeof(envelope), &envelope_length) == BOOT_CHAIN_OK,
          "the rejection envelope seals");

    CHECK(boot_chain_verify(NULL, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                            envelope_length, components, 3U, &report) ==
              BOOT_CHAIN_NULL,
          "a null policy is refused");
    CHECK(boot_chain_verify(&policy, NULL, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                            envelope_length, components, 3U, &report) ==
              BOOT_CHAIN_NULL,
          "a null trust set is refused");
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, NULL,
                            envelope_length, components, 3U, &report) ==
              BOOT_CHAIN_NULL,
          "a null envelope is refused");
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            envelope, envelope_length, components, 3U,
                            NULL) == BOOT_CHAIN_NULL,
          "a null report is refused");

    memcpy(tampered, envelope, envelope_length);
    tampered[envelope_length - 1U] ^= 0x01U;
    CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                            tampered, envelope_length, components, 3U,
                            &report) == BOOT_CHAIN_ENVELOPE_INVALID,
          "a tampered envelope seal is refused");

    {
        struct image_signature_store fixture_store;
        struct verification_policy fixture_policy;
        struct image_signature_anchor fixture_anchor;
        unsigned char fixture_envelope[BOOT_CHAIN_TEST_CAPACITY];
        size_t fixture_length = 0U;

        memset(&fixture_anchor, 0, sizeof(fixture_anchor));
        fixture_anchor.signer_id = BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID;
        fixture_anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
        fixture_anchor.accepted_purposes =
            KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
        memcpy(fixture_anchor.public_key, boot_chain_vector_fixture_public_key,
               sizeof(fixture_anchor.public_key));
        CHECK(image_signature_store_init(&fixture_store, false) ==
                  IMAGE_SIGNATURE_OK &&
                  image_signature_store_add(&fixture_store,
                                            &fixture_anchor) ==
                      IMAGE_SIGNATURE_OK &&
                  image_signature_policy_init(&fixture_store,
                                              &fixture_policy, 1U) ==
                      IMAGE_SIGNATURE_OK,
              "the fixture trust set binds");

        CHECK(boot_chain_manifest_seal(
                  &fixture_policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
                  BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID,
                  TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
                  boot_chain_vector_fixture_signature,
                  sizeof(boot_chain_vector_fixture_signature), fixture_envelope,
                  sizeof(fixture_envelope), &fixture_length) ==
                  BOOT_CHAIN_OK,
              "a fixture-signed manifest seals");
        CHECK(boot_chain_verify(&fixture_policy, &fixture_store,
                                BOOT_CHAIN_VECTOR_RELEASE_ID, fixture_envelope,
                                fixture_length, components, 3U,
                                &report) == BOOT_CHAIN_OK &&
                  boot_chain_report_is_trusted(&report),
              "the fixture-signed chain verifies under its own anchor");
        CHECK(boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                                fixture_envelope, fixture_length,
                                components, 3U, &report) ==
                  BOOT_CHAIN_ENVELOPE_INVALID,
              "an envelope signed by an untrusted signer is refused");
        CHECK(boot_chain_verify(&release_policy, &release_only,
                                BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                                envelope_length, components, 3U,
                                &report) == BOOT_CHAIN_ENVELOPE_INVALID,
              "a release-only policy refuses the development signer");

        /* Re-seal the manifest body with a tampered signature, so the
         * envelope seal is valid and the asymmetric check is what fails. */
        {
            unsigned char bad_signature[BOOT_CHAIN_SIGNATURE_SIZE];

            memcpy(bad_signature, boot_chain_vector_fixture_signature,
                   sizeof(bad_signature));
            bad_signature[3] ^= 0x40U;
            CHECK(boot_chain_manifest_seal(
                      &fixture_policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
                  BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID,
                      TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
                      bad_signature, sizeof(bad_signature),
                      fixture_envelope, sizeof(fixture_envelope),
                      &fixture_length) == BOOT_CHAIN_OK,
                  "a manifest with a tampered signature still seals");
            CHECK(boot_chain_verify(&fixture_policy, &fixture_store,
                                    BOOT_CHAIN_VECTOR_RELEASE_ID,
                                    fixture_envelope, fixture_length,
                                    components, 3U, &report) ==
                      BOOT_CHAIN_SIGNATURE_MISMATCH,
                  "the asymmetric check refuses a tampered signature");
            CHECK(!report.signature_verified,
                  "the refused report does not claim a verified signature");
        }
    }

    {
        unsigned char short_envelope[BOOT_CHAIN_TEST_CAPACITY];
        size_t short_length = 0U;

        CHECK(boot_chain_manifest_seal(
                  &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
                  IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                  TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
                  boot_chain_vector_dev_signature, sizeof(boot_chain_vector_dev_signature),
                  short_envelope, BOOT_CHAIN_MANIFEST_SIZE - 1U,
                  &short_length) == BOOT_CHAIN_CAPACITY,
              "an undersized producer buffer is refused");
        CHECK(short_length == 0U,
              "a refused seal publishes no length");
    }

    {
        struct boot_chain_component bad[1];

        bad[0].kind = 9U;
        bad[0].data = boot_payload;
        bad[0].length = sizeof(boot_payload);
        CHECK(boot_chain_chain_digest(BOOT_CHAIN_VECTOR_RELEASE_ID,
                                      IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                                      bad, 1U,
                                      report.chain_digest) ==
                  BOOT_CHAIN_MANIFEST_INVALID,
              "an unknown component kind is refused by the producer");
        bad[0].kind = BOOT_CHAIN_COMPONENT_KERNEL;
        bad[0].length = 0U;
        CHECK(boot_chain_chain_digest(BOOT_CHAIN_VECTOR_RELEASE_ID,
                                      IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                                      bad, 1U,
                                      report.chain_digest) ==
                  BOOT_CHAIN_MANIFEST_INVALID,
              "an empty component is refused by the producer");
        CHECK(boot_chain_chain_digest(BOOT_CHAIN_VECTOR_RELEASE_ID,
                                      IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                                      components,
                                      BOOT_CHAIN_COMPONENT_CAPACITY + 1U,
                                      report.chain_digest) ==
                  BOOT_CHAIN_COMPONENT_LIMIT,
              "a producer component count beyond the manifest is refused");
    }

    CHECK(strcmp(boot_chain_status_string(BOOT_CHAIN_OK), "OK") == 0 &&
              strcmp(boot_chain_status_string(
                         BOOT_CHAIN_SIGNATURE_MISMATCH),
                     "SIGNATURE_MISMATCH") == 0,
          "status strings are exact");
    CHECK(strcmp(boot_chain_component_kind_string(
                     BOOT_CHAIN_COMPONENT_KERNEL), "KERNEL") == 0 &&
              strcmp(boot_chain_component_kind_string(9U), "UNKNOWN") == 0,
          "component kind strings are exact");
}

int main(void)
{
    boot_chain_host_producer_and_verify();
    boot_chain_host_rejections();

    if (failures != 0) {
        fprintf(stderr, "Boot chain host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("Boot chain host tests: ALL OK (%d checks)\n", checks);
    return 0;
}

#else
int boot_chain_host_translation_unit_marker;
#endif
