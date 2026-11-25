/*
 * ITEM-521 (increment 13): host proof for the platform seam on the
 * build-preallocated FAT12 placement - the owner's answer (c).
 *
 * The seam (kernel/src/install_journal_platform.c) is linked with the real
 * journal core, the real block device and the real preallocated FAT12 module;
 * only serial and the durable boot-log store are stubbed.  The RAM medium
 * carries the volume the medium's build leaves behind: the FAT12 partition
 * already holds \HFOC\INSTALL.LOG, its directory size equals its chain
 * capacity, both FAT copies carry the chain and slot 0 carries the build's
 * journal header.  The seam allocates nothing; this witness proves it binds
 * that reservation, replays the staged records into the file's own clusters,
 * refuses a volume without the reservation by name with zero writes, and that
 * a refused write leaves the journal UNAVAILABLE by name without failing the
 * run.
 *
 * The FAT12 geometry is the writer's own witness geometry: 512 B sectors,
 * 2 sectors per cluster, 1 reserved sector, 2 FATs of 9 sectors, 240 root
 * entries, 3055 clusters.
 */

#include "install_journal_platform.h"

#include "block_device.h"
#include "install_medium.h"

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
#define FILE_CLUSTERS 4U
#define EOC 0x0FFFU
#define WRITE_LOG 64U

static unsigned checks;
static unsigned failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        ++checks;                                                            \
        if (!(cond)) {                                                       \
            ++failures;                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

/* ---------------------------------------------------------------- stubs --- */

static char serial_text[4096];
static size_t serial_length;
static char store_text[4096];
static size_t store_length;

void serial_write(const char *text) {
    size_t length;
    if (text == NULL) {
        return;
    }
    length = strlen(text);
    if (serial_length + length < sizeof(serial_text)) {
        memcpy(&serial_text[serial_length], text, length);
        serial_length += length;
        serial_text[serial_length] = '\0';
    }
}

void serial_write_u64(uint64_t value) { (void)value; }
void serial_write_hex(uint64_t value) { (void)value; }
uint64_t serial_sequence_begin(void) { return 0U; }
void serial_sequence_end(uint64_t flags) { (void)flags; }

void appliance_boot_log_store_observe(const char *record, size_t length) {
    if (record == NULL || store_length + length >= sizeof(store_text)) {
        return;
    }
    memcpy(&store_text[store_length], record, length);
    store_length += length;
    store_text[store_length] = '\0';
}

/* ------------------------------------------------------------- the medium --- */

struct write_record {
    uint64_t offset;
    size_t length;
};

struct ram_medium {
    uint8_t *bytes;
    uint64_t size;
    unsigned writes;
    uint64_t first_write;
    uint64_t last_write;
    bool fail_writes;
    unsigned recorded_writes;
    struct write_record log[WRITE_LOG];
};

static struct ram_medium medium;

static enum block_device_status ram_read(struct block_device *device,
                                         uint64_t offset, void *buffer,
                                         size_t length) {
    struct ram_medium *ram = device->context;
    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(buffer, &ram->bytes[offset], length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_write(struct block_device *device,
                                          uint64_t offset,
                                          const void *buffer, size_t length) {
    struct ram_medium *ram = device->context;
    if (offset + length > ram->size) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    if (ram->recorded_writes < WRITE_LOG) {
        ram->log[ram->recorded_writes].offset = offset;
        ram->log[ram->recorded_writes].length = length;
        ram->recorded_writes += 1U;
    }
    if (ram->fail_writes) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(&ram->bytes[offset], buffer, length);
    ram->writes += 1U;
    if (ram->first_write == 0U || offset < ram->first_write) {
        ram->first_write = offset;
    }
    if (offset + length > ram->last_write) {
        ram->last_write = offset + length;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ram_flush(struct block_device *device) {
    (void)device;
    return BLOCK_DEVICE_OK;
}

/* ------------------------------------------------------------ layout math --- */

static void put_le16(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
}

static void put_le32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value & 0xFFU);
    p[1] = (uint8_t)((value >> 8) & 0xFFU);
    p[2] = (uint8_t)((value >> 16) & 0xFFU);
    p[3] = (uint8_t)((value >> 24) & 0xFFU);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint32_t crc32_bytes(const uint8_t *bytes, size_t length) {
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

static uint8_t *volume_create(uint32_t total_sectors) {
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
    put_le16(&img[19],
             (uint16_t)(total_sectors <= 0xFFFFU ? total_sectors : 0U));
    img[21] = 0xF8U;
    put_le16(&img[22], SPF);
    put_le16(&img[24], 63U);
    put_le16(&img[26], 255U);
    put_le32(&img[28], 140U);
    put_le32(&img[32], total_sectors > 0xFFFFU ? total_sectors : 0U);
    img[38] = 0x29U;
    memcpy(&img[54], "FAT12   ", 8U);
    return img;
}

static uint64_t cluster_offset(uint32_t cluster) {
    return (uint64_t)(DATA_LBA + (cluster - 2U) * SPC) * BPS;
}

static void fat_set_copy(uint8_t *img, uint32_t copy, uint32_t cluster,
                         uint16_t value) {
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

static void fat_set(uint8_t *img, uint32_t cluster, uint16_t value) {
    for (uint32_t copy = 0U; copy < NFAT; ++copy) {
        fat_set_copy(img, copy, cluster, value);
    }
}

static uint16_t fat_get_copy(const uint8_t *img, uint64_t volume_base,
                             uint32_t copy, uint32_t cluster) {
    uint64_t base = volume_base + (uint64_t)(RESERVED + copy * SPF) * BPS;
    uint32_t off = cluster + cluster / 2U;
    uint16_t word = le16(&img[base + off]);
    return (cluster % 2U == 0U) ? (uint16_t)(word & 0x0FFFU)
                                : (uint16_t)(word >> 4);
}

static void make_entry(uint8_t *entry, const char *name8, const char *ext3,
                       uint8_t attr, uint32_t cluster, uint32_t size) {
    memset(entry, 0, 32U);
    memset(entry, ' ', 11U);
    memcpy(entry, name8, strlen(name8));
    memcpy(&entry[8], ext3, strlen(ext3));
    entry[11] = attr;
    put_le16(&entry[26], (uint16_t)cluster);
    put_le32(&entry[28], size);
}

static void header_put(uint8_t *slot, uint32_t capacity_slots,
                       uint32_t write_slot) {
    memset(slot, 0, BPS);
    memcpy(slot, INSTALL_JOURNAL_MAGIC, INSTALL_JOURNAL_MAGIC_BYTES);
    put_le32(&slot[9], INSTALL_JOURNAL_VERSION);
    put_le32(&slot[13], INSTALL_JOURNAL_SLOT_BYTES);
    put_le32(&slot[17], capacity_slots);
    put_le32(&slot[21], write_slot);
    put_le32(&slot[36], crc32_bytes(slot, 36U));
}

/*
 * The build's reservation inside an already-created volume: the HFOC
 * directory, \HFOC\INSTALL.LOG with size == chain capacity, the 12-bit chain
 * in both FAT copies and the slot-0 header.
 */
static void volume_reserve(uint8_t *img, uint32_t file_clusters) {
    fat_set(img, 0U, 0x0FF8U);
    fat_set(img, 1U, EOC);
    fat_set(img, HFOC_CLUSTER, EOC);
    for (uint32_t i = 0U; i < file_clusters; ++i) {
        uint16_t value =
            (i + 1U == file_clusters) ? EOC : (uint16_t)(FILE_FIRST + i + 1U);
        fat_set(img, FILE_FIRST + i, value);
    }
    make_entry(&img[ROOT_LBA * BPS], "HFOC", "", 0x10U, HFOC_CLUSTER, 0U);
    uint8_t *dir = &img[cluster_offset(HFOC_CLUSTER)];
    make_entry(&dir[0], ".", "", 0x10U, HFOC_CLUSTER, 0U);
    make_entry(&dir[32], "..", "", 0x10U, 0U, 0U);
    make_entry(&dir[64], "INSTALL", "LOG", 0x20U, FILE_FIRST,
               file_clusters * SPC * BPS);
    header_put(&img[cluster_offset(FILE_FIRST)], file_clusters * SPC, 1U);
}

static void medium_reset(uint64_t sectors) {
    free(medium.bytes);
    memset(&medium, 0, sizeof(medium));
    medium.size = sectors * 512U;
    medium.bytes = calloc(1U, (size_t)medium.size);
    if (medium.bytes == NULL) {
        printf("FAIL: cannot allocate the RAM medium\n");
        exit(1);
    }
}

/* Copy a fresh reserved FAT12 volume into the partition that starts at `lba`. */
static void medium_place_fat12(uint32_t lba) {
    uint8_t *volume = volume_create(TEST_SECTORS);
    volume_reserve(volume, FILE_CLUSTERS);
    memcpy(&medium.bytes[(uint64_t)lba * BPS], volume, TEST_SECTORS * BPS);
    free(volume);
}

/* A FAT12 volume the build left unreserved. */
static void medium_place_without_hfoc(uint32_t lba) {
    uint8_t *volume = volume_create(TEST_SECTORS);
    fat_set(volume, 0U, 0x0FF8U);
    fat_set(volume, 1U, EOC);
    memcpy(&medium.bytes[(uint64_t)lba * BPS], volume, TEST_SECTORS * BPS);
    free(volume);
}

/* A reserved volume whose INSTALL.LOG entry was never written. */
static void medium_place_hfoc_without_file(uint32_t lba) {
    uint8_t *volume = volume_create(TEST_SECTORS);
    volume_reserve(volume, FILE_CLUSTERS);
    memset(&volume[cluster_offset(HFOC_CLUSTER) + 64U], 0, 32U);
    memcpy(&medium.bytes[(uint64_t)lba * BPS], volume, TEST_SECTORS * BPS);
    free(volume);
}

static struct block_device medium_device(void) {
    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.block_size = 512U;
    device.block_count = medium.size / 512U;
    device.context = &medium;
    device.read_fn = ram_read;
    device.write_fn = ram_write;
    device.flush_fn = ram_flush;
    return device;
}

static void mbr_sign(uint8_t *lba0) {
    lba0[510] = 0x55U;
    lba0[511] = 0xAAU;
}

static void mbr_partition(uint8_t *lba0, unsigned index, uint8_t type,
                          uint32_t start_lba, uint32_t sectors) {
    uint8_t *entry = &lba0[446U + (index * 16U)];
    entry[4] = type;
    put_le32(&entry[8], start_lba);
    put_le32(&entry[12], sectors);
}

static struct install_medium_verdict matched_verdict(uint32_t partition_index,
                                                     uint64_t first_lba,
                                                     uint64_t length_lba) {
    struct install_medium_verdict verdict;
    memset(&verdict, 0, sizeof(verdict));
    verdict.result = INSTALL_MEDIUM_MATCH;
    verdict.reason = "OK";
    verdict.partition_index = partition_index;
    verdict.first_lba = first_lba;
    verdict.length_lba = length_lba;
    return verdict;
}

static unsigned count_text(const char *haystack, const char *needle) {
    unsigned found = 0U;
    const char *at = haystack;
    while ((at = strstr(at, needle)) != NULL) {
        found += 1U;
        at += strlen(needle);
    }
    return found;
}

static void reset_records(void) {
    serial_text[0] = '\0';
    serial_length = 0U;
    store_text[0] = '\0';
    store_length = 0U;
}

/* --------------------------------------------------- reading the medium --- */

static uint32_t medium_chain(uint32_t lba, uint32_t first, uint32_t *out,
                             uint32_t limit) {
    uint32_t count = 0U;
    uint32_t cluster = first;
    while (cluster >= 2U && cluster < CLUSTERS + 2U && count < limit) {
        out[count++] = cluster;
        uint16_t next =
            fat_get_copy(medium.bytes, (uint64_t)lba * BPS, 0U, cluster);
        if (next == 0U || next >= 0x0FF8U) {
            break;
        }
        cluster = next;
    }
    return count;
}

static uint64_t medium_cluster_offset(uint32_t lba, uint32_t cluster) {
    return (uint64_t)lba * BPS + cluster_offset(cluster);
}

static bool medium_read_file(uint32_t lba, uint32_t length, uint8_t *out) {
    uint32_t chain[16];
    uint32_t count = medium_chain(lba, FILE_FIRST, chain, 16U);
    uint32_t done = 0U;
    for (uint32_t i = 0U; i < count && done < length; ++i) {
        uint32_t take = length - done;
        if (take > SPC * BPS) {
            take = SPC * BPS;
        }
        memcpy(&out[done],
               &medium.bytes[medium_cluster_offset(lba, chain[i])], take);
        done += take;
    }
    return done == length;
}

static uint32_t medium_header_write_slot(uint32_t lba) {
    return le32(&medium.bytes[medium_cluster_offset(lba, FILE_FIRST) + 21U]);
}

/* Every recorded write must fall inside one of the file's own clusters. */
static bool medium_writes_inside_file(uint32_t lba) {
    uint32_t chain[16];
    uint32_t count = medium_chain(lba, FILE_FIRST, chain, 16U);
    for (unsigned w = 0U; w < medium.recorded_writes; ++w) {
        bool inside = false;
        for (uint32_t i = 0U; i < count; ++i) {
            uint64_t begin = medium_cluster_offset(lba, chain[i]);
            if (medium.log[w].offset >= begin &&
                medium.log[w].offset + medium.log[w].length <=
                    begin + (uint64_t)SPC * BPS) {
                inside = true;
            }
        }
        if (!inside) {
            return false;
        }
    }
    return true;
}

static bool medium_protected_same(uint32_t lba, const uint8_t *before) {
    uint64_t base = (uint64_t)lba * BPS;
    /* BPB, both FATs and the root directory */
    if (memcmp(&before[base], &medium.bytes[base], DATA_LBA * BPS) != 0) {
        return false;
    }
    /* the HFOC directory cluster */
    uint64_t dir = base + cluster_offset(HFOC_CLUSTER);
    return memcmp(&before[dir], &medium.bytes[dir], SPC * BPS) == 0;
}

static bool refused_by_name(void) {
    if (count_text(store_text, "[INSTALL-LOG] state=UNAVAILABLE reason=") !=
        1U) {
        return false;
    }
    return count_text(store_text, "reason=NOT_RESERVED") == 1U ||
           count_text(store_text, "reason=NO_PLACEMENT") == 1U;
}

/* ------------------------------------------------------------- the proof --- */

int main(void) {
    struct block_device device;
    struct install_medium_verdict verdict;
    const char *record_a = "[XHCI] state=READY controller=0";
    const char *record_b = "machine: memory=512MiB";

    /* 1. The loader named the FAT12 partition: the seam binds the build's
          preallocated reservation, replays the staged record into the file's
          own clusters and writes nowhere else. */
    medium_reset(2048U + TEST_SECTORS);
    medium_place_fat12(2048U);
    uint8_t *before = malloc((size_t)medium.size);
    CHECK(before != NULL, "medium snapshot allocated");
    memcpy(before, medium.bytes, (size_t)medium.size);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    install_journal_platform_observe(record_a, strlen(record_a));
    install_journal_platform_observe(record_b, strlen(record_b));
    verdict = matched_verdict(1U, 2048U, TEST_SECTORS);
    CHECK(install_journal_platform_bind_medium(&device, &verdict),
          "the seam binds the preallocated fixture volume");
    CHECK(medium.writes > 0U, "the staged records reach the medium");
    CHECK(count_text(store_text, "[INSTALL-LOG]") == 0U,
          "a bound preallocated placement names no refusal");
    CHECK(medium_writes_inside_file(2048U),
          "every write lands inside the reserved file's own clusters");
    CHECK(medium_protected_same(2048U, before),
          "the FATs and the directories are byte-for-byte unchanged");
    CHECK(medium_header_write_slot(2048U) == 3U,
          "the committed header advances over both staged records");
    uint8_t file[FILE_CLUSTERS * SPC * BPS];
    CHECK(medium_read_file(2048U, sizeof(file), file),
          "the reserved chain covers the whole file");
    CHECK(le32(&file[BPS]) == (uint32_t)strlen(record_a) &&
              crc32_bytes(&file[BPS + 8U], strlen(record_a)) ==
                  le32(&file[BPS + 4U]) &&
              memcmp(&file[BPS + 8U], record_a, strlen(record_a)) == 0 &&
              le32(&file[2U * BPS]) == (uint32_t)strlen(record_b) &&
              crc32_bytes(&file[2U * BPS + 8U], strlen(record_b)) ==
                  le32(&file[2U * BPS + 4U]) &&
              memcmp(&file[2U * BPS + 8U], record_b, strlen(record_b)) == 0,
          "both staged records reach the reserved file in order");
    free(before);

    /* 2. The loader read the medium through its own filesystem (isohybrid):
          the partition is found by the MBR table and binds the same way. */
    medium_reset(8192U);
    mbr_sign(medium.bytes);
    mbr_partition(medium.bytes, 0U, 0xEFU, 2048U, TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    install_journal_platform_observe(record_a, strlen(record_a));
    verdict = matched_verdict(0U, 0U, 8192U);
    CHECK(install_journal_platform_bind_medium(&device, &verdict),
          "the MBR table finds the preallocated FAT12 volume");
    CHECK(count_text(store_text, "[INSTALL-LOG]") == 0U,
          "the table-found placement names no refusal");
    CHECK(medium_header_write_slot(2048U) == 2U &&
              medium_writes_inside_file(2048U),
          "the table-found reservation is the one written");

    /* 3. A non-FAT partition earlier in the MBR does not shadow the volume. */
    medium_reset(8192U);
    mbr_sign(medium.bytes);
    mbr_partition(medium.bytes, 0U, 0x0CU, 1024U, 256U);
    mbr_partition(medium.bytes, 1U, 0xEFU, 2048U, TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    install_journal_platform_observe(record_a, strlen(record_a));
    verdict = matched_verdict(0U, 0U, 8192U);
    CHECK(install_journal_platform_bind_medium(&device, &verdict),
          "an earlier non-FAT partition does not shadow the reserved volume");
    CHECK(count_text(store_text, "[INSTALL-LOG]") == 0U,
          "the shadowing case binds without a refusal");

    /* 4. No reserved file: the seam refuses by name and not one sector is
          written.  Both shapes - no HFOC and no entry - are the same loss. */
    medium_reset(2048U + TEST_SECTORS);
    medium_place_without_hfoc(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    install_journal_platform_observe(record_b, strlen(record_b));
    verdict = matched_verdict(1U, 2048U, TEST_SECTORS);
    CHECK(!install_journal_platform_bind_medium(&device, &verdict),
          "a volume without HFOC is refused");
    CHECK(refused_by_name(),
          "the refusal is the named NOT_RESERVED/NO_PLACEMENT loss");
    CHECK(medium.writes == 0U && medium.recorded_writes == 0U,
          "the missing reservation writes no sector");

    medium_reset(2048U + TEST_SECTORS);
    medium_place_hfoc_without_file(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    install_journal_platform_observe(record_b, strlen(record_b));
    verdict = matched_verdict(1U, 2048U, TEST_SECTORS);
    CHECK(!install_journal_platform_bind_medium(&device, &verdict),
          "a HFOC directory without INSTALL.LOG is refused");
    CHECK(refused_by_name(),
          "the missing file names the same NOT_RESERVED/NO_PLACEMENT loss");
    CHECK(medium.writes == 0U && medium.recorded_writes == 0U,
          "the missing file writes no sector");

    /* 5. A refused write leaves the journal UNAVAILABLE by name, keeps the
          commit point where it was, and never fails the run. */
    medium_reset(2048U + TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_records();
    verdict = matched_verdict(1U, 2048U, TEST_SECTORS);
    CHECK(install_journal_platform_bind_medium(&device, &verdict),
          "the reservation binds before the write failure");
    CHECK(medium.writes == 0U,
          "binding the empty ring writes nothing");
    medium.fail_writes = true;
    install_journal_platform_observe(record_a, strlen(record_a));
    CHECK(count_text(store_text, "[INSTALL-LOG] state=UNAVAILABLE reason=") ==
              1U,
          "a refused write leaves the journal UNAVAILABLE by name");
    CHECK(count_text(store_text, "reason=WRITE") == 1U,
          "the refused write is named WRITE");
    CHECK(medium_header_write_slot(2048U) == 1U,
          "the refused write leaves the commit point at the build header");
    CHECK(le32(&medium.bytes[medium_cluster_offset(2048U, FILE_FIRST) + 512U]) ==
              0U,
          "the refused record never reaches the medium");
    size_t named_length = store_length;
    install_journal_platform_observe(record_b, strlen(record_b));
    CHECK(store_length == named_length,
          "records after the refusal are counted, not re-named");
    medium.fail_writes = false;
    CHECK(install_journal_platform_flush(),
          "the flush of the bound journal still reaches the device");
    CHECK(store_length == named_length,
          "the flush does not re-name the already named loss");
    CHECK(install_journal_platform_panic_flush(),
          "the panic path still flushes what is bound");
    CHECK(store_length == named_length,
          "the panic path does not re-name the already named loss");

    free(medium.bytes);
    printf("install-journal-platform-fat12-host: %u checks, %u failures\n",
           checks, failures);
    return failures == 0U ? 0 : 1;
}
