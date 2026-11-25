#include "installer_gpt_preserve.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-396.  GPT layout constants of this writer.  They are the GPT's own
 * standard shape (128 entries of 128 bytes on a 512-byte-sector disk), stated
 * here rather than taken from `installer_layout`: that module keeps
 * INSTALLER_LAYOUT_PARTITION_CAPACITY = 4 as the bound of *our* roles, and this
 * writer's preserved-entry bound is the table's, not the plan's.
 */
#define INSTALLER_GPT_PRESERVE_ALIGN_LBA \
    INSTALLER_LAYOUT_ALIGN_LBA
#define INSTALLER_GPT_PRESERVE_HEADER_OFFSET 0U
#define INSTALLER_GPT_PRESERVE_HEADER_SIZE_OFFSET 12U
#define INSTALLER_GPT_PRESERVE_HEADER_CRC_OFFSET 16U
#define INSTALLER_GPT_PRESERVE_HEADER_RESERVED_OFFSET 20U
#define INSTALLER_GPT_PRESERVE_HEADER_MY_LBA_OFFSET 24U
#define INSTALLER_GPT_PRESERVE_HEADER_ALTERNATE_OFFSET 32U
#define INSTALLER_GPT_PRESERVE_HEADER_FIRST_USABLE_OFFSET 40U
#define INSTALLER_GPT_PRESERVE_HEADER_LAST_USABLE_OFFSET 48U
#define INSTALLER_GPT_PRESERVE_HEADER_DISK_GUID_OFFSET 56U
#define INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET 72U
#define INSTALLER_GPT_PRESERVE_HEADER_ENTRY_COUNT_OFFSET 80U
#define INSTALLER_GPT_PRESERVE_HEADER_ENTRY_SIZE_OFFSET 84U
#define INSTALLER_GPT_PRESERVE_HEADER_ENTRY_CRC_OFFSET 88U
#define INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET 0U
#define INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET 16U
#define INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET 32U
#define INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET 40U
#define INSTALLER_GPT_PRESERVE_MBR_PARTITION_OFFSET 446U
#define INSTALLER_GPT_PRESERVE_MBR_PARTITION_COUNT 4U
#define INSTALLER_GPT_PRESERVE_MBR_PARTITION_SIZE 16U
#define INSTALLER_GPT_PRESERVE_MBR_SIGNATURE_OFFSET 510U
#define INSTALLER_GPT_PRESERVE_MBR_TYPE_OFFSET 4U
#define INSTALLER_GPT_PRESERVE_MBR_START_OFFSET 8U
#define INSTALLER_GPT_PRESERVE_MBR_SIZE_OFFSET 12U
#define INSTALLER_GPT_PRESERVE_MBR_TYPE_PROTECTIVE 0xEEU
#define INSTALLER_GPT_PRESERVE_RECORD_SLOT_LINES 24U

static const unsigned char installer_gpt_preserve_signature[8U] = {
    'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
};

static uint32_t installer_gpt_preserve_load_le32(
    const unsigned char *bytes)
{
    return (uint32_t)bytes[0U] |
           ((uint32_t)bytes[1U] << 8U) |
           ((uint32_t)bytes[2U] << 16U) |
           ((uint32_t)bytes[3U] << 24U);
}

static uint64_t installer_gpt_preserve_load_le64(
    const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void installer_gpt_preserve_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void installer_gpt_preserve_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t installer_gpt_preserve_crc32(
    const unsigned char *bytes,
    size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static bool installer_gpt_preserve_all_zero(
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool installer_gpt_preserve_add_overflows(
    uint64_t first,
    uint64_t length,
    uint64_t *last)
{
    if (length == 0U) {
        return true;
    }
    if (first > UINT64_MAX - (length - 1U)) {
        return true;
    }
    *last = first + (length - 1U);
    return false;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_device_read(
    struct block_device *device,
    uint64_t lba,
    unsigned char *sector)
{
    if (lba > UINT64_MAX / INSTALLER_GPT_PRESERVE_BLOCK_SIZE) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    if (block_device_read(
            device, lba * INSTALLER_GPT_PRESERVE_BLOCK_SIZE,
            sector, INSTALLER_GPT_PRESERVE_BLOCK_SIZE) !=
        BLOCK_DEVICE_OK) {
        return INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE;
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_device_write(
    struct block_device *device,
    uint64_t lba,
    const unsigned char *sector)
{
    if (lba > UINT64_MAX / INSTALLER_GPT_PRESERVE_BLOCK_SIZE) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    if (block_device_write(
            device, lba * INSTALLER_GPT_PRESERVE_BLOCK_SIZE,
            sector, INSTALLER_GPT_PRESERVE_BLOCK_SIZE) !=
        BLOCK_DEVICE_OK) {
        return INSTALLER_GPT_PRESERVE_IO_ERROR;
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

/*
 * Write one sector and read it straight back: a sector that does not survive
 * the round trip would mean the table on the disk is not the table this writer
 * believes it wrote, so the write stops there instead of pretending.
 */
static enum installer_gpt_preserve_status
installer_gpt_preserve_write_verified(
    struct block_device *device,
    uint64_t lba,
    const unsigned char *sector)
{
    unsigned char readback[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    enum installer_gpt_preserve_status status;

    status = installer_gpt_preserve_device_write(device, lba, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_device_read(device, lba, readback);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
    }
    if (memcmp(readback, sector,
               INSTALLER_GPT_PRESERVE_BLOCK_SIZE) != 0) {
        return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_table_check_device(
    struct block_device *device,
    struct installer_gpt_preserve_table *table,
    uint32_t disk_id)
{
    if (device == NULL || table == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (disk_id == 0U) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != INSTALLER_GPT_PRESERVE_BLOCK_SIZE) {
        return INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE;
    }
    /*
     * A table needs the primary header and array (34 sectors), the backup array
     * and header (34 sectors) and at least one usable sector between them.
     */
    if (device->block_count <
        2U * INSTALLER_GPT_PRESERVE_FIRST_USABLE_LBA + 1U) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    memset(table, 0, sizeof(*table));
    table->version = INSTALLER_GPT_PRESERVE_VERSION;
    table->read_complete = false;
    table->disk_id = disk_id;
    table->block_size = device->block_size;
    table->block_count = device->block_count;
    table->esp_index = INSTALLER_GPT_PRESERVE_NO_SLOT;
    table->released_index = INSTALLER_GPT_PRESERVE_NO_SLOT;
    return INSTALLER_GPT_PRESERVE_OK;
}

/*
 * The protective MBR is part of the table.  Its absence or an entry that does
 * not cover the disk means this is not a GPT this writer may rewrite, so the
 * install stops instead of "assuming empty" and erasing a boot record.
 */
static enum installer_gpt_preserve_status
installer_gpt_preserve_check_mbr(
    const unsigned char *mbr,
    uint64_t block_count)
{
    size_t index;

    if (mbr[INSTALLER_GPT_PRESERVE_MBR_SIGNATURE_OFFSET] != 0x55U ||
        mbr[INSTALLER_GPT_PRESERVE_MBR_SIGNATURE_OFFSET + 1U] !=
            0xAAU) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    for (index = 0U;
         index < INSTALLER_GPT_PRESERVE_MBR_PARTITION_COUNT;
         ++index) {
        const unsigned char *entry =
            mbr + INSTALLER_GPT_PRESERVE_MBR_PARTITION_OFFSET +
            index * INSTALLER_GPT_PRESERVE_MBR_PARTITION_SIZE;
        uint64_t start;
        uint64_t length;

        if (entry[INSTALLER_GPT_PRESERVE_MBR_TYPE_OFFSET] !=
            INSTALLER_GPT_PRESERVE_MBR_TYPE_PROTECTIVE) {
            continue;
        }
        start = installer_gpt_preserve_load_le32(
            entry + INSTALLER_GPT_PRESERVE_MBR_START_OFFSET);
        length = installer_gpt_preserve_load_le32(
            entry + INSTALLER_GPT_PRESERVE_MBR_SIZE_OFFSET);
        if (start != 1U || length == 0U) {
            return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
        }
        /*
         * The protective entry covers the disk, saturating at 0xFFFFFFFF on
         * media larger than 2 TiB (UEFI 2.10, 5.2.1).
         */
        if (length >= block_count - 1U ||
            length == UINT32_C(0xFFFFFFFF)) {
            return INSTALLER_GPT_PRESERVE_OK;
        }
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
}

struct installer_gpt_preserve_header {
    uint64_t my_lba;
    uint64_t alternate_lba;
    uint64_t first_usable_lba;
    uint64_t last_usable_lba;
    uint64_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
};

/*
 * Validate one GPT header sector against the disk it claims to describe.  The
 * header CRC is recomputed over the first 92 bytes with the stored CRC field
 * zeroed, and every geometry field must describe this disk: a header that
 * misdescribes the disk is refused, never repaired.
 */
static enum installer_gpt_preserve_status
installer_gpt_preserve_check_header(
    unsigned char *header,
    uint64_t block_count,
    uint64_t expected_my_lba,
    uint64_t expected_alternate_lba,
    uint64_t expected_entry_lba,
    struct installer_gpt_preserve_header *parsed)
{
    unsigned char scratch[INSTALLER_GPT_PRESERVE_HEADER_SIZE];
    uint32_t stored_crc;
    uint32_t computed_crc;
    uint32_t revision;
    uint32_t header_size;
    uint64_t first_usable;
    uint64_t last_usable;
    uint32_t entry_count;
    uint32_t entry_size;
    uint64_t entry_lba;
    size_t index;

    if (memcmp(header, installer_gpt_preserve_signature, 8U) != 0) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    /* Revision is major.minor in the two 16-bit halves of the field. */
    revision = installer_gpt_preserve_load_le32(header + 8U);
    if ((revision >> 16U) != 1U) {
        return INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE;
    }
    header_size = installer_gpt_preserve_load_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_SIZE_OFFSET);
    if (header_size != INSTALLER_GPT_PRESERVE_HEADER_SIZE) {
        return INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE;
    }
    for (index = 0U; index < 4U; ++index) {
        if (header[INSTALLER_GPT_PRESERVE_HEADER_RESERVED_OFFSET +
                   index] != 0U) {
            return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
        }
    }
    stored_crc = installer_gpt_preserve_load_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_CRC_OFFSET);
    memcpy(scratch, header, sizeof(scratch));
    memset(scratch + INSTALLER_GPT_PRESERVE_HEADER_CRC_OFFSET,
           0, 4U);
    computed_crc = installer_gpt_preserve_crc32(
        scratch, sizeof(scratch));
    if (stored_crc != computed_crc) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    if (installer_gpt_preserve_load_le64(
            header + INSTALLER_GPT_PRESERVE_HEADER_MY_LBA_OFFSET) !=
            expected_my_lba ||
        installer_gpt_preserve_load_le64(
            header + INSTALLER_GPT_PRESERVE_HEADER_ALTERNATE_OFFSET) !=
            expected_alternate_lba ||
        installer_gpt_preserve_load_le64(
            header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET) !=
            expected_entry_lba) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    first_usable = installer_gpt_preserve_load_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_FIRST_USABLE_OFFSET);
    last_usable = installer_gpt_preserve_load_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_LAST_USABLE_OFFSET);
    if (first_usable < INSTALLER_GPT_PRESERVE_FIRST_USABLE_LBA ||
        last_usable !=
            block_count - INSTALLER_GPT_PRESERVE_TAIL_RESERVE_LBA ||
        first_usable >= last_usable) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    entry_count = installer_gpt_preserve_load_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_COUNT_OFFSET);
    entry_size = installer_gpt_preserve_load_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_SIZE_OFFSET);
    entry_lba = expected_entry_lba;
    /*
     * The 32-sector entry array must lie on the disk and entirely outside the
     * usable region: the primary array below it, the backup array above it.  An
     * array that reached into the usable region would describe partitions it
     * also overlaps.
     */
    if (entry_lba > block_count ||
        INSTALLER_GPT_PRESERVE_ARRAY_SECTORS > block_count - entry_lba) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    if (entry_lba + INSTALLER_GPT_PRESERVE_ARRAY_SECTORS - 1U >=
            first_usable &&
        entry_lba <= last_usable) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    if (entry_count != INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY ||
        entry_size != INSTALLER_GPT_PRESERVE_ENTRY_SIZE) {
        return INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE;
    }
    parsed->my_lba = expected_my_lba;
    parsed->alternate_lba = expected_alternate_lba;
    parsed->first_usable_lba = first_usable;
    parsed->last_usable_lba = last_usable;
    parsed->entry_lba = entry_lba;
    parsed->entry_count = entry_count;
    parsed->entry_size = entry_size;
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_array_crc_check(
    struct block_device *device,
    unsigned char *header,
    uint64_t entry_lba)
{
    unsigned char sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char digest_input[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t stored = installer_gpt_preserve_load_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_CRC_OFFSET);
    uint32_t index;
    size_t byte_index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ARRAY_SECTORS;
         ++index) {
        enum installer_gpt_preserve_status status =
            installer_gpt_preserve_device_read(
                device, entry_lba + index, sector);

        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        memcpy(digest_input, sector, sizeof(digest_input));
        for (byte_index = 0U; byte_index < sizeof(digest_input);
             ++byte_index) {
            unsigned int bit;

            crc ^= digest_input[byte_index];
            for (bit = 0U; bit < 8U; ++bit) {
                if ((crc & 1U) != 0U) {
                    crc = (crc >> 1U) ^ 0xEDB88320U;
                } else {
                    crc >>= 1U;
                }
            }
        }
    }
    if ((crc ^ 0xFFFFFFFFU) != stored) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_arrays_match(
    struct block_device *device,
    uint64_t primary_lba,
    uint64_t backup_lba)
{
    unsigned char primary[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char backup[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ARRAY_SECTORS;
         ++index) {
        enum installer_gpt_preserve_status status =
            installer_gpt_preserve_device_read(
                device, primary_lba + index, primary);

        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        status = installer_gpt_preserve_device_read(
            device, backup_lba + index, backup);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        if (memcmp(primary, backup, sizeof(primary)) != 0) {
            return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
        }
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_read_entry(
    const unsigned char *entry,
    uint32_t index,
    const struct installer_gpt_preserve_table *table,
    struct installer_gpt_preserve_entry *record)
{
    uint64_t first;
    uint64_t last;

    memset(record, 0, sizeof(*record));
    record->index = index;
    if (installer_gpt_preserve_all_zero(
            entry, INSTALLER_GPT_PRESERVE_ENTRY_SIZE)) {
        record->origin = INSTALLER_GPT_PRESERVE_SLOT_EMPTY;
        return INSTALLER_GPT_PRESERVE_OK;
    }
    record->origin = INSTALLER_GPT_PRESERVE_SLOT_PRESERVED;
    memcpy(record->type_guid,
           entry + INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET, 16U);
    memcpy(record->unique_guid,
           entry + INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET, 16U);
    if (installer_gpt_preserve_all_zero(record->type_guid, 16U)) {
        /*
         * A non-zero slot without a partition type is not a partition; its
         * extent bytes carry no meaning, so it takes no part in the free-space
         * arithmetic.  It is still preserved byte-for-byte, because the writer
         * only ever writes slots that are all-zero.
         */
        return INSTALLER_GPT_PRESERVE_OK;
    }
    record->has_extent = true;
    first = installer_gpt_preserve_load_le64(
        entry + INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET);
    last = installer_gpt_preserve_load_le64(
        entry + INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET);
    if (first > last || first < table->first_usable_lba ||
        last > table->last_usable_lba) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    record->first_lba = first;
    record->last_lba = last;
    return INSTALLER_GPT_PRESERVE_OK;
}

/*
 * A present entry is preserved bytes and a reserved extent.  Two overlapping
 * extents would make the free-region arithmetic meaningless, so a table that
 * carries them is corrupt and refused instead of guessed about.
 */
static enum installer_gpt_preserve_status
installer_gpt_preserve_check_extents(
    const struct installer_gpt_preserve_table *table)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        uint32_t other;

        if (table->entries[index].origin !=
                INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
            !table->entries[index].has_extent) {
            continue;
        }
        for (other = index + 1U;
             other < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
             ++other) {
            if (table->entries[other].origin !=
                    INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
                !table->entries[other].has_extent) {
                continue;
            }
            if (table->entries[index].first_lba <=
                    table->entries[other].last_lba &&
                table->entries[other].first_lba <=
                    table->entries[index].last_lba) {
                return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
            }
        }
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

enum installer_gpt_preserve_status installer_gpt_preserve_read(
    struct block_device *device,
    uint32_t disk_id,
    struct installer_gpt_preserve_table *table)
{
    struct installer_gpt_preserve_header primary;
    struct installer_gpt_preserve_header backup;
    unsigned char sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    enum installer_gpt_preserve_status status;
    uint64_t backup_lba;
    uint64_t backup_array_lba;
    uint32_t index;
    uint64_t preserved_first = 0U;
    uint64_t preserved_last = 0U;
    bool any_preserved = false;

    status = installer_gpt_preserve_table_check_device(
        device, table, disk_id);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    backup_lba = table->block_count - 1U;
    /* The 32-sector backup array sits immediately below the backup header. */
    backup_array_lba = table->block_count -
        (INSTALLER_GPT_PRESERVE_ARRAY_SECTORS + 1U);

    status = installer_gpt_preserve_device_read(device, 0U, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_mbr(
        sector, table->block_count);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    memcpy(table->mbr, sector, sizeof(table->mbr));

    status = installer_gpt_preserve_device_read(device, 1U, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_header(
        sector, table->block_count, 1U, backup_lba, 2U, &primary);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    memcpy(table->primary_header, sector, sizeof(table->primary_header));
    memcpy(table->disk_guid,
           sector + INSTALLER_GPT_PRESERVE_HEADER_DISK_GUID_OFFSET,
           16U);
    table->first_usable_lba = primary.first_usable_lba;
    table->last_usable_lba = primary.last_usable_lba;
    table->entry_count = primary.entry_count;
    table->entry_size = primary.entry_size;

    status = installer_gpt_preserve_device_read(
        device, backup_lba, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_header(
        sector, table->block_count, backup_lba, 1U,
        backup_array_lba, &backup);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    memcpy(table->backup_header, sector, sizeof(table->backup_header));
    if (installer_gpt_preserve_load_le64(
            table->primary_header +
            INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET) !=
            primary.entry_lba ||
        backup.entry_lba != backup_array_lba) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }
    if (memcmp(table->primary_header +
                   INSTALLER_GPT_PRESERVE_HEADER_DISK_GUID_OFFSET,
               table->backup_header +
                   INSTALLER_GPT_PRESERVE_HEADER_DISK_GUID_OFFSET,
               16U) != 0) {
        return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
    }

    status = installer_gpt_preserve_array_crc_check(
        device, table->primary_header, primary.entry_lba);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_array_crc_check(
        device, table->backup_header, backup.entry_lba);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_arrays_match(
        device, primary.entry_lba, backup.entry_lba);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const uint64_t entry_lba =
            primary.entry_lba +
            (uint64_t)(index /
                       INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR);

        status = installer_gpt_preserve_device_read(
            device, entry_lba, sector);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        status = installer_gpt_preserve_read_entry(
            sector +
                (size_t)(index %
                         INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR) *
                    INSTALLER_GPT_PRESERVE_ENTRY_SIZE,
            index, table, &table->entries[index]);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
    }
    status = installer_gpt_preserve_check_extents(table);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        if (table->entries[index].origin !=
            INSTALLER_GPT_PRESERVE_SLOT_PRESERVED) {
            continue;
        }
        ++table->preserved_count;
        if (!table->entries[index].has_extent) {
            continue;
        }
        if (!any_preserved ||
            table->entries[index].first_lba < preserved_first) {
            preserved_first = table->entries[index].first_lba;
        }
        if (!any_preserved ||
            table->entries[index].last_lba > preserved_last) {
            preserved_last = table->entries[index].last_lba;
        }
        any_preserved = true;
    }
    table->present_count = table->preserved_count;
    if (any_preserved) {
        table->preserved_first_lba = preserved_first;
        table->preserved_last_lba = preserved_last;
    }
    table->read_complete = true;
    return INSTALLER_GPT_PRESERVE_OK;
}

static bool installer_gpt_preserve_span_covers(
    const struct installer_gpt_preserve_table *table,
    uint64_t first,
    uint64_t last)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const struct installer_gpt_preserve_entry *entry =
            &table->entries[index];

        if (entry->origin != INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
            !entry->has_extent) {
            continue;
        }
        if (first <= entry->last_lba && entry->first_lba <= last) {
            return true;
        }
    }
    return false;
}

enum installer_gpt_preserve_status installer_gpt_preserve_free_region(
    const struct installer_gpt_preserve_table *table,
    uint64_t first_lba,
    uint64_t *free_first_lba,
    uint64_t *free_length_lba)
{
    uint64_t usable_end;
    uint64_t region_first;
    uint64_t region_end;
    uint32_t index;

    if (table == NULL || free_first_lba == NULL ||
        free_length_lba == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (table->version != INSTALLER_GPT_PRESERVE_VERSION) {
        return INSTALLER_GPT_PRESERVE_BAD_VERSION;
    }
    if (table->last_usable_lba <= table->first_usable_lba ||
        first_lba < table->first_usable_lba) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    usable_end = table->last_usable_lba + 1U;
    if (first_lba >= usable_end) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    /*
     * The maximal free interval that contains `first_lba`: it starts after the
     * last preserved extent that ends before it and ends at the first preserved
     * extent that starts after it.  A point inside a preserved extent has no
     * free region at all, which is the NO_FREE_SPACE refusal.
     */
    region_first = table->first_usable_lba;
    region_end = usable_end;
    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const struct installer_gpt_preserve_entry *entry =
            &table->entries[index];

        if (entry->origin != INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
            !entry->has_extent) {
            continue;
        }
        if (first_lba < entry->first_lba) {
            if (entry->first_lba < region_end) {
                region_end = entry->first_lba;
            }
            continue;
        }
        if (first_lba <= entry->last_lba) {
            return INSTALLER_GPT_PRESERVE_NO_FREE_SPACE;
        }
        if (entry->last_lba != UINT64_MAX &&
            entry->last_lba + 1U > region_first) {
            region_first = entry->last_lba + 1U;
        }
    }
    if (region_end <= region_first || first_lba < region_first) {
        return INSTALLER_GPT_PRESERVE_NO_FREE_SPACE;
    }
    *free_first_lba = region_first;
    *free_length_lba = region_end - region_first;
    return INSTALLER_GPT_PRESERVE_OK;
}

static bool installer_gpt_preserve_entry_is_ours(
    const struct installer_gpt_preserve_entry *entry,
    enum installer_layout_role role,
    uint32_t partition_index)
{
    unsigned char unique[16U];
    const unsigned char *type_guid;

    if (entry->origin != INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
        !entry->has_extent) {
        return false;
    }
    type_guid = installer_layout_role_type_guid(role);
    if (type_guid == NULL) {
        return false;
    }
    installer_layout_role_unique_guid(unique, role, partition_index);
    return memcmp(entry->type_guid, type_guid, 16U) == 0 &&
           memcmp(entry->unique_guid, unique, 16U) == 0;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_claim_slot(
    struct installer_gpt_preserve_table *table,
    const struct installer_layout_partition *partition,
    bool reused_esp,
    uint32_t *slot)
{
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        if (installer_gpt_preserve_entry_is_ours(
                &table->entries[index], partition->role,
                partition->partition_index)) {
            *slot = index;
            table->entries[index].role = partition->role;
            table->entries[index].partition_index =
                partition->partition_index;
            table->entries[index].reused = reused_esp;
            /*
             * The slot carries this install's own entry, recognised from the
             * GUIDs actually on the disk, so writing it again is a rewrite and
             * not an overwrite of somebody else's partition.
             */
            table->entries[index].rewritten = true;
            return INSTALLER_GPT_PRESERVE_OK;
        }
    }
    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        if (table->entries[index].origin ==
                INSTALLER_GPT_PRESERVE_SLOT_EMPTY &&
            table->entries[index].role ==
                INSTALLER_LAYOUT_ROLE_UNKNOWN) {
            *slot = index;
            table->entries[index].role = partition->role;
            table->entries[index].partition_index =
                partition->partition_index;
            table->entries[index].reused = reused_esp;
            return INSTALLER_GPT_PRESERVE_OK;
        }
    }
    return INSTALLER_GPT_PRESERVE_CAPACITY;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_check_partition(
    const struct installer_layout_partition *partition,
    uint64_t first_usable_lba,
    uint64_t last_usable_lba)
{
    uint64_t last;

    if (!partition->present ||
        partition->role <= INSTALLER_LAYOUT_ROLE_UNKNOWN ||
        partition->role > INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    /*
     * The appliance's roles are placed on the 1 MiB boundary both ITEM-391
     * planners use; a request that does not is not a product layout and is
     * refused rather than written unaligned into somebody else's disk.
     */
    if (partition->first_lba %
            INSTALLER_GPT_PRESERVE_ALIGN_LBA != 0U) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    if (installer_gpt_preserve_add_overflows(
            partition->first_lba, partition->length_lba, &last)) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    if (partition->first_lba < first_usable_lba ||
        last > last_usable_lba) {
        return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

enum installer_gpt_preserve_status installer_gpt_preserve_check(
    struct installer_gpt_preserve_table *table,
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse)
{
    enum installer_gpt_preserve_status status;
    uint64_t span_first = 0U;
    uint64_t span_end = 0U;
    uint64_t free_first = 0U;
    uint64_t free_length = 0U;
    uint64_t reuse_last = 0U;
    uint32_t reuse_index = INSTALLER_GPT_PRESERVE_NO_SLOT;
    uint32_t index;

    if (table == NULL || plan == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (table->version != INSTALLER_GPT_PRESERVE_VERSION ||
        plan->version != INSTALLER_LAYOUT_VERSION) {
        return INSTALLER_GPT_PRESERVE_BAD_VERSION;
    }
    if (!table->read_complete) {
        /* A table that was refused, or only half read, is never written. */
        return INSTALLER_GPT_PRESERVE_BAD_STATE;
    }
    /*
     * Planning is read-only and does not need the confirmation: a plan is
     * checked (and published) before the operator confirms it, and the write -
     * which does need `confirmed` - checks it again against the table as it is
     * at that moment.
     */
    if (plan->executed) {
        return INSTALLER_GPT_PRESERVE_BAD_STATE;
    }
    if (plan->disk_id != table->disk_id ||
        plan->block_size != table->block_size ||
        plan->block_count != table->block_count) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    if (plan->partition_count == 0U ||
        plan->partition_count > INSTALLER_GPT_PRESERVE_ADDED_CAPACITY) {
        return INSTALLER_GPT_PRESERVE_CAPACITY;
    }
    /*
     * This writer preserves.  A plan that declares itself destructive is the
     * clean whole-disk install, whose writer stays `installer_layout_execute()`:
     * accepting it here would silently change what a confirmed destructive
     * install means.
     */
    if (plan->destructive) {
        return INSTALLER_GPT_PRESERVE_BAD_STATE;
    }
    /* Re-checking a table that was already checked must start clean. */
    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        struct installer_gpt_preserve_entry *entry =
            &table->entries[index];

        if (entry->origin == INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
            entry->origin = INSTALLER_GPT_PRESERVE_SLOT_EMPTY;
        }
        /*
         * ITEM-501: a slot the previous check released goes back to being a
         * preserved entry - the plan is re-checked against the table as it is
         * on the disk, not against the previous check's bookkeeping.
         */
        if (entry->origin == INSTALLER_GPT_PRESERVE_SLOT_RELEASED) {
            entry->origin = INSTALLER_GPT_PRESERVE_SLOT_PRESERVED;
        }
        if (entry->origin == INSTALLER_GPT_PRESERVE_SLOT_EMPTY) {
            memset(entry, 0, sizeof(*entry));
            entry->index = index;
            entry->origin = INSTALLER_GPT_PRESERVE_SLOT_EMPTY;
        } else {
            entry->role = INSTALLER_LAYOUT_ROLE_UNKNOWN;
            entry->partition_index = 0U;
            entry->reused = false;
        }
    }
    table->added_count = 0U;
    table->free_first_lba = 0U;
    table->free_length_lba = 0U;
    table->esp_index = INSTALLER_GPT_PRESERVE_NO_SLOT;
    table->esp_reused = false;
    table->released_index = INSTALLER_GPT_PRESERVE_NO_SLOT;

    /*
     * ITEM-501: the one entry the operator gave up.  The plan names it by the
     * discovery's partition index and by its extent; the slot on the disk must
     * carry exactly that entry, or the plan and the disk disagree about the
     * partition and nothing may be written.  Marking the slot RELEASED removes
     * it from every preserved-span calculation below, so the appliance's roles
     * may land in the extent it occupied; the write then stores zero bytes
     * there and nowhere else.
     */
    if (plan->releases_partition) {
        uint64_t released_end;
        uint32_t slot;

        if (plan->released_index == 0U ||
            plan->released_length_lba == 0U ||
            plan->released_length_lba >
                UINT64_MAX - plan->released_first_lba) {
            return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
        }
        released_end =
            plan->released_first_lba + plan->released_length_lba;
        slot = plan->released_index - 1U;
        if (slot >= INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY) {
            return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
        }
        if (plan->released_first_lba < table->first_usable_lba ||
            released_end - 1U > table->last_usable_lba) {
            return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
        }
        if (table->entries[slot].origin !=
                INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
            !table->entries[slot].has_extent ||
            table->entries[slot].first_lba != plan->released_first_lba ||
            table->entries[slot].last_lba != released_end - 1U) {
            return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
        }
        table->entries[slot].origin =
            INSTALLER_GPT_PRESERVE_SLOT_RELEASED;
        table->released_index = slot;
    }

    /*
     * The reused ESP is a foreign entry this install keeps: the dual-boot plan
     * reserved a range that must be exactly the entry the discovery found, so an
     * install that would have to move or rewrite it is refused.  Nothing is
     * added for it, which is why a dual-boot plan carries three roles, not four.
     */
    if (reuse != NULL && reuse->present) {
        const unsigned char *esp_guid;

        if (reuse->role != INSTALLER_LAYOUT_ROLE_EFI_SYSTEM ||
            reuse->length_lba == 0U) {
            return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
        }
        esp_guid = installer_layout_role_type_guid(reuse->role);
        if (esp_guid == NULL ||
            installer_gpt_preserve_add_overflows(
                reuse->first_lba, reuse->length_lba, &reuse_last) ||
            reuse->first_lba < table->first_usable_lba ||
            reuse_last > table->last_usable_lba) {
            return INSTALLER_GPT_PRESERVE_OUT_OF_RANGE;
        }
        for (index = 0U;
             index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY; ++index) {
            const struct installer_gpt_preserve_entry *entry =
                &table->entries[index];

            if (entry->origin !=
                    INSTALLER_GPT_PRESERVE_SLOT_PRESERVED ||
                !entry->has_extent) {
                continue;
            }
            if (entry->first_lba == reuse->first_lba &&
                entry->last_lba == reuse_last &&
                memcmp(entry->type_guid, esp_guid, 16U) == 0) {
                reuse_index = index;
                break;
            }
        }
        if (reuse_index == INSTALLER_GPT_PRESERVE_NO_SLOT) {
            /*
             * The plan reserved the existing ESP by range; the disk does not
             * carry that range as an EFI System Partition entry.  The two
             * disagree about the existing system, which is exactly the overlap
             * refusal: nothing is written and nothing is guessed.
             */
            return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
        }
        table->entries[reuse_index].reused = true;
        table->entries[reuse_index].role = reuse->role;
        table->esp_index = reuse_index;
        table->esp_reused = true;
    }
    /*
     * Place the appliance's roles before any overlap arithmetic: a slot this
     * install wrote before is reused (recognised by its type and unique GUID,
     * which is what makes the write idempotent), otherwise the lowest slot whose
     * 128 bytes are all zero.  A slot that holds anything at all is never a
     * candidate.  The roles are claimed first so that a re-run compares the plan
     * against the *foreign* entries and not against its own previous table.
     */
    for (index = 0U; index < plan->partition_count; ++index) {
        const struct installer_layout_partition *partition =
            &plan->partitions[index];
        uint32_t slot = 0U;

        status = installer_gpt_preserve_check_partition(
            partition, table->first_usable_lba,
            table->last_usable_lba);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        status = installer_gpt_preserve_claim_slot(
            table, partition, false, &slot);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        table->entries[slot].first_lba = partition->first_lba;
        table->entries[slot].last_lba =
            partition->first_lba + partition->length_lba - 1U;
        table->entries[slot].origin =
            INSTALLER_GPT_PRESERVE_SLOT_ADDED;
        ++table->added_count;
    }

    /* Each role must clear every entry the disk already carries. */
    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const struct installer_gpt_preserve_entry *entry =
            &table->entries[index];
        uint32_t other;

        if (entry->origin != INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
            continue;
        }
        if (installer_gpt_preserve_span_covers(
                table, entry->first_lba, entry->last_lba)) {
            return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
        }
        /*
         * ITEM-501: the roles exist because the operator gave up a partition,
         * so they must live inside the extent that was released.  A plan that
         * puts them anywhere else is not the install the operator confirmed.
         */
        if (table->released_index != INSTALLER_GPT_PRESERVE_NO_SLOT) {
            const struct installer_gpt_preserve_entry *released =
                &table->entries[table->released_index];

            if (entry->first_lba < released->first_lba ||
                entry->last_lba > released->last_lba) {
                return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
            }
        }
        if (table->esp_reused &&
            entry->first_lba <= reuse_last &&
            reuse->first_lba <= entry->last_lba) {
            return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
        }
        for (other = index + 1U;
             other < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY; ++other) {
            const struct installer_gpt_preserve_entry *candidate =
                &table->entries[other];

            if (candidate->origin !=
                INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
                continue;
            }
            if (entry->first_lba <= candidate->last_lba &&
                candidate->first_lba <= entry->last_lba) {
                return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
            }
        }
        if (span_end == 0U) {
            span_first = entry->first_lba;
        } else if (entry->first_lba < span_first) {
            span_first = entry->first_lba;
        }
        if (entry->last_lba + 1U > span_end) {
            span_end = entry->last_lba + 1U;
        }
    }
    if (span_end <= span_first) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    status = installer_gpt_preserve_free_region(
        table, span_first, &free_first, &free_length);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    /*
     * The declared layout must fit inside one free region, not merely miss what
     * is preserved: a layout that reaches across a preserved entry - every role
     * clear of it, but no single free region large enough to hold the declared
     * footprint - is refused here, before the first write, as NO_FREE_SPACE.
     */
    if (span_end > free_first + free_length) {
        return INSTALLER_GPT_PRESERVE_NO_FREE_SPACE;
    }
    table->free_first_lba = free_first;
    table->free_length_lba = free_length;
    if (table->esp_index == INSTALLER_GPT_PRESERVE_NO_SLOT) {
        /*
         * A clean install adds its own ESP; the record then reports the slot it
         * landed in with esp_reused=0.
         */
        for (index = 0U;
             index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY; ++index) {
            if (table->entries[index].origin ==
                    INSTALLER_GPT_PRESERVE_SLOT_ADDED &&
                table->entries[index].role ==
                    INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
                table->esp_index = index;
                break;
            }
        }
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

static void installer_gpt_preserve_build_entry(
    unsigned char *entry,
    const struct installer_layout_partition *partition)
{
    const unsigned char *type_guid =
        installer_layout_role_type_guid(partition->role);
    unsigned char unique_guid[16U];

    memset(entry, 0, INSTALLER_GPT_PRESERVE_ENTRY_SIZE);
    if (type_guid == NULL) {
        return;
    }
    memcpy(entry + INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET,
           type_guid, 16U);
    installer_layout_role_unique_guid(
        unique_guid, partition->role, partition->partition_index);
    memcpy(entry + INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET,
           unique_guid, 16U);
    installer_gpt_preserve_store_le64(
        entry + INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET,
        partition->first_lba);
    installer_gpt_preserve_store_le64(
        entry + INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET,
        partition->first_lba + partition->length_lba - 1U);
}

static const struct installer_layout_partition *
installer_gpt_preserve_partition_in_slot(
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_table *table,
    uint32_t slot)
{
    uint32_t index;

    if (table->entries[slot].origin !=
        INSTALLER_GPT_PRESERVE_SLOT_ADDED) {
        return NULL;
    }
    for (index = 0U; index < plan->partition_count; ++index) {
        if (plan->partitions[index].role ==
            table->entries[slot].role) {
            return &plan->partitions[index];
        }
    }
    return NULL;
}

/*
 * Write one copy of the entry array.  Every sector is read from the primary
 * array first - the authoritative copy this install validated - and only the
 * slots that hold one of our added entries are touched; a slot that is not
 * all-zero belongs to the machine and is never rewritten.  The CRC of the array
 * is computed while it is written, so both copies and both headers agree.
 */
static enum installer_gpt_preserve_status
installer_gpt_preserve_write_array(
    struct block_device *device,
    uint64_t target_lba,
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_table *table,
    uint32_t *array_crc)
{
    unsigned char sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ARRAY_SECTORS;
         ++index) {
        enum installer_gpt_preserve_status status =
            installer_gpt_preserve_device_read(
                device, 2U + index, sector);
        size_t entry;

        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        for (entry = 0U;
             entry < INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR;
             ++entry) {
            const uint32_t slot = (uint32_t)(
                index * INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR +
                entry);
            const struct installer_layout_partition *partition =
                installer_gpt_preserve_partition_in_slot(
                    plan, table, slot);

            /*
             * ITEM-501: the released entry becomes zero bytes.  The slot's
             * contents are compared with what `check()` validated before it is
             * zeroed, so a table that changed under the plan is refused instead
             * of erased.
             */
            if (slot == table->released_index) {
                const struct installer_gpt_preserve_entry *released =
                    &table->entries[slot];
                unsigned char *target =
                    sector +
                    entry * INSTALLER_GPT_PRESERVE_ENTRY_SIZE;

                if (installer_gpt_preserve_all_zero(
                        target,
                        INSTALLER_GPT_PRESERVE_ENTRY_SIZE) ||
                    memcmp(target +
                               INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET,
                           released->type_guid, 16U) != 0 ||
                    memcmp(target +
                               INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET,
                           released->unique_guid, 16U) != 0 ||
                    installer_gpt_preserve_load_le64(
                        target +
                        INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET) !=
                        released->first_lba ||
                    installer_gpt_preserve_load_le64(
                        target +
                        INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET) !=
                        released->last_lba) {
                    return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
                }
                memset(target, 0, INSTALLER_GPT_PRESERVE_ENTRY_SIZE);
                continue;
            }
            if (partition == NULL) {
                continue;
            }
            if (!table->entries[slot].rewritten &&
                !installer_gpt_preserve_all_zero(
                    sector +
                        entry * INSTALLER_GPT_PRESERVE_ENTRY_SIZE,
                    INSTALLER_GPT_PRESERVE_ENTRY_SIZE)) {
                /*
                 * Belt and braces: `check()` only ever claims an all-zero slot
                 * or a slot that was recognised as this install's own entry, so
                 * a non-zero slot here means the table changed under the plan.
                 */
                return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
            }
            installer_gpt_preserve_build_entry(
                sector + entry * INSTALLER_GPT_PRESERVE_ENTRY_SIZE,
                partition);
        }
        {
            size_t byte_index;

            for (byte_index = 0U; byte_index < sizeof(sector);
                 ++byte_index) {
                unsigned int bit;

                crc ^= sector[byte_index];
                for (bit = 0U; bit < 8U; ++bit) {
                    if ((crc & 1U) != 0U) {
                        crc = (crc >> 1U) ^ 0xEDB88320U;
                    } else {
                        crc >>= 1U;
                    }
                }
            }
        }
        status = installer_gpt_preserve_write_verified(
            device, target_lba + index, sector);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
    }
    *array_crc = crc ^ 0xFFFFFFFFU;
    return INSTALLER_GPT_PRESERVE_OK;
}

static void installer_gpt_preserve_build_header(
    unsigned char *header,
    const unsigned char *original,
    uint64_t block_count,
    uint64_t my_lba,
    uint64_t alternate_lba,
    uint64_t entry_lba,
    uint32_t array_crc)
{
    uint32_t crc;

    memcpy(header, original, INSTALLER_GPT_PRESERVE_BLOCK_SIZE);
    installer_gpt_preserve_store_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_MY_LBA_OFFSET, my_lba);
    installer_gpt_preserve_store_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_ALTERNATE_OFFSET,
        alternate_lba);
    installer_gpt_preserve_store_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET,
        entry_lba);
    installer_gpt_preserve_store_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_COUNT_OFFSET,
        INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY);
    installer_gpt_preserve_store_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_SIZE_OFFSET,
        INSTALLER_GPT_PRESERVE_ENTRY_SIZE);
    installer_gpt_preserve_store_le64(
        header + INSTALLER_GPT_PRESERVE_HEADER_LAST_USABLE_OFFSET,
        block_count - INSTALLER_GPT_PRESERVE_TAIL_RESERVE_LBA);
    installer_gpt_preserve_store_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_ENTRY_CRC_OFFSET,
        array_crc);
    installer_gpt_preserve_store_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_CRC_OFFSET, 0U);
    crc = installer_gpt_preserve_crc32(
        header, INSTALLER_GPT_PRESERVE_HEADER_SIZE);
    installer_gpt_preserve_store_le32(
        header + INSTALLER_GPT_PRESERVE_HEADER_CRC_OFFSET, crc);
}

enum installer_gpt_preserve_status installer_gpt_preserve_write(
    struct block_device *device,
    struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table)
{
    unsigned char header[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char backup_header[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t array_crc = 0U;
    uint64_t backup_lba;
    uint64_t backup_array_lba;
    enum installer_gpt_preserve_status status;

    if (device == NULL || plan == NULL || table == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (table->version != INSTALLER_GPT_PRESERVE_VERSION ||
        plan->version != INSTALLER_LAYOUT_VERSION) {
        return INSTALLER_GPT_PRESERVE_BAD_VERSION;
    }
    if (!plan->confirmed) {
        return INSTALLER_GPT_PRESERVE_NOT_CONFIRMED;
    }
    if (plan->executed) {
        return INSTALLER_GPT_PRESERVE_ALREADY_EXECUTED;
    }
    if (!table->read_complete) {
        return INSTALLER_GPT_PRESERVE_BAD_STATE;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != table->block_size ||
        device->block_count != table->block_count) {
        return INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE;
    }
    /*
     * Every refusal lives here, before the first write: the plan is re-checked
     * against the table that is actually on the disk.
     */
    status = installer_gpt_preserve_check(table, plan, reuse);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    backup_lba = table->block_count - 1U;
    backup_array_lba = table->block_count -
        (INSTALLER_GPT_PRESERVE_ARRAY_SECTORS + 1U);

    /*
     * The backup GPT is completed first, so a failure at any later point leaves
     * a complete, self-consistent table that still holds every foreign entry.
     */
    status = installer_gpt_preserve_write_array(
        device, backup_array_lba, plan, table, &array_crc);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    installer_gpt_preserve_build_header(
        backup_header, table->backup_header, table->block_count,
        backup_lba, 1U, backup_array_lba, array_crc);
    status = installer_gpt_preserve_write_verified(
        device, backup_lba, backup_header);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_write_array(
        device, 2U, plan, table, &array_crc);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    installer_gpt_preserve_build_header(
        header, table->primary_header, table->block_count,
        1U, backup_lba, 2U, array_crc);
    status = installer_gpt_preserve_write_verified(device, 1U, header);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    /*
     * The protective MBR is written back byte-for-byte as it was validated:
     * the boot code and the disk signature in bytes 0..445 belong to the
     * machine, not to this installer.
     */
    status = installer_gpt_preserve_write_verified(
        device, 0U, table->mbr);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    memcpy(table->primary_header, header, sizeof(table->primary_header));
    memcpy(table->backup_header, backup_header,
           sizeof(table->backup_header));
    plan->executed = true;
    return INSTALLER_GPT_PRESERVE_OK;
}

static enum installer_gpt_preserve_status
installer_gpt_preserve_verify_slots(
    struct block_device *device,
    const struct installer_gpt_preserve_table *table)
{
    unsigned char sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    uint32_t index;

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const struct installer_gpt_preserve_entry *entry =
            &table->entries[index];
        const unsigned char *bytes;
        enum installer_gpt_preserve_status status;

        if (entry->origin == INSTALLER_GPT_PRESERVE_SLOT_EMPTY) {
            continue;
        }
        status = installer_gpt_preserve_device_read(
            device,
            2U + (uint64_t)(index /
                            INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR),
            sector);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        bytes = sector +
            (size_t)(index %
                     INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR) *
                INSTALLER_GPT_PRESERVE_ENTRY_SIZE;
        /*
         * ITEM-501: the one slot this install released must now be all-zero.
         * A preserved entry in its place, or any other bytes, is a write that
         * did not do what the operator confirmed.
         */
        if (entry->origin ==
            INSTALLER_GPT_PRESERVE_SLOT_RELEASED) {
            if (!installer_gpt_preserve_all_zero(
                    bytes, INSTALLER_GPT_PRESERVE_ENTRY_SIZE)) {
                return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
            }
            continue;
        }
        if (entry->origin ==
            INSTALLER_GPT_PRESERVE_SLOT_PRESERVED) {
            if (!entry->has_extent) {
                if (installer_gpt_preserve_all_zero(
                        bytes,
                        INSTALLER_GPT_PRESERVE_ENTRY_SIZE)) {
                    return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
                }
                continue;
            }
            if (memcmp(bytes + INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET,
                       entry->type_guid, 16U) != 0 ||
                memcmp(bytes + INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET,
                       entry->unique_guid, 16U) != 0 ||
                installer_gpt_preserve_load_le64(
                    bytes +
                    INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET) !=
                    entry->first_lba ||
                installer_gpt_preserve_load_le64(
                    bytes +
                    INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET) !=
                    entry->last_lba) {
                return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
            }
            continue;
        }
        {
            const unsigned char *type_guid =
                installer_layout_role_type_guid(entry->role);
            unsigned char unique_guid[16U];

            if (type_guid == NULL) {
                return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
            }
            installer_layout_role_unique_guid(
                unique_guid, entry->role, entry->partition_index);
            if (installer_gpt_preserve_all_zero(
                    bytes, INSTALLER_GPT_PRESERVE_ENTRY_SIZE) ||
                memcmp(bytes +
                           INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET,
                       type_guid, 16U) != 0 ||
                memcmp(bytes +
                           INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET,
                       unique_guid, 16U) != 0 ||
                installer_gpt_preserve_load_le64(
                    bytes +
                    INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET) !=
                    entry->first_lba ||
                installer_gpt_preserve_load_le64(
                    bytes +
                    INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET) !=
                    entry->last_lba) {
                return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
            }
        }
    }
    return INSTALLER_GPT_PRESERVE_OK;
}

enum installer_gpt_preserve_status installer_gpt_preserve_verify(
    struct block_device *device,
    const struct installer_gpt_preserve_table *table)
{
    struct installer_gpt_preserve_table current;
    enum installer_gpt_preserve_status status;
    uint64_t backup_array_lba;

    if (device == NULL || table == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (table->version != INSTALLER_GPT_PRESERVE_VERSION) {
        return INSTALLER_GPT_PRESERVE_BAD_VERSION;
    }
    backup_array_lba = table->block_count -
        (INSTALLER_GPT_PRESERVE_ARRAY_SECTORS + 1U);
    /*
     * The table on the disk is re-read by the same validation the install
     * started from, so the verification cannot pass on a table this writer
     * would have refused.
     */
    status = installer_gpt_preserve_read(
        device, table->disk_id, &current);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
    }
    if (installer_gpt_preserve_load_le64(
            current.primary_header +
            INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET) != 2U ||
        installer_gpt_preserve_load_le64(
            current.backup_header +
            INSTALLER_GPT_PRESERVE_HEADER_ENTRY_LBA_OFFSET) !=
            backup_array_lba ||
        current.entry_count != INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY ||
        current.entry_size != INSTALLER_GPT_PRESERVE_ENTRY_SIZE ||
        current.block_count != table->block_count ||
        memcmp(current.disk_guid, table->disk_guid, 16U) != 0 ||
        memcmp(current.mbr, table->mbr, sizeof(current.mbr)) != 0) {
        return INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED;
    }
    return installer_gpt_preserve_verify_slots(device, table);
}

enum installer_gpt_preserve_status installer_gpt_preserve_format_guid(
    const unsigned char guid[16U],
    char *buffer,
    size_t capacity)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t written = 0U;
    size_t index;

    if (guid == NULL || buffer == NULL ||
        capacity < INSTALLER_GPT_PRESERVE_GUID_TEXT_LENGTH) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    /*
     * A GPT GUID is stored with its first three fields little endian, and that
     * is the order Limine's mixed-endian GUID parser expects in the text form.
     */
    for (index = 0U; index < 4U; ++index) {
        buffer[written++] = hex[(guid[3U - index] >> 4U) & 0x0FU];
        buffer[written++] = hex[guid[3U - index] & 0x0FU];
    }
    buffer[written++] = '-';
    for (index = 0U; index < 2U; ++index) {
        buffer[written++] = hex[(guid[5U - index] >> 4U) & 0x0FU];
        buffer[written++] = hex[guid[5U - index] & 0x0FU];
    }
    buffer[written++] = '-';
    for (index = 0U; index < 2U; ++index) {
        buffer[written++] = hex[(guid[7U - index] >> 4U) & 0x0FU];
        buffer[written++] = hex[guid[7U - index] & 0x0FU];
    }
    buffer[written++] = '-';
    for (index = 8U; index < 10U; ++index) {
        buffer[written++] = hex[(guid[index] >> 4U) & 0x0FU];
        buffer[written++] = hex[guid[index] & 0x0FU];
    }
    buffer[written++] = '-';
    for (index = 10U; index < 16U; ++index) {
        buffer[written++] = hex[(guid[index] >> 4U) & 0x0FU];
        buffer[written++] = hex[guid[index] & 0x0FU];
    }
    buffer[written] = '\0';
    return INSTALLER_GPT_PRESERVE_OK;
}

/*
 * Read one GPT entry - the EFI System Partition of the disk the caller points
 * at - and hand back its unique GUID.  Read-only by construction: it uses the
 * same validation as the install path and no write entry point at all.
 */
enum installer_gpt_preserve_status installer_gpt_preserve_esp_guid(
    struct block_device *device,
    uint64_t first_lba,
    uint64_t length_lba,
    unsigned char unique_guid[16U])
{
    struct installer_gpt_preserve_header primary;
    unsigned char sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    unsigned char array_sector[INSTALLER_GPT_PRESERVE_BLOCK_SIZE];
    const unsigned char *esp_guid;
    enum installer_gpt_preserve_status status;
    uint64_t backup_lba;
    uint64_t backup_array_lba;
    uint64_t last_lba;
    uint64_t entry_lba;
    uint32_t slot;
    uint32_t index;
    bool found = false;

    if (device == NULL || unique_guid == NULL) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (installer_gpt_preserve_add_overflows(
            first_lba, length_lba, &last_lba)) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != INSTALLER_GPT_PRESERVE_BLOCK_SIZE ||
        device->block_count <
            2U * INSTALLER_GPT_PRESERVE_FIRST_USABLE_LBA + 1U) {
        return INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE;
    }
    backup_lba = device->block_count - 1U;
    backup_array_lba = device->block_count -
        (INSTALLER_GPT_PRESERVE_ARRAY_SECTORS + 1U);
    status = installer_gpt_preserve_device_read(device, 0U, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_mbr(sector, device->block_count);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_device_read(device, 1U, sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_header(
        sector, device->block_count, 1U, backup_lba, 2U, &primary);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    /*
     * A corrupt primary array is refused here too: the menu must not chainload
     * a loader named by an entry the table's own checksum disowns.
     */
    status = installer_gpt_preserve_array_crc_check(
        device, sector, primary.entry_lba);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_device_read(
        device, backup_lba, array_sector);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    status = installer_gpt_preserve_check_header(
        array_sector, device->block_count, backup_lba, 1U,
        backup_array_lba, &primary);
    if (status != INSTALLER_GPT_PRESERVE_OK) {
        return status;
    }
    esp_guid = installer_layout_role_type_guid(
        INSTALLER_LAYOUT_ROLE_EFI_SYSTEM);
    if (esp_guid == NULL) {
        return INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT;
    }
    for (slot = 0U; slot < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++slot) {
        const unsigned char *entry;

        entry_lba = 2U + (uint64_t)(slot /
                                    INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR);
        status = installer_gpt_preserve_device_read(
            device, entry_lba, array_sector);
        if (status != INSTALLER_GPT_PRESERVE_OK) {
            return status;
        }
        entry = array_sector +
            (size_t)(slot %
                     INSTALLER_GPT_PRESERVE_ENTRIES_PER_SECTOR) *
                INSTALLER_GPT_PRESERVE_ENTRY_SIZE;
        if (installer_gpt_preserve_all_zero(
                entry, INSTALLER_GPT_PRESERVE_ENTRY_SIZE)) {
            continue;
        }
        if (memcmp(entry + INSTALLER_GPT_PRESERVE_ENTRY_TYPE_OFFSET,
                   esp_guid, 16U) != 0) {
            continue;
        }
        if (installer_gpt_preserve_load_le64(
                entry + INSTALLER_GPT_PRESERVE_ENTRY_FIRST_OFFSET) !=
                first_lba ||
            installer_gpt_preserve_load_le64(
                entry + INSTALLER_GPT_PRESERVE_ENTRY_LAST_OFFSET) !=
                last_lba) {
            continue;
        }
        memcpy(unique_guid,
               entry + INSTALLER_GPT_PRESERVE_ENTRY_UNIQUE_OFFSET, 16U);
        if (installer_gpt_preserve_all_zero(unique_guid, 16U)) {
            return INSTALLER_GPT_PRESERVE_CORRUPT_TABLE;
        }
        found = true;
        break;
    }
    if (!found) {
        return INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP;
    }
    (void)index;
    return INSTALLER_GPT_PRESERVE_OK;
}

static void installer_gpt_preserve_append(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    if (buffer == NULL || text == NULL || length == NULL) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (*length + 1U >= capacity) {
            return;
        }
        buffer[*length] = text[index];
        ++*length;
        buffer[*length] = '\0';
    }
}

static void installer_gpt_preserve_append_decimal(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint64_t value)
{
    char digits[24U];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        char single[2U];

        single[0U] = digits[count - 1U];
        single[1U] = '\0';
        installer_gpt_preserve_append(buffer, capacity, length, single);
        --count;
    }
}

static const char *installer_gpt_preserve_role_text(
    enum installer_layout_role role)
{
    switch (role) {
    case INSTALLER_LAYOUT_ROLE_EFI_SYSTEM:
        return "efi-system";
    case INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM:
        return "hfoc-system";
    case INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE:
        return "hfoc-persistence";
    case INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY:
        return "hfoc-recovery";
    default:
        return "foreign";
    }
}

enum installer_gpt_preserve_status installer_gpt_preserve_render_record(
    const struct installer_gpt_preserve_table *table,
    const struct installer_layout_plan *plan,
    uint32_t menu_entry_count,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    size_t used = 0U;
    uint32_t index;
    uint32_t listed = 0U;

    if (table == NULL || plan == NULL || buffer == NULL ||
        length == NULL || capacity == 0U) {
        return INSTALLER_GPT_PRESERVE_NULL;
    }
    if (table->version != INSTALLER_GPT_PRESERVE_VERSION ||
        plan->version != INSTALLER_LAYOUT_VERSION) {
        return INSTALLER_GPT_PRESERVE_BAD_VERSION;
    }
    buffer[0] = '\0';
    installer_gpt_preserve_append(
        buffer, capacity, &used, "[INSTALLER-PRESERVE] plan mode=");
    installer_gpt_preserve_append(
        buffer, capacity, &used,
        table->released_index != INSTALLER_GPT_PRESERVE_NO_SLOT
            ? "release"
            : (table->esp_reused ? "dual-boot" : "clean-whole-disk"));
    installer_gpt_preserve_append(
        buffer, capacity, &used, " disk=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->disk_id);
    installer_gpt_preserve_append(buffer, capacity, &used, " table=gpt");
    installer_gpt_preserve_append(
        buffer, capacity, &used, " entry_count=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->entry_count);
    installer_gpt_preserve_append(buffer, capacity, &used, " preserved=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->preserved_count);
    installer_gpt_preserve_append(buffer, capacity, &used, " added=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->added_count);
    /* ITEM-501: only a release run carries the slot it gives up. */
    if (table->released_index != INSTALLER_GPT_PRESERVE_NO_SLOT) {
        installer_gpt_preserve_append(
            buffer, capacity, &used, " released_index=");
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, table->released_index);
    }
    installer_gpt_preserve_append(
        buffer, capacity, &used, " esp_reused=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->esp_reused ? 1U : 0U);
    installer_gpt_preserve_append(buffer, capacity, &used, " esp_index=");
    if (table->esp_index == INSTALLER_GPT_PRESERVE_NO_SLOT) {
        installer_gpt_preserve_append(buffer, capacity, &used, "none");
    } else {
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, table->esp_index);
    }
    installer_gpt_preserve_append(
        buffer, capacity, &used, " preserved_first_lba=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->preserved_first_lba);
    installer_gpt_preserve_append(
        buffer, capacity, &used, " preserved_lba=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used,
        table->preserved_last_lba >= table->preserved_first_lba &&
                table->preserved_last_lba != 0U
            ? table->preserved_last_lba -
                  table->preserved_first_lba + 1U
            : 0U);
    installer_gpt_preserve_append(
        buffer, capacity, &used, " free_first_lba=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->free_first_lba);
    installer_gpt_preserve_append(buffer, capacity, &used, " free_lba=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, table->free_length_lba);
    installer_gpt_preserve_append(buffer, capacity, &used, " menu_entries=");
    installer_gpt_preserve_append_decimal(
        buffer, capacity, &used, menu_entry_count);
    installer_gpt_preserve_append(buffer, capacity, &used, "\n");

    for (index = 0U; index < INSTALLER_GPT_PRESERVE_ENTRY_CAPACITY;
         ++index) {
        const struct installer_gpt_preserve_entry *entry =
            &table->entries[index];
        uint64_t length_lba;

        if (entry->origin == INSTALLER_GPT_PRESERVE_SLOT_EMPTY) {
            continue;
        }
        if (listed >= INSTALLER_GPT_PRESERVE_RECORD_SLOT_LINES) {
            installer_gpt_preserve_append(
                buffer, capacity, &used,
                "[INSTALLER-PRESERVE] omitted=");
            installer_gpt_preserve_append_decimal(
                buffer, capacity, &used,
                (uint64_t)(table->preserved_count +
                           table->added_count - listed -
                           (table->released_index !=
                                    INSTALLER_GPT_PRESERVE_NO_SLOT
                                ? 1U
                                : 0U)));
            installer_gpt_preserve_append(
                buffer, capacity, &used, "\n");
            break;
        }
        ++listed;
        installer_gpt_preserve_append(
            buffer, capacity, &used, "[INSTALLER-PRESERVE] slot index=");
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, index);
        installer_gpt_preserve_append(
            buffer, capacity, &used, " origin=");
        installer_gpt_preserve_append(
            buffer, capacity, &used,
            installer_gpt_preserve_origin_string(entry->origin));
        installer_gpt_preserve_append(buffer, capacity, &used, " role=");
        installer_gpt_preserve_append(
            buffer, capacity, &used,
            entry->origin == INSTALLER_GPT_PRESERVE_SLOT_ADDED
                ? installer_gpt_preserve_role_text(entry->role)
                : (entry->role == INSTALLER_LAYOUT_ROLE_EFI_SYSTEM
                       ? "efi-system"
                       : (entry->has_extent ? "foreign" : "opaque")));
        installer_gpt_preserve_append(
            buffer, capacity, &used, " reused=");
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, entry->reused ? 1U : 0U);
        if (!entry->has_extent) {
            installer_gpt_preserve_append(
                buffer, capacity, &used, " extent=0\n");
            continue;
        }
        length_lba = entry->last_lba - entry->first_lba + 1U;
        installer_gpt_preserve_append(
            buffer, capacity, &used, " first_lba=");
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, entry->first_lba);
        installer_gpt_preserve_append(
            buffer, capacity, &used, " length_lba=");
        installer_gpt_preserve_append_decimal(
            buffer, capacity, &used, length_lba);
        installer_gpt_preserve_append(buffer, capacity, &used, "\n");
    }
    *length = used;
    return INSTALLER_GPT_PRESERVE_OK;
}

const char *installer_gpt_preserve_origin_string(
    enum installer_gpt_preserve_origin origin)
{
    switch (origin) {
    case INSTALLER_GPT_PRESERVE_SLOT_EMPTY:
        return "empty";
    case INSTALLER_GPT_PRESERVE_SLOT_PRESERVED:
        return "preserved";
    case INSTALLER_GPT_PRESERVE_SLOT_ADDED:
        return "added";
    case INSTALLER_GPT_PRESERVE_SLOT_RELEASED:
        return "released";
    default:
        return NULL;
    }
}

const char *installer_gpt_preserve_status_string(
    enum installer_gpt_preserve_status status)
{
    switch (status) {
    case INSTALLER_GPT_PRESERVE_OK:
        return "INSTALLER_GPT_PRESERVE_OK";
    case INSTALLER_GPT_PRESERVE_NULL:
        return "INSTALLER_GPT_PRESERVE_NULL";
    case INSTALLER_GPT_PRESERVE_BAD_VERSION:
        return "INSTALLER_GPT_PRESERVE_BAD_VERSION";
    case INSTALLER_GPT_PRESERVE_BAD_STATE:
        return "INSTALLER_GPT_PRESERVE_BAD_STATE";
    case INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT:
        return "INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT";
    case INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE:
        return "INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE";
    case INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE:
        return "INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE";
    case INSTALLER_GPT_PRESERVE_CORRUPT_TABLE:
        return "INSTALLER_GPT_PRESERVE_CORRUPT_TABLE";
    case INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE:
        return "INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE";
    case INSTALLER_GPT_PRESERVE_OUT_OF_RANGE:
        return "INSTALLER_GPT_PRESERVE_OUT_OF_RANGE";
    case INSTALLER_GPT_PRESERVE_CAPACITY:
        return "INSTALLER_GPT_PRESERVE_CAPACITY";
    case INSTALLER_GPT_PRESERVE_NO_FREE_SPACE:
        return "INSTALLER_GPT_PRESERVE_NO_FREE_SPACE";
    case INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP:
        return "INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP";
    case INSTALLER_GPT_PRESERVE_NOT_CONFIRMED:
        return "INSTALLER_GPT_PRESERVE_NOT_CONFIRMED";
    case INSTALLER_GPT_PRESERVE_ALREADY_EXECUTED:
        return "INSTALLER_GPT_PRESERVE_ALREADY_EXECUTED";
    case INSTALLER_GPT_PRESERVE_IO_ERROR:
        return "INSTALLER_GPT_PRESERVE_IO_ERROR";
    case INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED:
        return "INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED";
    default:
        return NULL;
    }
}
