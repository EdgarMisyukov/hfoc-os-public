#include "production_installer.h"
#include "production_installer_bios_boot.h"

#include "appliance_state_layout.h"
#include "efi_fat32_esp.h"
#include "installer_gpt_preserve.h"
#include "installer_layout_contract.h"
#include "installer_supported_hardware.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PRODUCTION_INSTALLER_BIOS_BOOT_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_MEDIA_LBA UINT64_C(400000)
#define FIXTURE_SECTOR_SLOTS 8192U
#define FIXTURE_KERNEL_BYTES (512U * 1024U)
#define FIXTURE_PAYLOAD_BYTES 1024U
#define FIXTURE_LOADER_BYTES 4096U
#define FIXTURE_MANIFEST_BYTES 288U
#define FIXTURE_SIGNATURE_LENGTH 32U
#define FIXTURE_RELEASE_ID 7U
#define FIXTURE_SIGNER_ID 1U
#define FIXTURE_RECORD_CAPACITY 8192U
#define FIXTURE_CONFIG_CAPACITY 65536U
#define FIXTURE_STAGE2_READ_BYTES 22016U
#define FIXTURE_MENU_TIMEOUT 5U

struct fixture_sector {
    bool used;
    uint64_t lba;
    unsigned char data[FIXTURE_BLOCK_SIZE];
};

static struct fixture_sector fixture_sectors[FIXTURE_SECTOR_SLOTS];
static size_t fixture_sector_count;
static uint64_t fixture_write_calls;
static char fixture_serial[FIXTURE_RECORD_CAPACITY];
static size_t fixture_serial_length;
static char fixture_console[FIXTURE_RECORD_CAPACITY];
static size_t fixture_console_length;
static int failures;

static const char fixture_config_text[] =
    "timeout: 3\n"
    "\n"
    "/HFOC OS 0.2\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-kernel.elf\n"
    "module_path: boot():/boot/hfoc-boot.man\n"
    "module_path: boot():/boot/limine/limine.conf\n";

static const char fixture_menu_text[] =
    "timeout: 5\n"
    "\n"
    "/HFOC OS\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-kernel.elf\n"
    "module_path: boot():/boot/hfoc-boot.man\n"
    "module_path: boot():/boot/hfoc/limine.conf\n";

static unsigned char fixture_kernel[FIXTURE_KERNEL_BYTES];
static unsigned char fixture_payload[FIXTURE_PAYLOAD_BYTES];
static unsigned char fixture_loader[FIXTURE_LOADER_BYTES];
static unsigned char fixture_manifest[FIXTURE_MANIFEST_BYTES];
static unsigned char fixture_media[64U];
static unsigned char fixture_envelope[256U];
static size_t fixture_envelope_length;
static struct verification_policy fixture_policy;

static const char fixture_disk_identity[] = "bios-boot-disk";

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* The boot-entry record is published through the serial seam. */
void serial_write(const char *text)
{
    while (text != NULL && *text != '\0' &&
           fixture_serial_length + 1U < sizeof(fixture_serial)) {
        fixture_serial[fixture_serial_length++] = *text++;
    }
    fixture_serial[fixture_serial_length] = '\0';
}

uint64_t serial_sequence_begin(void)
{
    return 0U;
}

void serial_sequence_end(uint64_t flags)
{
    (void)flags;
}

static void fixture_console_capture(const char *record, size_t length)
{
    size_t index;

    if (record == NULL) {
        return;
    }
    for (index = 0U; index < length; ++index) {
        if (record[index] == '\n' || record[index] == '\r') {
            break;
        }
        if (fixture_console_length + 1U >= sizeof(fixture_console)) {
            break;
        }
        fixture_console[fixture_console_length++] = record[index];
    }
    if (fixture_console_length + 1U < sizeof(fixture_console)) {
        fixture_console[fixture_console_length++] = '\n';
    }
    fixture_console[fixture_console_length] = '\0';
}

void console_publish_record(const char *record, size_t length)
{
    fixture_console_capture(record, length);
}

void console_publish_journal(const char *record, size_t length)
{
    fixture_console_capture(record, length);
}

static struct fixture_sector *fixture_sector_find(uint64_t lba)
{
    size_t index;

    for (index = 0U; index < fixture_sector_count; ++index) {
        if (fixture_sectors[index].used &&
            fixture_sectors[index].lba == lba) {
            return &fixture_sectors[index];
        }
    }
    return NULL;
}

static const struct fixture_sector *fixture_sector_find_in(
    const struct fixture_sector *sectors,
    size_t count,
    uint64_t lba)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        if (sectors[index].used && sectors[index].lba == lba) {
            return &sectors[index];
        }
    }
    return NULL;
}

static struct fixture_sector *fixture_sector_allocate(uint64_t lba)
{
    struct fixture_sector *sector = fixture_sector_find(lba);

    if (sector != NULL) {
        return sector;
    }
    if (fixture_sector_count >= FIXTURE_SECTOR_SLOTS) {
        return NULL;
    }
    sector = &fixture_sectors[fixture_sector_count++];
    memset(sector, 0, sizeof(*sector));
    sector->used = true;
    sector->lba = lba;
    return sector;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    uint64_t lba;
    uint64_t count;
    uint64_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_BLOCK_SIZE != 0U ||
        length % FIXTURE_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    lba = offset / FIXTURE_BLOCK_SIZE;
    count = (uint64_t)(length / FIXTURE_BLOCK_SIZE);
    if (lba >= FIXTURE_MEDIA_LBA ||
        count > FIXTURE_MEDIA_LBA - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < count; ++index) {
        const struct fixture_sector *sector =
            fixture_sector_find(lba + index);

        if (sector == NULL) {
            memset(bytes + (size_t)index * FIXTURE_BLOCK_SIZE, 0,
                   FIXTURE_BLOCK_SIZE);
        } else {
            memcpy(bytes + (size_t)index * FIXTURE_BLOCK_SIZE,
                   sector->data, FIXTURE_BLOCK_SIZE);
        }
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    uint64_t lba;
    uint64_t count;
    uint64_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_BLOCK_SIZE != 0U ||
        length % FIXTURE_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    ++fixture_write_calls;
    lba = offset / FIXTURE_BLOCK_SIZE;
    count = (uint64_t)(length / FIXTURE_BLOCK_SIZE);
    if (lba >= FIXTURE_MEDIA_LBA ||
        count > FIXTURE_MEDIA_LBA - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < count; ++index) {
        struct fixture_sector *sector =
            fixture_sector_allocate(lba + index);

        if (sector == NULL) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        memcpy(sector->data,
               bytes + (size_t)index * FIXTURE_BLOCK_SIZE,
               FIXTURE_BLOCK_SIZE);
    }
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
    device.block_count = FIXTURE_MEDIA_LBA;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void fixture_reset(void)
{
    size_t index;

    memset(fixture_sectors, 0, sizeof(fixture_sectors));
    fixture_sector_count = 0U;
    fixture_write_calls = 0U;
    memset(fixture_serial, 0, sizeof(fixture_serial));
    fixture_serial_length = 0U;
    memset(fixture_console, 0, sizeof(fixture_console));
    fixture_console_length = 0U;
    for (index = 0U; index < sizeof(fixture_kernel); ++index) {
        fixture_kernel[index] = (unsigned char)(
            (index * 17U + (index >> 5U) * 3U) & 0xFFU);
    }
    for (index = 0U; index < sizeof(fixture_payload); ++index) {
        fixture_payload[index] = (unsigned char)(index & 0xFFU);
    }
    for (index = 0U; index < sizeof(fixture_loader); ++index) {
        fixture_loader[index] = (unsigned char)(0x5AU ^ (index & 0xFFU));
    }
    memset(fixture_manifest, 0, sizeof(fixture_manifest));
    memcpy(fixture_manifest, "HFOCBOOT", 8U);
}

static void fixture_report(
    struct installer_discovery_report *report,
    enum installer_firmware_mode firmware_mode)
{
    memset(report, 0, sizeof(*report));
    report->version = INSTALLER_HARDWARE_DISCOVERY_VERSION;
    report->firmware_mode = firmware_mode;
    report->read_only = true;
    report->scan_complete = true;
    report->cpu_present = true;
    report->cpu_cores = 4U;
    report->ram_present = true;
    report->ram_bytes = 4294967296ULL;
    report->disk_count = 1U;
    report->disks[0].present = true;
    report->disks[0].id = 1U;
    report->disks[0].controller = INSTALLER_DISK_CONTROLLER_SATA;
    report->disks[0].partition_table = INSTALLER_PARTITION_TABLE_GPT;
    report->disks[0].block_size = FIXTURE_BLOCK_SIZE;
    report->disks[0].block_count = FIXTURE_MEDIA_LBA;
    memcpy(report->disks[0].identity, fixture_disk_identity,
           sizeof(fixture_disk_identity));
}

static struct production_installer_source fixture_source(void)
{
    struct production_installer_source source;

    memset(&source, 0, sizeof(source));
    source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    source.boot_payload = fixture_payload;
    source.boot_payload_size = sizeof(fixture_payload);
    source.limine_config = (const unsigned char *)fixture_config_text;
    source.limine_config_size = sizeof(fixture_config_text) - 1U;
    source.kernel = fixture_kernel;
    source.kernel_size = sizeof(fixture_kernel);
    return source;
}

static struct production_installer_boot_media fixture_media_view(bool menu)
{
    struct production_installer_boot_media media;

    memset(&media, 0, sizeof(media));
    media.version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    media.payload = fixture_payload;
    media.payload_size = sizeof(fixture_payload);
    media.config = (const unsigned char *)fixture_config_text;
    media.config_size = sizeof(fixture_config_text) - 1U;
    media.kernel = fixture_kernel;
    media.kernel_size = sizeof(fixture_kernel);
    media.loader = fixture_loader;
    media.loader_size = sizeof(fixture_loader);
    media.manifest = fixture_manifest;
    media.manifest_size = sizeof(fixture_manifest);
    if (menu) {
        media.menu = (const unsigned char *)fixture_menu_text;
        media.menu_size = sizeof(fixture_menu_text) - 1U;
        media.menu_entries = 1U;
        media.menu_timeout_seconds = FIXTURE_MENU_TIMEOUT;
    }
    return media;
}

static enum installer_media_signature_status fixture_signature_verify(
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
        signer_id != FIXTURE_SIGNER_ID ||
        signature_length != FIXTURE_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    for (index = 0U; index < FIXTURE_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != (unsigned char)(
                digest[index % SHA256_DIGEST_SIZE] ^ 0x27U)) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

static void fixture_bind_media(
    struct production_installer *installer,
    const struct installer_discovery_report *report)
{
    struct verification_policy_snapshot snapshot;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_media_source media_source;
    struct production_installer_source source = fixture_source();
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    unsigned char signature[FIXTURE_SIGNATURE_LENGTH];
    uint64_t source_length = 0U;
    size_t index;

    for (index = 0U; index < sizeof(fixture_media); ++index) {
        fixture_media[index] = (unsigned char)(index * 5U + 3U);
    }
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = source.boot_payload;
    media_source.boot_payload_size = source.boot_payload_size;
    media_source.limine_config = source.limine_config;
    media_source.limine_config_size = source.limine_config_size;
    media_source.kernel = source.kernel;
    media_source.kernel_size = source.kernel_size;
    if (sha256_compute(fixture_media, sizeof(fixture_media),
                       digest) != SHA256_OK ||
        installer_media_source_digest(
            &media_source, source_digest, &source_length) !=
            INSTALLER_MEDIA_OK) {
        return;
    }
    for (index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (unsigned char)(
            digest[index % SHA256_DIGEST_SIZE] ^ 0x27U);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = FIXTURE_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(&fixture_policy, 0, sizeof(fixture_policy));
    if (verification_policy_init(&fixture_policy, &snapshot) !=
        VERIFICATION_POLICY_OK) {
        return;
    }
    fixture_envelope_length = 0U;
    if (installer_media_manifest_seal(
            &fixture_policy, FIXTURE_RELEASE_ID, FIXTURE_SIGNER_ID,
            TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest,
            sizeof(fixture_media), source_digest, source_length,
            signature, sizeof(signature), fixture_envelope,
            sizeof(fixture_envelope), &fixture_envelope_length) !=
        INSTALLER_MEDIA_OK) {
        return;
    }
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = fixture_signature_verify;
    (void)production_installer_verify_media(
        installer, &fixture_policy, &verifier, FIXTURE_RELEASE_ID,
        fixture_envelope, fixture_envelope_length, fixture_media,
        sizeof(fixture_media));
    (void)installer_supported_hardware_policy_default(&hardware_policy);
    (void)production_installer_verify_hardware(
        installer, report, &hardware_policy);
}

/*
 * The controller runs the production order the install flow runs: evidence,
 * sealed media, hardware, contracted layout, preview, confirmation, then the
 * layout itself.  The report decides which contract planner is used, so a BIOS
 * report plans the five-role BIOS layout and a UEFI report the four-role one.
 */
static void fixture_prepare_executing(
    struct production_installer *installer,
    struct installer_discovery_report *report,
    struct installer_layout_plan *plan,
    struct block_device *device,
    enum installer_firmware_mode firmware_mode)
{
    struct production_installer_source source = fixture_source();
    struct installer_layout_snapshot snapshot;

    fixture_report(report, firmware_mode);
    CHECK(production_installer_init(installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              installer, report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              fixture_disk_identity,
              sizeof(fixture_disk_identity) - 1U, &source,
              firmware_mode != INSTALLER_FIRMWARE_UNKNOWN) ==
              PRODUCTION_INSTALLER_OK,
          "fixture controller prepares evidence");
    fixture_bind_media(installer, report);
    CHECK(installer->media_verified &&
              installer->source_digest_ready &&
              production_installer_build_plan(installer) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_plan_layout(
                  installer, report, plan, NULL) ==
                  PRODUCTION_INSTALLER_OK &&
              production_installer_publish_preview(installer) ==
                  PRODUCTION_INSTALLER_OK &&
              installer_layout_confirm(plan) ==
                  INSTALLER_LAYOUT_OK &&
              production_installer_confirm(installer) ==
                  PRODUCTION_INSTALLER_OK &&
              installer->stage ==
                  PRODUCTION_INSTALLER_STAGE_CONFIRMED,
          "fixture controller plans and confirms the contracted layout");
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(production_installer_execute_layout(
              installer, device, plan, &snapshot) ==
                  PRODUCTION_INSTALLER_OK &&
              installer->stage ==
                  PRODUCTION_INSTALLER_STAGE_EXECUTING,
          "fixture controller executes the contracted layout");
}

/*
 * The flow's own ESP lookup (`production_install_flow_role_partition`), so the
 * host proof can show what the BIOS plan does and does not contain.
 */
static const struct installer_layout_partition *fixture_role_partition(
    const struct installer_layout_plan *plan,
    enum installer_layout_role role)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_LAYOUT_PARTITION_CAPACITY; ++index) {
        if (plan->partitions[index].present &&
            plan->partitions[index].role == role) {
            return &plan->partitions[index];
        }
    }
    return NULL;
}

static struct production_installer_bios_partition fixture_bios_partition(
    const struct installer_layout_plan *plan,
    enum installer_layout_role role)
{
    const struct installer_layout_partition *partition =
        fixture_role_partition(plan, role);
    struct production_installer_bios_partition view;

    memset(&view, 0, sizeof(view));
    if (partition != NULL) {
        view.present = true;
        view.first_lba = partition->first_lba;
        view.length_lba = partition->length_lba;
    }
    return view;
}

static uint64_t fixture_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static uint32_t fixture_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0U] |
           ((uint32_t)bytes[1U] << 8U) |
           ((uint32_t)bytes[2U] << 16U) |
           ((uint32_t)bytes[3U] << 24U);
}

/*
 * The reference has to carry the active flag by the same rule the writer
 * applies: the LBA0 this install leaves behind is the measured stage1 recipe
 * *plus* the repair `limine bios-install` performs on a table with no active
 * entry.  A disk whose firmware does not consult the flag boots either way; a
 * disk whose firmware does consult it is not booted at all without the flag.
 */
/*
 * The register-preservation patch of the pinned Stage1, as the measured recipe:
 * three redirects into the stage's zero padding, where each BIOS call is made
 * with ECX saved and restored, and a tail that performs the carry test the
 * original `jc` at 0x8b made.  The proof builds the expected sector from these
 * bytes, not from the writer, so a change in either place is visible.
 */
static void fixture_apply_ecx_patch(unsigned char *reference)
{
    static const unsigned char trampolines[32] = {
        0xC7U, 0x04U, 0x1EU, 0x00U, 0x66U, 0x51U, 0xCDU, 0x13U,
        0x66U, 0x59U, 0xE9U, 0x48U, 0x00U,
        0xB4U, 0x42U, 0xF8U, 0x66U, 0x51U, 0xCDU, 0x13U, 0x66U,
        0x59U, 0xE9U, 0x79U, 0x00U,
        0x0FU, 0x82U, 0xA4U, 0x00U, 0xE9U, 0xC2U, 0xFFU,
    };
    static const unsigned char read_check[4] = {0xE9U, 0x38U, 0x00U, 0x90U};
    static const unsigned char params[5] = {0xE9U, 0xAEU, 0xFFU, 0x90U, 0x90U};
    static const unsigned char read[5] = {0xE9U, 0x7DU, 0xFFU, 0x90U, 0x90U};

    memcpy(reference + PRODUCTION_INSTALLER_BIOS_ECX_PADDING,
           trampolines, sizeof(trampolines));
    memcpy(reference + PRODUCTION_INSTALLER_BIOS_ECX_READ_CHECK,
           read_check, sizeof(read_check));
    memcpy(reference + PRODUCTION_INSTALLER_BIOS_ECX_PARAMS,
           params, sizeof(params));
    memcpy(reference + PRODUCTION_INSTALLER_BIOS_ECX_READ,
           read, sizeof(read));
}

static void fixture_reference_stage1(
    const struct production_installer_bios_blob *blob,
    const unsigned char original[FIXTURE_BLOCK_SIZE],
    uint64_t first_lba,
    unsigned char reference[FIXTURE_BLOCK_SIZE])
{
    const uint64_t stage2_offset = first_lba * FIXTURE_BLOCK_SIZE;
    bool any_active = false;
    size_t index;

    memcpy(reference, blob->stage,
           PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE);
    for (index = 0U; index < 8U; ++index) {
        reference[PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD + index] =
            (unsigned char)((stage2_offset >> (8U * index)) & 0xFFU);
    }
    memcpy(reference + 218U, original + 218U, 6U);
    memcpy(reference + 440U, original + 440U, 70U);
    for (index = 0U;
         index < PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_COUNT; ++index) {
        any_active =
            any_active ||
            reference[PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET +
                      index * PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_SIZE] ==
                PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG;
    }
    if (!any_active) {
        reference[PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET] =
            PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG;
    }
    fixture_apply_ecx_patch(reference);
}

static void fixture_read_lba(
    struct block_device *device,
    uint64_t lba,
    unsigned char *buffer)
{
    (void)device->read_fn(
        device, lba * FIXTURE_BLOCK_SIZE, buffer, FIXTURE_BLOCK_SIZE);
}

/* What the stage writer is allowed to touch, outside its own two roles. */
static void fixture_assert_write_footprint(
    const struct fixture_sector *before,
    size_t before_count,
    uint64_t bios_boot_first,
    uint64_t boot_volume_first,
    uint64_t boot_volume_length)
{
    size_t index;
    bool outside = false;

    for (index = 0U; index < fixture_sector_count; ++index) {
        const struct fixture_sector *now = &fixture_sectors[index];
        const struct fixture_sector *was;
        bool allowed;

        if (!now->used) {
            continue;
        }
        was = fixture_sector_find_in(before, before_count, now->lba);
        if (was != NULL && was->used &&
            memcmp(was->data, now->data, FIXTURE_BLOCK_SIZE) == 0) {
            continue;
        }
        allowed =
            now->lba == 0U ||
            (now->lba >= bios_boot_first &&
             now->lba < bios_boot_first + 43U) ||
            (now->lba >= boot_volume_first &&
             now->lba < boot_volume_first + boot_volume_length);
        if (!allowed) {
            outside = true;
            fprintf(stderr,
                    "FAIL: the BIOS stage wrote outside its footprint: "
                    "lba=%llu\n",
                    (unsigned long long)now->lba);
        }
    }
    CHECK(!outside,
          "the BIOS stage writes only LBA0, the BIOS boot role and its "
          "volume");
}

/*
 * The gate refuses before the first write: a BIOS boot partition below the
 * measured 64-sector minimum and a boot volume below the payload set's
 * requirement.  Both refusals are named, and neither touches the device.
 */
static void test_gate_refuses_before_first_write(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(true);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_BIOS_LEGACY);
    bios_boot = fixture_bios_partition(&plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
    boot_volume = fixture_bios_partition(
        &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);
    CHECK(bios_boot.present && boot_volume.present,
          "the BIOS clean plan carries the BIOS boot role and the volume");

    fixture_write_calls = 0U;
    {
        struct production_installer_bios_partition small = bios_boot;

        small.length_lba =
            INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS - 1U;
        CHECK(production_installer_bios_boot_requirements(
                  &installer, &small, &boot_volume, &media, &outcome) ==
                  PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL,
              "a BIOS boot partition below 64 sectors is refused by name");
    }
    {
        struct production_installer_bios_partition small = boot_volume;

        small.length_lba = 1024U;
        CHECK(production_installer_bios_boot_requirements(
                  &installer, &bios_boot, &small, &media, &outcome) ==
                  PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL,
              "a boot volume too small for the payload set is refused");
    }
    {
        struct production_installer_bios_partition absent;

        memset(&absent, 0, sizeof(absent));
        CHECK(production_installer_bios_boot_requirements(
                  &installer, &absent, &boot_volume, &media, &outcome) ==
                  PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE,
              "a missing BIOS boot partition is refused");
        CHECK(production_installer_bios_boot_requirements(
                  &installer, &bios_boot, &absent, &media, &outcome) ==
                  PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE,
              "a missing boot volume is refused");
    }
    CHECK(fixture_write_calls == 0U,
          "the pre-write refusals perform no write");
    CHECK(production_installer_bios_boot_requirements(
              &installer, &bios_boot, &boot_volume, &media, &outcome) ==
              PRODUCTION_INSTALLER_OK &&
              outcome.required_sectors <= boot_volume.length_lba,
          "the gate passes the contracted BIOS layout");
}

/*
 * The module is additive to the UEFI boot-stage vocabulary: a UEFI plan can
 * never enter it, and the mechanism name is exact.
 */
static void test_uefi_layout_never_enters_the_bios_path(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(true);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_UEFI);
    CHECK(fixture_role_partition(
              &plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT) == NULL &&
              fixture_role_partition(
                  &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME) == NULL &&
              fixture_role_partition(
                  &plan, INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) != NULL,
          "a UEFI clean plan has an ESP and no BIOS roles");
    /*
     * Even a caller that hands the gate BIOS-shaped extents cannot make a UEFI
     * layout enter the BIOS path: the summary mode is the gate.
     */
    memset(&bios_boot, 0, sizeof(bios_boot));
    bios_boot.present = true;
    bios_boot.first_lba = INSTALLER_LAYOUT_ALIGN_LBA;
    bios_boot.length_lba = INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS;
    boot_volume = bios_boot;
    boot_volume.first_lba = 4096U;
    boot_volume.length_lba = 67584U;
    fixture_write_calls = 0U;
    CHECK(production_installer_bios_boot_requirements(
              &installer, &bios_boot, &boot_volume, &media, &outcome) ==
              PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE,
          "a UEFI layout is refused by the BIOS gate");
    CHECK(production_installer_bios_boot_run(
              &installer, &device, &bios_boot, &boot_volume, &media,
              &outcome) == PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE &&
              fixture_write_calls == 0U,
          "a UEFI layout never reaches a BIOS write");
    CHECK(strcmp(production_installer_boot_mechanism_string(
                     PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR),
                 "BIOS_MBR") == 0,
          "the BIOS mechanism is additive to the boot-stage vocabulary");
}

/*
 * The pinned blob transport: the module installs the exact Limine 12.3.2
 * bytes, and the host proof refuses to proceed if the transport ever carries
 * a different release.
 */
static void test_blob_transport_is_the_pinned_release(void)
{
    static const unsigned char expected_stage_digest[SHA256_DIGEST_SIZE] = {
        0x83U, 0xA2U, 0x05U, 0xABU, 0x84U, 0xC1U, 0x88U, 0x8AU,
        0x67U, 0xC1U, 0xB5U, 0xC1U, 0xA8U, 0xFDU, 0xC6U, 0xBAU,
        0xCEU, 0x9AU, 0x61U, 0xFCU, 0x45U, 0x66U, 0x08U, 0x2EU,
        0x26U, 0x63U, 0x82U, 0x26U, 0x66U, 0x74U, 0xE5U, 0x4AU,
    };
    static const unsigned char expected_limine_digest[SHA256_DIGEST_SIZE] = {
        0x7DU, 0x3AU, 0xD7U, 0x41U, 0x8CU, 0x1AU, 0x95U, 0x4BU,
        0x77U, 0x03U, 0xB8U, 0xBFU, 0x86U, 0x54U, 0xBFU, 0x9FU,
        0x11U, 0x93U, 0xB6U, 0x2FU, 0xE8U, 0x8FU, 0x44U, 0x17U,
        0x79U, 0xD5U, 0xFEU, 0x2BU, 0x06U, 0x3FU, 0xBDU, 0xB1U,
    };
    const struct production_installer_bios_blob blob =
        production_installer_bios_blob();
    unsigned char digest[SHA256_DIGEST_SIZE];

    CHECK(blob.stage != NULL &&
              blob.stage_size == PRODUCTION_INSTALLER_BIOS_STAGE_SIZE &&
              blob.limine_bios_sys != NULL &&
              blob.limine_bios_sys_size ==
                  PRODUCTION_INSTALLER_BIOS_LIMINE_SYS_SIZE,
          "the installer kernel links the pinned Limine blob sizes");
    CHECK(sha256_compute(blob.stage, blob.stage_size, digest) ==
                  SHA256_OK &&
              memcmp(digest, expected_stage_digest,
                     SHA256_DIGEST_SIZE) == 0,
          "the stage blob is the measured Limine 12.3.2 image");
    CHECK(sha256_compute(blob.limine_bios_sys,
                             blob.limine_bios_sys_size, digest) ==
                  SHA256_OK &&
              memcmp(digest, expected_limine_digest,
                     SHA256_DIGEST_SIZE) == 0,
          "limine-bios.sys is the measured Limine 12.3.2 loader");
    CHECK(fixture_load_le64(
              blob.stage + PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD) ==
              0U,
          "the blob's stage2 offset field starts zero, so the patch is a "
          "write");
}

/*
 * The measured recipe, end to end: the BIOS clean plan reaches the BIOS boot
 * stage (not the ESP payload path), the payload set lands on the FAT boot
 * volume, stage1 lands at LBA0 with the protective MBR entry and the GPT
 * intact, stage2 lands at the partition start, and the record says so.
 */
static void test_bios_clean_plan_stages_the_measured_recipe(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(true);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;
    static struct fixture_sector before[FIXTURE_SECTOR_SLOTS];
    size_t before_count = 0U;
    unsigned char original_mbr[FIXTURE_BLOCK_SIZE];
    unsigned char original_gpt[2U][FIXTURE_BLOCK_SIZE];
    unsigned char reference[FIXTURE_BLOCK_SIZE];
    unsigned char read_back[FIXTURE_BLOCK_SIZE];
    unsigned char stage2_tail_before[
        FIXTURE_STAGE2_READ_BYTES - PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE];
    static unsigned char stage2_read[FIXTURE_STAGE2_READ_BYTES];
    static unsigned char kernel_read[FIXTURE_KERNEL_BYTES];
    static unsigned char config_read[FIXTURE_CONFIG_CAPACITY];
    unsigned char manifest_read[FIXTURE_MANIFEST_BYTES];
    unsigned char limine_read[64U];
    size_t read_size = 0U;
    const struct production_installer_bios_blob blob =
        production_installer_bios_blob();

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_BIOS_LEGACY);

    /*
     * The flow's role selection in BIOS mode: the BIOS boot role for stage2,
     * the FAT boot volume for the payload set, and no ESP at all.  The UEFI
     * branch would have failed here with `esp-payload`.
     */
    CHECK(fixture_role_partition(
              &plan, INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) == NULL,
          "the BIOS clean plan has no ESP for the flow to find");
    bios_boot = fixture_bios_partition(&plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
    boot_volume = fixture_bios_partition(
        &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);
    CHECK(bios_boot.present && boot_volume.present &&
              bios_boot.length_lba >=
                  INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS,
          "the flow selects the BIOS boot role and the boot volume");
    /*
     * The old flow path is what a BIOS plan must not take: its ESP lookup is
     * empty here and the UEFI pre-write gate refuses the absent ESP, which is
     * the `esp-payload` refusal.  The BIOS gate below is the path that is
     * reached instead.
     */
    {
        struct production_installer_boot_esp absent_esp;
        struct production_installer_boot_outcome uefi_outcome;

        memset(&absent_esp, 0, sizeof(absent_esp));
        memset(&uefi_outcome, 0, sizeof(uefi_outcome));
        fixture_write_calls = 0U;
        CHECK(production_installer_boot_stage_requirements(
                  &installer, &absent_esp, &media, &uefi_outcome) ==
                  PRODUCTION_INSTALLER_INVALID_ARGUMENT,
              "the UEFI ESP path refuses the BIOS plan (the old esp-payload)");
        CHECK(fixture_write_calls == 0U,
              "the ESP refusal performs no write");
    }
    CHECK(production_installer_bios_boot_requirements(
              &installer, &bios_boot, &boot_volume, &media, &outcome) ==
              PRODUCTION_INSTALLER_OK,
          "the BIOS clean plan reaches the BIOS boot gate");

    fixture_read_lba(&device, 0U, original_mbr);
    fixture_read_lba(&device, 1U, original_gpt[0U]);
    fixture_read_lba(&device, 2U, original_gpt[1U]);
    (void)device.read_fn(
        &device, bios_boot.first_lba * FIXTURE_BLOCK_SIZE, stage2_read,
        FIXTURE_STAGE2_READ_BYTES);
    memcpy(stage2_tail_before,
           stage2_read + PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE,
           sizeof(stage2_tail_before));
    memcpy(before, fixture_sectors, sizeof(before));
    before_count = fixture_sector_count;
    fixture_write_calls = 0U;

    CHECK(production_installer_bios_boot_run(
              &installer, &device, &bios_boot, &boot_volume, &media,
              &outcome) == PRODUCTION_INSTALLER_OK,
          "the BIOS boot path stages and verifies the disk");
    CHECK(outcome.payload_staged && outcome.payload_verified &&
              outcome.stage1_written && outcome.stage2_written &&
              outcome.stage1_verified && outcome.stage2_verified &&
              outcome.config_verified && outcome.gpt_verified &&
              outcome.mechanism ==
                  PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR,
          "the outcome records the staged, written and verified BIOS chain");
    CHECK(outcome.stage_sector_writes ==
              PRODUCTION_INSTALLER_BIOS_STAGE_SECTOR_WRITES,
          "the stage writer changes exactly 44 sectors");
    CHECK(outcome.stage2_offset ==
              bios_boot.first_lba * FIXTURE_BLOCK_SIZE &&
              outcome.stage2_field_offset ==
                  PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD_DECIMAL &&
              outcome.stage1_lba == 0U,
          "the outcome publishes the measured stage geometry");

    /* Stage1: the recipe against a reference built from the blob. */
    fixture_reference_stage1(&blob, original_mbr, bios_boot.first_lba,
                             reference);
    fixture_read_lba(&device, 0U, read_back);
    CHECK(memcmp(read_back, reference, FIXTURE_BLOCK_SIZE) == 0,
          "LBA0 is the measured stage1 recipe byte for byte");
    CHECK(fixture_load_le64(
              read_back + PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD) ==
              bios_boot.first_lba * FIXTURE_BLOCK_SIZE,
          "the 0x1a4 field is the stage2 byte offset in little endian");
    CHECK(memcmp(read_back + 218U, original_mbr + 218U, 6U) == 0 &&
              memcmp(read_back + 440U, original_mbr + 440U, 6U) == 0 &&
              memcmp(read_back + 447U, original_mbr + 447U, 63U) == 0,
          "the saved LBA0 ranges are restored");
    /*
     * The one byte the restore does not keep.  The layout writes a protective
     * MBR with no active entry, and firmware that consults the flag before it
     * runs stage1 - the L875's InsydeH2O - skips such a disk silently, so the
     * installed table is marked active exactly as `limine bios-install` marks
     * an MBR disk.  Everything else in the table, the protective 0xEE entry
     * included, is still the layout's own bytes.
     */
    CHECK(original_mbr[446U] == 0x00U &&
              read_back[446U] ==
                  PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG,
          "the protective entry is marked active for the firmware");
    /*
     * The register-preservation patch: this firmware's AHCI INT 13h handler
     * clobbers ECX, which the stage counts its sectors in, so both BIOS calls
     * are made from a trampoline in the stage's zero padding with ECX saved
     * and restored, and the carry test after the read still runs.
     */
    CHECK(memcmp(read_back + PRODUCTION_INSTALLER_BIOS_ECX_PADDING + 4U,
                 (const unsigned char[]){0x66U, 0x51U, 0xCDU, 0x13U,
                                         0x66U, 0x59U}, 6U) == 0 &&
              memcmp(read_back + PRODUCTION_INSTALLER_BIOS_ECX_PADDING + 16U,
                     (const unsigned char[]){0x66U, 0x51U, 0xCDU, 0x13U,
                                             0x66U, 0x59U}, 6U) == 0,
          "ECX is kept across both BIOS calls of the stage");
    CHECK(read_back[PRODUCTION_INSTALLER_BIOS_ECX_READ_CHECK] == 0xE9U &&
              read_back[PRODUCTION_INSTALLER_BIOS_ECX_PARAMS] == 0xE9U &&
              read_back[PRODUCTION_INSTALLER_BIOS_ECX_READ] == 0xE9U &&
              read_back[PRODUCTION_INSTALLER_BIOS_ECX_PADDING + 25U] ==
                  0x0FU &&
              read_back[PRODUCTION_INSTALLER_BIOS_ECX_PADDING + 26U] ==
                  0x82U,
          "the three call sites are redirected and the carry test survives");
    CHECK(memcmp(read_back + PRODUCTION_INSTALLER_BIOS_ECX_PADDING + 32U,
                 (const unsigned char[13]){0}, 13U) == 0,
          "the trampolines stay inside the stage's padding");
    CHECK(read_back[450U] == 0xEEU &&
              fixture_load_le32(read_back + 454U) == 1U &&
              fixture_load_le32(read_back + 458U) ==
                  (uint32_t)(FIXTURE_MEDIA_LBA - 1U),
          "the protective MBR entry survives the stage1 write");

    /* Stage2: verbatim at the partition start, tail sector preserved. */
    memset(stage2_read, 0, sizeof(stage2_read));
    (void)device.read_fn(
        &device, bios_boot.first_lba * FIXTURE_BLOCK_SIZE, stage2_read,
        FIXTURE_STAGE2_READ_BYTES);
    CHECK(memcmp(stage2_read, blob.stage +
                     PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE,
                 PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE) == 0,
          "stage2 lands at the BIOS boot partition start, byte for byte");
    CHECK(memcmp(stage2_read + PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE,
                 stage2_tail_before, sizeof(stage2_tail_before)) == 0,
          "the partial tail sector keeps the bytes stage2 does not cover");

    /* The GPT is untouched: LBA1/LBA2 byte-identical, CRCs still valid. */
    fixture_read_lba(&device, 1U, read_back);
    CHECK(memcmp(read_back, original_gpt[0U], FIXTURE_BLOCK_SIZE) == 0,
          "the primary GPT header survives the stage1 write");
    fixture_read_lba(&device, 2U, read_back);
    CHECK(memcmp(read_back, original_gpt[1U], FIXTURE_BLOCK_SIZE) == 0,
          "the primary GPT array survives the stage1 write");
    {
        static struct installer_gpt_preserve_table table;

        CHECK(installer_gpt_preserve_read(&device, 1U, &table) ==
                  INSTALLER_GPT_PRESERVE_OK,
              "the primary and backup GPT CRCs still validate");
    }

    /* The payload set is readable from the FAT boot volume. */
    {
        struct efi_fat32_esp esp;

        memset(&esp, 0, sizeof(esp));
        CHECK(efi_fat32_esp_bind_device(
                  &device, boot_volume.first_lba,
                  boot_volume.length_lba, &esp) == EFI_FAT32_ESP_OK,
              "the staged FAT boot volume binds");
        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_CONFIG_PATH,
                  config_read, sizeof(config_read),
                  &read_size) == EFI_FAT32_ESP_OK &&
                  read_size == sizeof(fixture_menu_text) - 1U &&
                  memcmp(config_read, fixture_menu_text,
                         read_size) == 0,
              "the generated menu is the installed configuration");
        read_size = 0U;
        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH,
                  config_read, sizeof(config_read),
                  &read_size) == EFI_FAT32_ESP_OK &&
                  read_size == sizeof(fixture_config_text) - 1U &&
                  memcmp(config_read, fixture_config_text,
                         read_size) == 0,
              "the sealed chain configuration the menu names is staged");
        read_size = 0U;
        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_KERNEL_PATH,
                  kernel_read, sizeof(kernel_read),
                  &read_size) == EFI_FAT32_ESP_OK &&
                  read_size == sizeof(fixture_kernel) &&
                  memcmp(kernel_read, fixture_kernel,
                         sizeof(fixture_kernel)) == 0,
              "the installed kernel is readable on the boot volume");
        read_size = 0U;
        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_MANIFEST_PATH,
                  manifest_read, sizeof(manifest_read),
                  &read_size) == EFI_FAT32_ESP_OK &&
                  read_size == sizeof(fixture_manifest) &&
                  memcmp(manifest_read, fixture_manifest,
                         sizeof(fixture_manifest)) == 0,
              "the sealed boot manifest is readable on the boot volume");
        read_size = 0U;
        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_STAGE2_PATH,
                  limine_read, sizeof(limine_read),
                  &read_size) == EFI_FAT32_ESP_CAPACITY,
              "limine-bios.sys is larger than a short buffer");
        {
            unsigned char digest[SHA256_DIGEST_SIZE];

            read_size = 0U;
            CHECK(efi_fat32_esp_digest_file(
                      &esp, PRODUCTION_INSTALLER_BIOS_STAGE2_PATH,
                      digest, &read_size) == EFI_FAT32_ESP_OK &&
                      read_size == blob.limine_bios_sys_size &&
                      memcmp(digest, outcome.limine_bios_sys_digest,
                             SHA256_DIGEST_SIZE) == 0,
                  "limine-bios.sys is readable with the pinned length");
        }
    }

    fixture_assert_write_footprint(
        before, before_count, bios_boot.first_lba,
        boot_volume.first_lba, boot_volume.length_lba);

    /* The record the acceptance run reads. */
    CHECK(strstr(fixture_serial,
                 "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
                 "mechanism=BIOS_MBR ") != NULL,
          "the BIOS boot-entry record names mechanism=BIOS_MBR");
    CHECK(strstr(fixture_serial,
                 "path=\\boot\\limine\\limine.conf") != NULL &&
              strstr(fixture_serial,
                     "config=\\boot\\hfoc\\limine.conf") != NULL &&
              strstr(fixture_serial,
                     "loader=\\boot\\limine\\limine-bios.sys") != NULL,
          "the record names the menu Limine reads, the chain configuration "
          "and the staged loader");
    CHECK(strstr(fixture_serial, " source=GENERATED ") != NULL &&
              strstr(fixture_serial, " verify=READBACK gpt=VALID") != NULL,
          "the record names the generated configuration and the read-back");
    CHECK(strstr(fixture_console,
                 "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
                 "mechanism=BIOS_MBR") != NULL,
          "the BIOS record reaches the durable journal");
    CHECK(fixture_write_calls >= outcome.stage_sector_writes,
          "the run's writes include the 44 stage sectors");
}

/*
 * A sealed configuration (no generated menu) is still a valid BIOS install:
 * the boot volume carries the sealed configuration and the record says so.
 */
static void test_sealed_configuration_without_menu(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(false);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;
    struct efi_fat32_esp esp;
    static unsigned char config_read[FIXTURE_CONFIG_CAPACITY];
    size_t read_size = 0U;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_BIOS_LEGACY);
    bios_boot = fixture_bios_partition(&plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
    boot_volume = fixture_bios_partition(
        &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);
    CHECK(production_installer_bios_boot_run(
              &installer, &device, &bios_boot, &boot_volume, &media,
              &outcome) == PRODUCTION_INSTALLER_OK &&
              !outcome.menu_installed,
          "a BIOS install with no generated menu installs the sealed config");
    memset(&esp, 0, sizeof(esp));
    CHECK(efi_fat32_esp_bind_device(
              &device, boot_volume.first_lba, boot_volume.length_lba,
              &esp) == EFI_FAT32_ESP_OK &&
              efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BIOS_CONFIG_PATH,
                  config_read, sizeof(config_read),
                  &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(fixture_config_text) - 1U &&
              memcmp(config_read, fixture_config_text, read_size) == 0,
          "the sealed configuration is the installed one");
    CHECK(strstr(fixture_serial, " source=SEALED") != NULL,
          "the record names the sealed configuration");
}

/*
 * A configuration that does not name the staged kernel cannot boot: the gate
 * refuses it before the first write.
 */
static void test_config_without_kernel_reference_is_refused(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(false);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_BIOS_LEGACY);
    bios_boot = fixture_bios_partition(&plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
    boot_volume = fixture_bios_partition(
        &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);
    media.config = (const unsigned char *)"timeout: 3\n";
    media.config_size = 11U;
    fixture_write_calls = 0U;
    CHECK(production_installer_bios_boot_requirements(
              &installer, &bios_boot, &boot_volume, &media, &outcome) ==
              PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID,
          "a configuration that does not name the kernel is refused");
    media = fixture_media_view(false);
    media.kernel_size = PRODUCTION_INSTALLER_BOOT_KERNEL_MAX + 1U;
    CHECK(production_installer_bios_boot_requirements(
              &installer, &bios_boot, &boot_volume, &media, &outcome) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "a kernel above its bound is refused");
    CHECK(fixture_write_calls == 0U,
          "the configuration refusals perform no write");
}

/*
 * The tap the flow performs after the gate: a failed run publishes the BIOS
 * refusal with its own reason, and an unverified medium cannot authorize the
 * stage.
 */
static void test_run_refusals_are_named(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view(true);
    struct production_installer_bios_boot_outcome outcome;
    struct production_installer_bios_partition bios_boot;
    struct production_installer_bios_partition boot_volume;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device,
                              INSTALLER_FIRMWARE_BIOS_LEGACY);
    bios_boot = fixture_bios_partition(&plan, INSTALLER_LAYOUT_ROLE_BIOS_BOOT);
    boot_volume = fixture_bios_partition(
        &plan, INSTALLER_LAYOUT_ROLE_BOOT_VOLUME);
    {
        struct production_installer_bios_partition small = bios_boot;

        memset(&outcome, 0, sizeof(outcome));
        small.length_lba = 16U;
        CHECK(production_installer_bios_boot_run(
                  &installer, &device, &small, &boot_volume, &media,
                  &outcome) ==
                  PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL,
              "the run refuses a too-small BIOS boot partition");
        (void)production_installer_bios_boot_publish(
            &outcome, PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL);
        CHECK(strstr(fixture_serial,
                     "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
                     "reason=PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_"
                     "TOO_SMALL") ==
                  fixture_serial,
              "the BIOS refusal is published with its own reason");
    }
    installer.media_verified = false;
    CHECK(production_installer_bios_boot_run(
              &installer, &device, &bios_boot, &boot_volume, &media,
              &outcome) == PRODUCTION_INSTALLER_MEDIA_UNVERIFIED,
          "an unverified media cannot authorize the BIOS boot path");
}

int main(void)
{
    test_gate_refuses_before_first_write();
    test_uefi_layout_never_enters_the_bios_path();
    test_blob_transport_is_the_pinned_release();
    test_bios_clean_plan_stages_the_measured_recipe();
    test_sealed_configuration_without_menu();
    test_config_without_kernel_reference_is_refused();
    test_run_refusals_are_named();

    if (failures != 0) {
        fprintf(stderr,
                "Production installer BIOS boot host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Production installer BIOS boot host tests: ALL OK");
    return 0;
}

#endif
