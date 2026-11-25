#ifndef HFOC_BOOT_CHAIN_H
#define HFOC_BOOT_CHAIN_H

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

/*
 * ITEM-372 signed boot chain.
 *
 * The boot chain is the set of files the loader actually loaded to run this
 * kernel.  A release signs one fixed little-endian manifest that declares,
 * for every component, its exact length and SHA-256 digest; the manifest is
 * sealed into the existing HFOCSIGN envelope and the manifest digest is
 * signed with the production ECDSA verifier (image_signature).
 *
 * Verification is fail closed and ordered: envelope policy -> manifest
 * structure -> signature -> release identity -> component lengths ->
 * component digests.  Every step that is skipped by an early refusal is
 * reported as false, so a caller can never read a partially verified chain
 * as trusted.
 *
 * The measured-boot register set is filled from the verified manifest and
 * the actual component bytes, so a reader sees both the declared and the
 * measured value.
 */

#define BOOT_CHAIN_VERSION 1U
#define BOOT_CHAIN_MANIFEST_MAGIC_SIZE 8U
#define BOOT_CHAIN_MANIFEST_FORMAT_VERSION 1U
#define BOOT_CHAIN_MANIFEST_HEADER_SIZE 32U
#define BOOT_CHAIN_COMPONENT_CAPACITY 4U
#define BOOT_CHAIN_COMPONENT_ENTRY_SIZE 48U
#define BOOT_CHAIN_MANIFEST_BODY_SIZE \
    (BOOT_CHAIN_MANIFEST_HEADER_SIZE + \
     (BOOT_CHAIN_COMPONENT_CAPACITY * BOOT_CHAIN_COMPONENT_ENTRY_SIZE))
#define BOOT_CHAIN_SIGNATURE_SIZE IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE
#define BOOT_CHAIN_MANIFEST_SIZE \
    (BOOT_CHAIN_MANIFEST_BODY_SIZE + BOOT_CHAIN_SIGNATURE_SIZE)
#define BOOT_CHAIN_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'B', 'O', 'O', 'T' }
#define BOOT_CHAIN_RELEASE_ID_MIN 1U
/* The release identity the production boot chain must carry. */
#define BOOT_CHAIN_EXPECTED_RELEASE_ID 1U

enum boot_chain_component_kind {
    BOOT_CHAIN_COMPONENT_BOOT_PAYLOAD = 1,
    BOOT_CHAIN_COMPONENT_LOADER_CONFIG = 2,
    BOOT_CHAIN_COMPONENT_KERNEL = 3,
};

/* The register a measured component extends.  Register 0 binds the whole
 * chain (the signed manifest digest); component kind k >= 1 extends
 * register k when it is inside the register set. */
#define BOOT_CHAIN_CHAIN_REGISTER 0U

enum boot_chain_status {
    BOOT_CHAIN_OK = 0,
    BOOT_CHAIN_NULL = 1,
    BOOT_CHAIN_BAD_VERSION = 2,
    BOOT_CHAIN_ENVELOPE_INVALID = 3,
    BOOT_CHAIN_POLICY_MISMATCH = 4,
    BOOT_CHAIN_MANIFEST_INVALID = 5,
    BOOT_CHAIN_ALGORITHM_UNSUPPORTED = 6,
    BOOT_CHAIN_VERIFIER_UNAVAILABLE = 7,
    BOOT_CHAIN_SIGNATURE_INVALID = 8,
    BOOT_CHAIN_SIGNATURE_MISMATCH = 9,
    BOOT_CHAIN_RELEASE_MISMATCH = 10,
    BOOT_CHAIN_COMPONENT_MISSING = 11,
    BOOT_CHAIN_COMPONENT_UNDECLARED = 12,
    BOOT_CHAIN_COMPONENT_LENGTH_MISMATCH = 13,
    BOOT_CHAIN_COMPONENT_DIGEST_MISMATCH = 14,
    BOOT_CHAIN_COMPONENT_LIMIT = 15,
    BOOT_CHAIN_CAPACITY = 16,
    BOOT_CHAIN_MEASUREMENT_FAILED = 17,
};

struct boot_chain_component {
    uint32_t kind;
    const void *data;
    size_t length;
};

struct boot_chain_component_record {
    uint32_t kind;
    uint32_t reserved;
    uint64_t declared_length;
    unsigned char digest[SHA256_DIGEST_SIZE];
    bool matched;
};

struct boot_chain_verify_report {
    uint32_t version;
    bool envelope_verified;
    bool signature_verified;
    bool release_verified;
    bool components_verified;
    bool trusted;
    uint32_t release_id;
    uint32_t signer_id;
    enum trust_anchor_algorithm algorithm;
    size_t component_count;
    unsigned char chain_digest[SHA256_DIGEST_SIZE];
    struct boot_chain_component_record
        components[BOOT_CHAIN_COMPONENT_CAPACITY];
};

/* Producer boundary: build the manifest for the exact component bytes,
 * embed the caller-supplied raw signature and seal the result into an
 * HFOCSIGN envelope.  The signature must sign the digest returned by
 * boot_chain_chain_digest() for the same release, signer and components. */
enum boot_chain_status boot_chain_chain_digest(
    uint32_t release_id,
    uint32_t signer_id,
    const struct boot_chain_component *components,
    size_t component_count,
    unsigned char digest_out[SHA256_DIGEST_SIZE]);
enum boot_chain_status boot_chain_manifest_seal(
    const struct verification_policy *policy,
    uint32_t release_id,
    uint32_t signer_id,
    enum trust_anchor_algorithm algorithm,
    const struct boot_chain_component *components,
    size_t component_count,
    const unsigned char *signature,
    size_t signature_length,
    void *buffer,
    size_t capacity,
    size_t *length_out);

/* Verify a sealed boot chain against the actual component bytes.  The
 * report is cleared first; a failed run never leaves a trusted report. */
enum boot_chain_status boot_chain_verify(
    const struct verification_policy *policy,
    const struct image_signature_store *store,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const struct boot_chain_component *components,
    size_t component_count,
    struct boot_chain_verify_report *report);

bool boot_chain_report_is_trusted(
    const struct boot_chain_verify_report *report);

/* Fill the measured-boot register set from a trusted report: the signed
 * chain digest binds register 0 and every declared component extends its
 * kind register and the log. */
enum boot_chain_status boot_chain_measure(
    const struct boot_chain_verify_report *report,
    struct boot_measurements *measurements);

const char *boot_chain_status_string(enum boot_chain_status status);
const char *boot_chain_component_kind_string(uint32_t kind);

#endif
