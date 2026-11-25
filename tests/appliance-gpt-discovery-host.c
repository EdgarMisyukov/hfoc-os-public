#include "appliance_gpt_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_GPT_DISCOVERY_HOST_TEST

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 16384U
#define FIXTURE_CAPACITY \
    (FIXTURE_BLOCK_SIZE * FIXTURE_BLOCK_COUNT)

static unsigned char fixture_storage[FIXTURE_CAPACITY];
static size_t fixture_write_calls;
static bool fixture_fail_read;

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const unsigned char fixture_hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static const unsigned char fixture_efi_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static void fixture_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void fixture_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t fixture_crc32(
    const unsigned char *bytes,
    size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
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
    return crc ^ 0xFFFFFFFFU;
}

static void fixture_init(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_write_calls = 0U;
    fixture_fail_read = false;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (fixture_fail_read ||
        offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    (void)offset;
    (void)buffer;
    (void)length;
    ++fixture_write_calls;
    return BLOCK_DEVICE_IO_ERROR;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static void fixture_set_gpt_header(uint32_t entry_count)
{
    unsigned char *header = fixture_storage + FIXTURE_BLOCK_SIZE;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char copy[FIXTURE_BLOCK_SIZE];

    memset(fixture_storage, 0, sizeof(fixture_storage));
    memcpy(header, signature, sizeof(signature));
    fixture_store_le32(header + 8U, UINT32_C(0x00010000));
    fixture_store_le32(header + 12U, 92U);
    fixture_store_le64(header + 24U, 1U);
    fixture_store_le64(header + 32U, FIXTURE_BLOCK_COUNT - 1U);
    fixture_store_le64(header + 40U, 34U);
    fixture_store_le64(header + 48U, FIXTURE_BLOCK_COUNT - 1U);
    fixture_store_le64(header + 72U, 2U);
    fixture_store_le32(header + 80U, entry_count);
    fixture_store_le32(header + 84U, 128U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 88U, 0, 4U);
    fixture_store_le32(header + 88U, 0U);
    fixture_store_le32(
        header + 88U, fixture_crc32(copy, 92U));
    fixture_store_le32(header + 16U, 0U);
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    fixture_store_le32(
        header + 16U, fixture_crc32(copy, 92U));
}

static void fixture_set_entry(
    uint32_t index,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba)
{
    unsigned char *entry =
        fixture_storage + 2U * FIXTURE_BLOCK_SIZE +
        (uint64_t)index * 128U;

    if (type_guid != NULL) {
        memcpy(entry, type_guid, 16U);
    }
    fixture_store_le64(entry + 16U, index + 1U);
    fixture_store_le64(entry + 32U, first_lba);
    fixture_store_le64(entry + 40U, last_lba);
}

static void fixture_finalize_entries(uint32_t entry_count)
{
    unsigned char *header = fixture_storage + FIXTURE_BLOCK_SIZE;
    unsigned char copy[FIXTURE_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t entry_index;
    const size_t bytes = (size_t)entry_count * 128U;

    for (entry_index = 0U; entry_index < bytes; ++entry_index) {
        unsigned int bit;
        uint32_t value = fixture_storage[
            2U * FIXTURE_BLOCK_SIZE + entry_index];
        unsigned char byte = (unsigned char)value;

        crc ^= byte;
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    fixture_store_le32(header + 88U, crc ^ 0xFFFFFFFFU);
    fixture_store_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    fixture_store_le32(
        header + 16U, fixture_crc32(copy, 92U));
}

static void fixture_set_hfoc_fixture(void)
{
    fixture_init();
    fixture_set_gpt_header(4U);
    fixture_set_entry(0U, fixture_efi_guid, 2048U, 4095U);
    fixture_set_entry(1U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_set_entry(2U, fixture_efi_guid, 8192U, 10000U);
    fixture_finalize_entries(4U);
}

static bool fixture_result_valid(
    const struct appliance_gpt_hfoc_partition *partition)
{
    return partition->present &&
           partition->partition_index == 2U &&
           partition->first_lba == 4096U &&
           partition->last_lba == 8191U &&
           partition->block_count == 4096U;
}

static void test_status_and_validation(void)
{
    struct block_device valid_device;
    struct appliance_gpt_hfoc_partition partition;

    CHECK(appliance_gpt_find_hfoc_partition(NULL, &partition) ==
              APPLIANCE_GPT_DISCOVERY_NULL,
          "find rejects null device");
    CHECK(appliance_gpt_find_hfoc_partition(
              &(struct block_device){0}, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_DEVICE,
          "find rejects invalid device");
    fixture_init();
    valid_device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&valid_device, NULL) ==
              APPLIANCE_GPT_DISCOVERY_NULL,
          "find rejects null partition output");
    memset(&partition, 0, sizeof(partition));
    CHECK(appliance_gpt_validate_hfoc_partition(&partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_ENTRY,
          "validate rejects absent partition");
    partition.present = true;
    partition.partition_index = 1U;
    partition.first_lba = 34U;
    partition.last_lba = 33U;
    partition.block_count = 0U;
    CHECK(appliance_gpt_validate_hfoc_partition(&partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_ENTRY,
          "validate rejects zero block count");
    partition.first_lba = 34U;
    partition.last_lba = 100U;
    partition.block_count = 67U;
    CHECK(appliance_gpt_validate_hfoc_partition(&partition) ==
              APPLIANCE_GPT_DISCOVERY_OK,
          "validate accepts exact contiguous HFOC range");
    CHECK(strcmp(appliance_gpt_discovery_status_string(
                     APPLIANCE_GPT_DISCOVERY_OK),
                 "APPLIANCE_GPT_DISCOVERY_OK") == 0,
          "GPT discovery OK status string is exact");
    CHECK(strcmp(appliance_gpt_discovery_status_string(
                     APPLIANCE_GPT_DISCOVERY_AMBIGUOUS),
                 "APPLIANCE_GPT_DISCOVERY_AMBIGUOUS") == 0,
          "GPT discovery ambiguous status string is exact");
}

static void test_valid_and_magic_isolation(void)
{
    struct block_device device;
    struct appliance_gpt_hfoc_partition partition;

    fixture_set_hfoc_fixture();
    memcpy(fixture_storage + 100U * FIXTURE_BLOCK_SIZE,
           "HFOCDATA", 8U);
    device = fixture_device();
    memset(&partition, 0xA5, sizeof(partition));
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK &&
              fixture_result_valid(&partition),
          "valid GPT with one HFOC persistence GUID is discovered exactly");
    CHECK(fixture_write_calls == 0U,
          "GPT discovery performs no writes");
    CHECK(appliance_gpt_validate_hfoc_partition(&partition) ==
              APPLIANCE_GPT_DISCOVERY_OK,
          "discovered HFOC partition validates");

    fixture_set_gpt_header(4U);
    fixture_set_entry(1U, fixture_efi_guid, 4096U, 8191U);
    memcpy(fixture_storage + 100U * FIXTURE_BLOCK_SIZE,
           "HFOCDATA", 8U);
    fixture_finalize_entries(4U);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_NOT_FOUND,
          "arbitrary HFOC magic outside an HFOC partition is not identity");
}

static void test_malformed_headers_and_entries(void)
{
    struct block_device device;
    struct appliance_gpt_hfoc_partition partition;

    fixture_set_hfoc_fixture();
    fixture_storage[FIXTURE_BLOCK_SIZE] = 'X';
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE,
          "invalid GPT signature is rejected");

    fixture_set_hfoc_fixture();
    fixture_storage[FIXTURE_BLOCK_SIZE + 16U] ^= 0x01U;
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_HEADER,
          "invalid GPT header CRC is rejected");

    fixture_set_hfoc_fixture();
    fixture_storage[
        2U * FIXTURE_BLOCK_SIZE + 33U] ^= 0x01U;
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_ENTRY,
          "invalid GPT entry CRC is rejected");

    fixture_set_gpt_header(4U);
    fixture_set_entry(0U, fixture_efi_guid, 2048U, 4095U);
    fixture_set_entry(1U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_set_entry(2U, fixture_hfoc_guid, 8192U, 10000U);
    fixture_finalize_entries(4U);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_AMBIGUOUS,
          "multiple HFOC partitions fail closed as ambiguous");

    fixture_set_gpt_header(4U);
    fixture_set_entry(0U, fixture_efi_guid, 2048U, 4095U);
    fixture_set_entry(1U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_set_entry(2U, fixture_efi_guid, 8190U, 10000U);
    fixture_finalize_entries(4U);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OVERLAP,
          "overlapping GPT partitions are rejected");

    fixture_set_gpt_header(4U);
    fixture_set_entry(0U, fixture_hfoc_guid, 4096U,
                      FIXTURE_BLOCK_COUNT + 100U);
    fixture_finalize_entries(4U);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_ENTRY,
          "partition extending beyond disk is rejected");

    fixture_set_hfoc_fixture();
    fixture_fail_read = true;
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_IO_ERROR,
          "injected read failure maps to stable I/O error");

    fixture_set_hfoc_fixture();
    device = fixture_device();
    device.block_size = 1024U;
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE,
          "unsupported block size is rejected before discovery");
}

static void test_capacity_bounds(void)
{
    struct block_device device;
    struct appliance_gpt_hfoc_partition partition;
    uint32_t index;
    uint64_t cursor = 2048U;

    /*
     * A table written by any other tool carries the GPT's full 128 slots, and
     * the preserving install keeps the machine's own table, so the reader has to
     * read it: the entry count is the slot count, not the number of partitions.
     */
    fixture_init();
    fixture_set_gpt_header(APPLIANCE_GPT_ENTRY_COUNT_MAX);
    fixture_set_entry(0U, fixture_efi_guid, 2048U, 4095U);
    fixture_set_entry(1U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_finalize_entries(APPLIANCE_GPT_ENTRY_COUNT_MAX);
    device = fixture_device();
    memset(&partition, 0, sizeof(partition));
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK &&
              fixture_result_valid(&partition),
          "the GPT's full 128-slot array is read");

    /*
     * The bound that protects the ranges array is the number of *present*
     * partitions, and it is still enforced: one more partition than the reader
     * can hold is refused, not truncated.
     */
    fixture_init();
    fixture_set_gpt_header(APPLIANCE_GPT_ENTRY_COUNT_MAX);
    for (index = 0U; index < APPLIANCE_GPT_PARTITION_CAPACITY + 1U;
         ++index) {
        fixture_set_entry(
            index, fixture_efi_guid, cursor, cursor);
        ++cursor;
    }
    fixture_finalize_entries(APPLIANCE_GPT_ENTRY_COUNT_MAX);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_CAPACITY,
          "more present partitions than the reader holds is refused");

    fixture_init();
    fixture_set_gpt_header(APPLIANCE_GPT_ENTRY_COUNT_MAX + 1U);
    fixture_set_entry(0U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_finalize_entries(APPLIANCE_GPT_ENTRY_COUNT_MAX + 1U);
    device = fixture_device();
    CHECK(appliance_gpt_find_hfoc_partition(&device, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_HEADER,
          "more slots than the GPT maximum is rejected");
}

/*
 * ITEM-410: the appliance boot log has to find the ESP that belongs to the
 * install it is running from, and a machine can carry more than one.  The walk
 * is read-only and reports the extents in GPT slot order; the caller then asks
 * each extent what it holds.
 */
static void test_esp_partition_walk(void)
{
    struct block_device device;
    struct appliance_gpt_hfoc_partition partition;

    fixture_set_hfoc_fixture();
    device = fixture_device();
    memset(&partition, 0, sizeof(partition));
    CHECK(appliance_gpt_find_esp_partition(&device, 0U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK &&
              partition.present && partition.partition_index == 1U &&
              partition.first_lba == 2048U &&
              partition.last_lba == 4095U &&
              partition.block_count == 2048U,
          "the first EFI System Partition is reported in GPT slot order");
    CHECK(appliance_gpt_find_esp_partition(&device, 1U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_OK &&
              partition.partition_index == 3U &&
              partition.first_lba == 8192U &&
              partition.last_lba == 10000U,
          "the second EFI System Partition is reported too");
    CHECK(appliance_gpt_find_esp_partition(&device, 2U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_NOT_FOUND,
          "a table with two ESPs has no third one");
    CHECK(fixture_write_calls == 0U,
          "walking the ESPs performs no writes");

    fixture_init();
    fixture_set_gpt_header(2U);
    fixture_set_entry(0U, fixture_hfoc_guid, 4096U, 8191U);
    fixture_finalize_entries(2U);
    device = fixture_device();
    CHECK(appliance_gpt_find_esp_partition(&device, 0U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_NOT_FOUND,
          "a table without an EFI System Partition reports none");

    fixture_set_hfoc_fixture();
    fixture_storage[FIXTURE_BLOCK_SIZE] = 'X';
    device = fixture_device();
    CHECK(appliance_gpt_find_esp_partition(&device, 0U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE,
          "the ESP walk refuses the same tables the persistence walk refuses");
    fixture_set_hfoc_fixture();
    fixture_storage[2U * FIXTURE_BLOCK_SIZE + 33U] ^= 0x01U;
    device = fixture_device();
    CHECK(appliance_gpt_find_esp_partition(&device, 0U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_BAD_ENTRY,
          "an ESP walk over a table with a broken entry array is refused");
    CHECK(appliance_gpt_find_esp_partition(NULL, 0U, &partition) ==
              APPLIANCE_GPT_DISCOVERY_NULL,
          "the ESP walk refuses a null device");
    CHECK(appliance_gpt_find_esp_partition(&device, 0U, NULL) ==
              APPLIANCE_GPT_DISCOVERY_NULL,
          "the ESP walk refuses a null result");
}

int main(void)
{
    test_status_and_validation();
    test_valid_and_magic_isolation();
    test_malformed_headers_and_entries();
    test_capacity_bounds();
    test_esp_partition_walk();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance GPT discovery host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance GPT discovery host tests: ALL OK");
    return 0;
}

#endif
