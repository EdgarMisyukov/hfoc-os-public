#include "production_installer_esp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRODUCTION_ESP_SECTOR_SIZE 512U
#define PRODUCTION_ESP_RESERVED_SECTORS 32U
#define PRODUCTION_ESP_FAT_COUNT 2U
#define PRODUCTION_ESP_ROOT_CLUSTER 2U
#define PRODUCTION_ESP_SECTORS_PER_CLUSTER 1U
#define PRODUCTION_ESP_FSINFO_SECTOR 1U
#define PRODUCTION_ESP_BACKUP_BOOT_SECTOR 6U

/*
 * ITEM-397: the FAT has to describe the volume it is written on.
 *
 * ITEM-350 formatted a fixed 32-sector FAT and a 4096-sector role.  The
 * installed system stages its kernel into the ESP, so the ESP is one
 * contracted image slot (32768 sectors) and a fixed 32-sector FAT can no
 * longer describe it: `efi_fat32_esp_bind()` refuses a volume whose FAT
 * cannot address its data region.  The size is derived here, from the volume,
 * with the same arithmetic the ESP writer checks against, and it stays bounded
 * by `EFI_FAT32_ESP_FAT_SECTORS_MAX` so a caller cannot ask for an unbounded
 * format.
 */
static uint32_t production_esp_sectors_per_fat(uint64_t block_count)
{
    uint64_t fat_sectors;
    uint64_t clusters;
    unsigned int iteration;

    clusters = block_count - PRODUCTION_ESP_RESERVED_SECTORS;
    fat_sectors =
        (2U + clusters + EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR - 1U) /
        EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR;
    for (iteration = 0U; iteration < 4U; ++iteration) {
        uint64_t needed;

        clusters = block_count - PRODUCTION_ESP_RESERVED_SECTORS -
                   (uint64_t)PRODUCTION_ESP_FAT_COUNT * fat_sectors;
        needed =
            (2U + clusters +
             EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR - 1U) /
            EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR;
        if (needed <= fat_sectors) {
            break;
        }
        fat_sectors = needed;
    }
    if (fat_sectors == 0U) {
        fat_sectors = 1U;
    }
    if (fat_sectors > EFI_FAT32_ESP_FAT_SECTORS_MAX) {
        return 0U;
    }
    return (uint32_t)fat_sectors;
}

static void production_esp_store_le16(
    unsigned char *bytes,
    uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)(value >> 8U);
}

static void production_esp_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static enum production_installer_status
production_esp_write_sector(
    struct block_device *device,
    uint64_t absolute_lba,
    const unsigned char *sector)
{
    enum block_device_status status;

    status = block_device_write(
        device, absolute_lba * PRODUCTION_ESP_SECTOR_SIZE,
        sector, PRODUCTION_ESP_SECTOR_SIZE);
    if (status != BLOCK_DEVICE_OK) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    return PRODUCTION_INSTALLER_OK;
}

static enum production_installer_status
production_esp_format(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count)
{
    unsigned char sector[PRODUCTION_ESP_SECTOR_SIZE];
    enum production_installer_status status;
    enum block_device_status io_status;
    uint64_t index;
    uint32_t sectors_per_fat;
    uint32_t fat_index;

    if (block_count <= PRODUCTION_ESP_ROOT_CLUSTER ||
        block_count > UINT32_C(0x0FFFFFFF)) {
        return PRODUCTION_INSTALLER_CAPACITY;
    }
    sectors_per_fat = production_esp_sectors_per_fat(block_count);
    if (sectors_per_fat == 0U) {
        return PRODUCTION_INSTALLER_CAPACITY;
    }
    if (first_lba > device->block_count ||
        block_count > device->block_count - first_lba) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }

    /*
     * ITEM-397: a complete FAT32 BIOS parameter block.
     *
     * ITEM-350 wrote only the fields the tree's own reader used, and left the
     * total-sector count, the media descriptor, the FSInfo pointer and the
     * filesystem type string at zero.  Firmware validates those fields before
     * it mounts a volume, so the ESP the installer produced was not mountable
     * by UEFI: OVMF enumerated the partition and then reported
     * `failed to load Boot0001 "UEFI QEMU HARDDISK" ... Not Found`, i.e. the
     * installed disk could never boot through the removable-media path.
     */
    memset(sector, 0, sizeof(sector));
    sector[0U] = 0xEBU;
    sector[1U] = 0x58U;
    sector[2U] = 0x90U;
    memcpy(sector + 3U, "HFOC    ", 8U);
    production_esp_store_le16(
        sector + 11U, PRODUCTION_ESP_SECTOR_SIZE);
    sector[13U] = PRODUCTION_ESP_SECTORS_PER_CLUSTER;
    production_esp_store_le16(
        sector + 14U, PRODUCTION_ESP_RESERVED_SECTORS);
    sector[16U] = PRODUCTION_ESP_FAT_COUNT;
    production_esp_store_le16(sector + 17U, 0U);
    production_esp_store_le16(sector + 19U, 0U);
    sector[21U] = 0xF8U;
    production_esp_store_le16(sector + 22U, 0U);
    production_esp_store_le16(sector + 24U, 32U);
    production_esp_store_le16(sector + 26U, 64U);
    production_esp_store_le32(
        sector + 28U, (uint32_t)(first_lba & 0xFFFFFFFFU));
    production_esp_store_le32(
        sector + 32U, (uint32_t)(block_count & 0xFFFFFFFFU));
    production_esp_store_le32(sector + 36U, sectors_per_fat);
    production_esp_store_le16(sector + 40U, 0U);
    production_esp_store_le16(sector + 42U, 0U);
    production_esp_store_le32(
        sector + 44U, PRODUCTION_ESP_ROOT_CLUSTER);
    production_esp_store_le16(
        sector + 48U, PRODUCTION_ESP_FSINFO_SECTOR);
    production_esp_store_le16(
        sector + 50U, PRODUCTION_ESP_BACKUP_BOOT_SECTOR);
    sector[64U] = 0x80U;
    sector[66U] = 0x29U;
    production_esp_store_le32(sector + 67U, 0x48464F43U);
    memcpy(sector + 71U, "HFOC-ESP   ", 11U);
    memcpy(sector + 82U, "FAT32   ", 8U);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    status = production_esp_write_sector(
        device, first_lba, sector);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }

    /*
     * The FSInfo sector and the backup boot sector live in the reserved
     * region, so they are part of the volume a firmware or another FAT driver
     * may consult.
     */
    memset(sector, 0, sizeof(sector));
    production_esp_store_le32(sector, 0x41615252U);
    production_esp_store_le32(sector + 484U, 0x61417272U);
    production_esp_store_le32(sector + 488U, 0xFFFFFFFFU);
    production_esp_store_le32(sector + 492U, 0xFFFFFFFFU);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    status = production_esp_write_sector(
        device,
        first_lba + PRODUCTION_ESP_FSINFO_SECTOR, sector);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }

    memset(sector, 0, sizeof(sector));
    sector[0U] = 0xEBU;
    sector[1U] = 0x58U;
    sector[2U] = 0x90U;
    memcpy(sector + 3U, "HFOC    ", 8U);
    production_esp_store_le16(
        sector + 11U, PRODUCTION_ESP_SECTOR_SIZE);
    sector[13U] = PRODUCTION_ESP_SECTORS_PER_CLUSTER;
    production_esp_store_le16(
        sector + 14U, PRODUCTION_ESP_RESERVED_SECTORS);
    sector[16U] = PRODUCTION_ESP_FAT_COUNT;
    sector[21U] = 0xF8U;
    production_esp_store_le16(sector + 24U, 32U);
    production_esp_store_le16(sector + 26U, 64U);
    production_esp_store_le32(
        sector + 28U, (uint32_t)(first_lba & 0xFFFFFFFFU));
    production_esp_store_le32(
        sector + 32U, (uint32_t)(block_count & 0xFFFFFFFFU));
    production_esp_store_le32(sector + 36U, sectors_per_fat);
    production_esp_store_le32(
        sector + 44U, PRODUCTION_ESP_ROOT_CLUSTER);
    production_esp_store_le16(
        sector + 48U, PRODUCTION_ESP_FSINFO_SECTOR);
    production_esp_store_le16(
        sector + 50U, PRODUCTION_ESP_BACKUP_BOOT_SECTOR);
    sector[64U] = 0x80U;
    sector[66U] = 0x29U;
    production_esp_store_le32(sector + 67U, 0x48464F43U);
    memcpy(sector + 71U, "HFOC-ESP   ", 11U);
    memcpy(sector + 82U, "FAT32   ", 8U);
    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    status = production_esp_write_sector(
        device,
        first_lba + PRODUCTION_ESP_BACKUP_BOOT_SECTOR, sector);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }

    /*
     * The first FAT sector of every copy carries the two reserved entries and
     * the root directory cluster.  Marking the root cluster allocated is what
     * keeps the first directory the installer creates from being handed the
     * root cluster itself: without it a subdirectory shares the root cluster
     * and its parent's entries, and an installed ESP resolves `\\EFI\\BOOT`
     * to the root `\\boot` directory.
     */
    memset(sector, 0, sizeof(sector));
    production_esp_store_le32(sector, 0x0FFFFFF8U);
    production_esp_store_le32(sector + 4U, 0x0FFFFFFFU);
    if ((uint64_t)PRODUCTION_ESP_ROOT_CLUSTER *
            EFI_FAT32_ESP_FAT_ENTRY_BYTES <
        sizeof(sector)) {
        production_esp_store_le32(
            sector + PRODUCTION_ESP_ROOT_CLUSTER *
                EFI_FAT32_ESP_FAT_ENTRY_BYTES,
            0x0FFFFFFFU);
    }
    for (fat_index = 0U; fat_index < PRODUCTION_ESP_FAT_COUNT;
         ++fat_index) {
        status = production_esp_write_sector(
            device,
            first_lba + PRODUCTION_ESP_RESERVED_SECTORS +
                (uint64_t)fat_index * sectors_per_fat,
            sector);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }

    memset(sector, 0, sizeof(sector));
    for (index = 1U;
         index < (uint64_t)PRODUCTION_ESP_FAT_COUNT * sectors_per_fat;
         ++index) {
        status = production_esp_write_sector(
            device,
            first_lba + PRODUCTION_ESP_RESERVED_SECTORS + index,
            sector);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }

    for (index = 0U; index < PRODUCTION_ESP_SECTORS_PER_CLUSTER;
         ++index) {
        uint64_t root_lba = first_lba +
            PRODUCTION_ESP_RESERVED_SECTORS +
            (uint64_t)PRODUCTION_ESP_FAT_COUNT * sectors_per_fat +
            index;
        status = production_esp_write_sector(
            device, root_lba, sector);
        if (status != PRODUCTION_INSTALLER_OK) {
            return status;
        }
    }

    io_status = block_device_flush(device);
    if (io_status != BLOCK_DEVICE_OK &&
        io_status != BLOCK_DEVICE_UNSUPPORTED) {
        return PRODUCTION_INSTALLER_IO_ERROR;
    }
    return PRODUCTION_INSTALLER_OK;
}

static enum production_installer_status
production_esp_map_fat_status(enum efi_fat32_esp_status status)
{
    switch (status) {
    case EFI_FAT32_ESP_OK:
        return PRODUCTION_INSTALLER_OK;
    case EFI_FAT32_ESP_CAPACITY:
    case EFI_FAT32_ESP_OUT_OF_RANGE:
        return PRODUCTION_INSTALLER_CAPACITY;
    case EFI_FAT32_ESP_IO_ERROR:
    case EFI_FAT32_ESP_FLUSH_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    default:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
}

enum production_installer_status production_installer_esp_bind_device(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count,
    bool format_owned,
    struct efi_fat32_esp *esp)
{
    enum efi_fat32_esp_status fat_status;
    enum production_installer_status status;

    if (device == NULL || esp == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (!format_owned) {
        /*
         * A shared ESP belongs to the system that was already on the machine.
         * It is bound as it is, and a volume this module cannot use - including
         * a volume whose boot sector describes an extent the partition does not
         * have (ITEM-405) - is refused, never reformatted.
         */
        fat_status = efi_fat32_esp_bind_device(
            device, first_lba, block_count, esp);
        if (fat_status == EFI_FAT32_ESP_OK) {
            return PRODUCTION_INSTALLER_OK;
        }
        return production_esp_map_fat_status(fat_status);
    }
    /*
     * ITEM-405: the role decides, not the bytes that happen to be in it.
     *
     * A whole-disk install owns the ESP role its own layout declares, so
     * whatever the range held before - a zeroed area, a formatted volume, or
     * the stale foreign FAT32 the previous partition table left behind - is not
     * a volume anybody owns.  The role is formatted and then bound, so the
     * writer always works on a volume that describes the partition it lives in.
     *
     * Before this item the format happened only when the bind failed, which is
     * how the acceptance run bound a stale 100 MiB FAT32 inside the 33 MiB role
     * and staged the installed boot set against a geometry the volume does not
     * have.
     */
    status = production_esp_format(device, first_lba, block_count);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    fat_status = efi_fat32_esp_bind_device(
        device, first_lba, block_count, esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_esp_bind(
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    bool format_owned,
    struct efi_fat32_esp *esp)
{
    if (environment == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    return production_installer_esp_bind_device(
        environment->block_device, first_lba, block_count, format_owned,
        esp);
}

enum production_installer_status production_installer_esp_install(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    const struct production_installer_source *source,
    bool format_owned)
{
    struct efi_fat32_esp esp;
    enum efi_fat32_esp_status fat_status;
    enum production_installer_status status;

    if (installer == NULL || environment == NULL ||
        source == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage !=
            PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
    }
    if (source->version != PRODUCTION_INSTALLER_SOURCE_VERSION ||
        source->boot_payload == NULL ||
        source->boot_payload_size == 0U ||
        source->boot_payload_size >
            EFI_FAT32_ESP_PAYLOAD_MAX) {
        return PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE;
    }

    status = production_installer_esp_bind(
        environment, first_lba, block_count, format_owned, &esp);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }

    fat_status = efi_fat32_esp_install_payload(
        &esp, source->boot_payload, source->boot_payload_size);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    fat_status = efi_fat32_esp_verify_payload(&esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    return PRODUCTION_INSTALLER_OK;
}

/*
 * ITEM-396: how many clusters two files of these sizes need on a volume whose
 * cluster size the bound ESP reports.  The bound geometry is what the real
 * allocation uses, so this pre-flight and the write agree by construction.
 */
static uint32_t production_esp_required_clusters(
    const struct efi_fat32_esp *esp,
    size_t first_length,
    size_t second_length)
{
    const size_t cluster_bytes =
        (size_t)esp->sectors_per_cluster * esp->bytes_per_sector;
    uint32_t clusters = 0U;

    if (cluster_bytes == 0U) {
        return 0U;
    }
    clusters = (uint32_t)((first_length + cluster_bytes - 1U) /
                          cluster_bytes);
    clusters += (uint32_t)((second_length + cluster_bytes - 1U) /
                           cluster_bytes);
    /* One cluster for the EFI directory and one for HFOC's, when new. */
    return clusters + 2U;
}

enum production_installer_status production_installer_esp_install_shared(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    const struct production_installer_source *source,
    const char *menu_text,
    size_t menu_length)
{
    struct efi_fat32_esp esp;
    enum efi_fat32_esp_status fat_status;
    enum production_installer_status status;
    uint32_t free_clusters = 0U;
    uint32_t required_clusters;

    if (installer == NULL || environment == NULL || source == NULL ||
        menu_text == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION ||
        installer->stage != PRODUCTION_INSTALLER_STAGE_EXECUTING ||
        !installer->confirmed) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return PRODUCTION_INSTALLER_EFI_UNAVAILABLE;
    }
    if (source->version != PRODUCTION_INSTALLER_SOURCE_VERSION ||
        source->boot_payload == NULL ||
        source->boot_payload_size == 0U ||
        source->boot_payload_size > EFI_FAT32_ESP_PAYLOAD_MAX ||
        menu_length == 0U ||
        menu_length > EFI_FAT32_ESP_CONFIG_MAX) {
        return PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE;
    }
    memset(&esp, 0, sizeof(esp));
    fat_status = efi_fat32_esp_bind(
        environment, first_lba, block_count, &esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        /*
         * The ESP belongs to the existing system.  Formatting it would delete
         * that system's loaders, so an ESP this module cannot use is refused
         * instead - the same reason a missing ESP is refused at plan time.
         */
        return PRODUCTION_INSTALLER_ESP_UNSUITABLE;
    }
    fat_status = efi_fat32_esp_free_clusters(&esp, &free_clusters);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return fat_status == EFI_FAT32_ESP_CAPACITY
                   ? PRODUCTION_INSTALLER_CAPACITY
                   : production_esp_map_fat_status(fat_status);
    }
    required_clusters = production_esp_required_clusters(
        &esp, source->boot_payload_size, menu_length);
    if (required_clusters == 0U || free_clusters < required_clusters) {
        /* Refused before the first write of the shared ESP. */
        return PRODUCTION_INSTALLER_CAPACITY;
    }
    fat_status = efi_fat32_esp_install_payload(
        &esp, source->boot_payload, source->boot_payload_size);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    fat_status = efi_fat32_esp_verify_payload(&esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    fat_status = efi_fat32_esp_install_config(
        &esp, (const unsigned char *)menu_text, menu_length);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    status = PRODUCTION_INSTALLER_OK;
    fat_status = efi_fat32_esp_verify_config(&esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_esp_map_fat_status(fat_status);
    }
    return status;
}
