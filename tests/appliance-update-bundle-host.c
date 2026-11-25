/*
 * ITEM-376 focused host proof: the signed update bundle -- canonical binding,
 * real ECDSA P-256 verification under the declared update purpose, and the
 * ordered downgrade and source admission rules.
 */

#ifdef HFOC_APPLIANCE_UPDATE_BUNDLE_HOST_TEST

#include "appliance_update_bundle.h"

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

/*
 * A real ECDSA P-256 signature over the canonical binding digest
 * e171c242bf577ed2a4db5faa0f049f19158bea139ba25133bd885db55e1bfe76, produced
 * by boot/keys/hfoc-development-signing-key.pem, whose public half is the
 * declared development anchor.  The fixture values below reproduce that
 * digest exactly.
 */
static const uint8_t host_signature[
    APPLIANCE_UPDATE_BUNDLE_SIGNATURE_SIZE] = {
    0xE4, 0x74, 0xE4, 0x55, 0x08, 0x76, 0xA4, 0xD6,
    0xEA, 0x13, 0x15, 0x4C, 0xDF, 0x0A, 0x1D, 0x61,
    0xD1, 0xCB, 0xEC, 0x12, 0x63, 0x7A, 0x33, 0xAA,
    0x10, 0x99, 0x51, 0xE3, 0xF4, 0x1C, 0x9B, 0xB8,
    0x76, 0x78, 0xF8, 0xEC, 0xB4, 0x0C, 0x4F, 0x64,
    0xC1, 0xBA, 0x1A, 0x51, 0x89, 0xDB, 0x57, 0x37,
    0x79, 0x94, 0x5E, 0x00, 0x29, 0x63, 0xB4, 0xC8,
    0x1B, 0x4B, 0xF1, 0x5F, 0x0B, 0x1D, 0xAC, 0x87,
};

static const uint8_t host_digest[SHA256_DIGEST_SIZE] = {
    0xE1, 0x71, 0xC2, 0x42, 0xBF, 0x57, 0x7E, 0xD2,
    0xA4, 0xDB, 0x5F, 0xAA, 0x0F, 0x04, 0x9F, 0x19,
    0x15, 0x8B, 0xEA, 0x13, 0x9B, 0xA2, 0x51, 0x33,
    0xBD, 0x88, 0x5D, 0xB5, 0x5E, 0x1B, 0xFE, 0x76,
};

static void host_manifest(struct appliance_update_manifest *manifest)
{
    memset(manifest, 0, sizeof(*manifest));
    manifest->update_package_version =
        APPLIANCE_UPDATE_MANIFEST_VERSION;
    (void)snprintf(manifest->release_id,
                   sizeof(manifest->release_id), "%s",
                   "HFOC-APPLIANCE-V1");
    (void)snprintf(manifest->architecture,
                   sizeof(manifest->architecture), "%s", "x86_64");
    (void)snprintf(manifest->source_revision,
                   sizeof(manifest->source_revision), "%s",
                   "0123456789abcdef0123456789abcdef01234567");
    (void)snprintf(
        manifest->metadata_digest, sizeof(manifest->metadata_digest),
        "%s",
        "1111111111111111111111111111111111111111111111111111111111111111");
    (void)snprintf(
        manifest->payload_digest, sizeof(manifest->payload_digest),
        "%s",
        "2222222222222222222222222222222222222222222222222222222222222222");
    manifest->compatibility.source_image_id = 1U;
    manifest->compatibility.target_image_id = 2U;
}

static void host_gate(struct appliance_update_integrity_gate *gate)
{
    memset(gate, 0, sizeof(*gate));
    gate->version = APPLIANCE_UPDATE_INTEGRITY_VERSION;
    gate->verified = true;
    gate->signer_id = IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID;
    memset(gate->metadata_digest, 0x11, SHA256_DIGEST_SIZE);
    memset(gate->payload_digest, 0x22, SHA256_DIGEST_SIZE);
}

/* 1. The binding is canonical, bounded and commits to the verified bytes. */
static void host_binding(void)
{
    struct appliance_update_manifest manifest;
    struct appliance_update_integrity_gate gate;
    struct appliance_update_bundle_binding binding;
    struct appliance_update_bundle_binding other;
    uint8_t encoding[APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE];
    uint8_t repeat[APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE];
    uint8_t digest[SHA256_DIGEST_SIZE];
    size_t length = 0U;
    size_t repeat_length = 0U;

    host_manifest(&manifest);
    host_gate(&gate);
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &binding) ==
              APPLIANCE_UPDATE_BUNDLE_OK,
          "the binding is built from the manifest and the gate");
    CHECK(binding.version == APPLIANCE_UPDATE_BUNDLE_VERSION &&
              binding.update_package_version == 1U &&
              binding.source_image_id == 1U &&
              binding.target_image_id == 2U &&
              strcmp(binding.release_id, "HFOC-APPLIANCE-V1") == 0 &&
              strcmp(binding.architecture, "x86_64") == 0,
          "the binding carries the manifest identity");
    CHECK(memcmp(binding.metadata_digest, gate.metadata_digest,
                 SHA256_DIGEST_SIZE) == 0 &&
              memcmp(binding.payload_digest, gate.payload_digest,
                     SHA256_DIGEST_SIZE) == 0,
          "the binding commits to the verified digests");

    CHECK(appliance_update_bundle_encode(
              &binding, encoding, sizeof(encoding), &length) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              length == APPLIANCE_UPDATE_BUNDLE_ENCODING_SIZE,
          "the canonical encoding is exactly bounded");
    CHECK(appliance_update_bundle_encode(
              &binding, repeat, sizeof(repeat), &repeat_length) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              repeat_length == length &&
              memcmp(encoding, repeat, length) == 0,
          "the canonical encoding is deterministic");
    CHECK(appliance_update_bundle_encode(
              &binding, encoding, length - 1U, &repeat_length) ==
              APPLIANCE_UPDATE_BUNDLE_BUFFER_TOO_SMALL,
          "a short encoding buffer is refused");

    CHECK(appliance_update_bundle_digest(&binding, digest) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              memcmp(digest, host_digest, SHA256_DIGEST_SIZE) == 0,
          "the canonical digest matches the signed vector");

    /* Every identity field changes the digest. */
    host_manifest(&manifest);
    manifest.compatibility.target_image_id = 3U;
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &other) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              appliance_update_bundle_digest(&other, repeat) ==
                  APPLIANCE_UPDATE_BUNDLE_OK &&
              memcmp(repeat, host_digest, SHA256_DIGEST_SIZE) != 0,
          "a different target image changes the signed digest");
    host_manifest(&manifest);
    manifest.compatibility.source_image_id = 9U;
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &other) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              appliance_update_bundle_digest(&other, repeat) ==
                  APPLIANCE_UPDATE_BUNDLE_OK &&
              memcmp(repeat, host_digest, SHA256_DIGEST_SIZE) != 0,
          "a different source image changes the signed digest");
    host_manifest(&manifest);
    (void)snprintf(manifest.source_revision,
                   sizeof(manifest.source_revision), "%s",
                   "89abcdef0123456789abcdef0123456789abcdef");
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &other) ==
              APPLIANCE_UPDATE_BUNDLE_OK &&
              appliance_update_bundle_digest(&other, repeat) ==
                  APPLIANCE_UPDATE_BUNDLE_OK &&
              memcmp(repeat, host_digest, SHA256_DIGEST_SIZE) != 0,
          "a different source revision changes the signed digest");

    /* The binding never accepts unverified digests. */
    host_manifest(&manifest);
    gate.verified = false;
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &binding) ==
              APPLIANCE_UPDATE_BUNDLE_GATE_NOT_VERIFIED,
          "an unverified gate cannot be bound");
    host_gate(&gate);
    gate.payload_digest[0] ^= 0xFFU;
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &binding) ==
              APPLIANCE_UPDATE_BUNDLE_BAD_BINDING,
          "a gate that disagrees with the manifest is refused");
    host_gate(&gate);
    (void)snprintf(manifest.metadata_digest,
                   sizeof(manifest.metadata_digest), "%s",
                   "not-a-hex-digest");
    CHECK(appliance_update_bundle_bind(&manifest, &gate, &binding) ==
              APPLIANCE_UPDATE_BUNDLE_BAD_BINDING,
          "a malformed digest field is refused");
}

/* 2. Only a real signature over the bound digest is accepted. */
static void host_signature_verification(void)
{
    struct appliance_update_manifest manifest;
    struct appliance_update_integrity_gate gate;
    struct appliance_update_bundle bundle;
    struct image_signature_store store;
    struct image_signature_store image_only;
    struct image_signature_anchor anchor;
    uint8_t tampered[APPLIANCE_UPDATE_BUNDLE_SIGNATURE_SIZE];

    host_manifest(&manifest);
    host_gate(&gate);
    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK,
          "the declared trust set binds");

    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, host_signature,
              sizeof(host_signature)) == APPLIANCE_UPDATE_BUNDLE_OK,
          "the development signature verifies under the update purpose");
    CHECK(appliance_update_bundle_is_verified(&bundle) &&
              bundle.verified && bundle.digest[0] == host_digest[0] &&
              memcmp(bundle.digest, host_digest, SHA256_DIGEST_SIZE) == 0 &&
              bundle.signer_id == IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
          "the verified bundle publishes the signer and the bound digest");

    /* A flipped signature bit never verifies. */
    memcpy(tampered, host_signature, sizeof(tampered));
    tampered[0] ^= 0x01U;
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, tampered,
              sizeof(tampered)) ==
              APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID,
          "a tampered signature is refused");
    memcpy(tampered, host_signature, sizeof(tampered));
    tampered[63] ^= 0x80U;
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, tampered,
              sizeof(tampered)) ==
              APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID,
          "a tampered signature tail is refused");

    /* The signature is bound to the exact manifest identity. */
    manifest.compatibility.target_image_id = 3U;
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, host_signature,
              sizeof(host_signature)) ==
              APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID,
          "a rewritten target image invalidates the signature");
    host_manifest(&manifest);

    /* A truncated or absent signature is not a signature. */
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, host_signature,
              sizeof(host_signature) - 1U) ==
              APPLIANCE_UPDATE_BUNDLE_SIGNATURE_INVALID,
          "a truncated signature is refused");
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, NULL, 0U) ==
              APPLIANCE_UPDATE_BUNDLE_UNSIGNED,
          "an unsigned bundle is refused");

    /* A signer the trust set does not know is refused. */
    gate.signer_id = 42U;
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &store, host_signature,
              sizeof(host_signature)) ==
              APPLIANCE_UPDATE_BUNDLE_SIGNATURE_UNAVAILABLE,
          "an unknown signer is refused");
    host_gate(&gate);

    /* An anchor that does not accept the update purpose denies it. */
    CHECK(image_signature_store_init(&image_only, false) ==
              IMAGE_SIGNATURE_OK,
          "the image-only trust set binds");
    memset(&anchor, 0, sizeof(anchor));
    anchor.signer_id = IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID;
    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    anchor.accepted_purposes = KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memcpy(anchor.public_key,
           image_signature_store_find(&store,
                                      IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID)
               ->public_key,
           IMAGE_SIGNATURE_PUBLIC_KEY_SIZE);
    CHECK(image_signature_store_add(&image_only, &anchor) ==
              IMAGE_SIGNATURE_OK,
          "the image-only anchor is added");
    CHECK(appliance_update_bundle_verify(
              &bundle, &manifest, &gate, &image_only, host_signature,
              sizeof(host_signature)) ==
              APPLIANCE_UPDATE_BUNDLE_PURPOSE_DENIED,
          "an anchor without the update purpose denies the bundle");

    CHECK(appliance_update_bundle_verify(
              NULL, &manifest, &gate, &store, host_signature,
              sizeof(host_signature)) == APPLIANCE_UPDATE_BUNDLE_NULL &&
              appliance_update_bundle_verify(
                  &bundle, NULL, &gate, &store, host_signature,
                  sizeof(host_signature)) ==
                  APPLIANCE_UPDATE_BUNDLE_NULL &&
              appliance_update_bundle_verify(
                  &bundle, &manifest, &gate, NULL, host_signature,
                  sizeof(host_signature)) ==
                  APPLIANCE_UPDATE_BUNDLE_NULL,
          "the verifier refuses null arguments");
}

/* 3. Ordered admission refuses downgrades and foreign sources. */
static void host_admission(void)
{
    struct appliance_update_manifest manifest;
    struct appliance_update_integrity_gate gate;
    struct appliance_update_bundle bundle;
    struct image_signature_store store;
    struct appliance_update_record record;
    struct appliance_update_record_entry known_good;

    host_manifest(&manifest);
    host_gate(&gate);
    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK &&
              appliance_update_bundle_verify(
                  &bundle, &manifest, &gate, &store, host_signature,
                  sizeof(host_signature)) == APPLIANCE_UPDATE_BUNDLE_OK,
          "the admission fixture verifies a bundle");

    memset(&record, 0, sizeof(record));
    memset(&known_good, 0, sizeof(known_good));
    record.version = APPLIANCE_UPDATE_RECORD_VERSION;
    record.installed_image_id = 1U;
    record.known_good = known_good;
    CHECK(appliance_update_bundle_admit(&bundle, &record) ==
              APPLIANCE_UPDATE_BUNDLE_OK,
          "a strictly newer bundle for the installed image is admitted");

    record.installed_image_id = 2U;
    CHECK(appliance_update_bundle_admit(&bundle, &record) ==
              APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED,
          "a bundle whose target is already installed is refused");
    record.installed_image_id = 3U;
    CHECK(appliance_update_bundle_admit(&bundle, &record) ==
              APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED,
          "a lower target is refused as a downgrade");

    record.installed_image_id = 1U;
    manifest.compatibility.source_image_id = 4U;
    manifest.compatibility.target_image_id = 5U;
    CHECK(appliance_update_bundle_bind(&manifest, &gate,
                                       &bundle.binding) ==
              APPLIANCE_UPDATE_BUNDLE_OK,
          "the source-mismatch binding is rebuilt");
    CHECK(appliance_update_bundle_admit(&bundle, &record) ==
              APPLIANCE_UPDATE_BUNDLE_SOURCE_MISMATCH,
          "a bundle built for another image is refused");

    /* An unverified bundle is never admitted. */
    bundle.verified = false;
    CHECK(appliance_update_bundle_admit(&bundle, &record) ==
              APPLIANCE_UPDATE_BUNDLE_NOT_VERIFIED,
          "an unverified bundle is never admitted");
    CHECK(appliance_update_bundle_admit(NULL, &record) ==
              APPLIANCE_UPDATE_BUNDLE_NULL &&
              appliance_update_bundle_admit(&bundle, NULL) ==
                  APPLIANCE_UPDATE_BUNDLE_NULL,
          "admission refuses null arguments");
    CHECK(!appliance_update_bundle_is_verified(NULL),
          "a null bundle is never verified");
    CHECK(strcmp(appliance_update_bundle_status_string(
                     APPLIANCE_UPDATE_BUNDLE_DOWNGRADE_REFUSED),
                 "DOWNGRADE_REFUSED") == 0 &&
              strcmp(appliance_update_bundle_status_string(
                         APPLIANCE_UPDATE_BUNDLE_PURPOSE_DENIED),
                     "PURPOSE_DENIED") == 0,
          "bundle status strings are exact");
}

int main(void)
{
    host_binding();
    host_signature_verification();
    host_admission();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance update bundle host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance update bundle host tests: ALL OK (%d checks)\n",
           checks);
    return 0;
}

#else
int appliance_update_bundle_host_translation_unit_marker;
#endif
