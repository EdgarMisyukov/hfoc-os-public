#ifndef HFOC_INSTALLER_MEDIA_VERIFICATION_H
#define HFOC_INSTALLER_MEDIA_VERIFICATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha256.h"
#include "trust_anchor.h"
#include "verification_policy.h"

/*
 * ITEM-371 signed-media gate.
 *
 * The installation media carries a manifest sealed in the existing
 * HFOCSIGN signed_metadata envelope.  The manifest declares the release
 * identity, the exact media length, the SHA-256 digest of the whole media,
 * the exact length and SHA-256 digest of the installer source payloads that
 * are about to be written, and an asymmetric signature over the manifest.
 * The installer may only write to a disk after this module reports that the
 * envelope is policy accepted, the manifest signature verifies, the media
 * bytes match the signed media digest and the source payloads match the
 * signed source digest.
 *
 * The asymmetric verification itself is a caller-bound cryptographic
 * boundary: the key material and the ECDSA/EdDSA verifier arrive with the
 * signed boot chain (ITEM-372).  When no verifier is bound the module
 * fails closed with INSTALLER_MEDIA_VERIFIER_UNAVAILABLE and never
 * authorizes a write.
 */

#define INSTALLER_MEDIA_VERIFICATION_VERSION 1U
#define INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE 8U
#define INSTALLER_MEDIA_MANIFEST_FORMAT_VERSION 1U
#define INSTALLER_MEDIA_MANIFEST_HEADER_SIZE 96U
#define INSTALLER_MEDIA_SIGNATURE_MIN 1U
#define INSTALLER_MEDIA_SIGNATURE_MAX 256U
#define INSTALLER_MEDIA_MANIFEST_MIN_LENGTH \
    (INSTALLER_MEDIA_MANIFEST_HEADER_SIZE + INSTALLER_MEDIA_SIGNATURE_MIN)
#define INSTALLER_MEDIA_MANIFEST_MAX_LENGTH \
    (INSTALLER_MEDIA_MANIFEST_HEADER_SIZE + INSTALLER_MEDIA_SIGNATURE_MAX)
#define INSTALLER_MEDIA_MAGIC_BYTES \
    { 'H', 'F', 'O', 'C', 'M', 'E', 'D', '1' }
#define INSTALLER_MEDIA_RELEASE_ID_MIN 1U

enum installer_media_verification_status {
    INSTALLER_MEDIA_OK = 0,
    INSTALLER_MEDIA_NULL = 1,
    INSTALLER_MEDIA_BAD_VERSION = 2,
    INSTALLER_MEDIA_ENVELOPE_INVALID = 3,
    INSTALLER_MEDIA_POLICY_MISMATCH = 4,
    INSTALLER_MEDIA_MANIFEST_INVALID = 5,
    INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED = 6,
    INSTALLER_MEDIA_VERIFIER_UNAVAILABLE = 7,
    INSTALLER_MEDIA_SIGNATURE_INVALID = 8,
    INSTALLER_MEDIA_DIGEST_MISMATCH = 9,
    INSTALLER_MEDIA_LENGTH_MISMATCH = 10,
    INSTALLER_MEDIA_RELEASE_MISMATCH = 11,
    INSTALLER_MEDIA_SOURCE_INVALID = 12,
    INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH = 13,
    INSTALLER_MEDIA_SOURCE_DIGEST_MISMATCH = 14,
    INSTALLER_MEDIA_CAPACITY = 15,
};

/* The signature verifier answers one question: does signature verify the
 * signed manifest digest under algorithm for the accepted signer?  Any
 * other answer, including an unavailable verifier, denies the write. */
enum installer_media_signature_status {
    INSTALLER_MEDIA_SIGNATURE_OK = 0,
    INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM = 1,
    INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE = 2,
    INSTALLER_MEDIA_SIGNATURE_MISMATCH = 3,
};

typedef enum installer_media_signature_status
(*installer_media_signature_verify_fn)(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length);

struct installer_media_verifier {
    void *context;
    installer_media_signature_verify_fn verify;
};

/* The installer source payloads that are about to be written to the target
 * disk.  The signed manifest binds their exact concatenated length and
 * SHA-256 digest, so a verified media digest cannot authorize installing
 * different bytes. */
struct installer_media_source {
    const void *boot_payload;
    size_t boot_payload_size;
    const void *limine_config;
    size_t limine_config_size;
    const void *kernel;
    size_t kernel_size;
};

struct installer_media_verification_report {
    uint32_t version;
    bool envelope_verified;
    bool signature_verified;
    bool digest_verified;
    bool release_verified;
    bool length_verified;
    bool source_verified;
    bool write_authorized;
    uint32_t release_id;
    uint32_t signer_id;
    enum trust_anchor_algorithm algorithm;
    uint64_t media_length;
    uint64_t source_length;
    unsigned char media_digest[SHA256_DIGEST_SIZE];
    unsigned char source_digest[SHA256_DIGEST_SIZE];
};

enum installer_media_verification_status
installer_media_source_digest(
    const struct installer_media_source *source,
    unsigned char digest[SHA256_DIGEST_SIZE],
    uint64_t *length_out);
enum installer_media_verification_status
installer_media_manifest_seal(
    const struct verification_policy *policy,
    uint32_t release_id,
    uint32_t signer_id,
    enum trust_anchor_algorithm algorithm,
    const unsigned char media_digest[SHA256_DIGEST_SIZE],
    uint64_t media_length,
    const unsigned char
        source_digest[SHA256_DIGEST_SIZE],
    uint64_t source_length,
    const unsigned char *signature,
    size_t signature_length,
    void *buffer,
    size_t capacity,
    size_t *length_out);
enum installer_media_verification_status
installer_media_verification_run(
    const struct verification_policy *policy,
    const struct installer_media_verifier *verifier,
    uint32_t expected_release_id,
    const void *envelope,
    size_t envelope_length,
    const void *media,
    size_t media_length,
    const struct installer_media_source *source,
    struct installer_media_verification_report *report);
bool installer_media_verification_authorizes_write(
    const struct installer_media_verification_report *report);
const char *installer_media_verification_status_string(
    enum installer_media_verification_status status);
const char *installer_media_signature_status_string(
    enum installer_media_signature_status status);

#endif
