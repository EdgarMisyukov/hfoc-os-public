#ifdef HFOC_PHYSICAL_STORAGE_QEMU_TEST

#include "appliance_gpt_discovery.h"
#include "block_device.h"
#include "config_journal.h"
#include "data_region.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PHYSICAL_STORAGE_QEMU_BLOCK_SIZE 512U
#define PHYSICAL_STORAGE_QEMU_BLOCK_COUNT 128U
#define PHYSICAL_STORAGE_QEMU_CAPACITY \
    (PHYSICAL_STORAGE_QEMU_BLOCK_SIZE * \
     PHYSICAL_STORAGE_QEMU_BLOCK_COUNT)
#define PHYSICAL_STORAGE_QEMU_GPT_ENTRY_COUNT 128U
#define PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE 92U
#define PHYSICAL_STORAGE_QEMU_GPT_ENTRY_SIZE 128U
#define PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA 34U
#define PHYSICAL_STORAGE_QEMU_HFOC_LAST_LBA 65U
#define PHYSICAL_STORAGE_QEMU_HFOC_BLOCK_COUNT \
    (PHYSICAL_STORAGE_QEMU_HFOC_LAST_LBA - \
     PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA + 1U)
#define PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET 1U
#define PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT 16U
#define PHYSICAL_STORAGE_QEMU_SCRATCH_LBA 6U

static unsigned char physical_storage_qemu_disk[
    PHYSICAL_STORAGE_QEMU_CAPACITY];
static bool physical_storage_qemu_fail_read;
static bool physical_storage_qemu_fail_flush;
static size_t physical_storage_qemu_read_calls;
static size_t physical_storage_qemu_flush_calls;

static const unsigned char physical_storage_qemu_hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static void physical_storage_qemu_failure(const char *message)
    __attribute__((noreturn));

static void physical_storage_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PHYSICAL-STORAGE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void physical_storage_qemu_require(bool condition,
                                          const char *message)
{
    if (!condition) {
        physical_storage_qemu_failure(message);
    }
}

static void physical_storage_qemu_store_le32(unsigned char *bytes,
                                             uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void physical_storage_qemu_store_le64(unsigned char *bytes,
                                             uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t physical_storage_qemu_crc32_update(
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

static uint32_t physical_storage_qemu_crc32(
    const unsigned char *bytes,
    size_t length)
{
    return physical_storage_qemu_crc32_update(
        0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static void physical_storage_qemu_zero_disk(void)
{
    memset(physical_storage_qemu_disk, 0,
           sizeof(physical_storage_qemu_disk));
    physical_storage_qemu_fail_read = false;
    physical_storage_qemu_fail_flush = false;
    physical_storage_qemu_read_calls = 0U;
    physical_storage_qemu_flush_calls = 0U;
}

static void physical_storage_qemu_set_gpt_header(void)
{
    unsigned char *header =
        physical_storage_qemu_disk + PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char copy[PHYSICAL_STORAGE_QEMU_BLOCK_SIZE];

    memcpy(header, signature, sizeof(signature));
    physical_storage_qemu_store_le32(
        header + 8U, UINT32_C(0x00010000));
    physical_storage_qemu_store_le32(
        header + 12U, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE);
    physical_storage_qemu_store_le64(header + 24U, 1U);
    physical_storage_qemu_store_le64(
        header + 32U, PHYSICAL_STORAGE_QEMU_BLOCK_COUNT - 1U);
    physical_storage_qemu_store_le64(
        header + 40U, PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA);
    physical_storage_qemu_store_le64(
        header + 48U, PHYSICAL_STORAGE_QEMU_BLOCK_COUNT - 1U);
    physical_storage_qemu_store_le64(header + 72U, 2U);
    physical_storage_qemu_store_le32(
        header + 80U, PHYSICAL_STORAGE_QEMU_GPT_ENTRY_COUNT);
    physical_storage_qemu_store_le32(
        header + 84U, PHYSICAL_STORAGE_QEMU_GPT_ENTRY_SIZE);
    physical_storage_qemu_store_le32(header + 88U, 0U);

    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE);
    physical_storage_qemu_store_le32(
        header + 88U,
        physical_storage_qemu_crc32(
            copy, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE));
    physical_storage_qemu_store_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE);
    memset(copy + 16U, 0, 4U);
    physical_storage_qemu_store_le32(
        header + 16U,
        physical_storage_qemu_crc32(
            copy, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE));
}

static void physical_storage_qemu_set_entry(uint32_t index,
                                            uint64_t first_lba,
                                            uint64_t last_lba)
{
    unsigned char *entry =
        physical_storage_qemu_disk +
        2U * PHYSICAL_STORAGE_QEMU_BLOCK_SIZE +
        (uint64_t)index * PHYSICAL_STORAGE_QEMU_GPT_ENTRY_SIZE;

    if (index == 1U) {
        memcpy(entry, physical_storage_qemu_hfoc_guid,
               sizeof(physical_storage_qemu_hfoc_guid));
    }
    physical_storage_qemu_store_le64(entry + 16U, index + 1U);
    physical_storage_qemu_store_le64(entry + 32U, first_lba);
    physical_storage_qemu_store_le64(entry + 40U, last_lba);
}

static void physical_storage_qemu_finalize_gpt_entries(void)
{
    unsigned char *header =
        physical_storage_qemu_disk + PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    unsigned char copy[PHYSICAL_STORAGE_QEMU_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t entry_bytes =
        PHYSICAL_STORAGE_QEMU_GPT_ENTRY_COUNT *
        PHYSICAL_STORAGE_QEMU_GPT_ENTRY_SIZE;
    size_t index;

    for (index = 0U; index < entry_bytes; ++index) {
        crc = physical_storage_qemu_crc32_update(
            crc,
            physical_storage_qemu_disk +
                2U * PHYSICAL_STORAGE_QEMU_BLOCK_SIZE + index,
            1U);
    }
    physical_storage_qemu_store_le32(
        header + 88U, crc ^ 0xFFFFFFFFU);

    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE);
    memset(copy + 16U, 0, 4U);
    physical_storage_qemu_store_le32(
        header + 16U,
        physical_storage_qemu_crc32(
            copy, PHYSICAL_STORAGE_QEMU_GPT_HEADER_SIZE));
}

static void physical_storage_qemu_install_hfoc_data_region(void)
{
    unsigned char *header =
        physical_storage_qemu_disk +
        PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA *
            PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memcpy(header, magic, sizeof(magic));
    header[8U] = DATA_REGION_FORMAT_VERSION;
    physical_storage_qemu_store_le64(
        header + 16U, PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET);
    physical_storage_qemu_store_le64(
        header + 24U, PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT);
}

static void physical_storage_qemu_install_valid_disk(void)
{
    physical_storage_qemu_zero_disk();
    physical_storage_qemu_set_gpt_header();
    physical_storage_qemu_set_entry(1U,
                                    PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA,
                                    PHYSICAL_STORAGE_QEMU_HFOC_LAST_LBA);
    physical_storage_qemu_finalize_gpt_entries();
    physical_storage_qemu_install_hfoc_data_region();
}

static enum block_device_status physical_storage_qemu_disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    ++physical_storage_qemu_read_calls;
    if (physical_storage_qemu_fail_read) {
        physical_storage_qemu_fail_read = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > sizeof(physical_storage_qemu_disk) ||
        length > sizeof(physical_storage_qemu_disk) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, physical_storage_qemu_disk + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status physical_storage_qemu_disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(physical_storage_qemu_disk) ||
        length > sizeof(physical_storage_qemu_disk) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(physical_storage_qemu_disk + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status physical_storage_qemu_disk_flush(
    struct block_device *device)
{
    (void)device;
    ++physical_storage_qemu_flush_calls;
    if (physical_storage_qemu_fail_flush) {
        physical_storage_qemu_fail_flush = false;
        return BLOCK_DEVICE_IO_ERROR;
    }
    return BLOCK_DEVICE_OK;
}

static struct block_device physical_storage_qemu_make_disk(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    device.block_count = PHYSICAL_STORAGE_QEMU_BLOCK_COUNT;
    device.read_fn = physical_storage_qemu_disk_read;
    device.write_fn = physical_storage_qemu_disk_write;
    device.flush_fn = physical_storage_qemu_disk_flush;
    return device;
}

struct physical_storage_qemu_partition_range {
    struct block_device parent;
    uint64_t byte_offset;
    uint64_t byte_count;
};

static enum block_device_status physical_storage_qemu_range_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct physical_storage_qemu_partition_range *range =
        (struct physical_storage_qemu_partition_range *)device->context;

    if (offset > range->byte_count ||
        (uint64_t)length > range->byte_count - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_read(
        &range->parent, range->byte_offset + offset, buffer, length);
}

static enum block_device_status physical_storage_qemu_range_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct physical_storage_qemu_partition_range *range =
        (struct physical_storage_qemu_partition_range *)device->context;

    if (offset > range->byte_count ||
        (uint64_t)length > range->byte_count - offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    return block_device_write(
        &range->parent, range->byte_offset + offset, buffer, length);
}

static enum block_device_status physical_storage_qemu_range_flush(
    struct block_device *device)
{
    struct physical_storage_qemu_partition_range *range =
        (struct physical_storage_qemu_partition_range *)device->context;

    return block_device_flush(&range->parent);
}

static struct block_device physical_storage_qemu_make_partition_device(
    struct physical_storage_qemu_partition_range *range,
    const struct appliance_gpt_hfoc_partition *partition)
{
    struct block_device device;

    memset(range, 0, sizeof(*range));
    range->parent = physical_storage_qemu_make_disk();
    range->byte_offset =
        partition->first_lba * PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    range->byte_count =
        partition->block_count * PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;

    memset(&device, 0, sizeof(device));
    device.block_size = PHYSICAL_STORAGE_QEMU_BLOCK_SIZE;
    device.block_count = partition->block_count;
    device.context = range;
    device.read_fn = physical_storage_qemu_range_read;
    device.write_fn = physical_storage_qemu_range_write;
    device.flush_fn = physical_storage_qemu_range_flush;
    return device;
}

static void physical_storage_qemu_test_contract(void)
{
    struct block_device disk = physical_storage_qemu_make_disk();
    struct block_device malformed;
    struct appliance_gpt_hfoc_partition partition;
    struct physical_storage_qemu_partition_range range;
    struct block_device partition_device;
    struct data_region region;
    struct config_journal journal;
    enum block_device_status io_status;
    enum config_journal_status journal_status;
    enum appliance_gpt_discovery_status gpt_status;
    unsigned char pattern[PHYSICAL_STORAGE_QEMU_BLOCK_SIZE];
    unsigned char readback[PHYSICAL_STORAGE_QEMU_BLOCK_SIZE];
    unsigned char record[CONFIG_JOURNAL_DATA_MAX];
    size_t record_length = 0U;
    static const char first_record[] = "v1:system.hostname=hfoc-a\n";
    static const char second_record[] = "v1:system.hostname=hfoc-b\n";

    physical_storage_qemu_install_valid_disk();
    serial_write("[PHYSICAL-STORAGE] proof init\n");

    memset(pattern, 0x55, sizeof(pattern));
    io_status = block_device_write(
        &disk,
        PHYSICAL_STORAGE_QEMU_SCRATCH_LBA *
            PHYSICAL_STORAGE_QEMU_BLOCK_SIZE,
        pattern, sizeof(pattern));
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_OK &&
            physical_storage_qemu_flush_calls == 0U,
        "deterministic backend write");
    io_status = block_device_flush(&disk);
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_OK &&
            physical_storage_qemu_flush_calls == 1U,
        "deterministic backend flush");

    memset(readback, 0, sizeof(readback));
    io_status = block_device_read(
        &disk,
        PHYSICAL_STORAGE_QEMU_SCRATCH_LBA *
            PHYSICAL_STORAGE_QEMU_BLOCK_SIZE,
        readback, sizeof(readback));
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_OK &&
            memcmp(readback, pattern, sizeof(pattern)) == 0U &&
            physical_storage_qemu_read_calls == 1U,
        "deterministic backend read");

    physical_storage_qemu_fail_read = true;
    io_status = block_device_read(
        &disk,
        PHYSICAL_STORAGE_QEMU_SCRATCH_LBA *
            PHYSICAL_STORAGE_QEMU_BLOCK_SIZE,
        readback, sizeof(readback));
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_IO_ERROR &&
            !physical_storage_qemu_fail_read &&
            physical_storage_qemu_read_calls == 2U,
        "injected deterministic read failure");

    physical_storage_qemu_fail_flush = true;
    io_status = block_device_flush(&disk);
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_IO_ERROR &&
            !physical_storage_qemu_fail_flush &&
            physical_storage_qemu_flush_calls == 2U,
        "injected deterministic flush failure");
    serial_write("[PHYSICAL-STORAGE] backend read write flush OK\n");

    memset(readback, 0xA5, sizeof(readback));
    io_status = block_device_read(
        &disk, sizeof(physical_storage_qemu_disk),
        readback, sizeof(readback));
    physical_storage_qemu_require(
        io_status == BLOCK_DEVICE_OUT_OF_RANGE &&
            physical_storage_qemu_read_calls == 2U,
        "out of range rejected before backend");

    malformed = physical_storage_qemu_make_disk();
    malformed.block_size = 511U;
    physical_storage_qemu_require(
        block_device_validate(&malformed) ==
            BLOCK_DEVICE_BAD_BLOCK_SIZE &&
            appliance_gpt_find_hfoc_partition(
                &malformed, &partition) ==
                APPLIANCE_GPT_DISCOVERY_BAD_DEVICE,
        "malformed device rejected");
    serial_write("[PHYSICAL-STORAGE] bounds and malformed rejected\n");

    physical_storage_qemu_install_valid_disk();
    disk = physical_storage_qemu_make_disk();
    memset(&partition, 0xA5, sizeof(partition));
    gpt_status = appliance_gpt_find_hfoc_partition(&disk, &partition);
    physical_storage_qemu_require(
        gpt_status == APPLIANCE_GPT_DISCOVERY_OK &&
            partition.present &&
            partition.first_lba ==
                PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA &&
            partition.last_lba ==
                PHYSICAL_STORAGE_QEMU_HFOC_LAST_LBA &&
            partition.block_count ==
                PHYSICAL_STORAGE_QEMU_HFOC_BLOCK_COUNT,
        "HFOC GPT partition discovery");
    serial_write("[PHYSICAL-STORAGE] HFOC partition discovered\n");

    physical_storage_qemu_disk[PHYSICAL_STORAGE_QEMU_BLOCK_SIZE] ^= 0x01U;
    gpt_status = appliance_gpt_find_hfoc_partition(&disk, &partition);
    physical_storage_qemu_require(
        gpt_status == APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE,
        "corrupt GPT signature rejected");
    serial_write("[PHYSICAL-STORAGE] corrupt GPT rejected\n");

    physical_storage_qemu_install_valid_disk();
    disk = physical_storage_qemu_make_disk();
    memset(&partition, 0xA5, sizeof(partition));
    physical_storage_qemu_require(
        appliance_gpt_find_hfoc_partition(&disk, &partition) ==
            APPLIANCE_GPT_DISCOVERY_OK,
        "journal fixture GPT discovery");
    partition_device =
        physical_storage_qemu_make_partition_device(&range, &partition);
    memset(&region, 0, sizeof(region));
    physical_storage_qemu_require(
        data_region_discover(&partition_device, &region) ==
            DATA_REGION_OK &&
            region.block_offset ==
                PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET &&
            region.block_count ==
                PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT,
        "data region over bounded partition");

    memset(&journal, 0, sizeof(journal));
    journal_status = config_journal_init(
        &journal, &partition_device,
        PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET,
        PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT);
    physical_storage_qemu_require(
        journal_status == CONFIG_JOURNAL_OK &&
            journal.next_block == 0U &&
            config_journal_head(
                &journal, record, sizeof(record),
                &record_length) == CONFIG_JOURNAL_EMPTY,
        "empty journal over partition");

    journal_status = config_journal_append(
        &journal, first_record, sizeof(first_record) - 1U);
    physical_storage_qemu_require(
        journal_status == CONFIG_JOURNAL_OK &&
            journal.next_block == 1U,
        "first journal record");
    journal_status = config_journal_append(
        &journal, second_record, sizeof(second_record) - 1U);
    physical_storage_qemu_require(
        journal_status == CONFIG_JOURNAL_OK &&
            journal.next_block == 2U,
        "second journal record");
    physical_storage_qemu_require(
        block_device_flush(&partition_device) == BLOCK_DEVICE_OK,
        "partition flush propagated");

    memset(&journal, 0, sizeof(journal));
    memset(record, 0, sizeof(record));
    record_length = 0U;
    physical_storage_qemu_require(
        config_journal_init(
            &journal, &partition_device,
            PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET,
            PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT) ==
            CONFIG_JOURNAL_OK &&
            config_journal_head(
                &journal, record, sizeof(record),
                &record_length) == CONFIG_JOURNAL_OK &&
            record_length == sizeof(second_record) - 1U &&
            memcmp(record, second_record,
                   sizeof(second_record) - 1U) == 0U,
        "journal replay through partition");
    serial_write("[PHYSICAL-STORAGE] journal replay OK\n");

    physical_storage_qemu_disk[
        (PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA +
         PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET) *
        PHYSICAL_STORAGE_QEMU_BLOCK_SIZE] ^= 0x01U;
    memset(&journal, 0, sizeof(journal));
    physical_storage_qemu_require(
        config_journal_init(
            &journal, &partition_device,
            PHYSICAL_STORAGE_QEMU_DATA_REGION_OFFSET,
            PHYSICAL_STORAGE_QEMU_JOURNAL_BLOCK_COUNT) !=
            CONFIG_JOURNAL_OK,
        "corrupt journal record fails closed");
    serial_write("[PHYSICAL-STORAGE] corrupt journal fails closed\n");

    physical_storage_qemu_disk[
        PHYSICAL_STORAGE_QEMU_HFOC_FIRST_LBA *
        PHYSICAL_STORAGE_QEMU_BLOCK_SIZE] = (unsigned char)'X';
    memset(&region, 0, sizeof(region));
    physical_storage_qemu_require(
        data_region_discover(&partition_device, &region) ==
            DATA_REGION_BAD_MAGIC,
        "corrupt HFOC header fails closed");
    serial_write("[PHYSICAL-STORAGE] corrupt HFOC header fails closed\n");

    serial_write("[PHYSICAL-STORAGE] clean shutdown\n");
    serial_write("[PHYSICAL-STORAGE] proof OK\n");
}

void physical_storage_qemu_test_init(void)
{
    physical_storage_qemu_test_contract();
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_physical_storage_qemu_translation_unit_marker;
#endif
