#include "appliance_update_integrity.h"

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

#ifdef APPLIANCE_UPDATE_INTEGRITY_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

static const unsigned char metadata_data[] = "update-metadata-v1";
static const unsigned char payload_data[] = "update-payload-v1";

static struct trust_anchor make_anchor(
    uint32_t id,
    enum trust_anchor_algorithm algorithm_id,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = algorithm_id;
    anchor.state = TRUST_ANCHOR_STATE_ACTIVE;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static void init_policy(struct verification_policy *policy)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot snapshot;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ED25519,
        KEY_USE_ACCEPTED_PURPOSES_ALL);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 7U;
    snapshot.anchor_count = 2U;
    memcpy(snapshot.anchors, anchors, sizeof(anchors));
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &snapshot) ==
              VERIFICATION_POLICY_OK,
          "appliance update integrity fixture publishes active policy");
}

static void bytes_to_hex(const unsigned char digest[SHA256_DIGEST_SIZE],
                         char output[2U * SHA256_DIGEST_SIZE + 1U])
{
    static const char digits[] = "0123456789abcdef";
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        output[index * 2U] = digits[digest[index] >> 4U];
        output[index * 2U + 1U] = digits[digest[index] & 0x0FU];
    }
    output[2U * SHA256_DIGEST_SIZE] = '\0';
}

static bool make_manifest(const char *metadata_hex,
                          const char *payload_hex,
                          struct appliance_update_manifest *manifest)
{
    char text[4096];
    int written;

    written = snprintf(
        text, sizeof(text),
        "update_package_version=1\n"
        "release_id=HFOC-APPLIANCE-V1\n"
        "architecture=x86_64\n"
        "source_revision=0123456789abcdef0123456789abcdef01234567\n"
        "metadata_digest=%s\n"
        "payload_digest=%s\n"
        "compatibility_version=1\n"
        "source_image_id=7\n"
        "target_image_id=8\n"
        "source_schema_min=1\n"
        "source_schema_max=1\n"
        "target_schema_min=1\n"
        "target_schema_max=2\n"
        "source_kernel_abi_min=1\n"
        "source_kernel_abi_max=3\n"
        "target_kernel_abi_min=1\n"
        "target_kernel_abi_max=3\n"
        "source_service_abi_min=1\n"
        "source_service_abi_max=1\n"
        "target_service_abi_min=1\n"
        "target_service_abi_max=1\n"
        "source_policy_generation_min=1\n"
        "source_policy_generation_max=1\n"
        "target_policy_generation_min=2\n"
        "target_policy_generation_max=2\n",
        metadata_hex, payload_hex);
    return written > 0 &&
           (size_t)written < sizeof(text) &&
           appliance_update_manifest_parse(
               text, (size_t)written, manifest) ==
               APPLIANCE_UPDATE_MANIFEST_OK;
}

static void make_binding(const unsigned char metadata_digest[32],
                         const unsigned char payload_digest[32],
                         unsigned char binding[
                             APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE])
{
    memcpy(binding, metadata_digest, SHA256_DIGEST_SIZE);
    memcpy(binding + SHA256_DIGEST_SIZE,
           payload_digest, SHA256_DIGEST_SIZE);
}

static bool make_envelope(const struct verification_policy *policy,
                          uint32_t purpose,
                          uint32_t signer_id,
                          const void *binding,
                          size_t binding_length,
                          unsigned char envelope[256],
                          size_t *length_out)
{
    return signed_metadata_seal(
        policy, purpose, signer_id, binding, binding_length,
        envelope, 256U, length_out) == SIGNED_METADATA_OK;
}

static bool make_fixture(struct appliance_update_manifest *manifest,
                         struct verification_policy *policy,
                         unsigned char envelope[256],
                         size_t *envelope_length,
                         unsigned char binding[
                             APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE])
{
    char metadata_hex[2U * SHA256_DIGEST_SIZE + 1U];
    char payload_hex[2U * SHA256_DIGEST_SIZE + 1U];
    unsigned char metadata_digest[SHA256_DIGEST_SIZE];
    unsigned char payload_digest[SHA256_DIGEST_SIZE];

    init_policy(policy);
    CHECK(sha256_compute(
              metadata_data, sizeof(metadata_data) - 1U,
              metadata_digest) == SHA256_OK,
          "appliance update integrity fixture hashes metadata");
    CHECK(sha256_compute(
              payload_data, sizeof(payload_data) - 1U,
              payload_digest) == SHA256_OK,
          "appliance update integrity fixture hashes payload");
    bytes_to_hex(metadata_digest, metadata_hex);
    bytes_to_hex(payload_digest, payload_hex);
    make_binding(metadata_digest, payload_digest, binding);
    CHECK(make_manifest(metadata_hex, payload_hex, manifest),
          "appliance update integrity fixture parses manifest");
    CHECK(make_envelope(
              policy, KEY_USE_PURPOSE_UPDATE, 1U,
              binding, APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE,
              envelope, envelope_length),
          "appliance update integrity fixture seals update envelope");
    return true;
}

static void poison_gate(struct appliance_update_integrity_gate *gate)
{
    memset(gate, 0xA5, sizeof(*gate));
    gate->verified = false;
}

static bool gate_unchanged(
    const struct appliance_update_integrity_gate *gate,
    const struct appliance_update_integrity_gate *original)
{
    return memcmp(gate, original, sizeof(*gate)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum appliance_update_integrity_status status;
        const char *string;
    } statuses[] = {
        {APPLIANCE_UPDATE_INTEGRITY_OK,
         "APPLIANCE_UPDATE_INTEGRITY_OK"},
        {APPLIANCE_UPDATE_INTEGRITY_NULL,
         "APPLIANCE_UPDATE_INTEGRITY_NULL"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_STATE,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_STATE"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST"},
        {APPLIANCE_UPDATE_INTEGRITY_NO_POLICY,
         "APPLIANCE_UPDATE_INTEGRITY_NO_POLICY"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD"},
        {APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH,
         "APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH"},
        {APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH,
         "APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE"},
        {APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER,
         "APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER"},
        {APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE,
         "APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE"},
        {APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED,
         "APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = appliance_update_integrity_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "appliance update integrity status string is exact");
    }
    CHECK(appliance_update_integrity_status_string(
              (enum appliance_update_integrity_status)13U) == NULL,
          "appliance update integrity undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance update integrity status handling performs no allocation");
}

static void test_null_and_bad_state(void)
{
    struct appliance_update_manifest manifest;
    struct verification_policy policy;
    unsigned char envelope[256];
    size_t envelope_length = 0U;
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct appliance_update_integrity_gate gate;
    struct appliance_update_integrity_gate zeroed;

    make_fixture(&manifest, &policy, envelope, &envelope_length, binding);
    memset(&zeroed, 0, sizeof(zeroed));
    poison_gate(&gate);

    CHECK(appliance_update_integrity_init(NULL) ==
              APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity init rejects null gate");
    CHECK(appliance_update_integrity_verify(
              NULL, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null gate");
    CHECK(appliance_update_integrity_verify(
              &gate, NULL, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null manifest");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, NULL, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null policy");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, NULL,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null metadata");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, NULL,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null payload");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, NULL,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_NULL,
          "appliance update integrity verify rejects null envelope");
    CHECK(appliance_update_integrity_verify(
              &zeroed, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_BAD_STATE,
          "appliance update integrity verify rejects uninitialized gate");
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_OK &&
              !gate.verified &&
              gate.version == APPLIANCE_UPDATE_INTEGRITY_VERSION,
          "appliance update integrity init publishes clean one-shot gate");
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_BAD_STATE,
          "appliance update integrity init rejects reinitialization");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_OK &&
              appliance_update_integrity_is_verified(&gate),
          "appliance update integrity fixture verifies valid update");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_BAD_STATE,
          "appliance update integrity verify is one-shot");
    CHECK(!appliance_update_integrity_is_verified(NULL) &&
              !appliance_update_integrity_is_verified(&zeroed),
          "appliance update integrity verified query rejects null and empty gate");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance update integrity null handling performs no allocation");
}

static void test_valid_verification(void)
{
    struct appliance_update_manifest manifest;
    struct verification_policy policy;
    unsigned char envelope[256];
    size_t envelope_length = 0U;
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct appliance_update_integrity_gate gate;
    unsigned char metadata_digest[SHA256_DIGEST_SIZE];
    unsigned char payload_digest[SHA256_DIGEST_SIZE];

    make_fixture(&manifest, &policy, envelope, &envelope_length, binding);
    poison_gate(&gate);
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_OK,
          "valid verification fixture initializes gate");
    CHECK(appliance_update_integrity_verify(
              &gate, &manifest, &policy, metadata_data,
              sizeof(metadata_data) - 1U, payload_data,
              sizeof(payload_data) - 1U, envelope,
              envelope_length) == APPLIANCE_UPDATE_INTEGRITY_OK &&
              gate.verified &&
              gate.policy_generation == 7U &&
              gate.signer_id == 1U &&
              gate.metadata_length == sizeof(metadata_data) - 1U &&
              gate.payload_length == sizeof(payload_data) - 1U,
          "valid update integrity verification publishes exact evidence");
    CHECK(sha256_compute(
              metadata_data, sizeof(metadata_data) - 1U,
              metadata_digest) == SHA256_OK &&
              sha256_compute(
                  payload_data, sizeof(payload_data) - 1U,
                  payload_digest) == SHA256_OK &&
              memcmp(gate.metadata_digest, metadata_digest,
                     SHA256_DIGEST_SIZE) == 0 &&
              memcmp(gate.payload_digest, payload_digest,
                     SHA256_DIGEST_SIZE) == 0,
          "valid update integrity gate stores exact raw digests");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "valid update integrity verification performs no allocation");
}

static void check_failure_leaves_unchanged(
    struct appliance_update_integrity_gate *gate,
    struct appliance_update_integrity_gate *original,
    const struct appliance_update_manifest *manifest,
    const struct verification_policy *policy,
    const void *metadata,
    size_t metadata_length,
    const void *payload,
    size_t payload_length,
    const void *envelope,
    size_t envelope_length,
    enum appliance_update_integrity_status expected,
    const char *message)
{
    CHECK(appliance_update_integrity_verify(
              gate, manifest, policy, metadata, metadata_length,
              payload, payload_length, envelope, envelope_length) ==
              expected &&
              gate_unchanged(gate, original),
          message);
}

static void test_failures_leave_gate_unchanged(void)
{
    struct appliance_update_manifest manifest;
    struct verification_policy policy;
    unsigned char envelope[256];
    size_t envelope_length = 0U;
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct appliance_update_integrity_gate gate;
    struct appliance_update_integrity_gate original;
    struct appliance_update_manifest invalid_manifest;
    struct verification_policy no_policy;
    unsigned char payload[64];
    unsigned char metadata[64];
    unsigned char wrong_envelope[256];
    size_t wrong_length = 0U;

    make_fixture(&manifest, &policy, envelope, &envelope_length, binding);
    invalid_manifest = manifest;
    invalid_manifest.metadata_digest[0] = 'z';
    memset(&no_policy, 0, sizeof(no_policy));
    memcpy(payload, payload_data, sizeof(payload_data));
    memcpy(metadata, metadata_data, sizeof(metadata_data));
    make_envelope(
        &policy, KEY_USE_PURPOSE_IMAGE, 1U,
        binding, APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE,
        wrong_envelope, &wrong_length);
    poison_gate(&gate);
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_OK,
          "failure fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));

    check_failure_leaves_unchanged(
        &gate, &original, &invalid_manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_MANIFEST,
        "invalid manifest cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &no_policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_NO_POLICY,
        "unpublished policy cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata, 0U, payload_data, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA,
        "empty metadata cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload, 0U, envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD,
        "empty payload cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data,
        (size_t)APPLIANCE_UPDATE_INTEGRITY_METADATA_MAX + 1U,
        payload_data, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_METADATA,
        "over-limit metadata cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data,
        (size_t)APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_MAX + 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_PAYLOAD,
        "over-limit payload cannot publish update integrity");

    memcpy(metadata, metadata_data, sizeof(metadata_data));
    metadata[0] ^= 0x01U;
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_METADATA_DIGEST_MISMATCH,
        "tampered metadata digest cannot publish update integrity");
    memcpy(metadata, metadata_data, sizeof(metadata_data));
    memcpy(payload, payload_data, sizeof(payload_data));
    payload[0] ^= 0x01U;
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload, sizeof(payload_data) - 1U,
        envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_PAYLOAD_DIGEST_MISMATCH,
        "tampered payload digest cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        wrong_envelope, wrong_length,
        APPLIANCE_UPDATE_INTEGRITY_PURPOSE_DENIED,
        "wrong-purpose envelope cannot publish update integrity");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update integrity failures perform no allocation");
}

static void test_signature_and_binding_failures(void)
{
    struct appliance_update_manifest manifest;
    struct verification_policy policy;
    unsigned char envelope[256];
    unsigned char tampered_envelope[256];
    unsigned char short_envelope[256];
    unsigned char unknown_envelope[256];
    size_t envelope_length = 0U;
    size_t short_length = 0U;
    size_t unknown_length = 0U;
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    unsigned char wrong_binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct appliance_update_integrity_gate gate;
    struct appliance_update_integrity_gate original;

    make_fixture(&manifest, &policy, envelope, &envelope_length, binding);
    memcpy(tampered_envelope, envelope, envelope_length);
    tampered_envelope[SIGNED_METADATA_HEADER_SIZE + 1U] ^= 0x01U;
    memset(wrong_binding, 0x33, sizeof(wrong_binding));
    make_envelope(
        &policy, KEY_USE_PURPOSE_UPDATE, 1U,
        wrong_binding, sizeof(wrong_binding),
        short_envelope, &short_length);
    make_envelope(
        &policy, KEY_USE_PURPOSE_UPDATE, 1U,
        binding, SHA256_DIGEST_SIZE,
        unknown_envelope, &unknown_length);

    poison_gate(&gate);
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_OK,
          "signature failure fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));

    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        tampered_envelope, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_SIGNATURE,
        "tampered signed envelope cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        short_envelope, short_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE,
        "wrong signed binding cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        unknown_envelope, unknown_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE,
        "short signed binding cannot publish update integrity");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update integrity signature failures perform no allocation");
}

static void test_unknown_signer_and_bad_envelope(void)
{
    struct appliance_update_manifest manifest;
    struct verification_policy policy;
    unsigned char envelope[256];
    unsigned char unknown_signer[256];
    unsigned char corrupt[256];
    size_t envelope_length = 0U;
    size_t unknown_length = 0U;
    unsigned char binding[APPLIANCE_UPDATE_INTEGRITY_BINDING_SIZE];
    struct appliance_update_integrity_gate gate;
    struct appliance_update_integrity_gate original;

    make_fixture(&manifest, &policy, envelope, &envelope_length, binding);
    memcpy(unknown_signer, envelope, envelope_length);
    unknown_signer[SIGNED_METADATA_MAGIC_SIZE + 1U] = 9U;
    unknown_length = envelope_length;
    memcpy(corrupt, envelope, envelope_length);
    corrupt[0] = 'X';

    poison_gate(&gate);
    CHECK(appliance_update_integrity_init(&gate) ==
              APPLIANCE_UPDATE_INTEGRITY_OK,
          "unknown signer fixture initializes gate");
    memcpy(&original, &gate, sizeof(gate));
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        unknown_signer, unknown_length,
        APPLIANCE_UPDATE_INTEGRITY_UNKNOWN_SIGNER,
        "unknown signer cannot publish update integrity");
    check_failure_leaves_unchanged(
        &gate, &original, &manifest, &policy,
        metadata_data, sizeof(metadata_data) - 1U,
        payload_data, sizeof(payload_data) - 1U,
        corrupt, envelope_length,
        APPLIANCE_UPDATE_INTEGRITY_BAD_ENVELOPE,
        "corrupt signed envelope cannot publish update integrity");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "update integrity envelope failures perform no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_bad_state();
    test_valid_verification();
    test_failures_leave_gate_unchanged();
    test_signature_and_binding_failures();
    test_unknown_signer_and_bad_envelope();
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "appliance update integrity host proof performs no allocation");

    if (failures != 0) {
        fprintf(stderr, "appliance update integrity host: %d failures\n",
                failures);
        return 1;
    }
    printf("appliance update integrity host: OK\n");
    return 0;
}

#endif
