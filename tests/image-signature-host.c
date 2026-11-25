#ifdef HFOC_IMAGE_SIGNATURE_HOST_TEST

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

/*
 * ITEM-372 verifier vectors.
 *
 * VECTOR_DEV is signed by boot/keys/hfoc-development-signing-key.pem, the
 * declared development identity whose public half is compiled into the
 * declared trust set.  VECTOR_FIXTURE is signed by a proof-only key that
 * exists only in the harness; the matching public key is bound explicitly.
 */
static const unsigned char vector_dev_digest[SHA256_DIGEST_SIZE] = {
    0x44, 0x10, 0x2F, 0x67, 0xD9, 0x45, 0x28, 0xC2, 0xCB, 0xAD, 0x9C,
    0x8B, 0x95, 0x34, 0x8B, 0x54, 0x6C, 0xA4, 0xAB, 0x4A, 0xD8, 0xFB,
    0x5B, 0xAB, 0x5B, 0xCA, 0x98, 0xD1, 0x57, 0xDB, 0x93, 0x96
};
static const unsigned char vector_dev_signature[
    IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE] = {
    0x72, 0x39, 0x27, 0x52, 0x08, 0xC7, 0xD0, 0xFF, 0x23, 0xAD, 0x3C,
    0x4B, 0x97, 0x9A, 0x67, 0x72, 0x29, 0xC3, 0x81, 0xFC, 0x57, 0x6E,
    0x4A, 0x3D, 0x31, 0x12, 0xA3, 0x0E, 0xA3, 0x4E, 0x02, 0x44, 0x44,
    0xA7, 0x87, 0x27, 0x66, 0x7B, 0x90, 0xE1, 0x50, 0xE7, 0x90, 0xE2,
    0x48, 0x2E, 0x3C, 0x25, 0x20, 0x73, 0x30, 0x4A, 0x93, 0x11, 0xD8,
    0x22, 0xEF, 0x06, 0x2D, 0x47, 0x16, 0x08, 0x10, 0x9A
};
static const unsigned char vector_fixture_digest[SHA256_DIGEST_SIZE] = {
    0x99, 0xED, 0x9B, 0x85, 0x8C, 0x91, 0xEE, 0xDF, 0x6A, 0x64, 0x22,
    0xAE, 0xF4, 0x92, 0x2E, 0xA1, 0xEB, 0x5F, 0xEB, 0x3C, 0x87, 0x83,
    0x0C, 0xCB, 0xB2, 0x69, 0x7F, 0x17, 0x09, 0xF2, 0x64, 0xA5
};
static const unsigned char vector_fixture_signature[
    IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE] = {
    0x28, 0xE6, 0x9C, 0x52, 0x64, 0x67, 0xBE, 0xE2, 0xA7, 0x44, 0xFA,
    0x6A, 0x79, 0x41, 0x7E, 0x40, 0x13, 0x2E, 0x1F, 0xC3, 0x75, 0xB9,
    0xB5, 0x5C, 0xB9, 0x8B, 0x3F, 0xD5, 0xCD, 0xEA, 0xEA, 0xAD, 0x30,
    0x1F, 0x13, 0x53, 0xE6, 0x75, 0x5D, 0x57, 0xD1, 0x29, 0x58, 0x4F,
    0x23, 0x04, 0x26, 0x85, 0xD4, 0x95, 0x27, 0xBA, 0x76, 0x73, 0x0B,
    0xCA, 0x51, 0xDE, 0xAF, 0x22, 0xAC, 0x52, 0xC4, 0xDC
};
static const unsigned char vector_fixture_public_key[
    IMAGE_SIGNATURE_PUBLIC_KEY_SIZE] = {
    0x04, 0x3B, 0xF0, 0xF4, 0xF2, 0x7B, 0xFE, 0x4E, 0xF1, 0x53, 0xCC,
    0x86, 0xDD, 0x19, 0x52, 0x74, 0xBD, 0x8D, 0xE4, 0x5F, 0x8E, 0xF5,
    0x29, 0x68, 0x40, 0xB7, 0x48, 0x2D, 0x9C, 0xE6, 0xE7, 0x74, 0x06,
    0xDB, 0xF9, 0xC5, 0x4C, 0x73, 0xE7, 0xA7, 0x7A, 0x9C, 0x30, 0x15,
    0x81, 0x16, 0x9C, 0x3B, 0x4A, 0x32, 0x90, 0xC5, 0xB5, 0xBD, 0x1A,
    0x6A, 0xD3, 0x78, 0xB2, 0x64, 0x61, 0x82, 0xAB, 0xF0, 0xAE
};

static void image_signature_host_declared_store(void)
{
    struct image_signature_store store;

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK,
          "declared trust set binds");
    CHECK(store.version == IMAGE_SIGNATURE_VERSION && store.bound,
          "declared trust set is versioned and bound");
    CHECK(image_signature_store_find(
              &store, IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID) != NULL,
          "development anchor is present in a development build");
    CHECK(image_signature_store_find(
              &store, IMAGE_SIGNATURE_RELEASE_SIGNER_ID) != NULL,
          "release anchor is declared");
    CHECK(image_signature_store_find(&store, 3U) == NULL,
          "unknown signer is not trusted");

    CHECK(image_signature_store_declared(&store, true) ==
              IMAGE_SIGNATURE_OK,
          "release-only trust set binds");
    CHECK(image_signature_store_find(
              &store, IMAGE_SIGNATURE_RELEASE_SIGNER_ID) != NULL,
          "release-only trust set keeps the release anchor");
    CHECK(store.release_only,
          "release-only trust set records the release-only policy");
}

static void image_signature_host_real_vectors(void)
{
    struct image_signature_store store;
    struct image_signature_anchor anchor;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char signature[IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE];

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK,
          "declared trust set binds for the development vector");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              vector_dev_signature, sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_OK,
          "development-signed digest verifies against the declared anchor");

    memcpy(digest, vector_dev_digest, sizeof(digest));
    digest[0] ^= 0x01U;
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, digest,
              vector_dev_signature, sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_MISMATCH,
          "tampered digest does not verify");

    memcpy(signature, vector_dev_signature, sizeof(signature));
    signature[7] ^= 0x80U;
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              signature, sizeof(signature)) ==
              IMAGE_SIGNATURE_MISMATCH,
          "tampered signature does not verify");

    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              vector_dev_signature, sizeof(vector_dev_signature) - 1U) ==
              IMAGE_SIGNATURE_MALFORMED_SIGNATURE,
          "short signature is malformed");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              vector_dev_signature, sizeof(vector_dev_signature) + 1U) ==
              IMAGE_SIGNATURE_MALFORMED_SIGNATURE,
          "long signature is malformed");

    memset(signature, 0, sizeof(signature));
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              signature, sizeof(signature)) ==
              IMAGE_SIGNATURE_MALFORMED_SIGNATURE,
          "all-zero r and s are rejected before curve arithmetic");

    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ED25519,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              vector_dev_signature, sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM,
          "a different algorithm is refused");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256, 3U,
              vector_dev_digest, vector_dev_signature,
              sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_UNKNOWN_SIGNER,
          "an unknown signer is refused");
    CHECK(image_signature_verify(
              NULL, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, vector_dev_digest,
              vector_dev_signature, sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_NULL,
          "a null store is refused");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
              IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID, NULL,
              vector_dev_signature, sizeof(vector_dev_signature)) ==
              IMAGE_SIGNATURE_NULL,
          "a null digest is refused");

    memset(&anchor, 0, sizeof(anchor));
    anchor.signer_id = 7U;
    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    anchor.accepted_purposes = KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memcpy(anchor.public_key, vector_fixture_public_key,
           sizeof(anchor.public_key));
    CHECK(image_signature_store_init(&store, false) == IMAGE_SIGNATURE_OK,
          "explicit trust set binds");
    CHECK(image_signature_store_add(&store, &anchor) == IMAGE_SIGNATURE_OK,
          "explicit anchor is accepted");
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_DUPLICATE_ANCHOR,
          "a duplicate signer id is refused");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256, 7U,
              vector_fixture_digest, vector_fixture_signature,
              sizeof(vector_fixture_signature)) == IMAGE_SIGNATURE_OK,
          "fixture-signed digest verifies against the bound anchor");
    CHECK(image_signature_verify(
              &store, TRUST_ANCHOR_ALGORITHM_ECDSA_P256, 7U,
              vector_dev_digest, vector_fixture_signature,
              sizeof(vector_fixture_signature)) ==
              IMAGE_SIGNATURE_MISMATCH,
          "a signature does not verify a different digest");
}

static void image_signature_host_anchor_validation(void)
{
    struct image_signature_store store;
    struct image_signature_anchor anchor;

    CHECK(image_signature_store_init(NULL, false) == IMAGE_SIGNATURE_NULL,
          "null store init is refused");
    CHECK(image_signature_store_init(&store, false) == IMAGE_SIGNATURE_OK,
          "explicit store init binds");

    memset(&anchor, 0, sizeof(anchor));
    anchor.signer_id = 9U;
    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.accepted_purposes = KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM,
          "an unsupported anchor algorithm is refused");

    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    anchor.accepted_purposes = 0U;
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_BAD_ANCHOR,
          "an anchor without purposes is refused");

    anchor.accepted_purposes = KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    anchor.signer_id = 0U;
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_BAD_ANCHOR,
          "an out-of-range signer id is refused");

    anchor.signer_id = 9U;
    anchor.public_key[0] = 0x02U;
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_UNSUPPORTED_KEY,
          "an uncompressed-only key policy is enforced");

    anchor.public_key[0] = 0x04U;
    for (anchor.signer_id = 10U; anchor.signer_id < 14U;
         ++anchor.signer_id) {
        CHECK(image_signature_store_add(&store, &anchor) ==
                  IMAGE_SIGNATURE_OK,
              "anchors fill the bounded registry");
    }
    anchor.signer_id = 14U;
    CHECK(image_signature_store_add(&store, &anchor) ==
              IMAGE_SIGNATURE_CAPACITY,
          "the bounded registry refuses overflow");
}

static void image_signature_host_policy_binding(void)
{
    struct image_signature_store store;
    struct verification_policy policy;
    const struct verification_policy_snapshot *active;
    struct signed_metadata_layout layout;
    unsigned char metadata[8] = { 'b', 'o', 'o', 't', '-', 'v', '1', '!' };
    unsigned char envelope[128];
    size_t envelope_length = 0U;

    CHECK(image_signature_store_declared(&store, false) ==
              IMAGE_SIGNATURE_OK,
          "declared trust set binds for policy construction");
    CHECK(image_signature_policy_init(&store, &policy, 1U) ==
              IMAGE_SIGNATURE_OK,
          "verification policy is built from the trust set");
    active = verification_policy_active(&policy);
    CHECK(active != NULL && active->generation == 1U,
          "the built policy is active");
    CHECK(active != NULL && active->anchor_count == store.anchor_count,
          "the built policy carries every anchor");
    CHECK(image_signature_policy_init(NULL, &policy, 1U) ==
              IMAGE_SIGNATURE_NULL,
          "a null store cannot build a policy");

    CHECK(signed_metadata_seal(&policy, KEY_USE_PURPOSE_IMAGE,
                               IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                               metadata, sizeof(metadata), envelope,
                               sizeof(envelope), &envelope_length) ==
              SIGNED_METADATA_OK,
          "the built policy accepts the development signer for image media");
    CHECK(signed_metadata_verify(&policy, KEY_USE_PURPOSE_IMAGE, envelope,
                                 envelope_length, &layout) ==
              SIGNED_METADATA_OK,
          "the sealed envelope verifies under the built policy");
    /* ITEM-376: the declared identities are authorised for update bundles
     * as well as images, and still deny every other purpose. */
    CHECK(signed_metadata_seal(&policy, KEY_USE_PURPOSE_UPDATE,
                               IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                               metadata, sizeof(metadata), envelope,
                               sizeof(envelope), &envelope_length) ==
              SIGNED_METADATA_OK,
          "the built policy accepts the development signer for updates");
    CHECK(signed_metadata_seal(&policy, KEY_USE_PURPOSE_BACKUP,
                               IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID,
                               metadata, sizeof(metadata), envelope,
                               sizeof(envelope), &envelope_length) ==
              SIGNED_METADATA_PURPOSE_DENIED,
          "the built policy denies a purpose the anchor does not accept");
}

int main(void)
{
    image_signature_host_declared_store();
    image_signature_host_real_vectors();
    image_signature_host_anchor_validation();
    image_signature_host_policy_binding();

    if (failures != 0) {
        fprintf(stderr, "Image signature host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("Image signature host tests: ALL OK (%d checks)\n", checks);
    return 0;
}

#else
int image_signature_host_translation_unit_marker;
#endif
