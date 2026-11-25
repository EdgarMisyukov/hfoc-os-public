#include "efi_fat32_esp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define EFI_FAT32_ENTRIES_PER_SECTOR 16U
#define EFI_FAT32_ATTR_READ_ONLY 0x01U
#define EFI_FAT32_ATTR_HIDDEN 0x02U
#define EFI_FAT32_ATTR_SYSTEM 0x04U
#define EFI_FAT32_ATTR_VOLUME_ID 0x08U
#define EFI_FAT32_ATTR_DIRECTORY 0x10U
#define EFI_FAT32_ATTR_ARCHIVE 0x20U
#define EFI_FAT32_ATTR_LONG_NAME 0x0FU
#define EFI_FAT32_EOC 0x0FFFFFFFU

/*
 * ITEM-487: the cycle counter the range writer's probe measures with.
 *
 * The symbols are weak on purpose.  The kernel links `arch/x86_64/timer.c` and
 * gets real microseconds; a host proof that links this file without a cycle
 * counter - the ESP proof, the installer proofs - resolves them to zero and
 * reads zeroes instead of failing to link, while a proof that does provide the
 * same two symbols (the console's stub, the boot-log store's own) is measured
 * exactly as the kernel is.  The probe never changes what a path does; only the
 * numbers it reports.
 */
extern uint64_t timer_cycles(void) __attribute__((weak));
extern uint64_t timer_cycles_to_us(uint64_t cycles) __attribute__((weak));

static uint64_t efi_fat32_probe_cycles(void)
{
    if (timer_cycles == NULL) {
        return 0U;
    }
    return timer_cycles();
}

static uint64_t efi_fat32_probe_us(uint64_t cycles)
{
    if (timer_cycles_to_us == NULL) {
        return 0U;
    }
    return timer_cycles_to_us(cycles);
}

/* The reading `efi_fat32_esp_io_stats_read()` hands back, and its raw cycles. */
static struct efi_fat32_esp_io_stats efi_fat32_io_stats;
static uint64_t efi_fat32_lookup_cycles;
static uint64_t efi_fat32_traversal_cycles;

void efi_fat32_esp_io_stats_reset(void)
{
    memset(&efi_fat32_io_stats, 0, sizeof(efi_fat32_io_stats));
    efi_fat32_lookup_cycles = 0U;
    efi_fat32_traversal_cycles = 0U;
}

bool efi_fat32_esp_io_stats_read(struct efi_fat32_esp_io_stats *stats_out)
{
    if (stats_out == NULL) {
        return false;
    }
    *stats_out = efi_fat32_io_stats;
    stats_out->lookup_us = efi_fat32_probe_us(efi_fat32_lookup_cycles);
    stats_out->traversal_us = efi_fat32_probe_us(efi_fat32_traversal_cycles);
    return true;
}

struct efi_fat32_dir_ref {
    bool present;
    uint32_t cluster;
    uint32_t lba;
    size_t offset;
};

struct efi_fat32_file_ref {
    bool present;
    struct efi_fat32_dir_ref ref;
    uint32_t first_cluster;
    uint32_t file_size;
    uint8_t attributes;
    /*
     * ITEM-398: the long-name entries that immediately precede this entry and
     * name it.  They are part of the name, not decoration: a reader that
     * resolves the long name binds the whole run to the entry that follows it,
     * so an entry that is moved without them leaves its name behind to attach
     * to whatever entry takes its place.
     */
    size_t lfn_count;
    struct efi_fat32_dir_ref lfn_refs[EFI_FAT32_ESP_LFN_ENTRIES_MAX];
};

static const unsigned char efi_fat32_name_efi[11U] = {
    'E', 'F', 'I', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};

static const unsigned char efi_fat32_name_hfoc[11U] = {
    'H', 'F', 'O', 'C', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};

static const unsigned char efi_fat32_name_bootx64[11U] = {
    'B', 'O', 'O', 'T', 'X', '6', '4', ' ', 'E', 'F', 'I'
};

/* ITEM-396: the Limine configuration the installed appliance boots from. */
static const unsigned char efi_fat32_name_limine[11U] = {
    'L', 'I', 'M', 'I', 'N', 'E', ' ', ' ', 'C', 'O', 'N'
};

/*
 * The UEFI loaders a foreign system installs, in the order a menu should chain
 * to them: a shim first (that is what the firmware boots), then GRUB, then the
 * fallback loader, then the Windows boot manager.
 *
 * ITEM-409: the array and this comment disagreed - GRUB was listed before the
 * shim - and a real Debian ESP carries both files, so the menu offered
 * `\EFI\DEBIAN\GRUBX64.EFI` where the firmware boots `\EFI\DEBIAN\SHIMX64.EFI`.
 * On a machine with Secure Boot enabled the firmware refuses the unsigned GRUB
 * outright, so the entry the install offered was a system that does not boot.
 * The shim is the signed entry point and the one the firmware is given; it
 * comes first now, which is also what the vendor table's own rule in
 * `installer_boot_menu.c` states.
 */
static const char *const efi_fat32_loader_names[] = {
    "SHIMX64.EFI", "GRUBX64.EFI", "BOOTX64.EFI", "BOOTMGFW.EFI"
};
#define EFI_FAT32_LOADER_NAME_COUNT \
    (sizeof(efi_fat32_loader_names) / sizeof(efi_fat32_loader_names[0U]))

static uint16_t efi_fat32_load_le16(
    const unsigned char *bytes)
{
    return (uint16_t)bytes[0U] |
           (uint16_t)((uint16_t)bytes[1U] << 8U);
}

static void efi_fat32_store_le16(
    unsigned char *bytes,
    uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)(value >> 8U);
}

static uint32_t efi_fat32_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0U] |
           ((uint32_t)bytes[1U] << 8U) |
           ((uint32_t)bytes[2U] << 16U) |
           ((uint32_t)bytes[3U] << 24U);
}

static void efi_fat32_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static enum efi_fat32_esp_status efi_fat32_map_block_status(
    enum block_device_status status)
{
    switch (status) {
    case BLOCK_DEVICE_OK:
        return EFI_FAT32_ESP_OK;
    case BLOCK_DEVICE_NULL:
    case BLOCK_DEVICE_BAD_DEVICE:
    case BLOCK_DEVICE_BAD_BLOCK_SIZE:
    case BLOCK_DEVICE_BAD_BLOCK_COUNT:
    case BLOCK_DEVICE_UNALIGNED:
    case BLOCK_DEVICE_INVALID_LENGTH:
    case BLOCK_DEVICE_OUT_OF_RANGE:
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    case BLOCK_DEVICE_IO_ERROR:
        return EFI_FAT32_ESP_IO_ERROR;
    case BLOCK_DEVICE_UNSUPPORTED:
        return EFI_FAT32_ESP_FLUSH_ERROR;
    default:
        return EFI_FAT32_ESP_IO_ERROR;
    }
}

static enum efi_fat32_esp_status efi_fat32_sector_read(
    const struct efi_fat32_esp *esp,
    uint64_t lba,
    unsigned char *buffer)
{
    enum block_device_status status;
    uint64_t byte_offset;

    if (esp == NULL || buffer == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (lba >= esp->block_count) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    byte_offset = (esp->first_lba + lba) *
                  (uint64_t)esp->bytes_per_sector;
    status = block_device_read(
        (struct block_device *)&esp->device,
        byte_offset, buffer, esp->bytes_per_sector);
    if (status == BLOCK_DEVICE_OK) {
        efi_fat32_io_stats.sector_reads += 1U;
    }
    return efi_fat32_map_block_status(status);
}

static enum efi_fat32_esp_status efi_fat32_sector_write(
    struct efi_fat32_esp *esp,
    uint64_t lba,
    const unsigned char *buffer)
{
    enum block_device_status status;
    uint64_t byte_offset;

    if (esp == NULL || buffer == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (lba >= esp->block_count) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    byte_offset = (esp->first_lba + lba) *
                  (uint64_t)esp->bytes_per_sector;
    status = block_device_write(
        &esp->device, byte_offset, buffer, esp->bytes_per_sector);
    if (status == BLOCK_DEVICE_OK) {
        efi_fat32_io_stats.sector_writes += 1U;
    }
    return efi_fat32_map_block_status(status);
}

static enum efi_fat32_esp_status efi_fat32_flush_device(
    struct efi_fat32_esp *esp)
{
    enum block_device_status status;

    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (esp->device.flush_fn == NULL) {
        return EFI_FAT32_ESP_OK;
    }
    status = block_device_flush(&esp->device);
    if (status == BLOCK_DEVICE_UNSUPPORTED) {
        return EFI_FAT32_ESP_OK;
    }
    return efi_fat32_map_block_status(status);
}

static bool efi_fat32_is_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static enum efi_fat32_esp_status efi_fat32_fat_value_read(
    const struct efi_fat32_esp *esp,
    uint32_t cluster,
    uint32_t *value)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint64_t fat_byte_offset;
    uint32_t fat_sector;
    size_t sector_offset;
    uint32_t max_cluster;

    if (esp == NULL || value == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    max_cluster = 2U + esp->cluster_count;
    if (cluster < 2U || cluster >= max_cluster) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    fat_byte_offset = (uint64_t)cluster * 4U;
    fat_sector = (uint32_t)(fat_byte_offset /
                            esp->bytes_per_sector);
    sector_offset = (size_t)(fat_byte_offset %
                             esp->bytes_per_sector);
    status = efi_fat32_sector_read(
        esp, esp->reserved_sectors + fat_sector, sector);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *value = efi_fat32_load_le32(sector + sector_offset) &
             0x0FFFFFFFU;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_fat_value_write(
    struct efi_fat32_esp *esp,
    uint32_t cluster,
    uint32_t value)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint64_t fat_byte_offset;
    uint32_t fat_sector;
    size_t sector_offset;
    uint32_t fat_index;
    uint32_t max_cluster;

    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    max_cluster = 2U + esp->cluster_count;
    if (cluster < 2U || cluster >= max_cluster) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    fat_byte_offset = (uint64_t)cluster * 4U;
    fat_sector = (uint32_t)(fat_byte_offset /
                            esp->bytes_per_sector);
    sector_offset = (size_t)(fat_byte_offset %
                             esp->bytes_per_sector);
    for (fat_index = 0U; fat_index < esp->fat_count; ++fat_index) {
        status = efi_fat32_sector_read(
            esp,
            esp->reserved_sectors +
                (uint64_t)fat_index * esp->sectors_per_fat +
                fat_sector,
            sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        uint32_t previous = efi_fat32_load_le32(
            sector + sector_offset);
        efi_fat32_store_le32(sector + sector_offset,
                             (previous & 0xF0000000U) |
                                 (value & 0x0FFFFFFFU));
        status = efi_fat32_sector_write(
            esp,
            esp->reserved_sectors +
                (uint64_t)fat_index * esp->sectors_per_fat +
                fat_sector,
            sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    return EFI_FAT32_ESP_OK;
}

static bool efi_fat32_fat_value_eoc(uint32_t value)
{
    return value >= 0x0FFFFFF8U && value <= EFI_FAT32_EOC;
}

static bool efi_fat32_fat_next(
    const struct efi_fat32_esp *esp,
    uint32_t cluster,
    uint32_t *next,
    bool *has_next)
{
    enum efi_fat32_esp_status status;
    uint32_t value;
    uint32_t max_cluster;

    *has_next = false;
    *next = 0U;
    max_cluster = 2U + esp->cluster_count;
    status = efi_fat32_fat_value_read(esp, cluster, &value);
    if (status != EFI_FAT32_ESP_OK ||
        efi_fat32_fat_value_eoc(value)) {
        return status == EFI_FAT32_ESP_OK;
    }
    if (value < 2U || value >= max_cluster) {
        return false;
    }
    *next = value;
    *has_next = true;
    return true;
}

static enum efi_fat32_esp_status efi_fat32_allocate_free_cluster(
    struct efi_fat32_esp *esp,
    uint32_t *cluster)
{
    enum efi_fat32_esp_status status;
    uint32_t candidate;
    uint32_t max_cluster;
    uint32_t value;

    if (esp == NULL || cluster == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    max_cluster = 2U + esp->cluster_count;
    for (candidate = 2U; candidate < max_cluster; ++candidate) {
        status = efi_fat32_fat_value_read(
            esp, candidate, &value);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        if (value == 0U) {
            *cluster = candidate;
            return EFI_FAT32_ESP_OK;
        }
    }
    return EFI_FAT32_ESP_CAPACITY;
}

static enum efi_fat32_esp_status efi_fat32_free_chain(
    struct efi_fat32_esp *esp,
    uint32_t first_cluster)
{
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t max_cluster;
    uint32_t steps;

    if (first_cluster == 0U) {
        return EFI_FAT32_ESP_OK;
    }
    current = first_cluster;
    max_cluster = 2U + esp->cluster_count;
    steps = 0U;
    while (steps < max_cluster) {
        if (current < 2U || current >= max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!efi_fat32_fat_next(esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        status = efi_fat32_fat_value_write(esp, current, 0U);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        if (!has_next) {
            return EFI_FAT32_ESP_OK;
        }
        current = next;
        ++steps;
    }
    return EFI_FAT32_ESP_CORRUPT;
}

static enum efi_fat32_esp_status efi_fat32_allocate_chain(
    struct efi_fat32_esp *esp,
    uint32_t count,
    uint32_t *first_cluster)
{
    uint32_t chain[EFI_FAT32_ESP_MAX_PAYLOAD_CLUSTERS];
    enum efi_fat32_esp_status status;
    uint32_t cluster_index;
    uint32_t max_cluster;
    uint32_t value;
    uint32_t allocated = 0U;

    if (esp == NULL || first_cluster == NULL || count == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (count > EFI_FAT32_ESP_MAX_PAYLOAD_CLUSTERS) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    max_cluster = 2U + esp->cluster_count;
    for (cluster_index = 2U;
         cluster_index < max_cluster && allocated < count;
         ++cluster_index) {
        status = efi_fat32_fat_value_read(
            esp, cluster_index, &value);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        if (value == 0U) {
            chain[allocated++] = cluster_index;
        }
    }
    if (allocated != count) {
        return EFI_FAT32_ESP_CAPACITY;
    }

    for (cluster_index = 0U; cluster_index < count; ++cluster_index) {
        status = efi_fat32_fat_value_write(
            esp, chain[cluster_index], EFI_FAT32_EOC);
        if (status != EFI_FAT32_ESP_OK) {
            while (cluster_index > 0U) {
                --cluster_index;
                (void)efi_fat32_fat_value_write(
                    esp, chain[cluster_index], 0U);
            }
            return status;
        }
    }
    for (cluster_index = 1U; cluster_index < count; ++cluster_index) {
        status = efi_fat32_fat_value_write(
            esp, chain[cluster_index - 1U],
            chain[cluster_index]);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_free_chain(esp, chain[0U]);
            for (cluster_index = 0U;
                 cluster_index < count;
                 ++cluster_index) {
                if (chain[cluster_index] != 0U) {
                    (void)efi_fat32_fat_value_write(
                        esp, chain[cluster_index], 0U);
                }
            }
            return status;
        }
    }
    *first_cluster = chain[0U];
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_dir_reserve_slot(
    struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    struct efi_fat32_dir_ref *ref)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t max_cluster;
    uint32_t cluster_sector;
    size_t entry;
    uint32_t last_cluster = 0U;
    bool last_known = false;
    uint32_t steps;

    if (esp == NULL || ref == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(ref, 0, sizeof(*ref));
    max_cluster = 2U + esp->cluster_count;
    current = directory_cluster;
    steps = 0U;
    while (true) {
        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        last_cluster = current;
        last_known = true;
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            for (entry = 0U;
                 entry < EFI_FAT32_ENTRIES_PER_SECTOR;
                 ++entry) {
                size_t offset = entry *
                    EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
                if (sector[offset] == 0x00U ||
                    sector[offset] == 0xE5U) {
                    ref->present = true;
                    ref->cluster = current;
                    ref->lba = (uint32_t)relative_lba;
                    ref->offset = offset;
                    return EFI_FAT32_ESP_OK;
                }
            }
        }
        if (!efi_fat32_fat_next(
                esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            break;
        }
        current = next;
        ++steps;
    }

    if (!last_known) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    status = efi_fat32_allocate_free_cluster(esp, &next);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(
        esp, last_cluster, next);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(
        esp, next, EFI_FAT32_EOC);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(
            esp, last_cluster, EFI_FAT32_EOC);
        return status;
    }
    memset(sector, 0, sizeof(sector));
    for (cluster_sector = 0U;
         cluster_sector < esp->sectors_per_cluster;
         ++cluster_sector) {
        uint64_t relative_lba =
            esp->data_start_lba +
            (uint64_t)(next - 2U) *
                esp->sectors_per_cluster +
            cluster_sector;
        if (relative_lba >= esp->block_count) {
            return EFI_FAT32_ESP_OUT_OF_RANGE;
        }
        status = efi_fat32_sector_write(
            esp, relative_lba, sector);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_fat_value_write(
                esp, next, 0U);
            (void)efi_fat32_fat_value_write(
                esp, last_cluster, EFI_FAT32_EOC);
            return status;
        }
    }
    ref->present = true;
    ref->cluster = next;
    ref->lba = (uint32_t)(
        esp->data_start_lba +
        (uint64_t)(next - 2U) * esp->sectors_per_cluster);
    ref->offset = 0U;
    return EFI_FAT32_ESP_OK;
}

static bool efi_fat32_upper_byte(unsigned char value)
{
    if (value >= (unsigned char)'a' &&
        value <= (unsigned char)'z') {
        return true;
    }
    return false;
}

static unsigned char efi_fat32_to_upper(unsigned char value)
{
    if (efi_fat32_upper_byte(value)) {
        return (unsigned char)(value - 0x20U);
    }
    return value;
}

static bool efi_fat32_name_equal(
    const unsigned char *left,
    const unsigned char *right)
{
    size_t index;

    for (index = 0U; index < 11U; ++index) {
        if (efi_fat32_to_upper(left[index]) !=
            efi_fat32_to_upper(right[index])) {
            return false;
        }
    }
    return true;
}

static enum efi_fat32_esp_status efi_fat32_dir_find(
    const struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    const unsigned char name[11U],
    struct efi_fat32_file_ref *result)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t max_cluster;
    uint32_t cluster_sector;
    size_t entry;
    uint32_t steps;

    if (esp == NULL || name == NULL || result == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(result, 0, sizeof(*result));
    max_cluster = 2U + esp->cluster_count;
    current = directory_cluster;
    steps = 0U;
    while (true) {
        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            for (entry = 0U;
                 entry < EFI_FAT32_ENTRIES_PER_SECTOR;
                 ++entry) {
                size_t offset = entry *
                    EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
                if (sector[offset] == 0x00U ||
                    sector[offset] == 0xE5U) {
                    if (sector[offset] == 0x00U) {
                        return EFI_FAT32_ESP_NOT_FOUND;
                    }
                    continue;
                }
                if ((sector[offset + 11U] &
                     EFI_FAT32_ATTR_LONG_NAME) ==
                    EFI_FAT32_ATTR_LONG_NAME ||
                    (sector[offset + 11U] &
                     EFI_FAT32_ATTR_VOLUME_ID) != 0U) {
                    continue;
                }
                if (efi_fat32_name_equal(
                        sector + offset, name)) {
                    result->present = true;
                    result->ref.present = true;
                    result->ref.cluster = current;
                    result->ref.lba = (uint32_t)relative_lba;
                    result->ref.offset = offset;
                    result->first_cluster =
                        efi_fat32_load_le16(
                            sector + offset + 20U) << 16U |
                        efi_fat32_load_le16(
                            sector + offset + 26U);
                    result->file_size =
                        efi_fat32_load_le32(
                            sector + offset + 28U);
                    result->attributes =
                        sector[offset + 11U];
                    return EFI_FAT32_ESP_OK;
                }
            }
        }
        if (!efi_fat32_fat_next(
                esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            return EFI_FAT32_ESP_NOT_FOUND;
        }
        current = next;
        ++steps;
    }
}

static void efi_fat32_make_dot_name(
    unsigned char *name,
    unsigned int dot_count)
{
    size_t index;

    for (index = 0U; index < 11U; ++index) {
        name[index] = (unsigned char)' ';
    }
    for (index = 0U; index < dot_count; ++index) {
        name[index] = (unsigned char)'.';
    }
}

/*
 * ITEM-413: every cluster of a directory this writer creates starts as zeroed
 * sectors, so nothing the volume held there before is reachable through the
 * directory - not by a firmware, not by another FAT driver and not by this
 * writer's own lookup.  A FAT directory ends at the first byte that is zero,
 * so the clearing is also what terminates the entries the caller writes next.
 */
static enum efi_fat32_esp_status efi_fat32_directory_clear(
    struct efi_fat32_esp *esp,
    uint32_t cluster)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    uint32_t index;

    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(sector, 0, sizeof(sector));
    for (index = 0U; index < esp->sectors_per_cluster; ++index) {
        uint64_t relative_lba =
            esp->data_start_lba +
            (uint64_t)(cluster - 2U) * esp->sectors_per_cluster +
            index;
        enum efi_fat32_esp_status status =
            efi_fat32_sector_write(esp, relative_lba, sector);

        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_create_directory(
    struct efi_fat32_esp *esp,
    uint32_t parent_cluster,
    const unsigned char name[11U],
    uint32_t *new_cluster)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    unsigned char entry[EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t cluster;

    if (esp == NULL || name == NULL || new_cluster == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    status = efi_fat32_allocate_free_cluster(esp, &cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(
        esp, cluster, EFI_FAT32_EOC);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    memset(sector, 0, sizeof(sector));
    memset(entry, 0, sizeof(entry));
    efi_fat32_make_dot_name(entry, 1U);
    entry[11U] = EFI_FAT32_ATTR_DIRECTORY;
    efi_fat32_store_le16(entry + 20U,
        (uint16_t)(cluster >> 16U));
    efi_fat32_store_le16(entry + 26U,
        (uint16_t)(cluster & 0xFFFFU));
    memcpy(sector, entry, sizeof(entry));

    memset(entry, 0, sizeof(entry));
    efi_fat32_make_dot_name(entry, 2U);
    entry[11U] = EFI_FAT32_ATTR_DIRECTORY;
    efi_fat32_store_le16(entry + 20U,
        (uint16_t)(parent_cluster >> 16U));
    efi_fat32_store_le16(entry + 26U,
        (uint16_t)(parent_cluster & 0xFFFFU));
    memcpy(sector + 32U, entry, sizeof(entry));

    /*
     * ITEM-413: the whole cluster is cleared first and the `.`/`..` entries
     * are written into it afterwards, so a directory is empty except for what
     * this writer puts there.  `efi_fat32_dir_ensure_element` creates the
     * nested directories and clears the same way.
     */
    status = efi_fat32_directory_clear(esp, cluster);
    if (status == EFI_FAT32_ESP_OK) {
        status = efi_fat32_sector_write(
            esp,
            esp->data_start_lba +
                (uint64_t)(cluster - 2U) *
                    esp->sectors_per_cluster,
            sector);
    }
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(esp, cluster, 0U);
        return status;
    }
    *new_cluster = cluster;
    return EFI_FAT32_ESP_OK;
}
static enum efi_fat32_esp_status efi_fat32_dir_entry_write(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_ref *ref,
    const unsigned char name[11U],
    uint8_t attributes,
    uint32_t first_cluster,
    uint32_t file_size)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;

    if (esp == NULL || ref == NULL || !ref->present ||
        name == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    status = efi_fat32_sector_read(esp, ref->lba, sector);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    memset(sector + ref->offset, 0,
           EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE);
    memcpy(sector + ref->offset, name, 11U);
    sector[ref->offset + 11U] = attributes;
    efi_fat32_store_le16(sector + ref->offset + 20U,
        (uint16_t)(first_cluster >> 16U));
    efi_fat32_store_le16(sector + ref->offset + 26U,
        (uint16_t)(first_cluster & 0xFFFFU));
    efi_fat32_store_le32(sector + ref->offset + 28U,
        file_size);
    return efi_fat32_sector_write(esp, ref->lba, sector);
}

static enum efi_fat32_esp_status efi_fat32_dir_entry_update(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_ref *ref,
    uint32_t first_cluster,
    uint32_t file_size)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;

    if (esp == NULL || ref == NULL || !ref->present) {
        return EFI_FAT32_ESP_NULL;
    }
    status = efi_fat32_sector_read(esp, ref->lba, sector);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    efi_fat32_store_le16(sector + ref->offset + 20U,
        (uint16_t)(first_cluster >> 16U));
    efi_fat32_store_le16(sector + ref->offset + 26U,
        (uint16_t)(first_cluster & 0xFFFFU));
    efi_fat32_store_le32(sector + ref->offset + 28U,
        file_size);
    return efi_fat32_sector_write(esp, ref->lba, sector);
}

static enum efi_fat32_esp_status efi_fat32_dir_entry_clear(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_ref *ref)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;

    if (esp == NULL || ref == NULL || !ref->present) {
        return EFI_FAT32_ESP_NULL;
    }
    status = efi_fat32_sector_read(esp, ref->lba, sector);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    sector[ref->offset] = 0xE5U;
    return efi_fat32_sector_write(esp, ref->lba, sector);
}

static enum efi_fat32_esp_status efi_fat32_ensure_directory(
    struct efi_fat32_esp *esp,
    uint32_t parent_cluster,
    const unsigned char name[11U],
    uint32_t *directory_cluster)
{
    struct efi_fat32_file_ref result;
    struct efi_fat32_dir_ref slot;
    enum efi_fat32_esp_status status;
    uint32_t created_cluster = 0U;

    status = efi_fat32_dir_find(
        esp, parent_cluster, name, &result);
    if (status == EFI_FAT32_ESP_OK) {
        if ((result.attributes &
             EFI_FAT32_ATTR_DIRECTORY) == 0U) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        *directory_cluster = result.first_cluster;
        return EFI_FAT32_ESP_OK;
    }
    if (status != EFI_FAT32_ESP_NOT_FOUND) {
        return status;
    }

    status = efi_fat32_create_directory(
        esp, parent_cluster, name, &created_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_reserve_slot(
        esp, parent_cluster, &slot);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(
            esp, created_cluster, 0U);
        return status;
    }
    status = efi_fat32_dir_entry_write(
        esp, &slot, name, EFI_FAT32_ATTR_DIRECTORY,
        created_cluster, 0U);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_dir_entry_clear(esp, &slot);
        (void)efi_fat32_fat_value_write(
            esp, created_cluster, 0U);
        return status;
    }
    *directory_cluster = created_cluster;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_write_payload_data(
    struct efi_fat32_esp *esp,
    uint32_t first_cluster,
    const unsigned char *payload,
    size_t payload_size,
    uint32_t cluster_count)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t cluster_index;
    uint32_t cluster_sector;
    size_t cluster_bytes;
    size_t file_offset;
    size_t cluster_start;

    current = first_cluster;
    cluster_bytes = (size_t)esp->sectors_per_cluster *
                    esp->bytes_per_sector;
    for (cluster_index = 0U;
         cluster_index < cluster_count;
         ++cluster_index) {
        cluster_start = (size_t)cluster_index * cluster_bytes;
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            file_offset = cluster_start +
                (size_t)cluster_sector *
                    esp->bytes_per_sector;
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            memset(sector, 0, sizeof(sector));
            if (file_offset < payload_size) {
                size_t copy = sizeof(sector);
                if (payload_size - file_offset < copy) {
                    copy = payload_size - file_offset;
                }
                memcpy(sector, payload + file_offset, copy);
            }
            status = efi_fat32_sector_write(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
        }
        if (cluster_index + 1U < cluster_count) {
            if (!efi_fat32_fat_next(
                    esp, current, &next, &has_next) ||
                !has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
        }
    }
    return EFI_FAT32_ESP_OK;
}

/* ------------------------------------------------- ITEM-397 payload set --- */

#define EFI_FAT32_ESP_LFN_ATTRIBUTE 0x0FU
#define EFI_FAT32_ESP_PATH_ELEMENTS_MAX 4U
#define EFI_FAT32_ESP_LFN_CHARACTERS 13U
#define EFI_FAT32_ESP_LFN_BUFFER_MAX (EFI_FAT32_ESP_ELEMENT_MAX + 1U)

/*
 * A canonical installed path, resolved once: the elements in order, the 8.3
 * alias each element is stored under, and whether the element needs a VFAT
 * long-name chain because it does not fit the 8.3 form.
 */
struct efi_fat32_esp_path {
    size_t element_count;
    size_t lengths[EFI_FAT32_ESP_PATH_ELEMENTS_MAX];
    char elements[EFI_FAT32_ESP_PATH_ELEMENTS_MAX]
                 [EFI_FAT32_ESP_ELEMENT_MAX + 1U];
    unsigned char short_names[EFI_FAT32_ESP_PATH_ELEMENTS_MAX]
                             [EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH];
    bool long_names[EFI_FAT32_ESP_PATH_ELEMENTS_MAX];
};

/* A run of consecutive free directory entry slots inside one cluster. */
struct efi_fat32_dir_run {
    bool present;
    uint32_t cluster;
    uint32_t lba;
    size_t offset;
};

/* Read-only cursor over one FAT copy, so walking a chain does not cost one
 * device read per link. */
struct efi_fat32_chain_cursor {
    bool buffered;
    uint32_t fat_sector;
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
};

/* Read/write accumulator for one FAT sector in every FAT copy. */
struct efi_fat32_fat_update {
    bool active;
    uint32_t fat_sector;
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    unsigned char dirty[EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR / 8U];
};

static enum efi_fat32_esp_status efi_fat32_dir_run_write_children(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_run *run,
    uint32_t self_cluster,
    uint32_t parent_cluster);

static bool efi_fat32_element_byte_valid(unsigned char value)
{
    if (value <= 0x20U || value >= 0x7FU) {
        return false;
    }
    switch (value) {
    case '"':
    case '*':
    case '+':
    case ',':
    case '/':
    case ':':
    case ';':
    case '<':
    case '=':
    case '>':
    case '?':
    case '[':
    case '\\':
    case ']':
    case '|':
        return false;
    default:
        return true;
    }
}

static unsigned char efi_fat32_short_checksum(
    const unsigned char name[EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH])
{
    unsigned char sum = 0U;
    size_t index;

    for (index = 0U;
         index < EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH;
         ++index) {
        sum = (unsigned char)(((sum & 1U) << 7U) +
                              (sum >> 1U) + name[index]);
    }
    return sum;
}

/*
 * Derive the 8.3 name an element is stored under.  A name whose base is at
 * most eight bytes and whose extension is at most three bytes is stored as
 * itself (padded, upper case) and needs no long-name chain; every other name
 * gets the documented deterministic alias `XXXXXX~1.EXT` plus a long-name
 * chain, so the alias is stable across installs and a re-install replaces the
 * same file instead of creating a second one.
 */
static bool efi_fat32_short_name_from_element(
    const char *element,
    size_t length,
    unsigned char short_name[EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH],
    bool *long_name_required)
{
    size_t dot = length;
    size_t base_length;
    size_t extension_length;
    size_t index;
    size_t position;
    bool fits;
    bool base_dot;

    if (element == NULL || short_name == NULL ||
        long_name_required == NULL ||
        length == 0U || length > EFI_FAT32_ESP_ELEMENT_MAX) {
        return false;
    }
    for (index = 0U; index < length; ++index) {
        if (!efi_fat32_element_byte_valid(
                (unsigned char)element[index]) &&
            element[index] != '.') {
            return false;
        }
    }
    for (index = length; index > 0U; --index) {
        if (element[index - 1U] == '.') {
            dot = index - 1U;
            break;
        }
    }
    if (dot == length) {
        base_length = length;
        extension_length = 0U;
    } else {
        if (dot == 0U || dot + 1U == length) {
            return false;
        }
        base_length = dot;
        extension_length = length - dot - 1U;
    }
    base_dot = false;
    for (index = 0U; index < base_length; ++index) {
        if (element[index] == '.') {
            base_dot = true;
        }
    }
    fits = !base_dot && base_length <= 8U && extension_length <= 3U;

    for (index = 0U;
         index < EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH;
         ++index) {
        short_name[index] = (unsigned char)' ';
    }
    if (fits) {
        for (index = 0U; index < base_length; ++index) {
            short_name[index] = efi_fat32_to_upper(
                (unsigned char)element[index]);
        }
        for (index = 0U; index < extension_length; ++index) {
            short_name[8U + index] = efi_fat32_to_upper(
                (unsigned char)element[dot + 1U + index]);
        }
        *long_name_required = false;
        return true;
    }

    position = 0U;
    for (index = 0U; index < base_length && position < 6U; ++index) {
        if (element[index] == '.') {
            continue;
        }
        short_name[position++] = efi_fat32_to_upper(
            (unsigned char)element[index]);
    }
    if (position == 0U) {
        return false;
    }
    short_name[position] = (unsigned char)'~';
    short_name[position + 1U] = (unsigned char)'1';
    for (index = 0U;
         index < extension_length && index < 3U;
         ++index) {
        short_name[8U + index] = efi_fat32_to_upper(
            (unsigned char)element[dot + 1U + index]);
    }
    *long_name_required = true;
    return true;
}

static size_t efi_fat32_lfn_entry_count(size_t length)
{
    return (length + EFI_FAT32_ESP_LFN_CHARACTERS) /
           EFI_FAT32_ESP_LFN_CHARACTERS;
}

static unsigned char *efi_fat32_lfn_slot(
    unsigned char *entry,
    size_t index)
{
    if (index < 5U) {
        return entry + 1U + index * 2U;
    }
    if (index < 11U) {
        return entry + 14U + (index - 5U) * 2U;
    }
    return entry + 28U + (index - 11U) * 2U;
}

static void efi_fat32_lfn_fill(
    unsigned char *entries,
    size_t entry_count,
    const char *name,
    size_t length,
    const unsigned char short_name[EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH])
{
    const unsigned char checksum =
        efi_fat32_short_checksum(short_name);
    size_t part;

    for (part = 1U; part <= entry_count; ++part) {
        unsigned char *entry =
            entries + (entry_count - part) *
                EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
        size_t index;

        memset(entry, 0xFF, EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE);
        entry[0U] = (unsigned char)part;
        if (part == entry_count) {
            entry[0U] |= 0x40U;
        }
        entry[11U] = EFI_FAT32_ESP_LFN_ATTRIBUTE;
        entry[12U] = 0U;
        entry[13U] = checksum;
        entry[26U] = 0U;
        entry[27U] = 0U;
        for (index = 0U;
             index < EFI_FAT32_ESP_LFN_CHARACTERS;
             ++index) {
            const size_t name_index =
                (part - 1U) * EFI_FAT32_ESP_LFN_CHARACTERS + index;
            unsigned int value = 0xFFFFU;
            unsigned char *slot = efi_fat32_lfn_slot(entry, index);

            if (name_index < length) {
                value = (unsigned char)name[name_index];
            } else if (name_index == length) {
                value = 0x0000U;
            }
            slot[0U] = (unsigned char)(value & 0xFFU);
            slot[1U] = (unsigned char)((value >> 8U) & 0xFFU);
        }
    }
}

static enum efi_fat32_esp_status efi_fat32_path_parse(
    const char *path,
    struct efi_fat32_esp_path *parsed)
{
    size_t length = 0U;
    size_t index;
    size_t element = 0U;
    size_t position = 0U;

    if (path == NULL || parsed == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(parsed, 0, sizeof(*parsed));
    while (length < EFI_FAT32_ESP_PATH_MAX &&
           path[length] != '\0') {
        ++length;
    }
    if (length < 2U || length >= EFI_FAT32_ESP_PATH_MAX ||
        path[0U] != '\\') {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    for (index = 1U; index <= length; ++index) {
        const bool end = index == length;
        const unsigned char byte =
            end ? (unsigned char)'\\' : (unsigned char)path[index];

        if (byte == (unsigned char)'\\') {
            if (position == 0U ||
                element >= EFI_FAT32_ESP_PATH_ELEMENTS_MAX) {
                return EFI_FAT32_ESP_PATH_DENIED;
            }
            parsed->elements[element][position] = '\0';
            parsed->lengths[element] = position;
            if (!efi_fat32_short_name_from_element(
                    parsed->elements[element], position,
                    parsed->short_names[element],
                    &parsed->long_names[element])) {
                return EFI_FAT32_ESP_PATH_DENIED;
            }
            ++element;
            position = 0U;
            continue;
        }
        if (position >= EFI_FAT32_ESP_ELEMENT_MAX ||
            (!efi_fat32_element_byte_valid(byte) &&
             byte != (unsigned char)'.')) {
            return EFI_FAT32_ESP_PATH_DENIED;
        }
        parsed->elements[element][position++] = (char)byte;
    }
    if (element == 0U) {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    parsed->element_count = element;
    return EFI_FAT32_ESP_OK;
}

static bool efi_fat32_path_prefix_equal(
    const struct efi_fat32_esp_path *left,
    const struct efi_fat32_esp_path *right,
    size_t prefix)
{
    size_t index;

    if (left->element_count <= prefix ||
        right->element_count <= prefix) {
        return false;
    }
    for (index = 0U; index < prefix; ++index) {
        if (left->lengths[index] != right->lengths[index] ||
            memcmp(left->elements[index], right->elements[index],
                   left->lengths[index]) != 0) {
            return false;
        }
    }
    return true;
}

static bool efi_fat32_path_element_equal(
    const struct efi_fat32_esp_path *left,
    size_t left_index,
    const struct efi_fat32_esp_path *right,
    size_t right_index)
{
    return left->lengths[left_index] == right->lengths[right_index] &&
           memcmp(left->elements[left_index],
                  right->elements[right_index],
                  left->lengths[left_index]) == 0;
}

/*
 * Find one path element in a directory.  The 8.3 alias selects the candidate
 * entry; a name that does not fit 8.3 additionally requires the long-name
 * chain in front of that entry to reconstruct exactly the requested name, so
 * a foreign file that happens to share the alias is never treated as the
 * staged file.
 */
static enum efi_fat32_esp_status efi_fat32_dir_find_element(
    const struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    const struct efi_fat32_esp_path *path,
    size_t element,
    bool directory_required,
    struct efi_fat32_file_ref *result)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    unsigned char lfn[EFI_FAT32_ESP_LFN_BUFFER_MAX];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t max_cluster;
    uint32_t cluster_sector;
    size_t entry;
    uint32_t steps;
    bool lfn_active = false;
    size_t lfn_expected = 0U;
    size_t lfn_received = 0U;
    size_t lfn_length = 0U;
    unsigned char lfn_checksum = 0U;
    struct efi_fat32_dir_ref lfn_refs[EFI_FAT32_ESP_LFN_ENTRIES_MAX];
    size_t lfn_ref_count = 0U;

    if (esp == NULL || path == NULL || result == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(result, 0, sizeof(*result));
    memset(lfn, 0, sizeof(lfn));
    memset(lfn_refs, 0, sizeof(lfn_refs));
    max_cluster = 2U + esp->cluster_count;
    current = directory_cluster;
    steps = 0U;
    while (true) {
        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            for (entry = 0U;
                 entry < EFI_FAT32_ENTRIES_PER_SECTOR;
                 ++entry) {
                size_t offset =
                    entry * EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
                const unsigned char *slot = sector + offset;
                const uint8_t attributes = slot[11U];

                if (slot[0U] == 0x00U) {
                    return EFI_FAT32_ESP_NOT_FOUND;
                }
                if (slot[0U] == 0xE5U) {
                    lfn_active = false;
                    lfn_ref_count = 0U;
                    continue;
                }
                if (attributes == EFI_FAT32_ESP_LFN_ATTRIBUTE) {
                    const size_t part =
                        (size_t)(slot[0U] & 0x1FU);
                    const bool last =
                        (slot[0U] & 0x40U) != 0U;
                    size_t character;

                    if (part == 0U ||
                        part > EFI_FAT32_ESP_LFN_ENTRIES_MAX) {
                        lfn_active = false;
                        lfn_ref_count = 0U;
                        continue;
                    }
                    if (last) {
                        lfn_active = true;
                        lfn_expected = part;
                        lfn_received = 0U;
                        lfn_length = 0U;
                        lfn_checksum = slot[13U];
                        lfn_ref_count = 0U;
                        memset(lfn, 0, sizeof(lfn));
                    } else if (!lfn_active ||
                               part + lfn_received != lfn_expected) {
                        lfn_active = false;
                        lfn_ref_count = 0U;
                        continue;
                    }
                    if (lfn_ref_count >=
                        EFI_FAT32_ESP_LFN_ENTRIES_MAX) {
                        lfn_active = false;
                        lfn_ref_count = 0U;
                        continue;
                    }
                    lfn_refs[lfn_ref_count].present = true;
                    lfn_refs[lfn_ref_count].cluster = current;
                    lfn_refs[lfn_ref_count].lba =
                        (uint32_t)relative_lba;
                    lfn_refs[lfn_ref_count].offset = offset;
                    ++lfn_ref_count;
                    for (character = 0U;
                         character < EFI_FAT32_ESP_LFN_CHARACTERS;
                         ++character) {
                        const unsigned char *source =
                            efi_fat32_lfn_slot(
                                (unsigned char *)slot, character);
                        const size_t name_index =
                            (part - 1U) *
                                EFI_FAT32_ESP_LFN_CHARACTERS +
                            character;

                        if (name_index >=
                            EFI_FAT32_ESP_ELEMENT_MAX) {
                            continue;
                        }
                        if (source[0U] == 0x00U &&
                            source[1U] == 0x00U) {
                            continue;
                        }
                        if (source[0U] == 0xFFU &&
                            source[1U] == 0xFFU) {
                            continue;
                        }
                        if (source[1U] != 0U) {
                            lfn_active = false;
                            break;
                        }
                        lfn[name_index] = (char)source[0U];
                        if (name_index + 1U > lfn_length) {
                            lfn_length = name_index + 1U;
                        }
                    }
                    if (lfn_active) {
                        ++lfn_received;
                    } else {
                        lfn_ref_count = 0U;
                    }
                    continue;
                }
                if ((attributes & EFI_FAT32_ATTR_VOLUME_ID) != 0U) {
                    lfn_active = false;
                    lfn_ref_count = 0U;
                    continue;
                }
                if (!efi_fat32_name_equal(
                        slot, path->short_names[element])) {
                    lfn_active = false;
                    lfn_ref_count = 0U;
                    continue;
                }
                if (path->long_names[element]) {
                    if (!lfn_active ||
                        lfn_received != lfn_expected ||
                        lfn_checksum !=
                            efi_fat32_short_checksum(
                                path->short_names[element]) ||
                        lfn_length != path->lengths[element] ||
                        memcmp(lfn, path->elements[element],
                               lfn_length) != 0) {
                        lfn_active = false;
                        lfn_ref_count = 0U;
                        continue;
                    }
                }
                if (((attributes & EFI_FAT32_ATTR_DIRECTORY) != 0U) !=
                    directory_required) {
                    lfn_active = false;
                    lfn_ref_count = 0U;
                    continue;
                }
                /*
                 * A complete run whose checksum is this short name's checksum is
                 * the entry's own long name, whether or not the caller asked for
                 * a long name.  It travels with the entry, so it is recorded
                 * here and cleared with the entry when the entry moves.
                 */
                if (lfn_active && lfn_expected != 0U &&
                    lfn_received == lfn_expected &&
                    lfn_ref_count == lfn_expected &&
                    lfn_checksum ==
                        efi_fat32_short_checksum(
                            path->short_names[element])) {
                    result->lfn_count = lfn_ref_count;
                    memcpy(result->lfn_refs, lfn_refs,
                           sizeof(result->lfn_refs));
                }
                result->present = true;
                result->ref.present = true;
                result->ref.cluster = current;
                result->ref.lba = (uint32_t)relative_lba;
                result->ref.offset = offset;
                result->first_cluster =
                    efi_fat32_load_le16(slot + 20U) << 16U |
                    efi_fat32_load_le16(slot + 26U);
                result->file_size =
                    efi_fat32_load_le32(slot + 28U);
                result->attributes = attributes;
                return EFI_FAT32_ESP_OK;
            }
        }
        if (!efi_fat32_fat_next(
                esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            return EFI_FAT32_ESP_NOT_FOUND;
        }
        current = next;
        ++steps;
    }
}

static enum efi_fat32_esp_status efi_fat32_dir_run_reserve(
    struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    size_t count,
    struct efi_fat32_dir_run *run)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    uint32_t max_cluster;
    uint32_t cluster_sector;
    size_t entry;
    uint32_t steps;
    uint32_t last_cluster = 0U;

    if (esp == NULL || run == NULL || count == 0U ||
        count > EFI_FAT32_ENTRIES_PER_SECTOR) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(run, 0, sizeof(*run));
    max_cluster = 2U + esp->cluster_count;
    current = directory_cluster;
    steps = 0U;
    while (true) {
        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        last_cluster = current;
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            for (entry = 0U;
                 entry + count <= EFI_FAT32_ENTRIES_PER_SECTOR;
                 ++entry) {
                size_t probe;
                bool free_run = true;

                for (probe = 0U; probe < count; ++probe) {
                    const size_t offset =
                        (entry + probe) *
                        EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;

                    if (sector[offset] != 0x00U &&
                        sector[offset] != 0xE5U) {
                        free_run = false;
                        break;
                    }
                }
                if (!free_run) {
                    continue;
                }
                run->present = true;
                run->cluster = current;
                run->lba = (uint32_t)relative_lba;
                run->offset =
                    entry * EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
                return EFI_FAT32_ESP_OK;
            }
        }
        if (!efi_fat32_fat_next(
                esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            break;
        }
        current = next;
        ++steps;
    }

    status = efi_fat32_allocate_free_cluster(esp, &next);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(esp, last_cluster, next);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(esp, next, EFI_FAT32_EOC);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(
            esp, last_cluster, EFI_FAT32_EOC);
        return status;
    }
    for (cluster_sector = 0U;
         cluster_sector < esp->sectors_per_cluster;
         ++cluster_sector) {
        uint64_t relative_lba =
            esp->data_start_lba +
            (uint64_t)(next - 2U) *
                esp->sectors_per_cluster +
            cluster_sector;

        if (relative_lba >= esp->block_count) {
            return EFI_FAT32_ESP_OUT_OF_RANGE;
        }
        memset(sector, 0, sizeof(sector));
        status = efi_fat32_sector_write(
            esp, relative_lba, sector);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_fat_value_write(esp, next, 0U);
            (void)efi_fat32_fat_value_write(
                esp, last_cluster, EFI_FAT32_EOC);
            return status;
        }
    }
    run->present = true;
    run->cluster = next;
    run->lba = (uint32_t)(
        esp->data_start_lba +
        (uint64_t)(next - 2U) * esp->sectors_per_cluster);
    run->offset = 0U;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_dir_run_write(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_run *run,
    size_t entry_count,
    const unsigned char *entries)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t lba;
    size_t offset;
    size_t remaining;
    const unsigned char *source;

    if (esp == NULL || run == NULL || entries == NULL ||
        !run->present || entry_count == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    lba = run->lba;
    offset = run->offset;
    remaining = entry_count;
    source = entries;
    while (remaining > 0U) {
        size_t slots = (EFI_FAT32_ESP_SECTOR_SIZE - offset) /
                       EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
        size_t bytes;

        if (slots > remaining) {
            slots = remaining;
        }
        bytes = slots * EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
        if (lba >= esp->block_count) {
            return EFI_FAT32_ESP_OUT_OF_RANGE;
        }
        status = efi_fat32_sector_read(esp, lba, sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        memcpy(sector + offset, source, bytes);
        status = efi_fat32_sector_write(esp, lba, sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        source += bytes;
        remaining -= slots;
        offset += bytes;
        if (offset >= EFI_FAT32_ESP_SECTOR_SIZE) {
            offset = 0U;
            lba += 1U;
        }
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_dir_run_clear(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_run *run,
    size_t entry_count)
{
    unsigned char entries[EFI_FAT32_ESP_LFN_ENTRIES_MAX + 1U]
                         [EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE];

    if (entry_count == 0U ||
        entry_count > EFI_FAT32_ESP_LFN_ENTRIES_MAX + 1U) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(entries, 0xE5, sizeof(entries));
    return efi_fat32_dir_run_write(
        esp, run, entry_count, &entries[0U][0U]);
}

static size_t efi_fat32_dir_element_entries(
    const struct efi_fat32_esp_path *path,
    size_t element)
{
    if (path->long_names[element]) {
        return efi_fat32_lfn_entry_count(path->lengths[element]) + 1U;
    }
    return 1U;
}

static enum efi_fat32_esp_status efi_fat32_dir_run_write_element(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_run *run,
    const struct efi_fat32_esp_path *path,
    size_t element,
    bool is_directory,
    uint32_t first_cluster,
    uint32_t file_size,
    size_t *entry_count_out)
{
    unsigned char entries[EFI_FAT32_ESP_LFN_ENTRIES_MAX + 1U]
                         [EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE];
    size_t lfn_entries = 0U;
    unsigned char *short_entry;
    enum efi_fat32_esp_status status;

    if (esp == NULL || run == NULL || path == NULL ||
        element >= path->element_count) {
        return EFI_FAT32_ESP_NULL;
    }
    if (path->long_names[element]) {
        lfn_entries = efi_fat32_lfn_entry_count(
            path->lengths[element]);
        if (lfn_entries == 0U ||
            lfn_entries > EFI_FAT32_ESP_LFN_ENTRIES_MAX) {
            return EFI_FAT32_ESP_PATH_DENIED;
        }
        efi_fat32_lfn_fill(
            &entries[0U][0U], lfn_entries,
            path->elements[element], path->lengths[element],
            path->short_names[element]);
    }
    short_entry = &entries[lfn_entries][0U];
    memset(short_entry, 0, EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE);
    memcpy(short_entry, path->short_names[element],
           EFI_FAT32_ESP_SET_SHORT_NAME_LENGTH);
    short_entry[11U] = is_directory
        ? EFI_FAT32_ATTR_DIRECTORY
        : EFI_FAT32_ATTR_ARCHIVE;
    efi_fat32_store_le16(short_entry + 20U,
        (uint16_t)(first_cluster >> 16U));
    efi_fat32_store_le16(short_entry + 26U,
        (uint16_t)(first_cluster & 0xFFFFU));
    efi_fat32_store_le32(short_entry + 28U, file_size);

    status = efi_fat32_dir_run_write(
        esp, run, lfn_entries + 1U, &entries[0U][0U]);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *entry_count_out = lfn_entries + 1U;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_dir_run_write_children(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_dir_run *run,
    uint32_t self_cluster,
    uint32_t parent_cluster)
{
    unsigned char entries[2U][EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE];

    memset(entries, 0, sizeof(entries));
    efi_fat32_make_dot_name(entries[0U], 1U);
    entries[0U][11U] = EFI_FAT32_ATTR_DIRECTORY;
    efi_fat32_store_le16(entries[0U] + 20U,
        (uint16_t)(self_cluster >> 16U));
    efi_fat32_store_le16(entries[0U] + 26U,
        (uint16_t)(self_cluster & 0xFFFFU));
    efi_fat32_make_dot_name(entries[1U], 2U);
    entries[1U][11U] = EFI_FAT32_ATTR_DIRECTORY;
    efi_fat32_store_le16(entries[1U] + 20U,
        (uint16_t)(parent_cluster >> 16U));
    efi_fat32_store_le16(entries[1U] + 26U,
        (uint16_t)(parent_cluster & 0xFFFFU));
    return efi_fat32_dir_run_write(
        esp, run, 2U, &entries[0U][0U]);
}

static enum efi_fat32_esp_status efi_fat32_dir_ensure_element(
    struct efi_fat32_esp *esp,
    uint32_t parent_cluster,
    const struct efi_fat32_esp_path *path,
    size_t element,
    uint32_t *directory_cluster)
{
    struct efi_fat32_file_ref result;
    struct efi_fat32_dir_run run;
    enum efi_fat32_esp_status status;
    uint32_t created_cluster = 0U;
    size_t entry_count = 0U;

    status = efi_fat32_dir_find_element(
        esp, parent_cluster, path, element, true, &result);
    if (status == EFI_FAT32_ESP_OK) {
        *directory_cluster = result.first_cluster;
        return EFI_FAT32_ESP_OK;
    }
    if (status != EFI_FAT32_ESP_NOT_FOUND) {
        return status;
    }
    status = efi_fat32_allocate_free_cluster(esp, &created_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_fat_value_write(
        esp, created_cluster, EFI_FAT32_EOC);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    /*
     * ITEM-413: the directory this writer creates starts empty.
     *
     * `create_directory` cleared the cluster it allocated and this path did
     * not, so a nested directory kept whatever the volume's data area held
     * where its cluster landed.  A formatted volume keeps the *previous*
     * install's bytes there: its directory clusters.  Those bytes are live to
     * every reader, and this writer's own lookup is one of them - it adopted a
     * stale entry, took the cluster that entry named, and never marked it
     * allocated in the freshly formatted FAT, so the file allocator handed the
     * very same cluster out for file data.  A second install onto the
     * acceptance disk lost its staged boot set that way and refused with
     * `VERIFICATION_FAILED`: no reboot, no boot log, machine left on the
     * stick.
     */
    status = efi_fat32_directory_clear(esp, created_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(esp, created_cluster, 0U);
        return status;
    }
    status = efi_fat32_dir_run_reserve(
        esp, parent_cluster,
        efi_fat32_dir_element_entries(path, element), &run);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_fat_value_write(
            esp, created_cluster, 0U);
        return status;
    }
    status = efi_fat32_dir_run_write_element(
        esp, &run, path, element, true, created_cluster, 0U,
        &entry_count);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_dir_run_clear(esp, &run, entry_count);
        (void)efi_fat32_fat_value_write(
            esp, created_cluster, 0U);
        return status;
    }
    status = efi_fat32_dir_run_reserve(
        esp, created_cluster, 2U, &run);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_run_write_children(
        esp, &run, created_cluster, parent_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *directory_cluster = created_cluster;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_fat_value_read_cursor(
    const struct efi_fat32_esp *esp,
    struct efi_fat32_chain_cursor *cursor,
    uint32_t cluster,
    uint32_t *value)
{
    uint64_t fat_byte_offset;
    uint32_t fat_sector;
    size_t sector_offset;
    enum efi_fat32_esp_status status;

    fat_byte_offset = (uint64_t)cluster * EFI_FAT32_ESP_FAT_ENTRY_BYTES;
    fat_sector = (uint32_t)(fat_byte_offset /
                            esp->bytes_per_sector);
    sector_offset = (size_t)(fat_byte_offset %
                             esp->bytes_per_sector);
    if (!cursor->buffered || cursor->fat_sector != fat_sector) {
        status = efi_fat32_sector_read(
            esp, esp->reserved_sectors + fat_sector,
            cursor->sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        cursor->buffered = true;
        cursor->fat_sector = fat_sector;
    }
    *value = efi_fat32_load_le32(cursor->sector + sector_offset) &
             0x0FFFFFFFU;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_chain_cursor_next(
    const struct efi_fat32_esp *esp,
    struct efi_fat32_chain_cursor *cursor,
    uint32_t cluster,
    uint32_t *next,
    bool *has_next)
{
    enum efi_fat32_esp_status status;
    uint32_t value;

    *has_next = false;
    *next = 0U;
    status = efi_fat32_fat_value_read_cursor(
        esp, cursor, cluster, &value);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (efi_fat32_fat_value_eoc(value)) {
        return EFI_FAT32_ESP_OK;
    }
    if (value < 2U || value >= 2U + esp->cluster_count) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    *next = value;
    *has_next = true;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_fat_update_flush(
    struct efi_fat32_esp *esp,
    struct efi_fat32_fat_update *update)
{
    unsigned char mirror[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t copy_index;
    size_t entry;

    if (!update->active) {
        return EFI_FAT32_ESP_OK;
    }
    status = efi_fat32_sector_write(
        esp, esp->reserved_sectors + update->fat_sector,
        update->sector);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    for (copy_index = 1U; copy_index < esp->fat_count; ++copy_index) {
        uint64_t lba = esp->reserved_sectors +
            (uint64_t)copy_index * esp->sectors_per_fat +
            update->fat_sector;

        status = efi_fat32_sector_read(esp, lba, mirror);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        for (entry = 0U;
             entry < EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR;
             ++entry) {
            if ((update->dirty[entry / 8U] &
                 (unsigned char)(1U << (entry % 8U))) == 0U) {
                continue;
            }
            memcpy(mirror + entry * EFI_FAT32_ESP_FAT_ENTRY_BYTES,
                   update->sector +
                       entry * EFI_FAT32_ESP_FAT_ENTRY_BYTES,
                   EFI_FAT32_ESP_FAT_ENTRY_BYTES);
        }
        status = efi_fat32_sector_write(esp, lba, mirror);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    update->active = false;
    update->fat_sector = 0U;
    memset(update->dirty, 0, sizeof(update->dirty));
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_fat_update_entry(
    struct efi_fat32_esp *esp,
    struct efi_fat32_fat_update *update,
    uint32_t cluster,
    uint32_t value)
{
    uint64_t fat_byte_offset;
    uint32_t fat_sector;
    size_t sector_offset;
    size_t entry_index;
    enum efi_fat32_esp_status status;

    fat_byte_offset = (uint64_t)cluster * EFI_FAT32_ESP_FAT_ENTRY_BYTES;
    fat_sector = (uint32_t)(fat_byte_offset /
                            esp->bytes_per_sector);
    sector_offset = (size_t)(fat_byte_offset %
                             esp->bytes_per_sector);
    entry_index = sector_offset / EFI_FAT32_ESP_FAT_ENTRY_BYTES;
    if (entry_index >= EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    if (update->active && update->fat_sector != fat_sector) {
        status = efi_fat32_fat_update_flush(esp, update);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    if (!update->active) {
        status = efi_fat32_sector_read(
            esp, esp->reserved_sectors + fat_sector,
            update->sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        update->active = true;
        update->fat_sector = fat_sector;
        memset(update->dirty, 0, sizeof(update->dirty));
    }
    efi_fat32_store_le32(update->sector + sector_offset, value);
    update->dirty[entry_index / 8U] |=
        (unsigned char)(1U << (entry_index % 8U));
    return EFI_FAT32_ESP_OK;
}

/*
 * Allocate `count` clusters and link them in ascending order, writing the FAT
 * entries one FAT sector at a time instead of one entry at a time.  The stack
 * cost is one FAT sector, so a multi-megabyte kernel costs no proportional
 * stack, and the allocation never leaves a half-linked chain behind: on a
 * failure the caller frees the chain it was told about.
 */
static enum efi_fat32_esp_status efi_fat32_allocate_chain_batched(
    struct efi_fat32_esp *esp,
    uint32_t count,
    uint32_t *first_cluster)
{
    struct efi_fat32_chain_cursor cursor;
    struct efi_fat32_fat_update update;
    enum efi_fat32_esp_status status;
    uint32_t max_cluster;
    uint32_t cluster;
    uint32_t allocated = 0U;
    uint32_t first = 0U;
    uint32_t previous = 0U;

    if (esp == NULL || first_cluster == NULL || count == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (count > esp->cluster_count) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    memset(&cursor, 0, sizeof(cursor));
    memset(&update, 0, sizeof(update));
    max_cluster = 2U + esp->cluster_count;
    for (cluster = 2U; cluster < max_cluster; ++cluster) {
        uint32_t value;

        if (allocated == count) {
            break;
        }
        status = efi_fat32_fat_value_read_cursor(
            esp, &cursor, cluster, &value);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_fat_update_flush(esp, &update);
            return status;
        }
        if (value != 0U) {
            continue;
        }
        if (allocated == 0U) {
            first = cluster;
        } else {
            status = efi_fat32_fat_update_entry(
                esp, &update, previous, cluster);
            if (status != EFI_FAT32_ESP_OK) {
                (void)efi_fat32_fat_update_flush(esp, &update);
                return status;
            }
        }
        status = efi_fat32_fat_update_entry(
            esp, &update, cluster, EFI_FAT32_EOC);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_fat_update_flush(esp, &update);
            return status;
        }
        previous = cluster;
        ++allocated;
    }
    if (allocated != count) {
        (void)efi_fat32_fat_update_flush(esp, &update);
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_fat_update_flush(esp, &update);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *first_cluster = first;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_free_chain_batched(
    struct efi_fat32_esp *esp,
    uint32_t first_cluster)
{
    struct efi_fat32_chain_cursor cursor;
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t max_cluster;
    uint32_t steps = 0U;

    if (first_cluster == 0U) {
        return EFI_FAT32_ESP_OK;
    }
    memset(&cursor, 0, sizeof(cursor));
    max_cluster = 2U + esp->cluster_count;
    current = first_cluster;
    while (steps <= max_cluster) {
        uint32_t next;
        bool has_next;

        if (current < 2U || current >= max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        status = efi_fat32_chain_cursor_next(
            esp, &cursor, current, &next, &has_next);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        status = efi_fat32_fat_value_write(esp, current, 0U);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        cursor.buffered = false;
        if (!has_next) {
            return EFI_FAT32_ESP_OK;
        }
        current = next;
        ++steps;
    }
    return EFI_FAT32_ESP_CORRUPT;
}

static enum efi_fat32_esp_status efi_fat32_chain_count(
    const struct efi_fat32_esp *esp,
    uint32_t first_cluster,
    uint64_t *count)
{
    struct efi_fat32_chain_cursor cursor;
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t max_cluster;
    uint64_t total = 0U;

    *count = 0U;
    if (first_cluster == 0U) {
        return EFI_FAT32_ESP_OK;
    }
    memset(&cursor, 0, sizeof(cursor));
    max_cluster = 2U + esp->cluster_count;
    current = first_cluster;
    while (true) {
        uint32_t next;
        bool has_next;

        if (current < 2U || current >= max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        status = efi_fat32_chain_cursor_next(
            esp, &cursor, current, &next, &has_next);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        ++total;
        if (total > esp->cluster_count) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            *count = total;
            return EFI_FAT32_ESP_OK;
        }
        current = next;
    }
}

static enum efi_fat32_esp_status efi_fat32_free_cluster_count(
    const struct efi_fat32_esp *esp,
    uint64_t *free_clusters)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t fat_sector;
    uint32_t fat_sectors;
    uint64_t total = 0U;
    uint32_t max_cluster;

    if (esp == NULL || free_clusters == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    max_cluster = 2U + esp->cluster_count;
    fat_sectors = (max_cluster * EFI_FAT32_ESP_FAT_ENTRY_BYTES +
                   esp->bytes_per_sector - 1U) /
                  esp->bytes_per_sector;
    if (fat_sectors == 0U ||
        fat_sectors > EFI_FAT32_ESP_FAT_SCAN_SECTORS_MAX ||
        fat_sectors > esp->sectors_per_fat) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    for (fat_sector = 0U; fat_sector < fat_sectors; ++fat_sector) {
        uint32_t entries = esp->bytes_per_sector /
                           EFI_FAT32_ESP_FAT_ENTRY_BYTES;
        uint32_t entry;

        status = efi_fat32_sector_read(
            esp, esp->reserved_sectors + fat_sector, sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        for (entry = 0U; entry < entries; ++entry) {
            const uint32_t cluster = fat_sector * entries + entry;

            if (cluster < 2U || cluster >= max_cluster) {
                continue;
            }
            if ((efi_fat32_load_le32(
                     sector + entry * EFI_FAT32_ESP_FAT_ENTRY_BYTES) &
                 0x0FFFFFFFU) == 0U) {
                ++total;
            }
        }
    }
    *free_clusters = total;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_dir_cluster_count(
    const struct efi_fat32_esp_path *paths,
    size_t payload_count,
    uint32_t sectors_per_cluster,
    uint64_t *directory_clusters)
{
    const uint64_t entries_per_cluster =
        (uint64_t)EFI_FAT32_ENTRIES_PER_SECTOR *
        sectors_per_cluster;
    uint64_t total = 0U;
    size_t depth;

    if (paths == NULL || directory_clusters == NULL ||
        entries_per_cluster == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    /*
     * Every directory the set touches costs at least one cluster, and a
     * directory whose entries (the `.`/`..` pair plus one entry and one
     * long-name chain per child) do not fit in one cluster costs more.  The
     * root cluster is already allocated by the formatter, so counting it here
     * is a deliberate over-estimate of the requirement.
     */
    for (depth = 0U;
         depth < EFI_FAT32_ESP_PATH_ELEMENTS_MAX; ++depth) {
        size_t index;

        for (index = 0U; index < payload_count; ++index) {
            uint64_t entries;
            uint64_t clusters;
            size_t other;
            bool first_of_group = true;

            if (paths[index].element_count <= depth) {
                continue;
            }
            for (other = 0U; other < index; ++other) {
                if (paths[other].element_count <= depth) {
                    continue;
                }
                if (efi_fat32_path_prefix_equal(
                        &paths[other], &paths[index], depth)) {
                    first_of_group = false;
                    break;
                }
            }
            if (!first_of_group) {
                continue;
            }
            entries = depth == 0U ? 0U : 2U;
            for (other = index; other < payload_count; ++other) {
                size_t probe;
                bool child_seen = false;

                if (paths[other].element_count <= depth ||
                    !efi_fat32_path_prefix_equal(
                        &paths[other], &paths[index], depth)) {
                    continue;
                }
                for (probe = index; probe < other; ++probe) {
                    if (paths[probe].element_count <= depth ||
                        !efi_fat32_path_prefix_equal(
                            &paths[probe], &paths[index], depth)) {
                        continue;
                    }
                    if (efi_fat32_path_element_equal(
                            &paths[probe], depth,
                            &paths[other], depth)) {
                        child_seen = true;
                        break;
                    }
                }
                if (!child_seen) {
                    entries += efi_fat32_dir_element_entries(
                        &paths[other], depth);
                }
            }
            clusters = (entries + entries_per_cluster - 1U) /
                       entries_per_cluster;
            if (clusters == 0U) {
                clusters = 1U;
            }
            total += clusters;
        }
    }
    if (total == 0U) {
        total = 1U;
    }
    *directory_clusters = total;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_payload_set_validate(
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count,
    struct efi_fat32_esp_path *paths,
    uint64_t *total_bytes)
{
    uint64_t total = 0U;
    size_t index;
    size_t other;

    if (payloads == NULL || payload_count == 0U ||
        paths == NULL || total_bytes == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (payload_count > EFI_FAT32_ESP_PAYLOAD_SET_MAX) {
        return EFI_FAT32_ESP_SET_TOO_LARGE;
    }
    for (index = 0U; index < payload_count; ++index) {
        enum efi_fat32_esp_status status;

        if (payloads[index].path == NULL ||
            payloads[index].data == NULL ||
            payloads[index].size == 0U) {
            return EFI_FAT32_ESP_NULL;
        }
        if (payloads[index].size > EFI_FAT32_ESP_FILE_MAX) {
            return EFI_FAT32_ESP_SET_TOO_LARGE;
        }
        total += (uint64_t)payloads[index].size;
        if (total > EFI_FAT32_ESP_PAYLOAD_SET_BYTES_MAX) {
            return EFI_FAT32_ESP_SET_TOO_LARGE;
        }
        status = efi_fat32_path_parse(
            payloads[index].path, &paths[index]);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    /*
     * Two set entries that resolve to the same on-disk name in the same
     * directory would silently overwrite each other; the set is refused
     * instead, before any write.
     */
    for (index = 0U; index < payload_count; ++index) {
        for (other = index + 1U; other < payload_count; ++other) {
            const struct efi_fat32_esp_path *left = &paths[index];
            const struct efi_fat32_esp_path *right = &paths[other];
            size_t parent;

            if (left->element_count != right->element_count) {
                continue;
            }
            parent = left->element_count - 1U;
            if (!efi_fat32_path_prefix_equal(left, right, parent)) {
                continue;
            }
            if (efi_fat32_name_equal(
                    left->short_names[parent],
                    right->short_names[parent])) {
                return EFI_FAT32_ESP_PATH_CONFLICT;
            }
        }
    }
    *total_bytes = total;
    return EFI_FAT32_ESP_OK;
}

static uint64_t efi_fat32_requirement_sectors(
    uint64_t required_clusters)
{
    uint64_t fat_sectors;
    uint64_t volume;
    unsigned int iteration;

    fat_sectors = (2U + required_clusters +
                   EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR - 1U) /
                  EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR;
    if (fat_sectors == 0U) {
        fat_sectors = 1U;
    }
    for (iteration = 0U; iteration < 4U; ++iteration) {
        const uint64_t needed =
            (2U + required_clusters +
             EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR - 1U) /
            EFI_FAT32_ESP_FAT_ENTRIES_PER_SECTOR;

        if (needed <= fat_sectors) {
            break;
        }
        fat_sectors = needed;
    }
    if (fat_sectors > EFI_FAT32_ESP_FAT_SECTORS_MAX) {
        return 0U;
    }
    volume = EFI_FAT32_ESP_RESERVED_SECTORS +
             EFI_FAT32_ESP_FAT_COPIES * fat_sectors +
             required_clusters;
    return volume;
}

enum efi_fat32_esp_status efi_fat32_esp_validate_payload_path(
    const char *path)
{
    struct efi_fat32_esp_path parsed;

    if (path == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    return efi_fat32_path_parse(path, &parsed);
}

enum efi_fat32_esp_status efi_fat32_esp_payload_requirement(
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count,
    uint32_t sectors_per_cluster,
    struct efi_fat32_esp_requirement *requirement)
{
    static struct efi_fat32_esp_path paths[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    enum efi_fat32_esp_status status;
    uint64_t total_bytes = 0U;
    uint64_t cluster_bytes;
    uint64_t payload_clusters = 0U;
    uint64_t directory_clusters = 0U;
    size_t index;

    if (requirement == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(requirement, 0, sizeof(*requirement));
    if (sectors_per_cluster == 0U ||
        sectors_per_cluster > 128U ||
        !efi_fat32_is_power_of_two(sectors_per_cluster)) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    status = efi_fat32_payload_set_validate(
        payloads, payload_count, paths, &total_bytes);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    cluster_bytes = (uint64_t)sectors_per_cluster *
                    EFI_FAT32_ESP_SECTOR_SIZE;
    for (index = 0U; index < payload_count; ++index) {
        payload_clusters +=
            ((uint64_t)payloads[index].size + cluster_bytes - 1U) /
            cluster_bytes;
    }
    status = efi_fat32_dir_cluster_count(
        paths, payload_count, sectors_per_cluster,
        &directory_clusters);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    requirement->version = EFI_FAT32_ESP_PAYLOAD_SET_VERSION;
    requirement->payload_count = payload_count;
    requirement->payload_bytes = total_bytes;
    requirement->payload_clusters = payload_clusters;
    requirement->directory_clusters = directory_clusters;
    requirement->required_clusters =
        payload_clusters + directory_clusters;
    requirement->required_sectors =
        efi_fat32_requirement_sectors(
            requirement->required_clusters);
    if (requirement->required_sectors == 0U) {
        return EFI_FAT32_ESP_SET_TOO_LARGE;
    }
    return EFI_FAT32_ESP_OK;
}

/*
 * Write a payload's bytes through the chain the installer just allocated.
 * Zero-filling the tail of the final sector keeps the staged file
 * byte-for-byte reproducible.
 */
static enum efi_fat32_esp_status efi_fat32_write_chain(
    struct efi_fat32_esp *esp,
    uint32_t first_cluster,
    const unsigned char *data,
    size_t size)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    struct efi_fat32_chain_cursor cursor;
    uint32_t current;
    uint32_t max_cluster;
    uint32_t steps = 0U;
    size_t offset = 0U;

    if (esp == NULL || data == NULL || size == 0U ||
        size > EFI_FAT32_ESP_FILE_MAX) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(&cursor, 0, sizeof(cursor));
    max_cluster = 2U + esp->cluster_count;
    current = first_cluster;
    while (offset < size) {
        uint32_t cluster_sector;
        uint32_t next;
        bool has_next;

        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             offset < size;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            size_t length = size - offset;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            if (length > sizeof(sector)) {
                length = sizeof(sector);
            }
            memset(sector, 0, sizeof(sector));
            memcpy(sector, data + offset, length);
            status = efi_fat32_sector_write(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            offset += length;
        }
        if (offset < size) {
            status = efi_fat32_chain_cursor_next(
                esp, &cursor, current, &next, &has_next);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            if (!has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
            ++steps;
        }
    }
    return EFI_FAT32_ESP_OK;
}

/*
 * Retire the entry a replaced file occupied, including the long-name chain in
 * front of it, so a re-install does not leave two entries for one name.  The
 * backwards scan is bounded by EFI_FAT32_ESP_LFN_ENTRIES_MAX entries and by
 * the directory cluster the entry lives in; an orphaned long-name entry that
 * crosses a cluster boundary is left in place, where every reader ignores it
 * because the short entry in front of it is deleted.
 */
static enum efi_fat32_esp_status efi_fat32_dir_clear_file_entry(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_file_ref *file)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t lba;
    uint32_t cluster_lba;
    size_t offset;
    size_t remaining = EFI_FAT32_ESP_LFN_ENTRIES_MAX;
    bool finished = false;
    bool short_entry_cleared = false;

    if (esp == NULL || file == NULL || !file->present) {
        return EFI_FAT32_ESP_NULL;
    }
    cluster_lba = (uint32_t)(
        esp->data_start_lba +
        (uint64_t)(file->ref.cluster - 2U) *
            esp->sectors_per_cluster);
    lba = file->ref.lba;
    offset = file->ref.offset;
    while (!finished) {
        bool modified = false;

        if (lba < cluster_lba || lba >= esp->block_count) {
            return EFI_FAT32_ESP_OUT_OF_RANGE;
        }
        status = efi_fat32_sector_read(esp, lba, sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        if (!short_entry_cleared && lba == file->ref.lba &&
            offset == file->ref.offset) {
            sector[offset] = 0xE5U;
            modified = true;
            short_entry_cleared = true;
        }
        while (offset > 0U && remaining > 0U) {
            const size_t candidate =
                ((offset / EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE) - 1U) *
                EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;

            if (sector[candidate + 11U] !=
                    EFI_FAT32_ESP_LFN_ATTRIBUTE ||
                (sector[candidate] & 0x3FU) == 0U) {
                finished = true;
                break;
            }
            sector[candidate] = 0xE5U;
            modified = true;
            offset = candidate;
            --remaining;
        }
        if (modified) {
            status = efi_fat32_sector_write(esp, lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
        }
        if (finished || remaining == 0U) {
            break;
        }
        if (offset == 0U) {
            if (lba == cluster_lba) {
                break;
            }
            lba -= 1U;
            offset = EFI_FAT32_ESP_SECTOR_SIZE;
        }
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_file_walk(
    const struct efi_fat32_esp *esp,
    const struct efi_fat32_file_ref *file,
    unsigned char *buffer,
    struct sha256 *digest,
    uint64_t *digest_length,
    const unsigned char *expected)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    struct efi_fat32_chain_cursor cursor;
    uint32_t current;
    uint32_t max_cluster;
    uint32_t steps = 0U;
    size_t offset = 0U;

    if (esp == NULL || file == NULL || !file->present ||
        file->file_size == 0U ||
        file->file_size > EFI_FAT32_ESP_FILE_MAX) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(&cursor, 0, sizeof(cursor));
    max_cluster = 2U + esp->cluster_count;
    current = file->first_cluster;
    while (offset < file->file_size) {
        uint32_t cluster_sector;
        uint32_t next;
        bool has_next;

        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             offset < file->file_size;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            size_t length = file->file_size - offset;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            if (length > sizeof(sector)) {
                length = sizeof(sector);
            }
            status = efi_fat32_sector_read(esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            if (expected != NULL &&
                memcmp(sector, expected + offset, length) != 0) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            if (buffer != NULL) {
                memcpy(buffer + offset, sector, length);
            }
            if (digest != NULL) {
                if (sha256_update(digest, sector, length) !=
                    SHA256_OK) {
                    return EFI_FAT32_ESP_CORRUPT;
                }
                *digest_length += (uint64_t)length;
            }
            offset += length;
        }
        if (offset < file->file_size) {
            status = efi_fat32_chain_cursor_next(
                esp, &cursor, current, &next, &has_next);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            if (!has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
            ++steps;
        }
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_find_payload(
    const struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_path *path,
    bool directory_required,
    struct efi_fat32_file_ref *file)
{
    uint32_t directory_cluster = esp->root_cluster;
    size_t element;

    for (element = 0U; element + 1U < path->element_count;
         ++element) {
        enum efi_fat32_esp_status status =
            efi_fat32_dir_find_element(
                esp, directory_cluster, path, element, true, file);

        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        directory_cluster = file->first_cluster;
    }
    return efi_fat32_dir_find_element(
        esp, directory_cluster, path, path->element_count - 1U,
        directory_required, file);
}

/*
 * Stage the whole payload set.  The capacity of the volume is checked against
 * the set - including the clusters a replaced file gives back - before the
 * first write, so a volume that cannot hold the set is refused instead of
 * receiving a partial set that cannot boot.  A failure after the first write
 * restores every directory entry it changed and frees every chain it
 * allocated.
 */
enum efi_fat32_esp_status efi_fat32_esp_install_payload_set(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count)
{
    static struct efi_fat32_esp_path paths[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static struct efi_fat32_file_ref existing[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static struct efi_fat32_dir_run runs[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static size_t run_entries[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static uint32_t new_first[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static uint32_t old_first[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static uint32_t old_size[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static bool replaced[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static bool entry_written[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    static bool chain_allocated[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    struct efi_fat32_esp_requirement requirement;
    enum efi_fat32_esp_status status;
    uint64_t total_bytes = 0U;
    uint64_t available = 0U;
    uint64_t cluster_bytes;
    size_t index;
    size_t rollback;

    if (esp == NULL || payloads == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_payload_set_validate(
        payloads, payload_count, paths, &total_bytes);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    cluster_bytes = (uint64_t)esp->sectors_per_cluster *
                    esp->bytes_per_sector;
    if (cluster_bytes == 0U) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    memset(&requirement, 0, sizeof(requirement));
    status = efi_fat32_esp_payload_requirement(
        payloads, payload_count, esp->sectors_per_cluster,
        &requirement);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }

    memset(existing, 0, sizeof(existing));
    memset(runs, 0, sizeof(runs));
    memset(run_entries, 0, sizeof(run_entries));
    memset(new_first, 0, sizeof(new_first));
    memset(old_first, 0, sizeof(old_first));
    memset(old_size, 0, sizeof(old_size));
    memset(replaced, 0, sizeof(replaced));
    memset(entry_written, 0, sizeof(entry_written));
    memset(chain_allocated, 0, sizeof(chain_allocated));

    /*
     * Read-only precheck: every file the set will replace is looked up and the
     * clusters it currently holds are counted as available, so a re-install
     * over the same ESP does not need the volume to be empty.
     */
    status = efi_fat32_free_cluster_count(esp, &available);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    for (index = 0U; index < payload_count; ++index) {
        uint64_t reclaimed = 0U;

        status = efi_fat32_find_payload(
            esp, &paths[index], false, &existing[index]);
        if (status == EFI_FAT32_ESP_OK) {
            replaced[index] = true;
            old_first[index] = existing[index].first_cluster;
            old_size[index] = existing[index].file_size;
            status = efi_fat32_chain_count(
                esp, existing[index].first_cluster, &reclaimed);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
        } else if (status != EFI_FAT32_ESP_NOT_FOUND) {
            return status;
        }
        if (reclaimed > UINT64_MAX - available) {
            return EFI_FAT32_ESP_CAPACITY;
        }
        available += reclaimed;
    }
    if (available < requirement.required_clusters) {
        return EFI_FAT32_ESP_CAPACITY;
    }

    for (index = 0U; index < payload_count; ++index) {
        const uint64_t clusters =
            ((uint64_t)payloads[index].size + cluster_bytes - 1U) /
            cluster_bytes;
        uint32_t first_cluster = 0U;
        uint32_t directory_cluster = esp->root_cluster;
        size_t element;

        if (clusters == 0U || clusters > UINT32_MAX) {
            status = EFI_FAT32_ESP_SET_TOO_LARGE;
            goto rollback;
        }
        for (element = 0U;
             element + 1U < paths[index].element_count; ++element) {
            status = efi_fat32_dir_ensure_element(
                esp, directory_cluster, &paths[index], element,
                &directory_cluster);
            if (status != EFI_FAT32_ESP_OK) {
                goto rollback;
            }
        }
        status = efi_fat32_allocate_chain_batched(
            esp, (uint32_t)clusters, &first_cluster);
        if (status != EFI_FAT32_ESP_OK) {
            goto rollback;
        }
        chain_allocated[index] = true;
        new_first[index] = first_cluster;
        status = efi_fat32_write_chain(
            esp, first_cluster, payloads[index].data,
            payloads[index].size);
        if (status != EFI_FAT32_ESP_OK) {
            goto rollback;
        }
        status = efi_fat32_dir_run_reserve(
            esp, directory_cluster,
            efi_fat32_dir_element_entries(&paths[index],
                paths[index].element_count - 1U),
            &runs[index]);
        if (status != EFI_FAT32_ESP_OK) {
            goto rollback;
        }
        status = efi_fat32_dir_run_write_element(
            esp, &runs[index], &paths[index],
            paths[index].element_count - 1U, false, first_cluster,
            (uint32_t)payloads[index].size, &run_entries[index]);
        if (status != EFI_FAT32_ESP_OK) {
            goto rollback;
        }
        entry_written[index] = true;
        if (replaced[index]) {
            status = efi_fat32_dir_clear_file_entry(
                esp, &existing[index]);
            if (status != EFI_FAT32_ESP_OK) {
                goto rollback;
            }
            existing[index].present = false;
        }
    }

    status = efi_fat32_flush_device(esp);
    if (status != EFI_FAT32_ESP_OK) {
        status = EFI_FAT32_ESP_FLUSH_ERROR;
        goto rollback;
    }
    for (index = 0U; index < payload_count; ++index) {
        if (replaced[index] && old_first[index] != 0U &&
            old_first[index] != new_first[index]) {
            (void)efi_fat32_free_chain_batched(
                esp, old_first[index]);
        }
    }
    return EFI_FAT32_ESP_OK;

rollback:
    for (rollback = payload_count; rollback > 0U; --rollback) {
        const size_t position = rollback - 1U;

        if (entry_written[position]) {
            if (replaced[position]) {
                size_t restored = 0U;

                /*
                 * The old file's bytes are still on the volume: the entry
                 * this install wrote points at the new chain, so restoring
                 * the old first cluster and length restores the file.
                 */
                (void)efi_fat32_dir_run_write_element(
                    esp, &runs[position], &paths[position],
                    paths[position].element_count - 1U, false,
                    old_first[position], old_size[position],
                    &restored);
            } else {
                (void)efi_fat32_dir_run_clear(
                    esp, &runs[position], run_entries[position]);
            }
        }
        if (chain_allocated[position]) {
            (void)efi_fat32_free_chain_batched(
                esp, new_first[position]);
        }
    }
    return status == EFI_FAT32_ESP_OK ? EFI_FAT32_ESP_IO_ERROR
                                      : status;
}

enum efi_fat32_esp_status efi_fat32_esp_verify_payload_set(
    const struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_payload *payloads,
    size_t payload_count,
    size_t sealed_count,
    unsigned char sealed_digest[SHA256_DIGEST_SIZE],
    uint64_t *sealed_length)
{
    static struct efi_fat32_esp_path paths[EFI_FAT32_ESP_PAYLOAD_SET_MAX];
    struct sha256 digest;
    enum efi_fat32_esp_status status;
    uint64_t total_bytes = 0U;
    uint64_t length = 0U;
    bool hashing;
    size_t index;

    if (esp == NULL || payloads == NULL ||
        sealed_length == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (sealed_count > payload_count) {
        return EFI_FAT32_ESP_SET_TOO_LARGE;
    }
    if (sealed_count != 0U && sealed_digest == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    status = efi_fat32_payload_set_validate(
        payloads, payload_count, paths, &total_bytes);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    hashing = sealed_count != 0U;
    if (hashing &&
        sha256_init(&digest) != SHA256_OK) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    for (index = 0U; index < payload_count; ++index) {
        struct efi_fat32_file_ref file;

        status = efi_fat32_find_payload(
            esp, &paths[index], false, &file);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        if (!file.present ||
            file.file_size != (uint32_t)payloads[index].size) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        status = efi_fat32_file_walk(
            esp, &file, NULL,
            index < sealed_count ? &digest : NULL, &length,
            payloads[index].data);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    if (hashing) {
        if (sha256_final(&digest, sealed_digest) != SHA256_OK) {
            return EFI_FAT32_ESP_CORRUPT;
        }
    }
    *sealed_length = length;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_read_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    unsigned char *buffer,
    size_t capacity,
    size_t *size_out)
{
    static struct efi_fat32_esp_path parsed;
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;

    if (esp == NULL || path == NULL || buffer == NULL ||
        size_out == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_path_parse(path, &parsed);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_find_payload(esp, &parsed, false, &file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (file.file_size == 0U || file.file_size > capacity ||
        file.file_size > EFI_FAT32_ESP_FILE_MAX) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_file_walk(
        esp, &file, buffer, NULL, NULL, NULL);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *size_out = file.file_size;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_digest_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    unsigned char digest[SHA256_DIGEST_SIZE],
    size_t *size_out)
{
    static struct efi_fat32_esp_path parsed;
    struct efi_fat32_file_ref file;
    struct sha256 context;
    enum efi_fat32_esp_status status;
    uint64_t length = 0U;

    if (esp == NULL || path == NULL || digest == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_path_parse(path, &parsed);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_find_payload(esp, &parsed, false, &file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (sha256_init(&context) != SHA256_OK) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    status = efi_fat32_file_walk(
        esp, &file, NULL, &context, &length, NULL);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (length != file.file_size ||
        sha256_final(&context, digest) != SHA256_OK) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    if (size_out != NULL) {
        *size_out = file.file_size;
    }
    return EFI_FAT32_ESP_OK;
}

/*
 * ITEM-517: the shared bind body, addressed by the device rather than by an EFI
 * environment.  `efi_fat32_esp_bind()` keeps its exact validation order - the
 * environment checks first, then everything below - and the legacy-BIOS boot
 * path reaches the same body through `efi_fat32_esp_bind_device()` without
 * constructing an EFI environment it does not have.
 */
static enum efi_fat32_esp_status efi_fat32_esp_bind_validated(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count,
    struct efi_fat32_esp *esp)
{
    enum block_device_status block_status;
    enum efi_fat32_esp_status status;
    unsigned char boot_sector[EFI_FAT32_ESP_SECTOR_SIZE];
    uint64_t fat_region_blocks;
    uint64_t volume_sectors;
    uint32_t cluster_count;
    uint32_t root_cluster;
    uint16_t reserved_sectors;
    uint16_t bytes_per_sector;
    uint16_t total_sectors16;
    uint8_t sectors_per_cluster;
    uint8_t fat_count;
    uint32_t sectors_per_fat;
    uint32_t total_sectors32;

    if (first_lba > device->block_count ||
        block_count == 0U ||
        block_count > device->block_count - first_lba) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    /*
     * ITEM-405: this module's bookkeeping - `data_start_lba`, the directory
     * references and the cluster arithmetic that derives an LBA from a cluster
     * - is 32-bit, so a volume whose sector count does not fit it cannot be
     * addressed correctly.  Refusing is the honest answer: a truncating
     * conversion would still land inside `block_count` and silently read or
     * write the wrong sector.
     */
    if (block_count > UINT32_MAX) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    if (device->block_size != EFI_FAT32_ESP_SECTOR_SIZE) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    block_status = block_device_validate(device);
    if (block_status != BLOCK_DEVICE_OK) {
        return EFI_FAT32_ESP_BAD_DEVICE;
    }

    memset(esp, 0, sizeof(*esp));
    status = efi_fat32_map_block_status(
        block_device_read(
            device, first_lba * device->block_size, boot_sector,
            EFI_FAT32_ESP_SECTOR_SIZE));
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (boot_sector[510U] != 0x55U ||
        boot_sector[511U] != 0xAAU) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    bytes_per_sector = efi_fat32_load_le16(boot_sector + 11U);
    sectors_per_cluster = boot_sector[13U];
    reserved_sectors = efi_fat32_load_le16(boot_sector + 14U);
    fat_count = boot_sector[16U];
    total_sectors16 = efi_fat32_load_le16(boot_sector + 19U);
    total_sectors32 = efi_fat32_load_le32(boot_sector + 32U);
    sectors_per_fat = efi_fat32_load_le32(boot_sector + 36U);
    root_cluster = efi_fat32_load_le32(boot_sector + 44U);

    if (bytes_per_sector != EFI_FAT32_ESP_SECTOR_SIZE ||
        sectors_per_cluster == 0U ||
        sectors_per_cluster > 128U ||
        !efi_fat32_is_power_of_two(sectors_per_cluster) ||
        reserved_sectors == 0U ||
        fat_count < 1U || fat_count > 2U ||
        sectors_per_fat == 0U ||
        root_cluster < 2U) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    /*
     * ITEM-405: the volume has to fit the partition it is bound inside.
     *
     * Until this item the bind checked the boot sector only against itself and
     * against the partition length it was handed, and never read the volume's
     * own total-sector count (FAT32 offset 32, FAT16 offset 19).  A disk that
     * had carried a foreign system therefore bound successfully on a stale
     * FAT32 whose metadata still described the *old* volume: the acceptance
     * run's ESP declared 204800 sectors (100 MiB) inside a 67584-sector
     * (33 MiB) role, so the installer treated it as a usable ESP, never
     * formatted it, and staged the installed boot set against a geometry the
     * volume does not have.
     *
     * The declared count is now authoritative for the volume's extent: it must
     * exist, it must not exceed the partition, and the data region the FAT
     * describes is measured against it.  `cluster_count` is derived from the
     * smaller of the two numbers, never from the partition alone, so every
     * cluster LBA this module computes stays inside the volume it actually
     * describes - and therefore inside the partition.
     */
    volume_sectors = total_sectors32 != 0U
                         ? (uint64_t)total_sectors32
                         : (uint64_t)total_sectors16;
    if (volume_sectors == 0U || volume_sectors > block_count) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    fat_region_blocks = (uint64_t)reserved_sectors +
        (uint64_t)fat_count * sectors_per_fat;
    if (fat_region_blocks >= volume_sectors) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    cluster_count = (uint32_t)(
        (volume_sectors - fat_region_blocks) /
        sectors_per_cluster);
    if (cluster_count < 1U) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    if (fat_region_blocks +
            (uint64_t)cluster_count * sectors_per_cluster >
        volume_sectors) {
        return EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY;
    }
    if ((uint64_t)sectors_per_fat * 128U <
        2U + (uint64_t)cluster_count) {
        return EFI_FAT32_ESP_CORRUPT;
    }

    esp->version = EFI_FAT32_ESP_VERSION;
    esp->bound = true;
    esp->bytes_per_sector = bytes_per_sector;
    esp->sectors_per_cluster = sectors_per_cluster;
    esp->reserved_sectors = reserved_sectors;
    esp->fat_count = fat_count;
    esp->sectors_per_fat = sectors_per_fat;
    esp->root_cluster = root_cluster;
    esp->data_start_lba = (uint32_t)fat_region_blocks;
    esp->cluster_count = cluster_count;
    /*
     * The volume's own extent, as the boot sector declares it.  It is never
     * larger than `block_count`, and it is what the cluster space is derived
     * from and what a caller can quote when it reports the volume it bound.
     */
    esp->volume_sectors = (uint32_t)volume_sectors;
    esp->first_lba = first_lba;
    esp->block_count = block_count;
    esp->device = *device;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_bind_device(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t block_count,
    struct efi_fat32_esp *esp)
{
    if (device == NULL || esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    return efi_fat32_esp_bind_validated(
        device, first_lba, block_count, esp);
}

enum efi_fat32_esp_status efi_fat32_esp_bind(
    const struct efi_boot_environment *environment,
    uint64_t first_lba,
    uint64_t block_count,
    struct efi_fat32_esp *esp)
{
    if (environment == NULL || esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return EFI_FAT32_ESP_BAD_ENVIRONMENT;
    }
    return efi_fat32_esp_bind_validated(
        environment->block_device, first_lba, block_count, esp);
}

/*
 * ITEM-396: write one file into one directory of a bound ESP.  This is the body
 * the HFOC boot payload always used, parameterised by the directory and the 8.3
 * name, so the Limine configuration and the boot payload share one write path -
 * one place that allocates a cluster chain, one place that updates a directory
 * entry, one place that rolls back on a flush failure.
 */
static enum efi_fat32_esp_status efi_fat32_file_install(
    struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    const unsigned char name[11U],
    const unsigned char *payload,
    size_t payload_size)
{
    struct efi_fat32_file_ref file;
    struct efi_fat32_dir_ref slot;
    enum efi_fat32_esp_status status;
    uint32_t first_cluster = 0U;
    uint32_t required_clusters;
    size_t cluster_bytes;
    bool replaced = false;
    bool entry_created = false;
    uint32_t old_first_cluster = 0U;
    uint32_t old_size = 0U;

    if (esp == NULL || name == NULL || payload == NULL ||
        payload_size == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (payload_size > EFI_FAT32_ESP_PAYLOAD_MAX) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    cluster_bytes = (size_t)esp->sectors_per_cluster *
                    esp->bytes_per_sector;
    required_clusters = (uint32_t)(
        (payload_size + cluster_bytes - 1U) / cluster_bytes);
    if (required_clusters > EFI_FAT32_ESP_MAX_PAYLOAD_CLUSTERS ||
        required_clusters > esp->cluster_count) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_dir_find(
        esp, directory_cluster, name, &file);
    if (status != EFI_FAT32_ESP_OK &&
        status != EFI_FAT32_ESP_NOT_FOUND) {
        return status;
    }
    if (file.present) {
        replaced = true;
        old_first_cluster = file.first_cluster;
        old_size = file.file_size;
    }

    status = efi_fat32_allocate_chain(
        esp, required_clusters, &first_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_write_payload_data(
        esp, first_cluster, payload, payload_size,
        required_clusters);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_free_chain(esp, first_cluster);
        return status;
    }

    if (replaced) {
        status = efi_fat32_dir_entry_update(
            esp, &file.ref, first_cluster,
            (uint32_t)payload_size);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_free_chain(esp, first_cluster);
            return status;
        }
    } else {
        status = efi_fat32_dir_reserve_slot(
            esp, directory_cluster, &slot);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_free_chain(esp, first_cluster);
            return status;
        }
        entry_created = true;
        status = efi_fat32_dir_entry_write(
            esp, &slot, name, EFI_FAT32_ATTR_ARCHIVE,
            first_cluster, (uint32_t)payload_size);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_dir_entry_clear(esp, &slot);
            (void)efi_fat32_free_chain(esp, first_cluster);
            return status;
        }
    }

    status = efi_fat32_flush_device(esp);
    if (status == EFI_FAT32_ESP_FLUSH_ERROR ||
        status == EFI_FAT32_ESP_IO_ERROR) {
        if (replaced) {
            (void)efi_fat32_dir_entry_update(
                esp, &file.ref, old_first_cluster, old_size);
        } else if (entry_created) {
            (void)efi_fat32_dir_entry_clear(esp, &slot);
        }
        (void)efi_fat32_free_chain(esp, first_cluster);
        return EFI_FAT32_ESP_FLUSH_ERROR;
    }
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }

    if (replaced && old_first_cluster != first_cluster &&
        old_first_cluster != 0U) {
        (void)efi_fat32_free_chain(esp, old_first_cluster);
    }
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_install_payload(
    struct efi_fat32_esp *esp,
    const unsigned char *payload,
    size_t payload_size)
{
    enum efi_fat32_esp_status status;
    uint32_t efi_cluster;
    uint32_t hfoc_cluster;
    uint32_t required_clusters;
    size_t cluster_bytes;

    if (esp == NULL || payload == NULL || payload_size == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    /*
     * The capacity refusal stays where it always was - before the first write,
     * so a payload this volume cannot hold does not create the EFI directory on
     * its way to being refused.  `efi_fat32_file_install()` re-checks it for the
     * callers that reach it directly.
     */
    cluster_bytes = (size_t)esp->sectors_per_cluster *
                    esp->bytes_per_sector;
    required_clusters = (uint32_t)(
        (payload_size + cluster_bytes - 1U) / cluster_bytes);
    if (payload_size > EFI_FAT32_ESP_PAYLOAD_MAX ||
        required_clusters > EFI_FAT32_ESP_MAX_PAYLOAD_CLUSTERS ||
        required_clusters > esp->cluster_count) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_ensure_directory(
        esp, esp->root_cluster, efi_fat32_name_efi,
        &efi_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_ensure_directory(
        esp, efi_cluster, efi_fat32_name_hfoc,
        &hfoc_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    return efi_fat32_file_install(
        esp, hfoc_cluster, efi_fat32_name_bootx64,
        payload, payload_size);
}

enum efi_fat32_esp_status efi_fat32_esp_verify_payload(
    const struct efi_fat32_esp *esp)
{
    struct efi_fat32_file_ref file;
    struct efi_fat32_file_ref efi_dir;
    struct efi_fat32_file_ref hfoc_dir;
    enum efi_fat32_esp_status status;

    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_dir_find(
        esp, esp->root_cluster, efi_fat32_name_efi,
        &efi_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_find(
        esp, efi_dir.first_cluster, efi_fat32_name_hfoc,
        &hfoc_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_find(
        esp, hfoc_dir.first_cluster,
        efi_fat32_name_bootx64, &file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (!file.present || file.file_size == 0U ||
        file.first_cluster < 2U) {
        return EFI_FAT32_ESP_NOT_FOUND;
    }
    return EFI_FAT32_ESP_OK;
}

/*
 * ITEM-396: locate the HFOC directory of a bound ESP.  Every file entry point
 * below resolves the same two levels, so a payload that verifies and a
 * configuration that verifies are found the same way.
 */
static enum efi_fat32_esp_status efi_fat32_hfoc_directory(
    const struct efi_fat32_esp *esp,
    uint32_t *cluster)
{
    struct efi_fat32_file_ref efi_dir;
    enum efi_fat32_esp_status status;

    status = efi_fat32_dir_find(
        esp, esp->root_cluster, efi_fat32_name_efi, &efi_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_find(
        esp, efi_dir.first_cluster, efi_fat32_name_hfoc,
        &efi_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    *cluster = efi_dir.first_cluster;
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_file_locate(
    const struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    const unsigned char name[11U],
    struct efi_fat32_file_ref *file)
{
    enum efi_fat32_esp_status status;

    status = efi_fat32_dir_find(
        esp, directory_cluster, name, file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (!file->present || file->file_size == 0U ||
        file->first_cluster < 2U) {
        return EFI_FAT32_ESP_NOT_FOUND;
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_file_read(
    const struct efi_fat32_esp *esp,
    const struct efi_fat32_file_ref *file,
    unsigned char *payload,
    size_t capacity,
    size_t *payload_size)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    size_t file_offset;
    size_t read_length;
    uint32_t steps;

    if (file->file_size > capacity) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    current = file->first_cluster;
    file_offset = 0U;
    steps = 0U;
    while (file_offset < file->file_size) {
        uint32_t cluster_sector;

        if (current < 2U ||
            current >= 2U + esp->cluster_count ||
            steps > esp->cluster_count) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             file_offset < file->file_size;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            read_length = file->file_size - file_offset;
            if (read_length > sizeof(sector)) {
                read_length = sizeof(sector);
            }
            memcpy(payload + file_offset, sector, read_length);
            file_offset += read_length;
        }
        if (file_offset >= file->file_size) {
            break;
        }
        if (!efi_fat32_fat_next(esp, current, &next, &has_next) ||
            !has_next) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        current = next;
        ++steps;
    }
    *payload_size = file_offset;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_install_config(
    struct efi_fat32_esp *esp,
    const unsigned char *config,
    size_t config_length)
{
    enum efi_fat32_esp_status status;
    uint32_t efi_cluster;
    uint32_t hfoc_cluster;

    if (esp == NULL || config == NULL || config_length == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (config_length > EFI_FAT32_ESP_CONFIG_MAX) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_ensure_directory(
        esp, esp->root_cluster, efi_fat32_name_efi,
        &efi_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_ensure_directory(
        esp, efi_cluster, efi_fat32_name_hfoc, &hfoc_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    return efi_fat32_file_install(
        esp, hfoc_cluster, efi_fat32_name_limine,
        config, config_length);
}

enum efi_fat32_esp_status efi_fat32_esp_verify_config(
    const struct efi_fat32_esp *esp)
{
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;
    uint32_t hfoc_cluster = 0U;

    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_hfoc_directory(esp, &hfoc_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    return efi_fat32_file_locate(
        esp, hfoc_cluster, efi_fat32_name_limine, &file);
}

enum efi_fat32_esp_status efi_fat32_esp_read_config(
    const struct efi_fat32_esp *esp,
    unsigned char *config,
    size_t capacity,
    size_t *config_length)
{
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;
    uint32_t hfoc_cluster = 0U;

    if (esp == NULL || config == NULL || config_length == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_hfoc_directory(esp, &hfoc_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_file_locate(
        esp, hfoc_cluster, efi_fat32_name_limine, &file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    return efi_fat32_file_read(esp, &file, config, capacity, config_length);
}

enum efi_fat32_esp_status efi_fat32_esp_free_clusters(
    const struct efi_fat32_esp *esp,
    uint32_t *free_clusters)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t sector_index;
    uint32_t cluster;
    uint32_t free_count = 0U;

    if (esp == NULL || free_clusters == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    *free_clusters = 0U;
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (esp->sectors_per_fat > EFI_FAT32_ESP_FREE_SCAN_MAX_SECTORS) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    cluster = 0U;
    for (sector_index = 0U;
         sector_index < esp->sectors_per_fat; ++sector_index) {
        size_t entry;

        if ((uint64_t)esp->reserved_sectors + sector_index >=
            esp->block_count) {
            return EFI_FAT32_ESP_OUT_OF_RANGE;
        }
        status = efi_fat32_sector_read(
            esp, esp->reserved_sectors + sector_index, sector);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        for (entry = 0U;
             entry < EFI_FAT32_ESP_SECTOR_SIZE /
                         sizeof(uint32_t);
             ++entry) {
            if (cluster >= esp->cluster_count) {
                break;
            }
            /*
             * The first two FAT entries describe the volume itself and the end
             * of chain marker, not data clusters.
             */
            if (cluster >= 2U &&
                efi_fat32_load_le32(sector + entry * 4U) == 0U) {
                ++free_count;
            }
            ++cluster;
        }
    }
    *free_clusters = free_count;
    return EFI_FAT32_ESP_OK;
}

/*
 * ITEM-396 loader inventory.  The vendor subdirectory names are copied out of
 * the directory entries as 8.3 text, which is all the menu needs: it prints the
 * vendor's product name from its own table and uses the path for the chainload.
 */
static void efi_fat32_name_to_text(
    const unsigned char *entry,
    char *text,
    size_t capacity)
{
    size_t length = 0U;
    size_t index;

    for (index = 0U; index < capacity; ++index) {
        text[index] = '\0';
    }
    for (index = 0U; index < 8U && entry[index] != (unsigned char)' ';
         ++index) {
        if (length + 1U >= capacity) {
            return;
        }
        text[length++] = (char)entry[index];
    }
    if (entry[8U] != (unsigned char)' ' && length + 1U < capacity) {
        text[length++] = '.';
        for (index = 8U; index < 11U &&
                         entry[index] != (unsigned char)' ';
             ++index) {
            if (length + 1U >= capacity) {
                break;
            }
            text[length++] = (char)entry[index];
        }
    }
    text[length] = '\0';
}

static enum efi_fat32_esp_status efi_fat32_make_name(
    const char *text,
    unsigned char name[11U])
{
    size_t index;
    size_t position = 0U;
    bool extension = false;

    for (index = 0U; index < 11U; ++index) {
        name[index] = (unsigned char)' ';
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        unsigned char value = (unsigned char)text[index];

        if (value == (unsigned char)'.' && !extension) {
            extension = true;
            position = 8U;
            continue;
        }
        if (value >= (unsigned char)'a' &&
            value <= (unsigned char)'z') {
            value = (unsigned char)(value - 32U);
        }
        if (extension) {
            if (position >= 11U) {
                return EFI_FAT32_ESP_PATH_DENIED;
            }
        } else if (position >= 8U) {
            return EFI_FAT32_ESP_PATH_DENIED;
        }
        name[position++] = value;
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_collect_vendors(
    const struct efi_fat32_esp *esp,
    uint32_t directory_cluster,
    char vendors[EFI_FAT32_ESP_LOADER_CAPACITY]
                 [EFI_FAT32_ESP_VENDOR_LENGTH],
    uint32_t vendor_clusters[EFI_FAT32_ESP_LOADER_CAPACITY],
    size_t capacity,
    size_t *count)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current = directory_cluster;
    uint32_t next;
    bool has_next;
    uint32_t steps = 0U;
    size_t found = 0U;

    while (true) {
        uint32_t cluster_sector;

        if (current < 2U ||
            current >= 2U + esp->cluster_count ||
            steps > esp->cluster_count) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            size_t entry;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            for (entry = 0U; entry < EFI_FAT32_ENTRIES_PER_SECTOR;
                 ++entry) {
                const size_t offset =
                    entry * EFI_FAT32_ESP_DIRECTORY_ENTRY_SIZE;
                const unsigned char attributes = sector[offset + 11U];

                if (sector[offset] == 0x00U) {
                    *count = found;
                    return EFI_FAT32_ESP_OK;
                }
                if (sector[offset] == 0xE5U ||
                    attributes == EFI_FAT32_ATTR_LONG_NAME ||
                    (attributes & EFI_FAT32_ATTR_VOLUME_ID) != 0U ||
                    (attributes & EFI_FAT32_ATTR_DIRECTORY) == 0U) {
                    continue;
                }
                if (sector[offset] == (unsigned char)'.') {
                    continue;
                }
                /*
                 * ITEM-396: `\EFI\HFOC` is the appliance's own directory.  The
                 * inventory reports the loaders that belong to *other* systems,
                 * so the menu never offers the appliance as its own chainload
                 * target and a before/after comparison of the ESP is meaningful.
                 */
                if (efi_fat32_name_equal(
                        sector + offset,
                        (const unsigned char[11U]) {
                            'H', 'F', 'O', 'C', ' ', ' ', ' ',
                            ' ', ' ', ' ', ' '
                        })) {
                    continue;
                }
                if (found == capacity) {
                    /* The inventory is bounded by the caller's capacity. */
                    *count = found;
                    return EFI_FAT32_ESP_OK;
                }
                efi_fat32_name_to_text(
                    sector + offset, vendors[found],
                    EFI_FAT32_ESP_VENDOR_LENGTH);
                vendor_clusters[found] = (uint32_t)(
                    efi_fat32_load_le16(sector + offset + 20U) << 16U |
                    efi_fat32_load_le16(sector + offset + 26U));
                ++found;
            }
        }
        if (!efi_fat32_fat_next(esp, current, &next, &has_next)) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (!has_next) {
            *count = found;
            return EFI_FAT32_ESP_OK;
        }
        current = next;
        ++steps;
    }
}

enum efi_fat32_esp_status efi_fat32_esp_list_loaders(
    const struct efi_fat32_esp *esp,
    struct efi_fat32_esp_loader *loaders,
    size_t capacity,
    size_t *count)
{
    char vendors[EFI_FAT32_ESP_LOADER_CAPACITY]
                [EFI_FAT32_ESP_VENDOR_LENGTH];
    uint32_t vendor_clusters[EFI_FAT32_ESP_LOADER_CAPACITY];
    struct efi_fat32_file_ref efi_dir;
    enum efi_fat32_esp_status status;
    size_t vendor_count = 0U;
    size_t found = 0U;
    size_t index;
    size_t unreadable_vendors = 0U;

    if (esp == NULL || count == NULL ||
        (loaders == NULL && capacity != 0U)) {
        return EFI_FAT32_ESP_NULL;
    }
    *count = 0U;
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (capacity == 0U) {
        return EFI_FAT32_ESP_OK;
    }
    if (capacity < EFI_FAT32_ESP_LOADER_CAPACITY) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_dir_find(
        esp, esp->root_cluster, efi_fat32_name_efi, &efi_dir);
    if (status == EFI_FAT32_ESP_NOT_FOUND) {
        /* An ESP with no EFI directory carries nothing to chainload. */
        return EFI_FAT32_ESP_OK;
    }
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (!efi_dir.present || efi_dir.first_cluster < 2U) {
        return EFI_FAT32_ESP_NOT_FOUND;
    }
    memset(vendors, 0, sizeof(vendors));
    memset(vendor_clusters, 0, sizeof(vendor_clusters));
    status = efi_fat32_collect_vendors(
        esp, efi_dir.first_cluster, vendors, vendor_clusters,
        EFI_FAT32_ESP_LOADER_CAPACITY, &vendor_count);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    for (index = 0U; index < vendor_count; ++index) {
        size_t candidate;

        if (vendor_clusters[index] < 2U) {
            continue;
        }
        for (candidate = 0U;
             candidate < EFI_FAT32_LOADER_NAME_COUNT; ++candidate) {
            unsigned char name[11U];
            struct efi_fat32_file_ref file;
            size_t length = 0U;

            status = efi_fat32_make_name(
                efi_fat32_loader_names[candidate], name);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            status = efi_fat32_dir_find(
                esp, vendor_clusters[index], name, &file);
            if (status == EFI_FAT32_ESP_NOT_FOUND) {
                continue;
            }
            if (status != EFI_FAT32_ESP_OK) {
                /*
                 * ITEM-409: one vendor directory this volume cannot walk does
                 * not discard the systems the other directories already gave
                 * us.  The machine's ESP is shared with whatever else was
                 * installed on it - a leftover `\EFI\Microsoft`, a directory
                 * whose chain the previous installer rewrote - and returning
                 * here threw away a loader that had been found and read
                 * successfully, which is how a machine that carries Debian
                 * ended up with a menu that offered nothing.  The inventory
                 * keeps what it found; a volume where nothing at all could be
                 * read still reports its own status.
                 */
                unreadable_vendors += 1U;
                break;
            }
            if (!file.present || file.file_size == 0U) {
                continue;
            }
            memset(&loaders[found], 0, sizeof(loaders[found]));
            for (length = 0U;
                 length + 1U < EFI_FAT32_ESP_VENDOR_LENGTH &&
                 vendors[index][length] != '\0';
                 ++length) {
                loaders[found].vendor[length] = vendors[index][length];
            }
            loaders[found].size = file.file_size;
            {
                size_t path_length = 0U;

                static const char prefix[] = "\\EFI\\";

                for (length = 0U; length + 1U < sizeof(prefix) &&
                                  prefix[length] != '\0';
                     ++length) {
                    loaders[found].path[path_length++] = prefix[length];
                }
                for (length = 0U;
                     path_length + 1U <
                         EFI_FAT32_ESP_LOADER_PATH_LENGTH &&
                     loaders[found].vendor[length] != '\0';
                     ++length) {
                    loaders[found].path[path_length++] =
                        loaders[found].vendor[length];
                }
                if (path_length + 1U <
                    EFI_FAT32_ESP_LOADER_PATH_LENGTH) {
                    loaders[found].path[path_length++] = '\\';
                }
                for (length = 0U;
                     path_length + 1U <
                         EFI_FAT32_ESP_LOADER_PATH_LENGTH &&
                     efi_fat32_loader_names[candidate][length] != '\0';
                     ++length) {
                    loaders[found].path[path_length++] =
                        efi_fat32_loader_names[candidate][length];
                }
                loaders[found].path[path_length] = '\0';
            }
            ++found;
            break;
        }
    }
    for (index = 0U; index < found; ++index) {
        if (loaders[index].path[0] == '\0' ||
            loaders[index].vendor[0] == '\0') {
            return EFI_FAT32_ESP_BAD_STATE;
        }
    }
    if (found == 0U && unreadable_vendors != 0U) {
        /*
         * Nothing was found anywhere and at least one vendor directory could
         * not be walked: the volume's honest answer is that it is unreadable,
         * not that it carries nothing.
         */
        return EFI_FAT32_ESP_CORRUPT;
    }
    *count = found;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_read_payload(
    const struct efi_fat32_esp *esp,
    unsigned char *payload,
    size_t capacity,
    size_t *payload_size)
{
    struct efi_fat32_file_ref efi_dir;
    struct efi_fat32_file_ref hfoc_dir;
    struct efi_fat32_file_ref file;
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    enum efi_fat32_esp_status status;
    uint32_t current;
    uint32_t next;
    bool has_next;
    size_t file_offset;
    size_t read_length;

    if (esp == NULL || payload == NULL ||
        payload_size == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_dir_find(
        esp, esp->root_cluster, efi_fat32_name_efi,
        &efi_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_find(
        esp, efi_dir.first_cluster, efi_fat32_name_hfoc,
        &hfoc_dir);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_find(
        esp, hfoc_dir.first_cluster,
        efi_fat32_name_bootx64, &file);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (file.file_size == 0U ||
        file.file_size > capacity) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    current = file.first_cluster;
    file_offset = 0U;
    while (file_offset < file.file_size) {
        uint32_t cluster_sector;

        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             file_offset < file.file_size;
             ++cluster_sector) {
            uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) *
                    esp->sectors_per_cluster +
                cluster_sector;
            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_read(
                esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            read_length = file.file_size - file_offset;
            if (read_length > sizeof(sector)) {
                read_length = sizeof(sector);
            }
            memcpy(payload + file_offset, sector, read_length);
            file_offset += read_length;
        }
        if (file_offset < file.file_size) {
            if (!efi_fat32_fat_next(
                    esp, current, &next, &has_next) ||
                !has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
        }
    }
    *payload_size = file.file_size;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_validate_path(
    const char *path,
    size_t path_length)
{
    static const char canonical[] = EFI_FAT32_ESP_CANONICAL_PATH;
    size_t canonical_length;
    size_t index;

    if (path == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    canonical_length = sizeof(canonical) - 1U;
    if (path_length != canonical_length) {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    for (index = 0U; index < path_length; ++index) {
        if (path[index] != canonical[index]) {
            return EFI_FAT32_ESP_PATH_DENIED;
        }
    }
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_probe_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    bool *present,
    uint32_t *size)
{
    static struct efi_fat32_esp_path parsed;
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;

    if (esp == NULL || path == NULL || present == NULL || size == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    *present = false;
    *size = 0U;
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_path_parse(path, &parsed);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_find_payload(esp, &parsed, false, &file);
    if (status == EFI_FAT32_ESP_NOT_FOUND) {
        return EFI_FAT32_ESP_OK;
    }
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (!file.present) {
        return EFI_FAT32_ESP_OK;
    }
    *present = true;
    *size = file.file_size;
    return EFI_FAT32_ESP_OK;
}

/*
 * ITEM-398: the directory a path lives in, created when it is not there yet.
 *
 * The rename that takes over a shared ESP moves an occupant to the documented
 * `.PRE` name, and those names live under `\EFI\HFOC\` - a directory the install
 * has not staged anything into at that point.  The data of the moved file never
 * moves, so nothing here copies a byte of it: this walks (and creates) only the
 * directories on the way to the entry's new home.
 */
static enum efi_fat32_esp_status efi_fat32_resolve_directory(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_esp_path *path,
    uint32_t *directory_cluster)
{
    uint32_t current = esp->root_cluster;
    size_t element;

    if (path->element_count == 0U) {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    for (element = 0U; element + 1U < path->element_count; ++element) {
        uint32_t next = 0U;
        enum efi_fat32_esp_status status = efi_fat32_ensure_directory(
            esp, current, path->short_names[element], &next);

        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
        current = next;
    }
    *directory_cluster = current;
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_rename_file(
    struct efi_fat32_esp *esp,
    const char *from_path,
    const char *to_path)
{
    static struct efi_fat32_esp_path from;
    static struct efi_fat32_esp_path to;
    struct efi_fat32_file_ref file;
    struct efi_fat32_file_ref existing;
    struct efi_fat32_dir_ref slot;
    enum efi_fat32_esp_status status;
    uint32_t destination_cluster = 0U;
    size_t index;

    if (esp == NULL || from_path == NULL || to_path == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    status = efi_fat32_path_parse(from_path, &from);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_path_parse(to_path, &to);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_find_payload(esp, &from, false, &file);
    if (status != EFI_FAT32_ESP_OK || !file.present) {
        return status == EFI_FAT32_ESP_OK ? EFI_FAT32_ESP_NOT_FOUND
                                          : status;
    }
    /* Never overwrite something that is already there. */
    status = efi_fat32_find_payload(esp, &to, false, &existing);
    if (status == EFI_FAT32_ESP_OK && existing.present) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    if (status != EFI_FAT32_ESP_OK &&
        status != EFI_FAT32_ESP_NOT_FOUND) {
        return status;
    }
    /*
     * The entry moves; the file does not.  Its chain, its first cluster and its
     * size are written into the destination directory unchanged, which is what
     * makes the original bytes recoverable under the new name.
     */
    status = efi_fat32_resolve_directory(esp, &to, &destination_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_reserve_slot(esp, destination_cluster, &slot);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    status = efi_fat32_dir_entry_write(
        esp, &slot, to.short_names[to.element_count - 1U],
        EFI_FAT32_ATTR_ARCHIVE, file.first_cluster, file.file_size);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_dir_entry_clear(esp, &slot);
        return status;
    }
    /*
     * ITEM-398: the long-name entries go with the entry.  A reader binds such a
     * run to the entry that follows it, so leaving it behind would hand this
     * file's name to whatever entry takes its place - a loader installed at the
     * documented path would answer to the old configuration's name, and a
     * firmware looking for that name would parse the loader as its
     * configuration.
     */
    for (index = 0U; index < file.lfn_count; ++index) {
        status = efi_fat32_dir_entry_clear(esp, &file.lfn_refs[index]);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    status = efi_fat32_dir_entry_clear(esp, &file.ref);
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_dir_entry_clear(esp, &slot);
        return status;
    }
    return efi_fat32_flush_device(esp);
}

/*
 * ITEM-410: the fixed-extent file the appliance boot log lives in.
 *
 * The boot log cannot be written the way a payload is: it is created once,
 * preallocated to a fixed size, and from then on every boot overwrites slots
 * inside the extent it already owns.  Growing a file cluster by cluster on
 * every flush would touch the FAT and the allocation of a volume that may be
 * shared with another system, and a torn chain in the middle of a boot is
 * exactly the kind of damage a log is supposed to help diagnose.
 *
 * So this section adds three bounded operations on one file:
 *
 *   * `create_fixed_file()` allocates a whole number of clusters in one
 *     chain, zeroes it (a slot that was never written must read as empty, not
 *     as whatever the cluster held before) and writes the one directory entry.
 *     A file that is already exactly the declared size is *reused untouched*:
 *     no FAT byte, no directory byte and no data byte of an existing extent is
 *     written, which is what makes the second boot append instead of rewrite;
 *   * `write_file_range()` overwrites bytes inside the existing extent and
 *     never allocates, never frees and never changes the file's length;
 *   * `read_file_range()` reads the extent back through the same chain walk.
 *
 * Nothing here deletes, renames or even looks at another file: the only
 * entries this code changes are the one for `path` and the directory chain
 * that leads to it.
 */

static enum efi_fat32_esp_status efi_fat32_zero_chain(
    struct efi_fat32_esp *esp,
    uint32_t first_cluster,
    uint32_t sectors)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    struct efi_fat32_chain_cursor cursor;
    uint32_t current = first_cluster;
    uint32_t max_cluster = 2U + esp->cluster_count;
    uint32_t written = 0U;
    uint32_t steps = 0U;

    memset(sector, 0, sizeof(sector));
    memset(&cursor, 0, sizeof(cursor));
    while (written < sectors) {
        uint32_t cluster_sector;

        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             written < sectors;
             ++cluster_sector, ++written) {
            const uint64_t relative_lba =
                esp->data_start_lba +
                (uint64_t)(current - 2U) * esp->sectors_per_cluster +
                cluster_sector;
            enum efi_fat32_esp_status status;

            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            status = efi_fat32_sector_write(esp, relative_lba, sector);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
        }
        if (written >= sectors) {
            break;
        }
        {
            uint32_t next;
            bool has_next;

            if (!efi_fat32_fat_next(esp, current, &next, &has_next) ||
                !has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
            ++steps;
        }
    }
    return EFI_FAT32_ESP_OK;
}

enum efi_fat32_esp_status efi_fat32_esp_create_fixed_file(
    struct efi_fat32_esp *esp,
    const char *path,
    uint32_t size,
    uint32_t *first_cluster_out,
    bool *reused_out)
{
    struct efi_fat32_esp_path parsed;
    struct efi_fat32_file_ref existing;
    struct efi_fat32_dir_run run;
    enum efi_fat32_esp_status status;
    uint64_t cluster_bytes;
    uint64_t clusters;
    uint32_t directory_cluster = 0U;
    uint32_t first_cluster = 0U;
    uint32_t old_first = 0U;
    uint32_t old_size = 0U;
    uint64_t chain_clusters = 0U;
    size_t entry_count = 0U;
    size_t element;
    bool replaced = false;

    if (esp == NULL || path == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (first_cluster_out != NULL) {
        *first_cluster_out = 0U;
    }
    if (reused_out != NULL) {
        *reused_out = false;
    }
    if (size == 0U || size > EFI_FAT32_ESP_FILE_MAX) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    status = efi_fat32_path_parse(path, &parsed);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if (parsed.element_count < 2U) {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    if (efi_fat32_esp_validate_payload_path(path) != EFI_FAT32_ESP_OK) {
        return EFI_FAT32_ESP_PATH_DENIED;
    }
    cluster_bytes = (uint64_t)esp->sectors_per_cluster *
                    esp->bytes_per_sector;
    if (cluster_bytes == 0U ||
        ((uint64_t)size % cluster_bytes) != 0U) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    clusters = (uint64_t)size / cluster_bytes;
    if (clusters == 0U || clusters > UINT32_MAX) {
        return EFI_FAT32_ESP_SET_TOO_LARGE;
    }

    memset(&existing, 0, sizeof(existing));
    status = efi_fat32_find_payload(esp, &parsed, false, &existing);
    if (status == EFI_FAT32_ESP_OK) {
        if ((existing.attributes & EFI_FAT32_ATTR_DIRECTORY) != 0U) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        if (existing.file_size == size) {
            status = efi_fat32_chain_count(
                esp, existing.first_cluster, &chain_clusters);
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            /*
             * The extent is already exactly what the caller declared: reuse it
             * without writing a single byte.  This is the path every boot after
             * the first one takes, and it is what keeps the log's history (and
             * everybody else's file on a shared ESP) untouched.
             */
            if (chain_clusters == clusters) {
                if (first_cluster_out != NULL) {
                    *first_cluster_out = existing.first_cluster;
                }
                if (reused_out != NULL) {
                    *reused_out = true;
                }
                return EFI_FAT32_ESP_OK;
            }
        }
        replaced = true;
        old_first = existing.first_cluster;
        old_size = existing.file_size;
    } else if (status != EFI_FAT32_ESP_NOT_FOUND) {
        return status;
    }

    directory_cluster = esp->root_cluster;
    for (element = 0U; element + 1U < parsed.element_count; ++element) {
        status = efi_fat32_dir_ensure_element(
            esp, directory_cluster, &parsed, element,
            &directory_cluster);
        if (status != EFI_FAT32_ESP_OK) {
            return status;
        }
    }
    status = efi_fat32_allocate_chain_batched(
        esp, (uint32_t)clusters, &first_cluster);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    /* Zero before the entry names the chain: a crash leaves free clusters,
     * never a file whose slots hold somebody else's bytes. */
    status = efi_fat32_zero_chain(esp, first_cluster, size / EFI_FAT32_ESP_SECTOR_SIZE);
    if (status == EFI_FAT32_ESP_OK) {
        status = efi_fat32_dir_run_reserve(
            esp, directory_cluster,
            efi_fat32_dir_element_entries(
                &parsed, parsed.element_count - 1U),
            &run);
    }
    if (status == EFI_FAT32_ESP_OK) {
        status = efi_fat32_dir_run_write_element(
            esp, &run, &parsed, parsed.element_count - 1U, false,
            first_cluster, size, &entry_count);
    }
    if (status != EFI_FAT32_ESP_OK) {
        (void)efi_fat32_free_chain_batched(esp, first_cluster);
        return status;
    }
    if (replaced) {
        status = efi_fat32_dir_clear_file_entry(esp, &existing);
        if (status != EFI_FAT32_ESP_OK) {
            (void)efi_fat32_dir_run_clear(esp, &run, entry_count);
            (void)efi_fat32_free_chain_batched(esp, first_cluster);
            return status;
        }
    }
    status = efi_fat32_flush_device(esp);
    if (status != EFI_FAT32_ESP_OK) {
        if (replaced) {
            size_t restored = 0U;

            (void)efi_fat32_dir_run_write_element(
                esp, &run, &parsed, parsed.element_count - 1U, false,
                old_first, old_size, &restored);
        } else {
            (void)efi_fat32_dir_run_clear(esp, &run, entry_count);
        }
        (void)efi_fat32_free_chain_batched(esp, first_cluster);
        return EFI_FAT32_ESP_FLUSH_ERROR;
    }
    if (replaced && old_first != 0U && old_first != first_cluster) {
        (void)efi_fat32_free_chain_batched(esp, old_first);
    }
    if (first_cluster_out != NULL) {
        *first_cluster_out = first_cluster;
    }
    return EFI_FAT32_ESP_OK;
}

/*
 * Walk a file's chain and hand every sector that intersects [offset, offset +
 * length) to `write` (or to the read buffer).  One cluster walk, one sector at
 * a time, bounded by the chain: a corrupt chain stops the walk instead of
 * looping, and a range outside the file's own length is refused before the
 * first device access.
 */
static enum efi_fat32_esp_status efi_fat32_file_range_transfer(
    struct efi_fat32_esp *esp,
    const struct efi_fat32_file_ref *file,
    uint32_t offset,
    unsigned char *payload,
    size_t length,
    bool write)
{
    unsigned char sector[EFI_FAT32_ESP_SECTOR_SIZE];
    struct efi_fat32_chain_cursor cursor;
    uint64_t file_offset = 0U;
    uint64_t range_end = (uint64_t)offset + (uint64_t)length;
    uint32_t current;
    uint32_t max_cluster;
    uint32_t steps = 0U;
    size_t done = 0U;

    if (esp == NULL || file == NULL || payload == NULL ||
        length == 0U) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!file->present || range_end > (uint64_t)file->file_size) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    memset(&cursor, 0, sizeof(cursor));
    max_cluster = 2U + esp->cluster_count;
    current = file->first_cluster;
    while (file_offset < (uint64_t)file->file_size) {
        uint32_t cluster_sector;

        if (current < 2U || current >= max_cluster ||
            steps > max_cluster) {
            return EFI_FAT32_ESP_CORRUPT;
        }
        for (cluster_sector = 0U;
             cluster_sector < esp->sectors_per_cluster &&
             file_offset < (uint64_t)file->file_size;
             ++cluster_sector, file_offset += EFI_FAT32_ESP_SECTOR_SIZE) {
            const uint64_t sector_start = file_offset;
            const uint64_t sector_end =
                file_offset + EFI_FAT32_ESP_SECTOR_SIZE;
            uint64_t start;
            uint64_t end;
            size_t sector_length;
            size_t sector_offset;
            uint64_t relative_lba;
            enum efi_fat32_esp_status status;

            if (sector_end <= (uint64_t)offset || sector_start >= range_end) {
                continue;
            }
            start = sector_start > (uint64_t)offset
                        ? sector_start
                        : (uint64_t)offset;
            end = sector_end < range_end ? sector_end : range_end;
            sector_offset = (size_t)(start - sector_start);
            sector_length = (size_t)(end - start);
            relative_lba = esp->data_start_lba +
                           (uint64_t)(current - 2U) *
                               esp->sectors_per_cluster +
                           cluster_sector;
            if (relative_lba >= esp->block_count) {
                return EFI_FAT32_ESP_OUT_OF_RANGE;
            }
            if (write) {
                if (sector_offset == 0U &&
                    sector_length == sizeof(sector)) {
                    memcpy(sector, payload + done, sizeof(sector));
                } else {
                    status = efi_fat32_sector_read(
                        esp, (uint32_t)relative_lba, sector);
                    if (status != EFI_FAT32_ESP_OK) {
                        return status;
                    }
                    memcpy(sector + sector_offset, payload + done,
                           sector_length);
                }
                status = efi_fat32_sector_write(
                    esp, (uint32_t)relative_lba, sector);
            } else {
                status = efi_fat32_sector_read(
                    esp, (uint32_t)relative_lba, sector);
                if (status == EFI_FAT32_ESP_OK) {
                    memcpy(payload + done, sector + sector_offset,
                           sector_length);
                }
            }
            if (status != EFI_FAT32_ESP_OK) {
                return status;
            }
            done += sector_length;
        }
        if (file_offset >= (uint64_t)file->file_size) {
            break;
        }
        {
            uint32_t next;
            bool has_next;
            const uint64_t walk_started = efi_fat32_probe_cycles();
            const bool walked =
                efi_fat32_fat_next(esp, current, &next, &has_next);

            efi_fat32_traversal_cycles +=
                efi_fat32_probe_cycles() - walk_started;
            efi_fat32_io_stats.fat_entries_read += 1U;
            if (!walked || !has_next) {
                return EFI_FAT32_ESP_CORRUPT;
            }
            current = next;
            ++steps;
        }
    }
    if (done != length) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    return EFI_FAT32_ESP_OK;
}

static enum efi_fat32_esp_status efi_fat32_find_file(
    const struct efi_fat32_esp *esp,
    const char *path,
    struct efi_fat32_file_ref *file)
{
    struct efi_fat32_esp_path parsed;
    enum efi_fat32_esp_status status;

    if (esp == NULL || path == NULL || file == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    memset(file, 0, sizeof(*file));
    status = efi_fat32_path_parse(path, &parsed);
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    return efi_fat32_find_payload(esp, &parsed, false, file);
}

enum efi_fat32_esp_status efi_fat32_esp_write_file_range(
    struct efi_fat32_esp *esp,
    const char *path,
    uint32_t offset,
    const unsigned char *data,
    size_t length)
{
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;
    const uint64_t lookup_started = efi_fat32_probe_cycles();

    if (esp == NULL || data == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    efi_fat32_io_stats.transfers += 1U;
    status = efi_fat32_find_file(esp, path, &file);
    efi_fat32_lookup_cycles += efi_fat32_probe_cycles() - lookup_started;
    efi_fat32_io_stats.lookups += 1U;
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if ((file.attributes & EFI_FAT32_ATTR_DIRECTORY) != 0U) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    /*
     * The extent's length is the boundary the writer may never move: a range
     * that would grow the file is refused instead of being written, so the
     * fixed extent stays fixed.
     */
    if (offset > file.file_size ||
        length > (size_t)(file.file_size - offset)) {
        return EFI_FAT32_ESP_OUT_OF_RANGE;
    }
    return efi_fat32_file_range_transfer(
        esp, &file, offset, (unsigned char *)data, length, true);
}

enum efi_fat32_esp_status efi_fat32_esp_read_file_range(
    const struct efi_fat32_esp *esp,
    const char *path,
    uint32_t offset,
    unsigned char *buffer,
    size_t capacity,
    size_t *length_out,
    uint32_t *file_size_out)
{
    struct efi_fat32_file_ref file;
    enum efi_fat32_esp_status status;
    const uint64_t lookup_started = efi_fat32_probe_cycles();

    if (esp == NULL || buffer == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    if (length_out != NULL) {
        *length_out = 0U;
    }
    if (file_size_out != NULL) {
        *file_size_out = 0U;
    }
    efi_fat32_io_stats.transfers += 1U;
    status = efi_fat32_find_file(esp, path, &file);
    efi_fat32_lookup_cycles += efi_fat32_probe_cycles() - lookup_started;
    efi_fat32_io_stats.lookups += 1U;
    if (status != EFI_FAT32_ESP_OK) {
        return status;
    }
    if ((file.attributes & EFI_FAT32_ATTR_DIRECTORY) != 0U) {
        return EFI_FAT32_ESP_CORRUPT;
    }
    if (file_size_out != NULL) {
        *file_size_out = file.file_size;
    }
    if (capacity == 0U) {
        return EFI_FAT32_ESP_OK;
    }
    if (offset > file.file_size ||
        capacity > (size_t)(file.file_size - offset)) {
        return EFI_FAT32_ESP_CAPACITY;
    }
    status = efi_fat32_file_range_transfer(
        (struct efi_fat32_esp *)esp, &file, offset, buffer, capacity,
        false);
    if (status == EFI_FAT32_ESP_OK && length_out != NULL) {
        *length_out = capacity;
    }
    return status;
}

enum efi_fat32_esp_status efi_fat32_esp_flush(
    struct efi_fat32_esp *esp)
{
    if (esp == NULL) {
        return EFI_FAT32_ESP_NULL;
    }
    if (!esp->bound || esp->version != EFI_FAT32_ESP_VERSION) {
        return EFI_FAT32_ESP_BAD_STATE;
    }
    return efi_fat32_flush_device(esp);
}

const char *efi_fat32_esp_status_string(
    enum efi_fat32_esp_status status)
{
    switch (status) {
    case EFI_FAT32_ESP_OK:
        return "EFI_FAT32_ESP_OK";
    case EFI_FAT32_ESP_NULL:
        return "EFI_FAT32_ESP_NULL";
    case EFI_FAT32_ESP_BAD_STATE:
        return "EFI_FAT32_ESP_BAD_STATE";
    case EFI_FAT32_ESP_BAD_ENVIRONMENT:
        return "EFI_FAT32_ESP_BAD_ENVIRONMENT";
    case EFI_FAT32_ESP_BAD_DEVICE:
        return "EFI_FAT32_ESP_BAD_DEVICE";
    case EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY:
        return "EFI_FAT32_ESP_UNSUPPORTED_GEOMETRY";
    case EFI_FAT32_ESP_OUT_OF_RANGE:
        return "EFI_FAT32_ESP_OUT_OF_RANGE";
    case EFI_FAT32_ESP_CORRUPT:
        return "EFI_FAT32_ESP_CORRUPT";
    case EFI_FAT32_ESP_PATH_DENIED:
        return "EFI_FAT32_ESP_PATH_DENIED";
    case EFI_FAT32_ESP_NOT_FOUND:
        return "EFI_FAT32_ESP_NOT_FOUND";
    case EFI_FAT32_ESP_CAPACITY:
        return "EFI_FAT32_ESP_CAPACITY";
    case EFI_FAT32_ESP_IO_ERROR:
        return "EFI_FAT32_ESP_IO_ERROR";
    case EFI_FAT32_ESP_FLUSH_ERROR:
        return "EFI_FAT32_ESP_FLUSH_ERROR";
    case EFI_FAT32_ESP_PATH_CONFLICT:
        return "EFI_FAT32_ESP_PATH_CONFLICT";
    case EFI_FAT32_ESP_SET_TOO_LARGE:
        return "EFI_FAT32_ESP_SET_TOO_LARGE";
    default:
        return NULL;
    }
}
