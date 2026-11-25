#include "appliance_gpt_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APPLIANCE_GPT_SIGNATURE_BYTES \
    { 'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T' }
#define APPLIANCE_GPT_HEADER_CRC_OFFSET 16U
#define APPLIANCE_GPT_HEADER_CURRENT_LBA_OFFSET 24U
#define APPLIANCE_GPT_HEADER_FIRST_USABLE_OFFSET 40U
#define APPLIANCE_GPT_HEADER_LAST_USABLE_OFFSET 48U
#define APPLIANCE_GPT_HEADER_ENTRY_LBA_OFFSET 72U
#define APPLIANCE_GPT_HEADER_ENTRY_COUNT_OFFSET 80U
#define APPLIANCE_GPT_HEADER_ENTRY_SIZE_OFFSET 84U
#define APPLIANCE_GPT_HEADER_ENTRY_CRC_OFFSET 88U

#define APPLIANCE_GPT_PARTITION_TYPE_OFFSET 0U
#define APPLIANCE_GPT_PARTITION_FIRST_LBA_OFFSET 32U
#define APPLIANCE_GPT_PARTITION_LAST_LBA_OFFSET 40U

static const unsigned char appliance_gpt_signature[8U] =
    APPLIANCE_GPT_SIGNATURE_BYTES;

static const unsigned char appliance_gpt_hfoc_persistence_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

/*
 * ITEM-410: the EFI System Partition type GUID
 * (C12A7328-F81F-11D2-BA4B-00A0C93EC93B, in the on-disk byte order), the one
 * the installer declares for its ESP role and the one
 * `scripts/hfoc-esp-read.py` selects.
 */
static const unsigned char appliance_gpt_esp_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

struct gpt_range {
    uint64_t first_lba;
    uint64_t last_lba;
};

static uint32_t appliance_gpt_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t appliance_gpt_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static uint32_t appliance_gpt_crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static uint32_t appliance_gpt_crc32(
    const unsigned char *bytes,
    size_t length)
{
    return appliance_gpt_crc32_update(
        0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static bool appliance_gpt_zero_range(
    const unsigned char *bytes,
    size_t offset,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[offset + index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool appliance_gpt_signature_valid(const unsigned char *bytes)
{
    size_t index;

    for (index = 0U; index < sizeof(appliance_gpt_signature); ++index) {
        if (bytes[index] != appliance_gpt_signature[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_gpt_guid_zero(const unsigned char guid[16U])
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        if (guid[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool appliance_gpt_guid_equal(
    const unsigned char left[16U],
    const unsigned char right[16U])
{
    size_t index;

    for (index = 0U; index < 16U; ++index) {
        if (left[index] != right[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_gpt_header_crc_valid(
    const unsigned char header[APPLIANCE_GPT_BLOCK_SIZE],
    uint32_t header_size)
{
    unsigned char copy[APPLIANCE_GPT_BLOCK_SIZE];
    uint32_t stored;
    uint32_t calculated;

    stored = appliance_gpt_load_le32(
        header + APPLIANCE_GPT_HEADER_CRC_OFFSET);
    memcpy(copy, header, header_size);
    memset(copy + APPLIANCE_GPT_HEADER_CRC_OFFSET, 0, 4U);
    calculated = appliance_gpt_crc32(copy, header_size);
    return stored == calculated;
}

static enum appliance_gpt_discovery_status
appliance_gpt_read_entries(
    struct block_device *device,
    uint64_t entry_lba,
    uint32_t entry_count,
    uint64_t first_usable,
    uint64_t last_usable,
    unsigned char *entry_block,
    struct gpt_range *ranges,
    size_t *range_count,
    bool *hfoc_found,
    uint32_t *hfoc_index,
    struct appliance_gpt_hfoc_partition *result)
{
    enum block_device_status io_status;
    size_t range_total = 0U;
    size_t entry_index = 0U;
    uint64_t remaining_entries = entry_count;
    uint64_t current_lba = entry_lba;

    while (remaining_entries != 0U) {
        const uint64_t entries_in_block =
            remaining_entries > (APPLIANCE_GPT_BLOCK_SIZE /
                                 APPLIANCE_GPT_ENTRY_SIZE)
                ? APPLIANCE_GPT_BLOCK_SIZE / APPLIANCE_GPT_ENTRY_SIZE
                : remaining_entries;
        size_t block_index;

        if (current_lba >= device->block_count) {
            return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
        }
        io_status = block_device_read(
            device, current_lba * APPLIANCE_GPT_BLOCK_SIZE,
            entry_block, APPLIANCE_GPT_BLOCK_SIZE);
        if (io_status != BLOCK_DEVICE_OK) {
            return APPLIANCE_GPT_DISCOVERY_IO_ERROR;
        }
        for (block_index = 0U;
             block_index < (size_t)entries_in_block;
             ++block_index, ++entry_index) {
            const unsigned char *entry =
                entry_block + block_index * APPLIANCE_GPT_ENTRY_SIZE;
            const unsigned char *type_guid =
                entry + APPLIANCE_GPT_PARTITION_TYPE_OFFSET;
            uint64_t first_lba;
            uint64_t last_lba;
            size_t range_index;

            if (appliance_gpt_guid_zero(type_guid)) {
                continue;
            }
            first_lba = appliance_gpt_load_le64(
                entry + APPLIANCE_GPT_PARTITION_FIRST_LBA_OFFSET);
            last_lba = appliance_gpt_load_le64(
                entry + APPLIANCE_GPT_PARTITION_LAST_LBA_OFFSET);
            if (first_lba == 0U ||
                first_lba < first_usable ||
                last_lba < first_lba ||
                last_lba > last_usable ||
                last_lba >= device->block_count) {
                return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
            }
            if (range_total >= APPLIANCE_GPT_PARTITION_CAPACITY) {
                return APPLIANCE_GPT_DISCOVERY_CAPACITY;
            }
            for (range_index = 0U; range_index < range_total;
                 ++range_index) {
                if (first_lba <= ranges[range_index].last_lba &&
                    last_lba >= ranges[range_index].first_lba) {
                    return APPLIANCE_GPT_DISCOVERY_OVERLAP;
                }
            }
            ranges[range_total].first_lba = first_lba;
            ranges[range_total].last_lba = last_lba;
            ++range_total;
            if (appliance_gpt_guid_equal(
                    type_guid, appliance_gpt_hfoc_persistence_guid)) {
                if (*hfoc_found) {
                    return APPLIANCE_GPT_DISCOVERY_AMBIGUOUS;
                }
                *hfoc_found = true;
                *hfoc_index = (uint32_t)entry_index + 1U;
                result->present = true;
                result->first_lba = first_lba;
                result->last_lba = last_lba;
                result->block_count = last_lba - first_lba + 1U;
            }
        }
        remaining_entries -= entries_in_block;
        ++current_lba;
    }
    *range_count = range_total;
    return APPLIANCE_GPT_DISCOVERY_OK;
}

/*
 * The header every walk of this module starts from: signature, revision, the
 * header CRC and the entry-array CRC, with the geometry the entries are read
 * with.  Extracted (ITEM-410) so the persistence lookup and the EFI System
 * Partition lookup accept and refuse exactly the same tables.
 */
static enum appliance_gpt_discovery_status appliance_gpt_read_header(
    struct block_device *device,
    unsigned char header[APPLIANCE_GPT_BLOCK_SIZE],
    unsigned char entry_block[APPLIANCE_GPT_BLOCK_SIZE],
    uint64_t *first_usable_out,
    uint64_t *last_usable_out,
    uint64_t *entry_lba_out,
    uint32_t *entry_count_out)
{
    enum block_device_status io_status;
    uint32_t revision;
    uint32_t header_size;
    uint32_t stored_entry_crc;
    uint32_t calculated_entry_crc;
    uint64_t current_lba;
    uint64_t first_usable;
    uint64_t last_usable;
    uint64_t entry_lba;
    uint32_t entry_count;
    uint32_t entry_size;
    uint64_t entry_bytes;
    uint64_t entry_blocks;
    uint64_t entry_remaining;
    uint64_t entry_current_lba;

    io_status = block_device_read(
        device,
        APPLIANCE_GPT_HEADER_LBA * APPLIANCE_GPT_BLOCK_SIZE,
        header, APPLIANCE_GPT_BLOCK_SIZE);
    if (io_status != BLOCK_DEVICE_OK) {
        return APPLIANCE_GPT_DISCOVERY_IO_ERROR;
    }
    if (!appliance_gpt_signature_valid(header)) {
        return APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE;
    }
    revision = appliance_gpt_load_le32(header + 8U);
    header_size = appliance_gpt_load_le32(header + 12U);
    if (revision != UINT32_C(0x00010000) ||
        header_size < APPLIANCE_GPT_HEADER_SIZE ||
        header_size > APPLIANCE_GPT_BLOCK_SIZE ||
        !appliance_gpt_zero_range(header, 20U, 4U) ||
        !appliance_gpt_header_crc_valid(header, header_size)) {
        return APPLIANCE_GPT_DISCOVERY_BAD_HEADER;
    }

    current_lba = appliance_gpt_load_le64(
        header + APPLIANCE_GPT_HEADER_CURRENT_LBA_OFFSET);
    first_usable = appliance_gpt_load_le64(
        header + APPLIANCE_GPT_HEADER_FIRST_USABLE_OFFSET);
    last_usable = appliance_gpt_load_le64(
        header + APPLIANCE_GPT_HEADER_LAST_USABLE_OFFSET);
    entry_lba = appliance_gpt_load_le64(
        header + APPLIANCE_GPT_HEADER_ENTRY_LBA_OFFSET);
    entry_count = appliance_gpt_load_le32(
        header + APPLIANCE_GPT_HEADER_ENTRY_COUNT_OFFSET);
    entry_size = appliance_gpt_load_le32(
        header + APPLIANCE_GPT_HEADER_ENTRY_SIZE_OFFSET);
    /*
     * The entry count is the size of the partition array in slots, not the
     * number of partitions on the disk: a table written by any other tool
     * carries the GPT's full 128 slots, and the preserving install keeps the
     * machine's own table (docs/hfoc-installer-preservation.md).
     */
    if (current_lba != APPLIANCE_GPT_HEADER_LBA ||
        first_usable < 34U ||
        last_usable < first_usable ||
        last_usable >= device->block_count ||
        entry_lba < 2U || entry_lba >= device->block_count ||
        entry_count == 0U ||
        entry_count > APPLIANCE_GPT_ENTRY_COUNT_MAX ||
        entry_size != APPLIANCE_GPT_ENTRY_SIZE) {
        return APPLIANCE_GPT_DISCOVERY_BAD_HEADER;
    }

    entry_bytes = (uint64_t)entry_count * entry_size;
    entry_blocks = (entry_bytes + APPLIANCE_GPT_BLOCK_SIZE - 1U) /
        APPLIANCE_GPT_BLOCK_SIZE;
    if (entry_blocks > device->block_count - entry_lba) {
        return APPLIANCE_GPT_DISCOVERY_OUT_OF_RANGE;
    }

    stored_entry_crc = appliance_gpt_load_le32(
        header + APPLIANCE_GPT_HEADER_ENTRY_CRC_OFFSET);
    calculated_entry_crc = 0xFFFFFFFFU;
    entry_remaining = entry_bytes;
    entry_current_lba = entry_lba;
    while (entry_remaining != 0U) {
        size_t crc_length = entry_remaining > APPLIANCE_GPT_BLOCK_SIZE
            ? APPLIANCE_GPT_BLOCK_SIZE
            : (size_t)entry_remaining;

        io_status = block_device_read(
            device, entry_current_lba * APPLIANCE_GPT_BLOCK_SIZE,
            entry_block, APPLIANCE_GPT_BLOCK_SIZE);
        if (io_status != BLOCK_DEVICE_OK) {
            return APPLIANCE_GPT_DISCOVERY_IO_ERROR;
        }
        calculated_entry_crc = appliance_gpt_crc32_update(
            calculated_entry_crc, entry_block, crc_length);
        entry_remaining -= crc_length;
        ++entry_current_lba;
    }
    calculated_entry_crc ^= 0xFFFFFFFFU;
    if (stored_entry_crc != calculated_entry_crc) {
        return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
    }

    *first_usable_out = first_usable;
    *last_usable_out = last_usable;
    *entry_lba_out = entry_lba;
    *entry_count_out = entry_count;
    return APPLIANCE_GPT_DISCOVERY_OK;
}

/*
 * ITEM-410: the `index`-th EFI System Partition of the table, in GPT slot
 * order.  The appliance boot log lives on the ESP that carries its own
 * installed boot set, and a machine can have more than one ESP - one per disk,
 * one per system - so the caller walks them and asks each one what it holds.
 * This is read-only: it reports an extent, and the caller decides whether the
 * extent is ours before anything is written anywhere.
 */
enum appliance_gpt_discovery_status appliance_gpt_find_esp_partition(
    struct block_device *device,
    uint32_t index,
    struct appliance_gpt_hfoc_partition *partition)
{
    enum block_device_status io_status;
    enum appliance_gpt_discovery_status status;
    unsigned char header[APPLIANCE_GPT_BLOCK_SIZE];
    unsigned char entry_block[APPLIANCE_GPT_BLOCK_SIZE];
    uint64_t first_usable;
    uint64_t last_usable;
    uint64_t entry_lba;
    uint32_t entry_count;
    uint64_t remaining_entries;
    uint64_t current_lba;
    uint32_t seen = 0U;
    uint32_t slot = 0U;

    if (device == NULL || partition == NULL) {
        return APPLIANCE_GPT_DISCOVERY_NULL;
    }
    memset(partition, 0, sizeof(*partition));
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return APPLIANCE_GPT_DISCOVERY_BAD_DEVICE;
    }
    if (device->block_size != APPLIANCE_GPT_BLOCK_SIZE) {
        return APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE;
    }
    if (device->block_count <= 33U) {
        return APPLIANCE_GPT_DISCOVERY_BAD_HEADER;
    }
    status = appliance_gpt_read_header(
        device, header, entry_block, &first_usable, &last_usable,
        &entry_lba, &entry_count);
    if (status != APPLIANCE_GPT_DISCOVERY_OK) {
        return status;
    }

    remaining_entries = entry_count;
    current_lba = entry_lba;
    while (remaining_entries != 0U) {
        const uint64_t entries_in_block =
            remaining_entries >
                    (APPLIANCE_GPT_BLOCK_SIZE / APPLIANCE_GPT_ENTRY_SIZE)
                ? APPLIANCE_GPT_BLOCK_SIZE / APPLIANCE_GPT_ENTRY_SIZE
                : remaining_entries;
        size_t block_index;

        if (current_lba >= device->block_count) {
            return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
        }
        io_status = block_device_read(
            device, current_lba * APPLIANCE_GPT_BLOCK_SIZE,
            entry_block, APPLIANCE_GPT_BLOCK_SIZE);
        if (io_status != BLOCK_DEVICE_OK) {
            return APPLIANCE_GPT_DISCOVERY_IO_ERROR;
        }
        for (block_index = 0U;
             block_index < (size_t)entries_in_block;
             ++block_index, ++slot) {
            const unsigned char *entry =
                entry_block + block_index * APPLIANCE_GPT_ENTRY_SIZE;
            const unsigned char *type_guid = entry;
            uint64_t entry_first;
            uint64_t entry_last;

            if (!appliance_gpt_guid_equal(
                    type_guid, appliance_gpt_esp_guid)) {
                continue;
            }
            entry_first = appliance_gpt_load_le64(
                entry + APPLIANCE_GPT_PARTITION_FIRST_LBA_OFFSET);
            entry_last = appliance_gpt_load_le64(
                entry + APPLIANCE_GPT_PARTITION_LAST_LBA_OFFSET);
            if (entry_first == 0U || entry_first < first_usable ||
                entry_last < entry_first || entry_last > last_usable ||
                entry_last >= device->block_count) {
                return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
            }
            if (seen == index) {
                partition->present = true;
                partition->partition_index = slot + 1U;
                partition->first_lba = entry_first;
                partition->last_lba = entry_last;
                partition->block_count = entry_last - entry_first + 1U;
                return APPLIANCE_GPT_DISCOVERY_OK;
            }
            ++seen;
        }
        remaining_entries -= entries_in_block;
        ++current_lba;
    }
    return APPLIANCE_GPT_DISCOVERY_NOT_FOUND;
}

enum appliance_gpt_discovery_status appliance_gpt_find_hfoc_partition(
    struct block_device *device,
    struct appliance_gpt_hfoc_partition *partition)
{
    enum appliance_gpt_discovery_status status;
    unsigned char header[APPLIANCE_GPT_BLOCK_SIZE];
    unsigned char entry_block[APPLIANCE_GPT_BLOCK_SIZE];
    struct gpt_range ranges[APPLIANCE_GPT_PARTITION_CAPACITY];
    uint64_t first_usable;
    uint64_t last_usable;
    uint64_t entry_lba;
    uint32_t entry_count;
    size_t range_count = 0U;
    bool hfoc_found = false;
    uint32_t hfoc_index = 0U;
    struct appliance_gpt_hfoc_partition draft;

    if (device == NULL || partition == NULL) {
        return APPLIANCE_GPT_DISCOVERY_NULL;
    }
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return APPLIANCE_GPT_DISCOVERY_BAD_DEVICE;
    }
    if (device->block_size != APPLIANCE_GPT_BLOCK_SIZE) {
        return APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE;
    }
    if (device->block_count <= 33U) {
        return APPLIANCE_GPT_DISCOVERY_BAD_HEADER;
    }
    status = appliance_gpt_read_header(
        device, header, entry_block, &first_usable, &last_usable,
        &entry_lba, &entry_count);
    if (status != APPLIANCE_GPT_DISCOVERY_OK) {
        return status;
    }

    memset(&draft, 0, sizeof(draft));
    memset(ranges, 0, sizeof(ranges));
    status = appliance_gpt_read_entries(
        device, entry_lba, entry_count, first_usable, last_usable,
        entry_block, ranges, &range_count, &hfoc_found,
        &hfoc_index, &draft);
    if (status != APPLIANCE_GPT_DISCOVERY_OK) {
        return status;
    }
    if (!hfoc_found) {
        return APPLIANCE_GPT_DISCOVERY_NOT_FOUND;
    }

    draft.partition_index = hfoc_index;
    *partition = draft;
    return APPLIANCE_GPT_DISCOVERY_OK;
}

enum appliance_gpt_discovery_status appliance_gpt_validate_hfoc_partition(
    const struct appliance_gpt_hfoc_partition *partition)
{
    if (partition == NULL) {
        return APPLIANCE_GPT_DISCOVERY_NULL;
    }
    if (!partition->present ||
        partition->partition_index == 0U ||
        partition->first_lba == 0U ||
        partition->last_lba < partition->first_lba ||
        partition->block_count == 0U ||
        partition->block_count !=
            partition->last_lba - partition->first_lba + 1U) {
        return APPLIANCE_GPT_DISCOVERY_BAD_ENTRY;
    }
    return APPLIANCE_GPT_DISCOVERY_OK;
}

const char *appliance_gpt_discovery_status_string(
    enum appliance_gpt_discovery_status status)
{
    switch (status) {
    case APPLIANCE_GPT_DISCOVERY_OK:
        return "APPLIANCE_GPT_DISCOVERY_OK";
    case APPLIANCE_GPT_DISCOVERY_NULL:
        return "APPLIANCE_GPT_DISCOVERY_NULL";
    case APPLIANCE_GPT_DISCOVERY_BAD_DEVICE:
        return "APPLIANCE_GPT_DISCOVERY_BAD_DEVICE";
    case APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE:
        return "APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE";
    case APPLIANCE_GPT_DISCOVERY_IO_ERROR:
        return "APPLIANCE_GPT_DISCOVERY_IO_ERROR";
    case APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE:
        return "APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE";
    case APPLIANCE_GPT_DISCOVERY_BAD_HEADER:
        return "APPLIANCE_GPT_DISCOVERY_BAD_HEADER";
    case APPLIANCE_GPT_DISCOVERY_BAD_ENTRY:
        return "APPLIANCE_GPT_DISCOVERY_BAD_ENTRY";
    case APPLIANCE_GPT_DISCOVERY_OVERLAP:
        return "APPLIANCE_GPT_DISCOVERY_OVERLAP";
    case APPLIANCE_GPT_DISCOVERY_AMBIGUOUS:
        return "APPLIANCE_GPT_DISCOVERY_AMBIGUOUS";
    case APPLIANCE_GPT_DISCOVERY_NOT_FOUND:
        return "APPLIANCE_GPT_DISCOVERY_NOT_FOUND";
    case APPLIANCE_GPT_DISCOVERY_OUT_OF_RANGE:
        return "APPLIANCE_GPT_DISCOVERY_OUT_OF_RANGE";
    case APPLIANCE_GPT_DISCOVERY_CAPACITY:
        return "APPLIANCE_GPT_DISCOVERY_CAPACITY";
    default:
        return "APPLIANCE_GPT_DISCOVERY_UNKNOWN";
    }
}
