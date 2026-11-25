/*
 * ITEM-521 (increment 12), the owner's answer (c) to the ITEM-520 fork: the
 * medium's build reserves \HFOC\INSTALL.LOG in the FAT12 partition and this
 * module only reads what the build reserved.
 *
 * There is no FAT12 allocation here and no directory entry is ever written.
 * `fat12_journal_bind()` reads the BPB, walks the fixed root to the HFOC
 * directory, finds INSTALL.LOG, walks the 12-bit chain the build linked and
 * verifies the volumes's own record of the file: the directory size must equal
 * the chain's capacity, every chain cluster must be marked allocated in both
 * FAT copies, copies must agree, and slot 0 must carry the header the build
 * wrote (magic, version, slot size, capacity, commit point, CRC).  Only then
 * is the journal bound.
 *
 * Crash discipline:
 *   append: write the record slot -> rewrite slot 0 with the advanced commit
 *           point (both inside the reserved clusters);
 *   flush:  block_device_flush.
 * A crash before the header write leaves the record invisible to the reader;
 * a crash after it leaves a committed record.  Neither path can allocate,
 * resize or move anything: the file's clusters are exactly the ones the build
 * reserved.
 */

#include "install_fat12_journal.h"

#include "install_journal_core.h"

#include <string.h>

#define FAT12_ATTR_DIRECTORY 0x10U
#define FAT12_DIR_ENTRY_BYTES 32U
#define FAT12_EOC_MIN 0x0FF8U
#define FAT12_CLUSTER_LIMIT 4085U

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

const char *fat12_journal_status_string(enum fat12_journal_status status)
{
    switch (status) {
    case FAT12_JOURNAL_OK:
        return "OK";
    case FAT12_JOURNAL_REFUSED_NULL:
        return "NULL";
    case FAT12_JOURNAL_REFUSED_DEVICE:
        return "NO_DEVICE";
    case FAT12_JOURNAL_REFUSED_BLOCK_SIZE:
        return "BAD_BLOCK_SIZE";
    case FAT12_JOURNAL_REFUSED_READ:
        return "READ";
    case FAT12_JOURNAL_REFUSED_WRITE:
        return "WRITE";
    case FAT12_JOURNAL_REFUSED_HEADER_WRITE:
        return "HEADER_WRITE";
    case FAT12_JOURNAL_REFUSED_FLUSH:
        return "FLUSH";
    case FAT12_JOURNAL_REFUSED_NO_SIGNATURE:
        return "NO_SIGNATURE";
    case FAT12_JOURNAL_REFUSED_NOT_FAT12:
        return "NOT_FAT12";
    case FAT12_JOURNAL_REFUSED_BAD_GEOMETRY:
        return "BAD_GEOMETRY";
    case FAT12_JOURNAL_REFUSED_NO_ROOT_SLOT:
        return "NO_ROOT_SLOT";
    case FAT12_JOURNAL_REFUSED_NO_SPACE:
        return "NO_SPACE";
    case FAT12_JOURNAL_REFUSED_BAD_CHAIN:
        return "BAD_CHAIN";
    case FAT12_JOURNAL_REFUSED_FULL:
        return "FULL";
    case FAT12_JOURNAL_REFUSED_NOT_BOUND:
        return "NOT_BOUND";
    case FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME:
        return "INCONSISTENT_VOLUME";
    case FAT12_JOURNAL_REFUSED_NOT_RESERVED:
        return "NOT_RESERVED";
    case FAT12_JOURNAL_REFUSED_BAD_HEADER:
        return "BAD_HEADER";
    case FAT12_JOURNAL_REFUSED_BOUND:
        return "BOUND";
    }
    return "UNKNOWN";
}

uint32_t fat12_journal_capacity_slots(const struct fat12_journal *journal)
{
    if (journal == NULL || !journal->bound) {
        return 0U;
    }
    return journal->capacity_slots;
}

uint32_t fat12_journal_write_slot(const struct fat12_journal *journal)
{
    if (journal == NULL || !journal->bound) {
        return 0U;
    }
    return journal->write_slot;
}

/* --- device access, bounded by the partition extent ---------------------
 *
 * block_device only accepts sector-aligned offsets and lengths that are a
 * multiple of the block size, so every sub-sector access (a 32-byte directory
 * entry, a 2-byte FAT entry) goes through a 512-byte sector window here.
 */

static enum fat12_journal_status journal_read(
    const struct fat12_journal *journal, uint64_t offset, void *buffer,
    size_t length)
{
    if (offset > journal->partition_bytes ||
        length > journal->partition_bytes - offset) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    uint8_t *out = buffer;
    while (length > 0U) {
        uint8_t sector[FAT12_JOURNAL_SLOT_BYTES];
        uint64_t base = offset - (offset % FAT12_JOURNAL_SLOT_BYTES);
        uint32_t within = (uint32_t)(offset % FAT12_JOURNAL_SLOT_BYTES);
        uint32_t take = FAT12_JOURNAL_SLOT_BYTES - within;
        if ((size_t)take > length) {
            take = (uint32_t)length;
        }
        if (block_device_read(journal->device, journal->base_offset + base,
                              sector, sizeof(sector)) != BLOCK_DEVICE_OK) {
            return FAT12_JOURNAL_REFUSED_READ;
        }
        memcpy(out, &sector[within], take);
        out += take;
        offset += take;
        length -= take;
    }
    return FAT12_JOURNAL_OK;
}

static enum fat12_journal_status journal_write(
    struct fat12_journal *journal, uint64_t offset, const void *buffer,
    size_t length)
{
    if (offset > journal->partition_bytes ||
        length > journal->partition_bytes - offset) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    const uint8_t *in = buffer;
    while (length > 0U) {
        uint8_t sector[FAT12_JOURNAL_SLOT_BYTES];
        uint64_t base = offset - (offset % FAT12_JOURNAL_SLOT_BYTES);
        uint32_t within = (uint32_t)(offset % FAT12_JOURNAL_SLOT_BYTES);
        uint32_t take = FAT12_JOURNAL_SLOT_BYTES - within;
        if ((size_t)take > length) {
            take = (uint32_t)length;
        }
        if (within != 0U || take != FAT12_JOURNAL_SLOT_BYTES) {
            const enum block_device_status read =
                block_device_read(journal->device, journal->base_offset + base,
                                  sector, sizeof(sector));

            journal->last_block_status = read;
            if (read != BLOCK_DEVICE_OK) {
                return FAT12_JOURNAL_REFUSED_READ;
            }
        } else {
            memset(sector, 0, sizeof(sector));
        }
        memcpy(&sector[within], in, take);
        {
            const enum block_device_status write =
                block_device_write(journal->device, journal->base_offset + base,
                                   sector, sizeof(sector));

            journal->last_block_status = write;
            if (write != BLOCK_DEVICE_OK) {
                return FAT12_JOURNAL_REFUSED_WRITE;
            }
        }
        in += take;
        offset += take;
        length -= take;
    }
    return FAT12_JOURNAL_OK;
}

/* --- FAT entries, both copies, read-only -------------------------------- */

static uint64_t fat_entry_offset(const struct fat12_journal *journal,
                                 uint32_t copy, uint32_t cluster)
{
    uint64_t base = journal->fat_offset +
                    (uint64_t)copy * journal->sectors_per_fat *
                        journal->bytes_per_sector;
    return base + (uint64_t)cluster + (uint64_t)(cluster / 2U);
}

/* Read one 12-bit entry from one copy.  The entry can straddle a sector
   boundary, so it is read as the two bytes it is. */
static enum fat12_journal_status fat_entry_read_copy(
    const struct fat12_journal *journal, uint32_t copy, uint32_t cluster,
    uint16_t *value)
{
    uint8_t raw[2];
    enum fat12_journal_status status =
        journal_read(journal, fat_entry_offset(journal, copy, cluster), raw,
                     sizeof(raw));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    uint16_t word = le16(raw);
    *value = (cluster % 2U == 0U) ? (uint16_t)(word & 0x0FFFU)
                                  : (uint16_t)(word >> 4);
    return FAT12_JOURNAL_OK;
}

static enum fat12_journal_status fat_entry_read(
    const struct fat12_journal *journal, uint32_t cluster, uint16_t *value)
{
    return fat_entry_read_copy(journal, 0U, cluster, value);
}

/* --- the reserved file --------------------------------------------------- */

static bool entry_name_is(const uint8_t *entry, const char *name8,
                          const char *ext3)
{
    size_t name_length = strlen(name8);
    size_t ext_length = strlen(ext3);
    if (name_length > 8U || ext_length > 3U) {
        return false;
    }
    for (uint32_t i = 0U; i < 8U; ++i) {
        char want = (i < name_length) ? name8[i] : ' ';
        if ((char)entry[i] != want) {
            return false;
        }
    }
    for (uint32_t i = 0U; i < 3U; ++i) {
        char want = (i < ext_length) ? ext3[i] : ' ';
        if ((char)entry[8U + i] != want) {
            return false;
        }
    }
    return true;
}

static uint64_t dir_entry_offset(uint64_t base, uint32_t index)
{
    return base + (uint64_t)index * FAT12_DIR_ENTRY_BYTES;
}

static uint32_t entry_first_cluster(const uint8_t entry[FAT12_DIR_ENTRY_BYTES])
{
    /* FAT12/16: the high word at +20 must be zero and is not part of the
       cluster number. */
    return (uint32_t)le16(&entry[26]);
}

/* Read one 32-byte root entry through a cached root sector. */
static enum fat12_journal_status root_entry_read(
    struct fat12_journal *journal, uint32_t index,
    uint8_t entry[FAT12_DIR_ENTRY_BYTES])
{
    uint64_t offset = dir_entry_offset(journal->root_offset, index);
    uint64_t base = offset - (offset % FAT12_JOURNAL_SLOT_BYTES);
    uint8_t sector[FAT12_JOURNAL_SLOT_BYTES];
    enum fat12_journal_status status =
        journal_read(journal, base, sector, sizeof(sector));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    memcpy(entry, &sector[offset - base], FAT12_DIR_ENTRY_BYTES);
    return FAT12_JOURNAL_OK;
}

/* Find an entry in the fixed root. */
static enum fat12_journal_status root_find(
    struct fat12_journal *journal, const char *name8, const char *ext3,
    uint8_t found_entry[FAT12_DIR_ENTRY_BYTES], bool *found)
{
    *found = false;
    for (uint32_t i = 0U; i < journal->root_entries; ++i) {
        uint8_t entry[FAT12_DIR_ENTRY_BYTES];
        enum fat12_journal_status status = root_entry_read(journal, i, entry);
        if (status != FAT12_JOURNAL_OK) {
            return status;
        }
        if (entry[0] == 0x00U) {
            return FAT12_JOURNAL_OK;
        }
        if (entry[0] == 0xE5U) {
            continue;
        }
        if (entry_name_is(entry, name8, ext3)) {
            *found = true;
            memcpy(found_entry, entry, sizeof(entry));
            return FAT12_JOURNAL_OK;
        }
    }
    return FAT12_JOURNAL_OK;
}

/* Walk one directory's own cluster chain, bounded by the volume. */
static enum fat12_journal_status directory_next(
    const struct fat12_journal *journal, uint32_t cluster, uint32_t *next,
    bool *last)
{
    if (cluster < 2U || cluster >= journal->cluster_count + 2U) {
        return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
    }
    uint16_t value = 0U;
    enum fat12_journal_status status =
        fat_entry_read(journal, cluster, &value);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    if (value == 0U) {
        return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
    }
    *last = value >= FAT12_EOC_MIN;
    *next = value;
    return FAT12_JOURNAL_OK;
}

/* The reserved file: the HFOC directory must already exist and carry
   INSTALL.LOG as a regular file.  Nothing here writes a byte. */
static enum fat12_journal_status find_reserved_file(
    struct fat12_journal *journal, uint8_t entry_out[FAT12_DIR_ENTRY_BYTES])
{
    uint8_t entry[FAT12_DIR_ENTRY_BYTES];
    bool found = false;
    enum fat12_journal_status status =
        root_find(journal, "HFOC", "", entry, &found);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    if (!found || (entry[11] & FAT12_ATTR_DIRECTORY) == 0U) {
        return FAT12_JOURNAL_REFUSED_NOT_RESERVED;
    }
    uint32_t dir_cluster = entry_first_cluster(entry);
    if (dir_cluster < 2U || dir_cluster >= journal->cluster_count + 2U) {
        return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
    }
    journal->dir_cluster = dir_cluster;

    uint32_t entries_per_cluster =
        journal->cluster_bytes / FAT12_DIR_ENTRY_BYTES;
    uint32_t cluster = dir_cluster;
    for (uint32_t step = 0U; step <= journal->cluster_count; ++step) {
        uint64_t base = journal->data_offset +
                        (uint64_t)(cluster - 2U) * journal->cluster_bytes;
        for (uint32_t i = 0U; i < entries_per_cluster; ++i) {
            uint64_t offset = base + (uint64_t)i * FAT12_DIR_ENTRY_BYTES;
            status = journal_read(journal, offset, entry, sizeof(entry));
            if (status != FAT12_JOURNAL_OK) {
                return status;
            }
            if (entry[0] == 0x00U) {
                return FAT12_JOURNAL_REFUSED_NOT_RESERVED;
            }
            if (entry[0] == 0xE5U) {
                continue;
            }
            if (entry_name_is(entry, "INSTALL", "LOG")) {
                if ((entry[11] & FAT12_ATTR_DIRECTORY) != 0U) {
                    return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
                }
                journal->dir_entry_offset = offset;
                journal->first_cluster = entry_first_cluster(entry);
                memcpy(entry_out, entry, sizeof(entry));
                return FAT12_JOURNAL_OK;
            }
        }
        uint32_t next = 0U;
        bool last = false;
        status = directory_next(journal, cluster, &next, &last);
        if (status != FAT12_JOURNAL_OK || last) {
            return status == FAT12_JOURNAL_OK
                       ? FAT12_JOURNAL_REFUSED_NOT_RESERVED
                       : status;
        }
        cluster = next;
    }
    return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
}

/* The 12-bit chain the build linked, verified against both FAT copies: every
   cluster marked allocated, the copies equal, no loop, and a terminating EOC. */
static enum fat12_journal_status verify_chain(struct fat12_journal *journal)
{
    if (journal->first_cluster < 2U ||
        journal->first_cluster >= journal->cluster_count + 2U) {
        return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
    }
    uint32_t cluster = journal->first_cluster;
    uint32_t count = 0U;
    for (;;) {
        if (count >= FAT12_JOURNAL_MAX_CLUSTERS) {
            return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
        }
        journal->chain[count++] = cluster;
        uint16_t first = 0U;
        enum fat12_journal_status status =
            fat_entry_read_copy(journal, 0U, cluster, &first);
        if (status != FAT12_JOURNAL_OK) {
            return status;
        }
        if (first == 0U) {
            /* The FAT calls a cluster of the reserved file free: the volume's
               own record does not back the reservation, so a write could land
               in someone else's data. */
            return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
        }
        if (journal->fat_count > 1U) {
            uint16_t second = 0U;
            status = fat_entry_read_copy(journal, 1U, cluster, &second);
            if (status != FAT12_JOURNAL_OK) {
                return status;
            }
            if (second != first) {
                return FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME;
            }
        }
        if (first >= FAT12_EOC_MIN) {
            break;
        }
        if (first < 2U || first >= journal->cluster_count + 2U) {
            return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
        }
        for (uint32_t i = 0U; i < count; ++i) {
            if (journal->chain[i] == first) {
                return FAT12_JOURNAL_REFUSED_BAD_CHAIN;
            }
        }
        cluster = first;
    }
    journal->chain_length = count;
    journal->file_clusters = count;
    journal->capacity_slots = count * journal->slots_per_cluster;
    return FAT12_JOURNAL_OK;
}

/* Slot 0 of the reserved file, in the layout install_journal_core.h owns:
   the build writes it, the kernel only advances the commit point. */
static void format_header(const struct fat12_journal *journal, uint8_t *slot)
{
    memset(slot, 0, FAT12_JOURNAL_SLOT_BYTES);
    memcpy(slot, FAT12_JOURNAL_MAGIC, FAT12_JOURNAL_MAGIC_BYTES);
    put_le32(&slot[FAT12_JOURNAL_HEADER_VERSION_OFFSET],
             FAT12_JOURNAL_VERSION);
    put_le32(&slot[FAT12_JOURNAL_HEADER_SLOT_OFFSET],
             FAT12_JOURNAL_SLOT_BYTES);
    put_le32(&slot[FAT12_JOURNAL_HEADER_CAPACITY_OFFSET],
             journal->capacity_slots);
    put_le32(&slot[FAT12_JOURNAL_HEADER_WRITE_OFFSET], journal->write_slot);
    /* ITEM-552: the session counter, low dword first (the header reserves
       bytes 25..32 for it). */
    put_le32(&slot[FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET],
             (uint32_t)(journal->sequence & 0xFFFFFFFFULL));
    put_le32(&slot[FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET + 4U],
             (uint32_t)(journal->sequence >> 32U));
    put_le32(&slot[FAT12_JOURNAL_HEADER_CRC_OFFSET],
             install_journal_crc32(slot, FAT12_JOURNAL_HEADER_CRC_BYTES));
}

static uint64_t slot_offset(const struct fat12_journal *journal,
                            uint32_t slot)
{
    uint32_t cluster_index = slot / journal->slots_per_cluster;
    uint32_t slot_in_cluster = slot % journal->slots_per_cluster;
    return journal->data_offset +
           (uint64_t)(journal->chain[cluster_index] - 2U) *
               journal->cluster_bytes +
           (uint64_t)slot_in_cluster * FAT12_JOURNAL_SLOT_BYTES;
}

/* The header the build wrote must describe this file and nothing else. */
static enum fat12_journal_status read_header(struct fat12_journal *journal)
{
    uint8_t slot[FAT12_JOURNAL_SLOT_BYTES];
    enum fat12_journal_status status =
        journal_read(journal, slot_offset(journal, 0U), slot, sizeof(slot));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    if (memcmp(slot, FAT12_JOURNAL_MAGIC, FAT12_JOURNAL_MAGIC_BYTES) != 0 ||
        le32(&slot[FAT12_JOURNAL_HEADER_VERSION_OFFSET]) !=
            FAT12_JOURNAL_VERSION ||
        le32(&slot[FAT12_JOURNAL_HEADER_SLOT_OFFSET]) !=
            FAT12_JOURNAL_SLOT_BYTES ||
        le32(&slot[FAT12_JOURNAL_HEADER_CAPACITY_OFFSET]) !=
            journal->capacity_slots) {
        return FAT12_JOURNAL_REFUSED_BAD_HEADER;
    }
    if (install_journal_crc32(slot, FAT12_JOURNAL_HEADER_CRC_BYTES) !=
        le32(&slot[FAT12_JOURNAL_HEADER_CRC_OFFSET])) {
        return FAT12_JOURNAL_REFUSED_BAD_HEADER;
    }
    uint32_t write_slot = le32(&slot[FAT12_JOURNAL_HEADER_WRITE_OFFSET]);
    if (write_slot < 1U || write_slot > journal->capacity_slots) {
        return FAT12_JOURNAL_REFUSED_BAD_HEADER;
    }
    journal->write_slot = write_slot;
    journal->sequence = (uint64_t)le32(&slot[FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET]) |
                        ((uint64_t)le32(
                             &slot[FAT12_JOURNAL_HEADER_SEQUENCE_OFFSET + 4U])
                         << 32U);
    return FAT12_JOURNAL_OK;
}

/* --- bind --------------------------------------------------------------- */

static enum fat12_journal_status volume_geometry(
    struct fat12_journal *journal)
{
    uint8_t bpb[FAT12_JOURNAL_SLOT_BYTES];
    enum fat12_journal_status status = journal_read(journal, 0U, bpb,
                                                    sizeof(bpb));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    if (bpb[510] != 0x55U || bpb[511] != 0xAAU) {
        return FAT12_JOURNAL_REFUSED_NO_SIGNATURE;
    }
    journal->bytes_per_sector = le16(&bpb[11]);
    journal->sectors_per_cluster = bpb[13];
    journal->reserved_sectors = le16(&bpb[14]);
    journal->fat_count = bpb[16];
    journal->root_entries = le16(&bpb[17]);
    uint32_t total16 = le16(&bpb[19]);
    journal->sectors_per_fat = le16(&bpb[22]);
    uint32_t total32 = le32(&bpb[32]);
    journal->total_sectors = total16 != 0U ? total16 : total32;

    if (journal->bytes_per_sector != FAT12_JOURNAL_SLOT_BYTES ||
        journal->sectors_per_cluster == 0U || journal->fat_count == 0U ||
        journal->root_entries == 0U || journal->sectors_per_fat == 0U ||
        journal->total_sectors == 0U) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    uint64_t root_sectors =
        ((uint64_t)journal->root_entries * FAT12_DIR_ENTRY_BYTES +
         journal->bytes_per_sector - 1U) / journal->bytes_per_sector;
    uint64_t overhead = journal->reserved_sectors +
                        (uint64_t)journal->fat_count *
                            journal->sectors_per_fat + root_sectors;
    if (overhead >= journal->total_sectors) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    journal->cluster_count = (uint32_t)(
        (journal->total_sectors - overhead) / journal->sectors_per_cluster);
    if (journal->cluster_count < 1U ||
        journal->cluster_count >= FAT12_CLUSTER_LIMIT) {
        return FAT12_JOURNAL_REFUSED_NOT_FAT12;
    }
    /* A 12-bit FAT of sectors_per_fat sectors holds floor(spf*bps*2/3) entries
       and must cover cluster 0 .. cluster_count+1. */
    uint64_t fat_bytes =
        (uint64_t)journal->sectors_per_fat * journal->bytes_per_sector;
    uint64_t fat_entries = (fat_bytes * 2U) / 3U;
    if (fat_entries < (uint64_t)journal->cluster_count + 2U) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    journal->fat_bits = 12U;
    journal->cluster_bytes =
        journal->sectors_per_cluster * journal->bytes_per_sector;
    journal->slots_per_cluster =
        journal->cluster_bytes / FAT12_JOURNAL_SLOT_BYTES;
    journal->fat_offset =
        (uint64_t)journal->reserved_sectors * journal->bytes_per_sector;
    journal->root_offset = journal->fat_offset +
                           (uint64_t)journal->fat_count *
                               journal->sectors_per_fat *
                               journal->bytes_per_sector;
    journal->data_offset =
        journal->root_offset + root_sectors * journal->bytes_per_sector;
    if ((uint64_t)journal->total_sectors * journal->bytes_per_sector >
        journal->partition_bytes) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    return FAT12_JOURNAL_OK;
}

enum fat12_journal_status fat12_journal_bind(
    struct fat12_journal *journal, struct block_device *device,
    uint64_t partition_lba, uint64_t partition_blocks)
{
    if (journal == NULL || device == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    memset(journal, 0, sizeof(*journal));
    if (block_device_validate(device) != BLOCK_DEVICE_OK) {
        return FAT12_JOURNAL_REFUSED_DEVICE;
    }
    if (device->block_size != FAT12_JOURNAL_SLOT_BYTES) {
        return FAT12_JOURNAL_REFUSED_BLOCK_SIZE;
    }
    journal->device = device;
    journal->base_offset = partition_lba * FAT12_JOURNAL_SLOT_BYTES;
    journal->partition_bytes = partition_blocks * FAT12_JOURNAL_SLOT_BYTES;

    enum fat12_journal_status status = volume_geometry(journal);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    uint8_t entry[FAT12_DIR_ENTRY_BYTES];
    status = find_reserved_file(journal, entry);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    status = verify_chain(journal);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    /* The build reserves the whole chain: the file's own size must say so.
       A file whose size is smaller than its chain was not reserved by the
       build this kernel writes with, and refusing it keeps the "write only
       inside the reservation" promise honest. */
    uint32_t size = le32(&entry[28]);
    if (size != journal->capacity_slots * FAT12_JOURNAL_SLOT_BYTES) {
        return FAT12_JOURNAL_REFUSED_BAD_GEOMETRY;
    }
    status = read_header(journal);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    journal->bound = true;
    return FAT12_JOURNAL_OK;
}

enum fat12_journal_status fat12_journal_append(
    struct fat12_journal *journal,
    const uint8_t slot[FAT12_JOURNAL_SLOT_BYTES])
{
    if (journal == NULL || slot == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    if (!journal->bound) {
        return FAT12_JOURNAL_REFUSED_NOT_BOUND;
    }
    if (journal->write_slot >= journal->capacity_slots) {
        return FAT12_JOURNAL_REFUSED_FULL;
    }
    enum fat12_journal_status status = journal_write(
        journal, slot_offset(journal, journal->write_slot), slot,
        FAT12_JOURNAL_SLOT_BYTES);
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    /* The commit point: slot 0 is inside the same reserved chain, so the
       reader sees a record only once the header names it. */
    uint8_t header[FAT12_JOURNAL_SLOT_BYTES];
    format_header(journal, header);
    put_le32(&header[FAT12_JOURNAL_HEADER_WRITE_OFFSET],
             journal->write_slot + 1U);
    put_le32(&header[FAT12_JOURNAL_HEADER_CRC_OFFSET],
             install_journal_crc32(header,
                                   FAT12_JOURNAL_HEADER_CRC_BYTES));
    status = journal_write(journal, slot_offset(journal, 0U), header,
                           sizeof(header));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    journal->write_slot += 1U;
    return FAT12_JOURNAL_OK;
}

uint64_t fat12_journal_sequence(const struct fat12_journal *journal)
{
    return journal != NULL ? journal->sequence : 0U;
}

enum fat12_journal_status fat12_journal_rewind(struct fat12_journal *journal,
                                               uint64_t sequence)
{
    uint8_t header[FAT12_JOURNAL_SLOT_BYTES];
    enum fat12_journal_status status;

    if (journal == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    if (!journal->bound) {
        return FAT12_JOURNAL_REFUSED_NOT_BOUND;
    }
    /* The commit point first: a rewind that is itself interrupted leaves an
       empty journal (the header names slot 1), never a report that mixes two
       sessions.  The slots of the previous stream are simply not committed. */
    journal->write_slot = 1U;
    journal->sequence = sequence;
    format_header(journal, header);
    status = journal_write(journal, slot_offset(journal, 0U), header,
                           sizeof(header));
    if (status != FAT12_JOURNAL_OK) {
        return status;
    }
    return fat12_journal_flush(journal);
}

enum fat12_journal_status fat12_journal_commit_sequence(
    struct fat12_journal *journal,
    uint64_t sequence)
{
    uint8_t header[FAT12_JOURNAL_SLOT_BYTES];
    enum fat12_journal_status status;

    if (journal == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    if (!journal->bound) {
        return FAT12_JOURNAL_REFUSED_NOT_BOUND;
    }
    /* ITEM-571: the receipt is the attempt's own, and it separates the header
       write from the flush that makes it durable. */
    journal->commit = (struct fat12_journal_commit){0};
    journal->commit.attempted = true;
    journal->sequence = sequence;
    format_header(journal, header);
    journal->commit.header_attempted = true;
    status = journal_write(journal, slot_offset(journal, 0U), header,
                           sizeof(header));
    journal->commit.header_status = status;
    if (status != FAT12_JOURNAL_OK) {
        journal->commit.write_status = journal->last_block_status;
        return FAT12_JOURNAL_REFUSED_HEADER_WRITE;
    }
    journal->commit.header_written = true;
    journal->commit.flush_attempted = true;
    status = fat12_journal_flush(journal);
    journal->commit.flush_status = journal->last_block_status;
    if (status != FAT12_JOURNAL_OK) {
        return FAT12_JOURNAL_REFUSED_FLUSH;
    }
    return FAT12_JOURNAL_OK;
}

const struct fat12_journal_commit *fat12_journal_commit_evidence(
    const struct fat12_journal *journal)
{
    if (journal == NULL || !journal->commit.attempted) {
        return NULL;
    }
    return &journal->commit;
}

enum fat12_journal_status fat12_journal_read_slot(
    struct fat12_journal *journal,
    uint32_t slot,
    uint8_t bytes[FAT12_JOURNAL_SLOT_BYTES])
{
    if (journal == NULL || bytes == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    if (!journal->bound) {
        return FAT12_JOURNAL_REFUSED_NOT_BOUND;
    }
    if (slot >= journal->write_slot) {
        return FAT12_JOURNAL_REFUSED_BOUND;
    }
    return journal_read(journal, slot_offset(journal, slot), bytes,
                        FAT12_JOURNAL_SLOT_BYTES);
}

enum fat12_journal_status fat12_journal_flush(struct fat12_journal *journal)
{
    enum block_device_status status;

    if (journal == NULL || journal->device == NULL) {
        return FAT12_JOURNAL_REFUSED_NULL;
    }
    status = block_device_flush(journal->device);
    journal->last_block_status = status;
    if (status != BLOCK_DEVICE_OK) {
        return FAT12_JOURNAL_REFUSED_WRITE;
    }
    return FAT12_JOURNAL_OK;
}
