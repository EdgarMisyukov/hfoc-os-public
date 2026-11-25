/*
 * ITEM-521 (increment 13): host proof for the install journal the medium's
 * build preallocates - the owner's answer (c) to the ITEM-520 fork.
 *
 * The build reserves \HFOC\INSTALL.LOG inside the FAT12 partition: it writes
 * the HFOC directory entry, the file's directory entry with size exactly equal
 * to the chain's capacity, the 12-bit chain in both FAT copies and the slot-0
 * journal header.  The kernel implements no FAT12 allocation at all: it reads
 * the BPB, the root, the HFOC directory, the file's own chain and the header,
 * and from then on it writes only inside the clusters the build already
 * assigned to the file.
 *
 * This witness drives install_fat12_journal.c over a RAM-backed block_device
 * that carries the same FAT12 geometry as the owner's medium (512 B sectors,
 * 2 sectors per cluster, 1 reserved sector, 2 FATs of 9 sectors, 240 root
 * entries, 3055 clusters), verifies every write with independent layout math
 * in this file, and pins:
 *
 *   * bind() reads the build reservation and writes zero sectors;
 *   * every append - the record slot and the slot-0 header rewrite - lands
 *     inside the reserved chain, and the FATs and directories never change;
 *   * the named refusals (NOT_RESERVED, BAD_HEADER, BAD_GEOMETRY,
 *     INCONSISTENT_VOLUME, BAD_CHAIN) and their status strings;
 *   * append/resume through the header's write_slot, the FULL bound, torn
 *     appends, the flush and the remaining geometry/device/NOT_BOUND coverage.
 *
 * block_device.c is linked in, so no kernel is needed.
 */

#include "install_fat12_journal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BPS 512U
#define SPF 9U
#define RESERVED 1U
#define NFAT 2U
#define ROOT_ENTRIES 240U
#define SPC 2U
#define TEST_SECTORS 6144U
#define ROOT_SECTORS ((ROOT_ENTRIES * 32U + BPS - 1U) / BPS)
#define ROOT_LBA (RESERVED + NFAT * SPF)
#define DATA_LBA (ROOT_LBA + ROOT_SECTORS)
#define CLUSTERS ((TEST_SECTORS - DATA_LBA) / SPC)

#define HFOC_CLUSTER 2U
#define FILE_FIRST 3U
#define EOC 0x0FFFU
#define WRITE_LOG 256U

static unsigned failures;
static unsigned checks;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

struct write_record {
    uint64_t offset;
    size_t length;
};

struct ram_disk {
    uint8_t *data;
    uint64_t bytes;
    bool fail_read;
    bool fail_write;
    bool fail_flush;
    uint64_t write_fail_offset;
    uint32_t read_calls;
    uint32_t write_calls;
    uint32_t flushes;
    uint32_t recorded_writes;
    struct write_record writes[WRITE_LOG];
};

static enum block_device_status ram_read(struct block_device *device,
                                          uint64_t offset, void *buffer,
                                          size_t length)
{
    struct ram_disk *disk = device->context;
    disk->read_calls += 1U;
    if (disk->fail_read) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > disk->bytes || length > disk->bytes - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, disk->data + offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_write(struct block_device *device,
                                           uint64_t offset,
                                           const void *buffer, size_t length)
{
    struct ram_disk *disk = device->context;
    disk->write_calls += 1U;
    if (disk->recorded_writes < WRITE_LOG) {
        disk->writes[disk->recorded_writes].offset = offset;
        disk->writes[disk->recorded_writes].length = length;
        disk->recorded_writes += 1U;
    }
    if (disk->fail_write && offset <= disk->write_fail_offset &&
        offset + length > disk->write_fail_offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > disk->bytes || length > disk->bytes - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(disk->data + offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_flush(struct block_device *device)
{
    struct ram_disk *disk = device->context;
    disk->flushes += 1U;
    if (disk->fail_flush) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static void put_le16(uint8_t *p, uint16_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *p, uint32_t value)
{
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* The standard IEEE CRC32 (zlib.crc32), computed here so the witness does not
   borrow the writer's own implementation. */
static uint32_t crc32_bytes(const uint8_t *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0U; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1) ^
                  (0xEDB88320U & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

/* --- independent FAT12 image layout helpers ----------------------------- */

static uint8_t *image_create(uint32_t total_sectors, uint16_t sectors_per_fat,
                             uint32_t *out_bytes)
{
    uint32_t bytes = total_sectors * BPS;
    uint8_t *img = calloc(1U, bytes);
    if (img == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    img[510] = 0x55U;
    img[511] = 0xAAU;
    put_le16(&img[11], BPS);
    img[13] = SPC;
    put_le16(&img[14], RESERVED);
    img[16] = NFAT;
    put_le16(&img[17], ROOT_ENTRIES);
    put_le16(&img[19], (uint16_t)(total_sectors <= 0xFFFFU ? total_sectors : 0U));
    img[21] = 0xF8U;
    put_le16(&img[22], sectors_per_fat);
    put_le16(&img[24], 63U);
    put_le16(&img[26], 255U);
    put_le32(&img[28], 140U);
    put_le32(&img[32], total_sectors > 0xFFFFU ? total_sectors : 0U);
    img[38] = 0x29U;
    memcpy(&img[54], "FAT12   ", 8U);
    *out_bytes = bytes;
    return img;
}

/* Set one raw 12-bit FAT entry in one copy only. */
static void raw_fat12_set_copy(uint8_t *img, uint32_t copy, uint32_t cluster,
                               uint16_t value)
{
    uint32_t base = (RESERVED + copy * SPF) * BPS;
    uint32_t off = base + cluster + cluster / 2U;
    uint16_t word = le16(&img[off]);
    if (cluster % 2U == 0U) {
        word = (uint16_t)((word & 0xF000U) | (value & 0x0FFFU));
    } else {
        word = (uint16_t)((word & 0x000FU) | ((value & 0x0FFFU) << 4));
    }
    put_le16(&img[off], word);
}

static void raw_fat12_set(uint8_t *img, uint32_t cluster, uint16_t value)
{
    for (uint32_t copy = 0U; copy < NFAT; ++copy) {
        raw_fat12_set_copy(img, copy, cluster, value);
    }
}

static uint16_t raw_fat12(const uint8_t *img, uint32_t copy, uint32_t cluster)
{
    uint32_t base = (RESERVED + copy * SPF) * BPS;
    uint32_t off = cluster + cluster / 2U;
    uint16_t word = le16(&img[base + off]);
    return (cluster % 2U == 0U) ? (uint16_t)(word & 0x0FFFU)
                                : (uint16_t)(word >> 4);
}

static uint64_t cluster_offset(uint32_t cluster)
{
    return (uint64_t)(DATA_LBA + (cluster - 2U) * SPC) * BPS;
}

static void make_entry(uint8_t *entry, const char *name8, const char *ext3,
                       uint8_t attr, uint32_t cluster, uint32_t size)
{
    memset(entry, 0, 32U);
    memset(entry, ' ', 11U);
    memcpy(entry, name8, strlen(name8));
    memcpy(&entry[8], ext3, strlen(ext3));
    entry[11] = attr;
    put_le16(&entry[26], (uint16_t)cluster);
    put_le32(&entry[28], size);
}

/* The header the build writes into slot 0. */
static void header_put(uint8_t *slot, uint32_t capacity_slots,
                       uint32_t write_slot, bool corrupt_crc)
{
    memset(slot, 0, FAT12_JOURNAL_SLOT_BYTES);
    memcpy(slot, FAT12_JOURNAL_MAGIC, FAT12_JOURNAL_MAGIC_BYTES);
    put_le32(&slot[9], FAT12_JOURNAL_VERSION);
    put_le32(&slot[13], FAT12_JOURNAL_SLOT_BYTES);
    put_le32(&slot[17], capacity_slots);
    put_le32(&slot[21], write_slot);
    uint32_t crc = crc32_bytes(slot, FAT12_JOURNAL_HEADER_CRC_BYTES);
    if (corrupt_crc) {
        crc ^= 1U;
    }
    put_le32(&slot[FAT12_JOURNAL_HEADER_CRC_OFFSET], crc);
}

/*
 * The build reservation: \HFOC\INSTALL.LOG over an explicit cluster chain,
 * size exactly the chain's capacity, the chain marked in both FAT copies and
 * the build header in slot 0 of the first cluster.
 */
static uint8_t *volume_with_chain(uint32_t total_sectors,
                                  const uint32_t *chain, uint32_t chain_length,
                                  uint32_t *out_bytes)
{
    uint8_t *img = image_create(total_sectors, SPF, out_bytes);
    raw_fat12_set(img, 0U, 0x0FF8U);
    raw_fat12_set(img, 1U, 0x0FFFU);
    raw_fat12_set(img, HFOC_CLUSTER, EOC);
    for (uint32_t i = 0U; i < chain_length; ++i) {
        uint16_t value = (i + 1U == chain_length) ? EOC
                                                  : (uint16_t)chain[i + 1U];
        raw_fat12_set(img, chain[i], value);
    }
    make_entry(&img[ROOT_LBA * BPS], "HFOC", "", 0x10U, HFOC_CLUSTER, 0U);
    uint8_t *dir = &img[cluster_offset(HFOC_CLUSTER)];
    make_entry(&dir[0], ".", "", 0x10U, HFOC_CLUSTER, 0U);
    make_entry(&dir[32], "..", "", 0x10U, 0U, 0U);
    make_entry(&dir[64], "INSTALL", "LOG", 0x20U, chain[0],
               chain_length * SPC * BPS);
    header_put(&img[cluster_offset(chain[0])], chain_length * SPC, 1U, false);
    return img;
}

static uint8_t *volume_reserved(uint32_t total_sectors, uint32_t file_clusters,
                                uint32_t *out_bytes)
{
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    for (uint32_t i = 0U; i < file_clusters; ++i) {
        chain[i] = FILE_FIRST + i;
    }
    return volume_with_chain(total_sectors, chain, file_clusters, out_bytes);
}

/* Walk the chain through FAT copy 0 like an independent reader would. */
static uint32_t chain_from(const uint8_t *img, uint32_t first, uint32_t *out,
                           uint32_t limit)
{
    uint32_t count = 0U;
    uint32_t cluster = first;
    while (cluster >= 2U && cluster < CLUSTERS + 2U && count < limit) {
        out[count++] = cluster;
        uint16_t next = raw_fat12(img, 0U, cluster);
        if (next == 0U || next >= 0x0FF8U) {
            break;
        }
        cluster = next;
    }
    return count;
}

static bool read_chain_bytes(const uint8_t *img, uint32_t first,
                             uint32_t length, uint8_t *out)
{
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    uint32_t count = chain_from(img, first, chain, FAT12_JOURNAL_MAX_CLUSTERS);
    uint32_t done = 0U;
    for (uint32_t i = 0U; i < count && done < length; ++i) {
        uint32_t take = length - done;
        if (take > SPC * BPS) {
            take = SPC * BPS;
        }
        memcpy(&out[done], &img[cluster_offset(chain[i])], take);
        done += take;
    }
    return done == length;
}

static uint32_t header_write_slot(const uint8_t *img)
{
    return le32(&img[cluster_offset(FILE_FIRST) +
                    FAT12_JOURNAL_HEADER_WRITE_OFFSET]);
}

static uint32_t header_capacity_slots(const uint8_t *img)
{
    return le32(&img[cluster_offset(FILE_FIRST) +
                    FAT12_JOURNAL_HEADER_CAPACITY_OFFSET]);
}

static bool header_crc_ok(const uint8_t *img)
{
    const uint8_t *slot = &img[cluster_offset(FILE_FIRST)];
    return le32(&slot[FAT12_JOURNAL_HEADER_CRC_OFFSET]) ==
           crc32_bytes(slot, FAT12_JOURNAL_HEADER_CRC_BYTES);
}

static struct block_device device_for(uint8_t *img, uint32_t bytes,
                                      struct ram_disk *disk)
{
    memset(disk, 0, sizeof(*disk));
    disk->data = img;
    disk->bytes = bytes;
    struct block_device device = {.block_size = BPS,
                                  .block_count = bytes / BPS,
                                  .context = disk,
                                  .read_fn = ram_read,
                                  .write_fn = ram_write,
                                  .flush_fn = ram_flush};
    return device;
}

/* The FATs, the BPB and both directories are outside the reserved file. */
static bool protected_regions_same(const uint8_t *before, const uint8_t *after)
{
    if (memcmp(before, after, DATA_LBA * BPS) != 0) {
        return false;
    }
    uint64_t dir = cluster_offset(HFOC_CLUSTER);
    return memcmp(before + dir, after + dir, SPC * BPS) == 0;
}

static bool offset_inside_chain(const uint8_t *img, uint32_t first,
                                uint64_t offset, size_t length)
{
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    uint32_t count = chain_from(img, first, chain, FAT12_JOURNAL_MAX_CLUSTERS);
    for (uint32_t i = 0U; i < count; ++i) {
        uint64_t begin = cluster_offset(chain[i]);
        if (offset >= begin && offset + length <= begin + (uint64_t)SPC * BPS) {
            return true;
        }
    }
    return false;
}

/* --- tests -------------------------------------------------------------- */

/* Requirement 1: the build reservation binds, reports the chain's capacity and
   the build's commit point, and not one sector is written. */
static void test_bind_reads_the_build_reservation(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    uint8_t *before = malloc(bytes);
    CHECK(before != NULL, "snapshot allocated");
    memcpy(before, img, bytes);

    struct fat12_journal journal;
    enum fat12_journal_status status =
        fat12_journal_bind(&journal, &device, 0U, bytes / BPS);
    CHECK(status == FAT12_JOURNAL_OK, "the build-reserved file binds");
    CHECK(disk.write_calls == 0U, "bind issues zero writes");
    CHECK(memcmp(before, img, bytes) == 0,
          "bind leaves the medium byte-identical");
    CHECK(fat12_journal_capacity_slots(&journal) == 8U,
          "capacity is the reserved chain's slots");
    CHECK(fat12_journal_write_slot(&journal) == 1U,
          "the build header commits slot 1");
    CHECK(journal.bound, "a successful bind marks the journal bound");
    CHECK(journal.first_cluster == FILE_FIRST && journal.chain_length == 4U,
          "bind follows the build chain");

    /* Independent audit of the fixture the build is supposed to have left. */
    const uint8_t *hfoc = &img[ROOT_LBA * BPS];
    CHECK(hfoc[11] == 0x10U, "the root carries HFOC as a directory");
    CHECK(le16(&hfoc[26]) == HFOC_CLUSTER, "HFOC is one cluster");
    CHECK(raw_fat12(img, 0U, HFOC_CLUSTER) >= 0x0FF8U,
          "the HFOC chain ends");
    const uint8_t *dir = &img[cluster_offset(HFOC_CLUSTER)];
    const uint8_t *file = &dir[64];
    CHECK(memcmp(file, "INSTALL ", 8U) == 0 && memcmp(&file[8], "LOG", 3U) == 0,
          "the HFOC directory carries INSTALL.LOG");
    CHECK(le32(&file[28]) == 4U * SPC * BPS,
          "the file's size is the chain's capacity");
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    uint32_t count = chain_from(img, (uint32_t)le16(&file[26]), chain,
                                FAT12_JOURNAL_MAX_CLUSTERS);
    CHECK(count == 4U && chain[0] == FILE_FIRST && chain[3] == FILE_FIRST + 3U,
          "the chain is four allocated clusters");
    for (uint32_t i = 0U; i < count; ++i) {
        uint16_t expect = (i + 1U == count) ? EOC
                                            : (uint16_t)(FILE_FIRST + i + 1U);
        CHECK(raw_fat12(img, 0U, chain[i]) == expect, "file chain in FAT1");
        CHECK(raw_fat12(img, 1U, chain[i]) == expect, "file chain in FAT2");
    }

    /* The slot-0 header the build wrote. */
    const uint8_t *slot = &img[cluster_offset(FILE_FIRST)];
    CHECK(memcmp(slot, FAT12_JOURNAL_MAGIC, FAT12_JOURNAL_MAGIC_BYTES) == 0,
          "slot 0 carries the journal magic");
    CHECK(le32(&slot[9]) == FAT12_JOURNAL_VERSION, "header version is 1");
    CHECK(le32(&slot[13]) == FAT12_JOURNAL_SLOT_BYTES,
          "header slot size is 512");
    CHECK(le32(&slot[17]) == 8U, "header capacity is N*slots_per_cluster");
    CHECK(le32(&slot[21]) == 1U, "header write_slot is 1");
    CHECK(le32(&slot[36]) == crc32_bytes(slot, 36U),
          "header CRC32 covers bytes 0..35");
    free(before);
    free(img);
}

/* Requirement 2: no allocation, no outside writes. */
static void test_bind_and_append_write_only_inside_the_file(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    uint8_t *before = malloc(bytes);
    CHECK(before != NULL, "snapshot allocated");
    memcpy(before, img, bytes);

    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "bind for the write-scope proof");
    CHECK(disk.write_calls == 0U, "bind writes nothing at all");

    uint8_t records[3][FAT12_JOURNAL_SLOT_BYTES];
    for (uint32_t i = 0U; i < 3U; ++i) {
        memset(records[i], (int)(0xA0U + i), sizeof(records[i]));
        CHECK(fat12_journal_append(&journal, records[i]) == FAT12_JOURNAL_OK,
              "append one record");
    }
    CHECK(fat12_journal_flush(&journal) == FAT12_JOURNAL_OK, "flush the bound journal");
    CHECK(disk.flushes == 1U, "flush reaches the device");

    /* Every write - the header rewrite included - is inside the chain. */
    CHECK(disk.write_calls == 6U, "three appends write six sectors");
    for (uint32_t i = 0U; i < disk.recorded_writes; ++i) {
        CHECK(offset_inside_chain(img, FILE_FIRST, disk.writes[i].offset,
                                  disk.writes[i].length),
              "every write lands inside the file's own clusters");
    }
    /* The documented commit order: record slot first, the slot-0 header last. */
    CHECK(disk.recorded_writes == 6U &&
              disk.writes[0].offset ==
                  cluster_offset(FILE_FIRST) + FAT12_JOURNAL_SLOT_BYTES &&
              disk.writes[1].offset == cluster_offset(FILE_FIRST),
          "the first append writes the record then the header");
    CHECK(disk.writes[2].offset == cluster_offset(FILE_FIRST + 1U) &&
              disk.writes[3].offset == cluster_offset(FILE_FIRST),
          "the second append reuses the header slot as its commit point");
    CHECK(protected_regions_same(before, img),
          "the FATs and every directory sector are byte-for-byte unchanged");
    free(before);
    free(img);
}

static void expect_refusal(uint8_t *img, uint32_t bytes,
                           enum fat12_journal_status expected,
                           const char *token, const char *what)
{
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    uint8_t *before = malloc(bytes);
    if (before == NULL) {
        fprintf(stderr, "out of memory\n");
        exit(2);
    }
    memcpy(before, img, bytes);
    struct fat12_journal journal;
    enum fat12_journal_status status =
        fat12_journal_bind(&journal, &device, 0U, bytes / BPS);
    CHECK(status == expected, what);
    char named[160];
    snprintf(named, sizeof(named), "%s (status string is %s)", what, token);
    CHECK(strcmp(fat12_journal_status_string(status), token) == 0, named);
    CHECK(!journal.bound, "a refusal leaves the journal unbound");
    CHECK(disk.write_calls == 0U, "a refusal writes no sector");
    CHECK(memcmp(before, img, bytes) == 0,
          "a refusal leaves the medium byte-identical");
    free(before);
}

/* Requirement 3: every refusal by its exact name. */
static void test_named_refusals(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = NULL;

    /* no HFOC directory in the root */
    img = image_create(TEST_SECTORS, SPF, &bytes);
    raw_fat12_set(img, 0U, 0x0FF8U);
    raw_fat12_set(img, 1U, EOC);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_NOT_RESERVED,
                   "NOT_RESERVED", "a volume without HFOC is NOT_RESERVED");
    free(img);

    /* HFOC exists but carries no INSTALL.LOG entry */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    memset(&img[cluster_offset(HFOC_CLUSTER) + 64U], 0, 32U);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_NOT_RESERVED,
                   "NOT_RESERVED", "a HFOC without INSTALL.LOG is NOT_RESERVED");
    free(img);

    /* the reserved file's size is smaller than its chain capacity */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    put_le32(&img[cluster_offset(HFOC_CLUSTER) + 64U + 28U],
             4U * SPC * BPS - BPS);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_GEOMETRY,
                   "BAD_GEOMETRY", "a size below the chain capacity is BAD_GEOMETRY");
    free(img);

    /* the reserved file's size is larger than its chain capacity */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    put_le32(&img[cluster_offset(HFOC_CLUSTER) + 64U + 28U],
             4U * SPC * BPS + BPS);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_GEOMETRY,
                   "BAD_GEOMETRY", "a size above the chain capacity is BAD_GEOMETRY");
    free(img);

    /* HFOC exists but is a regular file, not a directory */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    img[ROOT_LBA * BPS + 11U] = 0x20U;
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_NOT_RESERVED,
                   "NOT_RESERVED", "an HFOC that is not a directory is NOT_RESERVED");
    free(img);

    /* the HFOC entry claims a cluster outside the volume */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    put_le16(&img[ROOT_LBA * BPS + 26U], (uint16_t)(CLUSTERS + 8U));
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_CHAIN,
                   "BAD_CHAIN", "an out-of-volume HFOC cluster is BAD_CHAIN");
    free(img);

    /* the reserved file claims a first cluster outside the volume */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    put_le16(&img[cluster_offset(HFOC_CLUSTER) + 64U + 26U],
             (uint16_t)(CLUSTERS + 8U));
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_CHAIN,
                   "BAD_CHAIN", "an out-of-volume file cluster is BAD_CHAIN");
    free(img);

    /* slot 0 was never written by the build */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    memset(&img[cluster_offset(FILE_FIRST)], 0, FAT12_JOURNAL_SLOT_BYTES);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_HEADER,
                   "BAD_HEADER", "an absent build header is BAD_HEADER");
    free(img);

    /* slot 0 carries a header whose CRC does not check out */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    header_put(&img[cluster_offset(FILE_FIRST)], 8U, 1U, true);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_HEADER,
                   "BAD_HEADER", "a bad build-header CRC is BAD_HEADER");
    free(img);

    /* the header's capacity does not describe the reserved chain */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    header_put(&img[cluster_offset(FILE_FIRST)], 16U, 1U, false);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_HEADER,
                   "BAD_HEADER", "a header capacity mismatch is BAD_HEADER");
    free(img);

    /* the header's commit point is not a slot the file can hold */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    header_put(&img[cluster_offset(FILE_FIRST)], 8U, 0U, false);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_HEADER,
                   "BAD_HEADER", "a zero header write_slot is BAD_HEADER");
    free(img);

    /* the two FAT copies disagree about a chain cluster */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    raw_fat12_set_copy(img, 1U, FILE_FIRST + 1U, EOC);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME,
                   "INCONSISTENT_VOLUME",
                   "disagreeing FAT copies over the chain are INCONSISTENT_VOLUME");
    free(img);

    /* the chain reaches a free (0x000) cluster */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    raw_fat12_set(img, FILE_FIRST + 1U, 0x0000U);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_CHAIN,
                   "BAD_CHAIN", "a chain reaching a free cluster is BAD_CHAIN");
    free(img);

    /* the chain loops back onto its first cluster */
    img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    raw_fat12_set(img, FILE_FIRST + 2U, FILE_FIRST);
    expect_refusal(img, bytes, FAT12_JOURNAL_REFUSED_BAD_CHAIN,
                   "BAD_CHAIN", "a looping chain is BAD_CHAIN");
    free(img);
}

/* Requirement 4: append k slots, the header commits 1+k, and a later bind
   resumes there. */
static void test_append_and_resume(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 3U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "bind the three-cluster reservation");
    CHECK(fat12_journal_capacity_slots(&journal) == 6U, "three clusters are six slots");
    CHECK(fat12_journal_write_slot(&journal) == 1U, "the build header is slot 1");

    uint8_t records[3][FAT12_JOURNAL_SLOT_BYTES];
    for (uint32_t i = 0U; i < 3U; ++i) {
        memset(records[i], (int)(0x30U + i), sizeof(records[i]));
        CHECK(fat12_journal_append(&journal, records[i]) == FAT12_JOURNAL_OK,
              "append one record into the reservation");
        CHECK(fat12_journal_write_slot(&journal) == i + 2U,
              "the in-RAM commit point advances");
    }
    CHECK(header_write_slot(img) == 4U,
          "the header's write_slot reads 1+k");
    CHECK(header_capacity_slots(img) == 6U,
          "append preserves the header's capacity");
    CHECK(header_crc_ok(img), "every commit rewrites the header CRC");

    /* the k records land in the consecutive slots of the chain */
    uint8_t out[6U * BPS];
    CHECK(read_chain_bytes(img, FILE_FIRST, sizeof(out), out),
          "the reserved chain covers all six slots");
    for (uint32_t i = 0U; i < 3U; ++i) {
        CHECK(memcmp(&out[(i + 1U) * BPS], records[i], BPS) == 0,
              "record i lands in chain slot i+1");
    }

    /* a second bind on the same image resumes from the header */
    struct fat12_journal resumed;
    CHECK(fat12_journal_bind(&resumed, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "the image re-binds after the appends");
    CHECK(fat12_journal_write_slot(&resumed) == 4U,
          "the second bind resumes at 1+k");
    CHECK(fat12_journal_capacity_slots(&resumed) == 6U,
          "the resumed capacity is unchanged");
    uint8_t extra[FAT12_JOURNAL_SLOT_BYTES];
    memset(extra, 0xEE, sizeof(extra));
    CHECK(fat12_journal_append(&resumed, extra) == FAT12_JOURNAL_OK,
          "the resumed journal keeps appending");
    CHECK(fat12_journal_write_slot(&resumed) == 5U,
          "the resumed append advances the header");
    CHECK(header_write_slot(img) == 5U, "the header carries the resumed commit");
    CHECK(read_chain_bytes(img, FILE_FIRST, sizeof(out), out),
          "the chain still reads after the resume");
    CHECK(memcmp(&out[4U * BPS], extra, BPS) == 0,
          "the resumed record lands in slot 4");
    free(img);
}

/* Requirement 5: the header's capacity is the bound. */
static void test_full_refusal(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 2U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "bind the two-cluster reservation");
    uint8_t slot[FAT12_JOURNAL_SLOT_BYTES];
    memset(slot, 0x11, sizeof(slot));
    for (uint32_t i = 0U; i < 3U; ++i) {
        CHECK(fat12_journal_append(&journal, slot) == FAT12_JOURNAL_OK,
              "fill the four-slot reservation");
    }
    CHECK(fat12_journal_write_slot(&journal) == 4U, "the capacity is reached");
    CHECK(header_write_slot(img) == 4U, "the header commits the full capacity");

    uint8_t *before = malloc(bytes);
    CHECK(before != NULL, "snapshot allocated");
    memcpy(before, img, bytes);
    uint32_t writes = disk.write_calls;
    CHECK(fat12_journal_append(&journal, slot) == FAT12_JOURNAL_REFUSED_FULL,
          "a full reservation refuses by name");
    CHECK(fat12_journal_write_slot(&journal) == 4U,
          "a full refusal does not move the commit point");
    CHECK(disk.write_calls == writes,
          "a full refusal writes nothing past the chain");
    CHECK(memcmp(before, img, bytes) == 0,
          "a full refusal leaves the medium byte-identical");
    free(before);
    free(img);
}

/* Requirement 6: a torn append never advances the commit point. */
static void test_torn_append_is_invisible(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    uint32_t chain[FAT12_JOURNAL_MAX_CLUSTERS];
    uint32_t count = chain_from(img, FILE_FIRST, chain,
                                FAT12_JOURNAL_MAX_CLUSTERS);
    CHECK(count == 4U, "the reserved chain is four clusters");

    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "bind for the torn append");
    uint8_t committed[FAT12_JOURNAL_SLOT_BYTES];
    memset(committed, 0x22, sizeof(committed));
    CHECK(fat12_journal_append(&journal, committed) == FAT12_JOURNAL_OK,
          "the first record commits");
    CHECK(fat12_journal_write_slot(&journal) == 2U, "the commit point is 2");

    /* the next record would go to slot 2, the first slot of chain[1] */
    uint8_t torn[FAT12_JOURNAL_SLOT_BYTES];
    memset(torn, 0x33, sizeof(torn));
    disk.fail_write = true;
    disk.write_fail_offset = cluster_offset(chain[1]);
    CHECK(fat12_journal_append(&journal, torn) ==
              FAT12_JOURNAL_REFUSED_WRITE,
          "a failed record write is REFUSED_WRITE");
    CHECK(fat12_journal_write_slot(&journal) == 2U,
          "a failed record write does not advance the commit point");
    CHECK(header_write_slot(img) == 2U && header_crc_ok(img),
          "the header still commits the last good record");
    uint8_t zero[FAT12_JOURNAL_SLOT_BYTES] = {0};
    CHECK(memcmp(&img[cluster_offset(chain[1])], zero, sizeof(zero)) == 0,
          "the failed record left no bytes behind");

    /* a later reader sees the last committed prefix only */
    disk.fail_write = false;
    struct fat12_journal resumed;
    CHECK(fat12_journal_bind(&resumed, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "the image re-binds after the torn record");
    CHECK(fat12_journal_write_slot(&resumed) == 2U,
          "a later bind resumes at the committed prefix");
    uint8_t out[6U * BPS];
    CHECK(read_chain_bytes(img, FILE_FIRST, sizeof(out), out),
          "the committed chain reads back");
    CHECK(memcmp(&out[FAT12_JOURNAL_SLOT_BYTES], committed,
                 FAT12_JOURNAL_SLOT_BYTES) == 0,
          "the committed record is intact");

    /* a torn header rewrite: the record landed, the commit point did not */
    uint8_t second[FAT12_JOURNAL_SLOT_BYTES];
    memset(second, 0x44, sizeof(second));
    CHECK(fat12_journal_append(&resumed, second) == FAT12_JOURNAL_OK,
          "the retried record commits");
    CHECK(fat12_journal_write_slot(&resumed) == 3U,
          "the retried record advances the commit point");
    uint8_t third[FAT12_JOURNAL_SLOT_BYTES];
    memset(third, 0x55, sizeof(third));
    disk.fail_write = true;
    disk.write_fail_offset = cluster_offset(chain[0]);
    CHECK(fat12_journal_append(&resumed, third) ==
              FAT12_JOURNAL_REFUSED_WRITE,
          "a failed header rewrite is REFUSED_WRITE");
    CHECK(fat12_journal_write_slot(&resumed) == 3U,
          "a failed header rewrite leaves the commit point");
    CHECK(header_write_slot(img) == 3U && header_crc_ok(img),
          "the header still commits the record before the failure");
    CHECK(memcmp(&img[cluster_offset(chain[1]) + FAT12_JOURNAL_SLOT_BYTES],
                 third, FAT12_JOURNAL_SLOT_BYTES) == 0,
          "the uncommitted record bytes are present but not committed");
    disk.fail_write = false;
    struct fat12_journal again;
    CHECK(fat12_journal_bind(&again, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "re-bind after the failed header rewrite");
    CHECK(fat12_journal_write_slot(&again) == 3U,
          "the resumed commit point ignores the uncommitted bytes");
    free(img);
}

/* Requirement 7 and 8: geometry, device validation and NOT_BOUND. */
static void test_geometry_and_device_refusals(void)
{
    struct ram_disk disk;
    uint8_t *zero = calloc(1U, 4096U);
    CHECK(zero != NULL, "zero image allocated");
    struct block_device device = device_for(zero, 4096U, &disk);
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, 8U) ==
              FAT12_JOURNAL_REFUSED_NO_SIGNATURE,
          "no 0x55AA signature is named");
    device.block_size = 1024U;
    CHECK(fat12_journal_bind(&journal, &device, 0U, 8U) ==
              FAT12_JOURNAL_REFUSED_BLOCK_SIZE,
          "a non-512 block device is named");
    device.block_size = BPS;
    device.block_count = 0U;
    CHECK(fat12_journal_bind(&journal, &device, 0U, 0U) ==
              FAT12_JOURNAL_REFUSED_DEVICE,
          "a device with no blocks is named");
    device.block_count = 8U;
    device.read_fn = NULL;
    CHECK(fat12_journal_bind(&journal, &device, 0U, 8U) ==
              FAT12_JOURNAL_REFUSED_DEVICE,
          "a device without a reader is named");
    free(zero);

    uint32_t bytes = 0U;
    uint8_t *big = image_create(70000U, 32U, &bytes);
    struct ram_disk big_disk;
    struct block_device big_device = device_for(big, bytes, &big_disk);
    CHECK(fat12_journal_bind(&journal, &big_device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_NOT_FAT12,
          "a FAT16-sized volume is refused as NOT_FAT12");
    free(big);

    uint8_t *short_fat = image_create(2000U, 1U, &bytes);
    struct ram_disk short_disk;
    struct block_device short_device = device_for(short_fat, bytes, &short_disk);
    CHECK(fat12_journal_bind(&journal, &short_device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_BAD_GEOMETRY,
          "a FAT too short for its cluster count is refused");
    free(short_fat);

    uint8_t *reserved = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk reserved_disk;
    struct block_device reserved_device =
        device_for(reserved, bytes, &reserved_disk);
    CHECK(fat12_journal_bind(&journal, &reserved_device, 0U, 64U) ==
              FAT12_JOURNAL_REFUSED_BAD_GEOMETRY,
          "a partition smaller than the BPB extent is refused");
    CHECK(reserved_disk.write_calls == 0U, "the geometry refusal writes nothing");
    CHECK(fat12_journal_bind(NULL, &reserved_device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_NULL,
          "a null journal is refused");
    CHECK(fat12_journal_bind(&journal, NULL, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_NULL,
          "a null device is refused");
    free(reserved);
}

static void test_read_error(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    disk.fail_read = true;
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_READ,
          "a failing device is a named read refusal");
    CHECK(disk.write_calls == 0U, "a read failure writes nothing");
    free(img);
}

/* The build may hand the kernel a fragmented reservation; appending follows the
   chain, not the disk order. */
static void test_fragmented_chain(void)
{
    uint32_t bytes = 0U;
    const uint32_t chain[4] = {3U, 5U, 7U, 9U};
    uint8_t *img = volume_with_chain(TEST_SECTORS, chain, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "a fragmented reservation binds");
    CHECK(journal.chain_length == 4U &&
              journal.chain[0] == 3U && journal.chain[1] == 5U &&
              journal.chain[2] == 7U && journal.chain[3] == 9U,
          "bind keeps the build's chain order");

    uint8_t records[7][FAT12_JOURNAL_SLOT_BYTES];
    for (uint32_t i = 0U; i < 7U; ++i) {
        memset(records[i], (int)(0x40U + i), sizeof(records[i]));
        CHECK(fat12_journal_append(&journal, records[i]) == FAT12_JOURNAL_OK,
              "append across the fragmented reservation");
    }
    uint8_t out[8U * BPS];
    CHECK(read_chain_bytes(img, 3U, sizeof(out), out),
          "the fragmented chain covers all eight slots");
    for (uint32_t i = 0U; i < 7U; ++i) {
        CHECK(memcmp(&out[(i + 1U) * BPS], records[i], BPS) == 0,
              "fragmented slot round-trips");
    }
    for (uint32_t i = 0U; i < disk.recorded_writes; ++i) {
        CHECK(offset_inside_chain(img, 3U, disk.writes[i].offset,
                                  disk.writes[i].length),
              "fragmented writes stay inside the chain");
    }
    free(img);
}

/* The chain array in the journal is bounded; a reservation longer than the
   bound is refused by name instead of overflowing it. */
static void test_chain_longer_than_the_bound(void)
{
    uint32_t bytes = 0U;
    uint32_t *chain = malloc((FAT12_JOURNAL_MAX_CLUSTERS + 1U) * sizeof(uint32_t));
    CHECK(chain != NULL, "long chain allocated");
    for (uint32_t i = 0U; i < FAT12_JOURNAL_MAX_CLUSTERS + 1U; ++i) {
        chain[i] = FILE_FIRST + i;
    }
    uint8_t *img = volume_with_chain(TEST_SECTORS, chain,
                                     FAT12_JOURNAL_MAX_CLUSTERS + 1U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    struct fat12_journal journal;
    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_REFUSED_BAD_CHAIN,
          "a chain past the journal's bound is BAD_CHAIN");
    CHECK(disk.write_calls == 0U, "the over-long chain writes nothing");
    free(img);
    free(chain);
}

static void test_status_contract(void)
{
    CHECK(strcmp(fat12_journal_status_string(FAT12_JOURNAL_OK), "OK") == 0,
          "OK token");
    CHECK(strcmp(fat12_journal_status_string(FAT12_JOURNAL_REFUSED_NO_SPACE),
                 "NO_SPACE") == 0,
          "NO_SPACE token");
    CHECK(strcmp(fat12_journal_status_string(FAT12_JOURNAL_REFUSED_FULL),
                 "FULL") == 0,
          "FULL token");
    CHECK(strcmp(fat12_journal_status_string(
                     FAT12_JOURNAL_REFUSED_INCONSISTENT_VOLUME),
                 "INCONSISTENT_VOLUME") == 0,
          "INCONSISTENT_VOLUME token");
    CHECK(strcmp(fat12_journal_status_string(
                     FAT12_JOURNAL_REFUSED_NOT_RESERVED),
                 "NOT_RESERVED") == 0,
          "NOT_RESERVED token");
    CHECK(strcmp(fat12_journal_status_string(FAT12_JOURNAL_REFUSED_BAD_HEADER),
                 "BAD_HEADER") == 0,
          "BAD_HEADER token");
    CHECK(strcmp(fat12_journal_status_string(FAT12_JOURNAL_REFUSED_NOT_BOUND),
                 "NOT_BOUND") == 0,
          "NOT_BOUND token");

    struct fat12_journal journal;
    memset(&journal, 0, sizeof(journal));
    uint8_t slot[FAT12_JOURNAL_SLOT_BYTES];
    memset(slot, 0, sizeof(slot));
    CHECK(fat12_journal_append(&journal, slot) ==
              FAT12_JOURNAL_REFUSED_NOT_BOUND,
          "append before bind is refused");
    CHECK(fat12_journal_append(NULL, slot) == FAT12_JOURNAL_REFUSED_NULL,
          "a null journal append is refused");
    CHECK(fat12_journal_capacity_slots(&journal) == 0U,
          "an unbound journal reports no capacity");
    CHECK(fat12_journal_write_slot(&journal) == 0U,
          "an unbound journal reports no commit point");
    CHECK(fat12_journal_capacity_slots(NULL) == 0U,
          "a null journal reports no capacity");
    CHECK(fat12_journal_write_slot(NULL) == 0U,
          "a null journal reports no commit point");
}


/* ITEM-571: the commit point writes the header and then flushes it, and the two
   failures are different facts: one is a write the device refused, the other a
   flush that did not complete.  The receipt keeps which step failed and the
   block result it got - the same distinction the diag refusal prints. */
static void test_the_commit_point_separates_its_two_steps(void)
{
    uint32_t bytes = 0U;
    uint8_t *img = volume_reserved(TEST_SECTORS, 4U, &bytes);
    struct ram_disk disk;
    struct block_device device = device_for(img, bytes, &disk);
    struct fat12_journal journal;
    const struct fat12_journal_commit *commit;

    CHECK(fat12_journal_bind(&journal, &device, 0U, bytes / BPS) ==
              FAT12_JOURNAL_OK,
          "the reserved file binds before the commit point");
    CHECK(fat12_journal_commit_sequence(&journal, 7U) == FAT12_JOURNAL_OK,
          "a clean commit point succeeds");
    commit = fat12_journal_commit_evidence(&journal);
    CHECK(commit != NULL && commit->attempted && commit->header_written &&
              commit->flush_attempted &&
              commit->header_status == FAT12_JOURNAL_OK &&
              commit->flush_status == BLOCK_DEVICE_OK,
          "the receipt of a clean commit names both steps as done");
    CHECK(fat12_journal_sequence(&journal) == 7U,
          "the committed sequence is the one the header carries");

    /* The header write is refused by the medium: the flush never runs. */
    disk.fail_write = true;
    disk.write_fail_offset = cluster_offset(FILE_FIRST);
    CHECK(fat12_journal_commit_sequence(&journal, 8U) ==
              FAT12_JOURNAL_REFUSED_HEADER_WRITE,
          "a refused header write is HEADER_WRITE, not a flush failure");
    commit = fat12_journal_commit_evidence(&journal);
    CHECK(commit != NULL && !commit->header_written && !commit->flush_attempted,
          "a refused header write stops before the flush");
    CHECK(commit != NULL && commit->write_status == BLOCK_DEVICE_IO_ERROR,
          "the header write keeps the block result the device gave");
    disk.fail_write = false;

    /* The header lands and the flush is refused: the two are now different. */
    disk.fail_flush = true;
    CHECK(fat12_journal_commit_sequence(&journal, 9U) ==
              FAT12_JOURNAL_REFUSED_FLUSH,
          "a refused flush is FLUSH, not the same value as the header write");
    commit = fat12_journal_commit_evidence(&journal);
    CHECK(commit != NULL && commit->header_written &&
              commit->flush_status == BLOCK_DEVICE_IO_ERROR,
          "the flush receipt keeps that the header landed and the block result");
    disk.fail_flush = false;

    CHECK(fat12_journal_commit_sequence(&journal, 10U) == FAT12_JOURNAL_OK,
          "a later clean commit succeeds again");
    free(img);
}

int main(void)
{
    test_bind_reads_the_build_reservation();
    test_bind_and_append_write_only_inside_the_file();
    test_named_refusals();
    test_append_and_resume();
    test_full_refusal();
    test_torn_append_is_invisible();
    test_geometry_and_device_refusals();
    test_read_error();
    test_fragmented_chain();
    test_chain_longer_than_the_bound();
    test_status_contract();
    test_the_commit_point_separates_its_two_steps();
    printf("install-fat12-journal-host: %u checks, %u failures\n", checks,
           failures);
    return failures == 0U ? 0 : 1;
}
