#include "installer_layout_contract.h"

#include "block_device.h"
#include "installer_dual_boot.h"
#include "installer_hardware_discovery.h"
#include "installer_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INSTALLER_LAYOUT_CONTRACT_HOST_TEST

#define FIXTURE_SECTOR_SIZE 512U
#define FIXTURE_SECTOR_SLOTS 4096U
#define FIXTURE_MEDIA_LBA UINT64_C(234375000)
#define FIXTURE_MIB_LBA 2048U
#define FIXTURE_GPT_TAIL_LBA 34U

/* The declared numbers are pinned here as well as in the product header: a
 * size change must be deliberate and must move this proof with it. */
#define EXPECTED_EFI_SYSTEM_LBA UINT64_C(67584)
#define EXPECTED_SYSTEM_SLOT_LBA UINT64_C(32768)
#define EXPECTED_HFOC_SYSTEM_LBA UINT64_C(131072)
#define EXPECTED_HFOC_PERSISTENCE_LBA UINT64_C(65536)
#define EXPECTED_HFOC_RECOVERY_LBA UINT64_C(65536)
#define EXPECTED_FOOTPRINT_LBA UINT64_C(329728)
#define EXPECTED_DUAL_BOOT_FOOTPRINT_LBA UINT64_C(262144)
#define EXPECTED_MINIMUM_MEDIA_LBA UINT64_C(331810)
/*
 * ITEM-517: the legacy-BIOS plan is a second declared layout, pinned here the
 * same way.  It has no EFI System Partition: the BIOS-boot role carries the
 * Limine stage2 region and the FAT boot volume carries the installed payload.
 */
#define EXPECTED_BIOS_BOOT_LENGTH_LBA UINT64_C(2048)
#define EXPECTED_BIOS_BOOT_VOLUME_LENGTH_LBA UINT64_C(67584)
#define EXPECTED_BIOS_FOOTPRINT_LBA UINT64_C(331776)
#define EXPECTED_BIOS_MINIMUM_MEDIA_LBA UINT64_C(333858)
#define EXPECTED_CLEAN_ROLE_COUNT 4U
#define EXPECTED_BIOS_ROLE_COUNT 5U
static const unsigned char expected_bios_boot_type_guid[16U] = {
    0x48U, 0x61U, 0x68U, 0x21U, 0x49U, 0x64U, 0x6FU, 0x6EU,
    0x74U, 0x4EU, 0x65U, 0x65U, 0x64U, 0x45U, 0x46U, 0x49U
};

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct fixture_sector {
    bool used;
    uint64_t lba;
    unsigned char data[FIXTURE_SECTOR_SIZE];
};

static struct fixture_sector fixture_sectors[FIXTURE_SECTOR_SLOTS];
static size_t fixture_sector_count;
static uint64_t fixture_block_count = FIXTURE_MEDIA_LBA;
static size_t fixture_write_calls;
static size_t fixture_fail_write_call;

static struct fixture_sector *fixture_find(uint64_t lba)
{
    size_t index;

    for (index = 0U; index < fixture_sector_count; ++index) {
        if (fixture_sectors[index].used &&
            fixture_sectors[index].lba == lba) {
            return &fixture_sectors[index];
        }
    }
    return NULL;
}

static struct fixture_sector *fixture_allocate(uint64_t lba)
{
    struct fixture_sector *sector = fixture_find(lba);

    if (sector != NULL) {
        return sector;
    }
    if (fixture_sector_count >= FIXTURE_SECTOR_SLOTS) {
        return NULL;
    }
    sector = &fixture_sectors[fixture_sector_count++];
    memset(sector, 0, sizeof(*sector));
    sector->used = true;
    sector->lba = lba;
    return sector;
}

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    unsigned char *bytes = (unsigned char *)buffer;
    uint64_t lba;
    size_t count;
    size_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_SECTOR_SIZE != 0U ||
        length % FIXTURE_SECTOR_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    lba = offset / FIXTURE_SECTOR_SIZE;
    count = length / FIXTURE_SECTOR_SIZE;
    if (lba >= fixture_block_count ||
        count > fixture_block_count - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < count; ++index) {
        const struct fixture_sector *sector =
            fixture_find(lba + index);

        if (sector == NULL) {
            memset(bytes + index * FIXTURE_SECTOR_SIZE, 0,
                   FIXTURE_SECTOR_SIZE);
        } else {
            memcpy(bytes + index * FIXTURE_SECTOR_SIZE,
                   sector->data, FIXTURE_SECTOR_SIZE);
        }
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    const unsigned char *bytes = (const unsigned char *)buffer;
    uint64_t lba;
    size_t count;
    size_t index;

    (void)device;
    if (buffer == NULL || length == 0U ||
        offset % FIXTURE_SECTOR_SIZE != 0U ||
        length % FIXTURE_SECTOR_SIZE != 0U) {
        return BLOCK_DEVICE_UNALIGNED;
    }
    ++fixture_write_calls;
    if (fixture_fail_write_call != 0U &&
        fixture_write_calls == fixture_fail_write_call) {
        fixture_fail_write_call = 0U;
        return BLOCK_DEVICE_IO_ERROR;
    }
    lba = offset / FIXTURE_SECTOR_SIZE;
    count = length / FIXTURE_SECTOR_SIZE;
    if (lba >= fixture_block_count ||
        count > fixture_block_count - lba) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    for (index = 0U; index < count; ++index) {
        struct fixture_sector *sector =
            fixture_allocate(lba + index);

        if (sector == NULL) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        memcpy(sector->data,
               bytes + index * FIXTURE_SECTOR_SIZE,
               FIXTURE_SECTOR_SIZE);
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static void fixture_init(void)
{
    memset(fixture_sectors, 0, sizeof(fixture_sectors));
    fixture_sector_count = 0U;
    fixture_block_count = FIXTURE_MEDIA_LBA;
    fixture_write_calls = 0U;
    fixture_fail_write_call = 0U;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_SECTOR_SIZE;
    device.block_count = fixture_block_count;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static bool fixture_publish_disk(
    struct installer_discovery_report *report,
    uint32_t disk_id,
    const char *identity,
    enum installer_partition_table_kind table,
    uint64_t block_count)
{
    struct installer_discovery_disk disk;

    memset(&disk, 0, sizeof(disk));
    disk.id = disk_id;
    disk.present = true;
    disk.controller = INSTALLER_DISK_CONTROLLER_NVME;
    disk.partition_table = table;
    disk.block_size = FIXTURE_SECTOR_SIZE;
    disk.block_count = block_count;
    snprintf(disk.identity, sizeof(disk.identity), "%s", identity);
    return installer_discovery_publish_disk(report, &disk) ==
           INSTALLER_DISCOVERY_OK;
}

static bool fixture_publish_partition(
    struct installer_discovery_report *report,
    uint32_t partition_id,
    uint32_t disk_id,
    uint32_t partition_index,
    enum installer_partition_kind kind,
    bool efi_system,
    uint64_t first_lba,
    uint64_t length_lba)
{
    struct installer_discovery_partition partition;

    memset(&partition, 0, sizeof(partition));
    partition.id = partition_id;
    partition.present = true;
    partition.disk_id = disk_id;
    partition.partition_index = partition_index;
    partition.first_lba = first_lba;
    partition.length_lba = length_lba;
    partition.kind = kind;
    partition.efi_system = efi_system;
    return installer_discovery_publish_partition(
               report, &partition) == INSTALLER_DISCOVERY_OK;
}

static bool fixture_publish_os(
    struct installer_discovery_report *report,
    uint32_t os_id,
    enum installer_os_kind kind,
    uint32_t partition_id)
{
    struct installer_discovery_os os;

    memset(&os, 0, sizeof(os));
    os.id = os_id;
    os.present = true;
    os.kind = kind;
    os.partition_id = partition_id;
    return installer_discovery_publish_os(report, &os) ==
           INSTALLER_DISCOVERY_OK;
}

static bool fixture_clean_report(
    struct installer_discovery_report *report,
    uint64_t block_count)
{
    memset(report, 0, sizeof(*report));
    return installer_discovery_init(report) == INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_firmware(
               report, INSTALLER_FIRMWARE_UEFI) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_cpu(report, 2U) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_memory(
               report, 4294967296ULL) == INSTALLER_DISCOVERY_OK &&
           fixture_publish_disk(
               report, 1U, "nvme0",
               INSTALLER_PARTITION_TABLE_NONE, block_count) &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK;
}

/*
 * ITEM-517: the same clean target as the UEFI fixture, booted by a legacy
 * BIOS.  The firmware mode is the only difference; the target disk is a whole
 * disk with no table, which is the owner's machine.
 */
static bool fixture_bios_clean_report(
    struct installer_discovery_report *report,
    uint64_t block_count)
{
    memset(report, 0, sizeof(*report));
    return installer_discovery_init(report) == INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_firmware(
               report, INSTALLER_FIRMWARE_BIOS_LEGACY) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_cpu(report, 2U) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_memory(
               report, 4294967296ULL) == INSTALLER_DISCOVERY_OK &&
           fixture_publish_disk(
               report, 1U, "sata0",
               INSTALLER_PARTITION_TABLE_NONE, block_count) &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK;
}

static bool fixture_dual_boot_report(
    struct installer_discovery_report *report,
    uint64_t block_count,
    uint64_t linux_length_lba,
    uint64_t windows_length_lba)
{
    const uint64_t linux_first = 4096U;
    const uint64_t windows_first = linux_first + linux_length_lba;

    memset(report, 0, sizeof(*report));
    return installer_discovery_init(report) == INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_firmware(
               report, INSTALLER_FIRMWARE_UEFI) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_cpu(report, 2U) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_publish_memory(
               report, 4294967296ULL) == INSTALLER_DISCOVERY_OK &&
           fixture_publish_disk(
               report, 1U, "sata0",
               INSTALLER_PARTITION_TABLE_GPT, block_count) &&
           fixture_publish_partition(
               report, 1U, 1U, 1U,
               INSTALLER_PARTITION_EFI_SYSTEM, true, 2048U, 2048U) &&
           fixture_publish_partition(
               report, 2U, 1U, 2U,
               INSTALLER_PARTITION_LINUX_ROOT, false,
               linux_first, linux_length_lba) &&
           fixture_publish_partition(
               report, 3U, 1U, 3U,
               INSTALLER_PARTITION_WINDOWS, false,
               windows_first, windows_length_lba) &&
           fixture_publish_os(
               report, 1U, INSTALLER_OS_LINUX, 2U) &&
           fixture_publish_os(
               report, 2U, INSTALLER_OS_WINDOWS, 3U) &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_validate(report) ==
               INSTALLER_DISCOVERY_OK;
}

static bool fixture_role_equals(
    const struct installer_layout_contract_summary *summary,
    enum installer_layout_role role,
    uint64_t first_lba,
    uint64_t length_lba)
{
    size_t index;

    for (index = 0U; index < summary->role_count; ++index) {
        if (summary->roles[index].role == role) {
            return summary->roles[index].present &&
                   summary->roles[index].first_lba == first_lba &&
                   summary->roles[index].length_lba == length_lba;
        }
    }
    return false;
}

static bool fixture_gpt_entry(
    struct block_device *device,
    uint32_t entry_index,
    uint64_t *first_lba,
    uint64_t *last_lba)
{
    unsigned char array[FIXTURE_SECTOR_SIZE];
    const unsigned char *entry;
    size_t byte_index;

    if (block_device_read(device,
                          (2U + (entry_index / 4U)) *
                              FIXTURE_SECTOR_SIZE,
                          array, sizeof(array)) != BLOCK_DEVICE_OK) {
        return false;
    }
    if (entry_index >= 128U) {
        return false;
    }
    entry = array + (size_t)(entry_index % 4U) * 128U;
    for (byte_index = 0U; byte_index < 16U; ++byte_index) {
        if (entry[byte_index] != 0U) {
            break;
        }
        if (byte_index == 15U) {
            return false;
        }
    }
    *first_lba = 0U;
    *last_lba = 0U;
    for (byte_index = 0U; byte_index < 8U; ++byte_index) {
        *first_lba |= (uint64_t)entry[32U + byte_index] << (8U * byte_index);
        *last_lba |= (uint64_t)entry[40U + byte_index] << (8U * byte_index);
    }
    return true;
}

/* ITEM-517: the GPT type GUID the executed plan wrote for one entry. */
static bool fixture_gpt_entry_type(
    struct block_device *device,
    uint32_t entry_index,
    unsigned char type[16U])
{
    unsigned char array[FIXTURE_SECTOR_SIZE];
    const unsigned char *entry;

    if (device == NULL || type == NULL || entry_index >= 128U) {
        return false;
    }
    if (block_device_read(device,
                          (2U + (entry_index / 4U)) *
                              FIXTURE_SECTOR_SIZE,
                          array, sizeof(array)) != BLOCK_DEVICE_OK) {
        return false;
    }
    entry = array + (size_t)(entry_index % 4U) * 128U;
    memcpy(type, entry, 16U);
    return true;
}

static void test_declared_arithmetic(void)
{
    CHECK(INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA ==
              EXPECTED_EFI_SYSTEM_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA ==
              EXPECTED_HFOC_SYSTEM_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA ==
              EXPECTED_HFOC_PERSISTENCE_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA ==
              EXPECTED_HFOC_RECOVERY_LBA,
          "declared role sizes are the pinned product numbers");
    CHECK(INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA ==
              EXPECTED_FOOTPRINT_LBA &&
          INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA ==
              EXPECTED_DUAL_BOOT_FOOTPRINT_LBA,
          "declared clean and dual-boot footprints are the pinned totals");
    CHECK(INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA ==
              INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA,
          "the clean footprint is exactly the four role sizes");
    CHECK(INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA ==
              EXPECTED_MINIMUM_MEDIA_LBA &&
          INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA +
                  INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA,
          "the minimum media is the pinned exact layout floor");
    CHECK(INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U,
          "every role size and the footprint are 1 MiB aligned");
    CHECK(installer_layout_contract_mib(
              INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA) == 33U &&
          installer_layout_contract_mib(
              INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA) == 64U &&
          installer_layout_contract_mib(
              INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA) == 32U &&
          installer_layout_contract_mib(
              INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA) == 32U &&
          installer_layout_contract_mib(
              INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA) == 161U &&
          INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA ==
              EXPECTED_SYSTEM_SLOT_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA ==
              4U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA ==
              2U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA &&
          INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA ==
              2U * INSTALLER_LAYOUT_CONTRACT_SYSTEM_SLOT_LBA,
          "declared MiB values are exact");
    CHECK(installer_layout_contract_mib(0U) == 0U &&
          installer_layout_contract_mib(2048U) == 1U &&
          installer_layout_contract_mib(2047U) == 0U,
          "MiB conversion truncates below one MiB");
}

static void test_minimum_media_boundary(void)
{
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_contract_summary summary;

    fixture_init();
    CHECK(fixture_clean_report(
              &report, INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA),
          "minimum-media fixture report initializes");
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK,
          "the exact minimum media is accepted");
    CHECK(plan.version == INSTALLER_LAYOUT_VERSION &&
          plan.mode == INSTALLER_LAYOUT_MODE_WHOLE_DISK &&
          plan.partition_count == EXPECTED_CLEAN_ROLE_COUNT &&
          plan.confirmed == false && plan.executed == false,
          "the contracted plan is an unconfirmed whole-disk layout");
    CHECK(plan.partitions[0U].role ==
              INSTALLER_LAYOUT_ROLE_EFI_SYSTEM &&
          plan.partitions[0U].first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
          plan.partitions[0U].length_lba ==
              INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA &&
          plan.partitions[1U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM &&
          plan.partitions[1U].first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA &&
          plan.partitions[2U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE &&
          plan.partitions[3U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
          "the plan places the four roles in the declared order");
    CHECK(summary.mode ==
              INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK &&
          summary.media_lba ==
              INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA &&
          summary.declared_lba ==
              INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA &&
          summary.footprint_lba ==
              INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA &&
          summary.first_lba == INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
          summary.free_first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA &&
          summary.free_length_lba == 0U &&
          summary.role_count == EXPECTED_CLEAN_ROLE_COUNT,
          "the exact minimum media leaves no free space");

    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean(
              &report, 2U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "an unknown target disk is refused");

    memset(&plan, 0, sizeof(plan));
    if (!fixture_clean_report(
            &report,
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA - 1U)) {
        CHECK(false, "below-minimum fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL,
          "one sector below the minimum media is refused");
    CHECK(strcmp(installer_layout_contract_status_string(
                     INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL),
                 "INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL") == 0,
          "the minimum-media refusal has an exact status string");
    CHECK(plan.version == 0U && plan.partition_count == 0U,
          "a refused layout leaves no partial plan behind");

    fixture_init();
    if (!fixture_clean_report(
            &report,
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA +
                FIXTURE_MIB_LBA)) {
        CHECK(false, "one-MiB-above fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          summary.free_length_lba == FIXTURE_MIB_LBA &&
          summary.free_first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
          "one MiB above the minimum leaves exactly one MiB free");
}

/*
 * ITEM-517, requirement 3: the legacy-BIOS whole-disk layout.  The ESP role is
 * not required - it is not in the plan at all - and the plan carries the
 * BIOS-boot partition (the `Hah!IdontNeedEFI` GPT type) plus a FAT boot volume
 * holding the installed payload instead.
 */
static void test_bios_clean_plan(void)
{
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_contract_summary summary;
    struct installer_layout_snapshot snapshot;
    struct block_device device;
    unsigned char type_guid[16U];
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    char preview[INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH];
    uint64_t first_lba = 0U;
    uint64_t last_lba = 0U;
    size_t length = 0U;
    size_t index;
    bool esp_seen = false;

    CHECK(INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA ==
              EXPECTED_BIOS_BOOT_LENGTH_LBA &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA ==
              EXPECTED_BIOS_BOOT_VOLUME_LENGTH_LBA,
          "the BIOS role sizes are the pinned product numbers");
    CHECK(INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS == 64U &&
          INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_BYTES == 32768U &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA >=
              INSTALLER_LAYOUT_BIOS_BOOT_MINIMUM_SECTORS,
          "the declared BIOS-boot role holds the measured Limine minimum");
    CHECK(INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA ==
              EXPECTED_BIOS_FOOTPRINT_LBA &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA,
          "the BIOS footprint is exactly the five role sizes");
    CHECK(INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA ==
              EXPECTED_BIOS_MINIMUM_MEDIA_LBA &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA +
                  INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA,
          "the BIOS minimum media is the pinned exact layout floor");
    CHECK(INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U &&
          INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA %
                  INSTALLER_LAYOUT_ALIGN_LBA == 0U,
          "every BIOS role size and the footprint are 1 MiB aligned");

    fixture_init();
    if (!fixture_bios_clean_report(
            &report, INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA)) {
        CHECK(false, "the BIOS minimum-media fixture report initializes");
        return;
    }
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean_bios(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK,
          "the exact BIOS minimum media is accepted");
    CHECK(plan.partition_count == EXPECTED_BIOS_ROLE_COUNT &&
          plan.firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY,
          "the BIOS plan is a five-role whole-disk layout");
    CHECK(plan.partitions[0U].role == INSTALLER_LAYOUT_ROLE_BIOS_BOOT &&
          plan.partitions[0U].first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
          plan.partitions[0U].length_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA &&
          plan.partitions[1U].role ==
              INSTALLER_LAYOUT_ROLE_BOOT_VOLUME &&
          plan.partitions[1U].first_lba ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA &&
          plan.partitions[2U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM &&
          plan.partitions[3U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE &&
          plan.partitions[4U].role ==
              INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
          "the BIOS plan puts the BIOS boot role first and the payload roles after it");
    for (index = 0U; index < EXPECTED_BIOS_ROLE_COUNT; ++index) {
        if (plan.partitions[index].role ==
            INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            esp_seen = true;
        }
    }
    CHECK(!esp_seen, "the BIOS plan has no EFI System Partition role");
    CHECK(summary.mode ==
              INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS &&
          summary.footprint_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA &&
          summary.declared_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA &&
          summary.minimum_media_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA &&
          summary.media_lba ==
              INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA &&
          summary.first_lba == INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
          summary.free_length_lba == 0U &&
          summary.role_count == EXPECTED_BIOS_ROLE_COUNT &&
          summary.esp_reused == false,
          "the BIOS summary carries the BIOS constants and no reused ESP");
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &summary) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the BIOS summary passes its own geometry validation");
    CHECK(installer_layout_contract_role_string(
              INSTALLER_LAYOUT_ROLE_BIOS_BOOT) != NULL &&
          strcmp(installer_layout_contract_role_string(
                     INSTALLER_LAYOUT_ROLE_BIOS_BOOT),
                 "bios-boot") == 0 &&
          installer_layout_contract_role_string(
              INSTALLER_LAYOUT_ROLE_BOOT_VOLUME) != NULL &&
          strcmp(installer_layout_contract_role_string(
                     INSTALLER_LAYOUT_ROLE_BOOT_VOLUME),
                 "boot-volume") == 0,
          "the two BIOS roles have exact role strings");
    CHECK(installer_layout_contract_render_record(
              &summary, record, sizeof(record), &length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          strstr(record, "plan mode=clean-whole-disk-bios") != NULL &&
          strstr(record, " minimum_media_lba=333858") != NULL &&
          strstr(record,
                 "role=bios-boot first_lba=2048 length_lba=2048") != NULL &&
          strstr(record, "role=boot-volume") != NULL &&
          strstr(record, "role=efi-system") == NULL,
          "the BIOS record names the BIOS layout and no ESP");
    CHECK(installer_layout_contract_render_preview(
              &summary, preview, sizeof(preview), &length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          strstr(preview, "mode=clean-whole-disk-bios") != NULL &&
          strstr(preview, "bios-boot:2048+2048=1") != NULL &&
          strstr(preview, "boot-volume:4096+67584=33") != NULL,
          "the BIOS preview names both BIOS roles with their sectors");
    CHECK(installer_layout_role_type_guid(
              INSTALLER_LAYOUT_ROLE_BIOS_BOOT) != NULL &&
          memcmp(installer_layout_role_type_guid(
                     INSTALLER_LAYOUT_ROLE_BIOS_BOOT),
                 expected_bios_boot_type_guid, 16U) == 0,
          "the BIOS-boot role carries the Hah!IdontNeedEFI GPT type");

    /* The BIOS floor is its own: one sector below it is refused, and so is a
       disk that only satisfies the UEFI layout's smaller floor. */
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_bios_clean_report(
            &report,
            INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA - 1U)) {
        CHECK(false, "the below-BIOS-minimum fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_clean_bios(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL,
          "one sector below the BIOS minimum media is refused");
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_bios_clean_report(
            &report, INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA)) {
        CHECK(false, "the UEFI-floor fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_clean_bios(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL,
          "a disk that fits the UEFI layout is still below the BIOS floor");
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          plan.partition_count == EXPECTED_CLEAN_ROLE_COUNT,
          "the UEFI clean plan still fits that same disk");

    /* Execute the BIOS plan and read the GPT back: the type GUIDs the plan
       declared are the ones on the disk. */
    fixture_init();
    if (!fixture_bios_clean_report(&report, FIXTURE_MEDIA_LBA)) {
        CHECK(false, "the executable BIOS fixture report initializes");
        return;
    }
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    memset(&snapshot, 0, sizeof(snapshot));
    CHECK(installer_layout_contract_plan_clean_bios(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          installer_layout_contract_validate_summary(
              &report, 1U, &summary) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the executable BIOS plan validates");
    device = fixture_device();
    CHECK(installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
          installer_layout_execute(&device, &plan, &snapshot) ==
              INSTALLER_LAYOUT_OK &&
          plan.executed,
          "the BIOS plan executes on the target disk");
    memset(type_guid, 0, sizeof(type_guid));
    CHECK(fixture_gpt_entry_type(&device, 0U, type_guid) &&
          memcmp(type_guid, expected_bios_boot_type_guid, 16U) == 0,
          "the executed GPT carries the Hah!IdontNeedEFI BIOS-boot type");
    memset(type_guid, 0, sizeof(type_guid));
    CHECK(installer_layout_role_type_guid(
              INSTALLER_LAYOUT_ROLE_BOOT_VOLUME) != NULL &&
          fixture_gpt_entry_type(&device, 1U, type_guid) &&
          memcmp(type_guid, installer_layout_role_type_guid(
                     INSTALLER_LAYOUT_ROLE_BOOT_VOLUME), 16U) == 0,
          "the executed GPT carries the declared boot-volume type");
    CHECK(fixture_gpt_entry(&device, EXPECTED_BIOS_ROLE_COUNT - 1U,
                            &first_lba, &last_lba) &&
          first_lba ==
              plan.partitions[EXPECTED_BIOS_ROLE_COUNT - 1U].first_lba &&
          last_lba + 1U == summary.roles[
              EXPECTED_BIOS_ROLE_COUNT - 1U].first_lba +
              summary.roles[EXPECTED_BIOS_ROLE_COUNT - 1U].length_lba,
          "the executed GPT carries the fifth role's extent");
}

static void test_role_geometry(void)
{
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_contract_summary summary;
    struct installer_layout_contract_summary broken;
    uint64_t usable_end;
    uint64_t expected_first;
    size_t index;
    size_t other;

    fixture_init();
    CHECK(fixture_clean_report(
              &report, INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA),
          "geometry fixture report initializes");
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          installer_layout_contract_validate_summary(
              &report, 1U, &summary) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the contracted plan passes its own geometry validation");

    usable_end = summary.media_lba -
                 INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA;
    expected_first = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA;
    for (index = 0U; index < summary.role_count; ++index) {
        const struct installer_layout_contract_role *role =
            &summary.roles[index];

        CHECK(role->present &&
              (role->first_lba % INSTALLER_LAYOUT_ALIGN_LBA) == 0U &&
              role->first_lba == expected_first &&
              role->length_lba != 0U &&
              role->first_lba + role->length_lba <= usable_end,
              "every role is aligned and inside the GPT usable region");
        expected_first += role->length_lba;
        for (other = index + 1U; other < summary.role_count; ++other) {
            CHECK(role->first_lba + role->length_lba <=
                      summary.roles[other].first_lba,
                  "contracted roles do not overlap");
        }
    }
    CHECK(expected_first ==
              INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                  INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA &&
          summary.footprint_lba ==
              INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
          "the role chain adds up to the declared footprint");
    CHECK(summary.media_lba - usable_end ==
              INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA,
          "the GPT tail reserve matches the planner");

    broken = summary;
    broken.roles[1U].first_lba = broken.roles[0U].first_lba;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "overlapping roles are refused");
    broken = summary;
    broken.roles[2U].first_lba += 1U;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "a misaligned role is refused");
    broken = summary;
    broken.roles[3U].length_lba = usable_end;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "a role past the GPT usable region is refused");
    broken = summary;
    broken.media_lba = INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA - 1U;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL,
          "a summary below the minimum media is refused");
    broken = summary;
    broken.role_count = INSTALLER_LAYOUT_PARTITION_CAPACITY + 1U;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "a role count above the partition capacity is refused");
    broken = summary;
    broken.roles[3U].length_lba = UINT64_MAX - 1U;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "role arithmetic overflow is refused, not wrapped");
    broken = summary;
    broken.footprint_lba = 0U;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "a footprint that disagrees with the roles is refused");
    broken = summary;
    broken.roles[0U].present = false;
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &broken) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "an absent role is refused");
    CHECK(installer_layout_contract_validate_summary(
              NULL, 1U, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NULL &&
          installer_layout_contract_validate_summary(
              &report, 1U, NULL) == INSTALLER_LAYOUT_CONTRACT_NULL,
          "null validation inputs are refused");
}

static void test_dual_boot_reserve(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan dual_boot_plan;
    struct installer_layout_plan plan;
    struct installer_layout_contract_summary summary;
    uint64_t usable_end;

    fixture_init();
    memset(&requirements, 0, sizeof(requirements));
    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_dual_boot_requirements(
              &requirements) == INSTALLER_LAYOUT_CONTRACT_OK &&
          requirements.version ==
              INSTALLER_DUAL_BOOT_REQUIREMENTS_VERSION &&
          requirements.system_length_lba ==
              INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA &&
          requirements.persistence_length_lba ==
              INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA &&
          requirements.recovery_length_lba ==
              INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA,
          "dual-boot requirements come from the product sizes");

    CHECK(fixture_dual_boot_report(
              &report, FIXTURE_MEDIA_LBA,
              UINT64_C(2097152), UINT64_C(4194304)),
          "dual-boot fixture report initializes");
    CHECK(installer_layout_contract_plan_dual_boot(
              &report, 1U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK,
          "a disk with free space plans a dual-boot reservation");
    CHECK(dual_boot_plan.preserved_partition_count == 3U &&
          dual_boot_plan.existing_os_count == 2U &&
          dual_boot_plan.esp_partition_id == 1U &&
          !dual_boot_plan.destructive,
          "the existing system and its ESP are preserved");
    CHECK(summary.mode == INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT &&
          summary.esp_reused &&
          summary.existing_partition_count == 3U &&
          summary.existing_first_lba == 2048U,
          "the summary reports the preserved previous system");
    CHECK(fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_EFI_SYSTEM,
              2048U, 2048U) &&
          fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM,
              dual_boot_plan.system_first_lba,
              INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA) &&
          fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE,
              dual_boot_plan.persistence_first_lba,
              INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA) &&
          fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
              dual_boot_plan.recovery_first_lba,
              INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA),
          "the reserved roles carry the declared sizes at the free region");
    CHECK(summary.first_lba == dual_boot_plan.system_first_lba &&
          summary.footprint_lba ==
              INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA &&
          summary.declared_lba ==
              INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
          "a dual-boot plan allocates only the three HFOC roles");
    CHECK(summary.free_first_lba ==
              dual_boot_plan.recovery_first_lba +
                  INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA &&
          summary.free_length_lba ==
              dual_boot_plan.free_length_lba -
                  INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA,
          "the remainder after the reservation is reported");
    usable_end = FIXTURE_MEDIA_LBA - FIXTURE_GPT_TAIL_LBA;
    CHECK(summary.free_first_lba + summary.free_length_lba <= usable_end,
          "the remainder stays inside the GPT usable region");
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &summary) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the dual-boot reservation passes geometry validation");

    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_dual_boot_report(
            &report,
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA,
            UINT64_C(65536), UINT64_C(65536))) {
        CHECK(false, "tight dual-boot fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_dual_boot(
              &report, 1U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NO_RESERVE,
          "a dual-boot install that leaves the previous system no "
          "space is refused");
    CHECK(strcmp(installer_layout_contract_status_string(
                     INSTALLER_LAYOUT_CONTRACT_NO_RESERVE),
                 "INSTALLER_LAYOUT_CONTRACT_NO_RESERVE") == 0,
          "the reservation refusal has an exact status string");

    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_dual_boot_report(
            &report, FIXTURE_MEDIA_LBA, UINT64_C(2097152),
            (FIXTURE_MEDIA_LBA - FIXTURE_GPT_TAIL_LBA - 1024U) -
                (4096U + UINT64_C(2097152)))) {
        CHECK(false, "full dual-boot fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_dual_boot(
              &report, 1U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NO_RESERVE,
          "a previous system that fills the disk is refused");

    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_clean_report(&report, FIXTURE_MEDIA_LBA)) {
        CHECK(false, "clean fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_dual_boot(
              &report, 1U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED,
          "dual-boot planning on a disk with no existing system is "
          "refused");
    memset(&plan, 0, sizeof(plan));
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, NULL, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NULL &&
          installer_layout_contract_plan_clean(
              NULL, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NULL,
          "null planning inputs are refused");
}

static void test_publication_matches_plan(void)
{
    struct installer_discovery_report report;
    struct installer_layout_plan plan;
    struct installer_layout_snapshot snapshot;
    struct installer_layout_contract_summary summary;
    struct block_device device = fixture_device();
    struct installer_discovery_report rescan;
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    char preview[INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH];
    char expected[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    size_t record_length = 0U;
    size_t preview_length = 0U;
    uint64_t free_first;
    uint64_t free_length;
    uint64_t first_lba;
    uint64_t last_lba;
    size_t index;

    fixture_init();
    device = fixture_device();
    CHECK(fixture_clean_report(&report, FIXTURE_MEDIA_LBA),
          "publication fixture report initializes");
    memset(&plan, 0, sizeof(plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_clean(
              &report, 1U, &plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK,
          "publication fixture plan builds");
    CHECK(installer_layout_contract_render_record(
              &summary, record, sizeof(record), &record_length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          record_length != 0U &&
          record_length < sizeof(record) &&
          record[record_length - 1U] == '\n' &&
          strchr(record, '\0') != NULL,
          "the serial record renders as bounded text");

    free_first = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                 INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    free_length = FIXTURE_MEDIA_LBA - FIXTURE_GPT_TAIL_LBA - free_first;
    snprintf(expected, sizeof(expected),
             "[INSTALLER-LAYOUT] plan mode=clean-whole-disk disk=1 "
             "media_lba=%llu media_mib=%llu minimum_media_lba=%llu "
             "minimum_media_mib=%llu declared_lba=%llu declared_mib=%llu "
             "first_lba=%llu footprint_lba=%llu footprint_mib=%llu "
             "free_first_lba=%llu free_lba=%llu free_mib=%llu "
             "available_mib=%llu required_mib=%llu "
             "existing_partitions=0 existing_first_lba=0 existing_lba=0 "
             "esp_reused=0\n"
             "[INSTALLER-LAYOUT] role=efi-system first_lba=2048 "
             "length_lba=67584 length_mib=33 reused=0\n"
             "[INSTALLER-LAYOUT] role=hfoc-system first_lba=69632 "
             "length_lba=131072 length_mib=64 reused=0\n"
             "[INSTALLER-LAYOUT] role=hfoc-persistence first_lba=200704 "
             "length_lba=65536 length_mib=32 reused=0\n"
             "[INSTALLER-LAYOUT] role=hfoc-recovery first_lba=266240 "
             "length_lba=65536 length_mib=32 reused=0\n",
             (unsigned long long)FIXTURE_MEDIA_LBA,
             (unsigned long long)(FIXTURE_MEDIA_LBA / FIXTURE_MIB_LBA),
             (unsigned long long)
                 INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA,
             (unsigned long long)installer_layout_contract_mib(
                 INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA),
             (unsigned long long)
                 INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
             (unsigned long long)
                 installer_layout_contract_mib(
                     INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA),
             (unsigned long long)INSTALLER_LAYOUT_CONTRACT_FIRST_LBA,
             (unsigned long long)
                 INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA,
             (unsigned long long)
                 installer_layout_contract_mib(
                     INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA),
             (unsigned long long)free_first,
             (unsigned long long)free_length,
             (unsigned long long)(free_length / FIXTURE_MIB_LBA),
             (unsigned long long)installer_layout_contract_mib(
                 free_length + INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA),
             (unsigned long long)installer_layout_contract_mib(
                 INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA));
    CHECK(strcmp(record, expected) == 0,
          "the serial record carries the plan's own numbers");
    CHECK(strncmp(record, "[INSTALLER-LAYOUT] plan ", 24U) == 0,
          "the record uses the single-line marker style");

    CHECK(installer_layout_contract_render_preview(
              &summary, preview, sizeof(preview), &preview_length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
          preview_length != 0U &&
          preview_length < sizeof(preview) &&
          preview[0U] == ' ',
          "the plan preview renders as a bounded fragment");
    for (index = 0U; index < summary.role_count; ++index) {
        char role_line[128];

        snprintf(role_line, sizeof(role_line), "%s:%llu+%llu=",
                 installer_layout_contract_role_string(
                     summary.roles[index].role),
                 (unsigned long long)summary.roles[index].first_lba,
                 (unsigned long long)summary.roles[index].length_lba);
        CHECK(strstr(preview, role_line) != NULL,
              "the preview carries every role with its sectors");
    }
    CHECK(strstr(preview, "free_lba=") != NULL &&
          strstr(preview, "free_mib=") != NULL &&
          strstr(preview, "footprint_mib=161") != NULL &&
          strstr(preview, "minimum_media_lba=") != NULL,
          "the preview carries the footprint and the free space");

    CHECK(installer_layout_confirm(&plan) == INSTALLER_LAYOUT_OK &&
          installer_layout_execute(
              &device, &plan, &snapshot) == INSTALLER_LAYOUT_OK &&
          plan.executed && fixture_write_calls != 0U,
          "the contracted plan executes on the target disk");

    for (index = 0U; index < plan.partition_count; ++index) {
        CHECK(fixture_gpt_entry(&device, (uint32_t)index,
                                &first_lba, &last_lba) &&
              first_lba == plan.partitions[index].first_lba &&
              last_lba == plan.partitions[index].first_lba +
                             plan.partitions[index].length_lba - 1U &&
              first_lba == summary.roles[index].first_lba &&
              last_lba + 1U == summary.roles[index].first_lba +
                                   summary.roles[index].length_lba,
              "the executed GPT carries the published role ranges");
    }

    memset(&rescan, 0, sizeof(rescan));
    CHECK(installer_discovery_init(&rescan) == INSTALLER_DISCOVERY_OK &&
          installer_discovery_scan_block_device(
              &rescan, &device, 1U,
              INSTALLER_DISK_CONTROLLER_NVME, "nvme0", 5U) ==
              INSTALLER_DISCOVERY_OK,
          "the executed layout is readable by discovery");
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &rescan.partitions[index];

        if (!partition->present) {
            continue;
        }
        CHECK(partition->first_lba ==
                  summary.roles[partition->partition_index - 1U]
                      .first_lba &&
              partition->length_lba ==
                  summary.roles[partition->partition_index - 1U]
                      .length_lba,
              "the re-scanned partitions match the published roles");
    }
}

static void test_publication_refusals(void)
{
    struct installer_layout_contract_summary summary;
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    char preview[INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH];
    char small[8U];
    size_t length = 0U;

    memset(&summary, 0, sizeof(summary));
    summary.version = INSTALLER_LAYOUT_CONTRACT_VERSION;
    CHECK(installer_layout_contract_render_record(
              NULL, record, sizeof(record), &length) ==
              INSTALLER_LAYOUT_CONTRACT_NULL &&
          installer_layout_contract_render_preview(
              &summary, NULL, sizeof(preview), &length) ==
              INSTALLER_LAYOUT_CONTRACT_NULL,
          "null publication inputs are refused");
    summary.version = INSTALLER_LAYOUT_CONTRACT_VERSION + 1U;
    CHECK(installer_layout_contract_render_record(
              &summary, record, sizeof(record), &length) ==
              INSTALLER_LAYOUT_CONTRACT_BAD_VERSION,
          "an unknown contract version is refused");
    summary.version = INSTALLER_LAYOUT_CONTRACT_VERSION;
    summary.mode = INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK;
    CHECK(installer_layout_contract_render_record(
              &summary, small, sizeof(small), &length) ==
              INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT,
          "an undersized publication buffer is refused");

    CHECK(strcmp(installer_layout_contract_status_string(
                     INSTALLER_LAYOUT_CONTRACT_OK),
                 "INSTALLER_LAYOUT_CONTRACT_OK") == 0 &&
          strcmp(installer_layout_contract_mode_string(
                     INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT),
                 "dual-boot") == 0 &&
          strcmp(installer_layout_contract_role_string(
                     INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE),
                 "hfoc-persistence") == 0 &&
          installer_layout_contract_role_string(
              INSTALLER_LAYOUT_ROLE_UNKNOWN) == NULL &&
          installer_layout_contract_status_string(
              (enum installer_layout_contract_status)999U) == NULL,
          "status, mode and role strings are exact");
}

static void test_release_reserve(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_plan dual_boot_plan;
    struct installer_layout_contract_summary summary;
    char record[INSTALLER_LAYOUT_CONTRACT_RECORD_LENGTH];
    char preview[INSTALLER_LAYOUT_CONTRACT_PREVIEW_LENGTH];
    size_t length = 0U;

    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_dual_boot_report(
            &report, FIXTURE_MEDIA_LBA, UINT64_C(2097152),
            UINT64_C(4194304))) {
        CHECK(false, "release fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_release(
              &report, 1U, 2U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_OK,
          "the named partition is given up and the roles fit inside it");
    CHECK(summary.mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE &&
              summary.released_index == 2U &&
              summary.released_first_lba == 4096U &&
              summary.released_length_lba == UINT64_C(2097152) &&
              summary.esp_reused &&
              summary.first_lba == 4096U &&
              summary.footprint_lba ==
                  INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA,
          "the release summary names the partition and reuses the ESP");
    CHECK(summary.available_lba == UINT64_C(2097152) &&
              summary.required_lba ==
                  INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA,
          "the summary carries the room available and the room needed");
    CHECK(summary.free_first_lba ==
              4096U +
                  INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA &&
              summary.free_first_lba + summary.free_length_lba ==
                  4096U + UINT64_C(2097152),
          "the remainder is the rest of the given-up partition");
    CHECK(fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_EFI_SYSTEM, 2048U, 2048U) &&
          fixture_role_equals(
              &summary, INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM,
              dual_boot_plan.system_first_lba,
              INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA),
          "the release plan keeps the ESP and creates the three roles");
    CHECK(installer_layout_contract_validate_summary(
              &report, 1U, &summary) == INSTALLER_LAYOUT_CONTRACT_OK,
          "the release summary passes geometry validation");
    CHECK(installer_layout_contract_render_record(
              &summary, record, sizeof(record), &length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
              strstr(record, "plan mode=release") != NULL &&
              strstr(record, " released_index=2 released_first_lba=4096 "
                             "released_lba=2097152") != NULL &&
              strstr(record, "available_mib=1024 required_mib=128") != NULL,
          "the record names the released partition and the two numbers");
    CHECK(installer_layout_contract_render_preview(
              &summary, preview, sizeof(preview), &length) ==
              INSTALLER_LAYOUT_CONTRACT_OK &&
              strstr(preview, "mode=release") != NULL &&
              strstr(preview, "released_index=2") != NULL,
          "the preview names the released partition");

    /* The shared ESP is not a partition the operator may give up. */
    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    CHECK(installer_layout_contract_plan_release(
              &report, 1U, 1U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED,
          "the EFI System Partition cannot be released");
    /* A partition smaller than the three roles is refused with numbers. */
    memset(&dual_boot_plan, 0, sizeof(dual_boot_plan));
    memset(&summary, 0, sizeof(summary));
    if (!fixture_dual_boot_report(
            &report, FIXTURE_MEDIA_LBA, UINT64_C(65536),
            UINT64_C(4194304))) {
        CHECK(false, "small release fixture report initializes");
        return;
    }
    CHECK(installer_layout_contract_plan_release(
              &report, 1U, 2U, &dual_boot_plan, &summary) ==
              INSTALLER_LAYOUT_CONTRACT_NO_RESERVE,
          "a partition too small for the roles is refused");
}

int main(void)
{
    test_declared_arithmetic();
    test_minimum_media_boundary();
    test_bios_clean_plan();
    test_role_geometry();
    test_dual_boot_reserve();
    test_release_reserve();
    test_publication_matches_plan();
    test_publication_refusals();

    if (failures != 0) {
        fprintf(stderr,
                "Installer layout contract host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Installer layout contract host tests: ALL OK");
    return 0;
}

#endif
