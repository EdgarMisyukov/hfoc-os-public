#include "production_installer.h"
#include "production_installer_boot_stage.h"

#include "appliance_state_layout.h"
#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "installer_layout_contract.h"
#include "installer_supported_hardware.h"
#include "sha256.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PRODUCTION_INSTALLER_BOOT_STAGE_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_MEDIA_LBA UINT64_C(400000)
#define FIXTURE_SECTOR_SLOTS 4096U
#define FIXTURE_VARIABLE_CAPACITY 8U
#define FIXTURE_VARIABLE_DATA_MAX 512U
#define FIXTURE_KERNEL_BYTES (512U * 1024U)
#define FIXTURE_PAYLOAD_BYTES 1024U
#define FIXTURE_LOADER_BYTES 4096U
#define FIXTURE_MANIFEST_BYTES 288U
#define FIXTURE_SIGNATURE_LENGTH 32U
#define FIXTURE_RELEASE_ID 7U
#define FIXTURE_SIGNER_ID 1U
#define FIXTURE_RECORD_CAPACITY 4096U

struct fixture_sector {
    bool used;
    uint64_t lba;
    unsigned char data[FIXTURE_BLOCK_SIZE];
};

struct fixture_variable {
    bool present;
    struct efi_boot_variable_key key;
    size_t length;
    unsigned char data[FIXTURE_VARIABLE_DATA_MAX];
};

struct fixture_variable_store {
    size_t variable_count;
    struct fixture_variable variables[FIXTURE_VARIABLE_CAPACITY];
    size_t write_calls;
};

static struct fixture_sector fixture_sectors[FIXTURE_SECTOR_SLOTS];
static size_t fixture_sector_count;
static uint64_t fixture_write_calls;
static struct fixture_variable_store fixture_variables;
static char fixture_serial[FIXTURE_RECORD_CAPACITY];
static size_t fixture_serial_length;
static int failures;

static const char fixture_config_text[] =
    "timeout: 3\n"
    "\n"
    "/HFOC OS 0.2\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-kernel.elf\n"
    "module_path: boot():/boot/hfoc-boot.man\n"
    "module_path: boot():/boot/limine/limine.conf\n";

static unsigned char fixture_kernel[FIXTURE_KERNEL_BYTES];
static unsigned char fixture_payload[FIXTURE_PAYLOAD_BYTES];
static unsigned char fixture_loader[FIXTURE_LOADER_BYTES];
static unsigned char fixture_manifest[FIXTURE_MANIFEST_BYTES];
static unsigned char fixture_media[64U];
static unsigned char fixture_envelope[256U];
static size_t fixture_envelope_length;
static struct verification_policy fixture_policy;

static const unsigned char fixture_device_path[8U] = {
    0x01U, 0x01U, 0x04U, 0x00U, 0x7FU, 0xFFU, 0x04U, 0x00U
};
static const char fixture_disk_identity[] = "installed-boot-disk";

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

/* The boot-stage record is published through the serial seam: the host proof
 * captures it so the published line is checked, not assumed. */
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

/*
 * ITEM-405: the record the boot stage publishes is mirrored to the local
 * console, so the host proof captures that surface too and checks that the
 * refusal an operator would read on screen is really published - not just the
 * serial copy.  The seam only records; it never writes to serial.  ITEM-478:
 * the record now goes to the durable journal rather than the screen, and the
 * journal seam is captured here so the proof still checks the published line.
 */
static char fixture_console[FIXTURE_RECORD_CAPACITY];
static size_t fixture_console_length;

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

static bool fixture_key_equal(
    const struct efi_boot_variable_key *left,
    const struct efi_boot_variable_key *right)
{
    return memcmp(left->guid, right->guid,
                  EFI_BOOT_VARIABLE_GUID_LENGTH) == 0 &&
           strcmp(left->name, right->name) == 0;
}

static enum efi_boot_variable_io_status fixture_read_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    struct fixture_variable_store *store =
        (struct fixture_variable_store *)context;
    size_t index;

    if (store == NULL || key == NULL || data == NULL ||
        length == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    for (index = 0U; index < store->variable_count; ++index) {
        const struct fixture_variable *variable =
            &store->variables[index];

        if (variable->present &&
            fixture_key_equal(&variable->key, key)) {
            if (variable->length > capacity) {
                return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
            }
            memcpy(data, variable->data, variable->length);
            *length = variable->length;
            return EFI_BOOT_VARIABLE_IO_OK;
        }
    }
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status fixture_write_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    struct fixture_variable_store *store =
        (struct fixture_variable_store *)context;
    size_t index;

    if (store == NULL || key == NULL || data == NULL) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    ++store->write_calls;
    if (length > FIXTURE_VARIABLE_DATA_MAX) {
        return EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL;
    }
    for (index = 0U; index < store->variable_count; ++index) {
        if (store->variables[index].present &&
            fixture_key_equal(&store->variables[index].key, key)) {
            store->variables[index].length = length;
            memcpy(store->variables[index].data, data, length);
            return EFI_BOOT_VARIABLE_IO_OK;
        }
    }
    if (store->variable_count >= FIXTURE_VARIABLE_CAPACITY) {
        return EFI_BOOT_VARIABLE_IO_ERROR;
    }
    index = store->variable_count++;
    store->variables[index].present = true;
    store->variables[index].key = *key;
    store->variables[index].length = length;
    memcpy(store->variables[index].data, data, length);
    return EFI_BOOT_VARIABLE_IO_OK;
}

static void fixture_reset(void)
{
    size_t index;

    memset(fixture_sectors, 0, sizeof(fixture_sectors));
    fixture_sector_count = 0U;
    fixture_write_calls = 0U;
    memset(&fixture_variables, 0, sizeof(fixture_variables));
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

static void fixture_report(struct installer_discovery_report *report)
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

static struct production_installer_boot_media fixture_media_view(void)
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
 * The controller runs the production order: evidence, sealed media,
 * hardware, contracted layout, preview, confirmation, then the layout itself.
 */
static void fixture_prepare_executing(
    struct production_installer *installer,
    struct installer_discovery_report *report,
    struct installer_layout_plan *plan,
    struct block_device *device)
{
    struct production_installer_source source = fixture_source();
    struct installer_layout_snapshot snapshot;

    fixture_report(report);
    CHECK(production_installer_init(installer) ==
              PRODUCTION_INSTALLER_OK &&
          production_installer_prepare(
              installer, report,
              PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
              fixture_disk_identity,
              sizeof(fixture_disk_identity) - 1U, &source,
              true) == PRODUCTION_INSTALLER_OK,
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
 * ITEM-398: the boot stage takes the ESP explicitly instead of reading it out
 * of the plan, because a preserving install reuses an ESP that is not one of
 * the plan's roles at all.  A whole-disk fixture derives it from its own plan
 * - the EFI system role - and never marks it reused, so the writer may format
 * a volume that is still empty.
 */
static struct production_installer_boot_esp fixture_boot_esp(
    const struct installer_layout_plan *plan)
{
    struct production_installer_boot_esp esp;
    uint32_t index;

    memset(&esp, 0, sizeof(esp));
    for (index = 0U; index < INSTALLER_LAYOUT_PARTITION_CAPACITY; ++index) {
        if (plan->partitions[index].present &&
            plan->partitions[index].role ==
                INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            esp.present = true;
            esp.reused = false;
            esp.first_lba = plan->partitions[index].first_lba;
            esp.length_lba = plan->partitions[index].length_lba;
            break;
        }
    }
    return esp;
}

static void test_requirement_gate_refuses_before_first_write(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_partition requests[4U];
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp esp;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);

    memset(requests, 0, sizeof(requests));
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].length_lba = 1024U;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].length_lba = 2048U;
    requests[2U].present = true;
    requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    requests[2U].length_lba = 2048U;
    requests[3U].present = true;
    requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    requests[3U].length_lba = 2048U;
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK,
          "small-ESP fixture plans a manual layout");
    esp = fixture_boot_esp(&plan);
    fixture_write_calls = 0U;
    CHECK(production_installer_boot_stage_requirements(
              &installer, &esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_ESP_TOO_SMALL,
          "an ESP too small for the boot set is refused");
    CHECK(fixture_write_calls == 0U,
          "the refusal performs no write");
    CHECK(outcome.required_sectors > 1024U &&
              outcome.esp_length_lba == 1024U,
          "the refusal publishes the requirement and the declared ESP");
    memset(&outcome, 0, sizeof(outcome));
    CHECK(production_installer_boot_stage_requirements(
              &installer, &esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_ESP_TOO_SMALL,
          "the gate is repeatable");
    (void)production_installer_boot_stage_publish(
        &outcome, PRODUCTION_INSTALLER_ESP_TOO_SMALL);
    CHECK(strstr(fixture_serial,
                 "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
                 "reason=PRODUCTION_INSTALLER_ESP_TOO_SMALL") ==
              fixture_serial,
          "the refusal is published with its own reason");
    CHECK(strstr(fixture_serial, "required_sectors=") != NULL &&
              strstr(fixture_serial, "esp_sectors=1024") != NULL,
          "the refusal record names the requirement and the ESP");
}

static void test_stage_publishes_removable_media_mechanism(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &fixture_variables, fixture_read_variable,
              fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK &&
          efi_boot_environment_set_device_path(
              &environment, fixture_device_path,
              sizeof(fixture_device_path)) ==
              EFI_BOOT_ENVIRONMENT_OK,
          "installed-boot fixture initializes the EFI environment");
    memset(&outcome, 0, sizeof(outcome));
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_OK,
          "the contracted ESP holds the boot set");
    CHECK(outcome.required_sectors <= outcome.esp_length_lba &&
              outcome.esp_length_lba ==
                  INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA,
          "the requirement is met by the declared ESP role");
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_OK,
          "the installed boot set is staged and verified");
    CHECK(outcome.payload_staged && outcome.payload_verified,
          "the outcome records the staged and verified set");
    CHECK(outcome.mechanism ==
              PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE ||
              outcome.mechanism ==
                  PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH,
          "the outcome records a boot mechanism");

    memset(&esp, 0, sizeof(esp));
    CHECK(efi_fat32_esp_bind(
              &environment, outcome.esp_first_lba,
              outcome.esp_length_lba, &esp) == EFI_FAT32_ESP_OK,
          "the staged ESP binds");
    {
        unsigned char kernel_readback[64U];
        size_t read_size = 0U;

        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BOOT_KERNEL_PATH,
                  kernel_readback, sizeof(kernel_readback),
                  &read_size) == EFI_FAT32_ESP_CAPACITY,
              "the staged kernel is larger than a short buffer");
    }
    {
        unsigned char loader_readback[FIXTURE_LOADER_BYTES];
        size_t read_size = 0U;

        CHECK(efi_fat32_esp_read_file(
                  &esp, PRODUCTION_INSTALLER_BOOT_LOADER_PATH,
                  loader_readback, sizeof(loader_readback),
                  &read_size) == EFI_FAT32_ESP_OK &&
              read_size == sizeof(fixture_loader) &&
              memcmp(loader_readback, fixture_loader,
                     sizeof(fixture_loader)) == 0,
              "the loader is staged on the removable-media path");
    }
    CHECK(efi_fat32_esp_read_file(
              &esp, PRODUCTION_INSTALLER_BOOT_KERNEL_PATH, NULL,
              0U, NULL) == EFI_FAT32_ESP_NULL,
          "a null read is refused");
    CHECK(strstr(fixture_serial,
                 "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE") ==
              fixture_serial,
          "the install record publishes an available boot entry");
    CHECK(strstr(fixture_serial,
                 " mechanism=REMOVABLE_MEDIA_PATH path=\\EFI\\BOOT\\BOOTX64.EFI") !=
              NULL ||
              strstr(fixture_serial,
                     " mechanism=EFI_BOOT_VARIABLE path=\\EFI\\HFOC\\BOOTX64.EFI") !=
                  NULL,
          "the record names the mechanism and its path");
    CHECK(strstr(fixture_serial,
                 " kernel=\\boot\\hfoc-kernel.elf config=\\boot\\limine\\limine.conf") !=
              NULL,
          "the record names the staged kernel and its configuration");
    /*
     * ITEM-409: the record states which configuration the loader reads, how
     * many entries it offers and - for a caller with no menu - that the sealed
     * configuration is the installed one and why.  A record that said `NONE`
     * left an operator with a single-entry boot and no explanation.
     */
    CHECK(strstr(fixture_serial,
                 " menu=\\boot\\limine\\limine.conf entries=1 "
                 "source=SEALED menu_reason=MENU_UNAVAILABLE ") != NULL,
          "without a menu the record names the sealed configuration and why");
    CHECK(strstr(fixture_serial, "menu=NONE") == NULL,
          "the record no longer publishes a bare MENU=NONE");
    CHECK(strstr(fixture_serial, "timeout=") == NULL &&
              strstr(fixture_serial, "payload_set=5 ") != NULL,
          "a sealed set is five files and claims no menu timeout");
    CHECK(strstr(fixture_serial, "verify=SEALED_MEDIA") != NULL &&
              strstr(fixture_serial, "sealed_digest=") != NULL,
          "the record names the sealed-media verification");
}

/*
 * The mechanism the installed disk actually relies on in the field: the
 * running kernel has no EFI runtime, so no Boot#### variable can be written
 * and the removable-media path is what makes the firmware boot the disk.  The
 * record must say exactly that instead of claiming a boot entry.
 */
static void test_removable_media_path_is_published(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct efi_boot_environment environment;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &fixture_variables, fixture_read_variable,
              fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK,
          "no-runtime fixture initializes the EFI environment");
    memset(&outcome, 0, sizeof(outcome));
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_OK &&
              outcome.mechanism ==
                  PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH &&
              !outcome.boot_entry_created &&
              !outcome.variable_attempted,
          "without an EFI device path the removable-media path is used");
    CHECK(strncmp(fixture_serial,
                  "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
                  "mechanism=REMOVABLE_MEDIA_PATH "
                  "path=\\EFI\\BOOT\\BOOTX64.EFI ",
                  strlen("[PRODUCTION-INSTALL] boot-entry state=AVAILABLE "
                         "mechanism=REMOVABLE_MEDIA_PATH "
                         "path=\\EFI\\BOOT\\BOOTX64.EFI ")) == 0,
          "the record publishes the removable-media mechanism and path");
    CHECK(strstr(fixture_serial,
                 " variable=UNAVAILABLE boot_entry=NONE") != NULL,
          "the record does not claim a boot entry it did not create");
    CHECK(strcmp(production_installer_boot_mechanism_string(
                     outcome.mechanism),
                 "REMOVABLE_MEDIA_PATH") == 0 &&
              strcmp(production_installer_boot_mechanism_string(
                         PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE),
                     "EFI_BOOT_VARIABLE") == 0,
          "the mechanism names are exact");
}

static void test_config_without_kernel_reference_is_refused(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    media.config = (const unsigned char *)"timeout: 3\n";
    media.config_size = 11U;
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID,
          "a configuration that does not name the kernel is refused");
    media = fixture_media_view();
    media.config_size = PRODUCTION_INSTALLER_BOOT_CONFIG_MAX + 1U;
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "a configuration above its bound is refused");
    media = fixture_media_view();
    media.kernel_size = PRODUCTION_INSTALLER_BOOT_KERNEL_MAX + 1U;
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "a kernel above its bound is refused");
    media = fixture_media_view();
    media.version = 99U;
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, &media, &outcome) ==
              PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE,
          "a media description with the wrong version is refused");
    CHECK(production_installer_boot_stage_requirements(
              &installer, &boot_esp, NULL, &outcome) ==
              PRODUCTION_INSTALLER_NULL,
          "a missing media description is refused");
}

static void test_tampered_source_is_refused(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct efi_boot_environment environment;
    struct block_device device;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &fixture_variables, fixture_read_variable,
              fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK,
          "tampered-source fixture initializes the EFI environment");
    /*
     * The bytes handed to the writer are not the bytes the sealed media
     * declared, so the staged set cannot match the sealed source digest.
     */
    media.kernel = fixture_loader;
    media.kernel_size = sizeof(fixture_loader);
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_VERIFICATION_FAILED,
          "a staged set that does not match the sealed media is refused");
    CHECK(outcome.payload_staged && !outcome.payload_verified,
          "the outcome reports staged-but-unverified");
    media = fixture_media_view();
    installer.media_verified = false;
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_MEDIA_UNVERIFIED,
          "an unverified media cannot authorize a staged boot set");
}

/*
 * ITEM-405: the ESP role a whole-disk install created is formatted, and a
 * shared ESP is never formatted.
 *
 * The acceptance host installed onto a disk that had carried a Windows layout.
 * Its partition table was replaced by ours, but the 33 MiB ESP role still held
 * the *old* system's FAT32 volume, whose boot sector described the 100 MiB
 * volume the previous table had.  The bind used to accept that volume (it never
 * read the volume's own total-sector count), so the role was never formatted
 * and the installed boot set was staged against a geometry the volume does not
 * have.  These cases hold both halves of the fixed rule:
 *
 *   * a role this install owns is formatted, whatever the bytes in it are;
 *   * a shared ESP is bound as it is, refused when it does not fit its
 *     partition, and written to by nothing.
 */
static void fixture_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)(value >> 8U);
}

static void fixture_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static uint16_t fixture_load_le16(const unsigned char *bytes)
{
    return (uint16_t)((uint16_t)bytes[0U] |
                      ((uint16_t)bytes[1U] << 8U));
}

static uint32_t fixture_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0U] |
           ((uint32_t)bytes[1U] << 8U) |
           ((uint32_t)bytes[2U] << 16U) |
           ((uint32_t)bytes[3U] << 24U);
}

/* The stale volume the acceptance run found: a 100 MiB FAT32 inside the role. */
static void fixture_seed_stale_esp(
    struct block_device *device,
    uint64_t first_lba)
{
    unsigned char sector[FIXTURE_BLOCK_SIZE];

    memset(sector, 0, sizeof(sector));
    memcpy(sector + 3U, "MSDOS5.0", 8U);
    fixture_store_le16(sector + 11U, (uint16_t)FIXTURE_BLOCK_SIZE);
    sector[13U] = 2U;
    fixture_store_le16(sector + 14U, 6654U);
    sector[16U] = 2U;
    fixture_store_le32(sector + 32U, 204800U);
    fixture_store_le32(sector + 36U, 769U);
    fixture_store_le32(sector + 44U, 2U);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    (void)device->write_fn(
        device, first_lba * FIXTURE_BLOCK_SIZE, sector, sizeof(sector));
}

static void test_install_owns_the_esp_it_creates(void)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct production_installer_boot_media media = fixture_media_view();
    struct production_installer_boot_outcome outcome;
    struct production_installer_boot_esp boot_esp;
    struct efi_boot_environment environment;
    struct block_device device;
    unsigned char read_back[FIXTURE_BLOCK_SIZE];
    uint32_t sectors_per_fat;

    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    memset(&environment, 0, sizeof(environment));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              &fixture_variables, fixture_read_variable,
              fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK &&
          efi_boot_environment_set_device_path(
              &environment, fixture_device_path,
              sizeof(fixture_device_path)) ==
              EFI_BOOT_ENVIRONMENT_OK,
          "owned-ESP fixture initializes the EFI environment");
    CHECK(!boot_esp.reused,
          "a whole-disk install owns the ESP role it planned");

    /*
     * The role carries the stale foreign volume before the run.  The install
     * has to succeed and the role has to carry our own volume afterwards.
     */
    fixture_seed_stale_esp(&device, boot_esp.first_lba);
    memset(&outcome, 0, sizeof(outcome));
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_OK,
          "the install stages into the role despite the stale volume");
    memset(read_back, 0, sizeof(read_back));
    CHECK(device.read_fn(
              &device, boot_esp.first_lba * FIXTURE_BLOCK_SIZE,
              read_back, sizeof(read_back)) == BLOCK_DEVICE_OK &&
          memcmp(read_back + 3U, "HFOC    ", 8U) == 0,
          "the ESP role carries the installer's own volume");
    CHECK(fixture_load_le32(read_back + 32U) ==
              (uint32_t)boot_esp.length_lba &&
              fixture_load_le16(read_back + 14U) == 32U &&
              read_back[16U] == 2U &&
              read_back[13U] == 1U,
          "the formatted role declares the partition it lives in");
    sectors_per_fat = fixture_load_le32(read_back + 36U);
    CHECK(sectors_per_fat != 0U &&
              (uint64_t)sectors_per_fat * 128U >=
                  2U + ((uint64_t)boot_esp.length_lba - 32U -
                        (uint64_t)2U * sectors_per_fat),
          "the formatted FAT addresses the whole role");

    /*
     * The other half: the same stale volume on a *shared* ESP.  The volume
     * belongs to the system already on the machine, so it is never formatted -
     * it is refused, before the first write, as ESP_UNSUITABLE.
     */
    fixture_reset();
    device = fixture_device();
    fixture_prepare_executing(&installer, &report, &plan, &device);
    boot_esp = fixture_boot_esp(&plan);
    boot_esp.reused = true;
    fixture_seed_stale_esp(&device, boot_esp.first_lba);
    fixture_write_calls = 0U;
    memset(&outcome, 0, sizeof(outcome));
    CHECK(production_installer_boot_stage_run(
              &installer, &environment, &boot_esp, &media,
              &outcome) == PRODUCTION_INSTALLER_ESP_UNSUITABLE,
          "a shared ESP that does not fit its partition is refused");
    CHECK(fixture_write_calls == 0U,
          "the shared-ESP refusal performs no write at all");
    memset(read_back, 0, sizeof(read_back));
    CHECK(device.read_fn(
              &device, boot_esp.first_lba * FIXTURE_BLOCK_SIZE,
              read_back, sizeof(read_back)) == BLOCK_DEVICE_OK &&
          memcmp(read_back + 3U, "MSDOS5.0", 8U) == 0 &&
          fixture_load_le32(read_back + 32U) == 204800U,
          "the shared ESP keeps the foreign volume byte for byte");
    (void)production_installer_boot_stage_publish(
        &outcome, PRODUCTION_INSTALLER_ESP_UNSUITABLE);
    CHECK(strstr(fixture_serial,
                 "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
                 "reason=PRODUCTION_INSTALLER_ESP_UNSUITABLE") ==
              fixture_serial,
          "the shared-ESP refusal is published on serial");
    CHECK(strstr(fixture_console,
                 "[PRODUCTION-INSTALL] boot-entry state=UNAVAILABLE "
                 "reason=PRODUCTION_INSTALLER_ESP_UNSUITABLE") != NULL,
          "the shared-ESP refusal reaches the durable journal");
}

int main(void)
{
    test_requirement_gate_refuses_before_first_write();
    test_stage_publishes_removable_media_mechanism();
    test_removable_media_path_is_published();
    test_config_without_kernel_reference_is_refused();
    test_tampered_source_is_refused();
    test_install_owns_the_esp_it_creates();

    if (failures != 0) {
        fprintf(stderr,
                "Production installer boot stage host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Production installer boot stage host tests: ALL OK");
    return 0;
}

#endif
