#include "recovery_trust.h"

#include "image_policy.h"
#include "key_use.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_RECOVERY_TRUST_HOST_TEST

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

#define KERNEL_DATA "kernel-image"
#define INITRAMFS_DATA "initramfs-image"

#define KERNEL_HASH \
    "7d85a326c4eaf5c09d11bed101a08ea50915897a31b627285b5b3446bd37262f"
#define INITRAMFS_HASH \
    "c94f8be14b260af440b8e93fc1b208e55f8eb554439408552ea5aa078968158b"

static struct trust_anchor make_anchor(uint32_t id)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.state = TRUST_ANCHOR_STATE_ACTIVE;
    anchor.accepted_purposes = KEY_USE_ACCEPTED_PURPOSES_ALL;
    return anchor;
}

static struct verification_policy_snapshot make_policy_snapshot(
    uint32_t generation)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot snapshot;

    anchors[0] = make_anchor(1U);
    anchors[1] = make_anchor(2U);
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = 2U;
    memcpy(snapshot.anchors, anchors, sizeof(anchors));
    return snapshot;
}

static bool init_policy(struct verification_policy *policy,
                        uint32_t generation)
{
    struct verification_policy_snapshot snapshot =
        make_policy_snapshot(generation);

    memset(policy, 0, sizeof(*policy));
    return verification_policy_init(policy, &snapshot) ==
           VERIFICATION_POLICY_OK;
}

static struct system_image_manifest make_manifest(uint32_t image_id)
{
    struct system_image_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION;
    manifest.image_id = image_id;
    manifest.min_kernel_abi = 1U;
    manifest.max_kernel_abi = 3U;
    strcpy(manifest.arch, SYSTEM_IMAGE_ARCH_VALUE);
    manifest.component_count = 2U;
    strcpy(manifest.components[0].name, "kernel");
    strcpy(manifest.components[0].version, "0.2.6");
    strcpy(manifest.components[0].hash, KERNEL_HASH);
    strcpy(manifest.components[1].name, "initramfs");
    strcpy(manifest.components[1].version, "1.0");
    strcpy(manifest.components[1].hash, INITRAMFS_HASH);
    return manifest;
}

static void make_payloads(struct image_integrity_payload payloads[2])
{
    static const char kernel[] = KERNEL_DATA;
    static const char initramfs[] = INITRAMFS_DATA;

    payloads[0].name = "kernel";
    payloads[0].data = kernel;
    payloads[0].data_length = sizeof(kernel) - 1U;
    payloads[1].name = "initramfs";
    payloads[1].data = initramfs;
    payloads[1].data_length = sizeof(initramfs) - 1U;
}

static bool make_gate(struct image_integrity_gate *gate,
                      uint32_t image_id)
{
    struct image_integrity_payload payloads[2];
    struct system_image_manifest manifest = make_manifest(image_id);

    make_payloads(payloads);
    memset(gate, 0, sizeof(*gate));
    return image_integrity_verify(gate, &manifest, payloads, 2U) ==
           IMAGE_INTEGRITY_OK;
}

static void make_measurement(uint8_t measurement[SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        measurement[index] = (uint8_t)(0xA0U + index);
    }
}

static bool make_evidence(struct boot_evidence *evidence,
                          const struct verification_policy *policy)
{
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    uint8_t measurement[SHA256_DIGEST_SIZE];

    make_measurement(measurement);
    if (!make_gate(&gate, 7U)) {
        return false;
    }
    memset(&binding, 0, sizeof(binding));
    if (image_policy_bind(&binding, policy, &gate) != IMAGE_POLICY_OK) {
        return false;
    }
    memset(evidence, 0, sizeof(*evidence));
    return boot_evidence_init(
        evidence, policy, &binding, measurement) == BOOT_EVIDENCE_OK;
}

static bool seal_envelope(
    const struct verification_policy *policy,
    uint32_t purpose,
    uint32_t signer_id,
    const void *metadata,
    size_t metadata_length,
    unsigned char *buffer,
    size_t capacity,
    size_t *length_out)
{
    return signed_metadata_seal(
        policy, purpose, signer_id, metadata, metadata_length,
        buffer, capacity, length_out) == SIGNED_METADATA_OK;
}

static bool seal_recovery(
    const struct verification_policy *policy,
    const char *metadata,
    size_t metadata_length,
    unsigned char *buffer,
    size_t capacity,
    size_t *length_out)
{
    return seal_envelope(
        policy, KEY_USE_PURPOSE_RECOVERY, 1U, metadata,
        metadata_length, buffer, capacity, length_out);
}

static bool state_unchanged(
    const struct recovery_trust_state *state,
    const struct recovery_trust_state *original)
{
    return memcmp(state, original, sizeof(*state)) == 0;
}

static void test_status_strings(void)
{
    static const struct {
        enum recovery_trust_status status;
        const char *string;
    } statuses[] = {
        {RECOVERY_TRUST_OK, "RECOVERY_TRUST_OK"},
        {RECOVERY_TRUST_NULL, "RECOVERY_TRUST_NULL"},
        {RECOVERY_TRUST_BAD_STATE, "RECOVERY_TRUST_BAD_STATE"},
        {RECOVERY_TRUST_NO_POLICY, "RECOVERY_TRUST_NO_POLICY"},
        {RECOVERY_TRUST_NO_EVIDENCE, "RECOVERY_TRUST_NO_EVIDENCE"},
        {RECOVERY_TRUST_EVIDENCE_MISMATCH,
         "RECOVERY_TRUST_EVIDENCE_MISMATCH"},
        {RECOVERY_TRUST_INVALID_METADATA,
         "RECOVERY_TRUST_INVALID_METADATA"},
        {RECOVERY_TRUST_METADATA_DENIED,
         "RECOVERY_TRUST_METADATA_DENIED"},
        {RECOVERY_TRUST_POLICY_MISMATCH,
         "RECOVERY_TRUST_POLICY_MISMATCH"},
        {RECOVERY_TRUST_UNSUPPORTED_ACTION,
         "RECOVERY_TRUST_UNSUPPORTED_ACTION"},
        {RECOVERY_TRUST_MODE_DENIED, "RECOVERY_TRUST_MODE_DENIED"},
    };
    size_t index;

    for (index = 0U;
         index < sizeof(statuses) / sizeof(statuses[0]);
         ++index) {
        const char *string = recovery_trust_status_string(
            statuses[index].status);

        CHECK(string != NULL &&
                  strcmp(string, statuses[index].string) == 0,
              "recovery trust status string is exact");
    }
    CHECK(recovery_trust_status_string(
              (enum recovery_trust_status)11U) == NULL,
          "recovery trust undefined status is rejected");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery trust status handling performs no allocation");
}

static void test_null_and_bad_state(void)
{
    struct verification_policy policy;
    struct boot_evidence evidence;
    struct recovery_trust_state state;
    struct recovery_trust_state zeroed;
    struct recovery_trust_state original;
    struct signed_metadata_layout layout;
    unsigned char envelope[256];

    memset(&policy, 0, sizeof(policy));
    memset(&evidence, 0, sizeof(evidence));
    memset(&state, 0, sizeof(state));
    memset(&zeroed, 0, sizeof(zeroed));
    memset(&layout, 0, sizeof(layout));
    memset(envelope, 0, sizeof(envelope));

    CHECK(recovery_trust_init(NULL) == RECOVERY_TRUST_NULL,
          "recovery trust init rejects null state");
    CHECK(recovery_trust_enter(
              NULL, &policy, &evidence, envelope, sizeof(envelope),
              &layout) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null state");
    CHECK(recovery_trust_enter(
              &state, NULL, &evidence, envelope, sizeof(envelope),
              &layout) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null policy");
    CHECK(recovery_trust_enter(
              &state, &policy, NULL, envelope, sizeof(envelope),
              &layout) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null evidence");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, NULL, sizeof(envelope),
              &layout) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null envelope");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, envelope, sizeof(envelope),
              NULL) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null layout");
    CHECK(recovery_trust_authorize(NULL, RECOVERY_TRUST_ACTION_CONTROL) ==
              RECOVERY_TRUST_NULL,
          "recovery trust authorize rejects null state");
    CHECK(recovery_trust_exit(
              NULL, &policy, envelope, sizeof(envelope), &layout) ==
              RECOVERY_TRUST_NULL,
          "recovery trust exit rejects null state");
    CHECK(recovery_trust_exit(
              &state, NULL, envelope, sizeof(envelope), &layout) ==
              RECOVERY_TRUST_NULL,
          "recovery trust exit rejects null policy");
    CHECK(recovery_trust_exit(
              &state, &policy, NULL, sizeof(envelope), &layout) ==
              RECOVERY_TRUST_NULL,
          "recovery trust exit rejects null envelope");
    CHECK(recovery_trust_exit(
              &state, &policy, envelope, sizeof(envelope), NULL) ==
              RECOVERY_TRUST_NULL,
          "recovery trust exit rejects null layout");
    CHECK(!recovery_trust_is_recovery(NULL),
          "recovery trust recovery query rejects null state");

    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_OK &&
              state.version == RECOVERY_TRUST_VERSION &&
              state.mode == RECOVERY_TRUST_MODE_NORMAL &&
              state.policy_generation == 0U &&
              state.signer_id == 0U &&
              !state.active,
          "recovery trust init publishes clean normal state");
    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_BAD_STATE,
          "recovery trust init rejects reinitialization");
    CHECK(recovery_trust_authorize(&state, 0U) ==
              RECOVERY_TRUST_UNSUPPORTED_ACTION,
          "recovery trust authorize rejects zero action");
    CHECK(recovery_trust_authorize(&state, 99U) ==
              RECOVERY_TRUST_UNSUPPORTED_ACTION,
          "recovery trust authorize rejects unknown action");
    CHECK(recovery_trust_authorize(&zeroed, RECOVERY_TRUST_ACTION_CONTROL) ==
              RECOVERY_TRUST_BAD_STATE,
          "recovery trust authorize rejects invalid state version");
    memcpy(&original, &state, sizeof(state));
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, envelope, sizeof(envelope),
              &layout) == RECOVERY_TRUST_NO_POLICY &&
              state_unchanged(&state, &original),
          "recovery trust enter without active policy fails closed");
    CHECK(recovery_trust_exit(
              &state, &policy, envelope, sizeof(envelope), &layout) ==
              RECOVERY_TRUST_BAD_STATE &&
              state_unchanged(&state, &original),
          "recovery trust exit from normal state fails closed");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery trust null handling performs no allocation");
}

static void test_enter_requires_evidence_and_purpose(void)
{
    struct verification_policy policy;
    struct boot_evidence evidence;
    struct boot_evidence bad_evidence;
    struct recovery_trust_state state;
    struct recovery_trust_state original;
    struct signed_metadata_layout layout;
    unsigned char recovery[256];
    unsigned char image[256];
    unsigned char tampered[256];
    unsigned char wrong_payload[256];
    size_t recovery_length = 0U;
    size_t image_length = 0U;
    size_t tampered_length = 0U;
    size_t wrong_payload_length = 0U;

    CHECK(init_policy(&policy, 1U),
          "enter-requirement fixture initializes active policy");
    CHECK(make_evidence(&evidence, &policy),
          "enter-requirement fixture publishes boot evidence");
    CHECK(seal_recovery(
              &policy, RECOVERY_TRUST_ENTER_METADATA,
              RECOVERY_TRUST_ENTER_METADATA_LEN,
              recovery, sizeof(recovery), &recovery_length),
          "enter-requirement fixture seals recovery entry");
    CHECK(seal_envelope(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U, "image",
              sizeof("image") - 1U, image, sizeof(image),
              &image_length),
          "enter-requirement fixture seals image envelope");
    CHECK(seal_recovery(
              &policy, "wrong-recovery", sizeof("wrong-recovery") - 1U,
              wrong_payload, sizeof(wrong_payload),
              &wrong_payload_length),
          "enter-requirement fixture seals wrong recovery payload");
    memcpy(tampered, recovery, sizeof(tampered));
    tampered_length = recovery_length;
    if (tampered_length > 0U) {
        tampered[0] ^= 0x01U;
    }

    memset(&state, 0, sizeof(state));
    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_OK,
          "enter-requirement fixture initializes recovery state");
    memcpy(&original, &state, sizeof(state));
    CHECK(recovery_trust_enter(
              &state, &policy, NULL, recovery, recovery_length,
              &layout) == RECOVERY_TRUST_NULL,
          "recovery trust enter rejects null evidence");

    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, image, image_length,
              &layout) == RECOVERY_TRUST_METADATA_DENIED &&
              state_unchanged(&state, &original),
          "recovery trust enter denies image-purpose envelope");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, wrong_payload,
              wrong_payload_length, &layout) ==
              RECOVERY_TRUST_INVALID_METADATA &&
              state_unchanged(&state, &original),
          "recovery trust enter rejects wrong recovery payload");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, tampered, tampered_length,
              &layout) == RECOVERY_TRUST_INVALID_METADATA &&
              state_unchanged(&state, &original),
          "recovery trust enter rejects tampered envelope");

    bad_evidence = evidence;
    bad_evidence.digest[0] ^= 0x01U;
    CHECK(recovery_trust_enter(
              &state, &policy, &bad_evidence, recovery, recovery_length,
              &layout) == RECOVERY_TRUST_EVIDENCE_MISMATCH &&
              state_unchanged(&state, &original),
          "recovery trust enter rejects digest-mismatch evidence");

    bad_evidence = evidence;
    bad_evidence.policy_generation = 99U;
    bad_evidence.digest[0] ^= 0x01U;
    CHECK(recovery_trust_enter(
              &state, &policy, &bad_evidence, recovery, recovery_length,
              &layout) == RECOVERY_TRUST_EVIDENCE_MISMATCH &&
              state_unchanged(&state, &original),
          "recovery trust enter rejects superseded evidence");

    memset(&bad_evidence, 0, sizeof(bad_evidence));
    CHECK(recovery_trust_enter(
              &state, &policy, &bad_evidence, recovery, recovery_length,
              &layout) == RECOVERY_TRUST_NO_EVIDENCE &&
              state_unchanged(&state, &original),
          "recovery trust enter rejects unpublished evidence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery trust enter failures perform no allocation");
}

static void test_recovery_entry_gating_and_exit(void)
{
    struct verification_policy policy;
    struct verification_policy_snapshot next;
    struct boot_evidence evidence;
    struct recovery_trust_state state;
    struct recovery_trust_state original;
    struct signed_metadata_layout layout;
    unsigned char enter[256];
    unsigned char exit[256];
    unsigned char wrong_exit[256];
    size_t enter_length = 0U;
    size_t exit_length = 0U;
    size_t wrong_exit_length = 0U;

    CHECK(init_policy(&policy, 1U),
          "entry fixture initializes active policy");
    CHECK(make_evidence(&evidence, &policy),
          "entry fixture publishes boot evidence");
    CHECK(seal_recovery(
              &policy, RECOVERY_TRUST_ENTER_METADATA,
              RECOVERY_TRUST_ENTER_METADATA_LEN,
              enter, sizeof(enter), &enter_length),
          "entry fixture seals recovery entry");
    CHECK(seal_recovery(
              &policy, RECOVERY_TRUST_EXIT_METADATA,
              RECOVERY_TRUST_EXIT_METADATA_LEN,
              exit, sizeof(exit), &exit_length),
          "entry fixture seals recovery exit");
    CHECK(seal_recovery(
              &policy, "not-exit", sizeof("not-exit") - 1U,
              wrong_exit, sizeof(wrong_exit), &wrong_exit_length),
          "entry fixture seals wrong exit payload");
    memset(&state, 0, sizeof(state));
    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_OK,
          "entry fixture initializes recovery state");

    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, enter, enter_length,
              &layout) == RECOVERY_TRUST_OK &&
              state.version == RECOVERY_TRUST_VERSION &&
              state.mode == RECOVERY_TRUST_MODE_RECOVERY &&
              state.policy_generation == 1U &&
              state.signer_id == 1U &&
              state.active &&
              layout.valid &&
              layout.purpose == KEY_USE_PURPOSE_RECOVERY &&
              recovery_trust_is_recovery(&state),
          "verified recovery entry publishes exact recovery mode");

    CHECK(recovery_trust_authorize(
              &state, RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE) ==
              RECOVERY_TRUST_OK,
          "recovery mode authorizes read-only evidence");
    memcpy(&original, &state, sizeof(state));
    CHECK(recovery_trust_authorize(
              &state, RECOVERY_TRUST_ACTION_CONTROL) ==
              RECOVERY_TRUST_MODE_DENIED &&
              state_unchanged(&state, &original),
          "recovery mode denies control action");
    CHECK(recovery_trust_authorize(
              &state, RECOVERY_TRUST_ACTION_UPDATE) ==
              RECOVERY_TRUST_MODE_DENIED &&
              state_unchanged(&state, &original),
          "recovery mode denies update action");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, enter, enter_length,
              &layout) == RECOVERY_TRUST_BAD_STATE,
          "recovery enter rejects already-active state");

    CHECK(recovery_trust_exit(
              &state, &policy, wrong_exit, wrong_exit_length,
              &layout) == RECOVERY_TRUST_INVALID_METADATA &&
              recovery_trust_is_recovery(&state),
          "recovery exit rejects wrong payload");
    CHECK(recovery_trust_exit(
              &state, &policy, exit, exit_length, &layout) ==
              RECOVERY_TRUST_OK &&
              state.version == RECOVERY_TRUST_VERSION &&
              state.mode == RECOVERY_TRUST_MODE_NORMAL &&
              state.policy_generation == 0U &&
              state.signer_id == 0U &&
              !state.active &&
              !recovery_trust_is_recovery(&state),
          "verified recovery exit returns to normal mode");
    CHECK(recovery_trust_authorize(
              &state, RECOVERY_TRUST_ACTION_CONTROL) ==
              RECOVERY_TRUST_OK,
          "normal mode authorizes control after recovery exit");
    CHECK(recovery_trust_authorize(
              &state, RECOVERY_TRUST_ACTION_UPDATE) ==
              RECOVERY_TRUST_OK,
          "normal mode authorizes update after recovery exit");

    next = make_policy_snapshot(2U);
    CHECK(verification_policy_stage(&policy, &next) ==
              VERIFICATION_POLICY_OK,
          "policy-mismatch fixture stages generation two");
    CHECK(verification_policy_commit(&policy) ==
              VERIFICATION_POLICY_OK,
          "policy-mismatch fixture commits generation two");
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, enter, enter_length,
              &layout) == RECOVERY_TRUST_EVIDENCE_MISMATCH &&
              !recovery_trust_is_recovery(&state),
          "recovery entry rejects evidence from superseded policy");

    CHECK(recovery_trust_enter(
              &state, &policy, &evidence, enter, enter_length,
              &layout) == RECOVERY_TRUST_EVIDENCE_MISMATCH,
          "policy-mismatch fixture cannot enter without fresh evidence");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery entry and exit perform no allocation");
}

static void test_exit_policy_mismatch_keeps_recovery(void)
{
    struct verification_policy policy;
    struct verification_policy_snapshot next;
    struct boot_evidence evidence;
    struct recovery_trust_state state;
    struct recovery_trust_state original;
    struct signed_metadata_layout layout;
    unsigned char enter[256];
    unsigned char exit[256];
    size_t enter_length = 0U;
    size_t exit_length = 0U;

    CHECK(init_policy(&policy, 1U),
          "exit-mismatch fixture initializes policy");
    CHECK(make_evidence(&evidence, &policy),
          "exit-mismatch fixture publishes evidence");
    CHECK(seal_recovery(
              &policy, RECOVERY_TRUST_ENTER_METADATA,
              RECOVERY_TRUST_ENTER_METADATA_LEN,
              enter, sizeof(enter), &enter_length),
          "exit-mismatch fixture seals entry");
    CHECK(seal_recovery(
              &policy, RECOVERY_TRUST_EXIT_METADATA,
              RECOVERY_TRUST_EXIT_METADATA_LEN,
              exit, sizeof(exit), &exit_length),
          "exit-mismatch fixture seals exit under generation one");
    memset(&state, 0, sizeof(state));
    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_OK &&
              recovery_trust_enter(
                  &state, &policy, &evidence, enter, enter_length,
                  &layout) == RECOVERY_TRUST_OK,
          "exit-mismatch fixture enters recovery mode");

    next = make_policy_snapshot(2U);
    CHECK(verification_policy_stage(&policy, &next) ==
              VERIFICATION_POLICY_OK &&
              verification_policy_commit(&policy) ==
                  VERIFICATION_POLICY_OK,
          "exit-mismatch fixture advances active policy");
    memcpy(&original, &state, sizeof(state));
    CHECK(recovery_trust_exit(
              &state, &policy, exit, exit_length, &layout) ==
              RECOVERY_TRUST_POLICY_MISMATCH &&
              state_unchanged(&state, &original) &&
              recovery_trust_is_recovery(&state),
          "recovery exit under new policy is denied and preserves mode");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "recovery exit mismatch performs no allocation");
}

int main(void)
{
    test_status_strings();
    test_null_and_bad_state();
    test_enter_requires_evidence_and_purpose();
    test_recovery_entry_gating_and_exit();
    test_exit_policy_mismatch_keeps_recovery();

    if (failures != 0) {
        fprintf(stderr, "Recovery trust host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Recovery trust host tests: ALL OK\n");
    return 0;
}

#else
#error "This host test must be built with HFOC_RECOVERY_TRUST_HOST_TEST"
#endif
