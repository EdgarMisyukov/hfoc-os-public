#ifdef HFOC_BOOT_ATTESTATION_HOST_TEST

#include "boot-chain-vector.h"
#include "boot_attestation.h"
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

#define BOOT_ATTESTATION_TEST_CAPACITY 512U

static void boot_attestation_host_seal(
    const struct verification_policy *policy,
    const struct boot_chain_component components[3],
    unsigned char *envelope,
    size_t capacity,
    size_t *envelope_length)
{
    CHECK(boot_chain_manifest_seal(
              policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
              boot_chain_vector_dev_signature,
              BOOT_CHAIN_SIGNATURE_SIZE, envelope, capacity,
              envelope_length) == BOOT_CHAIN_OK,
          "the attestation fixture seals");
}

static void boot_attestation_host_attested(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    struct boot_chain_component components[3];
    struct boot_measurements measurements;
    struct boot_attestation attestation;
    unsigned char envelope[BOOT_ATTESTATION_TEST_CAPACITY];
    unsigned char aggregate[SHA256_DIGEST_SIZE];
    size_t envelope_length = 0U;

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK &&
              image_signature_policy_init(&store, &policy, 1U) ==
                  IMAGE_SIGNATURE_OK,
          "the declared trust set and policy build");
    boot_chain_vector_components(components);
    boot_attestation_host_seal(&policy, components, envelope,
                               sizeof(envelope), &envelope_length);

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK,
          "the register set resets");
    CHECK(boot_attestation_evaluate(
              &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
              envelope_length, components, 3U, &measurements,
              &attestation) == BOOT_CHAIN_OK,
          "a signed chain attests");
    CHECK(boot_attestation_is_published(&attestation),
          "the attestation is published");
    CHECK(attestation.verdict == BOOT_ATTESTATION_ATTESTED &&
              attestation.reason == BOOT_ATTESTATION_REASON_NONE,
          "the attested verdict carries no reason");
    CHECK(attestation.release_id == BOOT_CHAIN_VECTOR_RELEASE_ID &&
              attestation.signer_id ==
                  IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID &&
              attestation.algorithm ==
                  TRUST_ANCHOR_ALGORITHM_ECDSA_P256 &&
              attestation.component_count == 3U,
          "the attested report carries the signed identity");
    CHECK(memcmp(attestation.chain_digest,
                 boot_chain_vector_chain_digest,
                 SHA256_DIGEST_SIZE) == 0,
          "the attested report carries the signed chain digest");
    CHECK(attestation.measurements_published &&
              boot_measurements_is_published(&measurements) &&
              boot_measurements_aggregate(&measurements, aggregate) ==
                  BOOT_MEASUREMENTS_OK,
          "the attested report carries readable measurements");
    CHECK(memcmp(attestation.measurement_aggregate, aggregate,
                 SHA256_DIGEST_SIZE) == 0,
          "the published aggregate matches the register set");
    CHECK(!boot_attestation_blocks_production_boot(&attestation),
          "an attested chain never blocks production boot");
    CHECK(strcmp(boot_attestation_verdict_string(attestation.verdict),
                 "ATTESTED") == 0 &&
              strcmp(boot_attestation_reason_string(attestation.reason),
                     "NONE") == 0,
          "verdict and reason strings are exact");

    CHECK(boot_attestation_evaluate(
              &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
              envelope_length, components, 3U, NULL,
              &attestation) == BOOT_CHAIN_MEASUREMENT_FAILED,
          "a chain without a register set cannot attest");
    CHECK(attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
              attestation.reason ==
                  BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED &&
              boot_attestation_blocks_production_boot(&attestation),
          "a measurement failure is unattested and blocking");
}

static void boot_attestation_host_unavailable(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    struct boot_chain_component components[3];
    struct boot_measurements measurements;
    struct boot_attestation attestation;
    unsigned char envelope[BOOT_ATTESTATION_TEST_CAPACITY];
    size_t envelope_length = 0U;

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK &&
              image_signature_policy_init(&store, &policy, 1U) ==
                  IMAGE_SIGNATURE_OK,
          "the unavailable fixture trust set builds");
    boot_chain_vector_components(components);
    (void)boot_measurements_reset(&measurements);

    CHECK(boot_attestation_evaluate(
              &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, NULL, 0U,
              components, 3U, &measurements,
              &attestation) == BOOT_CHAIN_NULL,
          "an absent signed manifest is not a verification success");
    CHECK(boot_attestation_is_published(&attestation) &&
              attestation.verdict == BOOT_ATTESTATION_UNAVAILABLE &&
              attestation.reason ==
                  BOOT_ATTESTATION_REASON_NO_MANIFEST,
          "media without a signed chain publish UNAVAILABLE honestly");
    CHECK(!boot_attestation_blocks_production_boot(&attestation),
          "an unsigned development medium does not block boot");
    CHECK(strcmp(boot_attestation_verdict_string(attestation.verdict),
                 "UNAVAILABLE") == 0 &&
              strcmp(boot_attestation_reason_string(attestation.reason),
                     "NO_MANIFEST") == 0,
          "the unavailable strings are exact");

    CHECK(boot_attestation_publish_unavailable(&attestation, false) ==
              BOOT_ATTESTATION_REASON_MANIFEST_INVALID &&
              !boot_attestation_blocks_production_boot(&attestation),
          "an unusable manifest publishes an honest reason");
    CHECK(!boot_attestation_is_published(NULL) &&
              !boot_attestation_blocks_production_boot(NULL),
          "a null report is neither published nor blocking");

    /* The unavailable envelope path stays unavailable even with a valid
     * envelope available: an explicit NULL never attests. */
    boot_attestation_host_seal(&policy, components, envelope,
                               sizeof(envelope), &envelope_length);
    CHECK(envelope_length > 0U,
          "the sealed envelope is non-empty for the refusal matrix");
}

static void boot_attestation_host_refusals(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    struct boot_chain_component components[3];
    struct boot_measurements measurements;
    struct boot_attestation attestation;
    unsigned char envelope[BOOT_ATTESTATION_TEST_CAPACITY];
    unsigned char kernel[BOOT_CHAIN_VECTOR_KERNEL_LENGTH];
    size_t envelope_length = 0U;

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK &&
              image_signature_policy_init(&store, &policy, 1U) ==
                  IMAGE_SIGNATURE_OK,
          "the refusal trust set builds");
    boot_chain_vector_components(components);
    boot_attestation_host_seal(&policy, components, envelope,
                               sizeof(envelope), &envelope_length);

    memcpy(kernel, boot_chain_vector_kernel, sizeof(kernel));
    components[2].data = kernel;
    components[2].length = sizeof(kernel);
    kernel[7] ^= 0x20U;
    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
              boot_attestation_evaluate(
                  &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                  envelope_length, components, 3U, &measurements,
                  &attestation) ==
                  BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH,
          "a tampered component refuses");
    CHECK(attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
              attestation.reason ==
                  BOOT_ATTESTATION_REASON_COMPONENT_MISMATCH,
          "a tampered component is unattested with the exact reason");
    CHECK(boot_attestation_blocks_production_boot(&attestation),
          "a tampered chain blocks production boot");
    CHECK(!boot_measurements_is_published(&measurements),
          "a refused chain publishes no measurements");
    kernel[7] ^= 0x20U;

    CHECK(boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
              boot_attestation_evaluate(
                  &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID + 1U,
                  envelope, envelope_length, components, 3U, &measurements,
                  &attestation) == BOOT_CHAIN_RELEASE_MISMATCH,
          "a foreign release refuses");
    CHECK(attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
              attestation.reason ==
                  BOOT_ATTESTATION_REASON_RELEASE_MISMATCH,
          "a foreign release is unattested with the exact reason");

    {
        unsigned char bad_signature[BOOT_CHAIN_SIGNATURE_SIZE];

        memcpy(bad_signature, boot_chain_vector_dev_signature,
               sizeof(bad_signature));
        bad_signature[9] ^= 0x10U;
        CHECK(boot_chain_manifest_seal(
                  &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
                  IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                  TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
                  bad_signature, sizeof(bad_signature), envelope,
                  sizeof(envelope), &envelope_length) == BOOT_CHAIN_OK,
              "the tampered-signature envelope seals");
        CHECK(boot_measurements_reset(&measurements) ==
                  BOOT_MEASUREMENTS_OK &&
                  boot_attestation_evaluate(
                      &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                      envelope, envelope_length, components, 3U,
                      &measurements, &attestation) ==
                      BOOT_CHAIN_SIGNATURE_MISMATCH,
              "a tampered signature refuses");
        CHECK(attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
                  attestation.reason ==
                      BOOT_ATTESTATION_REASON_SIGNATURE_MISMATCH &&
                  boot_attestation_blocks_production_boot(&attestation),
              "a tampered signature blocks production boot");
    }

    {
        unsigned char short_envelope[BOOT_ATTESTATION_TEST_CAPACITY];
        size_t short_length = 0U;

        boot_attestation_host_seal(&policy, components, short_envelope,
                                   sizeof(short_envelope), &short_length);
        CHECK(boot_measurements_reset(&measurements) ==
                  BOOT_MEASUREMENTS_OK &&
                  boot_attestation_evaluate(
                      &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                      short_envelope, short_length - 1U, components, 3U,
                      &measurements, &attestation) ==
                      BOOT_CHAIN_ENVELOPE_INVALID,
              "a truncated envelope refuses");
        CHECK(attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
                  attestation.reason ==
                      BOOT_ATTESTATION_REASON_ENVELOPE_INVALID,
              "a truncated envelope is unattested with the exact reason");
    }

    CHECK(boot_attestation_evaluate(&policy, &store,
                                    BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                                    envelope_length, components, 3U,
                                    &measurements,
                                    NULL) == BOOT_CHAIN_NULL,
          "a null attestation output is refused");
}

int main(void)
{
    boot_attestation_host_attested();
    boot_attestation_host_unavailable();
    boot_attestation_host_refusals();

    if (failures != 0) {
        fprintf(stderr, "Boot attestation host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Boot attestation host tests: ALL OK (%d checks)\n", checks);
    return 0;
}

#else
int boot_attestation_host_translation_unit_marker;
#endif
