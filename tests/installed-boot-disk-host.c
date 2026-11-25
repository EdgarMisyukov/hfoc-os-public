/*
 * ITEM-397 installed-disk builder and proof.
 *
 * This is both a host proof and the fixture builder the QEMU disk-boot phase
 * uses.  It runs the production install entry points against a *file-backed*
 * block device - the contracted layout, the persistent state volume and the
 * installed boot payload set - so the disk image QEMU boots from is the disk
 * the production writer produces, not a hand-built imitation of it.
 *
 * Modes:
 *   (no arguments)                 host proof over a temporary image
 *   build  --disk F ...            write an installed disk image
 *   refuse --disk F --esp-lba N    prove the small-ESP refusal, no writes
 *   verify --disk F ...            re-verify the staged payload set on disk
 */
#include "production_installer.h"
#include "production_installer_boot_stage.h"
#include "production_installer_esp.h"
#include "production_installer_state.h"

#include "appliance_state_layout.h"
#include "block_device.h"
#include "efi_boot_environment.h"
#include "efi_fat32_esp.h"
#include "installer_layout_contract.h"
#include "installer_supported_hardware.h"
#include "sha256.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifdef HFOC_INSTALLED_BOOT_DISK_HOST_TEST

#define DISK_BLOCK_SIZE 512U
#define DISK_DEFAULT_LBA UINT64_C(400000)
#define DISK_REFUSAL_ESP_LBA UINT64_C(1024)
#define DISK_SIGNATURE_LENGTH 32U
#define DISK_RELEASE_ID 11U
#define DISK_SIGNER_ID 1U
#define DISK_UNUSED_BLOCKS 4U
#define DISK_RECORD_CAPACITY 2048U
#define DISK_PROOF_KERNEL_BYTES (2U * 1024U * 1024U)

struct disk_file {
    int fd;
    uint64_t block_count;
    uint64_t write_calls;
    uint64_t read_calls;
    bool fail_writes;
    char path[512U];
};

static struct disk_file disk_state;
static struct verification_policy disk_policy;
static char disk_record[DISK_RECORD_CAPACITY];
static size_t disk_record_length;
static int disk_failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++disk_failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void serial_write(const char *text)
{
    while (text != NULL && *text != '\0' &&
           disk_record_length + 1U < sizeof(disk_record)) {
        disk_record[disk_record_length++] = *text++;
    }
    disk_record[disk_record_length] = '\0';
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
 * ITEM-405: the boot stage publishes its record, and this builder links that
 * writer.  ITEM-478 sends the record to the durable journal rather than the
 * screen, and the journal is a surface this builder does not have, so it
 * records nothing here - and it deliberately does not write to the serial
 * seam, which would change the record the builder prints.
 */
void console_publish_record(const char *record, size_t length)
{
    (void)record;
    (void)length;
}

void console_publish_journal(const char *record, size_t length)
{
    (void)record;
    (void)length;
}

/*
 * The install media boot has no EFI runtime, so the variable seam is bound but
 * carries no boot entry: the removable-media path is the mechanism that makes
 * the disk boot, and the record has to say so.
 */
static enum efi_boot_variable_io_status disk_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status disk_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_OK;
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct disk_file *disk = (struct disk_file *)device->context;
    ssize_t result;
    uint64_t index;
    unsigned char *bytes = (unsigned char *)buffer;

    if (disk == NULL || disk->fd < 0 || buffer == NULL ||
        length == 0U || offset % DISK_BLOCK_SIZE != 0U ||
        length % DISK_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    if (offset / DISK_BLOCK_SIZE >= disk->block_count ||
        length / DISK_BLOCK_SIZE >
            disk->block_count - offset / DISK_BLOCK_SIZE) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++disk->read_calls;
    result = pread(disk->fd, buffer, length, (off_t)offset);
    if (result != (ssize_t)length) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    (void)index;
    (void)bytes;
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct disk_file *disk = (struct disk_file *)device->context;
    ssize_t result;

    if (disk == NULL || disk->fd < 0 || buffer == NULL ||
        length == 0U || offset % DISK_BLOCK_SIZE != 0U ||
        length % DISK_BLOCK_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    if (offset / DISK_BLOCK_SIZE >= disk->block_count ||
        length / DISK_BLOCK_SIZE >
            disk->block_count - offset / DISK_BLOCK_SIZE) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    ++disk->write_calls;
    if (disk->fail_writes) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    result = pwrite(disk->fd, buffer, length, (off_t)offset);
    if (result != (ssize_t)length) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(
    struct block_device *device)
{
    struct disk_file *disk = (struct disk_file *)device->context;

    if (disk == NULL || disk->fd < 0) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = DISK_BLOCK_SIZE;
    device.block_count = disk_state.block_count;
    device.context = &disk_state;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static bool disk_open(const char *path, uint64_t block_count, bool create)
{
    struct stat status;

    memset(&disk_state, 0, sizeof(disk_state));
    disk_state.fd = -1;
    disk_state.block_count = block_count;
    if (path == NULL || strlen(path) >= sizeof(disk_state.path)) {
        return false;
    }
    memcpy(disk_state.path, path, strlen(path) + 1U);
    if (create) {
        disk_state.fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    } else {
        disk_state.fd = open(path, O_RDWR);
    }
    if (disk_state.fd < 0) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (create) {
        if (ftruncate(disk_state.fd,
                      (off_t)(block_count * DISK_BLOCK_SIZE)) != 0) {
            fprintf(stderr, "cannot size %s: %s\n", path,
                    strerror(errno));
            return false;
        }
    } else if (fstat(disk_state.fd, &status) != 0 ||
               (uint64_t)status.st_size <
                   block_count * DISK_BLOCK_SIZE) {
        return false;
    }
    return true;
}

static void disk_close(void)
{
    if (disk_state.fd >= 0) {
        (void)close(disk_state.fd);
        disk_state.fd = -1;
    }
}

static bool disk_read_file(
    const char *path,
    unsigned char *buffer,
    size_t capacity,
    size_t *length)
{
    FILE *file = fopen(path, "rb");
    long size;

    if (file == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (size = ftell(file)) <= 0L ||
        (size_t)size > capacity ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "cannot size %s (bound %llu)\n", path,
                (unsigned long long)capacity);
        (void)fclose(file);
        return false;
    }
    if (fread(buffer, 1U, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        (void)fclose(file);
        return false;
    }
    (void)fclose(file);
    *length = (size_t)size;
    return true;
}

static bool disk_read_alloc(
    const char *path,
    unsigned char **data,
    size_t *length,
    size_t limit)
{
    FILE *file = fopen(path, "rb");
    long size;
    unsigned char *buffer;

    if (file == NULL) {
        fprintf(stderr, "cannot open %s: %s\n", path, strerror(errno));
        return false;
    }
    if (fseek(file, 0L, SEEK_END) != 0 ||
        (size = ftell(file)) <= 0L ||
        (size_t)size > limit ||
        fseek(file, 0L, SEEK_SET) != 0) {
        fprintf(stderr, "cannot size %s (bound %llu)\n", path,
                (unsigned long long)limit);
        (void)fclose(file);
        return false;
    }
    buffer = (unsigned char *)malloc((size_t)size);
    if (buffer == NULL) {
        (void)fclose(file);
        return false;
    }
    if (fread(buffer, 1U, (size_t)size, file) != (size_t)size) {
        fprintf(stderr, "cannot read %s\n", path);
        free(buffer);
        (void)fclose(file);
        return false;
    }
    (void)fclose(file);
    *data = buffer;
    *length = (size_t)size;
    return true;
}

static void disk_report(struct installer_discovery_report *report,
                        uint64_t block_count,
                        const char *identity)
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
    report->disks[0].block_size = DISK_BLOCK_SIZE;
    report->disks[0].block_count = block_count;
    memcpy(report->disks[0].identity, identity, strlen(identity) + 1U);
}

static enum installer_media_signature_status disk_signature_verify(
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
        signer_id != DISK_SIGNER_ID ||
        signature_length != DISK_SIGNATURE_LENGTH) {
        return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
    }
    for (index = 0U; index < DISK_SIGNATURE_LENGTH; ++index) {
        if (signature[index] != (unsigned char)(
                digest[index % SHA256_DIGEST_SIZE] ^ 0x3CU)) {
            return INSTALLER_MEDIA_SIGNATURE_MISMATCH;
        }
    }
    return INSTALLER_MEDIA_SIGNATURE_OK;
}

/*
 * The sealed media manifest.  The release key material is ITEM-372's
 * boundary; this builder seals the manifest in-process with the documented
 * deterministic fixture signature every other QEMU media proof uses, and the
 * installer verifies it through the same caller-bound verifier seam a release
 * would bind.
 */
static bool disk_seal_media(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    const struct production_installer_source *source)
{
    struct verification_policy_snapshot snapshot;
    struct installer_media_verifier verifier;
    struct installer_supported_hardware_policy hardware_policy;
    struct installer_media_source media_source;
    unsigned char media[64U];
    unsigned char digest[SHA256_DIGEST_SIZE];
    unsigned char source_digest[SHA256_DIGEST_SIZE];
    unsigned char signature[DISK_SIGNATURE_LENGTH];
    unsigned char envelope[256U];
    size_t envelope_length = 0U;
    uint64_t source_length = 0U;
    size_t index;

    for (index = 0U; index < sizeof(media); ++index) {
        media[index] = (unsigned char)(index * 7U + 5U);
    }
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = source->boot_payload;
    media_source.boot_payload_size = source->boot_payload_size;
    media_source.limine_config = source->limine_config;
    media_source.limine_config_size = source->limine_config_size;
    media_source.kernel = source->kernel;
    media_source.kernel_size = source->kernel_size;
    if (sha256_compute(media, sizeof(media), digest) != SHA256_OK ||
        installer_media_source_digest(
            &media_source, source_digest, &source_length) !=
            INSTALLER_MEDIA_OK) {
        return false;
    }
    for (index = 0U; index < sizeof(signature); ++index) {
        signature[index] = (unsigned char)(
            digest[index % SHA256_DIGEST_SIZE] ^ 0x3CU);
    }
    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = VERIFICATION_POLICY_VERSION;
    snapshot.generation = 1U;
    snapshot.anchor_count = 1U;
    snapshot.anchors[0U].id = DISK_SIGNER_ID;
    snapshot.anchors[0U].version = TRUST_ANCHOR_DESCRIPTOR_VERSION;
    snapshot.anchors[0U].algorithm_id =
        TRUST_ANCHOR_ALGORITHM_ECDSA_P256;
    snapshot.anchors[0U].state = TRUST_ANCHOR_STATE_ACTIVE;
    snapshot.anchors[0U].accepted_purposes =
        KEY_USE_PURPOSE_BIT(KEY_USE_PURPOSE_IMAGE);
    memset(&disk_policy, 0, sizeof(disk_policy));
    if (verification_policy_init(&disk_policy, &snapshot) !=
        VERIFICATION_POLICY_OK) {
        return false;
    }
    if (installer_media_manifest_seal(
            &disk_policy, DISK_RELEASE_ID, DISK_SIGNER_ID,
            TRUST_ANCHOR_ALGORITHM_ECDSA_P256, digest, sizeof(media),
            source_digest, source_length, signature,
            sizeof(signature), envelope, sizeof(envelope),
            &envelope_length) != INSTALLER_MEDIA_OK) {
        return false;
    }
    memset(&verifier, 0, sizeof(verifier));
    verifier.verify = disk_signature_verify;
    if (production_installer_verify_media(
            installer, &disk_policy, &verifier, DISK_RELEASE_ID,
            envelope, envelope_length, media, sizeof(media)) !=
        PRODUCTION_INSTALLER_OK) {
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

struct disk_components {
    struct production_installer_source source;
    struct production_installer_boot_media media;
};

static void disk_components_init(
    struct disk_components *components,
    unsigned char *payload,
    size_t payload_size,
    unsigned char *config,
    size_t config_size,
    unsigned char *kernel,
    size_t kernel_size,
    unsigned char *loader,
    size_t loader_size,
    unsigned char *manifest,
    size_t manifest_size,
    unsigned char *rootfs,
    size_t rootfs_size)
{
    memset(components, 0, sizeof(*components));
    components->source.version = PRODUCTION_INSTALLER_SOURCE_VERSION;
    components->source.boot_payload = payload;
    components->source.boot_payload_size = payload_size;
    components->source.limine_config = config;
    components->source.limine_config_size = config_size;
    components->source.kernel = kernel;
    components->source.kernel_size = kernel_size;
    components->media.version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    components->media.payload = payload;
    components->media.payload_size = payload_size;
    components->media.config = config;
    components->media.config_size = config_size;
    components->media.kernel = kernel;
    components->media.kernel_size = kernel_size;
    components->media.loader = loader;
    components->media.loader_size = loader_size;
    components->media.manifest = manifest;
    components->media.manifest_size = manifest_size;
    components->media.rootfs = rootfs;
    components->media.rootfs_size = rootfs_size;
}

/* Build the set the boot stage stages, in its declared order. */
static size_t disk_boot_set(
    const struct disk_components *components,
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U])
{
    const struct production_installer_boot_media *media =
        &components->media;
    size_t index;

    memset(set, 0, (PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U) *
                       sizeof(set[0U]));
    set[0U].path = PRODUCTION_INSTALLER_BOOT_PAYLOAD_PATH;
    set[0U].data = media->payload;
    set[0U].size = media->payload_size;
    set[1U].path = PRODUCTION_INSTALLER_BOOT_CONFIG_PATH;
    set[1U].data = media->config;
    set[1U].size = media->config_size;
    set[2U].path = PRODUCTION_INSTALLER_BOOT_KERNEL_PATH;
    set[2U].data = media->kernel;
    set[2U].size = media->kernel_size;
    set[3U].path = PRODUCTION_INSTALLER_BOOT_LOADER_PATH;
    set[3U].data = media->loader;
    set[3U].size = media->loader_size;
    set[4U].path = PRODUCTION_INSTALLER_BOOT_MANIFEST_PATH;
    set[4U].data = media->manifest;
    set[4U].size = media->manifest_size;
    index = PRODUCTION_INSTALLER_BOOT_SET_COUNT;
    if (media->rootfs != NULL && media->rootfs_size != 0U) {
        set[index].path = PRODUCTION_INSTALLER_BOOT_ROOTFS_PATH;
        set[index].data = media->rootfs;
        set[index].size = media->rootfs_size;
        ++index;
    }
    if (media->menu != NULL && media->menu_size != 0U) {
        set[index].path = PRODUCTION_INSTALLER_BOOT_MENU_PATH;
        set[index].data = media->menu;
        set[index].size = media->menu_size;
        ++index;
    }
    return index;
}

/*
 * The whole production install: evidence, sealed media, hardware, contracted
 * plan, preview, confirmation, layout, persistent state, installed boot set.
 */
static enum production_installer_status disk_install(
    struct disk_components *components,
    uint64_t media_lba,
    uint64_t esp_lba,
    const char *identity,
    struct production_installer_boot_outcome *outcome,
    uint64_t *write_calls,
    bool *esp_too_small)
{
    struct production_installer installer;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct appliance_state_layout state_layout;
    struct appliance_state_loader loader;
    struct production_installer_boot_media media = components->media;
    struct production_installer_boot_esp boot_esp;
    struct block_device device = disk_device();
    enum production_installer_status status;
    uint32_t role_index;

    *esp_too_small = false;
    disk_report(&report, media_lba, identity);
        if (production_installer_init(&installer) !=
            PRODUCTION_INSTALLER_OK ||
        production_installer_prepare(
            &installer, &report,
            PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK, 1U,
            identity, strlen(identity),
            &components->source, true) != PRODUCTION_INSTALLER_OK) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
        if (!disk_seal_media(&installer, &report, &components->source)) {
        fprintf(stderr, "install: media failed media_status=%s\n",
                installer_media_verification_status_string(
                    installer.media_status));
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    if (production_installer_build_plan(&installer) !=
        PRODUCTION_INSTALLER_OK) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (esp_lba == 0U) {
        enum production_installer_status layout_status =
            production_installer_plan_layout(
                &installer, &report, &plan, NULL);
        if (layout_status != PRODUCTION_INSTALLER_OK ||
            installer_layout_confirm(&plan) != INSTALLER_LAYOUT_OK) {
            return PRODUCTION_INSTALLER_BAD_STATE;
        }
    } else {
        struct installer_layout_partition requests[4U];

        memset(requests, 0, sizeof(requests));
        requests[0U].present = true;
        requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
        requests[0U].length_lba = esp_lba;
        requests[1U].present = true;
        requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
        requests[1U].length_lba =
            INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA;
        requests[2U].present = true;
        requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
        requests[2U].length_lba =
            INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA;
        requests[3U].present = true;
        requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
        requests[3U].length_lba =
            INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA;
        if (installer_layout_plan_whole_disk(
                &report, 1U, requests, 4U, &plan) !=
                INSTALLER_LAYOUT_OK ||
            installer_layout_confirm(&plan) != INSTALLER_LAYOUT_OK) {
            return PRODUCTION_INSTALLER_BAD_STATE;
        }
    }
    {
        enum production_installer_status preview_status =
            production_installer_publish_preview(&installer);
        enum production_installer_status confirm_status =
            production_installer_confirm(&installer);

        fprintf(stderr, "install: preview=%s confirm=%s\n",
                production_installer_status_string(preview_status),
                production_installer_status_string(confirm_status));
        if (preview_status != PRODUCTION_INSTALLER_OK ||
            confirm_status != PRODUCTION_INSTALLER_OK) {
            return PRODUCTION_INSTALLER_BAD_STATE;
        }
    }
    /*
     * The pre-write gate runs before the layout exists on the disk, so an ESP
     * that cannot hold the installed boot set is refused with no writes at
     * all - not after a partial install.  ITEM-398: the ESP is handed over
     * explicitly - this fixture is a whole-disk install, so it is the EFI
     * system role the plan carries and the writer owns the volume.
     */
    memset(&boot_esp, 0, sizeof(boot_esp));
    for (role_index = 0U;
         role_index < INSTALLER_LAYOUT_PARTITION_CAPACITY; ++role_index) {
        if (plan.partitions[role_index].present &&
            plan.partitions[role_index].role ==
                INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            boot_esp.present = true;
            boot_esp.reused = false;
            boot_esp.first_lba = plan.partitions[role_index].first_lba;
            boot_esp.length_lba = plan.partitions[role_index].length_lba;
            break;
        }
    }
    status = production_installer_boot_stage_requirements(
        &installer, &boot_esp, &media, outcome);
    if (status != PRODUCTION_INSTALLER_OK) {
        fprintf(stderr, "install: gate refused %s required=%llu esp=%llu\n",
                production_installer_status_string(status),
                (unsigned long long)outcome->required_sectors,
                (unsigned long long)outcome->esp_length_lba);
        *esp_too_small =
            status == PRODUCTION_INSTALLER_ESP_TOO_SMALL;
        (void)production_installer_boot_stage_publish(outcome, status);
        *write_calls = disk_state.write_calls;
        return status;
    }
    memset(&snapshot, 0, sizeof(snapshot));
    {
        enum production_installer_status layout_status =
            production_installer_execute_layout(
                &installer, &device, &plan, &snapshot);
        if (layout_status != PRODUCTION_INSTALLER_OK) {
            fprintf(stderr, "install: execute refused %s\n",
                    production_installer_status_string(layout_status));
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
    }
    memset(&loader, 0, sizeof(loader));
    if (appliance_state_layout_default(&state_layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    {
        /* ITEM-502: the install erases the previous state before it writes. */
        struct production_installer_state_erase_evidence erase_evidence;
        enum production_installer_status erase_status;

        memset(&erase_evidence, 0, sizeof(erase_evidence));
        erase_status = production_installer_state_erase(
            &installer, &device, &state_layout, &plan, &loader,
            &erase_evidence);
        if (erase_status != PRODUCTION_INSTALLER_OK ||
            !erase_evidence.verify_clean) {
            fprintf(stderr, "install: state erase refused %s\n",
                    production_installer_status_string(erase_status));
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
    }
    {
        enum production_installer_status state_status =
            production_installer_state_initialize(
                &installer, &device, &state_layout, &loader);
        if (state_status != PRODUCTION_INSTALLER_OK) {
            fprintf(stderr, "install: state refused %s\n",
                    production_installer_status_string(state_status));
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
    }
    {
        struct efi_boot_environment environment;

        memset(&environment, 0, sizeof(environment));
        if (efi_boot_environment_init(
                &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
                NULL, disk_variable_read, disk_variable_write) !=
            EFI_BOOT_ENVIRONMENT_OK) {
            return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
        }
        status = production_installer_boot_stage_run(
            &installer, &environment, &boot_esp, &media, outcome);
        fprintf(stderr, "install: stage=%s staged=%d verified=%d mech=%s\n",
                production_installer_status_string(status),
                (int)outcome->payload_staged,
                (int)outcome->payload_verified,
                production_installer_boot_mechanism_string(
                    outcome->mechanism));
    }
    *write_calls = disk_state.write_calls;
    return status;
}

static bool disk_verify_staged(
    const struct disk_components *components,
    uint64_t media_lba,
    uint64_t *esp_first_lba,
    uint64_t *esp_length_lba,
    unsigned char sealed_digest[SHA256_DIGEST_SIZE],
    uint64_t *sealed_length)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_contract_summary summary;
    struct block_device device = disk_device();
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U];
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    size_t config_length = 0U;
    size_t set_count = 0U;
    size_t index;

    disk_report(&report, media_lba, "installed-boot-disk");
    if (installer_layout_contract_plan_clean(
            &report, 1U, &plan, &summary) !=
        INSTALLER_LAYOUT_CONTRACT_OK) {
        return false;
    }
    memset(&environment, 0, sizeof(environment));
    if (efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
            NULL, disk_variable_read, disk_variable_write) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return false;
    }
    for (index = 0U; index < summary.role_count; ++index) {
        if (summary.roles[index].role ==
            INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            *esp_first_lba = summary.roles[index].first_lba;
            *esp_length_lba = summary.roles[index].length_lba;
        }
    }
    if (*esp_length_lba == 0U) {
        return false;
    }
    memset(&esp, 0, sizeof(esp));
    if (efi_fat32_esp_bind(
            &environment, *esp_first_lba, *esp_length_lba, &esp) !=
        EFI_FAT32_ESP_OK) {
        return false;
    }
    set_count = disk_boot_set(components, set);
    if (efi_fat32_esp_verify_payload_set(
            &esp, set, set_count, 3U,
            sealed_digest, sealed_length) != EFI_FAT32_ESP_OK) {
        return false;
    }
    if (efi_fat32_esp_read_file(
            &esp, PRODUCTION_INSTALLER_BOOT_CONFIG_PATH, config,
            sizeof(config), &config_length) != EFI_FAT32_ESP_OK ||
        config_length == 0U) {
        return false;
    }
    {
        static const char reference[] =
            PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE;
        const size_t reference_length = sizeof(reference) - 1U;
        size_t start;

        for (start = 0U;
             start + reference_length <= config_length; ++start) {
            if (memcmp(config + start, reference,
                       reference_length) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void disk_print_record(void)
{
    if (disk_record_length != 0U) {
        fputs(disk_record, stdout);
        if (disk_record[disk_record_length - 1U] != '\n') {
            fputc('\n', stdout);
        }
    }
}

/* ------------------------------------------------------------- host proof -- */

static void disk_proof_fixture(
    unsigned char *config,
    size_t *config_size,
    unsigned char *kernel,
    size_t *kernel_size)
{
    static const char text[] =
        "timeout: 3\n"
        "\n"
        "/HFOC OS 0.2\n"
        "    protocol: limine\n"
        "    path: boot():/boot/hfoc-kernel.elf\n"
        "module_path: boot():/boot/hfoc-boot.man\n"
        "module_path: boot():/boot/limine/limine.conf\n";
    size_t index;

    memcpy(config, text, sizeof(text) - 1U);
    *config_size = sizeof(text) - 1U;
    for (index = 0U; index < *kernel_size; ++index) {
        kernel[index] = (unsigned char)(
            (index * 13U + (index >> 7U) * 5U) & 0xFFU);
    }
}

static void disk_proof(void)
{
    static unsigned char payload[1024U];
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    static unsigned char kernel[DISK_PROOF_KERNEL_BYTES];
    static unsigned char loader[4096U];
    static unsigned char manifest[288U];
    static unsigned char rootfs[128U];
    struct disk_components components;
    struct production_installer_boot_outcome outcome;
    unsigned char sealed_digest[SHA256_DIGEST_SIZE];
    unsigned char expected_digest[SHA256_DIGEST_SIZE];
    struct installer_media_source media_source;
    uint64_t sealed_length = 0U;
    uint64_t expected_length = 0U;
    uint64_t esp_first_lba = 0U;
    uint64_t esp_length_lba = 0U;
    uint64_t write_calls = 0U;
    size_t config_size = 0U;
    size_t kernel_size = sizeof(kernel);
    bool esp_too_small = false;
    char path[] = "/tmp/hfoc-installed-boot-XXXXXX";
    int fd;
    size_t index;

    for (index = 0U; index < sizeof(payload); ++index) {
        payload[index] = (unsigned char)(index & 0xFFU);
    }
    for (index = 0U; index < sizeof(loader); ++index) {
        loader[index] = (unsigned char)(0x5AU ^ (index & 0xFFU));
    }
    memset(manifest, 0, sizeof(manifest));
    memcpy(manifest, "HFOCBOOT", 8U);
    for (index = 0U; index < sizeof(rootfs); ++index) {
        rootfs[index] = (unsigned char)(index * 3U + 1U);
    }
    disk_proof_fixture(config, &config_size, kernel, &kernel_size);
    disk_components_init(&components, payload, sizeof(payload), config,
                         config_size, kernel, kernel_size, loader,
                         sizeof(loader), manifest, sizeof(manifest),
                         rootfs, sizeof(rootfs));

    fd = mkstemp(path);
    CHECK(fd >= 0, "the proof image is created");
    if (fd >= 0) {
        (void)close(fd);
        (void)unlink(path);
    }
    CHECK(disk_open(path, DISK_DEFAULT_LBA, true),
          "the proof image opens as a block device");
    memset(&outcome, 0, sizeof(outcome));
    CHECK(disk_install(&components, DISK_DEFAULT_LBA, 0U,
                       "installed-boot-disk", &outcome, &write_calls,
                       &esp_too_small) == PRODUCTION_INSTALLER_OK,
          "the production install completes on the file-backed disk");
    CHECK(outcome.payload_staged && outcome.payload_verified &&
              outcome.mechanism ==
                  PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH,
          "the installed boot set is staged and the removable-media "
          "mechanism is published");
    CHECK(outcome.payload_count ==
              PRODUCTION_INSTALLER_BOOT_SET_COUNT + 1U,
          "the initramfs is one more staged component than the sealed five");
    CHECK(strstr(disk_record,
                 "[PRODUCTION-INSTALL] boot-entry state=AVAILABLE") ==
              disk_record,
          "the install record publishes the available boot entry");
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = payload;
    media_source.boot_payload_size = sizeof(payload);
    media_source.limine_config = config;
    media_source.limine_config_size = config_size;
    media_source.kernel = kernel;
    media_source.kernel_size = kernel_size;
    CHECK(installer_media_source_digest(
              &media_source, expected_digest, &expected_length) ==
              INSTALLER_MEDIA_OK,
          "the sealed source digest is recomputed");
    CHECK(disk_verify_staged(&components, DISK_DEFAULT_LBA,
                             &esp_first_lba, &esp_length_lba,
                             sealed_digest, &sealed_length),
          "the staged set re-verifies against the declared components");
    CHECK(sealed_length == expected_length &&
              memcmp(sealed_digest, expected_digest,
                     SHA256_DIGEST_SIZE) == 0,
          "the staged set matches the sealed media source digest");
    CHECK(esp_length_lba ==
              INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA,
          "the installed ESP is the declared role size");
    disk_close();

    /*
     * The refusal path: the same components with an ESP that cannot hold the
     * set must be refused before the first write.
     */
    CHECK(disk_open(path, DISK_DEFAULT_LBA, true),
          "the refusal image opens as a block device");
    disk_state.write_calls = 0U;
    memset(&outcome, 0, sizeof(outcome));
    CHECK(disk_install(&components, DISK_DEFAULT_LBA,
                       DISK_REFUSAL_ESP_LBA, "installed-boot-disk",
                       &outcome, &write_calls, &esp_too_small) ==
              PRODUCTION_INSTALLER_ESP_TOO_SMALL &&
              esp_too_small,
          "an ESP too small for the installed boot set is refused");
    CHECK(write_calls == 0U,
          "the refusal happens before the first write of the disk");
    CHECK(strstr(disk_record,
                 "state=UNAVAILABLE "
                 "reason=PRODUCTION_INSTALLER_ESP_TOO_SMALL") != NULL,
          "the refusal is published with its own reason");
    disk_close();
    (void)unlink(path);
}

/* ------------------------------------------------------------ tool modes -- */

static const char *disk_argument(
    int argc,
    char **argv,
    const char *name)
{
    int index;

    for (index = 2; index + 1 < argc; ++index) {
        if (strcmp(argv[index], name) == 0) {
            return argv[index + 1];
        }
    }
    return NULL;
}

static bool disk_argument_u64(
    int argc,
    char **argv,
    const char *name,
    uint64_t *value)
{
    const char *text = disk_argument(argc, argv, name);
    char *end = NULL;
    unsigned long long parsed;

    if (text == NULL) {
        return false;
    }
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == NULL || *end != '\0' || parsed == 0ULL) {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static int disk_run_build(int argc, char **argv, bool refusal)
{
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    static unsigned char manifest[PRODUCTION_INSTALLER_BOOT_MANIFEST_MAX];
    static unsigned char loader[PRODUCTION_INSTALLER_BOOT_LOADER_MAX];
    static unsigned char payload[PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX];
    static unsigned char rootfs[PRODUCTION_INSTALLER_BOOT_ROOTFS_MAX];
    struct disk_components components;
    struct production_installer_boot_outcome outcome;
    unsigned char *kernel = NULL;
    const char *config_path = disk_argument(argc, argv, "--config");
    const char *manifest_path = disk_argument(argc, argv, "--manifest");
    const char *loader_path = disk_argument(argc, argv, "--loader");
    const char *payload_path = disk_argument(argc, argv, "--payload");
    const char *kernel_path = disk_argument(argc, argv, "--kernel");
    const char *rootfs_path = disk_argument(argc, argv, "--rootfs");
    const char *disk_path = disk_argument(argc, argv, "--disk");
    const char *identity = disk_argument(argc, argv, "--identity");
    uint64_t media_lba = DISK_DEFAULT_LBA;
    uint64_t esp_lba = 0U;
    uint64_t write_calls = 0U;
    size_t config_size = 0U;
    size_t manifest_size = 0U;
    size_t loader_size = 0U;
    size_t payload_size = 0U;
    size_t rootfs_size = 0U;
    size_t kernel_size = 0U;
    bool esp_too_small = false;
    enum production_installer_status status;

    if (identity == NULL) {
        identity = "installed-boot-disk";
    }
    if (disk_path == NULL || config_path == NULL ||
        manifest_path == NULL || loader_path == NULL ||
        payload_path == NULL || kernel_path == NULL) {
        fprintf(stderr,
                "%s: --disk --config --manifest --loader --payload "
                "--kernel are required\n",
                refusal ? "refuse" : "build");
        return 2;
    }
    if (disk_argument(argc, argv, "--media-lba") != NULL &&
        !disk_argument_u64(argc, argv, "--media-lba", &media_lba)) {
        fprintf(stderr, "invalid --media-lba\n");
        return 2;
    }
    if (disk_argument(argc, argv, "--esp-lba") != NULL &&
        !disk_argument_u64(argc, argv, "--esp-lba", &esp_lba)) {
        fprintf(stderr, "invalid --esp-lba\n");
        return 2;
    }
    if (!disk_read_file(config_path, config, sizeof(config),
                        &config_size) ||
        !disk_read_file(manifest_path, manifest, sizeof(manifest),
                        &manifest_size) ||
        !disk_read_file(loader_path, loader, sizeof(loader),
                        &loader_size) ||
        !disk_read_file(payload_path, payload, sizeof(payload),
                        &payload_size) ||
        !disk_read_alloc(kernel_path, &kernel, &kernel_size,
                         EFI_FAT32_ESP_FILE_MAX) ||
        (rootfs_path != NULL &&
         !disk_read_file(rootfs_path, rootfs, sizeof(rootfs),
                         &rootfs_size))) {
        free(kernel);
        return 1;
    }
    disk_components_init(&components, payload, payload_size, config,
                         config_size, kernel, kernel_size, loader,
                         loader_size, manifest, manifest_size,
                         rootfs_path != NULL ? rootfs : NULL, rootfs_size);
    if (!disk_open(disk_path, media_lba, true)) {
        free(kernel);
        return 1;
    }
    disk_state.write_calls = 0U;
    memset(&outcome, 0, sizeof(outcome));
    status = disk_install(&components, media_lba, esp_lba, identity,
                          &outcome, &write_calls, &esp_too_small);
    disk_print_record();
    if (refusal) {
        printf("[INSTALLED-DISK] refuse reason=%s writes=%llu "
               "result=%s\n",
               production_installer_status_string(status) != NULL
                   ? production_installer_status_string(status)
                   : "PRODUCTION_INSTALLER_UNKNOWN",
               (unsigned long long)write_calls,
               (status == PRODUCTION_INSTALLER_ESP_TOO_SMALL &&
                write_calls == 0U)
                   ? "OK"
                   : "FAILED");
        disk_close();
        free(kernel);
        return (status == PRODUCTION_INSTALLER_ESP_TOO_SMALL &&
                write_calls == 0U)
                   ? 0
                   : 1;
    }
    printf("[INSTALLED-DISK] build media_lba=%llu esp_first_lba=%llu "
           "esp_lba=%llu required_sectors=%llu mechanism=%s "
           "staged=%d verified=%d writes=%llu result=%s\n",
           (unsigned long long)media_lba,
           (unsigned long long)outcome.esp_first_lba,
           (unsigned long long)outcome.esp_length_lba,
           (unsigned long long)outcome.required_sectors,
           production_installer_boot_mechanism_string(
               outcome.mechanism),
           (int)outcome.payload_staged, (int)outcome.payload_verified,
           (unsigned long long)write_calls,
           status == PRODUCTION_INSTALLER_OK ? "OK" : "FAILED");
    disk_close();
    free(kernel);
    return status == PRODUCTION_INSTALLER_OK ? 0 : 1;
}

static int disk_run_verify(int argc, char **argv)
{
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    static unsigned char manifest[PRODUCTION_INSTALLER_BOOT_MANIFEST_MAX];
    static unsigned char loader[PRODUCTION_INSTALLER_BOOT_LOADER_MAX];
    static unsigned char payload[PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX];
    static unsigned char rootfs[PRODUCTION_INSTALLER_BOOT_ROOTFS_MAX];
    struct disk_components components;
    struct installer_media_source media_source;
    unsigned char sealed_digest[SHA256_DIGEST_SIZE];
    unsigned char expected_digest[SHA256_DIGEST_SIZE];
    unsigned char *kernel = NULL;
    const char *config_path = disk_argument(argc, argv, "--config");
    const char *manifest_path = disk_argument(argc, argv, "--manifest");
    const char *loader_path = disk_argument(argc, argv, "--loader");
    const char *payload_path = disk_argument(argc, argv, "--payload");
    const char *kernel_path = disk_argument(argc, argv, "--kernel");
    const char *rootfs_path = disk_argument(argc, argv, "--rootfs");
    const char *disk_path = disk_argument(argc, argv, "--disk");
    uint64_t media_lba = DISK_DEFAULT_LBA;
    uint64_t esp_first_lba = 0U;
    uint64_t esp_length_lba = 0U;
    uint64_t sealed_length = 0U;
    uint64_t expected_length = 0U;
    size_t config_size = 0U;
    size_t manifest_size = 0U;
    size_t loader_size = 0U;
    size_t payload_size = 0U;
    size_t rootfs_size = 0U;
    size_t kernel_size = 0U;
    bool verified;

    if (disk_path == NULL || config_path == NULL ||
        manifest_path == NULL || loader_path == NULL ||
        payload_path == NULL || kernel_path == NULL) {
        fprintf(stderr, "verify: component paths are required\n");
        return 2;
    }
    if (disk_argument(argc, argv, "--media-lba") != NULL &&
        !disk_argument_u64(argc, argv, "--media-lba", &media_lba)) {
        return 2;
    }
    if (!disk_read_file(config_path, config, sizeof(config),
                        &config_size) ||
        !disk_read_file(manifest_path, manifest, sizeof(manifest),
                        &manifest_size) ||
        !disk_read_file(loader_path, loader, sizeof(loader),
                        &loader_size) ||
        !disk_read_file(payload_path, payload, sizeof(payload),
                        &payload_size) ||
        !disk_read_alloc(kernel_path, &kernel, &kernel_size,
                         EFI_FAT32_ESP_FILE_MAX) ||
        (rootfs_path != NULL &&
         !disk_read_file(rootfs_path, rootfs, sizeof(rootfs),
                         &rootfs_size))) {
        free(kernel);
        return 1;
    }
    disk_components_init(&components, payload, payload_size, config,
                         config_size, kernel, kernel_size, loader,
                         loader_size, manifest, manifest_size,
                         rootfs_path != NULL ? rootfs : NULL, rootfs_size);
    if (!disk_open(disk_path, media_lba, false)) {
        free(kernel);
        return 1;
    }
    memset(&media_source, 0, sizeof(media_source));
    media_source.boot_payload = payload;
    media_source.boot_payload_size = payload_size;
    media_source.limine_config = config;
    media_source.limine_config_size = config_size;
    media_source.kernel = kernel;
    media_source.kernel_size = kernel_size;
    verified = installer_media_source_digest(
                   &media_source, expected_digest, &expected_length) ==
                   INSTALLER_MEDIA_OK &&
               disk_verify_staged(&components, media_lba,
                                  &esp_first_lba, &esp_length_lba,
                                  sealed_digest, &sealed_length) &&
               sealed_length == expected_length &&
               memcmp(sealed_digest, expected_digest,
                      SHA256_DIGEST_SIZE) == 0;
    printf("[INSTALLED-DISK] verify esp_first_lba=%llu esp_lba=%llu "
           "sealed_bytes=%llu digest_match=%d result=%s\n",
           (unsigned long long)esp_first_lba,
           (unsigned long long)esp_length_lba,
           (unsigned long long)sealed_length,
           (int)(sealed_length == expected_length &&
                 memcmp(sealed_digest, expected_digest,
                        SHA256_DIGEST_SIZE) == 0),
           verified ? "OK" : "FAILED");
    disk_close();
    free(kernel);
    return verified ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc <= 1) {
        disk_proof();
        if (disk_failures != 0) {
            fprintf(stderr,
                    "Installed boot disk host tests: %d FAILURES\n",
                    disk_failures);
            return 1;
        }
        puts("Installed boot disk host tests: ALL OK");
        return 0;
    }
    if (strcmp(argv[1], "build") == 0) {
        return disk_run_build(argc, argv, false);
    }
    if (strcmp(argv[1], "refuse") == 0) {
        return disk_run_build(argc, argv, true);
    }
    if (strcmp(argv[1], "verify") == 0) {
        return disk_run_verify(argc, argv);
    }
    fprintf(stderr, "unknown mode: %s\n", argv[1]);
    return 2;
}

#endif
