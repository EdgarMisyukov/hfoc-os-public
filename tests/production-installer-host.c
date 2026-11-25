#include "production_installer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PRODUCTION_INSTALLER_HOST_TEST

static const unsigned char boot_payload[16U] = {
    0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
    0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU
};
static const unsigned char limine_config[] =
    "timeout: 3\n";
static const unsigned char kernel_payload[32U] = {
    0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
    0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU,
    0x20U, 0x21U, 0x22U, 0x23U, 0x24U, 0x25U, 0x26U, 0x27U,
    0x28U, 0x29U, 0x2AU, 0x2BU, 0x2CU, 0x2DU, 0x2EU, 0x2FU
};
static const char disk_identity[] = "test-nvme-disk";
static int failures;

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 16384U

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static size_t fixture_write_calls;
static size_t fixture_fail_write_call;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct production_installer_source make_source(void)
{
    struct production_installer_source source;

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = boot_payload;
    source.boot_payload_size = sizeof(boot_payload);
    source.limine_config = limine_config;
    source.limine_config_size = sizeof(limine_config) - 1U;
    source.kernel = kernel_payload;
    source.kernel_size = sizeof(kernel_payload);
    return source;
}

static void fixture_report(struct installer_discovery_report *report)
{
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 2U;
    report->ram_present = true;
    report->ram_bytes = 4294967296ULL;
    report->disks[0].present = true;
    report->disks[0].id = 1U;
    report->disks[0].controller = INSTALLER_DISK_CONTROLLER_NVME;
    report->disks[0].partition_table = INSTALLER_PARTITION_TABLE_GPT;
    report->disks[0].block_size = 512U;
    report->disks[0].block_count = 16384U;
    memcpy(report->disks[0].identity, disk_identity,
           sizeof(disk_identity));
}

#define HARDENING_MEDIA_LENGTH 64U
#define HARDENING_SIGNATURE_LENGTH 32U
#define HARDENING_RELEASE_ID 5U
#define HARDENING_SIGNER_ID 1U

static unsigned char hardening_media[HARDENING_MEDIA_LENGTH];
static unsigned char hardening_envelope[256U];
static size_t hardening_envelope_length;
static struct verification_policy hardening_policy;

static enum installer_media_signature_status hardening_signature_verify(
    void *context,
    enum trust_anchor_algorithm algorithm,
    uint32_t signer_id,
    const unsigned char digest[SHA256_DIGEST_SIZE],
    const unsigned char *signature,
    size_t signature_length)
{
    size_t index;

    (void)context;
    if (algorithm != TRUST_ANCHOR_ALGORITHM_ECDSA_P256 ||
        signer_id != HARDENING_SIGNER_ID ||
        signature_length != HARDENING_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    for (index = 0U; index < HARDENING_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != (unsigned char)(
                digest[index % SHA256_DIGEST_SIZE] ^ 0x5AU)) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

static bool fixture_bind_hardening(
    struct production_installer *installer,
    const struct installer_discovery_report *report)
{
    struct verification_policy_snapshot snapshot;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_media_source media_source;
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    unsigned char signature[HARDENING_SIGNATURE_LENGTH];
    uint64_t source_length = 0U;
    size_t index;

    for (index = 0U; index < sizeof(hardening_media); ++index) {
        hardening_media[index] = (unsigned char)(index * 3U + 1U);
    }
    if (sha256_compute(hardening_media, sizeof(hardening_media),
                       digest) != SHA256_OK) {
        return false;
    }
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = boot_payload;
    media_source.boot_payload_size = sizeof(boot_payload);
    media_source.limine_config = limine_config;
    media_source.limine_config_size = sizeof(limine_config) - 1U;
    media_source.kernel = kernel_payload;
    media_source.kernel_size = sizeof(kernel_payload);
    if (installer_media_source_digest(
            &media_source, source_digest, &source_length) !=
        INSTALLER_MEDIA_OK) {
        return false;
    }
    for (index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (unsigned char)(
            digest[index % SHA256_DIGEST_SIZE] ^ 0x5AU);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = HARDENING_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(&hardening_policy, 0, sizeof(hardening_policy));
    if (verification_policy_init(&hardening_policy, &snapshot) !=
        VERIFICATION_POLICY_OK) {
        return false;
    }
    hardening_envelope_length = 0U;
    if (installer_media_manifest_seal(
            &hardening_policy, HARDENING_RELEASE_ID,
            HARDENING_SIGNER_ID, TRUST_ANCHOR_ALGORITHM_ECDSA_P256,
            digest, sizeof(hardening_media), source_digest,
            source_length, signature, sizeof(signature),
            hardening_envelope, sizeof(hardening_envelope),
            &hardening_envelope_length) != INSTALLER_MEDIA_OK) {
        return false;
    }
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = hardening_signature_verify;
    if (production_installer_verify_media(
            installer, &hardening_policy, &verifier,
            HARDENING_RELEASE_ID, hardening_envelope,
            hardening_envelope_length, hardening_media,
            sizeof(hardening_media)) != PRODUCTION_INSTALLER_OK) {
        return false;
    }
    if (installer_supported_hardware_policy_default(&hardware_policy) !=
        INSTALLER_SUPPORTED_HARDWARE_OK) {
        return false;
    }
    return production_installer_verify_hardware(
               installer, report, &hardware_policy) ==
           PRODUCTION_INSTALLER_OK;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++fixture_write_calls;
    if (fixture_fail_write_call != 0U &&
        fixture_write_calls == fixture_fail_write_call) {
        fixture_fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void fixture_reset_storage(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_write_calls = 0U;
    fixture_fail_write_call = 0U;
}

static void fixture_install_report(
    struct installer_discovery_report *report)
{
    fixture_report(report);
    report->disks[0].block_count = FIXTURE_BLOCK_COUNT;
}

static void fixture_install_requests(
    struct installer_layout_partition requests[4U])
{
    memset(requests, 0, 4U * sizeof(requests[0U]));
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].length_lba = 2048U;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].length_lba = 2048U;
    requests[2U].present = true;
    requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    requests[2U].length_lba = 2048U;
    requests[3U].present = true;
    requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    requests[3U].length_lba = 2048U;
}

static bool fixture_prepare_installer(
    struct production_installer *installer,
    struct installer_discovery_report *report)
{
    struct production_installer_source source = make_source();

    fixture_install_report(report);
    return production_installer_init(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_prepare(
               installer, report,
               PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
               disk_identity, sizeof(disk_identity) - 1U,
               &source, true) ==
               PRODUCTION_INSTALLER_OK &&
           fixture_bind_hardening(installer, report) &&
           production_installer_build_plan(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_publish_preview(installer) ==
               PRODUCTION_INSTALLER_OK &&
           production_installer_confirm(installer) ==
               PRODUCTION_INSTALLER_OK;
}

static void test_init_and_prepare_rejections(void)
{
    struct production_installer installer;
    struct production_installer stale;
    struct installer_discovery_report report;
    struct production_installer_source source = make_source();
    struct production_installer_source invalid;

    fixture_report(&report);
    memset(&installer, 0, sizeof(installer));
    memset(&stale, 0, sizeof(stale));
    CHECK(production_installer_init(NULL) ==
              PRODUCTION_INSTALLER_NULL,
          "init rejects null installer");
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              installer.version == PRODUCTION_INSTALLER_VERSION &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_UNPREPARED,
          "init publishes unprepared state");
    CHECK(production_installer_prepare(
              &stale, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "prepare rejects stale uninitialized controller");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_UNKNOWN, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_INVALID_ARGUMENT,
          "prepare rejects unknown mode");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 0U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_INVALID_ARGUMENT,
          "prepare rejects zero disk id");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              "bad\nidentity", 11U,
              &source, true) ==
              PRODUCTION_INSTALLER_INVALID_ARGUMENT,
          "prepare rejects non-printable identity");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              "over-long-identity-over-32-bytes", 32U,
              &source, true) ==
              PRODUCTION_INSTALLER_INVALID_ARGUMENT,
          "prepare rejects over-capacity identity");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              NULL, true) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "prepare rejects missing source");
    invalid = source;
    invalid.boot_payload = NULL;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &invalid, true) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "prepare rejects incomplete source");
    invalid = source;
    invalid.boot_payload_size =
        PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX + 1U;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &invalid, true) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "prepare rejects oversized boot payload");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, false) ==
              PRODUCTION_INSTALLER_EFI_UNAVAILABLE,
          "prepare rejects missing EFI environment");

    report.read_only = false;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects writable discovery evidence");
    fixture_report(&report);
    report.version = 0U;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects stale discovery report");
    fixture_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_UNKNOWN;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects unknown firmware evidence");
    fixture_report(&report);
    report.disks[0].block_size = 4096U;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects unsupported disk block size");
    fixture_report(&report);
    report.disks[0].block_count =
        BLOCK_DEVICE_BLOCK_COUNT_MAX + 1U;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects oversized discovery disk");
    fixture_report(&report);
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              "test-other-dis", 14U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects identity that does not match selected disk");
    fixture_report(&report);
    report.disks[0].id = 2U;
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_UNSAFE_EVIDENCE,
          "prepare rejects missing selected disk");
}

static void test_stage_ordering(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct production_installer_source source = make_source();

    fixture_report(&report);
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_OK &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED &&
              installer.source_ready &&
              installer.efi_ready,
          "prepare accepts validated UEFI evidence");
    CHECK(fixture_bind_hardening(&installer, &report),
          "hardening evidence binds before the plan");
    CHECK(production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "prepare rejects already-prepared controller");
    /*
     * ITEM-517: the same validated evidence prepares the legacy-BIOS chain,
     * so the flow can install on a machine without UEFI.
     */
    {
        struct production_installer bios_installer;
        struct installer_discovery_report bios_report;

        fixture_report(&bios_report);
        bios_report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
        CHECK(production_installer_init(&bios_installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_prepare(
                  &bios_installer, &bios_report,
                  PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
                  disk_identity, sizeof(disk_identity) - 1U,
                  &source, true) ==
                  PRODUCTION_INSTALLER_OK &&
              bios_installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_EVIDENCE_VALIDATED,
              "prepare accepts validated legacy-BIOS evidence");
    }
    CHECK(production_installer_confirm(&installer) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "confirm rejects evidence before preview");
    CHECK(production_installer_publish_preview(&installer) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "preview rejects evidence before plan");
    CHECK(production_installer_build_plan(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_PLAN_BUILT,
          "build plan advances to plan-built");
    CHECK(production_installer_build_plan(&installer) ==
              PRODUCTION_INSTALLER_BAD_STATE,
          "build plan rejects repeated call");
    CHECK(production_installer_publish_preview(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              strstr(installer.preview, "mode=clean-whole-disk") != NULL &&
              strstr(installer.preview, "disk=1") != NULL &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED,
          "preview publishes deterministic plan text");
    CHECK(production_installer_confirm(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              installer.confirmed &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_CONFIRMED,
          "confirmed plan advances after preview");
    CHECK(production_installer_confirm(&installer) ==
              PRODUCTION_INSTALLER_ALREADY_CONFIRMED,
          "confirm rejects repeated confirmation");
}

static void test_layout_execute_verify(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct block_device device;

    fixture_reset_storage();
    fixture_install_report(&report);
    fixture_install_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "whole-disk layout fixture confirms");
    CHECK(fixture_prepare_installer(&installer, &report),
          "production installer reaches confirmed stage");
    memset(&snapshot, 0, sizeof(snapshot));
    device = fixture_device();
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_OK &&
              plan.executed &&
              fixture_write_calls != 0U &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_EXECUTING,
          "confirmed layout executes through controller");
    /*
     * ITEM-502: verification is refused until the run has erased the state the
     * layout will carry; this controller-level fixture has no state volume, so
     * it states the pre-condition the erase step sets.
     */
    CHECK(production_installer_verify_layout(
              &installer, &device, &plan) ==
              PRODUCTION_INSTALLER_BAD_STATE &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_EXECUTING,
          "controller refuses verification before the state erase");
    installer.state_erased = true;
    CHECK(production_installer_verify_layout(
              &installer, &device, &plan) ==
              PRODUCTION_INSTALLER_OK &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_VERIFIED,
          "controller verifies executed layout after the erase");
}

static void test_layout_unconfirmed_and_invalid(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct block_device device;
    struct production_installer_source source = make_source();

    fixture_reset_storage();
    fixture_install_report(&report);
    fixture_install_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "invalid-layout fixture plan confirms");
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_OK &&
          fixture_bind_hardening(&installer, &report) &&
          production_installer_build_plan(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_publish_preview(&installer) ==
              PRODUCTION_INSTALLER_OK,
          "invalid-layout controller reaches preview only");
    memset(&snapshot, 0, sizeof(snapshot));
    device = fixture_device();
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_BAD_STATE &&
              fixture_write_calls == 0U,
          "unconfirmed controller performs no layout writes");

    fixture_reset_storage();
    fixture_prepare_installer(&installer, &report);
    plan.identity[0] = 'X';
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_INVALID_ARGUMENT &&
              fixture_write_calls == 0U,
          "controller rejects layout identity mismatch");
}

static void test_layout_rollback_failure(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct block_device device;

    fixture_reset_storage();
    fixture_install_report(&report);
    fixture_install_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "rollback fixture plan confirms");
    CHECK(fixture_prepare_installer(&installer, &report),
          "rollback fixture reaches confirmed stage");
    device = fixture_device();
    memset(&snapshot, 0, sizeof(snapshot));
    fixture_fail_write_call = 2U;
    CHECK(production_installer_execute_layout(
              &installer, &device, &plan, &snapshot) ==
              PRODUCTION_INSTALLER_IO_ERROR &&
              !plan.executed &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_FAILED,
          "controller reports failed layout execution");
    CHECK(memcmp(fixture_storage, "\0\0\0\0", 4U) == 0 &&
              fixture_write_calls > 5U,
          "failed layout restores original sectors through rollback");
}

static void test_reset_and_strings(void)
{
    struct production_installer installer;
    struct production_installer stale;
    struct installer_discovery_report report;
    struct production_installer_source source = make_source();

    fixture_report(&report);
    memset(&stale, 0, sizeof(stale));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_DUAL_BOOT, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_reset(&installer) ==
              PRODUCTION_INSTALLER_OK &&
              installer.stage ==
                  PRODUCTION_INSTALLER_STAGE_UNPREPARED &&
              !installer.confirmed,
          "reset returns controller to unprepared state");
    CHECK(production_installer_reset(&stale) ==
              PRODUCTION_INSTALLER_BAD_VERSION,
          "reset rejects stale controller version");
    CHECK(strcmp(production_installer_stage_string(
                     PRODUCTION_INSTALLER_STAGE_CONFIRMED),
                 "PRODUCTION_INSTALLER_STAGE_CONFIRMED") == 0,
          "confirmed stage string is exact");
    CHECK(strcmp(production_installer_mode_string(
                     PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL),
                 "PRODUCTION_INSTALLER_MODE_CLEAN_MANUAL") == 0,
          "manual mode string is exact");
    CHECK(strcmp(production_installer_status_string(
                     PRODUCTION_INSTALLER_UNSAFE_EVIDENCE),
                 "PRODUCTION_INSTALLER_UNSAFE_EVIDENCE") == 0,
          "unsafe evidence status string is exact");
    CHECK(production_installer_stage_string(
              (enum production_installer_stage)99U) == NULL &&
          production_installer_mode_string(
              (enum production_installer_mode)99U) == NULL &&
          production_installer_status_string(
              (enum production_installer_status)99U) == NULL,
          "undefined strings are rejected");
}

/*
 * ITEM-517: the controller picks the whole-disk planner by the firmware mode.
 * A legacy-BIOS install plans the BIOS layout (BIOS boot + FAT boot volume +
 * three HFOC roles, no ESP) and never fails for a missing ESP; the preserving
 * planner stays UEFI-only and a BIOS dual-boot install is refused by name.
 */
static void test_bios_plan_and_preserve_refusal(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_dual_boot_plan dual_boot_plan;
    struct production_installer_source source = make_source();
    size_t index;
    bool esp_seen = false;

    fixture_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    report.disks[0].block_count =
        INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA;
    memset(&plan, 0, sizeof(plan));
    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) == PRODUCTION_INSTALLER_OK &&
          fixture_bind_hardening(&installer, &report) &&
          production_installer_build_plan(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_plan_layout(
              &installer, &report, &plan, &dual_boot_plan) ==
              PRODUCTION_INSTALLER_OK,
          "a legacy-BIOS whole-disk install plans without an ESP");
    CHECK(installer.layout.mode ==
              INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS &&
          installer.layout.minimum_media_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA &&
          installer.layout.role_count ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT &&
          plan.partition_count ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT &&
          plan.firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY,
          "the controller publishes the BIOS five-role layout");
    for (index = 0U; index < plan.partition_count; ++index) {
        if (plan.partitions[index].role ==
            INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            esp_seen = true;
        }
    }
    CHECK(!esp_seen, "the BIOS plan requires no EFI System Partition");

    /* The same machine asked to preserve another system is refused by name. */
    fixture_report(&report);
    report.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    memset(&plan, 0, sizeof(plan));
    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    CHECK(production_installer_init(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              &installer, &report,
              PRODUCTION_INSTALLER_MODE_DUAL_BOOT, 1U,
              disk_identity, sizeof(disk_identity) - 1U,
              &source, true) == PRODUCTION_INSTALLER_OK &&
          fixture_bind_hardening(&installer, &report) &&
          production_installer_build_plan(&installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_plan_layout(
              &installer, &report, &plan, &dual_boot_plan) ==
              PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED &&
          plan.partition_count == 0U && !installer.layout_ready,
          "a legacy-BIOS preserve install is refused by name");
    CHECK(strcmp(production_installer_status_string(
                     PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED),
                 "PRODUCTION_INSTALLER_BIOS_PRESERVE_UNSUPPORTED") == 0,
          "the BIOS preserve refusal has an exact status string");
}

int main(void)
{
    test_init_and_prepare_rejections();
    test_stage_ordering();
    test_layout_execute_verify();
    test_layout_unconfirmed_and_invalid();
    test_layout_rollback_failure();
    test_bios_plan_and_preserve_refusal();
    test_reset_and_strings();

    if (failures != 0) {
        fprintf(stderr,
                "Production installer host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Production installer host tests: ALL OK");
    return 0;
}

#endif
