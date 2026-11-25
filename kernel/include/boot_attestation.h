#ifndef HFOC_BOOT_ATTESTATION_H
#define HFOC_BOOT_ATTESTATION_H

#include "boot_chain.h"
#include "boot_measurements.h"
#include "image_signature.h"
#include "sha256.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-372 boot attestation report.
 *
 * The attestation is the single readable verdict a management plane or a
 * health consumer asks for: was the boot chain that is running verified
 * against a signed manifest, and do the measured values match what was
 * signed?
 *
 * Verdicts are honest by construction:
 *   - ATTESTED     a signed chain was provisioned and every step verified;
 *   - UNATTESTED   a signed chain was provisioned and did not verify;
 *   - UNAVAILABLE  no signed chain is provisioned on this media.
 *
 * A provisioned chain that fails is a refusal: the production boot
 * coordinator must not reach NORMAL mode.  Media without a signed chain
 * publish UNAVAILABLE rather than claiming a pass, and never claim
 * ATTESTED.
 */

#define BOOT_ATTESTATION_VERSION 1U

enum boot_attestation_verdict {
    BOOT_ATTESTATION_UNAVAILABLE = 0,
    BOOT_ATTESTATION_UNATTESTED = 1,
    BOOT_ATTESTATION_ATTESTED = 2,
};

enum boot_attestation_reason {
    BOOT_ATTESTATION_REASON_NONE = 0,
    BOOT_ATTESTATION_REASON_NO_MANIFEST = 1,
    BOOT_ATTESTATION_REASON_ENVELOPE_INVALID = 2,
    BOOT_ATTESTATION_REASON_POLICY_MISMATCH = 3,
    BOOT_ATTESTATION_REASON_MANIFEST_INVALID = 4,
    BOOT_ATTESTATION_REASON_ALGORITHM_UNSUPPORTED = 5,
    BOOT_ATTESTATION_REASON_VERIFIER_UNAVAILABLE = 6,
    BOOT_ATTESTATION_REASON_SIGNATURE_INVALID = 7,
    BOOT_ATTESTATION_REASON_SIGNATURE_MISMATCH = 8,
    BOOT_ATTESTATION_REASON_RELEASE_MISMATCH = 9,
    BOOT_ATTESTATION_REASON_COMPONENT_MISMATCH = 10,
    BOOT_ATTESTATION_REASON_MEASUREMENT_FAILED = 11,
};

struct boot_attestation {
    uint32_t version;
    bool published;
    enum boot_attestation_verdict verdict;
    enum boot_attestation_reason reason;
    uint32_t release_id;
    uint32_t signer_id;
    enum trust_anchor_algorithm algorithm;
    size_t component_count;
    bool measurements_published;
    unsigned char chain_digest[SHA256_DIGEST_SIZE];
    unsigned char measurement_aggregate[SHA256_DIGEST_SIZE];
};

/* Publish the honest report for media without a signed boot chain. */
enum boot_attestation_reason boot_attestation_publish_unavailable(
    struct boot_attestation *attestation,
    bool no_manifest);

/* Evaluate a provisioned signed chain: verify, measure and publish the
 * verdict.  The returned status is the verification status; the
 * attestation is always published, so a refusal is readable. */
enum boot_chain_status boot_attestation_evaluate(
    const struct verification_policy *policy,
    const struct image_signature_store *store,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const struct boot_chain_component *components,
    size_t component_count,
    struct boot_measurements *measurements,
    struct boot_attestation *attestation);

/* A provisioned chain that did not verify blocks normal production boot.
 * UNAVAILABLE media are published honestly but do not block. */
bool boot_attestation_blocks_production_boot(
    const struct boot_attestation *attestation);
bool boot_attestation_is_published(
    const struct boot_attestation *attestation);

const char *boot_attestation_verdict_string(
    enum boot_attestation_verdict verdict);
const char *boot_attestation_reason_string(
    enum boot_attestation_reason reason);

#endif
