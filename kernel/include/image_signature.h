#ifndef HFOC_IMAGE_SIGNATURE_H
#define HFOC_IMAGE_SIGNATURE_H

#include "key_use.h"
#include "sha256.h"
#include "trust_anchor.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-372 production asymmetric verifier.
 *
 * The repository had no asymmetric signature verification before this item:
 * signed_metadata is a keyed-policy SHA-256 envelope and struct trust_anchor
 * carries no key material.  This module is the missing cryptographic
 * boundary.  It owns a bounded set of trusted public keys (anchors) and
 * verifies a raw 64-byte ECDSA P-256 signature (r || s, big endian) over a
 * 32-byte SHA-256 digest with the vendored Mbed TLS implementation.
 *
 * Key custody:
 *   - the release anchor is the declared production signing identity; its
 *     private half is not in this repository and never is;
 *   - the development anchor is a declared non-secret build/test identity,
 *     compiled in only when HFOC_RELEASE_BUILD is not defined; release
 *     builds trust the release anchor only.
 *
 * Fail closed: an unknown signer, a denied purpose, an unsupported
 * algorithm or key, an unavailable verifier and a malformed or mismatching
 * signature all refuse.  A caller never receives IMAGE_SIGNATURE_OK from a
 * partial check.
 */

#define IMAGE_SIGNATURE_VERSION 1U
#define IMAGE_SIGNATURE_PUBLIC_KEY_SIZE 65U
#define IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE 64U
#define IMAGE_SIGNATURE_ANCHOR_CAPACITY 4U
#define IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID 1U
#define IMAGE_SIGNATURE_RELEASE_SIGNER_ID 2U

enum image_signature_status {
    IMAGE_SIGNATURE_OK = 0,
    IMAGE_SIGNATURE_NULL = 1,
    IMAGE_SIGNATURE_BAD_STATE = 2,
    IMAGE_SIGNATURE_BAD_VERSION = 3,
    IMAGE_SIGNATURE_BAD_ANCHOR = 4,
    IMAGE_SIGNATURE_DUPLICATE_ANCHOR = 5,
    IMAGE_SIGNATURE_CAPACITY = 6,
    IMAGE_SIGNATURE_UNKNOWN_SIGNER = 7,
    IMAGE_SIGNATURE_PURPOSE_DENIED = 8,
    IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM = 9,
    IMAGE_SIGNATURE_UNSUPPORTED_KEY = 10,
    IMAGE_SIGNATURE_MALFORMED_SIGNATURE = 11,
    IMAGE_SIGNATURE_MISMATCH = 12,
    IMAGE_SIGNATURE_UNAVAILABLE = 13,
};

/* The cryptographic boundary every signed manifest consumes.  The verifier
 * answers one question: does signature verify digest for signer_id under
 * algorithm?  Any other answer denies. */
typedef enum image_signature_status (*image_signature_verify_fn)(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length);
/* The same verifier for a declared key-use purpose.  A signed manifest that
 * is not an image (an update bundle) must not be verified with the image
 * purpose, so its purpose is explicit.  The purpose comes from the caller,
 * never from the artifact, and the anchor must accept it. */
typedef enum image_signature_status (*image_signature_verify_purpose_fn)(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    uint32_t purpose,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length);

struct image_signature_anchor {
    uint32_t signer_id;
    enum trust_anchor_algorithm algorithm;
    uint32_t accepted_purposes;
    unsigned char public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE];
};

struct image_signature_store {
    uint32_t version;
    bool bound;
    bool release_only;
    size_t anchor_count;
    struct image_signature_anchor anchors[IMAGE_SIGNATURE_ANCHOR_CAPACITY];
};

/* Bounded anchor registry.  release_only selects the trust set: a release
 * store trusts only the declared release anchor. */
enum image_signature_status image_signature_store_init(
    struct image_signature_store *store,
    bool release_only);
enum image_signature_status image_signature_store_add(
    struct image_signature_store *store,
    const struct image_signature_anchor *anchor);
const struct image_signature_anchor *image_signature_store_find(
    const struct image_signature_store *store,
    uint32_t signer_id);

/* The declared trust set: release anchor always, development anchor only in
 * a non-release build. */
enum image_signature_status image_signature_store_declared(
    struct image_signature_store *store,
    bool release_only);

/* Real ECDSA P-256 verification over a SHA-256 digest.  The signature is
 * the raw 64-byte r || s form.  When the cryptographic backend is not
 * compiled in the call reports IMAGE_SIGNATURE_UNAVAILABLE. */
enum image_signature_status image_signature_ecdsa_p256_verify(
    const unsigned char public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE],
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char signature[IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE]);

/* Bind the store as a caller-bound verifier: the exact callback shape used
 * by the signed-media gate and by the boot-chain gate.  The context is the
 * store, which must outlive the verifier. */
enum image_signature_status image_signature_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length);
/* ITEM-376: purpose-parameterised verification.  image_signature_verify is
 * exactly this function with KEY_USE_PURPOSE_IMAGE, so existing callers and
 * proofs are unchanged. */
enum image_signature_status image_signature_verify_purpose(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    uint32_t purpose,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length);

/* Build the verification policy consumed by signed_metadata_verify() from
 * the store anchors: every anchor becomes an ACTIVE trust anchor that
 * accepts the image purpose. */
enum image_signature_status image_signature_policy_init(
    const struct image_signature_store *store,
    struct verification_policy *policy,
    uint32_t generation);

const char *image_signature_status_string(
    enum image_signature_status status);

#endif
