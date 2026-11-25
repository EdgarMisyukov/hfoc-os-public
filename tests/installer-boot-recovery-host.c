#include "installer_boot_recovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool fixture_evidence(
    struct installer_boot_recovery_evidence *evidence,
    bool recovery_media,
    bool previous_config,
    bool existing_os_preserved)
{
    if (evidence == NULL) {
        return false;
    }
    memset(evidence, 0, sizeof(*evidence));
    evidence->version = INSTALLER_BOOT_RECOVERY_VERSION;
    evidence->firmware_mode = INSTALLER_FIRMWARE_UEFI;
    evidence->disk_id = 1U;
    evidence->esp_partition_id = 1U;
    evidence->esp_present = true;
    evidence->hfoc_bootloader_present = false;
    evidence->hfoc_efi_entry_present = false;
    evidence->recovery_media_available = recovery_media;
    evidence->previous_boot_configuration_available = previous_config;
    evidence->existing_os_boot_paths_preserved = existing_os_preserved;
    return true;
}

static void test_status_strings(void)
{
    CHECK(strcmp(installer_boot_recovery_status_string(
                     INSTALLER_BOOT_RECOVERY_OK),
                 "INSTALLER_BOOT_RECOVERY_OK") == 0,
          "boot recovery OK string is exact");
    CHECK(installer_boot_recovery_status_string(
              (enum installer_boot_recovery_status)999U) == NULL,
          "boot recovery undefined status is rejected");
}

static void test_damaged_bootloader(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;
    struct installer_boot_recovery_preview preview;

    CHECK(fixture_evidence(&evidence, true, true, true),
          "damaged bootloader evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_OK &&
              plan.bootloader_mutation_required &&
              !plan.efi_entry_mutation_required &&
              !plan.confirmed,
          "damaged bootloader plan requires bootloader mutation");
    CHECK(installer_boot_recovery_preview(&plan, &preview) ==
              INSTALLER_BOOT_RECOVERY_OK &&
              strstr(preview.text, "recovery") != NULL,
          "damaged bootloader preview is published");
    CHECK(installer_boot_recovery_confirm(&plan) ==
              INSTALLER_BOOT_RECOVERY_OK &&
              plan.confirmed,
          "damaged bootloader plan confirms");
    CHECK(installer_boot_recovery_verify_outcome(
              &plan, true, false, true) ==
              INSTALLER_BOOT_RECOVERY_OK &&
              plan.complete,
          "damaged bootloader repair outcome validates");
}

static void test_lost_efi_entry(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, true, true, true),
          "lost EFI entry evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_LOST_HFOC_EFI_ENTRY,
              "\\EFI\\HFOC\\BOOTX64.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_OK &&
              plan.efi_entry_mutation_required &&
              !plan.bootloader_mutation_required,
          "lost EFI entry plan requires EFI entry mutation");
}

static void test_failed_update_without_previous_config(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, true, false, true),
          "failed update evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_FAILED_BOOTLOADER_UPDATE,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_PREVIOUS_CONFIG_UNAVAILABLE,
          "failed update without previous boot config fails closed");
}

static void test_unavailable_recovery_media(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, false, true, true),
          "unavailable media evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_HFOC_BOOT_RESTORATION,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_RECOVERY_MEDIA_UNAVAILABLE,
          "boot restoration without recovery media fails closed");
}

static void test_existing_os_unprotected(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, true, true, false),
          "unprotected existing OS evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_HFOC_BOOT_RESTORATION,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED,
          "unprotected existing OS fails closed");
}

static void test_incomplete_repair_outcome(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, true, true, true),
          "incomplete outcome evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_OK,
          "incomplete outcome plan initializes");
    CHECK(installer_boot_recovery_verify_outcome(
              &plan, false, false, true) ==
              INSTALLER_BOOT_RECOVERY_NOT_CONFIRMED,
          "unconfirmed repair outcome fails closed");
    CHECK(installer_boot_recovery_confirm(&plan) ==
              INSTALLER_BOOT_RECOVERY_OK,
          "incomplete outcome plan confirms");
    CHECK(installer_boot_recovery_verify_outcome(
              &plan, false, false, true) ==
              INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR,
          "incomplete repaired bootloader fails closed");
}

static void test_invalid_scenario_and_firmware(void)
{
    struct installer_boot_recovery_evidence evidence;
    struct installer_boot_recovery_plan plan;

    CHECK(fixture_evidence(&evidence, true, true, true),
          "invalid scenario evidence initializes");
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_UNKNOWN,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT,
          "unknown recovery scenario is rejected");
    evidence.firmware_mode = INSTALLER_FIRMWARE_BIOS_LEGACY;
    CHECK(installer_boot_recovery_assess(
              &evidence,
              INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER,
              "\\EFI\\HFOC\\RECOVERY.EFI",
              "nvme0", &plan) ==
              INSTALLER_BOOT_RECOVERY_UNSUPPORTED_FIRMWARE,
          "legacy recovery is not claimed as supported");
}

int main(void)
{
    test_status_strings();
    test_damaged_bootloader();
    test_lost_efi_entry();
    test_failed_update_without_previous_config();
    test_unavailable_recovery_media();
    test_existing_os_unprotected();
    test_incomplete_repair_outcome();
    test_invalid_scenario_and_firmware();
    if (failures != 0) {
        fprintf(stderr, "Installer boot recovery host test: %d failures\n",
                failures);
        return 1;
    }
    printf("Installer boot recovery host test: OK\n");
    return 0;
}
