#include "appliance_gpt_discovery.h"
#include "config_journal.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NVME_STATE_INTEGRATION_HOST_TEST

#define DISK_BLOCK_SIZE 512U
#define DISK_BLOCK_COUNT 512U
#define DISK_CAPACITY (DISK_BLOCK_SIZE * DISK_BLOCK_COUNT)
#define HFOC_FIRST_LBA 34U
#define HFOC_LAST_LBA 65U
#define HFOC_BLOCK_COUNT \
    (HFOC_LAST_LBA - HFOC_FIRST_LBA + 1U)
#define DATA_REGION_BLOCK_OFFSET 1U
#define JOURNAL_BLOCK_COUNT 16U

static unsigned char disk_storage[DISK_CAPACITY];
static size_t disk_write_calls;
static bool disk_fail_read;
static bool disk_fail_flush;
static size_t disk_flush_calls;

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const unsigned char hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
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
    return crc;
}

static uint32_t crc32(
    const unsigned char *bytes,
    size_t length)
{
    return crc32_update(0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static void disk_init(void)
{
    memset(disk_storage, 0, sizeof(disk_storage));
    disk_write_calls = 0U;
    disk_fail_read = false;
    disk_fail_flush = false;
    disk_flush_calls = 0U;
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (disk_fail_read ||
        offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++disk_write_calls;
    if (offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(
    struct block_device *device)
{
    (void)device;
    ++disk_flush_calls;
    if (disk_fail_flush) {
        disk_fail_flush = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = DISK_BLOCK_SIZE;
    device.block_count = DISK_BLOCK_COUNT;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static void set_gpt_header(void)
{
    unsigned char *header = disk_storage + DISK_BLOCK_SIZE;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char copy[DISK_BLOCK_SIZE];

    memset(disk_storage, 0, sizeof(disk_storage));
    memcpy(header, signature, sizeof(signature));
    store_le32(header + 8U, UINT32_C(0x00010000));
    store_le32(header + 12U, 92U);
    store_le64(header + 24U, 1U);
    store_le64(header + 32U, DISK_BLOCK_COUNT - 1U);
    store_le64(header + 40U, 34U);
    store_le64(header + 48U, DISK_BLOCK_COUNT - 1U);
    store_le64(header + 72U, 2U);
    store_le32(header + 80U, 4U);
    store_le32(header + 84U, 128U);
    store_le32(header + 88U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    store_le32(header + 88U, crc32(copy, 92U));
    store_le32(header + 16U, 0U);
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void set_entry(uint32_t index, uint64_t first, uint64_t last)
{
    unsigned char *entry =
        disk_storage + 2U * DISK_BLOCK_SIZE +
        (uint64_t)index * 128U;

    if (index == 1U) {
        memcpy(entry, hfoc_guid, 16U);
    }
    store_le64(entry + 16U, index + 1U);
    store_le64(entry + 32U, first);
    store_le64(entry + 40U, last);
}

static void finalize_entries(void)
{
    unsigned char *header = disk_storage + DISK_BLOCK_SIZE;
    unsigned char copy[DISK_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < 4U * 128U; ++index) {
        crc = crc32_update(
            crc,
            disk_storage + 2U * DISK_BLOCK_SIZE + index,
            1U);
    }
    store_le32(header + 88U, crc ^ 0xFFFFFFFFU);
    store_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void install_hfoc_data_region(void)
{
    unsigned char *header =
        disk_storage + HFOC_FIRST_LBA * DISK_BLOCK_SIZE;
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memcpy(header, magic, sizeof(magic));
    header[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(header + 16U, DATA_REGION_BLOCK_OFFSET);
    store_le64(header + 24U, JOURNAL_BLOCK_COUNT);
}

struct partition_range {
    struct block_device parent;
    uint64_t byte_offset;
    uint64_t byte_count;
};

static enum block_device_status range_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct partition_range *range =
        (struct partition_range *)device->context;

    if (offset > range->byte_count ||
        length > range->byte_count - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_read(
        &range->parent, range->byte_offset + offset,
        buffer, length);
}

static enum block_device_status range_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct partition_range *range =
        (struct partition_range *)device->context;

    if (offset > range->byte_count ||
        length > range->byte_count - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_write(
        &range->parent, range->byte_offset + offset,
        buffer, length);
}

static enum block_device_status range_flush(
    struct block_device *device)
{
    struct partition_range *range =
        (struct partition_range *)device->context;

    return block_device_flush(&range->parent);
}

static struct block_device make_partition_device(
    struct partition_range *range,
    const struct appliance_gpt_hfoc_partition *partition)
{
    struct block_device device;

    memset(range, 0, sizeof(*range));
    range->parent = disk_device();
    range->byte_offset = partition->first_lba * DISK_BLOCK_SIZE;
    range->byte_count = partition->block_count * DISK_BLOCK_SIZE;

    memset(&device, 0, sizeof(device));
    device.block_size = DISK_BLOCK_SIZE;
    device.block_count = partition->block_count;
    device.context = range;
    device.read_fn = range_read;
    device.write_fn = range_write;
    device.flush_fn = range_flush;
    return device;
}

static void test_discovery_header_and_journal_replay(void)
{
    struct block_device disk;
    struct appliance_gpt_hfoc_partition partition;
    struct partition_range range;
    struct block_device partition_device;
    struct config_journal journal;
    struct data_region region;
    unsigned char record[CONFIG_JOURNAL_DATA_MAX];
    size_t record_length = 0U;
    static const char first_record[] =
        "v1:system.hostname=hfoc-a\n";
    static const char second_record[] =
        "v1:system.hostname=hfoc-b\n";

    disk_init();
    set_gpt_header();
    set_entry(0U, 34U, 33U);
    set_entry(1U, HFOC_FIRST_LBA, HFOC_LAST_LBA);
    finalize_entries();
    install_hfoc_data_region();
    disk = disk_device();

    memset(&partition, 0xA5, sizeof(partition));
    CHECK(appliance_gpt_find_hfoc_partition(&disk, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK &&
              partition.first_lba == HFOC_FIRST_LBA &&
              partition.last_lba == HFOC_LAST_LBA &&
              partition.block_count == HFOC_BLOCK_COUNT,
          "GPT discovery finds exact HFOC persistence partition");

    partition_device = make_partition_device(&range, &partition);
    memset(&region, 0, sizeof(region));
    CHECK(data_region_discover(&partition_device, &region) ==
              DATA_REGION_OK &&
              region.block_offset == DATA_REGION_BLOCK_OFFSET &&
              region.block_count == JOURNAL_BLOCK_COUNT,
          "HFOC data region validates through bounded partition view");

    memset(&journal, 0, sizeof(journal));
    CHECK(config_journal_init(
              &journal, &partition_device,
              DATA_REGION_BLOCK_OFFSET,
              JOURNAL_BLOCK_COUNT) == CONFIG_JOURNAL_OK &&
              config_journal_head(
                  &journal, record, sizeof(record),
                  &record_length) == CONFIG_JOURNAL_EMPTY,
          "fresh journal initializes empty through partition view");
    CHECK(config_journal_append(
              &journal, first_record,
              sizeof(first_record) - 1U) == CONFIG_JOURNAL_OK &&
              config_journal_append(
                  &journal, second_record,
                  sizeof(second_record) - 1U) == CONFIG_JOURNAL_OK,
          "journal append writes two HFOC records through partition view");

    memset(&journal, 0, sizeof(journal));
    memset(record, 0, sizeof(record));
    CHECK(config_journal_init(
              &journal, &partition_device,
              DATA_REGION_BLOCK_OFFSET,
              JOURNAL_BLOCK_COUNT) == CONFIG_JOURNAL_OK &&
              config_journal_head(
                  &journal, record, sizeof(record),
                  &record_length) == CONFIG_JOURNAL_OK &&
              record_length == sizeof(second_record) - 1U &&
              memcmp(record, second_record,
                     sizeof(second_record) - 1U) == 0,
          "journal replay returns committed head through partition view");
    CHECK(block_device_flush(&partition_device) == BLOCK_DEVICE_OK &&
              disk_flush_calls == 1U,
          "partition view propagates flush to physical backend");
}

static void test_corruption_fail_closed(void)
{
    struct block_device disk;
    struct appliance_gpt_hfoc_partition partition;
    struct partition_range range;
    struct block_device partition_device;
    struct config_journal journal;
    struct data_region region;
    static const char record[] = "v1:system.hostname=hfoc-c\n";
    static const char second_record[] = "v1:system.hostname=hfoc-d\n";

    disk_init();
    set_gpt_header();
    set_entry(1U, HFOC_FIRST_LBA, HFOC_LAST_LBA);
    finalize_entries();
    install_hfoc_data_region();
    disk = disk_device();
    CHECK(appliance_gpt_find_hfoc_partition(&disk, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK,
          "corruption fixture finds HFOC partition");
    partition_device = make_partition_device(&range, &partition);
    CHECK(config_journal_init(
              &journal, &partition_device,
              DATA_REGION_BLOCK_OFFSET,
              JOURNAL_BLOCK_COUNT) == CONFIG_JOURNAL_OK &&
              config_journal_append(
                  &journal, record, sizeof(record) - 1U) ==
                  CONFIG_JOURNAL_OK &&
              config_journal_append(
                  &journal, second_record,
                  sizeof(second_record) - 1U) ==
                  CONFIG_JOURNAL_OK,
          "corruption fixture appends two committed records");
    disk_storage[
        (HFOC_FIRST_LBA + DATA_REGION_BLOCK_OFFSET) *
            DISK_BLOCK_SIZE] ^= 0x01U;
    memset(&journal, 0, sizeof(journal));
    CHECK(config_journal_init(
              &journal, &partition_device,
              DATA_REGION_BLOCK_OFFSET,
              JOURNAL_BLOCK_COUNT) != CONFIG_JOURNAL_OK,
          "corrupted journal record fails closed on replay");

    disk_init();
    set_gpt_header();
    set_entry(1U, HFOC_FIRST_LBA, HFOC_LAST_LBA);
    finalize_entries();
    install_hfoc_data_region();
    CHECK(appliance_gpt_find_hfoc_partition(&disk, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK,
          "corrupt header fixture finds HFOC partition");
    disk_storage[HFOC_FIRST_LBA * DISK_BLOCK_SIZE] = 'X';
    partition_device = make_partition_device(&range, &partition);
    memset(&region, 0, sizeof(region));
    CHECK(data_region_discover(&partition_device, &region) ==
              DATA_REGION_BAD_MAGIC,
          "corrupted HFOC header is rejected without publishing a region");
}

int main(void)
{
    test_discovery_header_and_journal_replay();
    test_corruption_fail_closed();

    if (failures != 0) {
        fprintf(stderr,
                "NVMe state integration host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("NVMe state integration host tests: ALL OK");
    return 0;
}

#endif
