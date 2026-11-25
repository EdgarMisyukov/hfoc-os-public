/*
 * ITEM-521 (increment 8, completed by increment 12): host proof for the
 * platform seam and for the naming of a proven medium that got no placement.
 *
 * The owner's requirement 3 says a journal that cannot be written is not a
 * failure of the run, but it is a record: "the write about it goes into the
 * ordinary records".  The optical boot has its own named refusal (increment
 * 6); this witness covers the other half - the identity guard proved the
 * medium and `install_journal_platform_bind_medium()` did not bind a placement
 * on it (no partition carries FAT12, the volume's own audit refuses it, the
 * table has no signature).  Nothing may be left to disappear with the
 * machine: the loss is named once instead.
 *
 * Increment 8 wrote this witness against the empty seam: every bind returned
 * false and the counting block view proved no sector was touched.  ITEM-520
 * answered "(c) the medium's build reserves \HFOC\INSTALL.LOG and the kernel
 * only writes inside it", so the seam now binds that reservation or refuses by
 * name, and the witness follows it:
 *
 *   * the partition the loader named, and a partition found through the MBR
 *     table, both bind the build-preallocated FAT12 file; a record staged
 *     before the bind is replayed into the file's own clusters and every write
 *     lands inside them;
 *   * a medium whose partitions carry no FAT12, or whose FAT12 volume carries
 *     no reserved file, is refused by name before the first write, and that
 *     name is one ordinary record;
 *   * the fallback `NO_PLACEMENT` naming from xhci.c never overwrites a
 *     specific refusal, and a later MATCH does not erase the optical reason;
 *   * the flush hooks and the panic path stay silent when the loss is already
 *     named, and nothing reaches a device the seam never bound.
 *
 * The module under test is kernel/src/install_journal_platform.c, linked
 * beside the real core and the real preallocated FAT12 writer.  The two kernel
 * services it reports through are stubs here: the serial stream and the
 * durable boot-log store, so the text, the route and the once-only rule are
 * all checkable without a machine.  The medium is a RAM disk holding the same
 * FAT12 geometry and the same build reservation the writer's own witness uses,
 * and its every access is counted.
 *
 * What this cannot prove, and says so instead of pretending: the call site
 * (the identity guard's MATCH branch in kernel/src/xhci.c) is pinned by the
 * check slices, and the boot-time shape of the line is the QEMU proof's.
 */

#include "install_journal_platform.h"

#include "block_device.h"
#include "install_medium.h"

#include <stdbool.h>
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
#define HFOC_CLUSTER 2U
#define FILE_FIRST 3U
#define FILE_CLUSTERS 4U
#define EOC 0x0FFFU

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

#define STORE_LINES 8U
#define STORE_LINE_BYTES 128U

static char serial_text[1024];
static size_t serial_length;
static char store_lines[STORE_LINES][STORE_LINE_BYTES];
static unsigned store_count;

void serial_write(const char *text) {
    if (text == NULL) {
        return;
    }
    const size_t length = strlen(text);
    if (serial_length + length < sizeof(serial_text)) {
        memcpy(&serial_text[serial_length], text, length);
        serial_length += length;
        serial_text[serial_length] = '\0';
    }
}

void serial_write_u64(uint64_t value) {
    (void)value;
}

void serial_write_hex(uint64_t value) {
    (void)value;
}

uint64_t serial_sequence_begin(void) {
    return 0U;
}

void serial_sequence_end(uint64_t flags) {
    (void)flags;
}

void appliance_boot_log_store_observe(const char *record, size_t length) {
    if (record == NULL || store_count >= STORE_LINES) {
        return;
    }
    size_t copy = length;
    if (copy >= STORE_LINE_BYTES) {
        copy = STORE_LINE_BYTES - 1U;
    }
    memcpy(store_lines[store_count], record, copy);
    store_lines[store_count][copy] = '\0';
    store_count += 1U;
}

/* ------------------------------------------------------------- the medium --- */

struct ram_medium {
    uint8_t *bytes;
    uint64_t size;
    unsigned writes;
    uint64_t first_write;
    uint64_t last_write;
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

/* The standard IEEE CRC32 (zlib.crc32), computed here so the witness does not
   borrow the writer's own implementation. */
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

static uint64_t cluster_offset(uint32_t cluster) {
    return (uint64_t)(DATA_LBA + (cluster - 2U) * SPC) * BPS;
}

static void fat_set_copy_at(uint8_t *img, uint64_t volume_base, uint32_t copy,
                            uint32_t cluster, uint16_t value) {
    uint64_t base = volume_base + (uint64_t)(RESERVED + copy * SPF) * BPS;
    uint32_t off = cluster + cluster / 2U;
    uint16_t word = le16(&img[base + off]);
    if (cluster % 2U == 0U) {
        word = (uint16_t)((word & 0xF000U) | (value & 0x0FFFU));
    } else {
        word = (uint16_t)((word & 0x000FU) | ((value & 0x0FFFU) << 4));
    }
    put_le16(&img[base + off], word);
}

static void fat_set(uint8_t *img, uint32_t cluster, uint16_t value) {
    for (uint32_t copy = 0U; copy < NFAT; ++copy) {
        fat_set_copy_at(img, 0U, copy, cluster, value);
    }
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
 * The reservation the medium's build leaves behind: the HFOC directory,
 * \HFOC\INSTALL.LOG with size == chain capacity, the 12-bit chain in both FAT
 * copies and the slot-0 journal header with write_slot = 1.  The kernel writes
 * nothing of this; the seam only binds it.
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

static uint8_t *volume_create(uint32_t total_sectors) {
    uint8_t *img = calloc(1U, (size_t)total_sectors * BPS);
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

static void medium_reset(uint64_t sectors) {
    free(medium.bytes);
    memset(&medium, 0, sizeof(medium));
    medium.size = sectors * BPS;
    medium.bytes = calloc(1U, (size_t)medium.size);
    if (medium.bytes == NULL) {
        printf("FAIL: cannot allocate the RAM medium\n");
        exit(1);
    }
}

/* Copy a fresh volume with the build's \HFOC\INSTALL.LOG reservation into the
   partition that starts at `lba`. */
static void medium_place_fat12(uint32_t lba) {
    uint8_t *volume = volume_create(TEST_SECTORS);
    volume_reserve(volume, FILE_CLUSTERS);
    memcpy(&medium.bytes[(uint64_t)lba * BPS], volume, TEST_SECTORS * BPS);
    free(volume);
}

/* Corrupt one raw 12-bit FAT entry of one copy of the placed volume. */
static void medium_fat_set_copy(uint32_t lba, uint32_t copy, uint32_t cluster,
                                uint16_t value) {
    fat_set_copy_at(medium.bytes, (uint64_t)lba * BPS, copy, cluster, value);
}

static struct block_device medium_device(void) {
    struct block_device device;
    memset(&device, 0, sizeof(device));
    device.block_size = BPS;
    device.block_count = medium.size / BPS;
    device.context = &medium;
    device.read_fn = ram_read;
    device.write_fn = ram_write;
    device.flush_fn = ram_flush;
    return device;
}

static void mbr_sign(void) {
    medium.bytes[510] = 0x55U;
    medium.bytes[511] = 0xAAU;
}

static void mbr_partition(unsigned index, uint8_t type, uint32_t start_lba,
                          uint32_t sectors) {
    uint8_t *entry = &medium.bytes[446U + (index * 16U)];
    entry[4] = type;
    put_le32(&entry[8], start_lba);
    put_le32(&entry[12], sectors);
}

static void reset_observations(void) {
    serial_text[0] = '\0';
    serial_length = 0U;
    memset(store_lines, 0, sizeof(store_lines));
    store_count = 0U;
}

static unsigned count_serial_lines(const char *needle) {
    unsigned found = 0U;
    const char *at = serial_text;
    while ((at = strstr(at, needle)) != NULL) {
        found += 1U;
        at += strlen(needle);
    }
    return found;
}

static unsigned count_store_lines(const char *needle) {
    unsigned found = 0U;
    for (unsigned i = 0U; i < store_count; ++i) {
        if (strcmp(store_lines[i], needle) == 0) {
            found += 1U;
        }
    }
    return found;
}

/* A substring count over the stored records: the refusal lines carry a reason
   the check may not want to spell out in full. */
static unsigned count_store_text(const char *needle) {
    unsigned found = 0U;
    for (unsigned i = 0U; i < store_count; ++i) {
        const char *at = store_lines[i];
        while ((at = strstr(at, needle)) != NULL) {
            found += 1U;
            at += strlen(needle);
        }
    }
    return found;
}

/* The region [lba, lba+sectors) of the medium contains `needle` - the staged
   record, once the writer has replayed it into the volume. */
static bool region_contains(uint32_t lba, uint32_t sectors,
                            const char *needle) {
    const size_t needle_length = strlen(needle);
    const uint64_t begin = (uint64_t)lba * BPS;
    const uint64_t end = begin + (uint64_t)sectors * BPS;
    for (uint64_t at = begin; at + needle_length <= end; ++at) {
        if (memcmp(&medium.bytes[at], needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------- the proof --- */

static const char record_a[] = "[XHCI] state=READY controller=0";
static const char record_b[] = "machine: memory=512MiB";

int main(void) {
    struct install_medium_verdict matched;
    struct block_device device;

    memset(&matched, 0, sizeof(matched));
    matched.result = INSTALL_MEDIUM_MATCH;
    matched.reason = "OK";

    /* 1. Records published before a medium exists wait in the ring: nothing
          is printed, nothing is stored and no device is touched. */
    install_journal_platform_init();
    reset_observations();
    install_journal_platform_observe(record_a, sizeof(record_a) - 1U);
    install_journal_platform_observe(record_b, sizeof(record_b) - 1U);
    CHECK(serial_length == 0U, "a staged record prints nothing by itself");
    CHECK(store_count == 0U, "a staged record stores nothing by itself");

    /* 2. The fallback name of a proven medium that got no placement is still
          the platform's own one-line policy: NO_PLACEMENT, once, and it never
          fails the run (the identity guard's false branch calls it). */
    CHECK(!install_journal_platform_medium_unbound(),
          "naming the unbound medium never fails the run");
    CHECK(count_serial_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT\n") == 1U,
          "the proven medium without a placement is named once on serial");
    CHECK(count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT") == 1U,
          "the same line is one ordinary durable record");
    CHECK(serial_length == strlen(
              "[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT\n"),
          "the journal's status is one line and nothing else");
    CHECK(!install_journal_platform_medium_unbound(),
          "a repeated naming is still not a failure");
    CHECK(count_serial_lines("[INSTALL-LOG] ") == 1U,
          "the first named reason stands and is not overwritten");

    /* 3. The partition the loader named is FAT12: the seam binds the writer,
          replays the staged record into the volume, and every write lands
          inside that partition. */
    medium_reset(2048U + TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_observations();
    install_journal_platform_observe(record_b, sizeof(record_b) - 1U);
    matched.partition_index = 1U;
    matched.first_lba = 2048U;
    matched.length_lba = TEST_SECTORS;
    CHECK(install_journal_platform_bind_medium(&device, &matched),
          "a named FAT12 partition binds");
    CHECK(medium.writes > 0U, "binding a named FAT12 partition writes it");
    CHECK(count_store_text("[INSTALL-LOG]") == 0U,
          "a bound FAT12 placement names no refusal");
    CHECK(medium.first_write >= 2048U * BPS &&
              medium.last_write <= (2048U + TEST_SECTORS) * BPS,
          "a named FAT12 partition binds and writes only inside it");
    CHECK(region_contains(2048U, TEST_SECTORS, record_b),
          "the staged record is replayed into the FAT12 volume");

    /* 4. The loader read the medium through its own filesystem (isohybrid):
          the partition is found by the MBR table, not by the verdict. */
    medium_reset(8192U);
    mbr_sign();
    mbr_partition(0U, 0xEFU, 2048U, TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    install_journal_platform_init();
    reset_observations();
    install_journal_platform_observe(record_a, sizeof(record_a) - 1U);
    matched.partition_index = 0U;
    matched.first_lba = 0U;
    matched.length_lba = 8192U;
    CHECK(install_journal_platform_bind_medium(&device, &matched),
          "the FAT12 partition is found through the MBR table");
    CHECK(count_store_text("[INSTALL-LOG]") == 0U,
          "the table-found FAT12 placement names no refusal");
    CHECK(medium.first_write >= 2048U * BPS &&
              medium.last_write <= (2048U + TEST_SECTORS) * BPS,
          "the table-found partition is the one written");

    /* 5. No partition carries FAT12: refused by name before the first write -
          one ordinary record, nothing printed by the platform. */
    medium_reset(8192U);
    mbr_sign();
    mbr_partition(0U, 0x0CU, 2048U, TEST_SECTORS);
    device = medium_device();
    install_journal_platform_init();
    reset_observations();
    CHECK(!install_journal_platform_bind_medium(&device, &matched),
          "a medium without FAT12 is refused");
    CHECK(medium.writes == 0U,
          "a medium without FAT12 is refused and writes no sector");
    CHECK(count_serial_lines("[INSTALL-LOG] state=UNAVAILABLE reason=") == 1U,
          "the refusal is named once on serial");
    CHECK(count_store_text("[INSTALL-LOG] state=UNAVAILABLE reason=") == 1U,
          "the refusal is one ordinary durable record");

    /* 6. The call site's fallback does not overwrite that name: xhci.c calls
          `medium_unbound()` on the seam's false answer, and the core keeps the
          specific reason. */
    CHECK(!install_journal_platform_medium_unbound(),
          "the fallback naming still never fails the run");
    CHECK(count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT") == 0U,
          "the specific refusal stands and is not overwritten");
    CHECK(count_serial_lines("[INSTALL-LOG] ") == 1U,
          "the fallback adds no second status line");

    /* 7. The refusal page's flush and the panic path do not pretend and do not
          re-name what is already named. */
    CHECK(!install_journal_platform_flush(),
          "the flush of an unbound journal reports that nothing was written");
    CHECK(count_serial_lines("[INSTALL-LOG] ") == 1U,
          "the refusal page's flush adds no second name");
    CHECK(!install_journal_platform_panic_flush(),
          "the panic flush of an unbound journal reports the same");
    CHECK(count_serial_lines("[INSTALL-LOG] ") == 1U,
          "the panic path does not re-name an already named loss");
    CHECK(medium.writes == 0U,
          "no flush path reaches a device the seam never bound");

    /* 8. An optical boot names its own reason first, and a later MATCH on the
          same run keeps that reason - the CD proof's exactly-one assertion
          stays true whatever else the machine probes.  The later MATCH may
          still bind (the volume is there); the name does not. */
    medium_reset(2048U + TEST_SECTORS);
    medium_place_fat12(2048U);
    device = medium_device();
    matched.partition_index = 1U;
    matched.first_lba = 2048U;
    matched.length_lba = TEST_SECTORS;
    install_journal_platform_init();
    reset_observations();
    CHECK(!install_journal_platform_refuse("READ_ONLY"),
          "the optical refusal does not fail the run");
    CHECK(count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY") == 1U,
          "the optical refusal is named once");
    CHECK(install_journal_platform_bind_medium(&device, &matched),
          "a later MATCH binds the placement on the FAT12 partition");
    CHECK(count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY") == 1U &&
          count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=NO_PLACEMENT") == 0U,
          "the optical reason stands after a later MATCH");

    /* 9. A proven medium with no MBR signature is not a layout: refused by
          name, without a write.  An inconsistent FAT12 volume keeps its own
          reason rather than the earlier non-FAT partition's. */
    medium_reset(8192U);
    device = medium_device();
    matched.partition_index = 0U;
    matched.first_lba = 0U;
    matched.length_lba = 8192U;
    install_journal_platform_init();
    reset_observations();
    CHECK(!install_journal_platform_bind_medium(&device, &matched),
          "a medium without an MBR signature is refused");
    CHECK(medium.writes == 0U, "the signature refusal writes no sector");
    CHECK(count_store_lines(
              "[INSTALL-LOG] state=UNAVAILABLE reason=SIGNATURE") == 1U,
          "the signature refusal is named once");
    medium_reset(8192U);
    mbr_sign();
    mbr_partition(0U, 0x0CU, 1024U, 256U);
    mbr_partition(1U, 0xEFU, 2048U, TEST_SECTORS);
    medium_place_fat12(2048U);
    /* The second FAT copy describes the reserved file's chain differently:
       the placement refuses INCONSISTENT_VOLUME before the first write, and
       that specific reason must beat the earlier non-FAT partition's. */
    medium_fat_set_copy(2048U, 1U, FILE_FIRST + 1U, EOC);
    device = medium_device();
    install_journal_platform_init();
    reset_observations();
    CHECK(!install_journal_platform_bind_medium(&device, &matched),
          "an inconsistent FAT12 volume is refused");
    CHECK(count_store_text("reason=GEOMETRY") == 1U,
          "the inconsistent volume names its own reason once");
    CHECK(count_store_text("reason=NOT_FAT12") == 0U,
          "an earlier non-FAT partition does not shadow the FAT12 failure");
    CHECK(medium.writes == 0U, "the inconsistent volume is not written");

    /* 10. The funnel hook survives the degenerate inputs the console may hand
           it, in every state, without touching a device. */
    install_journal_platform_init();
    reset_observations();
    install_journal_platform_observe(NULL, 0U);
    install_journal_platform_observe("", 0U);
    (void)install_journal_platform_medium_unbound();
    install_journal_platform_observe(NULL, 7U);
    install_journal_platform_observe(record_a, 0U);
    CHECK(count_serial_lines("[INSTALL-LOG] ") == 1U,
          "degenerate funnel input does not multiply the named loss");

    free(medium.bytes);
    printf("install-journal-platform-host: %u checks, %u failures\n", checks,
           failures);
    return failures == 0U ? 0 : 1;
}
