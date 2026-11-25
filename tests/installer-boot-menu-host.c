#include "installer_boot_menu.h"

#include "block_device.h"
#include "installer_gpt_preserve.h"
#include "installer_hardware_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_INSTALLER_BOOT_MENU_HOST_TEST

/*
 * ITEM-517, requirement 4: the generated menu on a legacy-BIOS machine.
 *
 * The scanner and builder are driven on the host with a synthetic FAT32 ESP,
 * so both firmware vectors run the real code:
 *
 *   * a UEFI report whose target disk carries a Debian loader is the vector
 *     that existed before this item; its configuration and record are pinned
 *     byte for byte and must not move;
 *   * a legacy-BIOS report builds the appliance entry without a refusal - no
 *     EFI application can be chainloaded by BIOS Limine, so the menu records
 *     every found system and disk as NO_ESP instead of offering an entry that
 *     would not boot.
 */

#define FIXTURE_SECTOR_SIZE 512U
#define FIXTURE_VOLUME_LBA UINT64_C(2048)
#define FIXTURE_VOLUME_SECTORS UINT64_C(4096)
#define FIXTURE_BLOCK_COUNT \
    (FIXTURE_VOLUME_LBA + FIXTURE_VOLUME_SECTORS)
#define FIXTURE_RESERVED_SECTORS 32U
#define FIXTURE_FAT_COUNT 2U
#define FIXTURE_SECTORS_PER_FAT 32U
#define FIXTURE_DATA_START 96U
#define FIXTURE_ROOT_CLUSTER 2U
#define FIXTURE_EFI_CLUSTER 3U
#define FIXTURE_DEBIAN_CLUSTER 4U
#define FIXTURE_FILE_CLUSTER 6U
#define FIXTURE_FILE_SIZE 13U

static unsigned char fixture_storage[
    FIXTURE_BLOCK_COUNT * FIXTURE_SECTOR_SIZE];
static size_t fixture_menu_device_calls;
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const unsigned char name_efi[11U] = {
    'E', 'F', 'I', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_debian[11U] = {
    'D', 'E', 'B', 'I', 'A', 'N', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_grub[11U] = {
    'G', 'R', 'U', 'B', 'X', '6', '4', ' ', 'E', 'F', 'I'
};
static const unsigned char name_dot[11U] = {
    '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};
static const unsigned char name_dotdot[11U] = {
    '.', '.', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' '
};

static void fixture_store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
}

static void fixture_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0U] = (unsigned char)(value & 0xFFU);
    bytes[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static unsigned char *fixture_volume_sector(uint64_t volume_lba)
{
    return fixture_storage +
           ((uint64_t)FIXTURE_VOLUME_LBA + volume_lba) *
               FIXTURE_SECTOR_SIZE;
}

static void fixture_fat_set(uint32_t cluster, uint32_t value)
{
    const uint64_t entry_offset = (uint64_t)cluster * 4U;
    const uint64_t lba = FIXTURE_RESERVED_SECTORS +
                         entry_offset / FIXTURE_SECTOR_SIZE;
    const size_t offset =
        (size_t)(entry_offset % FIXTURE_SECTOR_SIZE);
    unsigned char *entry = fixture_volume_sector(lba) + offset;

    entry[0U] = (unsigned char)(value & 0xFFU);
    entry[1U] = (unsigned char)((value >> 8U) & 0xFFU);
    entry[2U] = (unsigned char)((value >> 16U) & 0xFFU);
    entry[3U] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void fixture_dir_entry(
    uint64_t volume_lba,
    uint32_t slot,
    const unsigned char name[11U],
    uint8_t attributes,
    uint32_t first_cluster,
    uint32_t size)
{
    unsigned char *entry = fixture_volume_sector(volume_lba) +
                           (size_t)slot * 32U;

    memcpy(entry, name, 11U);
    entry[11U] = attributes;
    fixture_store_le16(entry + 20U,
        (uint16_t)(first_cluster >> 16U));
    fixture_store_le16(entry + 26U,
        (uint16_t)(first_cluster & 0xFFFFU));
    fixture_store_le32(entry + 28U, size);
}

static void fixture_init(void)
{
    unsigned char *boot;

    memset(fixture_storage, 0, sizeof(fixture_storage));
    fixture_menu_device_calls = 0U;

    boot = fixture_volume_sector(0U);
    fixture_store_le16(boot + 11U, FIXTURE_SECTOR_SIZE);
    boot[13U] = 1U;
    fixture_store_le16(boot + 14U, FIXTURE_RESERVED_SECTORS);
    boot[16U] = FIXTURE_FAT_COUNT;
    fixture_store_le32(boot + 32U, (uint32_t)FIXTURE_VOLUME_SECTORS);
    fixture_store_le32(boot + 36U, FIXTURE_SECTORS_PER_FAT);
    fixture_store_le32(boot + 44U, FIXTURE_ROOT_CLUSTER);
    boot[510U] = 0x55U;
    boot[511U] = 0xAAU;

    fixture_fat_set(0U, 0x0FFFFFF8U);
    fixture_fat_set(1U, 0x0FFFFFFFU);
    fixture_fat_set(FIXTURE_ROOT_CLUSTER, 0x0FFFFFFFU);
    fixture_fat_set(FIXTURE_EFI_CLUSTER, 0x0FFFFFFFU);
    fixture_fat_set(FIXTURE_DEBIAN_CLUSTER, 0x0FFFFFFFU);
    fixture_fat_set(FIXTURE_FILE_CLUSTER, 0x0FFFFFFFU);

    fixture_dir_entry(FIXTURE_DATA_START, 0U, name_efi, 0x10U,
                      FIXTURE_EFI_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 1U, 0U, name_dot, 0x10U,
                      FIXTURE_EFI_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 1U, 1U, name_dotdot, 0x10U,
                      FIXTURE_ROOT_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 1U, 2U, name_debian, 0x10U,
                      FIXTURE_DEBIAN_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 2U, 0U, name_dot, 0x10U,
                      FIXTURE_DEBIAN_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 2U, 1U, name_dotdot, 0x10U,
                      FIXTURE_EFI_CLUSTER, 0U);
    fixture_dir_entry(FIXTURE_DATA_START + 2U, 2U, name_grub, 0x20U,
                      FIXTURE_FILE_CLUSTER, FIXTURE_FILE_SIZE);
    memcpy(fixture_volume_sector(
               FIXTURE_DATA_START +
                   (FIXTURE_FILE_CLUSTER - FIXTURE_ROOT_CLUSTER)),
           "grub payload\n", FIXTURE_FILE_SIZE);
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
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device fixture_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = FIXTURE_SECTOR_SIZE;
    device.block_count = FIXTURE_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

/*
 * The scan asks for the block device of every disk that carries an ESP; the
 * BIOS vector asserts this callback is never reached.
 */
static struct block_device fixture_block_device;

static struct block_device *fixture_menu_device(
    void *context,
    uint32_t disk_id)
{
    size_t *calls = (size_t *)context;

    (void)disk_id;
    if (calls != NULL) {
        ++*calls;
    }
    return &fixture_block_device;
}

/* The synthetic FAT32 ESP only exists for disk 1's extent (2048+4096). */
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
    disk.controller = INSTALLER_DISK_CONTROLLER_SATA;
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
    partition.kind = kind;
    partition.efi_system = efi_system;
    partition.first_lba = first_lba;
    partition.length_lba = length_lba;
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

/*
 * The UEFI vector: the target disk carries an ESP with a Debian loader and a
 * Linux system on it.  This is the shape the menu has always been built from.
 */
static bool fixture_uefi_report(
    struct installer_discovery_report *report)
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
               report, 1U, "fixture-esp",
               INSTALLER_PARTITION_TABLE_GPT,
               UINT64_C(234375000)) &&
           fixture_publish_partition(
               report, 1U, 1U, 1U, INSTALLER_PARTITION_EFI_SYSTEM,
               true, FIXTURE_VOLUME_LBA, FIXTURE_VOLUME_SECTORS) &&
           fixture_publish_partition(
               report, 2U, 1U, 2U, INSTALLER_PARTITION_LINUX_ROOT,
               false, 69632U, 131072U) &&
           fixture_publish_os(
               report, 1U, INSTALLER_OS_LINUX, 2U) &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_validate(report) ==
               INSTALLER_DISCOVERY_OK;
}

/*
 * The BIOS vector: the target is a whole disk with no table at all, and the
 * machine's other disk carries an ESP and a Linux system.  BIOS Limine cannot
 * chainload that loader, so the menu must say so instead of offering it.
 */
static bool fixture_bios_report(
    struct installer_discovery_report *report)
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
               report, 1U, "fixture-bios-target",
               INSTALLER_PARTITION_TABLE_NONE,
               UINT64_C(234375000)) &&
           fixture_publish_disk(
               report, 2U, "fixture-debian",
               INSTALLER_PARTITION_TABLE_GPT,
               UINT64_C(234375000)) &&
           fixture_publish_partition(
               report, 1U, 2U, 1U, INSTALLER_PARTITION_EFI_SYSTEM,
               true, FIXTURE_VOLUME_LBA, FIXTURE_VOLUME_SECTORS) &&
           fixture_publish_partition(
               report, 2U, 2U, 2U, INSTALLER_PARTITION_LINUX_ROOT,
               false, 69632U, 131072U) &&
           fixture_publish_os(
               report, 1U, INSTALLER_OS_LINUX, 2U) &&
           installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_validate(report) ==
               INSTALLER_DISCOVERY_OK;
}

static void fixture_appliance(
    struct installer_boot_menu_entry_source *appliance,
    struct installer_boot_menu_entry_source *recovery)
{
    memset(appliance, 0, sizeof(*appliance));
    appliance->label = "HFOC OS";
    appliance->path = "boot():/boot/hfoc-kernel.elf";
    appliance->module_paths[0U] = "boot():/boot/hfoc-boot.man";
    appliance->module_paths[1U] = "boot():/boot/limine/limine.conf";
    appliance->module_count = 2U;
    appliance->cmdline = "hfoc.management=local-only";

    memset(recovery, 0, sizeof(*recovery));
    recovery->label = "HFOC recovery";
    recovery->path = "boot():/boot/hfoc-recovery.elf";
    recovery->module_count = 0U;
}

/*
 * ITEM-517: the legacy-BIOS appliance entry.  The flow names the sealed chain
 * configuration it stages beside the menu (`boot():/boot/hfoc/limine.conf`),
 * exactly as the UEFI flow names the sealed configuration staged at
 * `\boot\limine\limine.conf` - a BIOS install reads its menu from that path,
 * so the module has to be the second file.
 */
static void fixture_bios_appliance(
    struct installer_boot_menu_entry_source *appliance,
    struct installer_boot_menu_entry_source *recovery)
{
    fixture_appliance(appliance, recovery);
    appliance->module_paths[1U] = "boot():/boot/hfoc/limine.conf";
}

/*
 * ITEM-517: the UEFI vector, pinned byte for byte.  These two literals are the
 * configuration and record the menu produced before this item and must keep
 * producing after it.
 */
static const char expected_uefi_text[] =
    "timeout: 10\n"
    "\n"
    "/HFOC OS\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-kernel.elf\n"
    "    cmdline: hfoc.management=local-only\n"
    "    module_path: boot():/boot/hfoc-boot.man\n"
    "    module_path: boot():/boot/limine/limine.conf\n"
    "\n"
    "/Debian GNU/Linux\n"
    "    protocol: efi\n"
    "    path: boot():/EFI/DEBIAN/GRUBX64.EFI\n"
    "\n"
    "/HFOC recovery\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-recovery.elf\n";

static const char expected_uefi_record[] =
    "[INSTALLER-BOOT-MENU] menu firmware=uefi entries=3 loaders=1 "
    "missing=0 unreadable_disks=0\n"
    "[INSTALLER-BOOT-MENU] disk id=1 esp=2048+4096 volume=boot() "
    "scan=OK loaders=1\n"
    "[INSTALLER-BOOT-MENU] loader disk=1 vendor=DEBIAN "
    "path=\\EFI\\DEBIAN\\GRUBX64.EFI size=13\n"
    "[INSTALLER-BOOT-MENU] entry index=0 protocol=limine label=HFOC OS "
    "path=boot():/boot/hfoc-kernel.elf modules=2 "
    "cmdline=hfoc.management=local-only\n"
    "[INSTALLER-BOOT-MENU] entry index=1 protocol=efi "
    "label=Debian GNU/Linux path=boot():/EFI/DEBIAN/GRUBX64.EFI "
    "modules=0\n"
    "[INSTALLER-BOOT-MENU] entry index=2 protocol=limine "
    "label=HFOC recovery path=boot():/boot/hfoc-recovery.elf modules=0\n";

static void test_the_uefi_vector_is_unchanged(void)
{
    struct installer_discovery_report report;
    struct installer_boot_menu_inventory inventory;
    struct installer_boot_menu menu;
    struct installer_boot_menu_entry_source appliance;
    struct installer_boot_menu_entry_source recovery;
    char record[INSTALLER_BOOT_MENU_RECORD_LENGTH];
    size_t length = 0U;

    fixture_init();
    fixture_block_device = fixture_device();
    fixture_appliance(&appliance, &recovery);
    if (!fixture_uefi_report(&report)) {
        CHECK(false, "the UEFI fixture report initializes");
        return;
    }
    CHECK(installer_boot_menu_scan(
              &report, 1U, fixture_menu_device,
              &fixture_menu_device_calls, &inventory) ==
              INSTALLER_BOOT_MENU_OK &&
          fixture_menu_device_calls == 1U &&
          inventory.disk_count == 1U &&
          inventory.disks[0U].scan == INSTALLER_BOOT_MENU_SCAN_OK &&
          inventory.disks[0U].loader_count == 1U &&
          strcmp(inventory.disks[0U].volume, "boot()") == 0,
          "the UEFI scan reads the target ESP and finds the Debian loader");
    CHECK(installer_boot_menu_build(
              &report, &inventory, &appliance, &recovery, &menu) ==
              INSTALLER_BOOT_MENU_OK &&
          menu.firmware_mode == INSTALLER_FIRMWARE_UEFI &&
          menu.entry_count == 3U && menu.loader_count == 1U &&
          menu.missing_count == 0U &&
          strcmp(menu.text, expected_uefi_text) == 0,
          "the UEFI configuration is byte for byte the pre-item vector");
    CHECK(installer_boot_menu_render_record(
              &menu, &inventory, record, sizeof(record), &length) ==
              INSTALLER_BOOT_MENU_OK &&
          strcmp(record, expected_uefi_record) == 0,
          "the UEFI record is byte for byte the pre-item vector");
}

/*
 * ITEM-517: the legacy-BIOS vector.  The appliance and recovery entries are
 * exactly the UEFI ones; the discovered system and both disks are named as
 * unofferable in comments instead of becoming EFI chainload entries.
 */
static const char expected_bios_text[] =
    "timeout: 10\n"
    "\n"
    "/HFOC OS\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-kernel.elf\n"
    "    cmdline: hfoc.management=local-only\n"
    "    module_path: boot():/boot/hfoc-boot.man\n"
    "    module_path: boot():/boot/hfoc/limine.conf\n"
    "\n"
    "/HFOC recovery\n"
    "    protocol: limine\n"
    "    path: boot():/boot/hfoc-recovery.elf\n"
    "# system Linux reason=NO_ESP\n"
    "# disk id=1 reason=NO_ESP\n"
    "# disk id=2 reason=NO_ESP\n";

static void test_the_bios_menu_is_honest_about_no_esp(void)
{
    struct installer_discovery_report report;
    struct installer_boot_menu_inventory inventory;
    struct installer_boot_menu menu;
    struct installer_boot_menu_entry_source appliance;
    struct installer_boot_menu_entry_source recovery;
    char record[INSTALLER_BOOT_MENU_RECORD_LENGTH];
    size_t length = 0U;

    fixture_init();
    fixture_block_device = fixture_device();
    fixture_bios_appliance(&appliance, &recovery);
    if (!fixture_bios_report(&report)) {
        CHECK(false, "the BIOS fixture report initializes");
        return;
    }
    CHECK(installer_boot_menu_scan(
              &report, 1U, fixture_menu_device,
              &fixture_menu_device_calls, &inventory) ==
              INSTALLER_BOOT_MENU_OK &&
          fixture_menu_device_calls == 0U &&
          inventory.disk_count == 2U,
          "a BIOS scan reads no ESP at all");
    CHECK(inventory.disks[0U].scan == INSTALLER_BOOT_MENU_SCAN_NO_ESP &&
          !inventory.disks[0U].esp_present &&
          strcmp(inventory.disks[0U].volume, "boot()") == 0 &&
          inventory.disks[0U].loader_count == 0U &&
          inventory.disks[1U].scan == INSTALLER_BOOT_MENU_SCAN_NO_ESP &&
          !inventory.disks[1U].esp_present &&
          inventory.disks[1U].loader_count == 0U,
          "every BIOS disk is recorded NO_ESP and the target volume is boot()");

    CHECK(installer_boot_menu_build(
              &report, &inventory, &appliance, &recovery, &menu) ==
              INSTALLER_BOOT_MENU_OK &&
          menu.firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY &&
          menu.entry_count == 2U && menu.loader_count == 0U &&
          menu.missing_count == 1U,
          "a BIOS menu builds without a refusal and counts the system it "
          "cannot offer");
    CHECK(strcmp(menu.text, expected_bios_text) == 0,
          "the BIOS configuration is the appliance and recovery entries plus "
          "the NO_ESP comments, byte for byte");
    CHECK(menu.entries[0U].present &&
          strcmp(menu.entries[0U].label, "HFOC OS") == 0 &&
          strcmp(menu.entries[0U].path,
                 "boot():/boot/hfoc-kernel.elf") == 0 &&
          menu.entries[0U].module_count == 2U &&
          strcmp(menu.entries[0U].module_paths[0U],
                 "boot():/boot/hfoc-boot.man") == 0 &&
          strcmp(menu.entries[0U].module_paths[1U],
                 "boot():/boot/hfoc/limine.conf") == 0 &&
          strcmp(menu.entries[0U].cmdline,
                 "hfoc.management=local-only") == 0,
          "the BIOS appliance entry names the chain configuration module and "
          "keeps its command line");
    CHECK(strstr(menu.text, "/HFOC OS\n    protocol: limine\n") != NULL &&
          strstr(menu.text, "    path: boot():/boot/hfoc-kernel.elf\n") !=
              NULL &&
          strstr(menu.text, "/HFOC recovery\n") != NULL,
          "the BIOS configuration carries the appliance and recovery entries");
    CHECK(strstr(menu.text, "# system Linux reason=NO_ESP\n") != NULL &&
          strstr(menu.text, "# disk id=1 reason=NO_ESP\n") != NULL &&
          strstr(menu.text, "# disk id=2 reason=NO_ESP\n") != NULL,
          "the BIOS configuration names every system and disk NO_ESP");
    CHECK(strstr(menu.text, "protocol: efi") == NULL &&
          strstr(menu.text, "/Debian") == NULL,
          "the BIOS configuration offers no EFI chainload entry");

    CHECK(installer_boot_menu_render_record(
              &menu, &inventory, record, sizeof(record), &length) ==
              INSTALLER_BOOT_MENU_OK &&
          strstr(record,
                 "[INSTALLER-BOOT-MENU] menu firmware=bios-legacy "
                 "entries=2 loaders=0 missing=1 unreadable_disks=0\n") ==
              record &&
          strstr(record,
                 "[INSTALLER-BOOT-MENU] disk id=1 esp=none volume=boot() "
                 "scan=NO_ESP loaders=0\n") != NULL &&
          strstr(record,
                 "[INSTALLER-BOOT-MENU] disk id=2 esp=none volume=none "
                 "scan=NO_ESP loaders=0\n") != NULL &&
          strstr(record, "protocol=efi") == NULL,
          "the BIOS record publishes firmware=bios-legacy and no EFI entry");
}

int main(void)
{
    test_the_uefi_vector_is_unchanged();
    test_the_bios_menu_is_honest_about_no_esp();

    if (failures != 0) {
        fprintf(stderr,
                "Installer boot menu host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Installer boot menu host tests: ALL OK");
    return 0;
}

#endif
