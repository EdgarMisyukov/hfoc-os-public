#ifdef HFOC_TRUST_FAILURE_DRILL_QEMU_TEST

#include "audit.h"
#include "boot_evidence.h"
#include "event_ring.h"
#include "fault_injection.h"
#include "image_integrity.h"
#include "image_policy.h"
#include "key_use.h"
#include "recovery_trust.h"
#include "serial.h"
#include "sha256.h"
#include "signed_metadata.h"
#include "system_image.h"
#include "trust_anchor.h"
#include "trust_revocation.h"
#include "verification_policy.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

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

static void trust_failure_drill_fail(const char *message)
    __attribute__((noreturn));

static void trust_failure_drill_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[TRUST-FAILURE-DRILL] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void trust_failure_drill_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        trust_failure_drill_fail(message);
    }
}

static void trust_failure_drill_zero(
    void *destination,
    size_t length)
{
    unsigned char *const bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool trust_failure_drill_equal(
    const void *left,
    const void *right,
    size_t length)
{
    const unsigned char *const left_bytes = left;
    const unsigned char *const right_bytes = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (left_bytes[index] != right_bytes[index]) {
            return false;
        }
    }
    return true;
}

static void trust_failure_drill_copy(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *const output = destination;
    const unsigned char *const input = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static void trust_failure_drill_copy_string(
    char *destination,
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

static struct trust_anchor trust_failure_drill_make_anchor(
    uint32_t id,
    enum trust_anchor_state state,
    uint32_t accepted_purposes)
{
    struct trust_anchor anchor;

    trust_failure_drill_zero(&anchor, sizeof(anchor));
    anchor.id = id;
    anchor.version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    anchor.algorithm_id = TRUST_ANCHOR_ALGORITHM_ED25519;
    anchor.state = state;
    anchor.accepted_purposes = accepted_purposes;
    return anchor;
}

static struct verification_policy_snapshot
trust_failure_drill_make_snapshot(
    uint32_t generation,
    const struct trust_anchor *anchors,
    uint32_t anchor_count)
{
    struct verification_policy_snapshot snapshot;

    trust_failure_drill_zero(&snapshot, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = generation;
    snapshot.anchor_count = anchor_count;
    trust_failure_drill_copy(
        snapshot.anchors, anchors,
        anchor_count * sizeof(snapshot.anchors[0]));
    return snapshot;
}

static void trust_failure_drill_make_manifest(
    struct system_image_manifest *manifest,
    uint32_t image_id)
{
    trust_failure_drill_zero(manifest, sizeof(*manifest));
    manifest->manifest_version = SYSTEM_IMAGE_MANIFEST_VERSION;
    manifest->image_id = image_id;
    manifest->min_kernel_abi = 1U;
    manifest->max_kernel_abi = 3U;
    trust_failure_drill_copy_string(
        manifest->arch, sizeof(manifest->arch), SYSTEM_IMAGE_ARCH_VALUE);
    manifest->component_count = 2U;
    trust_failure_drill_copy_string(
        manifest->components[0].name,
        sizeof(manifest->components[0].name), "kernel");
    trust_failure_drill_copy_string(
        manifest->components[0].version,
        sizeof(manifest->components[0].version), "0.2.6");
    trust_failure_drill_copy_string(
        manifest->components[0].hash,
        sizeof(manifest->components[0].hash), KERNEL_HASH);
    trust_failure_drill_copy_string(
        manifest->components[1].name,
        sizeof(manifest->components[1].name), "initramfs");
    trust_failure_drill_copy_string(
        manifest->components[1].version,
        sizeof(manifest->components[1].version), "1.0");
    trust_failure_drill_copy_string(
        manifest->components[1].hash,
        sizeof(manifest->components[1].hash), INITRAMFS_HASH);
}

static void trust_failure_drill_make_payloads(
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

static bool trust_failure_drill_make_gate(
    struct image_integrity_gate *gate,
    uint32_t image_id)
{
    struct system_image_manifest manifest;
    struct image_integrity_payload payloads[2];

    trust_failure_drill_make_manifest(&manifest, image_id);
    trust_failure_drill_make_payloads(payloads);
    trust_failure_drill_zero(gate, sizeof(*gate));
    return image_integrity_verify(gate, &manifest, payloads, 2U) ==
           IMAGE_INTEGRITY_OK;
}

static void trust_failure_drill_make_measurement(
    uint8_t measurement[SHA256_DIGEST_SIZE])
{
    size_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        measurement[index] = (uint8_t)(0xA0U + index);
    }
}

static bool trust_failure_drill_make_evidence(
    struct boot_evidence *evidence,
    const struct verification_policy *policy)
{
    struct image_integrity_gate gate;
    struct image_policy_binding binding;
    uint8_t measurement[SHA256_DIGEST_SIZE];

    trust_failure_drill_make_measurement(measurement);
    if (!trust_failure_drill_make_gate(
            &gate, TRUST_FAILURE_IMAGE_ID)) {
        return false;
    }
    trust_failure_drill_zero(&binding, sizeof(binding));
    if (image_policy_bind(&binding, policy, &gate) != IMAGE_POLICY_OK) {
        return false;
    }
    trust_failure_drill_zero(evidence, sizeof(*evidence));
    return boot_evidence_init(
        evidence, policy, &binding, measurement) == BOOT_EVIDENCE_OK;
}

static bool trust_failure_drill_seal(
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

void trust_failure_drill_qemu_test_init(void)
{
    /*
     * ITEM-393: the fixtures live in .bss, not on the boot stack.
     *
     * This proof runs once from kmain, i.e. on the stack the bootloader handed
     * over: nine kilobytes of fixtures there is the same defect shape that
     * overflowed the boot stack and triple-faulted the storage-interruption
     * target into a silent reboot. The function stays single-shot and
     * non-reentrant; only the storage class of its fixtures changes.
     */
    static struct trust_anchor anchors[2];
    static struct verification_policy_snapshot initial_snapshot;
    static struct verification_policy policy;
    static struct event_ring ring;
    static struct trust_revocation revocation;
    static struct audit_record trust_record;
    static struct signed_metadata_layout layout;
    static struct boot_evidence evidence;
    static struct recovery_trust_state state;
    static struct recovery_trust_state original;
    static unsigned char signer1_recovery[256];
    static unsigned char signer2_recovery[256];
    static unsigned char tampered[256];
    static unsigned char update[256];
    static unsigned char exit_envelope[256];
    const struct fault_injection_site *site = NULL;
    size_t signer1_recovery_length = 0U;
    size_t signer2_recovery_length = 0U;
    size_t tampered_length = 0U;
    size_t update_length = 0U;
    size_t exit_length = 0U;
    uint64_t sequence = UINT64_MAX;

    serial_write("[TRUST-FAILURE-DRILL] proof init\n");

    trust_failure_drill_require(
        fault_injection_site_by_id(
            FAULT_INJECTION_TRUST_REVOKED_SIGNER, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
            site->bounded && site->test_only &&
            site->expected_outcome != NULL &&
            site->expected_outcome[0] != '\0' &&
        fault_injection_site_by_id(
            FAULT_INJECTION_TRUST_TAMPERED_METADATA, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
            site->bounded && site->test_only &&
            site->expected_outcome != NULL &&
            site->expected_outcome[0] != '\0' &&
        fault_injection_site_by_id(
            FAULT_INJECTION_TRUST_RECOVERY_PURPOSE, &site) ==
            FAULT_INJECTION_OK &&
            site->subsystem == FAULT_INJECTION_SUBSYSTEM_TRUST &&
            site->bounded && site->test_only &&
            site->expected_outcome != NULL &&
            site->expected_outcome[0] != '\0',
        "catalogue trust fault points");
    serial_write(
        "[TRUST-FAILURE-DRILL] catalogue trust fault points exact\n");

    anchors[0] = trust_failure_drill_make_anchor(
        1U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    anchors[1] = trust_failure_drill_make_anchor(
        2U, TRUST_ANCHOR_STATE_ACTIVE, KEY_USE_ACCEPTED_PURPOSES_ALL);
    initial_snapshot = trust_failure_drill_make_snapshot(
        1U, anchors, 2U);
    trust_failure_drill_zero(&policy, sizeof(policy));
    trust_failure_drill_require(
        verification_policy_init(
            &policy, &initial_snapshot) == VERIFICATION_POLICY_OK,
        "verification policy publication");

    trust_failure_drill_require(
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
            RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN,
            signer1_recovery, sizeof(signer1_recovery),
            &signer1_recovery_length) &&
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            signer1_recovery, signer1_recovery_length, &layout) ==
            SIGNED_METADATA_OK &&
        layout.valid && layout.signer_id == 1U,
        "valid signer-one recovery envelope");

    trust_failure_drill_copy(
        tampered, signer1_recovery, signer1_recovery_length);
    tampered_length = signer1_recovery_length;
    tampered[SIGNED_METADATA_HEADER_SIZE] ^= 0x01U;
    trust_failure_drill_zero(&layout, sizeof(layout));
    trust_failure_drill_require(
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            tampered, tampered_length, &layout) ==
            SIGNED_METADATA_BAD_SIGNATURE &&
        !layout.valid &&
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            signer1_recovery, signer1_recovery_length, &layout) ==
            SIGNED_METADATA_OK &&
        verification_policy_active(&policy)->generation == 1U &&
        !verification_policy_has_pending(&policy),
        "tampered metadata denial preserves known-good envelope");
    serial_write("[TRUST-FAILURE-DRILL] tampered metadata denied\n");

    trust_failure_drill_zero(&ring, sizeof(ring));
    trust_failure_drill_require(
        event_ring_init(&ring) == EVENT_RING_OK &&
        trust_revocation_init(&revocation) == TRUST_REVOCATION_OK,
        "revocation audit fixture");
    trust_failure_drill_require(
        trust_revocation_revoke(
            &revocation, &policy, 1U, &ring, &sequence) ==
            TRUST_REVOCATION_OK &&
        sequence == 0U &&
        trust_revocation_anchor_is_revoked(
            &revocation, &policy, 1U) &&
        event_ring_count(&ring) == 1U &&
        verification_policy_active(&policy)->generation == 2U &&
        !verification_policy_has_pending(&policy),
        "revoked signer publication");
    trust_failure_drill_zero(&layout, sizeof(layout));
    trust_failure_drill_require(
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            signer1_recovery, signer1_recovery_length, &layout) ==
            SIGNED_METADATA_UNKNOWN_SIGNER &&
        !layout.valid &&
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 1U,
            RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN,
            signer1_recovery, sizeof(signer1_recovery),
            &signer1_recovery_length) == false &&
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 2U,
            RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN,
            signer2_recovery, sizeof(signer2_recovery),
            &signer2_recovery_length) &&
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            signer2_recovery, signer2_recovery_length, &layout) ==
            SIGNED_METADATA_OK &&
        layout.signer_id == 2U,
        "revoked signer denial and active signer-two known-good state");
    trust_failure_drill_zero(&trust_record, sizeof(trust_record));
    trust_failure_drill_require(
        audit_record_pop(&ring, &trust_record) == AUDIT_OK &&
        trust_record.sequence == 0U &&
        trust_record.type == AUDIT_RECORD_TRUST &&
        trust_record.operation == TRUST_REVOCATION_AUDIT_OPERATION_REVOKE &&
        trust_record.result == TRUST_REVOCATION_AUDIT_RESULT_OK &&
        trust_record.pid == 1U &&
        trust_record.detail ==
            (1U | (2U << TRUST_REVOCATION_DETAIL_GENERATION_SHIFT)) &&
        event_ring_count(&ring) == 0U,
        "revocation audit exact-once evidence");
    serial_write("[TRUST-FAILURE-DRILL] revoked signer denied\n");

    trust_failure_drill_require(
        trust_failure_drill_make_evidence(&evidence, &policy),
        "boot evidence under active policy");
    trust_failure_drill_require(
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_UPDATE, 2U,
            TRUST_FAILURE_UPDATE_METADATA,
            TRUST_FAILURE_UPDATE_METADATA_LEN,
            update, sizeof(update), &update_length) &&
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 2U,
            RECOVERY_TRUST_ENTER_METADATA,
            RECOVERY_TRUST_ENTER_METADATA_LEN,
            signer2_recovery, sizeof(signer2_recovery),
            &signer2_recovery_length) &&
        trust_failure_drill_seal(
            &policy, KEY_USE_PURPOSE_RECOVERY, 2U,
            RECOVERY_TRUST_EXIT_METADATA,
            RECOVERY_TRUST_EXIT_METADATA_LEN,
            exit_envelope, sizeof(exit_envelope), &exit_length),
        "recovery and update envelope publication");
    trust_failure_drill_zero(&state, sizeof(state));
    trust_failure_drill_require(
        recovery_trust_init(&state) == RECOVERY_TRUST_OK,
        "recovery state init");
    original = state;
    trust_failure_drill_require(
        recovery_trust_enter(
            &state, &policy, &evidence,
            update, update_length, &layout) ==
            RECOVERY_TRUST_METADATA_DENIED &&
        trust_failure_drill_equal(
            &state, &original, sizeof(state)) &&
        signed_metadata_verify(
            &policy, KEY_USE_PURPOSE_RECOVERY,
            update, update_length, &layout) ==
            SIGNED_METADATA_PURPOSE_DENIED &&
        verification_policy_active(&policy)->generation == 2U &&
        !verification_policy_has_pending(&policy),
        "recovery-purpose mismatch denial");
    trust_failure_drill_require(
        recovery_trust_enter(
            &state, &policy, &evidence,
            signer2_recovery, signer2_recovery_length, &layout) ==
            RECOVERY_TRUST_OK &&
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
        "recovery-mode action gating");
    trust_failure_drill_require(
        recovery_trust_exit(
            &state, &policy,
            exit_envelope, exit_length, &layout) ==
            RECOVERY_TRUST_OK &&
        !recovery_trust_is_recovery(&state) &&
        recovery_trust_authorize(
            &state, RECOVERY_TRUST_ACTION_CONTROL) ==
            RECOVERY_TRUST_OK,
        "normal control after verified recovery exit");
    serial_write("[TRUST-FAILURE-DRILL] recovery purpose denied\n");
    serial_write(
        "[TRUST-FAILURE-DRILL] known-good trust state preserved\n");
    serial_write("[TRUST-FAILURE-DRILL] proof OK\n");
}

#else
typedef int trust_failure_drill_qemu_translation_unit_guard;
#endif
