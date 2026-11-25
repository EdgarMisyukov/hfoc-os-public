#include "audit.h"
#include "boot_evidence.h"
#include "config_kv.h"
#include "event_ring.h"
#include "fault_injection.h"
#include "image_integrity.h"
#include "image_policy.h"
#include "key_use.h"
#include "recovery_trust.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "system_image.h"
#include "trust_anchor.h"
#include "trust_revocation.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_TRUST_FAILURE_DRILL_HOST_TEST

#define TRUST_FAILURE_IMAGE_ID 7U
#define TRUST_FAILURE_IMAGE_METADATA "image-v1"
#define TRUST_FAILURE_IMAGE_METADATA_LEN 8U
#define TRUST_FAILURE_UPDATE_METADATA "update-v1"
#define TRUST_FAILURE_UPDATE_METADATA_LEN 9U

#define KERNEL_DATA "kernel-image"
#define INITRAMFS_DATA "initramfs-image"

#define KERNEL_HASH \
    "7d85a326c4eaf5c09d11bed101a08ea50915897a31b627285b5b3446bd37262f"
#define INITRAMFS_HASH \
    "c94f8be14b260af440b8e93fc1b208e55f8eb554439408552ea5aa078968158b"

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

static struct trust_anchor make_anchor(
    uint32_t id,
    enum trust_anchor_state state,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    memset(&anchor, 0, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.state = state;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static struct verification_policy_snapshot make_snapshot(
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = anchor_count;
    if (anchor_count > 0U && anchors != NULL) {
        memcpy(snapshot.anchors, anchors,
               anchor_count * sizeof(snapshot.anchors[0]));
    }
    return snapshot;
}

static bool init_policy(
    struct verification_policy *policy,
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    snapshot = make_snapshot(generation, anchors, anchor_count);
    memset(policy, 0, sizeof(*policy));
    return verification_policy_init(policy, &snapshot) ==
           VERIFICATION_POLICY_OK;
}

static bool snapshot_equal(
    const struct verification_policy_snapshot *left,
    const struct verification_policy_snapshot *right)
{
    return left != NULL && right != NULL &&
           memcmp(left, right, sizeof(*left)) == 0;
}

static bool active_equal(
    const struct verification_policy *policy,
    const struct verification_policy_snapshot *expected)
{
    return snapshot_equal(verification_policy_active(policy), expected);
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
    if (!make_gate(&gate, TRUST_FAILURE_IMAGE_ID)) {
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
    const char *metadata,
    size_t metadata_length,
    unsigned char *buffer,
    size_t capacity,
    size_t *length_out)
{
    return signed_metadata_seal(
        policy, purpose, signer_id, metadata, metadata_length,
        buffer, capacity, length_out) == SIGNED_METADATA_OK;
}

static bool state_unchanged(
    const struct recovery_trust_state *state,
    const struct recovery_trust_state *original)
{
    return memcmp(state, original, sizeof(*state)) == 0;
}

static void test_trust_fault_catalogue(void)
{
    const struct fault_injection_site *site = NULL;

    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_TRUST_REVOKED_SIGNER, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "trust revoked-signer fault point is exact");
    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_TRUST_TAMPERED_METADATA, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "trust tampered-metadata fault point is exact");
    CHECK(fault_injection_site_by_id(
              FAULT_INJECTION_TRUST_RECOVERY_PURPOSE, &site) ==
              FAULT_INJECTION_OK &&
              site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
              site->bounded && site->test_only &&
              site->expected_outcome != NULL &&
              site->expected_outcome[0] != '\0',
          "trust recovery-purpose fault point is exact");
}

static void test_tampered_metadata_denies_without_mutation(void)
{
    struct trust_anchor anchors[1];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct signed_metadata_layout layout;
    unsigned char valid[256];
    unsigned char tampered[256];
    size_t valid_length = 0U;
    size_t tampered_length = 0U;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 1U);
    CHECK(init_policy(&policy, 1U, anchors, 1U),
          "tamper fixture initializes active policy");
    memset(&layout, 0, sizeof(layout));
    CHECK(seal_envelope(
              &policy, KEY_USE_PURPOSE_IMAGE, 1U,
              TRUST_FAILURE_IMAGE_METADATA,
              TRUST_FAILURE_IMAGE_METADATA_LEN,
              valid, sizeof(valid), &valid_length) &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_IMAGE,
                  valid, valid_length, &layout) ==
                  SIGNED_METADATA_OK &&
              layout.valid &&
              layout.signer_id == 1U,
          "tamper fixture seals and verifies valid metadata");

    memcpy(tampered, valid, valid_length);
    tampered_length = valid_length;
    tampered[SIGNED_METADATA_HEADER_SIZE] ^= 0x01U;
    memset(&layout, 0, sizeof(layout));
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE,
              tampered, tampered_length, &layout) ==
              SIGNED_METADATA_BAD_SIGNATURE &&
              !layout.valid,
          "tampered metadata is denied with exact signature mismatch");
    memset(&layout, 0, sizeof(layout));
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_IMAGE,
              valid, valid_length, &layout) == SIGNED_METADATA_OK &&
              active_equal(&policy, &initial) &&
              !verification_policy_has_pending(&policy),
          "tamper denial preserves original metadata and active policy");
}

static void test_revoked_signer_denies_with_known_good(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct event_ring ring;
    struct trust_revocation revocation;
    struct audit_record record;
    struct signed_metadata_layout layout;
    unsigned char old_recovery[256];
    unsigned char good_recovery[256];
    size_t old_recovery_length = 0U;
    size_t good_recovery_length = 0U;
    uint64_t sequence = UINT64_MAX;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    CHECK(init_policy(&policy, 1U, anchors, 2U),
          "revocation fixture initializes active policy");
    memset(&ring, 0, sizeof(ring));
    CHECK(event_ring_init(&ring) == EVENT_RING_OK &&
              trust_revocation_init(&revocation) == TRUST_REVOCATION_OK,
          "revocation fixture initializes audit ring and transaction");
    CHECK(seal_envelope(
              &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
              RECOVERY_TRUST_ENTER_METADATA,
              RECOVERY_TRUST_ENTER_METADATA_LEN,
              old_recovery, sizeof(old_recovery),
              &old_recovery_length) &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_RECOVERY,
                  old_recovery, old_recovery_length, &layout) ==
                  SIGNED_METADATA_OK,
          "revocation fixture seals valid signer-one recovery envelope");

    CHECK(trust_revocation_revoke(
              &revocation, &policy, 1U, &ring, &sequence) ==
              TRUST_REVOCATION_OK &&
              sequence == 0U &&
              trust_revocation_anchor_is_revoked(
                  &revocation, &policy, 1U) &&
              event_ring_count(&ring) == 1U &&
              revocation.last_anchor_id == 1U &&
              revocation.last_generation == 2U &&
              verification_policy_active(&policy)->generation == 2U &&
              !verification_policy_has_pending(&policy),
          "revocation publishes explicit revoked state with exact audit");
    memset(&layout, 0, sizeof(layout));
    CHECK(signed_metadata_verify(
              &policy, KEY_USE_PURPOSE_RECOVERY,
              old_recovery, old_recovery_length, &layout) ==
              SIGNED_METADATA_UNKNOWN_SIGNER &&
              !layout.valid &&
              signed_metadata_seal(
                  &policy, KEY_USE_PURPOSE_IMAGE, 1U,
                  TRUST_FAILURE_IMAGE_METADATA,
                  TRUST_FAILURE_IMAGE_METADATA_LEN,
                  old_recovery, sizeof(old_recovery),
                  &old_recovery_length) == SIGNED_METADATA_UNKNOWN_SIGNER,
          "revoked signer cannot verify or seal new metadata");

    CHECK(seal_envelope(
              &policy, KEY_USE_PURPOSE_RECOVERY, 2U,
              RECOVERY_TRUST_ENTER_METADATA,
              RECOVERY_TRUST_ENTER_METADATA_LEN,
              good_recovery, sizeof(good_recovery),
              &good_recovery_length) &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_RECOVERY,
                  good_recovery, good_recovery_length, &layout) ==
                  SIGNED_METADATA_OK &&
              layout.signer_id == 2U &&
              active_equal(
                  &policy, &initial) == false &&
              verification_policy_active(&policy)->anchor_count == 2U,
          "active signer-two preserves known-good trust operation");
    memset(&record, 0, sizeof(record));
    CHECK(audit_record_pop(&ring, &record) == AUDIT_OK &&
              record.sequence == 0U &&
              record.type == AUDIT_RECORD_TRUST &&
              record.operation == TRUST_REVOCATION_AUDIT_OPERATION_REVOKE &&
              record.result == TRUST_REVOCATION_AUDIT_RESULT_OK &&
              record.pid == 1U &&
              record.detail ==
                  (1U | (2U << TRUST_REVOCATION_DETAIL_GENERATION_SHIFT)) &&
              event_ring_count(&ring) == 0U,
          "revocation audit event is consumed exactly once");
}

static void test_recovery_purpose_denies_with_known_good(void)
{
    struct trust_anchor anchors[2];
    struct verification_policy_snapshot initial;
    struct verification_policy policy;
    struct boot_evidence evidence;
    struct recovery_trust_state state;
    struct recovery_trust_state original;
    struct signed_metadata_layout layout;
    unsigned char update[256];
    unsigned char enter[256];
    unsigned char exit[256];
    size_t update_length = 0U;
    size_t enter_length = 0U;
    size_t exit_length = 0U;

    anchors[0] = make_anchor(
        1U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = make_anchor(
        2U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial = make_snapshot(1U, anchors, 2U);
    CHECK(init_policy(&policy, 1U, anchors, 2U) &&
              make_evidence(&evidence, &policy),
          "recovery fixture initializes policy and boot evidence");
    CHECK(seal_envelope(
              &policy, KEY_USE_PURPOSE_UPDATE, 1U,
              TRUST_FAILURE_UPDATE_METADATA,
              TRUST_FAILURE_UPDATE_METADATA_LEN,
              update, sizeof(update), &update_length) &&
              seal_envelope(
                  &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
                  RECOVERY_TRUST_ENTER_METADATA,
                  RECOVERY_TRUST_ENTER_METADATA_LEN,
                  enter, sizeof(enter), &enter_length) &&
              seal_envelope(
                  &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
                  RECOVERY_TRUST_EXIT_METADATA,
                  RECOVERY_TRUST_EXIT_METADATA_LEN,
                  exit, sizeof(exit), &exit_length),
          "recovery fixture seals update, entry, and exit envelopes");

    memset(&state, 0, sizeof(state));
    CHECK(recovery_trust_init(&state) == RECOVERY_TRUST_OK,
          "recovery fixture initializes normal state");
    memcpy(&original, &state, sizeof(state));
    memset(&layout, 0, sizeof(layout));
    CHECK(recovery_trust_enter(
              &state, &policy, &evidence,
              update, update_length, &layout) ==
              RECOVERY_TRUST_METADATA_DENIED &&
              state_unchanged(&state, &original) &&
              signed_metadata_verify(
                  &policy, KEY_USE_PURPOSE_RECOVERY,
                  update, update_length, &layout) ==
                  SIGNED_METADATA_PURPOSE_DENIED &&
              active_equal(&policy, &initial) &&
              !verification_policy_has_pending(&policy),
          "recovery-purpose mismatch is denied without state mutation");

    CHECK(recovery_trust_enter(
              &state, &policy, &evidence,
              enter, enter_length, &layout) == RECOVERY_TRUST_OK &&
              recovery_trust_is_recovery(&state) &&
              recovery_trust_authorize(
                  &state, RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE) ==
                  RECOVERY_TRUST_OK &&
              recovery_trust_authorize(
                  &state, RECOVERY_TRUST_ACTION_CONTROL) ==
                  RECOVERY_TRUST_MODE_DENIED &&
              recovery_trust_authorize(
                  &state, RECOVERY_TRUST_ACTION_UPDATE) ==
                  RECOVERY_TRUST_MODE_DENIED,
          "valid recovery entry publishes strict action gating");
    CHECK(recovery_trust_exit(
              &state, &policy, exit, exit_length, &layout) ==
              RECOVERY_TRUST_OK &&
              !recovery_trust_is_recovery(&state) &&
              recovery_trust_authorize(
                  &state, RECOVERY_TRUST_ACTION_CONTROL) ==
                  RECOVERY_TRUST_OK &&
              active_equal(&policy, &initial),
          "known-good normal control returns after verified exit");
}

int main(void)
{
    test_trust_fault_catalogue();
    test_tampered_metadata_denies_without_mutation();
    test_revoked_signer_denies_with_known_good();
    test_recovery_purpose_denies_with_known_good();

    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Trust failure drill host tests performed allocation\n");
        ++failures;
    }
    if (failures != 0) {
        fprintf(stderr,
                "Trust failure drill host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    printf("Trust failure drill host tests: ALL OK\n");
    return 0;
}

#else
#error "trust-failure-drill-host.c must be compiled with HFOC_TRUST_FAILURE_DRILL_HOST_TEST"
#endif
