#include "hfoc_efi_installer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EFI_INSTALLER_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 16384U
#define TEST_CAPACITY (TEST_BLOCK_SIZE * TEST_BLOCK_COUNT)
#define TEST_DISK_COUNT 3U
#define TEST_ESP_FIRST_LBA 2048U
#define TEST_ESP_BLOCK_COUNT 2048U
#define TEST_HFOC_FIRST_LBA 8192U
#define TEST_HFOC_BLOCK_COUNT 2048U

struct disk_fixture {
    unsigned char storage[TEST_CAPACITY];
    size_t write_calls;
};

static struct disk_fixture disks[TEST_DISK_COUNT];
static int failures;

static const unsigned char efi_system_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static const unsigned char hfoc_persistence_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

#define CHECK(cond, message) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", message); \
    } \
} while (0)

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
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
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U
                ? (crc >> 1U) ^ 0xEDB88320U
                : crc >> 1U;
        }
    }
    return crc;
}

static uint32_t crc32(const unsigned char *bytes, size_t length)
{
    return crc32_update(0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static void finalize_gpt(struct disk_fixture *disk)
{
    unsigned char *header = disk->storage + TEST_BLOCK_SIZE;
    unsigned char copy[TEST_BLOCK_SIZE];
    uint32_t entries_crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < 4U * 128U; ++index) {
        entries_crc = crc32_update(
            entries_crc,
            disk->storage + 2U * TEST_BLOCK_SIZE + index,
            1U);
    }
    store_le32(header + 88U, entries_crc ^ 0xFFFFFFFFU);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void set_partition(
    struct disk_fixture *disk,
    size_t entry_index,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba)
{
    unsigned char *entry = disk->storage +
        2U * TEST_BLOCK_SIZE + entry_index * 128U;

    memcpy(entry, type_guid, 16U);
    store_le64(entry + 16U, (uint64_t)entry_index + 1U);
    store_le64(entry + 32U, first_lba);
    store_le64(entry + 40U, last_lba);
}

static void disk_init(struct disk_fixture *disk)
{
    unsigned char *mbr = disk->storage;
    unsigned char *header = disk->storage + TEST_BLOCK_SIZE;

    memset(disk->storage, 0, sizeof(disk->storage));
    disk->write_calls = 0U;
    mbr[446U + 4U] = 0xEEU;
    store_le32(mbr + 446U + 8U, 1U);
    store_le32(mbr + 446U + 12U, TEST_BLOCK_COUNT - 1U);
    mbr[510U] = 0x55U;
    mbr[511U] = 0xAAU;

    memcpy(header, "EFI PART", 8U);
    store_le32(header + 8U, UINT32_C(0x00010000));
    store_le32(header + 12U, 92U);
    store_le64(header + 24U, 1U);
    store_le64(header + 32U, TEST_BLOCK_COUNT - 1U);
    store_le64(header + 40U, 34U);
    store_le64(header + 48U, TEST_BLOCK_COUNT - 34U);
    header[56U] = 1U;
    store_le64(header + 72U, 2U);
    store_le32(header + 80U, 4U);
    store_le32(header + 84U, 128U);
}

static void disk_with_esp_and_hfoc(struct disk_fixture *disk)
{
    disk_init(disk);
    set_partition(
        disk, 0U, efi_system_guid,
        TEST_ESP_FIRST_LBA,
        TEST_ESP_FIRST_LBA + TEST_ESP_BLOCK_COUNT - 1U);
    set_partition(
        disk, 1U, hfoc_persistence_guid,
        TEST_HFOC_FIRST_LBA,
        TEST_HFOC_FIRST_LBA + TEST_HFOC_BLOCK_COUNT - 1U);
    finalize_gpt(disk);
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct disk_fixture *disk =
        (struct disk_fixture *)device->context;

    if (offset > sizeof(disk->storage) ||
        length > sizeof(disk->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct disk_fixture *disk =
        (struct disk_fixture *)device->context;

    ++disk->write_calls;
    if (offset > sizeof(disk->storage) ||
        length > sizeof(disk->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(struct disk_fixture *disk)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.context = disk;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static void test_no_target_and_order_independence(void)
{
    struct block_device devices[TEST_DISK_COUNT];
    struct hfoc_efi_installer_target target;
    size_t index;

    disk_init(&disks[0]);
    set_partition(
        &disks[0], 0U, efi_system_guid,
        TEST_ESP_FIRST_LBA,
        TEST_ESP_FIRST_LBA + TEST_ESP_BLOCK_COUNT - 1U);
    finalize_gpt(&disks[0]);
    devices[0] = disk_device(&disks[0]);
    memset(&target, 0, sizeof(target));
    CHECK(hfoc_efi_installer_select_target(
              devices, 1U, &target) ==
              HFOC_EFI_INSTALLER_NO_TARGET &&
              disks[0].write_calls == 0U,
          "no HFOC persistence partition degrades without writes");

    disk_with_esp_and_hfoc(&disks[0]);
    disk_init(&disks[1]);
    set_partition(
        &disks[1], 0U, efi_system_guid,
        TEST_ESP_FIRST_LBA,
        TEST_ESP_FIRST_LBA + TEST_ESP_BLOCK_COUNT - 1U);
    finalize_gpt(&disks[1]);
    devices[0] = disk_device(&disks[1]);
    devices[1] = disk_device(&disks[0]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 2U, &target) ==
              HFOC_EFI_INSTALLER_OK &&
              target.device->context == &disks[0] &&
              target.esp_first_lba == TEST_ESP_FIRST_LBA &&
              target.esp_block_count == TEST_ESP_BLOCK_COUNT,
          "single HFOC target is selected");

    for (index = 0U; index < TEST_DISK_COUNT; ++index) {
        devices[index] = disk_device(&disks[index]);
    }
    devices[0] = disk_device(&disks[0]);
    devices[1] = disk_device(&disks[1]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 2U, &target) ==
              HFOC_EFI_INSTALLER_OK &&
              target.device->context == &disks[0],
          "device enumeration order does not select the winner");
    devices[0] = disk_device(&disks[1]);
    devices[1] = disk_device(&disks[0]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 2U, &target) ==
              HFOC_EFI_INSTALLER_OK &&
              target.device->context == &disks[0],
          "reversed enumeration selects the same target");
}

static void test_ambiguity_and_esp_failures(void)
{
    struct block_device devices[TEST_DISK_COUNT];
    struct hfoc_efi_installer_target target;

    disk_with_esp_and_hfoc(&disks[0]);
    disk_with_esp_and_hfoc(&disks[1]);
    devices[0] = disk_device(&disks[0]);
    devices[1] = disk_device(&disks[1]);
    memset(&target, 0, sizeof(target));
    CHECK(hfoc_efi_installer_select_target(
              devices, 2U, &target) ==
              HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET &&
              target.device == NULL &&
              disks[0].write_calls == 0U &&
              disks[1].write_calls == 0U,
          "multiple HFOC disks fail closed");

    disk_init(&disks[0]);
    set_partition(
        &disks[0], 0U, efi_system_guid,
        TEST_ESP_FIRST_LBA,
        TEST_ESP_FIRST_LBA + TEST_ESP_BLOCK_COUNT - 1U);
    set_partition(
        &disks[0], 1U, hfoc_persistence_guid,
        TEST_HFOC_FIRST_LBA,
        TEST_HFOC_FIRST_LBA + TEST_HFOC_BLOCK_COUNT - 1U);
    set_partition(
        &disks[0], 2U, hfoc_persistence_guid,
        TEST_HFOC_FIRST_LBA + TEST_HFOC_BLOCK_COUNT,
        TEST_HFOC_FIRST_LBA + 2U * TEST_HFOC_BLOCK_COUNT - 1U);
    finalize_gpt(&disks[0]);
    devices[0] = disk_device(&disks[0]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 1U, &target) ==
              HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET,
          "multiple HFOC partitions on one disk fail closed");

    disk_with_esp_and_hfoc(&disks[0]);
    set_partition(
        &disks[0], 2U, efi_system_guid,
        TEST_ESP_FIRST_LBA + TEST_ESP_BLOCK_COUNT,
        TEST_ESP_FIRST_LBA + 2U * TEST_ESP_BLOCK_COUNT - 1U);
    finalize_gpt(&disks[0]);
    devices[0] = disk_device(&disks[0]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 1U, &target) ==
              HFOC_EFI_INSTALLER_AMBIGUOUS_ESP,
          "multiple target ESP partitions fail closed");

    disk_init(&disks[0]);
    set_partition(
        &disks[0], 0U, hfoc_persistence_guid,
        TEST_HFOC_FIRST_LBA,
        TEST_HFOC_FIRST_LBA + TEST_HFOC_BLOCK_COUNT - 1U);
    finalize_gpt(&disks[0]);
    devices[0] = disk_device(&disks[0]);
    CHECK(hfoc_efi_installer_select_target(
              devices, 1U, &target) ==
              HFOC_EFI_INSTALLER_NO_ESP,
          "HFOC target without ESP fails closed");
}

static void test_status_strings(void)
{
    CHECK(strcmp(hfoc_efi_installer_status_string(
                     HFOC_EFI_INSTALLER_OK),
                 "HFOC_EFI_INSTALLER_OK") == 0,
          "OK status string is exact");
    CHECK(strcmp(hfoc_efi_installer_status_string(
                     HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET),
                 "HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET") == 0,
          "ambiguous-target status string is exact");
    CHECK(hfoc_efi_installer_status_string(
              (enum hfoc_efi_installer_status)99U) == NULL,
          "undefined helper status is rejected");
}

int main(void)
{
    test_no_target_and_order_independence();
    test_ambiguity_and_esp_failures();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "HFOC EFI installer host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("HFOC EFI installer host tests: ALL OK");
    return 0;
}

#endif
