#ifdef HFOC_BOOT_CHAIN_QEMU_TEST

#include "appliance_production_boot.h"
#include "boot-chain-vector.h"
#include "boot_attestation.h"
#include "boot_chain.h"
#include "boot_measurements.h"
#include "image_signature.h"
#include "key_use.h"
#include "serial.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "trust_anchor.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BOOT_CHAIN_QEMU_ENVELOPE_CAPACITY 512U

static void boot_chain_qemu_fail(const char *message)
    __attribute__((noreturn));

static void boot_chain_qemu_fail(const char *message)
{
    const uint64_t flags = serial_sequence_begin();

    serial_write("[BOOT-CHAIN] failed: ");
    serial_write(message);
    serial_write("\n");
    serial_sequence_end(flags);
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void boot_chain_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        boot_chain_qemu_fail(message);
    }
}

static void boot_chain_qemu_fixture_store(
    struct image_signature_store *store,
    struct verification_policy *policy)
{
    struct image_signature_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.signer_id = BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID;
    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    anchor.accepted_purposes = KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memcpy(anchor.public_key, boot_chain_vector_fixture_public_key,
           sizeof(anchor.public_key));

    boot_chain_qemu_require(
        image_signature_store_init(store, false) == IMAGE_SIGNATURE_OK,
        "fixture trust set binds");
    boot_chain_qemu_require(
        image_signature_store_add(store, &anchor) == IMAGE_SIGNATURE_OK,
        "fixture anchor binds");
    boot_chain_qemu_require(
        image_signature_policy_init(store, policy, 1U) ==
            IMAGE_SIGNATURE_OK,
        "fixture policy builds");
}

static void boot_chain_qemu_proof(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    struct boot_chain_component components[3];
    struct boot_measurements measurements;
    struct boot_attestation attestation;
    struct boot_chain_verify_report report;
    unsigned char envelope[BOOT_CHAIN_QEMU_ENVELOPE_CAPACITY];
    unsigned char kernel[BOOT_CHAIN_VECTOR_KERNEL_LENGTH];
    size_t envelope_length = 0U;
    unsigned char aggregate[SHA256_DIGEST_SIZE];
    unsigned char bad_signature[BOOT_CHAIN_SIGNATURE_SIZE];

    serial_write("[BOOT-CHAIN] proof init\n");
    boot_chain_qemu_fixture_store(&store, &policy);
    boot_chain_vector_components(components);

    boot_chain_qemu_require(
        boot_chain_manifest_seal(
            &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
            BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID,
            TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
            boot_chain_vector_fixture_signature,
            BOOT_CHAIN_SIGNATURE_SIZE, envelope, sizeof(envelope),
            &envelope_length) == BOOT_CHAIN_OK,
        "fixture manifest seals");
    boot_chain_qemu_require(
        boot_chain_verify(&policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID,
                          envelope, envelope_length, components, 3U,
                          &report) == BOOT_CHAIN_OK &&
            boot_chain_report_is_trusted(&report),
        "signed chain verifies in the kernel");

    boot_chain_qemu_require(
        boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
            boot_attestation_evaluate(
                &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                envelope_length, components, 3U, &measurements,
                &attestation) == BOOT_CHAIN_OK,
        "attested chain evaluates");
    boot_chain_qemu_require(
        attestation.verdict == BOOT_ATTESTATION_ATTESTED &&
            attestation.reason == BOOT_ATTESTATION_REASON_NONE &&
            attestation.measurements_published &&
            !boot_attestation_blocks_production_boot(&attestation),
        "attested verdict is honest");
    boot_chain_qemu_require(
        boot_measurements_log_count(&measurements) == 4U &&
            boot_measurements_read(&measurements,
                                   BOOT_CHAIN_COMPONENT_KERNEL,
                                   aggregate) == BOOT_MEASUREMENTS_OK &&
            boot_measurements_aggregate(&measurements, aggregate) ==
                BOOT_MEASUREMENTS_OK,
        "measured boot registers are readable");
    serial_write("[BOOT-CHAIN] measured boot register proof OK\n");

    /* A tampered kernel component must be refused and must block normal
     * production boot. */
    memcpy(kernel, boot_chain_vector_kernel, sizeof(kernel));
    components[2].data = kernel;
    components[2].length = sizeof(kernel);
    kernel[11] ^= 0x40U;
    boot_chain_qemu_require(
        boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
            boot_attestation_evaluate(
                &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                envelope_length, components, 3U, &measurements,
                &attestation) == BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH,
        "tampered kernel component refuses");
    boot_chain_qemu_require(
        attestation.verdict == BOOT_ATTESTATION_UNATTESTED &&
            attestation.reason ==
                BOOT_ATTESTATION_REASON_COMPONENT_MISMATCH &&
            boot_attestation_blocks_production_boot(&attestation) &&
            !boot_measurements_is_published(&measurements),
        "tampered chain is unattested and blocking");
    serial_write("[BOOT-CHAIN] tamper refusal proof OK\n");
    kernel[11] ^= 0x40U;

    /* A provisioned chain whose signature does not verify must also be
     * refused, with the exact asymmetric reason. */
    memcpy(bad_signature, boot_chain_vector_fixture_signature,
           sizeof(bad_signature));
    bad_signature[5] ^= 0x08U;
    boot_chain_qemu_require(
        boot_chain_manifest_seal(
            &policy, BOOT_CHAIN_VECTOR_RELEASE_ID,
            BOOT_CHAIN_VECTOR_FIXTURE_SIGNER_ID,
            TRUST_ANCHOR_ALGORITHM_ECDSA_P256, components, 3U,
            bad_signature, sizeof(bad_signature), envelope,
            sizeof(envelope), &envelope_length) == BOOT_CHAIN_OK,
        "tampered-signature manifest seals");
    boot_chain_qemu_require(
        boot_measurements_reset(&measurements) == BOOT_MEASUREMENTS_OK &&
            boot_attestation_evaluate(
                &policy, &store, BOOT_CHAIN_VECTOR_RELEASE_ID, envelope,
                envelope_length, components, 3U, &measurements,
                &attestation) == BOOT_CHAIN_SIGNATURE_MISMATCH &&
            attestation.reason ==
                BOOT_ATTESTATION_REASON_SIGNATURE_MISMATCH &&
            boot_attestation_blocks_production_boot(&attestation),
        "tampered signature refuses");
    serial_write("[BOOT-CHAIN] asymmetric refusal proof OK\n");

    /* Bind the refused attestation into the production boot coordinator:
     * the appliance must refuse to reach NORMAL mode. */
    {
        struct appliance_production_boot boot;

        boot_chain_qemu_require(
            appliance_production_boot_init(&boot) ==
                APPLIANCE_PRODUCTION_BOOT_OK,
            "coordinator initialises");
        boot_chain_qemu_require(
            appliance_production_boot_set_attestation(
                &boot,
                APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED,
                boot_attestation_reason_string(attestation.reason)) ==
                APPLIANCE_PRODUCTION_BOOT_OK,
            "refused attestation binds");
        boot_chain_qemu_require(
            appliance_production_boot_run(&boot, NULL, NULL) ==
                    APPLIANCE_PRODUCTION_BOOT_UNATTESTED &&
                appliance_production_boot_readiness(&boot) ==
                    APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE &&
                appliance_production_boot_mode(&boot) ==
                    APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED,
            "an unattested chain refuses production boot");
        {
            const uint64_t serial_flags = serial_sequence_begin();

            serial_write(
                "[BOOT-CHAIN] production boot refusal proof OK\n");
            serial_write("[BOOT-CHAIN] status=");
            serial_write(appliance_production_boot_status_string(
                boot.status));
            serial_write(" mode=");
            serial_write(appliance_production_boot_mode_string(
                appliance_production_boot_mode(&boot)));
            serial_write(" reason=");
            serial_write(
                appliance_production_boot_attestation_detail(&boot) !=
                        NULL
                    ? appliance_production_boot_attestation_detail(&boot)
                    : "NONE");
            serial_write("\n");
            serial_sequence_end(serial_flags);
        }
    }

    serial_write("[BOOT-CHAIN] proof OK\n");
}

void boot_chain_qemu_test_init(void)
{
    boot_chain_qemu_proof();
}

#else
int boot_chain_qemu_translation_unit_marker;
#endif
