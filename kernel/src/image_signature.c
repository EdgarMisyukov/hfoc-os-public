#include "image_signature.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Declared trust anchors.
 *
 * The release anchor is the production signing identity.  Only its public
 * half exists in this repository; the release ceremony signs media with the
 * offline private half (ITEM-380 owns that ceremony).  The development
 * anchor pairs with boot/keys/hfoc-development-signing-key.pem, a declared
 * non-secret build identity used for development media and QEMU proofs; a
 * release build (HFOC_RELEASE_BUILD) never compiles it in.
 */
static const unsigned char image_signature_release_public_key[
    IMAGE_SIGNATURE_PUBLIC_KEY_SIZE] = {
    0x04, 0x18, 0x4A, 0xB3, 0x5E, 0xFD, 0xE4, 0x6C, 0x99, 0x37, 0xA1,
    0x6D, 0xA5, 0x70, 0x8F, 0xEB, 0x73, 0x57, 0x0C, 0xC5, 0x25, 0x6E,
    0x6B, 0xE9, 0x56, 0xA3, 0x6D, 0x46, 0x0F, 0xE7, 0x6C, 0xAD, 0x01,
    0x78, 0x4E, 0xF1, 0x99, 0x4D, 0x9D, 0xC0, 0x37, 0xB4, 0x89, 0x6E,
    0x4B, 0xDD, 0x31, 0x47, 0x64, 0x19, 0x05, 0xDD, 0x55, 0xBC, 0xE1,
    0xFF, 0x95, 0x99, 0x55, 0x16, 0xD1, 0x74, 0xD7, 0x41, 0x61
};

#ifndef HFOC_RELEASE_BUILD
static const unsigned char image_signature_development_public_key[
    IMAGE_SIGNATURE_PUBLIC_KEY_SIZE] = {
    0x04, 0xB9, 0x1F, 0x6E, 0x7F, 0xB5, 0xB7, 0xCD, 0x7B, 0x4A, 0x29,
    0x79, 0x4B, 0x5A, 0x4B, 0xE5, 0x70, 0x90, 0x60, 0xB3, 0x81, 0x7C,
    0x1B, 0xDF, 0x8A, 0x07, 0xBB, 0x21, 0x64, 0xCD, 0xC1, 0x36, 0x14,
    0x7D, 0x99, 0xCE, 0x86, 0xF5, 0xD2, 0xF6, 0xCB, 0x06, 0xCB, 0xE4,
    0x71, 0x42, 0x9A, 0x78, 0xD3, 0xC0, 0x6B, 0x3E, 0x33, 0x94, 0x27,
    0xC9, 0x06, 0x9A, 0x58, 0xCE, 0xA8, 0x84, 0x3E, 0x02, 0x6C
};
#endif

static const char *const image_signature_status_names[] = {
    "OK",
    "NULL",
    "BAD_STATE",
    "BAD_VERSION",
    "BAD_ANCHOR",
    "DUPLICATE_ANCHOR",
    "CAPACITY",
    "UNKNOWN_SIGNER",
    "PURPOSE_DENIED",
    "UNSUPPORTED_ALGORITHM",
    "UNSUPPORTED_KEY",
    "MALFORMED_SIGNATURE",
    "MISMATCH",
    "UNAVAILABLE",
};

/*
 * The arithmetic backend lives in image_signature_ecdsa.c, which only the
 * verification kernels link (the production kernel and the boot-chain proof
 * kernel).  Everywhere else this weak default fails closed: a kernel
 * without the backend reports UNAVAILABLE and never accepts a signature.
 */
__attribute__((weak))
enum image_signature_status image_signature_ecdsa_p256_verify(
    const unsigned char public_key[IMAGE_SIGNATURE_PUBLIC_KEY_SIZE],
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char signature[IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE])
{
    (void)public_key;
    (void)digest;
    (void)signature;
    return IMAGE_SIGNATURE_UNAVAILABLE;
}

static enum image_signature_status image_signature_anchor_validate(
    const struct image_signature_anchor *anchor)
{
    if (anchor == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    if (anchor->signer_id < TRUST_ANCHOR_ID_MIN ||
        anchor->signer_id > TRUST_ANCHOR_ID_MAX) {
        return IMAGE_SIGNATURE_BAD_ANCHOR;
    }
    if (anchor->algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM;
    }
    if (!key_use_accepted_purposes_valid(anchor->accepted_purposes) ||
        anchor->accepted_purposes == 0U) {
        return IMAGE_SIGNATURE_BAD_ANCHOR;
    }
    if (anchor->public_key[0] != 0x04U) {
        return IMAGE_SIGNATURE_UNSUPPORTED_KEY;
    }
    return IMAGE_SIGNATURE_OK;
}

enum image_signature_status image_signature_store_init(
    struct image_signature_store *store,
    bool release_only)
{
    if (store == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    memset(store, 0, sizeof(*store));
    store->version = IMAGE_SIGNATURE_VERSION;
    store->release_only = release_only;
    store->bound = true;
    return IMAGE_SIGNATURE_OK;
}

enum image_signature_status image_signature_store_add(
    struct image_signature_store *store,
    const struct image_signature_anchor *anchor)
{
    enum image_signature_status status;

    if (store == NULL || anchor == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    if (!store->bound || store->version != IMAGE_SIGNATURE_VERSION) {
        return IMAGE_SIGNATURE_BAD_STATE;
    }
    status = image_signature_anchor_validate(anchor);
    if (status != IMAGE_SIGNATURE_OK) {
        return status;
    }
    if (store->anchor_count >= IMAGE_SIGNATURE_ANCHOR_CAPACITY) {
        return IMAGE_SIGNATURE_CAPACITY;
    }
    if (image_signature_store_find(store, anchor->signer_id) != NULL) {
        return IMAGE_SIGNATURE_DUPLICATE_ANCHOR;
    }
    store->anchors[store->anchor_count] = *anchor;
    ++store->anchor_count;
    return IMAGE_SIGNATURE_OK;
}

const struct image_signature_anchor *image_signature_store_find(
    const struct image_signature_store *store,
    uint32_t signer_id)
{
    size_t index;

    if (store == NULL || !store->bound ||
        store->version != IMAGE_SIGNATURE_VERSION) {
        return NULL;
    }
    for (index = 0U; index < store->anchor_count; ++index) {
        if (store->anchors[index].signer_id == signer_id) {
            return &store->anchors[index];
        }
    }
    return NULL;
}

enum image_signature_status image_signature_store_declared(
    struct image_signature_store *store,
    bool release_only)
{
    struct image_signature_anchor anchor;
    enum image_signature_status status;

    status = image_signature_store_init(store, release_only);
    if (status != IMAGE_SIGNATURE_OK) {
        return status;
    }

    memset(&anchor, 0, sizeof(anchor));
    anchor.signer_id = IMAGE_SIGNATURE_RELEASE_SIGNER_ID;
    anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    anchor.accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE) |
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE);
    memcpy(anchor.public_key, image_signature_release_public_key,
           sizeof(anchor.public_key));
    status = image_signature_store_add(store, &anchor);
    if (status != IMAGE_SIGNATURE_OK) {
        return status;
    }

#ifndef HFOC_RELEASE_BUILD
    if (!release_only) {
        memset(&anchor, 0, sizeof(anchor));
        anchor.signer_id = IMAGE_SIGNATURE_DEVELOPMENT_SIGNER_ID;
        anchor.algorithm = TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
        anchor.accepted_purposes =
            KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE) |
            KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE);
        memcpy(anchor.public_key,
               image_signature_development_public_key,
               sizeof(anchor.public_key));
        status = image_signature_store_add(store, &anchor);
        if (status != IMAGE_SIGNATURE_OK) {
            return status;
        }
    }
#else
    (void)release_only;
#endif

    if (store->anchor_count == 0U) {
        return IMAGE_SIGNATURE_UNAVAILABLE;
    }
    return IMAGE_SIGNATURE_OK;
}

enum image_signature_status image_signature_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    return image_signature_verify_purpose(
        context, algorithm, signer_id, KEY_USE_PURPOSE_IMAGE, digest,
        signature, signature_length);
}

enum image_signature_status image_signature_verify_purpose(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    uint32_t purpose,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    const struct image_signature_store *store =
        (const struct image_signature_store *)context;
    const struct image_signature_anchor *anchor;

    if (store == NULL || digest == NULL || signature == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    if (!store->bound || store->version != IMAGE_SIGNATURE_VERSION) {
        return IMAGE_SIGNATURE_BAD_STATE;
    }
    anchor = image_signature_store_find(store, signer_id);
    if (anchor == NULL) {
        return IMAGE_SIGNATURE_UNKNOWN_SIGNER;
    }
    if (anchor->algorithm != algorithm) {
        return IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM;
    }
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return IMAGE_SIGNATURE_UNSUPPORTED_ALGORITHM;
    }
    if (signature_length != IMAGE_SIGNATURE_RAW_SIGNATURE_SIZE) {
        return IMAGE_SIGNATURE_MALFORMED_SIGNATURE;
    }
    if (!key_use_purpose_valid(purpose) ||
        !key_use_purpose_is_accepted(anchor->accepted_purposes,
                                     purpose)) {
        return IMAGE_SIGNATURE_PURPOSE_DENIED;
    }
    return image_signature_ecdsa_p256_verify(
        anchor->public_key, digest, signature);
}

enum image_signature_status image_signature_policy_init(
    const struct image_signature_store *store,
    struct verification_policy *policy,
    uint32_t generation)
{
    struct verification_policy_snapshot snapshot;
    size_t index;

    if (store == NULL || policy == NULL) {
        return IMAGE_SIGNATURE_NULL;
    }
    if (!store->bound || store->version != IMAGE_SIGNATURE_VERSION ||
        store->anchor_count == 0U) {
        return IMAGE_SIGNATURE_BAD_STATE;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = (uint32_t)store->anchor_count;
    for (index = 0U; index < store->anchor_count; ++index) {
        snapshot.anchors[index].id = store->anchors[index].signer_id;
        snapshot.anchors[index].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
        snapshot.anchors[index].algorithm_id =
            store->anchors[index].algorithm;
        snapshot.anchors[index].state = TRUST_ANCHOR_STATE_ACTIVE;
        snapshot.anchors[index].accepted_purposes =
            store->anchors[index].accepted_purposes;
    }
    /* The store is the single source of truth for the policy: rebuild the
     * two-slot policy from scratch so a caller cannot inherit a stale
     * active or pending snapshot. */
    memset(policy, 0, sizeof(*policy));
    if (verification_policy_init(policy, &snapshot) !=
        VERIFICATION_POLICY_OK) {
        return IMAGE_SIGNATURE_BAD_ANCHOR;
    }
    return IMAGE_SIGNATURE_OK;
}

const char *image_signature_status_string(
    enum image_signature_status status)
{
    if ((size_t)status >=
        sizeof(image_signature_status_names) /
            sizeof(image_signature_status_names[0])) {
        return "UNKNOWN";
    }
    return image_signature_status_names[status];
}
