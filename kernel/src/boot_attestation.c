#include "boot_attestation.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static const char *const boot_attestation_verdict_names[] = {
    "UNAVAILABLE",
    "UNATTESTED",
    "ATTESTED",
};

static const char *const boot_attestation_reason_names[] = {
    "NONE",
    "NO_MANIFEST",
    "ENVELOPE_INVALID",
    "POLICY_MISMATCH",
    "MANIFEST_INVALID",
    "ALGORITHM_UNSUPPORTED",
    "VERIFIER_UNAVAILABLE",
    "SIGNATURE_INVALID",
    "SIGNATURE_MISMATCH",
    "RELEASE_MISMATCH",
    "COMPONENT_MISMATCH",
    "MEASUREMENT_FAILED",
};

static enum boot_attestation_reason boot_attestation_reason_for_status(
    enum boot_chain_status status)
{
    switch (status) {
    case BOOT_CHAIN_ENVELOPE_INVALID:
        return BOOT_ATTESTATION_REASON_ENVELOPE_INVALID;
    case BOOT_CHAIN_POLICY_MISMATCH:
        return BOOT_ATTESTATION_REASON_POLICY_MISMATCH;
    case BOOT_CHAIN_MANIFEST_INVALID:
    case BOOT_CHAIN_BAD_VERSION:
    case BOOT_CHAIN_COMPONENT_UNDECLARED:
    case BOOT_CHAIN_CAPACITY:
        return BOOT_ATTESTATION_REASON_MANIFEST_INVALID;
    case BOOT_CHAIN_ALGORITHM_UNSUPPORTED:
        return BOOT_ATTESTATION_REASON_ALGORITHM_UNSUPPORTED;
    case BOOT_CHAIN_VERIFIER_UNAVAILABLE:
        return BOOT_ATTESTATION_REASON_VERIFIER_UNAVAILABLE;
    case BOOT_CHAIN_SIGNATURE_INVALID:
        return BOOT_ATTESTATION_REASON_SIGNATURE_INVALID;
    case BOOT_CHAIN_SIGNATURE_MISMATCH:
        return BOOT_ATTESTATION_REASON_SIGNATURE_MISMATCH;
    case BOOT_CHAIN_RELEASE_MISMATCH:
        return BOOT_ATTESTATION_REASON_RELEASE_MISMATCH;
    case BOOT_CHAIN_COMPONENT_MISSING:
    case BOOT_CHAIN_COMPONENT_LENGTH_MISMATCH:
    case BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH:
    case BOOT_CHAIN_COMPONENT_LIMIT:
        return BOOT_ATTESTATION_REASON_COMPONENT_MISMATCH;
    case BOOT_CHAIN_MEASUREMENT_FAILED:
        return BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED;
    default:
        return BOOT_ATTESTATION_REASON_MANIFEST_INVALID;
    }
}

enum boot_attestation_reason boot_attestation_publish_unavailable(
    struct boot_attestation *attestation,
    bool no_manifest)
{
    if (attestation == NULL) {
        return BOOT_ATTESTATION_REASON_MANIFEST_INVALID;
    }
    memset(attestation, 0, sizeof(*attestation));
    attestation->version = BOOT_ATTESTATION_VERSION;
    attestation->published = true;
    attestation->verdict = BOOT_ATTESTATION_UNAVAILABLE;
    attestation->reason = no_manifest ?
        BOOT_ATTESTATION_REASON_NO_MANIFEST :
        BOOT_ATTESTATION_REASON_MANIFEST_INVALID;
    return attestation->reason;
}

enum boot_chain_status boot_attestation_evaluate(
    const struct verification_policy *policy,
    const struct image_signature_store *store,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const struct boot_chain_component *components,
    size_t component_count,
    struct boot_measurements *measurements,
    struct boot_attestation *attestation)
{
    struct boot_chain_verify_report report;
    enum boot_chain_status status;

    if (attestation == NULL) {
        return BOOT_CHAIN_NULL;
    }
    if (envelope == NULL) {
        (void)boot_attestation_publish_unavailable(attestation, true);
        return BOOT_CHAIN_NULL;
    }

    status = boot_chain_verify(policy, store, expected_release_id, envelope,
                               envelope_length, components, component_count,
                               &report);
    memset(attestation, 0, sizeof(*attestation));
    attestation->version = BOOT_ATTESTATION_VERSION;
    attestation->published = true;
    attestation->release_id = report.release_id;
    attestation->signer_id = report.signer_id;
    attestation->algorithm = report.algorithm;
    attestation->component_count = report.component_count;
    memcpy(attestation->chain_digest, report.chain_digest,
           SHA256_DIGEST_SIZE);

    if (status != BOOT_CHAIN_OK) {
        attestation->verdict = BOOT_ATTESTATION_UNATTESTED;
        attestation->reason =
            boot_attestation_reason_for_status(status);
        return status;
    }
    if (measurements == NULL) {
        attestation->verdict = BOOT_ATTESTATION_UNATTESTED;
        attestation->reason = BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED;
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }

    status = boot_chain_measure(&report, measurements);
    if (status != BOOT_CHAIN_OK ||
        !boot_measurements_is_published(measurements)) {
        attestation->verdict = BOOT_ATTESTATION_UNATTESTED;
        attestation->reason = BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED;
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }

    attestation->verdict = BOOT_ATTESTATION_ATTESTED;
    attestation->reason = BOOT_ATTESTATION_REASON_NONE;
    attestation->measurements_published = true;
    if (boot_measurements_aggregate(measurements,
                                    attestation->measurement_aggregate) !=
        BOOT_MEASUREMENTS_OK) {
        attestation->verdict = BOOT_ATTESTATION_UNATTESTED;
        attestation->reason = BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED;
        attestation->measurements_published = false;
        return BOOT_CHAIN_MEASUREMENT_FAILED;
    }
    return BOOT_CHAIN_OK;
}

bool boot_attestation_blocks_production_boot(
    const struct boot_attestation *attestation)
{
    return attestation != NULL &&
           attestation->version == BOOT_ATTESTATION_VERSION &&
           attestation->published &&
           attestation->verdict == BOOT_ATTESTATION_UNATTESTED;
}

bool boot_attestation_is_published(
    const struct boot_attestation *attestation)
{
    return attestation != NULL &&
           attestation->version == BOOT_ATTESTATION_VERSION &&
           attestation->published;
}

const char *boot_attestation_verdict_string(
    enum boot_attestation_verdict verdict)
{
    if ((size_t)verdict >=
        sizeof(boot_attestation_verdict_names) /
            sizeof(boot_attestation_verdict_names[0])) {
        return "UNKNOWN";
    }
    return boot_attestation_verdict_names[verdict];
}

const char *boot_attestation_reason_string(
    enum boot_attestation_reason reason)
{
    if ((size_t)reason >=
        sizeof(boot_attestation_reason_names) /
            sizeof(boot_attestation_reason_names[0])) {
        return "UNKNOWN";
    }
    return boot_attestation_reason_names[reason];
}
