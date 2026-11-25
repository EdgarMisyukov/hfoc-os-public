#ifdef HFOC_BOOT_POLICY_QEMU_TEST

#include "boot_policy.h"
#include "image_integrity.h"
#include "recovery_image.h"
#include "serial.h"
#include "system_image.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BOOT_POLICY_TEST_NORMAL_IMAGE_ID 7U
#define BOOT_POLICY_TEST_RECOVERY_IMAGE_ID 9U
#define BOOT_POLICY_TEST_MISMATCH_IMAGE_ID 8U
#define BOOT_POLICY_TEST_NORMAL_DATA "boot-policy-normal"
#define BOOT_POLICY_TEST_RECOVERY_DATA "boot-policy-recovery"
#define BOOT_POLICY_TEST_TAMPERED_DATA "boot-policy-tampered"
#define BOOT_POLICY_TEST_NORMAL_HASH \
    "57e6ba349f4772657841053450847b2ec2d0038ae50b8cbc1d9c5c1c0c928696"
#define BOOT_POLICY_TEST_RECOVERY_HASH \
    "ed0aa3b6ea518152140ab5f5d855f1f1f27ca635571e6cdd1460c1e4e425be21"

static void boot_policy_failure(const char *message)
    __attribute__((noreturn));

static void boot_policy_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[BOOT-POLICY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void boot_policy_require(bool condition, const char *message)
{
    if (!condition) {
        boot_policy_failure(message);
    }
}

static size_t boot_policy_str_length(const char *text)
{
    size_t length = 0U;

    while (text[length] != '\0') {
        ++length;
    }
    return length;
}

static void boot_policy_str_copy(char *destination,
                                 const char *source,
                                 size_t capacity)
{
    size_t index;

    for (index = 0U; index + 1U < capacity && source[index] != '\0'; ++index) {
        destination[index] = source[index];
    }
    destination[index] = '\0';
}

static struct boot_policy_record boot_policy_test_record(void)
{
    struct boot_policy_record record;

    record.image_ids[BOOT_POLICY_MODE_NORMAL] =
        BOOT_POLICY_TEST_NORMAL_IMAGE_ID;
    record.image_ids[BOOT_POLICY_MODE_RECOVERY] =
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID;
    record.image_ids[BOOT_POLICY_MODE_LAST_KNOWN_GOOD] =
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID;
    return record;
}

static bool boot_policy_test_publish(struct boot_policy *policy)
{
    struct boot_policy_record record = boot_policy_test_record();

    return boot_policy_init(policy, &record) == BOOT_POLICY_OK;
}

static struct system_image_manifest boot_policy_test_manifest(
    uint32_t image_id,
    const char *hash,
    uint32_t manifest_version)
{
    struct system_image_manifest manifest;

    memset(&manifest, 0, sizeof(manifest));
    manifest.manifest_version = manifest_version;
    manifest.image_id = image_id;
    manifest.min_kernel_abi = SYSTEM_IMAGE_KERNEL_ABI_MIN;
    manifest.max_kernel_abi = SYSTEM_IMAGE_KERNEL_ABI_MAX;
    boot_policy_str_copy(manifest.arch, SYSTEM_IMAGE_ARCH_VALUE,
                         sizeof(manifest.arch));
    manifest.component_count = 1U;
    boot_policy_str_copy(manifest.components[0].name, "initramfs",
                         sizeof(manifest.components[0].name));
    boot_policy_str_copy(manifest.components[0].version, "1.0",
                         sizeof(manifest.components[0].version));
    boot_policy_str_copy(manifest.components[0].hash, hash,
                         sizeof(manifest.components[0].hash));
    return manifest;
}

static struct image_integrity_payload boot_policy_test_payload(
    const char *data)
{
    struct image_integrity_payload payload;

    payload.name = "initramfs";
    payload.data = data;
    payload.data_length = boot_policy_str_length(data);
    return payload;
}

static struct recovery_image_candidate boot_policy_test_candidate(
    const struct system_image_manifest *manifest,
    const struct image_integrity_payload *payload)
{
    struct recovery_image_candidate candidate;

    candidate.manifest = manifest;
    candidate.payloads = payload;
    candidate.payload_count = 1U;
    return candidate;
}

static void boot_policy_test_normal_path(void)
{
    struct boot_policy policy;
    struct recovery_image_selection selection;
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    struct image_integrity_payload normal_payload;
    struct image_integrity_payload recovery_payload;
    struct recovery_image_candidate normal_candidate;
    struct recovery_image_candidate recovery_candidate;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    boot_policy_require(boot_policy_test_publish(&policy),
                        "normal path policy publish");
    normal_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_NORMAL_IMAGE_ID,
        BOOT_POLICY_TEST_NORMAL_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    recovery_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        BOOT_POLICY_TEST_RECOVERY_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    normal_payload = boot_policy_test_payload(BOOT_POLICY_TEST_NORMAL_DATA);
    recovery_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_RECOVERY_DATA);
    normal_candidate = boot_policy_test_candidate(
        &normal_manifest, &normal_payload);
    recovery_candidate = boot_policy_test_candidate(
        &recovery_manifest, &recovery_payload);
    memset(&selection, 0, sizeof(selection));
    status = recovery_image_select(
        &selection, &policy, &normal_candidate, &recovery_candidate);
    boot_policy_require(
        status == RECOVERY_IMAGE_OK &&
            recovery_image_is_published(&selection) &&
            selection.selected_mode == RECOVERY_IMAGE_SELECTED_NORMAL &&
            selection.selected_image_id == BOOT_POLICY_TEST_NORMAL_IMAGE_ID &&
            recovery_image_manifest(&selection) != NULL &&
            recovery_image_manifest(&selection)->image_id ==
                BOOT_POLICY_TEST_NORMAL_IMAGE_ID,
        "normal image selection");
    serial_write("[BOOT-POLICY] normal selected image=7\n");
    boot_policy_require(
        boot_policy_destroy(&policy) == BOOT_POLICY_OK,
        "normal path policy destroy");
}

static void boot_policy_test_malformed_policy_path(void)
{
    struct boot_policy policy;
    struct boot_policy_record invalid_record;
    struct recovery_image_selection selection;
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    struct image_integrity_payload normal_payload;
    struct image_integrity_payload recovery_payload;
    struct recovery_image_candidate normal_candidate;
    struct recovery_image_candidate recovery_candidate;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    invalid_record = boot_policy_test_record();
    invalid_record.image_ids[BOOT_POLICY_MODE_NORMAL] = 0U;
    boot_policy_require(
        boot_policy_init(&policy, &invalid_record) ==
            BOOT_POLICY_INVALID_RECORD &&
            !policy.published,
        "malformed policy rejection");

    normal_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_NORMAL_IMAGE_ID,
        BOOT_POLICY_TEST_NORMAL_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    recovery_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        BOOT_POLICY_TEST_RECOVERY_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    normal_payload = boot_policy_test_payload(BOOT_POLICY_TEST_NORMAL_DATA);
    recovery_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_RECOVERY_DATA);
    normal_candidate = boot_policy_test_candidate(
        &normal_manifest, &normal_payload);
    recovery_candidate = boot_policy_test_candidate(
        &recovery_manifest, &recovery_payload);
    memset(&selection, 0, sizeof(selection));
    status = recovery_image_select(
        &selection, &policy, &normal_candidate, &recovery_candidate);
    boot_policy_require(
        status == RECOVERY_IMAGE_POLICY_UNAVAILABLE &&
            !recovery_image_is_published(&selection) &&
            selection.selected_image_id == 0U,
        "malformed policy no publication");
    serial_write("[BOOT-POLICY] malformed policy rejected image=0\n");
}

static void boot_policy_test_malformed_manifest_path(void)
{
    struct boot_policy policy;
    struct recovery_image_selection selection;
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    struct image_integrity_payload normal_payload;
    struct image_integrity_payload recovery_payload;
    struct recovery_image_candidate normal_candidate;
    struct recovery_image_candidate recovery_candidate;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    boot_policy_require(boot_policy_test_publish(&policy),
                        "malformed manifest path policy publish");
    normal_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_NORMAL_IMAGE_ID,
        BOOT_POLICY_TEST_NORMAL_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION + 1U);
    recovery_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        BOOT_POLICY_TEST_RECOVERY_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    normal_payload = boot_policy_test_payload(BOOT_POLICY_TEST_NORMAL_DATA);
    recovery_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_RECOVERY_DATA);
    normal_candidate = boot_policy_test_candidate(
        &normal_manifest, &normal_payload);
    recovery_candidate = boot_policy_test_candidate(
        &recovery_manifest, &recovery_payload);
    memset(&selection, 0, sizeof(selection));
    status = recovery_image_select(
        &selection, &policy, &normal_candidate, &recovery_candidate);
    boot_policy_require(
        status == RECOVERY_IMAGE_OK &&
            recovery_image_is_published(&selection) &&
            selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
            selection.selected_image_id == BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        "malformed manifest recovery selection");
    serial_write(
        "[BOOT-POLICY] malformed manifest selected recovery image=9\n");
    boot_policy_require(
        boot_policy_destroy(&policy) == BOOT_POLICY_OK,
        "malformed manifest path policy destroy");
}

static void boot_policy_test_mismatch_path(void)
{
    struct boot_policy policy;
    struct recovery_image_selection selection;
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    struct image_integrity_payload normal_payload;
    struct image_integrity_payload recovery_payload;
    struct recovery_image_candidate normal_candidate;
    struct recovery_image_candidate recovery_candidate;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    boot_policy_require(boot_policy_test_publish(&policy),
                        "mismatch path policy publish");
    normal_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_MISMATCH_IMAGE_ID,
        BOOT_POLICY_TEST_NORMAL_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    recovery_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        BOOT_POLICY_TEST_RECOVERY_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    normal_payload = boot_policy_test_payload(BOOT_POLICY_TEST_NORMAL_DATA);
    recovery_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_RECOVERY_DATA);
    normal_candidate = boot_policy_test_candidate(
        &normal_manifest, &normal_payload);
    recovery_candidate = boot_policy_test_candidate(
        &recovery_manifest, &recovery_payload);
    memset(&selection, 0, sizeof(selection));
    status = recovery_image_select(
        &selection, &policy, &normal_candidate, &recovery_candidate);
    boot_policy_require(
        status == RECOVERY_IMAGE_OK &&
            recovery_image_is_published(&selection) &&
            selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
            selection.selected_image_id == BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        "mismatch recovery selection");
    serial_write("[BOOT-POLICY] mismatch selected recovery image=9\n");
    boot_policy_require(
        boot_policy_destroy(&policy) == BOOT_POLICY_OK,
        "mismatch path policy destroy");
}

static void boot_policy_test_recovery_path(void)
{
    struct boot_policy policy;
    struct recovery_image_selection selection;
    struct system_image_manifest normal_manifest;
    struct system_image_manifest recovery_manifest;
    struct image_integrity_payload normal_payload;
    struct image_integrity_payload recovery_payload;
    struct recovery_image_candidate normal_candidate;
    struct recovery_image_candidate recovery_candidate;
    enum recovery_image_status status;

    memset(&policy, 0, sizeof(policy));
    boot_policy_require(boot_policy_test_publish(&policy),
                        "recovery path policy publish");
    normal_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_NORMAL_IMAGE_ID,
        BOOT_POLICY_TEST_NORMAL_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    recovery_manifest = boot_policy_test_manifest(
        BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        BOOT_POLICY_TEST_RECOVERY_HASH,
        SYSTEM_IMAGE_MANIFEST_VERSION);
    normal_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_TAMPERED_DATA);
    recovery_payload =
        boot_policy_test_payload(BOOT_POLICY_TEST_RECOVERY_DATA);
    normal_candidate = boot_policy_test_candidate(
        &normal_manifest, &normal_payload);
    recovery_candidate = boot_policy_test_candidate(
        &recovery_manifest, &recovery_payload);
    memset(&selection, 0, sizeof(selection));
    status = recovery_image_select(
        &selection, &policy, &normal_candidate, &recovery_candidate);
    boot_policy_require(
        status == RECOVERY_IMAGE_OK &&
            recovery_image_is_published(&selection) &&
            selection.selected_mode == RECOVERY_IMAGE_SELECTED_RECOVERY &&
            selection.selected_image_id == BOOT_POLICY_TEST_RECOVERY_IMAGE_ID,
        "hash mismatch recovery selection");
    serial_write("[BOOT-POLICY] recovery selected image=9\n");
    boot_policy_require(
        boot_policy_destroy(&policy) == BOOT_POLICY_OK,
        "recovery path policy destroy");
}

static void boot_policy_test_clean_shutdown_path(void)
{
    struct boot_policy policy;

    memset(&policy, 0, sizeof(policy));
    boot_policy_require(boot_policy_test_publish(&policy),
                        "clean shutdown path policy publish");
    boot_policy_require(
        boot_policy_destroy(&policy) == BOOT_POLICY_OK &&
            !policy.published,
        "clean shutdown policy destroy");
    serial_write("[BOOT-POLICY] clean shutdown\n");
}

void boot_policy_qemu_test_init(void)
{
    serial_write("[BOOT-POLICY] proof init\n");
    boot_policy_test_normal_path();
    boot_policy_test_malformed_policy_path();
    boot_policy_test_malformed_manifest_path();
    boot_policy_test_mismatch_path();
    boot_policy_test_recovery_path();
    boot_policy_test_clean_shutdown_path();
    serial_write("[BOOT-POLICY] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_boot_policy_qemu_translation_unit_marker;
#endif
