#include "installer_layout.h"

#include "installer_hardware_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_BLOCK_SIZE 512U
#define FIXTURE_BLOCK_COUNT 16384U

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_BLOCK_SIZE];
static size_t fixture_write_calls;
static size_t fixture_fail_write_number;

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static uint32_t fixture_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0U] | ((uint32_t)bytes[1U] << 8U) |
        ((uint32_t)bytes[2U] << 16U) | ((uint32_t)bytes[3U] << 24U);
}

static uint64_t fixture_load_le64(const unsigned char *bytes)
{
    return (uint64_t)fixture_load_le32(bytes) |
        ((uint64_t)fixture_load_le32(bytes + 4U) << 32U);
}

/* A CRC-32 the proof computes for itself, so the table's own field is checked
 * against an implementation that shares no code with the writer's. */
static uint32_t fixture_crc32(const unsigned char *bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0xEDB88320U
                                   : crc >> 1U;
        }
    }
    return crc ^ 0xFFFFFFFFU;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(fixture_storage) ||
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
    if (fixture_fail_write_number != 0U &&
        fixture_write_calls == fixture_fail_write_number - 1U) {
        fixture_fail_write_number = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    ++fixture_write_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device make_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_BLOCK_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    return device;
}

static void fixture_init(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_write_calls = 0U;
    fixture_fail_write_number = 0U;
}

static void fixture_store_le32(
    unsigned char *bytes,
    uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void fixture_set_mbr(void)
{
    unsigned char *partition = fixture_storage + 446U;

    fixture_init();
    fixture_storage[510U] = 0x55U;
    fixture_storage[511U] = 0xAAU;
    partition[4U] = 0x83U;
    fixture_store_le32(partition + 8U, 2048U);
    fixture_store_le32(partition + 12U, 4096U);
    partition += 16U;
    partition[4U] = 0x07U;
    fixture_store_le32(partition + 8U, 8192U);
    fixture_store_le32(partition + 12U, 4096U);
}

static bool fixture_empty_report(
    struct installer_discovery_report *report)
{
    struct installer_discovery_disk disk;

    if (installer_discovery_init(report) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_firmware(
            report, INSTALLER_FIRMWARE_UEFI) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_cpu(
            report, 1U) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_memory(
            report, 536870912U) != INSTALLER_DISCOVERY_OK) {
        return false;
    }
    memset(&disk, 0, sizeof(disk));
    disk.id = 1U;
    disk.present = true;
    disk.partition_table = INSTALLER_PARTITION_TABLE_NONE;
    disk.block_size = FIXTURE_BLOCK_SIZE;
    disk.block_count = FIXTURE_BLOCK_COUNT;
    strcpy(disk.identity, "nvme0");
    return installer_discovery_publish_disk(report, &disk) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK;
}

static void fixture_requests(
    struct installer_layout_partition requests[4U])
{
    memset(requests, 0, sizeof(requests[0U]) * 4U);
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].length_lba = 2048U;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].length_lba = 2048U;
    requests[2U].present = true;
    requests[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    requests[2U].length_lba = 2048U;
    requests[3U].present = true;
    requests[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    requests[3U].length_lba = 2048U;
}

static void fixture_manual_requests(
    struct installer_layout_partition requests[4U])
{
    fixture_requests(requests);
    requests[0U].first_lba = 2048U;
    requests[1U].first_lba = 4096U;
    requests[2U].first_lba = 6144U;
    requests[3U].first_lba = 8192U;
}

static bool fixture_rescan_has_partitions(
    size_t expected_partitions,
    bool has_esp)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();
    size_t index;
    size_t present = 0U;
    bool esp = false;

    memset(&report, 0, sizeof(report));
    if (installer_discovery_init(&report) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_scan_block_device(
            &report, &device, 1U,
            INSTALLER_DISK_CONTROLLER_NVME,
            "nvme0", 5U) != INSTALLER_DISCOVERY_OK) {
        return false;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report.partitions[index].present) {
            ++present;
            if (report.partitions[index].efi_system) {
                esp = true;
            }
        }
    }
    return present == expected_partitions &&
           esp == has_esp;
}

static void test_status_strings(void)
{
    CHECK(strcmp(installer_layout_status_string(
                     INSTALLER_LAYOUT_OK),
                 "INSTALLER_LAYOUT_OK") == 0,
          "installer layout OK string is exact");
    CHECK(installer_layout_status_string(
              (enum installer_layout_status)999U) == NULL,
          "installer layout undefined status is rejected");
}

static void test_whole_disk_lifecycle(void)
{
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_preview preview;
    struct installer_layout_snapshot snapshot;
    struct block_device device = make_device();

    fixture_init();
    CHECK(fixture_empty_report(&report),
          "whole-disk fixture report initializes");
    fixture_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
              plan.partition_count == 4U,
          "whole-disk plan creates four HFOC partitions");
    CHECK(plan.destructive,
          "whole-disk plan is always marked destructive");
    CHECK(installer_layout_preview(&plan, &preview) ==
              INSTALLER_LAYOUT_OK &&
              preview.partition_count == 4U &&
              strstr(preview.text, "whole-disk") != NULL,
          "whole-disk preview is published");
    CHECK(installer_layout_execute(
              &device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_NOT_CONFIRMED &&
              fixture_write_calls == 0U,
          "unconfirmed installer layout does not write");
    CHECK(installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "installer layout confirmation succeeds");
    CHECK(installer_layout_execute(
              &device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_OK &&
              fixture_write_calls != 0U &&
              plan.executed,
          "confirmed whole-disk layout writes and marks executed");
    CHECK(fixture_rescan_has_partitions(4U, true),
          "written whole-disk GPT is readable by discovery");
}

static void test_manual_lifecycle(void)
{
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_preview preview;
    struct installer_layout_snapshot snapshot;
    struct block_device device = make_device();

    fixture_init();
    CHECK(fixture_empty_report(&report),
          "manual fixture report initializes");
    fixture_manual_requests(requests);
    CHECK(installer_layout_plan_manual(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
              plan.mode == INSTALLER_LAYOUT_MODE_MANUAL,
          "manual layout plan accepts aligned free ranges");
    CHECK(installer_layout_preview(&plan, &preview) ==
              INSTALLER_LAYOUT_OK &&
              strstr(preview.text, "manual") != NULL,
          "manual layout preview is published");
    CHECK(installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_execute(
              &device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_OK,
          "confirmed manual layout writes successfully");
    CHECK(fixture_rescan_has_partitions(4U, true),
          "written manual GPT is readable by discovery");
}

static void test_failure_rollback(void)
{
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct block_device device = make_device();
    unsigned char original[FIXTURE_BLOCK_SIZE * 3U];

    fixture_init();
    CHECK(fixture_empty_report(&report),
          "rollback fixture report initializes");
    fixture_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) ==
              INSTALLER_LAYOUT_OK,
          "rollback fixture plan is confirmed");
    memcpy(original, fixture_storage, sizeof(original));
    fixture_fail_write_number = 3U;
    CHECK(installer_layout_execute(
              &device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_IO_ERROR,
          "injected write failure stops execution");
    CHECK(memcmp(original, fixture_storage, sizeof(original)) == 0,
          "failed layout execution restores original sectors");
}

/*
 * ITEM-414: the table this writer produces is one other implementations have to
 * accept.
 *
 * The partition entry array a GPT reserves is 16 KiB - 128 entries of 128 bytes
 * - and the header carries both that count and the CRC of those bytes.  This
 * writer declared four entries over one block, so every disk it produced
 * described itself as a table a reader is free to refuse: this project's own
 * reader does refuse it (`installer_gpt_preserve.c` requires 128), which is why
 * the menu could not offer a system on a disk this installer had written.  The
 * proof reads the table back out of the raw storage with its own little-endian
 * readers and its own CRC-32, and requires every field of a conformant table.
 */
static void test_written_table_is_conformant(void)
{
    enum { ARRAY_BYTES = 16384, ARRAY_BLOCKS = ARRAY_BYTES / 512 };
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct block_device device = make_device();
    static unsigned char expected[ARRAY_BYTES];
    const unsigned char *header;
    const unsigned char *array;
    const unsigned char *backup_header;
    const unsigned char *backup_array;
    const uint64_t backup_array_lba =
        FIXTURE_BLOCK_COUNT - 1U - ARRAY_BLOCKS;
    uint32_t array_crc;
    size_t index;
    bool tail_is_zero = true;

    fixture_init();
    CHECK(fixture_empty_report(&report),
          "conformance fixture report initializes");
    fixture_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
          installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK,
          "conformance fixture plan is confirmed");
    CHECK(installer_layout_execute(&device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_OK,
          "conformance fixture writes the table");

    header = fixture_storage + FIXTURE_BLOCK_SIZE;
    array = fixture_storage + 2U * FIXTURE_BLOCK_SIZE;
    backup_header = fixture_storage +
        (FIXTURE_BLOCK_COUNT - 1U) * FIXTURE_BLOCK_SIZE;
    backup_array = fixture_storage + backup_array_lba * FIXTURE_BLOCK_SIZE;

    CHECK(fixture_load_le32(header + 80U) == 128U &&
              fixture_load_le32(header + 84U) == 128U,
          "the header declares 128 entries of 128 bytes");
    CHECK(fixture_load_le64(header + 72U) == 2U,
          "the primary header names the primary array");
    CHECK(fixture_load_le64(header + 40U) == 34U &&
              fixture_load_le64(header + 48U) ==
                  FIXTURE_BLOCK_COUNT - 34U,
          "the usable range starts after the array and stops before the "
          "backup array");
    {
        unsigned char header_copy[92U];

        memcpy(header_copy, header, sizeof(header_copy));
        memset(header_copy + 16U, 0, 4U);
        CHECK(fixture_load_le32(header + 16U) ==
                  fixture_crc32(header_copy, sizeof(header_copy)),
              "the header's own CRC covers the header with its CRC field "
              "zeroed");
    }

    memset(expected, 0, sizeof(expected));
    memcpy(expected, array, FIXTURE_BLOCK_SIZE);
    array_crc = fixture_crc32(expected, sizeof(expected));
    CHECK(fixture_load_le32(header + 88U) == array_crc,
          "the array CRC is the CRC of the 16 KiB the disk carries");
    for (index = 4U * 128U; index < ARRAY_BYTES; ++index) {
        if (array[index] != 0U) {
            tail_is_zero = false;
            break;
        }
    }
    CHECK(tail_is_zero,
          "the entries after the fourth are zero, as the CRC assumes");

    CHECK(fixture_load_le64(backup_header + 72U) == backup_array_lba,
          "the backup header names the backup array, not the primary one");
    CHECK(fixture_load_le32(backup_header + 88U) == array_crc &&
              memcmp(backup_array, array, FIXTURE_BLOCK_SIZE) == 0,
          "the backup table carries the same array and the same CRC");
}

static void test_invalid_manual_overlap(void)
{
    struct installer_discovery_report report;
    struct installer_layout_partition requests[2U];
    struct installer_layout_plan plan;

    fixture_init();
    CHECK(fixture_empty_report(&report),
          "overlap fixture report initializes");
    memset(requests, 0, sizeof(requests));
    requests[0U].present = true;
    requests[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    requests[0U].first_lba = 2048U;
    requests[0U].length_lba = 4096U;
    requests[1U].present = true;
    requests[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    requests[1U].first_lba = 4096U;
    requests[1U].length_lba = 4096U;
    CHECK(installer_layout_plan_manual(
              &report, 1U, requests, 2U, &plan) ==
              INSTALLER_LAYOUT_AMBIGUOUS,
          "overlapping manual ranges fail closed");
}

static void test_destructive_preview(void)
{
    struct installer_discovery_report report;
    struct installer_layout_partition requests[4U];
    struct installer_layout_plan plan;
    struct installer_layout_preview preview;
    struct block_device device = make_device();

    fixture_set_mbr();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_UEFI) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 1U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(
              &report, 536870912U) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "sata0", 5U) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK,
          "destructive fixture discovery report initializes");
    fixture_requests(requests);
    CHECK(installer_layout_plan_whole_disk(
              &report, 1U, requests, 4U, &plan) ==
              INSTALLER_LAYOUT_OK &&
              plan.existing_partition_count == 2U &&
              plan.destructive,
          "whole-disk plan marks existing partitions destructive");
    CHECK(installer_layout_preview(&plan, &preview) ==
              INSTALLER_LAYOUT_OK &&
              preview.existing_partition_count == 2U &&
              preview.destructive,
          "destructive preview reports existing partition count");
}

int main(void)
{
    test_status_strings();
    test_whole_disk_lifecycle();
    test_manual_lifecycle();
    test_failure_rollback();
    test_written_table_is_conformant();
    test_invalid_manual_overlap();
    test_destructive_preview();
    if (failures != 0) {
        fprintf(stderr, "Installer layout host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer layout host test: OK\n");
    return 0;
}
