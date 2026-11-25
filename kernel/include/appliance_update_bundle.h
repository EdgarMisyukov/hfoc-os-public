#ifndef HFOC_APPLIANCE_UPDATE_BUNDLE_H
#define HFOC_APPLIANCE_UPDATE_BUNDLE_H

#include "appliance_update_integrity.h"
#include "appliance_update_manifest.h"
#include "appliance_update_record.h"
#include "image_signature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-376 signed update bundle.
 *
 * The update integrity gate proves that the manifest parses, that the
 * metadata and payload digests match the actual bytes and that the envelope
 * satisfies the declared signer and purpose policy.  It does not prove who
 * produced the bundle: the envelope carries an unkeyed SHA-256 checksum, so
 * anyone who can set a signer id can forge it.
 *
 * This module closes that gap.  It binds the admitted manifest identity
 * (package version, release, architecture, source revision, source and target
 * image ids) together with the verified metadata and payload digests into one
 * canonical byte string, and requires a real ECDSA P-256 signature over its
 * SHA-256 digest under KEY_USE_PURPOSE_UPDATE.  The purpose comes from this
 * caller, never from the artifact.
 *
 * It also owns the ordered admission rule against the durable update record:
 * a bundle whose target image is not strictly newer than the installed image
 * is refused as a downgrade, and a bundle built for a different installed
 * image is refused as a source mismatch.  Rolling back to the known-good slot
 * is a recorded outcome of the durable record, not a downgrade.
 *
 * Allocation-free; no block device and no private key is ever touched.
 */

#define APPLIANCE_UPDATE_BUNDLE_VERSION 1U
#define APPLIANCE_UPDATE_BUNDLE_SIGNATURE_SIZE \
    IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE
#define APPLIANCE_UPDATE_BUNDLE_MAGIC_SIZE 8U
#define APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE 151U
#define APPLIANCE_UPDATE_BUNDLE_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'U', 'P', 'D', 'B' }
#define APPLIANCE_UPDATE_BUNDLE_ALGORITHM \
    TRUST_ANCHOR_ALGORITHM_ECDSA_P256

enum appliance_update_bundle_status {
    APPLIANCE_UPDATE_BUNDLE_OK = 0,
    APPLIANCE_UPDATE_BUNDLE_NULL = 1,
    APPLIANCE_UPDATE_BUNDLE_BAD_STATE = 2,
    APPLIANCE_UPDATE_BUNDLE_BAD_VERSION = 3,
    APPLIANCE_UPDATE_BUNDLE_BAD_BINDING = 4,
    APPLIANCE_UPDATE_BUNDLE_GATE_NOT_VERIFIED = 5,
    APPLIANCE_UPDATE_BUNDLE_UNSIGNED = 6,
    APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID = 7,
    APPLIANCE_UPDATE_BUNDLE_SIGNATURE_UNAVAILABLE = 8,
    APPLIANCE_UPDATE_BUNDLE_PURPOSE_DENIED = 9,
    APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED = 10,
    APPLIANCE_UPDATE_BUNDLE_SOURCE_MISMATCH = 11,
    APPLIANCE_UPDATE_BUNDLE_NOT_VERIFIED = 12,
    APPLIANCE_UPDATE_BUNDLE_BUFFER_TOO_SMALL = 13,
};

struct appliance_update_bundle_binding {
    uint32_t version;
    uint32_t update_package_version;
    uint32_t source_image_id;
    uint32_t target_image_id;
    /* NUL-terminated in memory; the canonical encoding copies exactly the
     * declared field length, so the signed bytes stay fixed-size. */
    char release_id[RELEASE_PROFILE_ID_LENGTH + 1U];
    char architecture[RELEASE_PROFILE_ARCH_LENGTH + 1U];
    char source_revision[APPLIANCE_UPDATE_MANIFEST_REVISION_LENGTH + 1U];
    uint8_t metadata_digest[SHA256_DIGEST_SIZE];
    uint8_t payload_digest[SHA256_DIGEST_SIZE];
};

struct appliance_update_bundle {
    uint32_t version;
    bool verified;
    uint32_t signer_id;
    enum trust_anchor_algorithm algorithm;
    struct appliance_update_bundle_binding binding;
    uint8_t digest[SHA256_DIGEST_SIZE];
};

/* Bind the admitted manifest identity to the verified digests. */
enum appliance_update_bundle_status appliance_update_bundle_bind(
    const struct appliance_update_manifest *manifest,
    const struct appliance_update_integrity_gate *gate,
    struct appliance_update_bundle_binding *binding_out);
/* The canonical bytes the signature covers, for signing and for diagnosis. */
enum appliance_update_bundle_status appliance_update_bundle_encode(
    const struct appliance_update_bundle_binding *binding,
    uint8_t *output,
    size_t capacity,
    size_t *length_out);
enum appliance_update_bundle_status appliance_update_bundle_digest(
    const struct appliance_update_bundle_binding *binding,
    uint8_t digest_out[SHA256_DIGEST_SIZE]);
/*
 * Verify the real signature over the bound bundle.  The gate must already be
 * verified, otherwise the digests the binding commits to are unproven.
 */
enum appliance_update_bundle_status appliance_update_bundle_verify(
    struct appliance_update_bundle *bundle,
    const struct appliance_update_manifest *manifest,
    const struct appliance_update_integrity_gate *gate,
    const struct image_signature_store *store,
    const void *signature,
    size_t signature_length);
/* Ordered admission: downgrade and foreign-source refusal. */
enum appliance_update_bundle_status appliance_update_bundle_admit(
    const struct appliance_update_bundle *bundle,
    const struct appliance_update_record *record);
bool appliance_update_bundle_is_verified(
    const struct appliance_update_bundle *bundle);
const char *appliance_update_bundle_status_string(
    enum appliance_update_bundle_status status);

#endif
