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
    fixture_fail_read = false;
}

static void fixture_set_mbr_signature(void)
{
    fixture_storage[510U] = 0x55U;
    fixture_storage[511U] = 0xAAU;
}

static void fixture_set_mbr_partition(
    size_t index,
    uint8_t type,
    uint32_t first_lba,
    uint32_t length_lba)
{
    const size_t offset = 446U + index * 16U;

    fixture_storage[offset + 4U] = type;
    fixture_store_le32(fixture_storage + offset + 8U, first_lba);
    fixture_store_le32(fixture_storage + offset + 12U, length_lba);
}

static void fixture_set_protective_mbr(void)
{
    fixture_set_mbr_signature();
    fixture_set_mbr_partition(
        0U, 0xEEU, 1U, FIXTURE_BLOCK_COUNT - 1U);
}

static const unsigned char fixture_efi_guid[16U] = {
    0x28U, 0x73U, 0x2AU, 0xC1U, 0x1FU, 0xF8U, 0xD2U, 0x11U,
    0xBAU, 0x4BU, 0x00U, 0xA0U, 0xC9U, 0x3EU, 0xC9U, 0x3BU
};

static const unsigned char fixture_linux_guid[16U] = {
    0xAFU, 0x3DU, 0xC6U, 0x0FU, 0x83U, 0x84U, 0x72U, 0x47U,
    0x8EU, 0x79U, 0x3DU, 0x69U, 0xD8U, 0x47U, 0x7DU, 0xE4U
};

static const unsigned char fixture_windows_guid[16U] = {
    0xA2U, 0xA0U, 0xD0U, 0xEBU, 0xE5U, 0xB9U, 0x33U, 0x44U,
    0x87U, 0xC0U, 0x68U, 0xB6U, 0xB7U, 0x26U, 0x99U, 0xC7U
};

static void fixture_set_gpt_header(void)
{
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char *header = fixture_storage + FIXTURE_BLOCK_SIZE;

    memcpy(header, signature, sizeof(signature));
    header[8U] = 0x00U;
    header[9U] = 0x00U;
    header[10U] = 0x01U;
    header[11U] = 0x00U;
    fixture_store_le32(header + 12U, 92U);
    fixture_store_le64(header + 24U, 1U);
    fixture_store_le64(header + 32U, FIXTURE_BLOCK_COUNT - 1U);
    fixture_store_le64(header + 40U, 34U);
    fixture_store_le64(header + 48U, FIXTURE_BLOCK_COUNT - 1U);
    fixture_store_le64(header + 72U, 2U);
    fixture_store_le32(header + 80U, 128U);
    fixture_store_le32(header + 84U, 128U);
}

static void fixture_set_gpt_partition(
    size_t index,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba)
{
    const size_t offset =
        (2U + index / 4U) * FIXTURE_BLOCK_SIZE +
        (index % 4U) * 128U;

    memcpy(fixture_storage + offset, type_guid, 16U);
    fixture_store_le64(fixture_storage + offset + 16U, index + 1U);
    fixture_store_le64(fixture_storage + offset + 32U, first_lba);
    fixture_store_le64(fixture_storage + offset + 40U, last_lba);
}

/*
 * ITEM-501: an EBR sector of an extended partition.  Entry 0 is the logical
 * partition, relative to this sector; entry 1 is the next EBR, relative to the
 * start of the extended partition (the `link_lba` argument).
 */
static void fixture_set_ebr(
    uint32_t ebr_lba,
    uint8_t logical_type,
    uint32_t logical_first,
    uint32_t logical_length,
    uint32_t link_lba)
{
    unsigned char *sector =
        fixture_storage + (size_t)ebr_lba * FIXTURE_BLOCK_SIZE;

    sector[510U] = 0x55U;
    sector[511U] = 0xAAU;
    sector[446U + 4U] = logical_type;
    fixture_store_le32(sector + 446U + 8U, logical_first);
    fixture_store_le32(sector + 446U + 12U, logical_length);
    if (link_lba != 0U) {
        sector[446U + 16U + 4U] = 0x0FU;
        fixture_store_le32(sector + 446U + 16U + 8U, link_lba);
        fixture_store_le32(sector + 446U + 16U + 12U, 1U);
    }
}

/* ITEM-501: the Btrfs superblock magic at byte 0x10040 of a partition. */
static void fixture_set_btrfs_superblock(uint64_t first_lba)
{
    static const unsigned char magic[8U] = {
        '_', 'B', 'H', 'R', 'f', 'S', '_', 'M'
    };

    memcpy(fixture_storage + (size_t)first_lba * FIXTURE_BLOCK_SIZE +
               0x10040U,
           magic, sizeof(magic));
}

static const struct installer_discovery_partition *fixture_partition_at(
    const struct installer_discovery_report *report,
    uint32_t partition_index)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].partition_index == partition_index) {
            return &report->partitions[index];
        }
    }
    return NULL;
}

static void fixture_set_mbr_fixture(void)
{
    fixture_init();
    fixture_set_mbr_signature();
    fixture_set_mbr_partition(0U, 0x83U, 2048U, 4096U);
    fixture_set_mbr_partition(1U, 0x07U, 8192U, 4096U);
}

static void fixture_set_overlapping_mbr_fixture(void)
{
    fixture_init();
    fixture_set_mbr_signature();
    fixture_set_mbr_partition(0U, 0x83U, 2048U, 4096U);
    fixture_set_mbr_partition(1U, 0x07U, 4096U, 4096U);
}

static void fixture_set_gpt_fixture(void)
{
    fixture_init();
    fixture_set_protective_mbr();
    fixture_set_gpt_header();
    fixture_set_gpt_partition(0U, fixture_efi_guid, 2048U, 4095U);
    fixture_set_gpt_partition(1U, fixture_linux_guid, 4096U, 8191U);
    fixture_set_gpt_partition(2U, fixture_windows_guid, 8192U, 12287U);
}

static void fixture_set_too_many_gpt_fixture(void)
{
    size_t index;

    fixture_init();
    fixture_set_protective_mbr();
    fixture_set_gpt_header();
    for (index = 0U; index < 65U; ++index) {
        fixture_set_gpt_partition(
            index, fixture_linux_guid,
            2048U + (uint64_t)index * 2U,
            2049U + (uint64_t)index * 2U);
    }
}

static bool fixture_has_partition_kind(
    const struct installer_discovery_report *report,
    enum installer_partition_kind kind)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].kind == kind) {
            return true;
        }
    }
    return false;
}

static bool fixture_has_os_kind(
    const struct installer_discovery_report *report,
    enum installer_os_kind kind)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        if (report->oss[index].present &&
            report->oss[index].kind == kind) {
            return true;
        }
    }
    return false;
}

static void test_status_strings(void)
{
    CHECK(installer_discovery_status_string(
              INSTALLER_DISCOVERY_OK) != NULL,
          "installer discovery status string is published");
    CHECK(strcmp(installer_discovery_status_string(
                     INSTALLER_DISCOVERY_OK),
                 "INSTALLER_DISCOVERY_OK") == 0,
          "installer discovery OK string is exact");
    CHECK(installer_discovery_status_string(
              (enum installer_discovery_status)999U) == NULL,
          "installer discovery undefined status is rejected");
}

static void test_init_and_publish(void)
{
    struct installer_discovery_report report;
    struct installer_discovery_disk disk;
    struct installer_discovery_partition partition;
    struct installer_discovery_nic nic;
    struct installer_discovery_pci pci;
    struct installer_discovery_os os;

    memset(&report, 0, sizeof(report));
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
              report.read_only &&
              report.version == INSTALLER_HARDWARE_DISCOVERY_VERSION,
          "installer discovery init publishes read-only report");
    CHECK(installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_UEFI) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes UEFI firmware mode");
    CHECK(installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
              INSTALLER_DISCOVERY_ALREADY_PUBLISHED,
          "installer discovery rejects conflicting firmware mode");
    CHECK(installer_discovery_publish_cpu(&report, 8U) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes CPU cores");
    CHECK(installer_discovery_publish_cpu(&report, 0U) ==
              INSTALLER_DISCOVERY_OUT_OF_RANGE,
          "installer discovery rejects zero CPU cores");
    CHECK(installer_discovery_publish_memory(&report, 536870912U) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes RAM bytes");

    memset(&disk, 0, sizeof(disk));
    disk.id = 1U;
    disk.present = true;
    disk.controller = INSTALLER_DISK_CONTROLLER_NVME;
    disk.partition_table = INSTALLER_PARTITION_TABLE_MBR;
    disk.block_size = 512U;
    disk.block_count = 4096U;
    strcpy(disk.identity, "nvme0");
    CHECK(installer_discovery_publish_disk(&report, &disk) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes disk");
    CHECK(installer_discovery_publish_disk(&report, &disk) ==
              INSTALLER_DISCOVERY_ALREADY_PUBLISHED,
          "installer discovery rejects duplicate disk");

    memset(&partition, 0, sizeof(partition));
    partition.id = 1U;
    partition.present = true;
    partition.disk_id = 1U;
    partition.partition_index = 1U;
    partition.first_lba = 2048U;
    partition.length_lba = 2048U;
    partition.kind = INSTALLER_PARTITION_EFI_SYSTEM;
    partition.efi_system = true;
    CHECK(installer_discovery_publish_partition(&report, &partition) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes EFI System Partition");
    CHECK(installer_discovery_publish_partition(&report, &partition) ==
              INSTALLER_DISCOVERY_ALREADY_PUBLISHED,
          "installer discovery rejects duplicate partition");

    memset(&nic, 0, sizeof(nic));
    nic.id = 1U;
    nic.present = true;
    nic.vendor_id = 0x1AF4U;
    nic.product_id = 0x1041U;
    nic.mac_address[0] = 0x02U;
    nic.mac_address[1] = 0x00U;
    nic.mac_address[2] = 0x00U;
    nic.mac_address[3] = 0x00U;
    nic.mac_address[4] = 0x00U;
    nic.mac_address[5] = 0x01U;
    CHECK(installer_discovery_publish_nic(&report, &nic) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes NIC");

    memset(&pci, 0, sizeof(pci));
    pci.id = 1U;
    pci.present = true;
    pci.vendor_id = 0x1AF4U;
    pci.product_id = 0x1041U;
    pci.class_code = 0x0200U;
    CHECK(installer_discovery_publish_pci(&report, &pci) ==
              INSTALLER_DISCOVERY_OK &&
              report.pcis[0].category == INSTALLER_PCI_NETWORK,
          "installer discovery classifies network PCI device");

    memset(&os, 0, sizeof(os));
    os.id = 1U;
    os.present = true;
    os.kind = INSTALLER_OS_OTHER;
    os.boot_path[0] = '/';
    strcpy(os.boot_path, "/EFI/HFOC/bootx64.efi");
    CHECK(installer_discovery_publish_os(&report, &os) ==
              INSTALLER_DISCOVERY_OK,
          "installer discovery publishes explicit existing OS");
}

static void test_mbr_scan(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();

    fixture_set_mbr_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 1U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(&report, 536870912U) ==
              INSTALLER_DISCOVERY_OK,
          "MBR fixture report is initialized");
    CHECK(installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "sata0", 5U) == INSTALLER_DISCOVERY_OK,
          "MBR fixture scan succeeds");
    CHECK(report.disks[0].present &&
              report.disks[0].partition_table ==
                  INSTALLER_PARTITION_TABLE_MBR &&
              report.partition_count == 2U,
          "MBR scan publishes disk and two partitions");
    CHECK(fixture_has_partition_kind(
              &report, INSTALLER_PARTITION_LINUX_ROOT),
          "MBR scan detects Linux partition");
    CHECK(fixture_has_partition_kind(
              &report, INSTALLER_PARTITION_WINDOWS),
          "MBR scan detects Windows partition");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
              fixture_has_os_kind(&report, INSTALLER_OS_LINUX) &&
              fixture_has_os_kind(&report, INSTALLER_OS_WINDOWS),
          "MBR scan derives Linux and Windows OS candidates");
    CHECK(installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_OK,
          "MBR discovery report validates");
}

static void test_publication_owner_inference(void)
{
    struct installer_discovery_report report;
    struct installer_discovery_disk disk;
    struct installer_discovery_partition partition;
    struct installer_discovery_partition invalid_partition;

    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_UEFI) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 1U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(&report, 536870912U) ==
              INSTALLER_DISCOVERY_OK,
          "owner-inference fixture report is initialized");

    memset(&disk, 0, sizeof(disk));
    disk.id = 1U;
    disk.present = true;
    disk.partition_table = INSTALLER_PARTITION_TABLE_GPT;
    disk.block_size = 512U;
    disk.block_count = 4096U;
    strcpy(disk.identity, "nvme0");
    CHECK(installer_discovery_publish_disk(&report, &disk) ==
              INSTALLER_DISCOVERY_OK,
          "owner-inference fixture publishes disk");

    memset(&partition, 0, sizeof(partition));
    partition.id = 1U;
    partition.present = true;
    partition.disk_id = 1U;
    partition.partition_index = 1U;
    partition.first_lba = 2048U;
    partition.length_lba = 2048U;
    partition.kind = INSTALLER_PARTITION_LINUX_ROOT;
    CHECK(installer_discovery_publish_partition(&report, &partition) ==
              INSTALLER_DISCOVERY_OK &&
              report.partitions[0].owner == INSTALLER_OS_LINUX,
          "manual Linux partition owner is inferred");

    memset(&invalid_partition, 0, sizeof(invalid_partition));
    invalid_partition.id = 2U;
    invalid_partition.present = true;
    invalid_partition.disk_id = 1U;
    invalid_partition.partition_index = 2U;
    invalid_partition.first_lba = 4096U;
    invalid_partition.length_lba = 2048U;
    invalid_partition.kind = INSTALLER_PARTITION_WINDOWS;
    invalid_partition.owner = INSTALLER_OS_LINUX;
    CHECK(installer_discovery_publish_partition(
              &report, &invalid_partition) ==
              INSTALLER_DISCOVERY_INVALID_ARGUMENT,
          "conflicting partition owner fails closed");

    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
              fixture_has_os_kind(&report, INSTALLER_OS_LINUX),
          "manual Linux partition derives existing OS candidate");
}

static void test_gpt_scan(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();

    fixture_set_gpt_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_UEFI) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 4U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(&report, 1073741824U) ==
              INSTALLER_DISCOVERY_OK,
          "GPT fixture report is initialized");
    CHECK(installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_NVME,
              "nvme0", 5U) == INSTALLER_DISCOVERY_OK,
          "GPT fixture scan succeeds");
    CHECK(report.disks[0].partition_table ==
              INSTALLER_PARTITION_TABLE_GPT &&
              report.disks[0].has_efi_system_partition &&
              report.partition_count == 3U,
          "GPT scan publishes ESP flag and partitions");
    CHECK(fixture_has_partition_kind(
              &report, INSTALLER_PARTITION_EFI_SYSTEM) &&
              fixture_has_partition_kind(
                  &report, INSTALLER_PARTITION_LINUX_ROOT) &&
              fixture_has_partition_kind(
                  &report, INSTALLER_PARTITION_WINDOWS),
          "GPT scan classifies ESP, Linux, and Windows partitions");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
              fixture_has_os_kind(&report, INSTALLER_OS_LINUX) &&
              fixture_has_os_kind(&report, INSTALLER_OS_WINDOWS),
          "GPT scan derives Linux and Windows OS candidates");
    CHECK(installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_OK,
          "GPT discovery report validates");
}

static void test_multiple_disk_scan(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();

    fixture_set_mbr_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 2U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(&report, 1073741824U) ==
              INSTALLER_DISCOVERY_OK,
          "multiple-disk fixture report is initialized");
    CHECK(installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "sata0", 5U) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 2U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "sata1", 5U) == INSTALLER_DISCOVERY_OK,
          "second disk scan succeeds with globally unique partition IDs");
    CHECK(report.disk_count == 2U &&
              report.partition_count == 4U &&
              report.disks[0].id == 1U &&
              report.disks[1].id == 2U,
          "multiple-disk scan publishes both disks and partitions");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_OK,
          "multiple-disk discovery report validates");
}

static void test_read_only_and_failures(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();
    struct installer_discovery_report empty;

    fixture_set_gpt_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_NVME,
              "nvme0", 5U) == INSTALLER_DISCOVERY_OK,
          "scan executes before finalization");
    CHECK(fixture_write_calls == 0U,
          "discovery scan performs no block writes");

    fixture_set_overlapping_mbr_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_firmware(
              &report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_cpu(&report, 1U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_publish_memory(&report, 536870912U) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "sata0", 5U) == INSTALLER_DISCOVERY_OK,
          "overlapping MBR fixture is scanned without writes");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
          installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_AMBIGUOUS,
          "overlapping partitions fail closed as ambiguous");

    fixture_init();
    fixture_storage[0U] = 0x01U;
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_SATA,
              "bad", 3U) ==
              INSTALLER_DISCOVERY_INVALID_PARTITION_TABLE,
          "nonblank invalid MBR/GPT sector fails closed");

    fixture_set_too_many_gpt_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_NVME,
              "nvme0", 5U) ==
              INSTALLER_DISCOVERY_TOO_MANY_PARTITIONS,
          "GPT with more than 64 nonzero partitions fails closed");

    fixture_init();
    device.block_size = 4096U;
    device.block_count = sizeof(fixture_storage) / 4096U;
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &report, &device, 1U,
              INSTALLER_DISK_CONTROLLER_NVME,
              "nvme0", 5U) ==
              INSTALLER_DISCOVERY_UNSUPPORTED_BLOCK_SIZE,
          "unsupported block size fails closed");

    memset(&empty, 0, sizeof(empty));
    CHECK(installer_discovery_validate(&empty) ==
              INSTALLER_DISCOVERY_BAD_VERSION,
          "uninitialized report cannot validate as safe");
}

/*
 * ITEM-495: the name the hardware reported for a disk, recorded beside the
 * identity and never folded into it.  The setter is a value machine with the
 * same fail-closed rules as the scan: a disk the report does not hold, text a
 * terminal cannot show, or a report that was already finalized are all refused
 * with the report unchanged.
 */
static void test_disk_model_recording(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();
    const char model[] = "HFOC Example SATA Fixture";

    fixture_set_mbr_fixture();
    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_firmware(
                  &report, INSTALLER_FIRMWARE_UEFI) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_cpu(&report, 2U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_memory(&report, 1073741824U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_scan_block_device(
                  &report, &device, 1U, INSTALLER_DISK_CONTROLLER_SATA,
                  "DEADBEEF00000001", 16U) == INSTALLER_DISCOVERY_OK,
          "a disk the model can be recorded on is scanned");

    CHECK(installer_discovery_set_disk_model(&report, 1U, model,
                                             sizeof(model) - 1U) ==
              INSTALLER_DISCOVERY_OK &&
              strcmp(report.disks[0].model, model) == 0 &&
              strcmp(report.disks[0].identity, "DEADBEEF00000001") == 0,
          "the model is recorded beside the identity, and the identity is not "
          "rewritten");
    CHECK(installer_discovery_set_disk_model(&report, 2U, model,
                                             sizeof(model) - 1U) ==
              INSTALLER_DISCOVERY_OUT_OF_RANGE,
          "a disk the report does not hold cannot be named");
    CHECK(installer_discovery_set_disk_model(&report, 1U, NULL, 4U) ==
              INSTALLER_DISCOVERY_INVALID_ARGUMENT &&
              installer_discovery_set_disk_model(&report, 1U, "", 0U) ==
                  INSTALLER_DISCOVERY_INVALID_ARGUMENT &&
              installer_discovery_set_disk_model(&report, 1U, "two\nlines", 9U) ==
                  INSTALLER_DISCOVERY_INVALID_ARGUMENT,
          "empty, null and unprintable names are refused");
    CHECK(installer_discovery_set_disk_model(&report, 1U,
                                             "012345678901234567890123456789"
                                             "01234567890", 41U) ==
              INSTALLER_DISCOVERY_INVALID_ARGUMENT &&
              strcmp(report.disks[0].model, model) == 0,
          "a name longer than the record holds is refused, and the recorded "
          "name survives the refusal");
    CHECK(installer_discovery_set_disk_model(NULL, 1U, model,
                                             sizeof(model) - 1U) ==
              INSTALLER_DISCOVERY_NULL,
          "a null report cannot be given a name");
    CHECK(installer_discovery_finalize(&report) == INSTALLER_DISCOVERY_OK &&
              installer_discovery_set_disk_model(&report, 1U, "LATE", 4U) ==
                  INSTALLER_DISCOVERY_ALREADY_PUBLISHED &&
              strcmp(report.disks[0].model, model) == 0,
          "a finished reading is not renamed");
}

/*
 * ITEM-501: what the plain table cannot say.  A Fedora laptop put its whole
 * system in an LVM volume group on a partition the discovery could only call
 * "Linux/unknown", and an MBR disk with logical partitions lost every one of
 * them because only the four primary entries were read.  These proofs name the
 * container, the logical partitions and the volume layout the operator has to
 * see before choosing anything.
 */
static void test_layout_names(void)
{
    CHECK(strcmp(installer_partition_layout_name(
                     INSTALLER_PARTITION_LAYOUT_PLAIN), "PLAIN") == 0 &&
              strcmp(installer_partition_layout_name(
                         INSTALLER_PARTITION_LAYOUT_LVM), "LVM") == 0 &&
              strcmp(installer_partition_layout_name(
                         INSTALLER_PARTITION_LAYOUT_BTRFS), "BTRFS") == 0 &&
              strcmp(installer_partition_layout_name(
                         INSTALLER_PARTITION_LAYOUT_EXTENDED),
                     "EXTENDED") == 0 &&
              strcmp(installer_partition_layout_name(
                         INSTALLER_PARTITION_LAYOUT_LOGICAL),
                     "LOGICAL") == 0 &&
              installer_partition_layout_name(
                  (enum installer_partition_layout)99U) == NULL,
          "every partition layout has one stable name");
}

static void test_mbr_logical_partitions(void)
{
    struct installer_discovery_report report;
    struct block_device device = make_device();
    const struct installer_discovery_partition *container;
    const struct installer_discovery_partition *first_logical;
    const struct installer_discovery_partition *second_logical;

    fixture_init();
    fixture_set_mbr_signature();
    /* The extended container: LBA 4096, 8192 sectors. */
    fixture_set_mbr_partition(0U, 0x0FU, 4096U, 8192U);
    /* EBR 1 at 4096: one Linux logical partition, link to the next EBR. */
    fixture_set_ebr(4096U, 0x83U, 2048U, 2048U, 4096U);
    /* EBR 2 at 8192: one Windows logical partition, no further link. */
    fixture_set_ebr(8192U, 0x07U, 1024U, 2048U, 0U);

    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_firmware(
                  &report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_cpu(&report, 1U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_memory(&report, 536870912U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_scan_block_device(
                  &report, &device, 1U, INSTALLER_DISK_CONTROLLER_SATA,
                  "sata0", 5U) == INSTALLER_DISCOVERY_OK,
          "an MBR disk with an extended partition scans");
    CHECK(report.partition_count == 3U,
          "the scan publishes the container and both logical partitions");

    container = fixture_partition_at(&report, 1U);
    first_logical = fixture_partition_at(&report, 5U);
    second_logical = fixture_partition_at(&report, 6U);
    CHECK(container != NULL && container->container &&
              container->layout == INSTALLER_PARTITION_LAYOUT_EXTENDED &&
              container->first_lba == 4096U && container->length_lba == 8192U,
          "the extended container is published as a container, not a target");
    CHECK(first_logical != NULL &&
              first_logical->layout == INSTALLER_PARTITION_LAYOUT_LOGICAL &&
              first_logical->kind == INSTALLER_PARTITION_LINUX_ROOT &&
              first_logical->first_lba == 6144U &&
              first_logical->length_lba == 2048U,
          "the first logical partition is named with its own extent");
    CHECK(second_logical != NULL &&
              second_logical->layout == INSTALLER_PARTITION_LAYOUT_LOGICAL &&
              second_logical->kind == INSTALLER_PARTITION_WINDOWS &&
              second_logical->first_lba == 9216U &&
              second_logical->length_lba == 2048U,
          "the second EBR's logical partition is reached through the chain");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
              fixture_has_os_kind(&report, INSTALLER_OS_LINUX) &&
              fixture_has_os_kind(&report, INSTALLER_OS_WINDOWS),
          "the logical partitions contribute their own systems");
    CHECK(installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_OK,
          "the extended-partition report validates");
}

static void test_gpt_names_lvm_and_btrfs(void)
{
    static const unsigned char lvm_guid[16U] = {
        0x79U, 0xD3U, 0xD6U, 0xE6U, 0x07U, 0xF5U, 0xC2U, 0x44U,
        0xA2U, 0x3CU, 0x23U, 0x8FU, 0x2AU, 0x3DU, 0xF9U, 0x28U
    };
    struct installer_discovery_report report;
    struct block_device device = make_device();
    const struct installer_discovery_partition *lvm;
    const struct installer_discovery_partition *btrfs;
    const struct installer_discovery_partition *plain;

    fixture_init();
    fixture_set_protective_mbr();
    fixture_set_gpt_header();
    fixture_set_gpt_partition(0U, lvm_guid, 2048U, 4095U);
    fixture_set_gpt_partition(1U, fixture_linux_guid, 4096U, 8191U);
    fixture_set_gpt_partition(2U, fixture_linux_guid, 8192U, 12287U);
    fixture_set_btrfs_superblock(4096U);

    CHECK(installer_discovery_init(&report) == INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_firmware(
                  &report, INSTALLER_FIRMWARE_UEFI) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_cpu(&report, 1U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_publish_memory(&report, 536870912U) ==
                  INSTALLER_DISCOVERY_OK &&
              installer_discovery_scan_block_device(
                  &report, &device, 1U, INSTALLER_DISK_CONTROLLER_NVME,
                  "nvme0", 5U) == INSTALLER_DISCOVERY_OK,
          "a GPT disk with LVM, Btrfs and plain Linux scans");

    lvm = fixture_partition_at(&report, 1U);
    btrfs = fixture_partition_at(&report, 2U);
    plain = fixture_partition_at(&report, 3U);
    CHECK(lvm != NULL && lvm->layout == INSTALLER_PARTITION_LAYOUT_LVM,
          "the LVM physical volume is named as LVM");
    CHECK(btrfs != NULL && btrfs->layout == INSTALLER_PARTITION_LAYOUT_BTRFS &&
              btrfs->kind == INSTALLER_PARTITION_LINUX_ROOT,
          "the Btrfs filesystem is named by its superblock");
    CHECK(plain != NULL && plain->layout == INSTALLER_PARTITION_LAYOUT_PLAIN,
          "a Linux partition without the Btrfs magic stays plain");
    CHECK(installer_discovery_finalize(&report) ==
              INSTALLER_DISCOVERY_OK &&
              installer_discovery_validate(&report) ==
              INSTALLER_DISCOVERY_OK,
          "the layout report validates");
}

int main(void)
{
    test_status_strings();
    test_layout_names();
    test_init_and_publish();
    test_mbr_scan();
    test_mbr_logical_partitions();
    test_publication_owner_inference();
    test_gpt_scan();
    test_gpt_names_lvm_and_btrfs();
    test_multiple_disk_scan();
    test_read_only_and_failures();
    test_disk_model_recording();
    if (failures != 0) {
        fprintf(stderr, "Installer discovery host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer discovery host test: OK\n");
    return 0;
}
