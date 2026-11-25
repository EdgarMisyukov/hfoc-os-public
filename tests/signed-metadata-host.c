#include "signed_metadata.h"

#include "key_use.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_SIGNED_METADATA_HOST_TEST

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

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value);
    bytes[1] = (unsigned char)(value >> 8U);
    bytes[2] = (unsigned char)(value >> 16U);
    bytes[3] = (unsigned char)(value >> 24U);
}

static uint32_t load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

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

static struct trust_anchor make_all_anchor(
    uint32_t id,
    enum trust_anchor_algorithm algorithm_id)
{
    return make_anchor(id, algorithm_id, KEY_USE_ACCEPTED_PURPOSES_ALL);
}

static struct verification_policy_snapshot make_snapshot(
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = anchor_count;
    memcpy(snapshot.anchors, anchors,
           anchor_count * sizeof(snapshot.anchors[0]));
    return snapshot;
}

static void init_test_policy(struct verification_policy *policy)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;

    anchors[0] = make_all_anchor(1U, TRUST_ANCHOR_ALGORITHM_ED25519);
    anchors[1] = make_all_anchor(2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256);
    initial = make_snapshot(anchors, 2U);
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &initial) ==
              VERIFICATION_POLICY_OK,
          "signed metadata fixture initializes active policy");
}

static void init_limited_policy(struct verification_policy *policy)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_ALGORITHM_ED25519,
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE));
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_UPDATE));
    initial = make_snapshot(anchors, 2U);
    memset(policy, 0, sizeof(*policy));
    CHECK(verification_policy_init(policy, &initial) ==
              VERIFICATION_POLICY_OK,
          "signed metadata fixture initializes limited-purpose policy");
}

static size_t expected_length(size_t metadata_length)
{
    return SIGNED_METADATA_HEADER_SIZE +
           metadata_length + SIGNED_METADATA_DIGEST_SIZE;
}

static void check_seal_failure(
    const struct verification_policy *policy,
    uint32_t purpose,
    uint32_t signer_id,
    const void *metadata,
    size_t metadata_length,
    size_t capacity,
    enum signed_metadata_status expected,
    const char *message)
{
    unsigned char buffer[2048];
    unsigned char before[sizeof(buffer)];
    size_t length = 0x5A5A5A5A5A5A5A5AULL;

    memset(buffer, 0xA5, sizeof(buffer));
    memcpy(before, buffer, sizeof(before));
    CHECK(signed_metadata_seal(
              policy, purpose, signer_id, metadata, metadata_length,
              buffer, capacity, &length) == expected &&
              length == 0x5A5A5A5A5A5A5A5AULL &&
              memcmp(buffer, before, sizeof(buffer)) == 0,
          message);
}

static void check_verify_failure(
    const struct verification_policy *policy,
    uint32_t purpose,
    const void *envelope,
    size_t envelope_length,
    enum signed_metadata_status expected,
    const char *message)
{
    struct signed_metadata_layout layout;
    unsigned char before[sizeof(layout)];

    memset(&layout, 0x5A, sizeof(layout));
    memcpy(before, &layout, sizeof(layout));
    CHECK(signed_metadata_verify(
              policy, purpose, envelope, envelope_length, &layout) ==
              expected &&
              memcmp(&layout, before, sizeof(layout)) == 0,
          message);
}

static void test_status_strings(void)
{
    static const struct {
        enum signed_metadata_status status;
        const char *string;
    } statuses[] = {
        {SIGNED_METADATA_OK, "SIGNED_METADATA_OK"},
        {SIGNED_METADATA_NULL, "SIGNED_METADATA_NULL"},
        {SIGNED_METADATA_NO_POLICY, "SIGNED_METADATA_NO_POLICY"},
        {SIGNED_METADATA_UNSUPPORTED, "SIGNED_METADATA_UNSUPPORTED"},
        {SIGNED_METADATA_TOO_SHORT, "SIGNED_METADATA_TOO_SHORT"},
        {SIGNED_METADATA_BUFFER_TOO_SMALL,
         "SIGNED_METADATA_BUFFER_TOO_SMALL"},
        {SIGNED_METADATA_TOO_LONG, "SIGNED_METADATA_TOO_LONG"},
        {SIGNED_METADATA_CORRUPT, "SIGNED_METADATA_CORRUPT"},
        {SIGNED_METADATA_INVALID_METADATA,
         "SIGNED_METADATA_INVALID_METADATA"},
        {SIGNED_METADATA_UNKNOWN_SIGNER,
         "SIGNED_METADATA_UNKNOWN_SIGNER"},
        {SIGNED_METADATA_BAD_SIGNATURE, "SIGNED_METADATA_BAD_SIGNATURE"},
        {SIGNED_METADATA_INVALID_PURPOSE,
         "SIGNED_METADATA_INVALID_PURPOSE"},
        {SIGNED_METADATA_PURPOSE_DENIED,
         "SIGNED_METADATA_PURPOSE_DENIED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = signed_metadata_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "signed metadata status string is exact");
    }
    CHECK(signed_metadata_status_string(
              (enum signed_metadata_status)13U) == NULL,
          "signed metadata undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata status handling performs no allocation");
}

static void test_null_and_no_policy(void)
{
    struct verification_policy policy;
    unsigned char buffer[256];
    unsigned char envelope[256];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "metadata";
    size_t length = 0U;

    memset(&policy, 0, sizeof(policy));
    memset(buffer, 0, sizeof(buffer));
    memset(envelope, 0, sizeof(envelope));
    memset(&layout, 0, sizeof(layout));

    CHECK(signed_metadata_seal(
              NULL, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_NULL,
          "signed metadata seal rejects null policy");
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, NULL,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_NULL,
          "signed metadata seal rejects null metadata");
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), NULL, 0U, &length) ==
              SIGNED_METADATA_NULL,
          "signed metadata seal rejects null buffer");
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), NULL) ==
              SIGNED_METADATA_NULL,
          "signed metadata seal rejects null length output");
    CHECK(signed_metadata_verify(
              NULL, KEY_USE_PURPOSE_IMAGE, envelope,
              sizeof(envelope), &layout) == SIGNED_METADATA_NULL,
          "signed metadata verify rejects null policy");
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE, NULL,
              sizeof(envelope), &layout) == SIGNED_METADATA_NULL,
          "signed metadata verify rejects null envelope");
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE, envelope,
              sizeof(envelope), NULL) == SIGNED_METADATA_NULL,
          "signed metadata verify rejects null layout");
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_NO_POLICY,
          "signed metadata seal rejects unpublished policy");
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE, envelope,
              sizeof(envelope), &layout) == SIGNED_METADATA_NO_POLICY,
          "signed metadata verify rejects unpublished policy");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata null handling performs no allocation");
}

static void test_seal_failures(void)
{
    struct verification_policy policy;
    unsigned char buffer[2048];
    const unsigned char metadata[] = "image metadata";

    init_test_policy(&policy);
    memset(buffer, 0xA5, sizeof(buffer));
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, 200U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_UNKNOWN_SIGNER,
        "signed metadata seal rejects unknown signer without writing");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, 0U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_UNKNOWN_SIGNER,
        "signed metadata seal rejects zero signer id");
    check_seal_failure(
        &policy, 0U, 1U, metadata, sizeof(metadata),
        sizeof(buffer), SIGNED_METADATA_INVALID_PURPOSE,
        "signed metadata seal rejects zero purpose");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_MAX + 1U, 1U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_INVALID_PURPOSE,
        "signed metadata seal rejects unknown purpose");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata, 0U,
        sizeof(buffer), SIGNED_METADATA_INVALID_METADATA,
        "signed metadata seal rejects empty metadata");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
        SIGNED_METADATA_MAX_METADATA_LENGTH + 1U,
        sizeof(buffer), SIGNED_METADATA_INVALID_METADATA,
        "signed metadata seal rejects over-limit metadata");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata, sizeof(metadata),
        expected_length(sizeof(metadata)) - 1U,
        SIGNED_METADATA_BUFFER_TOO_SMALL,
        "signed metadata seal rejects small output buffer");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata seal failure performs no allocation");
}

static void test_seal_denied_purpose(void)
{
    struct verification_policy policy;
    unsigned char buffer[2048];
    const unsigned char metadata[] = "image manifest v1";

    init_limited_policy(&policy);
    memset(buffer, 0xA5, sizeof(buffer));
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, 1U, metadata, sizeof(metadata),
        sizeof(buffer), SIGNED_METADATA_PURPOSE_DENIED,
        "signed metadata seal denies image-only signer for update");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_BACKUP, 1U, metadata, sizeof(metadata),
        sizeof(buffer), SIGNED_METADATA_PURPOSE_DENIED,
        "signed metadata seal denies image-only signer for backup");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_INTEGRATION, 1U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_PURPOSE_DENIED,
        "signed metadata seal denies image-only signer for integration");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_RECOVERY, 1U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_PURPOSE_DENIED,
        "signed metadata seal denies image-only signer for recovery");
    check_seal_failure(
        &policy, KEY_USE_PURPOSE_RECOVERY, 2U, metadata,
        sizeof(metadata), sizeof(buffer), SIGNED_METADATA_PURPOSE_DENIED,
        "signed metadata seal denies update-only signer for recovery");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata denied-purpose sealing performs no allocation");
}

static void test_seal_and_verify_success(void)
{
    static const uint32_t purposes[] = {
        KEY_USE_PURPOSE_IMAGE,
        KEY_USE_PURPOSE_UPDATE,
        KEY_USE_PURPOSE_BACKUP,
        KEY_USE_PURPOSE_INTEGRATION,
        KEY_USE_PURPOSE_RECOVERY,
    };
    struct verification_policy policy;
    unsigned char buffer[2048];
    unsigned char digest[SIGNED_METADATA_DIGEST_SIZE];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "image manifest v1";
    const unsigned char magic[SIGNED_METADATA_MAGIC_SIZE] =
        SIGNED_METADATA_MAGIC_BYTES;
    size_t length = 0U;
    size_t metadata_offset = SIGNED_METADATA_HEADER_SIZE;
    size_t signature_offset;
    uint32_t index;

    init_test_policy(&policy);
    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_OK &&
              length == expected_length(sizeof(metadata)),
          "signed metadata seal produces exact valid envelope");
    CHECK(memcmp(buffer, magic, SIGNED_METADATA_MAGIC_SIZE) == 0 &&
              buffer[SIGNED_METADATA_MAGIC_SIZE] ==
                  SIGNED_METADATA_FORMAT_VERSION &&
              buffer[SIGNED_METADATA_MAGIC_SIZE + 1U] == 1U,
          "signed metadata seal writes exact magic, version, and signer");
    CHECK(buffer[SIGNED_METADATA_PURPOSE_OFFSET] ==
              KEY_USE_PURPOSE_IMAGE,
          "signed metadata seal writes exact purpose byte");
    for (index = SIGNED_METADATA_PURPOSE_OFFSET + 1U;
         index < 16U; ++index) {
        CHECK(buffer[index] == 0U,
              "signed metadata seal zeroes first reserved range");
    }
    CHECK(load_le32(buffer + 16U) == sizeof(metadata),
          "signed metadata seal writes exact metadata length");
    for (index = 20U; index < 32U; ++index) {
        CHECK(buffer[index] == 0U,
              "signed metadata seal zeroes second reserved range");
    }
    CHECK(memcmp(buffer + metadata_offset, metadata,
                 sizeof(metadata)) == 0,
          "signed metadata seal copies exact metadata payload");
    signature_offset = metadata_offset + sizeof(metadata);
    CHECK(signature_offset == length - SIGNED_METADATA_DIGEST_SIZE,
          "signed metadata seal places digest after payload");
    CHECK(sha256_compute(buffer, signature_offset, digest) == SHA256_OK &&
              memcmp(buffer + signature_offset, digest,
                     SIGNED_METADATA_DIGEST_SIZE) == 0,
          "signed metadata seal writes matching SHA-256 digest");

    memset(&layout, 0x5A, sizeof(layout));
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE, buffer, length,
              &layout) == SIGNED_METADATA_OK &&
              layout.valid &&
              layout.purpose == KEY_USE_PURPOSE_IMAGE &&
              layout.signer_id == 1U &&
              layout.metadata_length == sizeof(metadata) &&
              layout.metadata_offset == metadata_offset &&
              layout.signature_offset == signature_offset &&
              layout.envelope_length == length,
          "signed metadata verify accepts exact envelope and layout");

    for (index = 0U; index < sizeof(purposes) / sizeof(purposes[0]);
         ++index) {
        const uint32_t purpose = purposes[index];
        const uint32_t signer_id = (index == 0U) ? 1U : 2U;

        memset(buffer, 0, sizeof(buffer));
        CHECK(signed_metadata_seal(
                  &policy, purpose, signer_id, metadata,
                  sizeof(metadata), buffer, sizeof(buffer), &length) ==
                  SIGNED_METADATA_OK,
              "signed metadata seal accepts every purpose for active signer");
        memset(&layout, 0x5A, sizeof(layout));
        CHECK(signed_metadata_verify(
                  &policy, purpose, buffer, length, &layout) ==
                  SIGNED_METADATA_OK &&
                  layout.purpose == purpose &&
                  layout.signer_id == signer_id,
              "signed metadata verify accepts every matching purpose");
    }
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata seal success performs no allocation");
}

static void test_cross_purpose_replay(void)
{
    struct verification_policy policy;
    unsigned char buffer[2048];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "update metadata";
    size_t length = 0U;

    init_test_policy(&policy);
    memset(buffer, 0, sizeof(buffer));
    memset(&layout, 0, sizeof(layout));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_OK,
          "cross-purpose fixture seals image envelope");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "image envelope is denied as update metadata");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_BACKUP, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "image envelope is denied as backup metadata");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_INTEGRATION, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "image envelope is denied as integration metadata");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_RECOVERY, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "image envelope is denied as recovery metadata");

    memset(buffer, 0, sizeof(buffer));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_UPDATE, 2U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_OK,
          "cross-purpose fixture seals update envelope");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_BACKUP, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "update envelope is denied as backup metadata");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "cross-purpose replay rejection performs no allocation");
}

static void test_limited_policy_verify(void)
{
    struct verification_policy policy;
    unsigned char buffer[2048];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "image manifest";
    size_t length = 0U;

    init_limited_policy(&policy);
    memset(buffer, 0, sizeof(buffer));
    memset(&layout, 0, sizeof(layout));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_OK,
          "limited image-only signer seals image metadata");
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE, buffer, length,
              &layout) == SIGNED_METADATA_OK &&
              layout.purpose == KEY_USE_PURPOSE_IMAGE,
          "limited image-only signer verifies image metadata");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, buffer, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "image-only signer denies image envelope as update");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "limited-policy verification performs no allocation");
}

static void test_verify_failures(void)
{
    struct verification_policy policy;
    unsigned char buffer[2048];
    unsigned char mutated[2048];
    struct signed_metadata_layout layout;
    const unsigned char metadata[] = "update metadata";
    size_t length = 0U;
    size_t metadata_offset = SIGNED_METADATA_HEADER_SIZE;
    size_t signature_offset;

    init_test_policy(&policy);
    memset(buffer, 0, sizeof(buffer));
    memset(mutated, 0, sizeof(mutated));
    CHECK(signed_metadata_seal(
              &policy, KEY_USE_PURPOSE_UPDATE, 1U, metadata,
              sizeof(metadata), buffer, sizeof(buffer), &length) ==
              SIGNED_METADATA_OK,
          "signed metadata failure fixture seals valid envelope");
    signature_offset = metadata_offset + sizeof(metadata);
    memset(&layout, 0x5A, sizeof(layout));

    check_verify_failure(
        &policy, 0U, buffer, length, SIGNED_METADATA_INVALID_PURPOSE,
        "signed metadata verify rejects zero requested purpose");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_MAX + 1U, buffer, length,
        SIGNED_METADATA_INVALID_PURPOSE,
        "signed metadata verify rejects unknown requested purpose");

    memcpy(mutated, buffer, length);
    mutated[metadata_offset] ^= 0x01U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_BAD_SIGNATURE,
        "altered signed metadata payload fails verification");

    memcpy(mutated, buffer, length);
    store_le32(mutated + 16U, (uint32_t)sizeof(metadata) + 1U);
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_CORRUPT,
        "altered signed metadata length field fails verification");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_MAGIC_SIZE] =
        SIGNED_METADATA_FORMAT_VERSION + 1U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_UNSUPPORTED,
        "altered signed metadata version fails verification");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_MAGIC_SIZE + 1U] = 200U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_UNKNOWN_SIGNER,
        "unknown signed metadata signer fails verification");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_MAGIC_SIZE + 1U] = 2U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_BAD_SIGNATURE,
        "altered active signed metadata signer fails stale digest");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_PURPOSE_OFFSET] = 0U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_INVALID_PURPOSE,
        "invalid signed metadata purpose byte fails verification");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_PURPOSE_OFFSET] = 99U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_INVALID_PURPOSE,
        "unknown signed metadata purpose byte fails verification");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_PURPOSE_OFFSET] = KEY_USE_PURPOSE_IMAGE;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_PURPOSE_DENIED,
        "cross-purpose signed metadata byte fails requested-purpose check");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_PURPOSE_OFFSET] = KEY_USE_PURPOSE_IMAGE;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_IMAGE, mutated, length,
        SIGNED_METADATA_BAD_SIGNATURE,
        "tampered signed metadata purpose fails stale digest");

    memcpy(mutated, buffer, length);
    mutated[SIGNED_METADATA_PURPOSE_OFFSET + 1U] = 1U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_CORRUPT,
        "nonzero signed metadata reserved byte fails verification");

    memcpy(mutated, buffer, length);
    mutated[signature_offset] ^= 0x01U;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_BAD_SIGNATURE,
        "altered signed metadata digest fails verification");

    memcpy(mutated, buffer, length);
    mutated[0U] = 'X';
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length,
        SIGNED_METADATA_CORRUPT,
        "bad signed metadata magic fails verification");

    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, buffer,
        SIGNED_METADATA_MIN_ENVELOPE_LENGTH - 1U,
        SIGNED_METADATA_TOO_SHORT,
        "truncated below minimum signed metadata envelope fails verification");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, buffer, length - 1U,
        SIGNED_METADATA_CORRUPT,
        "truncated signed metadata envelope fails exact length");
    memcpy(mutated, buffer, length);
    mutated[length] = 0x5AU;
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, mutated, length + 1U,
        SIGNED_METADATA_CORRUPT,
        "trailing byte in signed metadata envelope fails verification");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, (const void *)1U,
        SIGNED_METADATA_MAX_ENVELOPE_LENGTH + 1U,
        SIGNED_METADATA_TOO_LONG,
        "over-limit signed metadata envelope fails verification");
    check_verify_failure(
        &policy, KEY_USE_PURPOSE_UPDATE, buffer, 0U,
        SIGNED_METADATA_TOO_SHORT,
        "empty signed metadata envelope fails verification");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "signed metadata verify failure performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_no_policy();
    test_seal_failures();
    test_seal_denied_purpose();
    test_seal_and_verify_success();
    test_cross_purpose_replay();
    test_limited_policy_verify();
    test_verify_failures();

    if (failures != 0) {
        fprintf(stderr, "Signed metadata host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Signed metadata host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_SIGNED_METADATA_HOST_TEST"
#endif
