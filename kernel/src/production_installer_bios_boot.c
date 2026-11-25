/*
 * ITEM-517 step 2: the BIOS boot path of the installed disk.
 *
 * The UEFI boot stage (`production_installer_boot_stage.c`) stages a payload
 * set into an ESP.  This module is the other half of requirement 2: it writes
 * the BIOS boot chain of the installed disk exactly the way the measured
 * Limine 12.3.2 `bios-install` contract writes it (ITEM-516 preparation,
 * section 1(g)), and verifies every byte of it by read-back before the install
 * is reported as bootable.
 *
 *   * the payload set (the generated menu Limine reads, the sealed chain
 *     configuration the menu names as a module, the kernel, the sealed boot
 *     manifest and `\boot\limine\limine-bios.sys`) goes to the FAT boot
 *     volume, which this install owns and therefore formats;
 *   * stage1 (the first 512 bytes of the pinned 22,516-byte stage image) is
 *     written at LBA0, with the little-endian u64 at byte offset 0x1a4 patched
 *     to the stage2 byte offset (`bios_boot_first_lba * 512`) and the original
 *     bytes [218:224) and [440:510) restored, so the protective MBR entry and
 *     with it the GPT survive;
 *   * the restored table is then given the active flag `limine bios-install`
 *     itself gives an MBR disk whose table has none, because a firmware that
 *     consults the flag skips a disk without it before it ever runs stage1;
 *   * stage2 (the remaining 22,004 bytes) is written verbatim at
 *     `bios_boot_first_lba * 512`; the trailing 12 bytes of its partial last
 *     sector are preserved by a read-modify-write, so the stage writer changes
 *     exactly 44 sectors and nothing else on the disk;
 *   * the read-back verifies stage1 against the recipe, stage2 against the
 *     blob, the staged configuration against the source and its own kernel
 *     reference, `limine-bios.sys` against the pinned length and digest, and
 *     the primary and backup GPT CRCs with the table reader the preserving
 *     writer uses.
 *
 * The stage image is the loader stage1 in LBA0, so the *mechanism* here is
 * `BIOS_MBR`; the record is published on the same `boot-entry` vocabulary the
 * UEFI path uses, with the BIOS configuration path
 * (`\boot\limine\limine.conf`) in its `config=` field.
 *
 * The blobs themselves are installer-only (`HFOC_PRODUCTION_INSTALL_MEDIA`):
 * the product kernel carries neither `limine-bios.sys` nor the stage image,
 * and `production_installer_bios_blob()` returns an empty blob there, which
 * this module refuses with BIOS_STAGE_UNAVAILABLE before the first write.
 */

#ifdef HFOC_PRODUCTION_INSTALL_MEDIA

#include "production_installer_bios_boot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "console.h"
#include "efi_fat32_esp.h"
#include "installer_gpt_preserve.h"
#include "production_installer_esp.h"
#include "serial.h"

#define PRODUCTION_BIOS_VOLUME_SECTORS_PER_CLUSTER 1U
/*
 * ITEM-517: `blob_digest=` is the whole pinned-stage SHA-256, as 64 hex digits.
 * The record carried the first 16 bytes (32 digits) while the proof requires
 * the full digest; the record stays well inside its 1024-byte buffer.
 */
#define PRODUCTION_BIOS_HEX_DIGEST SHA256_DIGEST_SIZE
/*
 * The stage2 read-back covers 43 sectors: the 22,004 stage bytes plus the 12
 * trailing bytes of the partial last sector, which the writer must leave as it
 * found them.
 */
#define PRODUCTION_BIOS_STAGE2_READ_SIZE 22016U
#define PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET \
    (PRODUCTION_INSTALLER_BIOS_STAGE2_FULL_SECTORS * \
     EFI_FAT32_ESP_SECTOR_SIZE)

/* The SHA-256 of the pinned Limine 12.3.2 release (measured, section 1(g)). */
static const unsigned char production_bios_expected_stage_digest[
    SHA256_DIGEST_SIZE] = {
    0x83U, 0xA2U, 0x05U, 0xABU, 0x84U, 0xC1U, 0x88U, 0x8AU,
    0x67U, 0xC1U, 0xB5U, 0xC1U, 0xA8U, 0xFDU, 0xC6U, 0xBAU,
    0xCEU, 0x9AU, 0x61U, 0xFCU, 0x45U, 0x66U, 0x08U, 0x2EU,
    0x26U, 0x63U, 0x82U, 0x26U, 0x66U, 0x74U, 0xE5U, 0x4AU,
};

static const unsigned char production_bios_expected_limine_digest[
    SHA256_DIGEST_SIZE] = {
    0x7DU, 0x3AU, 0xD7U, 0x41U, 0x8CU, 0x1AU, 0x95U, 0x4BU,
    0x77U, 0x03U, 0xB8U, 0xBFU, 0x86U, 0x54U, 0xBFU, 0x9FU,
    0x11U, 0x93U, 0xB6U, 0x2FU, 0xE8U, 0x8FU, 0x44U, 0x17U,
    0x79U, 0xD5U, 0xFEU, 0x2BU, 0x06U, 0x3FU, 0xBDU, 0xB1U,
};

static void production_bios_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] =
            (unsigned char)((value >> (8U * index)) & 0xFFU);
    }
}

/*
 * Whether the restored partition table already declares a bootable entry.
 * Only the four 16-byte entries are inspected: byte 0 of each is the status
 * flag, and a table that carries 0x80 anywhere needs no help from us.
 */
static bool production_bios_table_has_active(
    const unsigned char *sector)
{
    size_t index;

    for (index = 0U;
         index < PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_COUNT; ++index) {
        if (sector[PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET +
                   index * PRODUCTION_INSTALLER_BIOS_MBR_ENTRY_SIZE] ==
            PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG) {
            return true;
        }
    }
    return false;
}

/*
 * The two trampolines and the three redirects of the register-preservation
 * patch (see the header).  The bytes are the measured recipe: each redirect is
 * an inline jump into the stage's zero padding, each trampoline performs the
 * BIOS call it replaced with ECX saved and restored, and the tail at 0xc6 makes
 * the same carry test the original `jc` at 0x8b made before returning.
 */
static const unsigned char production_bios_ecx_read_check_before[4] = {
    0x0FU, 0x82U, 0xDFU, 0x00U,
};
static const unsigned char production_bios_ecx_read_check_after[4] = {
    0xE9U, 0x38U, 0x00U, 0x90U,
};
static const unsigned char production_bios_ecx_params_before[6] = {
    0xC7U, 0x04U, 0x1EU, 0x00U, 0xCDU, 0x13U,
};
static const unsigned char production_bios_ecx_params_after[5] = {
    0xE9U, 0xAEU, 0xFFU, 0x90U, 0x90U,
};
static const unsigned char production_bios_ecx_read_before[5] = {
    0xB4U, 0x42U, 0xF8U, 0xCDU, 0x13U,
};
static const unsigned char production_bios_ecx_read_after[5] = {
    0xE9U, 0x7DU, 0xFFU, 0x90U, 0x90U,
};
static const unsigned char production_bios_ecx_trampolines[
    PRODUCTION_INSTALLER_BIOS_ECX_TRAMPOLINES_SIZE] = {
    /* 0xad: the AH=48h parameters call, ECX kept across it. */
    0xC7U, 0x04U, 0x1EU, 0x00U, /* mov word [si],30 */
    0x66U, 0x51U,               /* push ecx         */
    0xCDU, 0x13U,               /* int 13h          */
    0x66U, 0x59U,               /* pop ecx          */
    0xE9U, 0x48U, 0x00U,        /* jmp 0x102        */
    /* 0xba: the AH=42h read call, ECX kept across it. */
    0xB4U, 0x42U,               /* mov ah,42h       */
    0xF8U,                      /* clc              */
    0x66U, 0x51U,               /* push ecx         */
    0xCDU, 0x13U,               /* int 13h          */
    0x66U, 0x59U,               /* pop ecx          */
    0xE9U, 0x79U, 0x00U,        /* jmp 0x13f        */
    /* 0xc6: the carry test of 0x8b, then back to 0x8f. */
    0x0FU, 0x82U, 0xA4U, 0x00U, /* jb err.4 (0x170) */
    0xE9U, 0xC2U, 0xFFU,        /* jmp 0x8f         */
};

/*
 * Apply the register-preservation patch to a copy of the pinned Stage1.  The
 * three sites and the whole padding are compared against the measured bytes
 * first, so a substituted stage image or a different release is refused instead
 * of half-patched; the caller maps a false to a verification failure.
 */
static bool production_bios_preserve_ecx(unsigned char *lba0)
{
    size_t index;

    if (lba0 == NULL) {
        return false;
    }
    if (memcmp(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_READ_CHECK,
               production_bios_ecx_read_check_before,
               sizeof(production_bios_ecx_read_check_before)) != 0 ||
        memcmp(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_PARAMS,
               production_bios_ecx_params_before,
               sizeof(production_bios_ecx_params_before)) != 0 ||
        memcmp(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_READ,
               production_bios_ecx_read_before,
               sizeof(production_bios_ecx_read_before)) != 0) {
        return false;
    }
    for (index = 0U;
         index < PRODUCTION_INSTALLER_BIOS_ECX_PADDING_SIZE; ++index) {
        if (lba0[PRODUCTION_INSTALLER_BIOS_ECX_PADDING + index] != 0U) {
            return false;
        }
    }
    memcpy(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_PADDING,
           production_bios_ecx_trampolines,
           sizeof(production_bios_ecx_trampolines));
    memcpy(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_READ_CHECK,
           production_bios_ecx_read_check_after,
           sizeof(production_bios_ecx_read_check_after));
    memcpy(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_PARAMS,
           production_bios_ecx_params_after,
           sizeof(production_bios_ecx_params_after));
    memcpy(lba0 + PRODUCTION_INSTALLER_BIOS_ECX_READ,
           production_bios_ecx_read_after,
           sizeof(production_bios_ecx_read_after));
    return true;
}

static bool production_bios_media_valid(
    const struct production_installer_boot_media *media)
{
    return media != NULL &&
           media->version == PRODUCTION_INSTALLER_BOOT_STAGE_VERSION &&
           media->config != NULL && media->config_size != 0U &&
           media->config_size <= PRODUCTION_INSTALLER_BOOT_CONFIG_MAX &&
           media->kernel != NULL && media->kernel_size != 0U &&
           media->kernel_size <= PRODUCTION_INSTALLER_BOOT_KERNEL_MAX &&
           media->manifest != NULL && media->manifest_size != 0U &&
           media->manifest_size <=
               PRODUCTION_INSTALLER_BOOT_MANIFEST_MAX &&
           /* ITEM-623: the same optional, bounded initramfs as the UEFI set. */
           (media->rootfs == NULL
                ? media->rootfs_size == 0U
                : media->rootfs_size != 0U &&
                      media->rootfs_size <=
                          PRODUCTION_INSTALLER_BOOT_ROOTFS_MAX) &&
           /*
            * The menu, when the caller offers one, is the installed
            * configuration - the same rule the UEFI boot stage applies.  A
            * complete offer carries the bytes, the entry count the record
            * publishes and the timeout the configuration declares.
            */
           (media->menu == NULL
                ? media->menu_size == 0U && media->menu_entries == 0U
                : media->menu_size != 0U &&
                      media->menu_size <=
                          PRODUCTION_INSTALLER_BOOT_CONFIG_MAX &&
                      media->menu_entries != 0U &&
                      media->menu_timeout_seconds != 0U);
}

/* The pinned blob transport, with its sizes and both digests checked. */
static bool production_bios_blob_valid(
    const struct production_installer_bios_blob *blob,
    unsigned char stage_digest[SHA256_DIGEST_SIZE],
    unsigned char limine_digest[SHA256_DIGEST_SIZE])
{
    if (blob == NULL ||
        blob->version != PRODUCTION_INSTALLER_BIOS_BOOT_VERSION ||
        blob->stage == NULL ||
        blob->stage_size != PRODUCTION_INSTALLER_BIOS_STAGE_SIZE ||
        blob->limine_bios_sys == NULL ||
        blob->limine_bios_sys_size !=
            PRODUCTION_INSTALLER_BIOS_LIMINE_SYS_SIZE) {
        return false;
    }
    if (sha256_compute(blob->stage, blob->stage_size, stage_digest) !=
            SHA256_OK ||
        sha256_compute(blob->limine_bios_sys,
                       blob->limine_bios_sys_size, limine_digest) !=
            SHA256_OK) {
        return false;
    }
    return memcmp(stage_digest, production_bios_expected_stage_digest,
                  SHA256_DIGEST_SIZE) == 0 &&
           memcmp(limine_digest, production_bios_expected_limine_digest,
                  SHA256_DIGEST_SIZE) == 0;
}

static const unsigned char *production_bios_installed_config(
    const struct production_installer_boot_media *media,
    size_t *length)
{
    if (media->menu != NULL && media->menu_size != 0U) {
        *length = media->menu_size;
        return media->menu;
    }
    *length = media->config_size;
    return media->config;
}

/*
 * The BIOS payload set, in the order the boot volume carries it: the menu
 * Limine reads, the sealed configuration the menu names as a module, the
 * kernel, the sealed boot manifest and Limine's BIOS loader.  There is no ESP
 * on this disk, so the UEFI payload and its fallback loader are not part of
 * the set; the loader Limine BIOS reads is `limine-bios.sys`.
 *
 * The menu and the chain configuration are two files on purpose: Limine reads
 * `\boot\limine\limine.conf` itself, while the kernel's boot chain receives
 * the sealed configuration as the module
 * `boot():/boot/hfoc/limine.conf` and verifies it against the signed
 * manifest.  Staging one file for both roles would hand the kernel the
 * generated menu, whose bytes the manifest does not seal.
 */
static size_t production_bios_set_size(
    const struct production_installer_boot_media *media)
{
    return media->rootfs != NULL && media->rootfs_size != 0U
               ? PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U
               : PRODUCTION_INSTALLER_BIOS_SET_COUNT;
}

static void production_bios_set_build(
    const struct production_installer_boot_media *media,
    const struct production_installer_bios_blob *blob,
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U])
{
    size_t config_length = 0U;
    const unsigned char *config =
        production_bios_installed_config(media, &config_length);

    memset(set, 0, (PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U) *
                       sizeof(set[0U]));
    set[0U].path = PRODUCTION_INSTALLER_BIOS_CONFIG_PATH;
    set[0U].data = config;
    set[0U].size = config_length;
    set[1U].path = PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH;
    set[1U].data = media->config;
    set[1U].size = media->config_size;
    set[2U].path = PRODUCTION_INSTALLER_BIOS_KERNEL_PATH;
    set[2U].data = media->kernel;
    set[2U].size = media->kernel_size;
    set[3U].path = PRODUCTION_INSTALLER_BIOS_MANIFEST_PATH;
    set[3U].data = media->manifest;
    set[3U].size = media->manifest_size;
    set[4U].path = PRODUCTION_INSTALLER_BIOS_STAGE2_PATH;
    set[4U].data = blob->limine_bios_sys;
    set[4U].size = blob->limine_bios_sys_size;
    if (media->rootfs != NULL && media->rootfs_size != 0U) {
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT].path =
            PRODUCTION_INSTALLER_BIOS_ROOTFS_PATH;
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT].data = media->rootfs;
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT].size = media->rootfs_size;
    }
}

static uint64_t production_bios_set_bytes(
    const struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U],
    size_t count)
{
    uint64_t total = 0U;
    size_t index;

    for (index = 0U; index < count;
         ++index) {
        total += (uint64_t)set[index].size;
    }
    return total;
}

/*
 * The staged configuration must locate the kernel it was staged with.  A
 * bounded search for the declared reference line, exactly as the UEFI boot
 * stage checks it, so a configuration that names a different kernel (or names
 * none) is refused instead of being reported as a bootable system.
 */
static bool production_bios_config_references_kernel(
    const unsigned char *config,
    size_t config_length)
{
    static const char reference[] =
        PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE;
    const size_t reference_length = sizeof(reference) - 1U;
    size_t start;

    if (config == NULL || config_length < reference_length) {
        return false;
    }
    for (start = 0U; start + reference_length <= config_length; ++start) {
        if (memcmp(config + start, reference, reference_length) == 0) {
            return true;
        }
    }
    return false;
}

static enum production_installer_status production_bios_map_fat(
    enum efi_fat32_esp_status status)
{
    switch (status) {
    case EFI_FAT32_ESP_OK:
        return PRODUCTION_INSTALLER_OK;
    case EFI_FAT32_ESP_CAPACITY:
        return PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL;
    case EFI_FAT32_ESP_OUT_OF_RANGE:
        return PRODUCTION_INSTALLER_CAPACITY;
    case EFI_FAT32_ESP_IO_ERROR:
    case EFI_FAT32_ESP_FLUSH_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    case EFI_FAT32_ESP_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case EFI_FAT32_ESP_SET_TOO_LARGE:
        return PRODUCTION_INSTALLER_CAPACITY;
    default:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
}

enum production_installer_status
production_installer_bios_boot_requirements(
    const struct production_installer *installer,
    const struct production_installer_bios_partition *bios_boot,
    const struct production_installer_bios_partition *boot_volume,
    const struct production_installer_boot_media *media,
    struct production_installer_bios_boot_outcome *outcome)
{
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U];
    struct efi_fat32_esp_requirement requirement;
    struct production_installer_bios_blob blob;
    enum efi_fat32_esp_status fat_status;
    size_t installed_config_size = 0U;

    if (installer == NULL || bios_boot == NULL || boot_volume == NULL ||
        media == NULL || outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    memset(outcome, 0, sizeof(*outcome));
    outcome->version = PRODUCTION_INSTALLER_BIOS_BOOT_VERSION;
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    /*
     * As in the UEFI boot stage, the gate is a planning check the flow runs
     * after the confirmation and before the layout write.
     */
    if (!installer->confirmed ||
        installer->stage == PRODUCTION_INSTALLER_STAGE_UNPREPARED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    /*
     * A UEFI install can never enter this path.  The whole-disk BIOS contract
     * publishes its own summary mode (`MODE_CLEAN_WHOLE_DISK_BIOS`), and that
     * mode - not the caller's partition extents - is what selects this module.
     */
    if (installer->layout.mode !=
        INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS) {
        return PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE;
    }
    blob = production_installer_bios_blob();
    if (!production_bios_blob_valid(
            &blob, outcome->blob_digest,
            outcome->limine_bios_sys_digest)) {
        return PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE;
    }
    if (!production_bios_media_valid(media)) {
        return PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE;
    }
    /*
     * The stage contract needs both extents.  A BIOS clean plan always carries
     * them; a caller without them has no BIOS stage to write.
     */
    if (!bios_boot->present || bios_boot->length_lba == 0U ||
        !boot_volume->present || boot_volume->length_lba == 0U) {
        return PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE;
    }
    if (bios_boot->first_lba > UINT64_MAX / EFI_FAT32_ESP_SECTOR_SIZE ||
        bios_boot->length_lba > UINT64_MAX / EFI_FAT32_ESP_SECTOR_SIZE) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    /*
     * The measured Limine minimum: 64 sectors (32 KiB).  The stage2 image is
     * 22,004 bytes, so a partition at the minimum holds it with room to spare;
     * anything below the minimum is refused by name before the first write.
     */
    if (bios_boot->length_lba <
            INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS ||
        bios_boot->length_lba * EFI_FAT32_ESP_SECTOR_SIZE <
            PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE) {
        return PRODUCTION_INSTALLER_BIOS_BOOT_PARTITION_TOO_SMALL;
    }
    production_bios_set_build(media, &blob, set);
    memset(&requirement, 0, sizeof(requirement));
    fat_status = efi_fat32_esp_payload_requirement(
        set, production_bios_set_size(media),
        PRODUCTION_BIOS_VOLUME_SECTORS_PER_CLUSTER, &requirement);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_bios_map_fat(fat_status);
    }
    if (boot_volume->length_lba < requirement.required_sectors) {
        return PRODUCTION_INSTALLER_BOOT_VOLUME_TOO_SMALL;
    }
    {
        const unsigned char *installed_config =
            production_bios_installed_config(
                media, &installed_config_size);

        if (!production_bios_config_references_kernel(
                installed_config, installed_config_size)) {
            return PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID;
        }
    }

    outcome->bios_boot_first_lba = bios_boot->first_lba;
    outcome->bios_boot_length_lba = bios_boot->length_lba;
    outcome->boot_volume_first_lba = boot_volume->first_lba;
    outcome->boot_volume_length_lba = boot_volume->length_lba;
    outcome->payload_count = (uint32_t)production_bios_set_size(media);
    outcome->payload_bytes =
        production_bios_set_bytes(set, production_bios_set_size(media));
    outcome->required_sectors = requirement.required_sectors;
    outcome->stage1_lba = 0U;
    outcome->stage2_bytes = PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE;
    outcome->stage2_offset =
        bios_boot->first_lba * EFI_FAT32_ESP_SECTOR_SIZE;
    outcome->stage2_field_offset =
        PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD_DECIMAL;
    outcome->limine_bios_sys_bytes = blob.limine_bios_sys_size;
    outcome->menu_installed =
        media->menu != NULL && media->menu_size != 0U;
    outcome->menu_entries =
        outcome->menu_installed ? media->menu_entries : 0U;
    outcome->menu_timeout_seconds = media->menu_timeout_seconds;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_bios_boot_run(
    struct production_installer *installer,
    struct block_device *device,
    const struct production_installer_bios_partition *bios_boot,
    const struct production_installer_bios_partition *boot_volume,
    const struct production_installer_boot_media *media,
    struct production_installer_bios_boot_outcome *outcome)
{
    static struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BIOS_SET_COUNT + 1U];
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    static unsigned char lba0[PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE];
    static unsigned char original[PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE];
    static unsigned char stage2_tail[EFI_FAT32_ESP_SECTOR_SIZE];
    static unsigned char stage2_read[PRODUCTION_BIOS_STAGE2_READ_SIZE];
    static struct installer_gpt_preserve_table gpt;
    struct production_installer_bios_blob blob;
    struct efi_fat32_esp esp;
    enum production_installer_status status;
    enum efi_fat32_esp_status fat_status;
    enum block_device_status io_status;
    const unsigned char *installed_config = NULL;
    size_t installed_config_size = 0U;
    size_t config_length = 0U;
    uint64_t sealed_bytes = 0U;
    uint64_t stage2_offset;
    size_t index;

    if (installer == NULL || device == NULL || bios_boot == NULL ||
        boot_volume == NULL || media == NULL || outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    status = production_installer_bios_boot_requirements(
        installer, bios_boot, boot_volume, media, outcome);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    if (installer->stage != PRODUCTION_INSTALLER_STAGE_EXECUTING) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    /*
     * Fail closed on the sealed media, exactly as the UEFI boot stage does:
     * the staged configuration and kernel are the bytes the signed source
     * declared, and without the verified source digest the disk is not
     * reported as bootable.
     */
    if (!installer->media_verified || !installer->source_digest_ready) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    blob = production_installer_bios_blob();
    if (blob.stage == NULL || blob.stage_size !=
            PRODUCTION_INSTALLER_BIOS_STAGE_SIZE) {
        return PRODUCTION_INSTALLER_BIOS_STAGE_UNAVAILABLE;
    }

    /*
     * The boot volume is a role this whole-disk BIOS install owns, so it is
     * formatted before it is bound - a stale filesystem the previous partition
     * table left in the range is never written into.  There is no EFI
     * environment here: this is a plain FAT32 volume, not an ESP.
     */
    memset(&esp, 0, sizeof(esp));
    status = production_installer_esp_bind_device(
        device, boot_volume->first_lba, boot_volume->length_lba, true,
        &esp);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    production_bios_set_build(media, &blob, set);
    fat_status = efi_fat32_esp_install_payload_set(
        &esp, set, production_bios_set_size(media));
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_bios_map_fat(fat_status);
    }
    outcome->payload_staged = true;
    fat_status = efi_fat32_esp_verify_payload_set(
        &esp, set, production_bios_set_size(media), 0U, NULL,
        &sealed_bytes);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_bios_map_fat(fat_status);
    }
    outcome->payload_verified = true;

    /*
     * The staged configuration is read back from the volume and must be the
     * configuration this run staged *and* name the staged kernel: the record
     * then describes the configuration that is on the disk, not the one the
     * media carried.
     */
    installed_config = production_bios_installed_config(
        media, &installed_config_size);
    fat_status = efi_fat32_esp_read_file(
        &esp, PRODUCTION_INSTALLER_BIOS_CONFIG_PATH, config,
        sizeof(config), &config_length);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_bios_map_fat(fat_status);
    }
    if (!production_bios_config_references_kernel(
            config, config_length) ||
        config_length != installed_config_size ||
        memcmp(config, installed_config, config_length) != 0) {
        return PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID;
    }
    /*
     * The generated menu names the sealed configuration as its module
     * (`boot():/boot/hfoc/limine.conf`), so that file - not the menu - is what
     * the kernel's boot chain receives and verifies against the signed
     * manifest.  It is read back independently here: the install must not
     * report a bootable disk whose chain configuration is missing, truncated
     * or not the bytes the sealed source declared.
     */
    config_length = 0U;
    fat_status = efi_fat32_esp_read_file(
        &esp, PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH, config,
        sizeof(config), &config_length);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_bios_map_fat(fat_status);
    }
    if (!production_bios_config_references_kernel(config, config_length) ||
        config_length != media->config_size ||
        memcmp(config, media->config, config_length) != 0) {
        return PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID;
    }
    outcome->config_verified = true;

    /*
     * The loader stage2 reads must be readable on the volume, with the pinned
     * length and the digest of the pinned release.  `verify_payload_set()`
     * above already compared every staged byte with the source; this is the
     * independent read of the one file whose absence makes the BIOS disk
     * unbootable.
     */
    {
        unsigned char digest[SHA256_DIGEST_SIZE];
        size_t length = 0U;

        fat_status = efi_fat32_esp_digest_file(
            &esp, PRODUCTION_INSTALLER_BIOS_STAGE2_PATH, digest,
            &length);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return production_bios_map_fat(fat_status);
        }
        if (length != blob.limine_bios_sys_size ||
            memcmp(digest, outcome->limine_bios_sys_digest,
                   SHA256_DIGEST_SIZE) != 0) {
            return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
        }
        outcome->limine_bios_sys_bytes = length;
    }

    /*
     * The measured stage recipe.  Save what the overwrite must not disturb
     * before writing LBA0; the restored ranges carry the protective MBR entry
     * (440..509) and the two zero bytes at 218..223 of the layout writer's
     * MBR.
     */
    stage2_offset =
        bios_boot->first_lba * EFI_FAT32_ESP_SECTOR_SIZE;
    io_status = block_device_read(
        device, 0U, original, PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE);
    if (io_status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    memcpy(lba0, blob.stage, PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE);
    production_bios_store_le64(
        lba0 + PRODUCTION_INSTALLER_BIOS_STAGE2_OFFSET_FIELD,
        stage2_offset);
    memcpy(lba0 + 218U, original + 218U, 6U);
    memcpy(lba0 + 440U, original + 440U, 70U);
    /*
     * The restored table is the layout's protective MBR, and like every
     * standard protective MBR it declares no active entry.  `limine
     * bios-install` repairs exactly that on the media it writes ("No active
     * partition found, some systems may not boot"); this writer has to do the
     * same or the installed disk is less bootable than the medium that
     * installed it.  The L875's InsydeH2O refuses such a disk without a word -
     * no stage1 output, straight to the next boot device and `PXE-E53` - and
     * boots the same chain from a stick whose MBR has the flag.
     */
    if (!production_bios_table_has_active(lba0)) {
        lba0[PRODUCTION_INSTALLER_BIOS_MBR_FIRST_ENTRY_OFFSET] =
            PRODUCTION_INSTALLER_BIOS_MBR_ACTIVE_FLAG;
    }
    /*
     * The register-preservation patch.  Without it the firmware's AHCI INT 13h
     * handler clobbers the sector counter of the stage's read loop, the loop
     * overruns and the machine reboots before stage2 runs; the same image boots
     * from a USB stick, whose INT 13h path preserves the register.  A stage
     * image whose sites or padding do not match the measured recipe is refused
     * here, before LBA0 is written.
     */
    if (!production_bios_preserve_ecx(lba0)) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    io_status = block_device_write(
        device, 0U, lba0, PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE);
    if (io_status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    outcome->stage1_written = true;
    outcome->stage_sector_writes = 1U;

    /*
     * Stage2: 42 whole sectors verbatim, then the partial 43rd sector read,
     * patched in its first 500 bytes and written back, so the 12 bytes past
     * the stage2 image stay exactly as they were.  This is what makes the
     * write footprint exactly 44 sectors.
     */
    for (index = 0U;
         index < PRODUCTION_INSTALLER_BIOS_STAGE2_FULL_SECTORS; ++index) {
        io_status = block_device_write(
            device,
            stage2_offset + (uint64_t)index * EFI_FAT32_ESP_SECTOR_SIZE,
            blob.stage + PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE +
                index * EFI_FAT32_ESP_SECTOR_SIZE,
            EFI_FAT32_ESP_SECTOR_SIZE);
        if (io_status != BLOCK_DEVICE_OK) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        outcome->stage_sector_writes += 1U;
    }
    io_status = block_device_read(
        device, stage2_offset + PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET,
        stage2_tail, EFI_FAT32_ESP_SECTOR_SIZE);
    if (io_status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    memcpy(stage2_tail,
           blob.stage + PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE +
               PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET,
           PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE -
               PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET);
    io_status = block_device_write(
        device, stage2_offset + PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET,
        stage2_tail, EFI_FAT32_ESP_SECTOR_SIZE);
    if (io_status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    outcome->stage_sector_writes += 1U;
    outcome->stage2_written = true;

    /*
     * Read-back verification of the whole stage contract: LBA0 is the recipe
     * byte for byte (including the restored ranges and the patched field), and
     * the stage2 region is the blob's second half with the partial sector's
     * tail still whatever it was.
     */
    memset(stage2_read, 0, sizeof(stage2_read));
    io_status = block_device_read(
        device, 0U, original, PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE);
    if (io_status != BLOCK_DEVICE_OK ||
        memcmp(original, lba0, PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE) !=
            0) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    outcome->stage1_verified = true;
    io_status = block_device_read(
        device, stage2_offset, stage2_read, sizeof(stage2_read));
    if (io_status != BLOCK_DEVICE_OK ||
        memcmp(stage2_read,
               blob.stage + PRODUCTION_INSTALLER_BIOS_STAGE1_SIZE,
               PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE) != 0 ||
        memcmp(stage2_read + PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE,
               stage2_tail + (PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE -
                              PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET),
               PRODUCTION_BIOS_STAGE2_PARTIAL_OFFSET +
                   EFI_FAT32_ESP_SECTOR_SIZE -
                   PRODUCTION_INSTALLER_BIOS_STAGE2_SIZE) != 0) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    /*
     * The read-back above proves stage2 byte for byte and proves the partial
     * sector's tail is the one the read-modify-write preserved; the 0x1a4
     * field of stage1 was checked with LBA0 by the recipe comparison.
     */
    outcome->stage2_verified = true;

    /*
     * The stage write is LBA0 plus a role inside the GPT.  Re-reading the
     * table proves the protective MBR, the primary and backup headers and
     * their arrays, and both CRCs survived it.
     */
    memset(&gpt, 0, sizeof(gpt));
    {
        enum installer_gpt_preserve_status gpt_status =
            installer_gpt_preserve_read(device, installer->disk_id, &gpt);

        if (gpt_status == INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE ||
            gpt_status == INSTALLER_GPT_PRESERVE_IO_ERROR) {
            return PRODUCTION_INSTALLER_IO_ERROR;
        }
        if (gpt_status != INSTALLER_GPT_PRESERVE_OK) {
            return PRODUCTION_INSTALLER_TABLE_CORRUPT;
        }
    }
    outcome->gpt_verified = true;

    outcome->mechanism = PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR;
    (void)production_installer_bios_boot_publish(
        outcome, PRODUCTION_INSTALLER_OK);
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_bios_boot_publish(
    const struct production_installer_bios_boot_outcome *outcome,
    enum production_installer_status status)
{
    char record[PRODUCTION_INSTALLER_BIOS_RECORD_LENGTH];
    size_t length = 0U;
    uint64_t serial_flags;
    size_t index;

    if (outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
#define PRODUCTION_BIOS_APPEND(text) do { \
        static const char fragment[] = text; \
        const size_t fragment_length = sizeof(fragment) - 1U; \
        if (length + fragment_length >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        memcpy(record + length, fragment, fragment_length); \
        length += fragment_length; \
    } while (0)
#define PRODUCTION_BIOS_APPEND_TEXT(value) do { \
        const char *runtime_text = (value); \
        size_t runtime_length = 0U; \
        if (runtime_text == NULL) { \
            return PRODUCTION_INSTALLER_NULL; \
        } \
        while (runtime_text[runtime_length] != '\0') { \
            ++runtime_length; \
        } \
        if (length + runtime_length >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        memcpy(record + length, runtime_text, runtime_length); \
        length += runtime_length; \
    } while (0)
#define PRODUCTION_BIOS_APPEND_U64(value) do { \
        uint64_t digits = (uint64_t)(value); \
        char digits_buffer[20U]; \
        size_t digit_count = 0U; \
        if (digits == 0U) { \
            digits_buffer[digit_count++] = '0'; \
        } \
        while (digits != 0U) { \
            digits_buffer[digit_count++] = \
                (char)('0' + (digits % 10U)); \
            digits /= 10U; \
        } \
        if (length + digit_count >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        while (digit_count > 0U) { \
            record[length++] = digits_buffer[--digit_count]; \
        } \
    } while (0)

    PRODUCTION_BIOS_APPEND("[PRODUCTION-INSTALL] boot-entry ");
    if (status == PRODUCTION_INSTALLER_OK &&
        outcome->payload_staged && outcome->payload_verified &&
        outcome->stage1_written && outcome->stage2_written &&
        outcome->stage1_verified && outcome->stage2_verified) {
        PRODUCTION_BIOS_APPEND("state=AVAILABLE mechanism=");
        PRODUCTION_BIOS_APPEND_TEXT(
            production_installer_boot_mechanism_string(
                outcome->mechanism));
        PRODUCTION_BIOS_APPEND(" path=");
        PRODUCTION_BIOS_APPEND(PRODUCTION_INSTALLER_BIOS_CONFIG_PATH);
        PRODUCTION_BIOS_APPEND(" kernel=");
        PRODUCTION_BIOS_APPEND(PRODUCTION_INSTALLER_BIOS_KERNEL_PATH);
        PRODUCTION_BIOS_APPEND(" config=");
        PRODUCTION_BIOS_APPEND(PRODUCTION_INSTALLER_BIOS_CHAIN_CONFIG_PATH);
        PRODUCTION_BIOS_APPEND(" manifest=");
        PRODUCTION_BIOS_APPEND(PRODUCTION_INSTALLER_BIOS_MANIFEST_PATH);
        PRODUCTION_BIOS_APPEND(" loader=");
        PRODUCTION_BIOS_APPEND(PRODUCTION_INSTALLER_BIOS_STAGE2_PATH);
        PRODUCTION_BIOS_APPEND(" entries=");
        PRODUCTION_BIOS_APPEND_U64(
            outcome->menu_installed ? (uint64_t)outcome->menu_entries
                                    : 1U);
        if (outcome->menu_installed) {
            PRODUCTION_BIOS_APPEND(" timeout=");
            PRODUCTION_BIOS_APPEND_U64(
                (uint64_t)outcome->menu_timeout_seconds);
            PRODUCTION_BIOS_APPEND("s");
        }
        PRODUCTION_BIOS_APPEND(" source=");
        PRODUCTION_BIOS_APPEND_TEXT(
            outcome->menu_installed ? "GENERATED" : "SEALED");
        PRODUCTION_BIOS_APPEND(" payload_set=");
        PRODUCTION_BIOS_APPEND_U64(outcome->payload_count);
        PRODUCTION_BIOS_APPEND(" payload_bytes=");
        PRODUCTION_BIOS_APPEND_U64(outcome->payload_bytes);
        PRODUCTION_BIOS_APPEND(" required_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->required_sectors);
        PRODUCTION_BIOS_APPEND(" boot_volume_first_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->boot_volume_first_lba);
        PRODUCTION_BIOS_APPEND(" boot_volume_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->boot_volume_length_lba);
        PRODUCTION_BIOS_APPEND(" bios_boot_first_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->bios_boot_first_lba);
        PRODUCTION_BIOS_APPEND(" bios_boot_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->bios_boot_length_lba);
        PRODUCTION_BIOS_APPEND(" stage1_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->stage1_lba);
        PRODUCTION_BIOS_APPEND(" stage2_first_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->bios_boot_first_lba);
        PRODUCTION_BIOS_APPEND(" stage2_bytes=");
        PRODUCTION_BIOS_APPEND_U64(outcome->stage2_bytes);
        PRODUCTION_BIOS_APPEND(" stage2_offset=");
        PRODUCTION_BIOS_APPEND_U64(outcome->stage2_offset);
        PRODUCTION_BIOS_APPEND(" stage2_field=");
        PRODUCTION_BIOS_APPEND_U64(outcome->stage2_field_offset);
        PRODUCTION_BIOS_APPEND(" stage_writes=");
        PRODUCTION_BIOS_APPEND_U64(outcome->stage_sector_writes);
        PRODUCTION_BIOS_APPEND(" limine_bios_sys_bytes=");
        PRODUCTION_BIOS_APPEND_U64(outcome->limine_bios_sys_bytes);
        PRODUCTION_BIOS_APPEND(" blob_digest=");
        for (index = 0U; index < PRODUCTION_BIOS_HEX_DIGEST; ++index) {
            static const char hex[] = "0123456789abcdef";

            if (length + 2U >= sizeof(record)) {
                return PRODUCTION_INSTALLER_CAPACITY;
            }
            record[length++] =
                hex[(outcome->blob_digest[index] >> 4U) & 0x0FU];
            record[length++] =
                hex[outcome->blob_digest[index] & 0x0FU];
        }
        PRODUCTION_BIOS_APPEND(" verify=READBACK gpt=VALID");
    } else {
        PRODUCTION_BIOS_APPEND("state=UNAVAILABLE reason=");
        PRODUCTION_BIOS_APPEND_TEXT(
            production_installer_status_string(status) != NULL
                ? production_installer_status_string(status)
                : "PRODUCTION_INSTALLER_UNKNOWN");
        PRODUCTION_BIOS_APPEND(" bios_boot_first_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->bios_boot_first_lba);
        PRODUCTION_BIOS_APPEND(" bios_boot_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->bios_boot_length_lba);
        PRODUCTION_BIOS_APPEND(" boot_volume_first_lba=");
        PRODUCTION_BIOS_APPEND_U64(outcome->boot_volume_first_lba);
        PRODUCTION_BIOS_APPEND(" boot_volume_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->boot_volume_length_lba);
        PRODUCTION_BIOS_APPEND(" required_sectors=");
        PRODUCTION_BIOS_APPEND_U64(outcome->required_sectors);
    }
    PRODUCTION_BIOS_APPEND("\n");
#undef PRODUCTION_BIOS_APPEND_U64
#undef PRODUCTION_BIOS_APPEND_TEXT
#undef PRODUCTION_BIOS_APPEND

    serial_flags = serial_sequence_begin();
    serial_write(record);
    serial_sequence_end(serial_flags);
    console_publish_journal(record, length);
    return PRODUCTION_INSTALLER_OK;
}

#else
/* Keep this production translation unit non-empty off the install medium. */
int hfoc_production_installer_bios_boot_translation_unit_marker;
#endif
