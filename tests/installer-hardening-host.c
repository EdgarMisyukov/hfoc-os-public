#include "installer_media_verification.h"
#include "installer_supported_hardware.h"
#include "installer_uninstall.h"
#include "production_installer.h"
#include "signed_metadata.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INSTALLER_HARDENING_HOST_TEST

#define MEDIA_LENGTH 512U
#define MEDIA_SIGNATURE_LENGTH 64U
#define MEDIA_RELEASE_ID 7U
#define MEDIA_SIGNER_ID 1U
#define MEDIA_DIGEST_SIZE 32U
#define MEDIA_ENVELOPE_CAPACITY 1024U
#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 4096U
#define TEST_IDENTITY "installer-hardening-disk"

static int failures;

static bool text_equals(const char *actual, const char *expected)
{
    return actual != NULL && strcmp(actual, expected) == 0;
}

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* signed media fixtures                                              */
/* ------------------------------------------------------------------ */

static unsigned char media_bytes[MEDIA_LENGTH];
static unsigned char media_envelope[MEDIA_ENVELOPE_CAPACITY];
static size_t media_envelope_length;
static struct verification_policy media_policy;
static struct verification_policy media_wide_policy;
static struct verification_policy media_multi_signer_policy;

static struct installer_media_source media_source_fixture(void)
{
    struct installer_media_source source;

    memset(&source, 0, sizeof(source));
    source.boot_payload = media_bytes;
    source.boot_payload_size = 16U;
    source.limine_config = media_bytes + 16U;
    source.limine_config_size = 13U;
    source.kernel = media_bytes + 29U;
    source.kernel_size = MEDIA_LENGTH - 29U;
    return source;
}

static void media_reset(void)
{
    size_t index;

    for (index = 0U; index < sizeof(media_bytes); ++index) {
        media_bytes[index] = (unsigned char)(index * 7U + 3U);
    }
    memset(media_envelope, 0, sizeof(media_envelope));
    media_envelope_length = 0U;
}

static void media_policy_build(
    struct verification_policy *policy,
    uint32_t anchor_id,
    uint32_t accepted_purposes)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = anchor_id;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes = accepted_purposes;
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &snapshot) ==
              VERIFICATION_POLICY_OK,
          "verification policy fixture initialized");
}

static void media_multi_signer_policy_build(
    struct verification_policy *policy)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 2U;
    snapshot.anchors[0U].id = MEDIA_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    snapshot.anchors[1U].id = 2U;
    snapshot.anchors[1U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[1U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[1U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[1U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &snapshot) ==
              VERIFICATION_POLICY_OK,
          "multi-signer policy fixture initialized");
}

static void media_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value);
    bytes[1U] = (unsigned char)(value >> 8U);
    bytes[2U] = (unsigned char)(value >> 16U);
    bytes[3U] = (unsigned char)(value >> 24U);
}

static void media_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (index * 8U));
    }
}

static void media_signature_build(
    const unsigned char digest[MEDIA_DIGEST_SIZE],
    unsigned char signature[MEDIA_SIGNATURE_LENGTH])
{
    size_t index;

    for (index = 0U; index < MEDIA_SIGNATURE_LENGTH; ++index) {
        signature[index] = (unsigned char)(
            digest[index % MEDIA_DIGEST_SIZE] ^ 0xA5U);
    }
}

static size_t media_manifest_build(
    unsigned char *metadata,
    size_t capacity,
    uint32_t release_id,
    uint64_t media_length,
    const unsigned char digest[MEDIA_DIGEST_SIZE],
    uint64_t source_length,
    const unsigned char source_digest[MEDIA_DIGEST_SIZE],
    enum trust_anchor_algorithm algorithm,
    const unsigned char *signature,
    size_t signature_length)
{
    static const unsigned char manifest_magic[
        INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE] =
        INSTALLER_MEDIA_MAGIC_BYTES;
    size_t metadata_length =
        INSTALLER_MEDIA_MANIFEST_HEADER_SIZE + signature_length;

    if (capacity < metadata_length ||
        signature_length < INSTALLER_MEDIA_SIGNATURE_MIN ||
        signature_length > INSTALLER_MEDIA_SIGNATURE_MAX) {
        return 0U;
    }
    memset(metadata, 0, metadata_length);
    memcpy(metadata, manifest_magic,
           INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE);
    metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE] =
        INSTALLER_MEDIA_MANIFEST_FORMAT_VERSION;
    metadata[INSTALLER_MEDIA_MANIFEST_MAGIC_SIZE + 1U] =
        (unsigned char)algorithm;
    media_store_le32(metadata + 12U, release_id);
    media_store_le64(metadata + 16U, media_length);
    memcpy(metadata + 24U, digest, MEDIA_DIGEST_SIZE);
    media_store_le64(metadata + 56U, source_length);
    memcpy(metadata + 64U, source_digest, MEDIA_DIGEST_SIZE);
    memcpy(metadata + INSTALLER_MEDIA_MANIFEST_HEADER_SIZE,
           signature, signature_length);
    return metadata_length;
}

static void media_seal(
    const struct verification_policy *policy,
    uint32_t purpose,
    uint32_t signer_id,
    uint32_t release_id,
    const void *media,
    size_t media_length,
    enum trust_anchor_algorithm algorithm,
    size_t signature_length,
    bool corrupt_manifest,
    bool corrupt_signature)
{
    unsigned char metadata[INSTALLER_MEDIA_MANIFEST_MAX_LENGTH];
    unsigned char digest[MEDIA_DIGEST_SIZE];
    unsigned char source_digest[MEDIA_DIGEST_SIZE];
    unsigned char signature[INSTALLER_MEDIA_SIGNATURE_MAX];
    struct installer_media_source source = media_source_fixture();
    uint64_t source_length = 0U;
    size_t metadata_length;
    size_t envelope_length = 0U;

    CHECK(sha256_compute(media, media_length, digest) == SHA256_OK,
          "media digest fixture computed");
    CHECK(installer_media_source_digest(
              &source, source_digest, &source_length) ==
              INSTALLER_MEDIA_OK,
          "media source digest fixture computed");
    memset(signature, 0, sizeof(signature));
    if (signature_length != 0U) {
        media_signature_build(digest, signature);
    }
    if (corrupt_signature && signature_length != 0U) {
        signature[0U] ^= 0xFFU;
    }
    metadata_length = media_manifest_build(
        metadata, sizeof(metadata), release_id, media_length,
        digest, source_length, source_digest, algorithm,
        signature, signature_length);
    CHECK(metadata_length != 0U, "media manifest fixture built");
    if (corrupt_manifest) {
        metadata[0U] ^= 0xFFU;
    }
    CHECK(signed_metadata_seal(
              policy, purpose, signer_id, metadata, metadata_length,
              media_envelope, sizeof(media_envelope),
              &envelope_length) == SIGNED_METADATA_OK,
          "media envelope fixture sealed");
    media_envelope_length = envelope_length;
}

static void media_seal_bound(
    const struct verification_policy *policy,
    uint32_t release_id,
    const struct installer_media_source *source)
{
    unsigned char metadata[INSTALLER_MEDIA_MANIFEST_MAX_LENGTH];
    unsigned char media_digest[MEDIA_DIGEST_SIZE];
    unsigned char source_digest[MEDIA_DIGEST_SIZE];
    unsigned char signature[MEDIA_SIGNATURE_LENGTH];
    uint64_t source_length = 0U;
    size_t metadata_length;
    size_t envelope_length = 0U;

    CHECK(sha256_compute(media_bytes, sizeof(media_bytes),
                         media_digest) == SHA256_OK,
          "media digest fixture computed");
    CHECK(installer_media_source_digest(
              source, source_digest, &source_length) ==
              INSTALLER_MEDIA_OK,
          "media source digest fixture computed");
    media_signature_build(media_digest, signature);
    metadata_length = media_manifest_build(
        metadata, sizeof(metadata), release_id, sizeof(media_bytes),
        media_digest, source_length, source_digest,
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        signature, MEDIA_SIGNATURE_LENGTH);
    CHECK(metadata_length != 0U, "media manifest fixture built");
    CHECK(installer_media_manifest_seal(
              policy, release_id, MEDIA_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, media_digest,
              sizeof(media_bytes), source_digest, source_length,
              signature, MEDIA_SIGNATURE_LENGTH, media_envelope,
              sizeof(media_envelope), &envelope_length) ==
              INSTALLER_MEDIA_OK,
          "media envelope fixture sealed");
    media_envelope_length = envelope_length;
}

static enum installer_media_signature_status media_test_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[MEDIA_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    unsigned char expected[MEDIA_SIGNATURE_LENGTH];
    size_t index;

    (void)context;
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256) {
        return INSTALLER_MEDIA_SIGNATURE_UNSUPPORTED_ALGORITHM;
    }
    if (signer_id != MEDIA_SIGNER_ID ||
        signature_length != MEDIA_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    media_signature_build(digest, expected);
    for (index = 0U; index < MEDIA_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != expected[index]) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

static struct installer_media_verifier media_verifier(void)
{
    struct installer_media_verifier verifier;

    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = media_test_verify;
    return verifier;
}

static struct installer_media_verification_report media_report(void)
{
    struct installer_media_verification_report report;

    memset(&report, 0, sizeof(report));
    return report;
}

static void test_media_verification(void)
{
    struct installer_media_verifier verifier = media_verifier();
    struct installer_media_verifier unavailable;
    struct installer_media_source source = media_source_fixture();
    struct installer_media_verification_report report;
    unsigned char tampered[MEDIA_LENGTH];

    memset(&unavailable, 0, sizeof(unavailable));
    media_policy_build(&media_policy, MEDIA_SIGNER_ID,
                       KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE));
    media_policy_build(&media_wide_policy, MEDIA_SIGNER_ID,
                       KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE) |
                           KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE));
    media_multi_signer_policy_build(&media_multi_signer_policy);
    media_reset();

    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_OK,
          "signed media verifies");
    CHECK(report.version == INSTALLER_MEDIA_VERIFICATION_VERSION &&
              report.envelope_verified &&
              report.signature_verified &&
              report.digest_verified &&
              report.release_verified &&
              report.length_verified &&
              report.write_authorized &&
              report.release_id == MEDIA_RELEASE_ID &&
              report.signer_id == MEDIA_SIGNER_ID &&
              report.algorithm == TRUST_ANCHOR_ALGORITHM_ECDSA_P256 &&
              report.media_length == MEDIA_LENGTH,
          "signed media report records every verified fact");
    CHECK(installer_media_verification_authorizes_write(&report),
          "verified media authorizes the write");

    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &unavailable, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_VERIFIER_UNAVAILABLE &&
              !report.write_authorized,
          "unbound verifier fails closed");

    report = media_report();
    media_bytes[0U] ^= 0x01U;
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_DIGEST_MISMATCH &&
              report.signature_verified && !report.digest_verified &&
              !report.write_authorized,
          "tampered media digest is refused");
    media_bytes[0U] ^= 0x01U;

    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes) - 1U, &source, &report) ==
              INSTALLER_MEDIA_LENGTH_MISMATCH &&
              !report.write_authorized,
          "truncated media length is refused");
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID + 1U,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_RELEASE_MISMATCH &&
              !report.write_authorized,
          "foreign release identity is refused");

    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length - 1U,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_ENVELOPE_INVALID &&
              !report.write_authorized,
          "truncated envelope is refused");
    report = media_report();
    media_envelope[8U] ^= 0xFFU;
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_ENVELOPE_INVALID &&
              !report.write_authorized,
          "tampered envelope is refused");
    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);

    report = media_report();
    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, true);
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_SIGNATURE_INVALID &&
              report.envelope_verified &&
              !report.signature_verified &&
              !report.write_authorized,
          "bad media signature is refused");
    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);

    report = media_report();
    media_seal(&media_wide_policy, KEY_USE_PURPOSE_UPDATE,
               MEDIA_SIGNER_ID, MEDIA_RELEASE_ID,
               media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_POLICY_MISMATCH &&
              !report.write_authorized,
          "envelope with a foreign purpose is refused");

    media_reset();
    media_seal(&media_multi_signer_policy, KEY_USE_PURPOSE_IMAGE, 2U,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_POLICY_MISMATCH &&
              !report.write_authorized,
          "envelope from an unknown signer is refused");

    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, true, false);
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_MANIFEST_INVALID &&
              !report.write_authorized,
          "corrupt media manifest is refused");

    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ED25519,
               MEDIA_SIGNATURE_LENGTH, false, false);
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_ALGORITHM_UNSUPPORTED &&
              !report.write_authorized,
          "unsupported media signature algorithm is refused");

    report = media_report();
    CHECK(installer_media_verification_run(
              NULL, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_NULL,
          "media verification rejects a null policy");
    report = media_report();
    report.write_authorized = true;
    report.digest_verified = true;
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              NULL, 0U, &source, &report) == INSTALLER_MEDIA_NULL &&
              !report.write_authorized && !report.digest_verified &&
              report.version == INSTALLER_MEDIA_VERIFICATION_VERSION,
          "media verification rejects empty media and clears the report");
    report = media_report();
    CHECK(installer_media_verification_run(
              NULL, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_NULL,
          "media verification rejects a null policy with a cleared report");
    CHECK(installer_media_verification_authorizes_write(NULL) == false,
          "no report authorizes no write");

    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);
    report = media_report();
    memcpy(tampered, media_bytes, sizeof(tampered));
    media_bytes[1U] ^= 0x80U;
    report.write_authorized = true;
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_DIGEST_MISMATCH &&
              !report.write_authorized,
          "a stale authorized report cannot survive a failed run");
    memcpy(media_bytes, tampered, sizeof(media_bytes));

    CHECK(text_equals(installer_media_verification_status_string(
                     INSTALLER_MEDIA_DIGEST_MISMATCH), "INSTALLER_MEDIA_DIGEST_MISMATCH") &&
              installer_media_verification_status_string(
                  (enum installer_media_verification_status)99) == NULL &&
              text_equals(installer_media_signature_status_string(
                         INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE), "INSTALLER_MEDIA_SIGNATURE_UNAVAILABLE"),
          "media status strings are exact");
}

static void test_media_manifest_seal(void)
{
    struct installer_media_verifier verifier = media_verifier();
    struct installer_media_source source = media_source_fixture();
    struct installer_media_verification_report report;
    struct verification_policy outsider_policy;
    unsigned char digest[MEDIA_DIGEST_SIZE];
    unsigned char source_digest[MEDIA_DIGEST_SIZE];
    unsigned char signature[MEDIA_SIGNATURE_LENGTH];
    unsigned char envelope[MEDIA_ENVELOPE_CAPACITY];
    uint64_t source_length = 0U;
    size_t envelope_length = 0U;

    media_reset();
    CHECK(sha256_compute(media_bytes, sizeof(media_bytes), digest) ==
              SHA256_OK,
          "media seal fixture digest computed");
    media_signature_build(digest, signature);

    CHECK(installer_media_manifest_seal(
              &media_policy, MEDIA_RELEASE_ID, MEDIA_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
              sizeof(media_bytes), digest, sizeof(media_bytes),
              signature, sizeof(signature),
              envelope, sizeof(envelope), &envelope_length) ==
              INSTALLER_MEDIA_OK &&
              envelope_length != 0U,
          "media manifest seal publishes a signed envelope");
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              envelope, envelope_length, media_bytes,
              sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_OK &&
              report.write_authorized,
          "sealed media manifest verifies end to end");

    CHECK(installer_media_manifest_seal(
              &media_policy, MEDIA_RELEASE_ID, MEDIA_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
              sizeof(media_bytes), digest, sizeof(media_bytes),
              signature, sizeof(signature),
              envelope, 8U, &envelope_length) ==
              INSTALLER_MEDIA_CAPACITY,
          "media manifest seal refuses a short buffer");
    media_policy_build(&outsider_policy, 9U,
                       KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE));
    CHECK(installer_media_manifest_seal(
              &outsider_policy, MEDIA_RELEASE_ID, MEDIA_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
              sizeof(media_bytes), digest, sizeof(media_bytes),
              signature, sizeof(signature),
              envelope, sizeof(envelope), &envelope_length) ==
              INSTALLER_MEDIA_POLICY_MISMATCH,
          "media manifest seal refuses an unknown signer");
    CHECK(installer_media_manifest_seal(
              &media_policy, MEDIA_RELEASE_ID, MEDIA_SIGNER_ID,
              TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
              sizeof(media_bytes), digest, sizeof(media_bytes),
              signature, 0U,
              envelope, sizeof(envelope), &envelope_length) ==
              INSTALLER_MEDIA_MANIFEST_INVALID &&
              installer_media_manifest_seal(
                  &media_policy, 0U, MEDIA_SIGNER_ID,
                  TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
                  sizeof(media_bytes), digest, sizeof(media_bytes),
                  signature, sizeof(signature),
                  envelope, sizeof(envelope), &envelope_length) ==
                  INSTALLER_MEDIA_MANIFEST_INVALID &&
              installer_media_manifest_seal(
                  &media_policy, MEDIA_RELEASE_ID, MEDIA_SIGNER_ID,
                  (enum trust_anchor_algorithm)99, digest,
                  sizeof(media_bytes), digest, sizeof(media_bytes),
                  signature, sizeof(signature),
                  envelope, sizeof(envelope), &envelope_length) ==
                  INSTALLER_MEDIA_MANIFEST_INVALID,
          "media manifest seal rejects invalid producers");
    CHECK(installer_media_source_digest(
              &source, source_digest, &source_length) ==
              INSTALLER_MEDIA_OK &&
              source_length == sizeof(media_bytes),
          "source digest publishes the installed length");

    report = media_report();
    media_reset();
    media_seal_bound(&media_policy, MEDIA_RELEASE_ID, &source);
    source.kernel_size -= 1U;
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH &&
              !report.source_verified && !report.write_authorized,
          "source length mismatch is refused");
    source.kernel_size += 1U;

    media_bytes[MEDIA_LENGTH - 1U] ^= 0x01U;
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_DIGEST_MISMATCH &&
              !report.write_authorized,
          "tampered media is refused before the source check");
    media_bytes[MEDIA_LENGTH - 1U] ^= 0x01U;

    media_seal_bound(&media_policy, MEDIA_RELEASE_ID, &source);
    source.kernel = media_bytes + 29U;
    source.kernel_size = MEDIA_LENGTH - 30U;
    report = media_report();
    CHECK(installer_media_verification_run(
              &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes), &source, &report) ==
              INSTALLER_MEDIA_SOURCE_LENGTH_MISMATCH &&
              !report.write_authorized,
          "source that no longer matches the signed length is refused");

    {
        struct installer_media_source invalid = media_source_fixture();

        invalid.kernel = NULL;
        report = media_report();
        CHECK(installer_media_verification_run(
                  &media_policy, &verifier, MEDIA_RELEASE_ID,
                  media_envelope, media_envelope_length,
                  media_bytes, sizeof(media_bytes), &invalid, &report) ==
                  INSTALLER_MEDIA_SOURCE_INVALID &&
                  !report.write_authorized,
              "incomplete source is refused");
    }
}

/* ------------------------------------------------------------------ */
/* supported hardware fixtures                                        */
/* ------------------------------------------------------------------ */

static void hardware_report(struct installer_discovery_report *report)
{
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 4U;
    report->ram_present = true;
    report->ram_bytes = 4294967296ULL;
    report->disk_count = 1U;
    report->disks[0U].present = true;
    report->disks[0U].id = 1U;
    report->disks[0U].controller = INSTALLER_DISK_CONTROLLER_NVME;
    report->disks[0U].partition_table = INSTALLER_PARTITION_TABLE_GPT;
    report->disks[0U].block_size = 512U;
    report->disks[0U].block_count = 16384U;
    memcpy(report->disks[0U].identity, TEST_IDENTITY,
           sizeof(TEST_IDENTITY));
}

static void test_supported_hardware(void)
{
    struct installer_supported_hardware_policy policy;
    struct installer_supported_hardware_verdict verdict;
    struct installer_discovery_report report;

    CHECK(installer_supported_hardware_policy_default(&policy) ==
              INSTALLER_SUPPORTED_HARDWARE_OK &&
              policy.version == INSTALLER_SUPPORTED_HARDWARE_VERSION &&
              policy.required_firmware == INSTALLER_FIRMWARE_UEFI &&
              policy.minimum_cpu_cores == 1U &&
              policy.minimum_ram_bytes == 2147483648ULL &&
              policy.required_block_size == 512U &&
              policy.allow_bios_legacy &&
              !policy.require_efi_system_partition,
          "default supported-hardware policy is declared");

    hardware_report(&report);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_OK &&
              installer_supported_hardware_verdict_supported(&verdict) &&
              verdict.reason == INSTALLER_HARDWARE_REASON_SUPPORTED &&
              verdict.disk_id == 1U &&
              verdict.version == INSTALLER_SUPPORTED_HARDWARE_VERSION,
          "declared hardware is supported");

    /*
     * ITEM-517: the declared hardware policy admits the legacy-BIOS boot by
     * default, because the appliance installs on machines without UEFI.  A
     * policy that wants the old fail-closed behaviour can say so, and an
     * unknown firmware is never admitted.
     */
    hardware_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_OK &&
              installer_supported_hardware_verdict_supported(&verdict) &&
              verdict.reason == INSTALLER_HARDWARE_REASON_SUPPORTED,
          "legacy firmware is supported when the policy allows it");

    hardware_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    policy.allow_bios_legacy = false;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED,
          "a policy that forbids legacy firmware still fails closed");
    policy.allow_bios_legacy = true;

    hardware_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_UNKNOWN;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED,
          "an unknown firmware still fails closed");

    hardware_report(&report);
    report.scan_complete = false;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_INCOMPLETE_SCAN,
          "incomplete discovery fails closed");

    hardware_report(&report);
    report.read_only = false;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_INVALID_REPORT,
          "writable discovery report fails closed");

    hardware_report(&report);
    report.cpu_present = false;
    report.cpu_cores = 0U;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason == INSTALLER_HARDWARE_REASON_CPU_ABSENT,
          "absent CPU fails closed");

    hardware_report(&report);
    report.ram_bytes = 1073741824ULL;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT &&
              strstr(verdict.detail, "ram=") != NULL,
          "insufficient memory fails closed with a detail");

    hardware_report(&report);
    report.disks[0U].controller = INSTALLER_DISK_CONTROLLER_UNKNOWN;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED,
          "unknown storage controller fails closed");

    hardware_report(&report);
    report.disks[0U].block_size = 4096U;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_BLOCK_SIZE_UNSUPPORTED,
          "unsupported block size fails closed");

    hardware_report(&report);
    report.disks[0U].block_count = 1024U;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_CAPACITY_INSUFFICIENT,
          "undersized target disk fails closed");

    hardware_report(&report);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 9U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_TARGET_DISK_MISSING,
          "missing target disk fails closed");

    hardware_report(&report);
    report.disks[1U] = report.disks[0U];
    report.disk_count = 2U;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_TARGET_DISK_AMBIGUOUS,
          "duplicate target disk identity fails closed");

    hardware_report(&report);
    report.disks[1U] = report.disks[0U];
    report.disks[1U].id = 2U;
    report.disks[2U] = report.disks[0U];
    report.disks[2U].id = 3U;
    report.disks[3U] = report.disks[0U];
    report.disks[3U].id = 4U;
    report.disk_count = 4U;
    policy.maximum_disk_count = 2U;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason == INSTALLER_HARDWARE_REASON_TOO_MANY_DISKS,
          "too many disks fail closed");
    policy.maximum_disk_count = INSTALLER_DISCOVERY_DISK_CAPACITY;

    hardware_report(&report);
    policy.require_efi_system_partition = true;
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED &&
              verdict.reason ==
                  INSTALLER_HARDWARE_REASON_EFI_SYSTEM_PARTITION_MISSING,
          "missing required ESP fails closed");
    policy.require_efi_system_partition = false;

    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              NULL, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_NULL,
          "null discovery report fails closed");
    hardware_report(&report);
    memset(&verdict, 0, sizeof(verdict));
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, NULL, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_NULL,
          "null policy fails closed");
    policy.version = 99U;
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION,
          "unknown policy version fails closed");
    policy.version = INSTALLER_SUPPORTED_HARDWARE_VERSION;
    policy.allowed_controllers = 0U;
    CHECK(installer_supported_hardware_evaluate(
              &report, 1U, &policy, &verdict) ==
              INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY,
          "policy without a supported controller fails closed");
    CHECK(installer_supported_hardware_policy_default(&policy) ==
              INSTALLER_SUPPORTED_HARDWARE_OK,
          "supported-hardware policy fixture restored");
    CHECK(installer_supported_hardware_policy_default(NULL) ==
              INSTALLER_SUPPORTED_HARDWARE_NULL,
          "null policy builder fails closed");
    CHECK(installer_supported_hardware_verdict_supported(NULL) == false,
          "no verdict is not a supported verdict");
    memset(&verdict, 0, sizeof(verdict));
    verdict.version = INSTALLER_SUPPORTED_HARDWARE_VERSION;
    verdict.supported = true;
    CHECK(installer_supported_hardware_verdict_supported(&verdict),
          "explicit supported verdict is supported");

    CHECK(text_equals(installer_supported_hardware_reason_string(
                     INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED), "INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED") &&
              installer_supported_hardware_reason_string(
                  (enum installer_supported_hardware_reason)99) == NULL &&
              text_equals(installer_supported_hardware_status_string(
                         INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED), "INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED"),
          "supported-hardware strings are exact");
}

/* ------------------------------------------------------------------ */
/* uninstall fixtures                                                 */
/* ------------------------------------------------------------------ */

static unsigned char uninstall_storage[TEST_BLOCK_COUNT * TEST_BLOCK_SIZE];
static size_t uninstall_write_calls;

static enum block_device_status uninstall_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(uninstall_storage) ||
        length > sizeof(uninstall_storage) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, uninstall_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status uninstall_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++uninstall_write_calls;
    if (offset > sizeof(uninstall_storage) ||
        length > sizeof(uninstall_storage) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(uninstall_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status uninstall_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device uninstall_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.read_fn = uninstall_read;
    device.write_fn = uninstall_write;
    device.flush_fn = uninstall_flush;
    return device;
}

static struct installer_layout_snapshot uninstall_snapshot(void)
{
    struct installer_layout_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.sector_count = 1U;
    snapshot.sector_lbas[0U] = 0U;
    memset(snapshot.sectors[0U], 0x5AU, TEST_BLOCK_SIZE);
    return snapshot;
}

struct uninstall_boot_fixture {
    bool remove_called;
    uint32_t removed_index;
    bool order_called;
    size_t order_length;
    uint32_t order_entry_count;
    unsigned char order[INSTALLER_UNINSTALL_ORDER_CAPACITY];
    bool fail_remove;
    bool fail_order;
    bool observe_ok;
    struct installer_uninstall_outcome outcome;
};

static enum installer_uninstall_boot_status
uninstall_remove_boot_entry(void *context, uint32_t entry_index)
{
    struct uninstall_boot_fixture *fixture =
        (struct uninstall_boot_fixture *)context;

    fixture->remove_called = true;
    fixture->removed_index = entry_index;
    if (fixture->fail_remove) {
        return INSTALLER_UNINSTALL_BOOT_FAILED;
    }
    return INSTALLER_UNINSTALL_BOOT_OK;
}

static enum installer_uninstall_boot_status
uninstall_restore_boot_order(
    void *context,
    const unsigned char *order,
    size_t order_length,
    uint32_t entry_count)
{
    struct uninstall_boot_fixture *fixture =
        (struct uninstall_boot_fixture *)context;

    fixture->order_called = true;
    fixture->order_length = order_length;
    fixture->order_entry_count = entry_count;
    memcpy(fixture->order, order, order_length);
    if (fixture->fail_order) {
        return INSTALLER_UNINSTALL_BOOT_FAILED;
    }
    return INSTALLER_UNINSTALL_BOOT_OK;
}

static enum installer_uninstall_boot_status
uninstall_observe_outcome(
    void *context,
    struct installer_uninstall_outcome *outcome)
{
    struct uninstall_boot_fixture *fixture =
        (struct uninstall_boot_fixture *)context;

    if (!fixture->observe_ok) {
        return INSTALLER_UNINSTALL_BOOT_VERIFICATION_FAILED;
    }
    *outcome = fixture->outcome;
    return INSTALLER_UNINSTALL_BOOT_OK;
}

static void uninstall_boot_ops(
    struct installer_uninstall_boot_ops *ops)
{
    memset(ops, 0, sizeof(*ops));
    ops->remove_boot_entry = uninstall_remove_boot_entry;
    ops->restore_boot_order = uninstall_restore_boot_order;
    ops->observe_outcome = uninstall_observe_outcome;
}

static struct uninstall_boot_fixture uninstall_boot_fixture_default(void)
{
    struct uninstall_boot_fixture fixture;

    memset(&fixture, 0, sizeof(fixture));
    fixture.observe_ok = true;
    fixture.outcome.version = INSTALLER_UNINSTALL_VERSION;
    fixture.outcome.hfoc_partitions_remaining = false;
    fixture.outcome.hfoc_boot_entry_present = false;
    fixture.outcome.previous_boot_order_restored = true;
    fixture.outcome.previous_os_boot_paths_preserved = true;
    fixture.outcome.partition_table_restored = true;
    return fixture;
}

static void uninstall_evidence(
    struct installer_uninstall_evidence *evidence)
{
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = INSTALLER_UNINSTALL_VERSION;
    evidence->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    evidence->disk_id = 1U;
    memcpy(evidence->identity, TEST_IDENTITY, sizeof(TEST_IDENTITY));
    evidence->hfoc_partition_count = 4U;
    evidence->hfoc_boot_entry_count = 1U;
    evidence->hfoc_boot_entry_indices[0U] = 0U;
    evidence->previous_os_count = 2U;
    evidence->previous_boot_entry_count = 2U;
    evidence->previous_boot_order_available = true;
    evidence->previous_boot_order_length = 4U;
    evidence->previous_boot_order[0U] = 0x01U;
    evidence->previous_boot_order[1U] = 0x00U;
    evidence->previous_boot_order[2U] = 0x02U;
    evidence->previous_boot_order[3U] = 0x00U;
    evidence->partition_table_snapshot_available = true;
    {
        struct installer_layout_snapshot snapshot = uninstall_snapshot();

        (void)installer_uninstall_snapshot_digest(
            &snapshot, evidence->snapshot_digest);
    }
}

static void uninstall_report(struct installer_discovery_report *report)
{
    hardware_report(report);
    report->os_count = 2U;
    report->oss[0U].present = true;
    report->oss[0U].id = 1U;
    report->oss[0U].kind = INSTALLER_OS_LINUX;
    report->oss[0U].has_efi_entry = true;
    report->oss[1U].present = true;
    report->oss[1U].id = 2U;
    report->oss[1U].kind = INSTALLER_OS_WINDOWS;
    report->oss[1U].has_efi_entry = true;
}

static void test_uninstall_plan_and_preview(void)
{
    struct installer_uninstall_evidence evidence;
    struct installer_uninstall_plan plan;
    struct installer_uninstall_preview preview;
    struct installer_discovery_report report;

    uninstall_report(&report);
    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_PLANNED &&
              plan.dry_run && !plan.confirmed &&
              plan.scenario ==
                  INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS &&
              plan.disk_id == 1U &&
              plan.remove_partition_count == 4U &&
              plan.remove_boot_entry_count == 1U &&
              plan.remove_boot_entry_indices[0U] == 0U &&
              plan.preserve_boot_entry_count == 2U &&
              plan.restore_previous_boot_order &&
              plan.previous_os_preserved_required &&
              plan.previous_boot_order_length == 4U &&
              strcmp(plan.identity, TEST_IDENTITY) == 0,
          "uninstall plan captures the HFOC footprint");

    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              plan.stage ==
                  INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED &&
              preview.version == INSTALLER_UNINSTALL_VERSION &&
              preview.destructive && preview.previous_os_preserved &&
              preview.remove_partition_count == 4U &&
              preview.preserve_boot_entry_count == 2U &&
              strstr(preview.text, "dry-run") != NULL &&
              strstr(preview.text, "scenario=restore-previous-os") != NULL &&
              strstr(preview.text, "disk=1") != NULL &&
              strstr(preview.text, "remove-partitions=4") != NULL &&
              strstr(preview.text, "remove-boot-entries=1") != NULL &&
              strstr(preview.text, "preserve-boot-entries=2") != NULL &&
              strstr(preview.text, "previous-os=restored") != NULL,
          "uninstall preview publishes the dry-run plan");

    CHECK(installer_uninstall_confirm(&plan) ==
              INSTALLER_UNINSTALL_OK &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_CONFIRMED &&
              plan.confirmed && !plan.dry_run,
          "uninstall plan confirms after the preview");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_BAD_STATE,
          "uninstall confirm is refused before the preview");
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK &&
              installer_uninstall_preview(&plan, NULL) ==
                  INSTALLER_UNINSTALL_NULL &&
              installer_uninstall_preview(&plan, &preview) ==
                  INSTALLER_UNINSTALL_OK &&
              installer_uninstall_preview(&plan, &preview) ==
                  INSTALLER_UNINSTALL_BAD_STATE,
          "uninstall preview is published exactly once");

    uninstall_evidence(&evidence);
    evidence.hfoc_partition_count = 0U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_NO_HFOC_FOOTPRINT,
          "uninstall refuses without an HFOC footprint");

    uninstall_evidence(&evidence);
    evidence.previous_os_count = 0U;
    evidence.previous_boot_entry_count = 0U;
    report.os_count = 0U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_NO_PREVIOUS_OS,
          "return-to-previous-OS refuses without a previous OS");
    uninstall_report(&report);

    uninstall_evidence(&evidence);
    evidence.previous_os_count = 1U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_INVALID_ARGUMENT,
          "evidence that contradicts discovery is refused");

    uninstall_evidence(&evidence);
    evidence.previous_boot_order_available = false;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING,
          "missing previous boot order is refused");

    uninstall_evidence(&evidence);
    evidence.partition_table_snapshot_available = false;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE,
          "missing partition-table snapshot is refused");

    uninstall_evidence(&evidence);
    memcpy(evidence.identity, "installer-hardening-othe", 24U);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_INVALID_ARGUMENT,
          "uninstall refuses evidence for another disk identity");

    uninstall_evidence(&evidence);
    evidence.snapshot_digest[0U] ^= 0xFFU;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK &&
              memcmp(plan.snapshot_digest, evidence.snapshot_digest,
                     sizeof(plan.snapshot_digest)) == 0,
          "uninstall plan binds the planned snapshot digest");

    uninstall_evidence(&evidence);
    memset(evidence.snapshot_digest, 0, sizeof(evidence.snapshot_digest));
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE,
          "uninstall refuses an unbound snapshot");

    uninstall_evidence(&evidence);
    evidence.hfoc_boot_entry_indices[0U] = 65536U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_INVALID_ARGUMENT,
          "out-of-range boot entry index is refused");

    uninstall_evidence(&evidence);
    evidence.previous_os_count = 0U;
    evidence.previous_boot_entry_count = 0U;
    evidence.previous_boot_order_available = false;
    evidence.previous_boot_order_length = 0U;
    report.os_count = 0U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY,
              &plan) == INSTALLER_UNINSTALL_OK &&
              !plan.previous_os_preserved_required &&
              !plan.restore_previous_boot_order &&
              plan.preserve_boot_entry_count == 0U,
          "erase-only uninstall plans without a previous OS");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              strstr(preview.text, "scenario=erase-hfoc-only") != NULL &&
              !preview.previous_os_preserved,
          "erase-only preview records no previous OS");

    CHECK(installer_uninstall_plan_build(
              NULL, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY,
              &plan) == INSTALLER_UNINSTALL_NULL &&
              installer_uninstall_plan_build(
                  &report, NULL,
                  INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY,
                  &plan) == INSTALLER_UNINSTALL_NULL &&
              installer_uninstall_plan_build(
                  &report, &evidence,
                  INSTALLER_UNINSTALL_SCENARIO_UNKNOWN,
                  &plan) == INSTALLER_UNINSTALL_INVALID_ARGUMENT,
          "uninstall plan rejects invalid arguments");

    CHECK(text_equals(installer_uninstall_scenario_string(
                     INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS), "INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS") &&
              installer_uninstall_scenario_string(
                  INSTALLER_UNINSTALL_SCENARIO_UNKNOWN) == NULL &&
              text_equals(installer_uninstall_stage_string(
                         INSTALLER_UNINSTALL_STAGE_COMPLETE), "INSTALLER_UNINSTALL_STAGE_COMPLETE") &&
              text_equals(installer_uninstall_status_string(
                         INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE), "INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE") &&
              installer_uninstall_status_string(
                  (enum installer_uninstall_status)99) == NULL,
          "uninstall strings are exact");
}

static void test_uninstall_execution(void)
{
    struct installer_uninstall_evidence evidence;
    struct installer_uninstall_plan plan;
    struct installer_uninstall_preview preview;
    struct installer_uninstall_boot_ops ops;
    struct uninstall_boot_fixture fixture;
    struct installer_discovery_report report;
    struct installer_layout_snapshot snapshot = uninstall_snapshot();
    struct block_device device = uninstall_device();

    uninstall_report(&report);
    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall execution fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall execution fixture confirmed");

    fixture = uninstall_boot_fixture_default();
    uninstall_boot_ops(&ops);
    memset(uninstall_storage, 0, sizeof(uninstall_storage));
    uninstall_write_calls = 0U;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, NULL) ==
              INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE,
          "uninstall refuses without boot operations");
    ops.remove_boot_entry = NULL;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE,
          "uninstall refuses an incomplete boot operation boundary");
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, NULL, &ops) ==
              INSTALLER_UNINSTALL_INVALID_ARGUMENT,
          "uninstall refuses without the partition-table snapshot");

    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_OK &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_COMPLETE &&
              fixture.remove_called && fixture.removed_index == 0U &&
              fixture.order_called && fixture.order_length == 4U &&
              fixture.order_entry_count == 2U &&
              fixture.order[0U] == 0x01U &&
              fixture.order[2U] == 0x02U &&
              memcmp(uninstall_storage, snapshot.sectors[0U],
                     TEST_BLOCK_SIZE) == 0 &&
              uninstall_write_calls >= 1U,
          "uninstall restores the partition table and boot state");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall failure fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK,
          "uninstall failure fixture previewed");
    CHECK(installer_uninstall_confirm(&plan) ==
              INSTALLER_UNINSTALL_OK,
          "uninstall failure fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.fail_remove = true;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_BOOT_OPS_FAILED &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "failed boot-entry removal fails the uninstall");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall outcome fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall outcome fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.outcome.hfoc_partitions_remaining = true;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "remaining HFOC partitions fail the uninstall");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall damaged-OS fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall damaged-OS fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.outcome.previous_os_boot_paths_preserved = false;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "lost previous-OS boot path fails the uninstall");

    fixture = uninstall_boot_fixture_default();
    fixture.outcome.version = INSTALLER_UNINSTALL_VERSION;
    fixture.outcome.hfoc_boot_entry_present = true;
    CHECK(installer_uninstall_verify_outcome(&plan, &fixture.outcome) ==
              INSTALLER_UNINSTALL_BAD_STATE,
          "outcome verification is refused after a failed run");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall snapshot-mismatch fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall snapshot-mismatch fixture confirmed");
    {
        struct installer_layout_snapshot other = uninstall_snapshot();

        other.sectors[0U][0U] ^= 0xFFU;
        uninstall_write_calls = 0U;
        fixture = uninstall_boot_fixture_default();
        uninstall_boot_ops(&ops);
        ops.context = &fixture;
        CHECK(installer_uninstall_execute(
                  &plan, &device, &other, &ops) ==
                  INSTALLER_UNINSTALL_SNAPSHOT_MISMATCH &&
                  plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED &&
                  uninstall_write_calls == 0U,
              "a substituted snapshot is refused before any write");
    }

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall restore-order failure fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall restore-order failure fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.fail_order = true;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_BOOT_OPS_FAILED &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "failed boot-order restoration fails the uninstall");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall observe failure fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall observe failure fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.observe_ok = false;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_BOOT_OPS_FAILED &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "unobservable boot state fails the uninstall");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall unrestored-order fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall unrestored-order fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.outcome.previous_boot_order_restored = false;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "unrestored boot order fails the uninstall");

    uninstall_evidence(&evidence);
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_uninstall_plan_build(
              &report, &evidence,
              INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
              &plan) == INSTALLER_UNINSTALL_OK,
          "uninstall unrestored-table fixture planned");
    memset(&preview, 0, sizeof(preview));
    CHECK(installer_uninstall_preview(&plan, &preview) ==
              INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
          "uninstall unrestored-table fixture confirmed");
    fixture = uninstall_boot_fixture_default();
    fixture.outcome.partition_table_restored = false;
    uninstall_boot_ops(&ops);
    ops.context = &fixture;
    CHECK(installer_uninstall_execute(
              &plan, &device, &snapshot, &ops) ==
              INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE &&
              plan.stage == INSTALLER_UNINSTALL_STAGE_FAILED,
          "unverified partition table fails the uninstall");

    {
        struct installer_discovery_report erase_report;
        struct installer_uninstall_evidence erase_evidence;

        uninstall_report(&erase_report);
        erase_report.os_count = 0U;
        memset(erase_report.oss, 0, sizeof(erase_report.oss));
        uninstall_evidence(&erase_evidence);
        erase_evidence.previous_os_count = 0U;
        erase_evidence.previous_boot_entry_count = 0U;
        erase_evidence.previous_boot_order_available = false;
        erase_evidence.previous_boot_order_length = 0U;
        memset(&plan, 0, sizeof(plan));
        CHECK(installer_uninstall_plan_build(
                  &erase_report, &erase_evidence,
                  INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY,
                  &plan) == INSTALLER_UNINSTALL_OK,
              "erase-only uninstall fixture planned");
        memset(&preview, 0, sizeof(preview));
        CHECK(installer_uninstall_preview(&plan, &preview) ==
                  INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&plan) ==
                  INSTALLER_UNINSTALL_OK,
              "erase-only uninstall fixture confirmed");
        fixture = uninstall_boot_fixture_default();
        fixture.outcome.previous_boot_order_restored = false;
        fixture.outcome.previous_os_boot_paths_preserved = false;
        uninstall_boot_ops(&ops);
        ops.context = &fixture;
        CHECK(installer_uninstall_execute(
                  &plan, &device, &snapshot, &ops) ==
                  INSTALLER_UNINSTALL_OK &&
                  plan.stage == INSTALLER_UNINSTALL_STAGE_COMPLETE &&
                  !fixture.order_called,
              "erase-only uninstall skips the previous-OS restore");
    }

    {
        struct installer_uninstall_plan complete;
        struct installer_uninstall_evidence fresh;

        uninstall_evidence(&fresh);
        memset(&complete, 0, sizeof(complete));
        CHECK(installer_uninstall_plan_build(
                  &report, &fresh,
                  INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS,
                  &complete) == INSTALLER_UNINSTALL_OK &&
              installer_uninstall_preview(&complete, &preview) ==
                  INSTALLER_UNINSTALL_OK &&
              installer_uninstall_confirm(&complete) ==
                  INSTALLER_UNINSTALL_OK,
              "outcome verification fixture confirmed");
        fixture = uninstall_boot_fixture_default();
        uninstall_boot_ops(&ops);
        ops.context = &fixture;
        CHECK(installer_uninstall_execute(
                  &complete, &device, &snapshot, &ops) ==
                  INSTALLER_UNINSTALL_OK,
              "outcome verification fixture executed");
        fixture.outcome.hfoc_boot_entry_present = true;
        CHECK(installer_uninstall_verify_outcome(
                  &complete, &fixture.outcome) ==
                  INSTALLER_UNINSTALL_BAD_STATE &&
                  installer_uninstall_verify_outcome(
                      &complete, NULL) == INSTALLER_UNINSTALL_NULL,
              "completed uninstall outcome is verified exactly once");
    }
}

/* ------------------------------------------------------------------ */
/* production installer hardening gate                                */
/* ------------------------------------------------------------------ */

static struct production_installer_source hardening_source(void)
{
    struct production_installer_source source;

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = media_bytes;
    source.boot_payload_size = 16U;
    source.limine_config = media_bytes + 16U;
    source.limine_config_size = 13U;
    source.kernel = media_bytes + 29U;
    source.kernel_size = MEDIA_LENGTH - 29U;
    return source;
}

static void test_production_installer_hardening(void)
{
    struct production_installer installer;
    struct production_installer_source source = hardening_source();
    struct installer_discovery_report report;
    struct installer_supported_hardware_policy policy;
    struct installer_media_verifier verifier = media_verifier();
    unsigned char tampered[MEDIA_LENGTH];

    media_policy_build(&media_policy, MEDIA_SIGNER_ID,
                       KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE));
    media_reset();
    media_seal(&media_policy, KEY_USE_PURPOSE_IMAGE, MEDIA_SIGNER_ID,
               MEDIA_RELEASE_ID, media_bytes, sizeof(media_bytes),
               TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
               MEDIA_SIGNATURE_LENGTH, false, false);
    hardware_report(&report);
    CHECK(installer_supported_hardware_policy_default(&policy) ==
              INSTALLER_SUPPORTED_HARDWARE_OK,
          "installer hardening policy fixture ready");

    memset(&installer, 0, sizeof(installer));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &installer, &report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  TEST_IDENTITY, sizeof(TEST_IDENTITY) - 1U,
                  &source, true) == PRODUCTION_INSTALLER_OK &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_MEDIA_UNVERIFIED,
          "installer plans nothing before media verification");

    CHECK(production_installer_verify_media(
              &installer, &media_policy, &verifier, MEDIA_RELEASE_ID,
              media_envelope, media_envelope_length,
              media_bytes, sizeof(media_bytes)) ==
              PRODUCTION_INSTALLER_OK &&
              installer.media_verified &&
              installer.media_release_id == MEDIA_RELEASE_ID &&
              installer.media_signer_id == MEDIA_SIGNER_ID &&
              installer.media_status == INSTALLER_MEDIA_OK &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE,
          "installer plans nothing before hardware verification");

    CHECK(production_installer_verify_hardware(
              &installer, &report, &policy) ==
              PRODUCTION_INSTALLER_OK &&
              installer.hardware_supported &&
              installer.hardware_reason ==
                  INSTALLER_HARDWARE_REASON_SUPPORTED &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_publish_preview(&installer) ==
                  PRODUCTION_INSTALLER_OK &&
              strstr(installer.preview, "media=verified") != NULL &&
              strstr(installer.preview, "hardware=supported") != NULL,
          "installer plans after both hardening gates");

    memcpy(tampered, media_bytes, sizeof(tampered));
    media_bytes[2U] ^= 0x40U;
    memset(&installer, 0, sizeof(installer));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &installer, &report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  TEST_IDENTITY, sizeof(TEST_IDENTITY) - 1U,
                  &source, true) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_media(
                  &installer, &media_policy, &verifier, MEDIA_RELEASE_ID,
                  media_envelope, media_envelope_length,
                  media_bytes, sizeof(media_bytes)) ==
                  PRODUCTION_INSTALLER_MEDIA_UNVERIFIED &&
              !installer.media_verified &&
              installer.media_status == INSTALLER_MEDIA_DIGEST_MISMATCH,
          "installer records a refused media verification");
    memcpy(media_bytes, tampered, sizeof(media_bytes));

    memset(&installer, 0, sizeof(installer));
    report.ram_bytes = 268435456ULL;
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &installer, &report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  TEST_IDENTITY, sizeof(TEST_IDENTITY) - 1U,
                  &source, true) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_media(
                  &installer, &media_policy, &verifier,
                  MEDIA_RELEASE_ID, media_envelope,
                  media_envelope_length, media_bytes,
                  sizeof(media_bytes)) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_hardware(
                  &installer, &report, &policy) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE &&
              !installer.hardware_supported &&
              installer.hardware_reason ==
                  INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE,
          "installer refuses unsupported hardware");
    hardware_report(&report);

    memset(&installer, 0, sizeof(installer));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              production_installer_verify_media(
                  &installer, &media_policy, &verifier,
                  MEDIA_RELEASE_ID, media_envelope,
                  media_envelope_length, media_bytes,
                  sizeof(media_bytes)) ==
                  PRODUCTION_INSTALLER_BAD_STATE &&
              production_installer_verify_hardware(
                  &installer, &report, &policy) ==
                  PRODUCTION_INSTALLER_BAD_STATE,
          "hardening gates require prepared installer evidence");

    memset(&installer, 0, sizeof(installer));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &installer, &report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  TEST_IDENTITY, sizeof(TEST_IDENTITY) - 1U,
                  &source, true) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_media(
                  &installer, &media_policy, &verifier,
                  MEDIA_RELEASE_ID, media_envelope,
                  media_envelope_length, media_bytes,
                  sizeof(media_bytes)) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_hardware(
                  &installer, NULL, &policy) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE,
          "installer hardening rejects a null discovery report");

    CHECK(text_equals(production_installer_status_string(
                          PRODUCTION_INSTALLER_MEDIA_UNVERIFIED),
                      "PRODUCTION_INSTALLER_MEDIA_UNVERIFIED") &&
              text_equals(production_installer_status_string(
                              PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE),
                          "PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE"),
          "installer hardening status strings are exact");

    {
        struct installer_layout_partition requests[4U];
        struct installer_layout_plan gate_plan;
        struct installer_layout_snapshot gate_snapshot;
        struct block_device gate_device = uninstall_device();

        hardware_report(&report);
        report.disks[0U].block_count = TEST_BLOCK_COUNT;
        policy.minimum_disk_blocks = 1024U;
        memset(requests, 0, sizeof(requests));
        requests[0U].present = true;
        requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
        requests[0U].length_lba = 256U;
        requests[1U].present = true;
        requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
        requests[1U].length_lba = 256U;
        requests[2U].present = true;
        requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
        requests[2U].length_lba = 256U;
        requests[3U].present = true;
        requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
        requests[3U].length_lba = 256U;
        memset(&gate_plan, 0, sizeof(gate_plan));
        memset(&gate_snapshot, 0, sizeof(gate_snapshot));
        CHECK(installer_layout_plan_whole_disk(
                  &report, 1U, requests, 4U, &gate_plan) ==
                  INSTALLER_LAYOUT_OK &&
              installer_layout_confirm(&gate_plan) ==
                  INSTALLER_LAYOUT_OK,
              "write-site gate layout fixture");
        memset(&installer, 0, sizeof(installer));
        CHECK(production_installer_init(&installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &installer, &report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  TEST_IDENTITY, sizeof(TEST_IDENTITY) - 1U,
                  &source, true) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_media(
                  &installer, &media_policy, &verifier,
                  MEDIA_RELEASE_ID, media_envelope,
                  media_envelope_length, media_bytes,
                  sizeof(media_bytes)) == PRODUCTION_INSTALLER_OK &&
              production_installer_verify_hardware(
                  &installer, &report, &policy) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_build_plan(&installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_publish_preview(&installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_confirm(&installer) ==
                  PRODUCTION_INSTALLER_OK,
              "write-site gate fixture confirmed");
        installer.media_verified = false;
        CHECK(production_installer_execute_layout(
                  &installer, &gate_device, &gate_plan,
                  &gate_snapshot) ==
                  PRODUCTION_INSTALLER_MEDIA_UNVERIFIED &&
              !gate_plan.executed &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_CONFIRMED,
              "the write site re-checks the media gate");
        installer.media_verified = true;
        installer.hardware_supported = false;
        CHECK(production_installer_execute_layout(
                  &installer, &gate_device, &gate_plan,
                  &gate_snapshot) ==
                  PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE &&
              !gate_plan.executed,
              "the write site re-checks the hardware gate");
        installer.hardware_supported = true;
        CHECK(production_installer_execute_layout(
                  &installer, &gate_device, &gate_plan,
                  &gate_snapshot) == PRODUCTION_INSTALLER_OK &&
              gate_plan.executed,
              "the verified write site still installs");
    }
}

int main(void)
{
    test_media_verification();
    test_media_manifest_seal();
    test_supported_hardware();
    test_uninstall_plan_and_preview();
    test_uninstall_execution();
    test_production_installer_hardening();

    if (failures != 0) {
        fprintf(stderr, "installer hardening host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer media and hardening host tests: ALL OK\n");
    return 0;
}

#else
int hfoc_installer_hardening_host_translation_unit_marker;
#endif
