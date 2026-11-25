#ifdef HFOC_TRUST_POLICY_QEMU_TEST

#include "audit.h"
#include "boot_evidence.h"
#include "config_kv.h"
#include "event_ring.h"
#include "image_integrity.h"
#include "image_policy.h"
#include "key_use.h"
#include "kstring.h"
#include "recovery_trust.h"
#include "serial.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "system_image.h"
#include "trust_anchor.h"
#include "trust_revocation.h"
#include "trust_rotation.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TRUST_POLICY_AUDIT_CAPACITY 4U
#define TRUST_POLICY_IMAGE_METADATA "image-v1"
#define TRUST_POLICY_IMAGE_METADATA_LEN 8U
#define TRUST_POLICY_UPDATE_METADATA "update-v1"
#define TRUST_POLICY_UPDATE_METADATA_LEN 9U
#define TRUST_POLICY_CURRENT_IMAGE_METADATA "image-v2"
#define TRUST_POLICY_CURRENT_IMAGE_METADATA_LEN 8U
#define TRUST_POLICY_IMAGE_ID 7U

#define KERNEL_DATA "kernel-image"
#define INITRAMFS_DATA "initramfs-image"

#define KERNEL_HASH \
    "7d85a326c4eaf5c09d11bed101a08ea50915897a31b627285b5b3446bd37262f"
#define INITRAMFS_HASH \
    "c94f8be14b260af440b8e93fc1b208e55f8eb554439408552ea5aa078968158b"

static void trust_policy_qemu_fail(const char *message)
    __attribute__((noreturn));

static void trust_policy_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[TRUST-POLICY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void trust_policy_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        trust_policy_qemu_fail(message);
    }
}

static void trust_policy_qemu_zero(void *destination, size_t length)
{
    memset(destination, 0, length);
}

static bool trust_policy_qemu_equal(const void *left,
                                    const void *right,
                                    size_t length)
{
    return memcmp(left, right, length) == 0;
}

static void trust_policy_qemu_copy_string(char *destination,
                                          size_t capacity,
                                          const char *source)
{
    size_t index = 0U;

    while (source[index] != '\0' && index + 1U < capacity) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static struct trust_anchor trust_policy_qemu_make_anchor(
    uint32_t id,
    enum trust_anchor_state state,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    trust_policy_qemu_zero(&anchor, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.state = state;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static struct verification_policy_snapshot
trust_policy_qemu_make_snapshot(
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    trust_policy_qemu_zero(&snapshot, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = anchor_count;
    memcpy(snapshot.anchors, anchors,
           anchor_count * sizeof(snapshot.anchors[0]));
    return snapshot;
}

static uint32_t trust_policy_qemu_anchor_state(
    const struct verification_policy_snapshot *snapshot,
    uint32_t anchor_id)
{
    uint32_t index;

    for (index = 0U; index < snapshot->anchor_count; ++index) {
        if (snapshot->anchors[index].id == anchor_id) {
            return (uint32_t)snapshot->anchors[index].state;
        }
    }
    return UINT32_MAX;
}

static void trust_policy_qemu_make_manifest(
    struct system_image_manifest *manifest,
    uint32_t image_id)
{
    trust_policy_qemu_zero(manifest, sizeof(*manifest));
    manifest->manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION;
    manifest->image_id = image_id;
    manifest->min_kernel_abi = 1U;
    manifest->max_kernel_abi = 3U;
    trust_policy_qemu_copy_string(
        manifest->arch, sizeof(manifest->arch), SYSTEM_IMAGE_ARCH_VALUE);
    manifest->component_count = 2U;
    trust_policy_qemu_copy_string(
        manifest->components[0].name,
        sizeof(manifest->components[0].name), "kernel");
    trust_policy_qemu_copy_string(
        manifest->components[0].version,
        sizeof(manifest->components[0].version), "0.2.6");
    trust_policy_qemu_copy_string(
        manifest->components[0].hash,
        sizeof(manifest->components[0].hash), KERNEL_HASH);
    trust_policy_qemu_copy_string(
        manifest->components[1].name,
        sizeof(manifest->components[1].name), "initramfs");
    trust_policy_qemu_copy_string(
        manifest->components[1].version,
        sizeof(manifest->components[1].version), "1.0");
    trust_policy_qemu_copy_string(
        manifest->components[1].hash,
        sizeof(manifest->components[1].hash), INITRAMFS_HASH);
}

static void trust_policy_qemu_make_payloads(
    struct image_integrity_payload payloads[2])
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

static bool trust_policy_qemu_make_gate(
    struct image_integrity_gate *gate,
    uint32_t image_id)
{
    struct system_image_manifest manifest;
    struct image_integrity_payload payloads[2];

    trust_policy_qemu_make_manifest(&manifest, image_id);
    trust_policy_qemu_make_payloads(payloads);
    trust_policy_qemu_zero(gate, sizeof(*gate));
    return image_integrity_verify(gate, &manifest, payloads, 2U) ==
           IMAGE_INTEGRITY_OK;
}

static void trust_policy_qemu_make_measurement(
    uint8_t measurement[SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        measurement[index] = (uint8_t)(0xA0U + index);
    }
}

static bool trust_policy_qemu_seal(
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

static bool trust_policy_qemu_layout_matches(
    const struct signed_metadata_layout *layout,
    const unsigned char *envelope,
    uint32_t expected_purpose,
    uint32_t expected_signer_id,
    const char *expected_metadata,
    size_t expected_metadata_length)
{
    return layout->valid &&
           layout->purpose == expected_purpose &&
           layout->signer_id == expected_signer_id &&
           layout->metadata_length == expected_metadata_length &&
           trust_policy_qemu_equal(
               envelope + layout->metadata_offset,
               expected_metadata, expected_metadata_length);
}

void trust_policy_qemu_test_init(void)
{
    static struct trust_anchor anchors[3];
    static struct verification_policy policy;
    static struct verification_policy_snapshot initial_snapshot;
    static const struct verification_policy_snapshot *active;
    static struct trust_rotation rotation;
    static struct trust_anchor new_anchor;
    static struct trust_revocation revocation;
    struct event_ring *const ring = kernel_event_ring_get();
    static uint64_t audit_sequence = UINT64_MAX;
    static struct audit_record trust_record;
    static struct image_integrity_gate gate;
    static struct image_policy_binding binding;
    static struct boot_evidence evidence;
    static struct boot_evidence loaded;
    static struct config_kv_entry audit_entries[TRUST_POLICY_AUDIT_CAPACITY];
    static struct config_kv audit_kv;
    static uint8_t measurement[SHA256_DIGEST_SIZE];
    static unsigned char image_envelope[256];
    static unsigned char tampered_envelope[256];
    static unsigned char update_envelope[256];
    static unsigned char current_image_envelope[256];
    static unsigned char recovery_enter_envelope[256];
    static unsigned char recovery_exit_envelope[256];
    static size_t image_length = 0U;
    static size_t update_length = 0U;
    static size_t current_image_length = 0U;
    static size_t recovery_enter_length = 0U;
    static size_t recovery_exit_length = 0U;
    static struct signed_metadata_layout layout;
    static struct recovery_trust_state recovery_state;

    trust_policy_qemu_require(
        ring != NULL && event_ring_is_published(ring) &&
        event_ring_count(ring) == 0U,
        "kernel event ring fixture");
    serial_write("[TRUST-POLICY] proof init\n");

    anchors[0] = trust_policy_qemu_make_anchor(
        1U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = trust_policy_qemu_make_anchor(
        2U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial_snapshot = trust_policy_qemu_make_snapshot(
        1U, anchors, 2U);
    trust_policy_qemu_require(
        verification_policy_snapshot_validate(&initial_snapshot) ==
            VERIFICATION_POLICY_OK,
        "trust anchor snapshot validation");
    trust_policy_qemu_zero(&policy, sizeof(policy));
    trust_policy_qemu_require(
        verification_policy_init(&policy, &initial_snapshot) ==
            VERIFICATION_POLICY_OK,
        "verification policy publication");

    trust_policy_qemu_require(
        trust_policy_qemu_seal(
            &policy, KEY_USE_PURPOSE_IMAGE, 1U,
            TRUST_POLICY_IMAGE_METADATA,
            TRUST_POLICY_IMAGE_METADATA_LEN,
            image_envelope, sizeof(image_envelope), &image_length),
        "signed image envelope seal");
    trust_policy_qemu_zero(&layout, sizeof(layout));
    trust_policy_qemu_require(
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_IMAGE,
            image_envelope, image_length, &layout) ==
            SIGNED_METADATA_OK &&
        trust_policy_qemu_layout_matches(
            &layout, image_envelope, KEY_USE_PURPOSE_IMAGE, 1U,
            TRUST_POLICY_IMAGE_METADATA,
            TRUST_POLICY_IMAGE_METADATA_LEN),
        "valid signed metadata verification");
    serial_write("[TRUST-POLICY] valid verification\n");

    memcpy(tampered_envelope, image_envelope, image_length);
    tampered_envelope[SIGNED_METADATA_HEADER_SIZE] ^= 0x01U;
    trust_policy_qemu_require(
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_IMAGE,
            tampered_envelope, image_length, &layout) !=
            SIGNED_METADATA_OK,
        "tampered signed metadata rejection");
    serial_write("[TRUST-POLICY] tamper rejection\n");

    trust_policy_qemu_require(
        trust_policy_qemu_seal(
            &policy, KEY_USE_PURPOSE_UPDATE, 1U,
            TRUST_POLICY_UPDATE_METADATA,
            TRUST_POLICY_UPDATE_METADATA_LEN,
            update_envelope, sizeof(update_envelope), &update_length),
        "update envelope seal");
    trust_policy_qemu_require(
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_UPDATE,
            image_envelope, image_length, &layout) ==
            SIGNED_METADATA_PURPOSE_DENIED &&
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_IMAGE,
            update_envelope, update_length, &layout) ==
            SIGNED_METADATA_PURPOSE_DENIED,
        "wrong-purpose signed metadata rejection");
    serial_write("[TRUST-POLICY] wrong-purpose rejection\n");

    trust_policy_qemu_require(
        trust_rotation_init(&rotation) == TRUST_ROTATION_OK,
        "rotation transaction init");
    new_anchor = trust_policy_qemu_make_anchor(
        3U, TRUST_ANCHOR_STATE_INACTIVE,
        KEY_USE_ACCEPTED_PURPOSES_ALL);
    trust_policy_qemu_require(
        trust_rotation_stage(&rotation, &policy, &new_anchor) ==
            TRUST_ROTATION_OK &&
        trust_rotation_has_staged(&rotation) &&
        trust_rotation_prepare(&rotation, &policy) ==
            TRUST_ROTATION_OK &&
        verification_policy_has_pending(&policy) &&
        verification_policy_active(&policy)->generation == 1U,
        "rotation prepared candidate");
    trust_policy_qemu_require(
        trust_rotation_rollback(&rotation, &policy) ==
            TRUST_ROTATION_OK &&
        !verification_policy_has_pending(&policy) &&
        !trust_rotation_has_staged(&rotation) &&
        verification_policy_active(&policy)->generation == 1U,
        "interrupted rotation preserves active policy");
    trust_policy_qemu_require(
        trust_rotation_stage(&rotation, &policy, &new_anchor) ==
            TRUST_ROTATION_OK &&
        trust_rotation_prepare(&rotation, &policy) ==
            TRUST_ROTATION_OK &&
        trust_rotation_activate(&rotation, &policy) ==
            TRUST_ROTATION_OK,
        "trust anchor rotation activation");
    active = verification_policy_active(&policy);
    trust_policy_qemu_require(
        active != NULL &&
        active->generation == 2U &&
        trust_policy_qemu_anchor_state(active, 3U) ==
            (uint32_t)TRUST_ANCHOR_STATE_ACTIVE,
        "rotated anchor active in generation two");
    serial_write("[TRUST-POLICY] rotation activated\n");

    trust_policy_qemu_require(
        trust_revocation_init(&revocation) == TRUST_REVOCATION_OK,
        "revocation transaction init");
    trust_policy_qemu_require(
        trust_revocation_revoke(
            &revocation, &policy, 2U, ring, &audit_sequence) ==
            TRUST_REVOCATION_OK &&
        audit_sequence == 0U &&
        trust_revocation_anchor_is_revoked(&revocation, &policy, 2U),
        "trust anchor revocation");
    active = verification_policy_active(&policy);
    trust_policy_qemu_require(
        active != NULL &&
        active->generation == 3U &&
        trust_policy_qemu_anchor_state(active, 2U) ==
            (uint32_t)TRUST_ANCHOR_STATE_REVOKED,
        "revoked anchor state in generation three");
    trust_policy_qemu_zero(&trust_record, sizeof(trust_record));
    trust_policy_qemu_require(
        audit_record_pop(ring, &trust_record) == AUDIT_OK &&
        trust_record.sequence == 0U &&
        trust_record.type == AUDIT_RECORD_TRUST &&
        trust_record.operation == TRUST_REVOCATION_AUDIT_OPERATION_REVOKE &&
        trust_record.result == TRUST_REVOCATION_AUDIT_RESULT_OK &&
        trust_record.pid == 2U &&
        trust_record.domain == 0U &&
        trust_record.generation == 0U &&
        trust_record.detail ==
            (2U | (3U << TRUST_REVOCATION_DETAIL_GENERATION_SHIFT)) &&
        event_ring_count(ring) == 0U,
        "trust revocation audit exact-once consume");
    trust_policy_qemu_require(
        signed_metadata_seal(
            &policy, KEY_USE_PURPOSE_IMAGE, 2U,
            TRUST_POLICY_CURRENT_IMAGE_METADATA,
            TRUST_POLICY_CURRENT_IMAGE_METADATA_LEN,
            current_image_envelope, sizeof(current_image_envelope),
            &current_image_length) == SIGNED_METADATA_UNKNOWN_SIGNER,
        "revoked signer cannot seal new metadata");
    serial_write("[TRUST-POLICY] revocation enforced\n");

    trust_policy_qemu_require(
        trust_policy_qemu_make_gate(&gate, TRUST_POLICY_IMAGE_ID),
        "verified system image gate");
    trust_policy_qemu_zero(&binding, sizeof(binding));
    trust_policy_qemu_require(
        image_policy_bind(&binding, &policy, &gate) ==
            IMAGE_POLICY_OK,
        "image policy binding to active generation");
    trust_policy_qemu_make_measurement(measurement);
    trust_policy_qemu_zero(&evidence, sizeof(evidence));
    trust_policy_qemu_require(
        boot_evidence_init(
            &evidence, &policy, &binding, measurement) ==
            BOOT_EVIDENCE_OK,
        "measured boot evidence publication");
    trust_policy_qemu_zero(audit_entries, sizeof(audit_entries));
    trust_policy_qemu_require(
        config_kv_init(
            &audit_kv, audit_entries, TRUST_POLICY_AUDIT_CAPACITY) ==
            CONFIG_KV_OK &&
        boot_evidence_save(&evidence, &audit_kv) == BOOT_EVIDENCE_OK,
        "boot evidence audit continuity save");
    trust_policy_qemu_zero(&loaded, sizeof(loaded));
    trust_policy_qemu_require(
        boot_evidence_load(&loaded, &audit_kv) == BOOT_EVIDENCE_OK &&
        loaded.policy_generation == 3U &&
        loaded.image_id == TRUST_POLICY_IMAGE_ID &&
        loaded.digest_match &&
        boot_evidence_check_policy(&loaded, &policy) == BOOT_EVIDENCE_OK,
        "boot evidence audit continuity load");
    serial_write("[TRUST-POLICY] audit continuity\n");

    trust_policy_qemu_require(
        trust_policy_qemu_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
            RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN,
            recovery_enter_envelope, sizeof(recovery_enter_envelope),
            &recovery_enter_length) &&
        trust_policy_qemu_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
            RECOVERY_TRUST_EXIT_METADATA,
            RECOVERY_TRUST_EXIT_METADATA_LEN,
            recovery_exit_envelope, sizeof(recovery_exit_envelope),
            &recovery_exit_length) &&
        trust_policy_qemu_seal(
            &policy, KEY_USE_PURPOSE_IMAGE, 1U,
            TRUST_POLICY_CURRENT_IMAGE_METADATA,
            TRUST_POLICY_CURRENT_IMAGE_METADATA_LEN,
            current_image_envelope, sizeof(current_image_envelope),
            &current_image_length),
        "recovery and current image envelopes");
    trust_policy_qemu_require(
        recovery_trust_init(&recovery_state) == RECOVERY_TRUST_OK,
        "recovery trust state init");
    trust_policy_qemu_require(
        recovery_trust_enter(
            &recovery_state, &policy, &loaded,
            current_image_envelope, current_image_length, &layout) ==
            RECOVERY_TRUST_METADATA_DENIED &&
        !recovery_trust_is_recovery(&recovery_state),
        "image-purpose recovery entry denial");
    trust_policy_qemu_require(
        recovery_trust_enter(
            &recovery_state, &policy, &loaded,
            recovery_enter_envelope, recovery_enter_length, &layout) ==
            RECOVERY_TRUST_OK &&
        recovery_state.mode == RECOVERY_TRUST_MODE_RECOVERY &&
        recovery_state.policy_generation == 3U &&
        recovery_state.signer_id == 1U &&
        recovery_state.active &&
        recovery_trust_is_recovery(&recovery_state) &&
        recovery_trust_authorize(
            &recovery_state, RECOVERY_TRUST_ACTION_READ_ONLY_EVIDENCE) ==
            RECOVERY_TRUST_OK &&
        recovery_trust_authorize(
            &recovery_state, RECOVERY_TRUST_ACTION_CONTROL) ==
            RECOVERY_TRUST_MODE_DENIED &&
        recovery_trust_authorize(
            &recovery_state, RECOVERY_TRUST_ACTION_UPDATE) ==
            RECOVERY_TRUST_MODE_DENIED,
        "recovery mode verified entry and action gating");
    trust_policy_qemu_require(
        recovery_trust_exit(
            &recovery_state, &policy,
            recovery_exit_envelope, recovery_exit_length, &layout) ==
            RECOVERY_TRUST_OK &&
        !recovery_trust_is_recovery(&recovery_state) &&
        recovery_state.mode == RECOVERY_TRUST_MODE_NORMAL &&
        recovery_trust_authorize(
            &recovery_state, RECOVERY_TRUST_ACTION_CONTROL) ==
            RECOVERY_TRUST_OK,
        "recovery mode verified exit and normal control");
    serial_write(
        "[TRUST-POLICY] recovery restrictions enforced\n");
    serial_write("[TRUST-POLICY] clean reboot\n");
    serial_write("[TRUST-POLICY] proof OK\n");
}

#else
typedef int trust_policy_qemu_translation_unit_guard;
#endif
