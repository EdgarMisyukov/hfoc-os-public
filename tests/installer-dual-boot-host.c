#include "installer_dual_boot.h"

#include "installer_hardware_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FIXTURE_BLOCK_COUNT 32768U
#define FIXTURE_LARGE_BLOCK_COUNT 16384U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool fixture_requirements(
    struct installer_dual_boot_requirements *requirements)
{
    if (requirements == NULL) {
        return false;
    }
    memset(requirements, 0, sizeof(*requirements));
    requirements->version =
        INSTALLER_DUAL_BOOT_REQUIREMENTS_VERSION;
    requirements->system_length_lba = 2048U;
    requirements->persistence_length_lba = 2048U;
    requirements->recovery_length_lba = 2048U;
    return true;
}

static bool fixture_init_report(
    struct installer_discovery_report *report,
    enum installer_firmware_mode firmware,
    uint32_t disk_id,
    const char *identity,
    uint64_t block_count)
{
    struct installer_discovery_disk disk;

    if (report == NULL || identity == NULL ||
        installer_discovery_init(report) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_firmware(
            report, firmware) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_cpu(
            report, 1U) != INSTALLER_DISCOVERY_OK ||
        installer_discovery_publish_memory(
            report, 536870912U) != INSTALLER_DISCOVERY_OK) {
        return false;
    }
    memset(&disk, 0, sizeof(disk));
    disk.id = disk_id;
    disk.present = true;
    disk.controller = INSTALLER_DISK_CONTROLLER_NVME;
    disk.partition_table = INSTALLER_PARTITION_TABLE_GPT;
    disk.block_size = 512U;
    disk.block_count = block_count;
    snprintf(disk.identity, sizeof(disk.identity), "%s", identity);
    return installer_discovery_publish_disk(report, &disk) ==
           INSTALLER_DISCOVERY_OK;
}

static bool fixture_add_disk(
    struct installer_discovery_report *report,
    uint32_t disk_id,
    const char *identity,
    uint64_t block_count)
{
    struct installer_discovery_disk disk;

    memset(&disk, 0, sizeof(disk));
    disk.id = disk_id;
    disk.present = true;
    disk.controller = INSTALLER_DISK_CONTROLLER_NVME;
    disk.partition_table = INSTALLER_PARTITION_TABLE_GPT;
    disk.block_size = 512U;
    disk.block_count = block_count;
    snprintf(disk.identity, sizeof(disk.identity), "%s", identity);
    return installer_discovery_publish_disk(report, &disk) ==
           INSTALLER_DISCOVERY_OK;
}

static bool fixture_add_partition(
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

static bool fixture_add_os(
    struct installer_discovery_report *report,
    uint32_t os_id,
    enum installer_os_kind kind,
    uint32_t partition_id,
    bool has_efi_entry,
    const char *boot_path)
{
    struct installer_discovery_os os;

    memset(&os, 0, sizeof(os));
    os.id = os_id;
    os.present = true;
    os.kind = kind;
    os.partition_id = partition_id;
    os.has_efi_entry = has_efi_entry;
    if (boot_path != NULL) {
        snprintf(os.boot_path, sizeof(os.boot_path), "%s", boot_path);
    }
    return installer_discovery_publish_os(report, &os) ==
           INSTALLER_DISCOVERY_OK;
}

static bool fixture_finalize(
    struct installer_discovery_report *report)
{
    return installer_discovery_finalize(report) ==
               INSTALLER_DISCOVERY_OK &&
           installer_discovery_validate(report) ==
               INSTALLER_DISCOVERY_OK;
}

static bool fixture_linux_windows_report(
    struct installer_discovery_report *report,
    bool publish_boot_paths)
{
    if (!fixture_init_report(
            report, INSTALLER_FIRMWARE_UEFI, 1U,
            "nvme0", FIXTURE_BLOCK_COUNT) ||
        !fixture_add_partition(
            report, 1U, 1U, 1U,
            INSTALLER_PARTITION_EFI_SYSTEM, true,
            2048U, 2048U) ||
        !fixture_add_partition(
            report, 2U, 1U, 2U,
            INSTALLER_PARTITION_LINUX_ROOT, false,
            4096U, 4096U) ||
        !fixture_add_partition(
            report, 3U, 1U, 3U,
            INSTALLER_PARTITION_WINDOWS, false,
            8192U, 4096U)) {
        return false;
    }
    if (publish_boot_paths &&
        (!fixture_add_os(
             report, 1U, INSTALLER_OS_LINUX, 2U, false,
             "\\EFI\\debian\\grubx64.efi") ||
         !fixture_add_os(
             report, 2U, INSTALLER_OS_WINDOWS, 3U, false,
             "\\EFI\\Microsoft\\Boot\\bootmgfw.efi"))) {
        return false;
    }
    return fixture_finalize(report);
}

static bool fixture_two_linux_disks_report(
    struct installer_discovery_report *report)
{
    if (!fixture_init_report(
            report, INSTALLER_FIRMWARE_UEFI, 1U,
            "nvme0", FIXTURE_BLOCK_COUNT) ||
        !fixture_add_disk(
            report, 2U, "nvme1", FIXTURE_BLOCK_COUNT) ||
        !fixture_add_partition(
            report, 1U, 1U, 1U,
            INSTALLER_PARTITION_EFI_SYSTEM, true,
            2048U, 2048U) ||
        !fixture_add_partition(
            report, 2U, 1U, 2U,
            INSTALLER_PARTITION_LINUX_ROOT, false,
            4096U, 4096U) ||
        !fixture_add_partition(
            report, 5U, 2U, 1U,
            INSTALLER_PARTITION_EFI_SYSTEM, true,
            2048U, 2048U) ||
        !fixture_add_partition(
            report, 6U, 2U, 2U,
            INSTALLER_PARTITION_LINUX_ROOT, false,
            4096U, 4096U) ||
        !fixture_add_os(
            report, 1U, INSTALLER_OS_LINUX, 2U, false,
            "\\EFI\\debian\\grubx64.efi") ||
        !fixture_add_os(
            report, 2U, INSTALLER_OS_LINUX, 6U, false,
            "\\EFI\\debian\\grubx64.efi")) {
        return false;
    }
    return fixture_finalize(report);
}

static bool fixture_esp_only_report(
    struct installer_discovery_report *report)
{
    if (!fixture_init_report(
            report, INSTALLER_FIRMWARE_UEFI, 1U,
            "nvme-esp", FIXTURE_BLOCK_COUNT) ||
        !fixture_add_partition(
            report, 1U, 1U, 1U,
            INSTALLER_PARTITION_EFI_SYSTEM, true,
            2048U, 2048U)) {
        return false;
    }
    return fixture_finalize(report);
}

static bool fixture_report_snapshot(
    const struct installer_discovery_report *report,
    unsigned char *snapshot,
    size_t capacity)
{
    if (capacity < sizeof(*report)) {
        return false;
    }
    memcpy(snapshot, report, sizeof(*report));
    return true;
}

static bool fixture_reports_equal(
    const unsigned char *left,
    const unsigned char *right)
{
    return memcmp(left, right, sizeof(struct installer_discovery_report))
        == 0;
}

static void test_status_strings(void)
{
    CHECK(strcmp(installer_dual_boot_status_string(
                     INSTALLER_DUAL_BOOT_OK),
                 "INSTALLER_DUAL_BOOT_OK") == 0,
          "dual boot OK string is exact");
    CHECK(installer_dual_boot_status_string(
              (enum installer_dual_boot_status)999U) == NULL,
          "dual boot undefined status is rejected");
}

static void test_linux_windows_plan_and_menu(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;
    struct installer_dual_boot_preview preview;
    struct installer_dual_boot_menu menu;
    unsigned char before[sizeof(report)];
    char render[INSTALLER_DUAL_BOOT_RENDER_LENGTH];
    size_t index;

    CHECK(fixture_linux_windows_report(&report, true),
          "Linux/Windows dual-boot fixture report initializes");
    CHECK(fixture_requirements(&requirements),
          "dual-boot requirements fixture initializes");
    CHECK(fixture_report_snapshot(&report, before, sizeof(before)),
          "dual-boot fixture report snapshot initializes");
    CHECK(installer_dual_boot_plan_selected(
              &report, 1U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_OK &&
              plan.disk_id == 1U &&
              plan.esp_partition_id == 1U &&
              plan.existing_os_count == 2U &&
              plan.preserved_partition_count == 3U &&
              !plan.destructive &&
              !plan.confirmed,
          "selected dual-boot plan preserves Linux/Windows/ESP evidence");
    CHECK(fixture_reports_equal(
              before, (const unsigned char *)&report),
          "dual-boot planning does not mutate discovery report");
    CHECK(installer_dual_boot_preview(&plan, &preview) ==
              INSTALLER_DUAL_BOOT_OK &&
              preview.preserved_partition_count == 3U &&
              preview.added_partition_count == 3U &&
              strstr(preview.text, "nvme0") != NULL &&
              strstr(preview.text, "preserve") != NULL,
          "dual-boot preview preserves partition summary");
    CHECK(installer_dual_boot_confirm(&plan) ==
              INSTALLER_DUAL_BOOT_OK &&
              plan.confirmed,
          "dual-boot plan confirmation succeeds");
    CHECK(installer_dual_boot_menu_build(
              &report,
              "\\EFI\\HFOC\\BOOTX64.EFI",
              "\\EFI\\HFOC\\RECOVERY.EFI",
              &menu) == INSTALLER_DUAL_BOOT_OK &&
              menu.entry_count == 4U,
          "dual-boot menu includes four entries");
    for (index = 0U; index < menu.entry_count; ++index) {
        if (!menu.entries[index].present) {
            continue;
        }
        if (menu.entries[index].kind == INSTALLER_DUAL_BOOT_ENTRY_LINUX) {
            CHECK(strcmp(menu.entries[index].label, "Linux") == 0,
                  "Linux menu label is exact");
        }
        if (menu.entries[index].kind == INSTALLER_DUAL_BOOT_ENTRY_WINDOWS) {
            CHECK(strcmp(
                      menu.entries[index].label,
                      "Windows Boot Manager") == 0,
                  "Windows menu label is exact");
        }
    }
    CHECK(installer_dual_boot_menu_render(
              &menu, render, sizeof(render)) ==
              INSTALLER_DUAL_BOOT_OK &&
              strstr(render, "HFOC OS") != NULL &&
              strstr(render, "Linux") != NULL &&
              strstr(render, "Windows Boot Manager") != NULL &&
              strstr(render, "Recovery") != NULL,
          "dual-boot menu renders all expected entries");
}

static void test_automatic_target_is_ambiguous(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;

    CHECK(fixture_two_linux_disks_report(&report),
          "two eligible automatic targets fixture initializes");
    CHECK(fixture_requirements(&requirements),
          "automatic ambiguity requirements initialize");
    CHECK(installer_dual_boot_plan_automatic(
              &report, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_AMBIGUOUS,
          "multiple automatic targets fail closed as ambiguous");
}

static void test_no_existing_os(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;

    CHECK(fixture_esp_only_report(&report),
          "ESP-only fixture initializes");
    CHECK(fixture_requirements(&requirements),
          "no-existing-OS requirements initialize");
    CHECK(installer_dual_boot_plan_automatic(
              &report, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_NO_EXISTING_OS,
          "ESP-only disk cannot be automatic dual-boot target");
}

static void test_unsafe_firmware_and_missing_esp(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;

    CHECK(fixture_init_report(
              &report, INSTALLER_FIRMWARE_BIOS_LEGACY, 1U,
              "legacy", FIXTURE_BLOCK_COUNT) &&
          fixture_add_partition(
              &report, 1U, 1U, 1U,
              INSTALLER_PARTITION_LINUX_ROOT, false,
              2048U, 4096U) &&
          fixture_finalize(&report),
          "legacy MBR fixture initializes");
    CHECK(fixture_requirements(&requirements),
          "legacy requirements initialize");
    CHECK(installer_dual_boot_plan_selected(
              &report, 1U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE,
          "legacy firmware is not an automatic dual-boot target");

    CHECK(fixture_init_report(
              &report, INSTALLER_FIRMWARE_UEFI, 1U,
              "no-esp", FIXTURE_BLOCK_COUNT) &&
          fixture_add_partition(
              &report, 1U, 1U, 1U,
              INSTALLER_PARTITION_LINUX_ROOT, false,
              4096U, 4096U) &&
          fixture_finalize(&report),
          "missing ESP fixture initializes");
    CHECK(installer_dual_boot_plan_selected(
              &report, 1U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION,
          "missing ESP fails closed");
}

static void test_unknown_boot_path_and_insufficient_space(void)
{
    struct installer_discovery_report report;
    struct installer_discovery_report small;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;
    struct installer_dual_boot_menu menu;

    CHECK(fixture_linux_windows_report(&report, false),
          "unknown-path fixture initializes");
    CHECK(installer_dual_boot_menu_build(
              &report,
              "\\EFI\\HFOC\\BOOTX64.EFI",
              "\\EFI\\HFOC\\RECOVERY.EFI",
              &menu) == INSTALLER_DUAL_BOOT_UNKNOWN_BOOT_PATH,
          "unknown existing-OS boot path fails closed");

    CHECK(fixture_init_report(
              &small, INSTALLER_FIRMWARE_UEFI, 1U,
              "small", FIXTURE_LARGE_BLOCK_COUNT) &&
          fixture_add_partition(
              &small, 1U, 1U, 1U,
              INSTALLER_PARTITION_EFI_SYSTEM, true,
              2048U, 2048U) &&
          fixture_add_partition(
              &small, 2U, 1U, 2U,
              INSTALLER_PARTITION_LINUX_ROOT, false,
              4096U, 8192U) &&
          fixture_add_os(
              &small, 1U, INSTALLER_OS_LINUX, 2U, false,
              "\\EFI\\debian\\grubx64.efi") &&
          fixture_finalize(&small),
          "insufficient-space fixture initializes");
    CHECK(fixture_requirements(&requirements),
          "insufficient-space requirements initialize");
    CHECK(installer_dual_boot_plan_selected(
              &small, 1U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE,
          "insufficient contiguous free space fails closed");
}

static bool fixture_release_report(
    struct installer_discovery_report *report,
    uint64_t released_length_lba)
{
    struct installer_discovery_partition partition;

    if (!fixture_init_report(
            report, INSTALLER_FIRMWARE_UEFI, 1U,
            "nvme0", FIXTURE_BLOCK_COUNT) ||
        !fixture_add_partition(
            report, 1U, 1U, 1U,
            INSTALLER_PARTITION_EFI_SYSTEM, true,
            2048U, 2048U)) {
        return false;
    }
    /* The Fedora case: the Linux system lives in an LVM volume group. */
    memset(&partition, 0, sizeof(partition));
    partition.id = 2U;
    partition.present = true;
    partition.disk_id = 1U;
    partition.partition_index = 2U;
    partition.first_lba = 4096U;
    partition.length_lba = released_length_lba;
    partition.kind = INSTALLER_PARTITION_LINUX_ROOT;
    partition.layout = INSTALLER_PARTITION_LAYOUT_LVM;
    if (installer_discovery_publish_partition(report, &partition) !=
            INSTALLER_DISCOVERY_OK ||
        !fixture_add_os(
            report, 1U, INSTALLER_OS_LINUX, 2U, false,
            "\\EFI\\fedora\\grubx64.efi")) {
        return false;
    }
    return fixture_finalize(report);
}

static void test_release_plan(void)
{
    struct installer_discovery_report report;
    struct installer_dual_boot_requirements requirements;
    struct installer_dual_boot_plan plan;
    struct installer_dual_boot_preview preview;
    struct installer_discovery_partition partition;

    CHECK(fixture_release_report(&report, 8192U),
          "release fixture initializes");
    CHECK(fixture_requirements(&requirements),
          "release requirements initialize");
    CHECK(installer_dual_boot_plan_release(
              &report, 1U, 2U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_OK,
          "a named LVM partition can be given up");
    CHECK(plan.releases_partition &&
              plan.released_index == 2U &&
              plan.released_layout == INSTALLER_PARTITION_LAYOUT_LVM &&
              plan.system_first_lba == 4096U &&
              plan.recovery_first_lba + plan.recovery_length_lba <=
                  plan.released_first_lba + plan.released_length_lba &&
              plan.preserved_partition_count == 1U,
          "the release plan names the partition and keeps the rest");
    CHECK(installer_dual_boot_preview(&plan, &preview) ==
              INSTALLER_DUAL_BOOT_OK &&
              strstr(preview.text, " release ") != NULL &&
              strstr(preview.text, " released=2:4096:8192") != NULL,
          "the preview names what is given up");

    /* The shared ESP is never a candidate. */
    CHECK(installer_dual_boot_plan_release(
              &report, 1U, 1U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED,
          "the EFI System Partition is refused as a release target");
    /* A container is a table, not a partition. */
    memset(&partition, 0, sizeof(partition));
    partition.id = 9U;
    partition.present = true;
    partition.disk_id = 1U;
    partition.partition_index = 3U;
    partition.first_lba = 16384U;
    partition.length_lba = 8192U;
    partition.kind = INSTALLER_PARTITION_LINUX_ROOT;
    partition.container = true;
    partition.layout = INSTALLER_PARTITION_LAYOUT_EXTENDED;
    CHECK(installer_discovery_publish_partition(&report, &partition) ==
              INSTALLER_DISCOVERY_OK &&
              installer_discovery_finalize(&report) ==
                  INSTALLER_DISCOVERY_OK,
          "container fixture publishes");
    CHECK(installer_dual_boot_plan_release(
              &report, 1U, 3U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED,
          "an extended container is refused as a release target");
    /* A partition that is not there is not a target. */
    CHECK(installer_dual_boot_plan_release(
              &report, 1U, 7U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_INVALID_ARGUMENT,
          "an absent partition index is refused");
    /* A partition too small for the three roles is refused with numbers. */
    CHECK(fixture_release_report(&report, 2048U) &&
              fixture_requirements(&requirements),
          "small release fixture initializes");
    CHECK(installer_dual_boot_plan_release(
              &report, 1U, 2U, &requirements, &plan) ==
              INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE,
          "a partition smaller than the three roles is refused");
}

int main(void)
{
    test_status_strings();
    test_linux_windows_plan_and_menu();
    test_automatic_target_is_ambiguous();
    test_no_existing_os();
    test_unsafe_firmware_and_missing_esp();
    test_unknown_boot_path_and_insufficient_space();
    test_release_plan();
    if (failures != 0) {
        fprintf(stderr, "Installer dual boot host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer dual boot host test: OK\n");
    return 0;
}
