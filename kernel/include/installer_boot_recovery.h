#ifndef HFOC_INSTALLER_BOOT_RECOVERY_H
#define HFOC_INSTALLER_BOOT_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#include "installer_hardware_discovery.h"

#define INSTALLER_BOOT_RECOVERY_VERSION 1U
#define INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH 32U
#define INSTALLER_BOOT_RECOVERY_PATH_LENGTH 80U
#define INSTALLER_BOOT_RECOVERY_PREVIEW_LENGTH 768U

enum installer_boot_recovery_status {
    INSTALLER_BOOT_RECOVERY_OK = 0,
    INSTALLER_BOOT_RECOVERY_NULL = 1,
    INSTALLER_BOOT_RECOVERY_BAD_VERSION = 2,
    INSTALLER_BOOT_RECOVERY_BAD_STATE = 3,
    INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT = 4,
    INSTALLER_BOOT_RECOVERY_UNSUPPORTED_FIRMWARE = 5,
    INSTALLER_BOOT_RECOVERY_MISSING_ESP = 6,
    INSTALLER_BOOT_RECOVERY_RECOVERY_MEDIA_UNAVAILABLE = 7,
    INSTALLER_BOOT_RECOVERY_PREVIOUS_CONFIG_UNAVAILABLE = 8,
    INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED = 9,
    INSTALLER_BOOT_RECOVERY_NOT_CONFIRMED = 10,
    INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR = 11,
};

enum installer_boot_recovery_scenario {
    INSTALLER_BOOT_RECOVERY_SCENARIO_UNKNOWN = 0,
    INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER = 1,
    INSTALLER_BOOT_RECOVERY_SCENARIO_LOST_HFOC_EFI_ENTRY = 2,
    INSTALLER_BOOT_RECOVERY_SCENARIO_FAILED_BOOTLOADER_UPDATE = 3,
    INSTALLER_BOOT_RECOVERY_SCENARIO_HFOC_BOOT_RESTORATION = 4,
    INSTALLER_BOOT_RECOVERY_SCENARIO_EXISTING_OS_PRESERVATION = 5,
};

struct installer_boot_recovery_evidence {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    uint32_t disk_id;
    uint32_t esp_partition_id;
    bool esp_present;
    bool hfoc_bootloader_present;
    bool hfoc_efi_entry_present;
    bool recovery_media_available;
    bool previous_boot_configuration_available;
    bool existing_os_boot_paths_preserved;
};

struct installer_boot_recovery_plan {
    uint32_t version;
    enum installer_boot_recovery_scenario scenario;
    uint32_t disk_id;
    uint32_t esp_partition_id;
    bool bootloader_mutation_required;
    bool efi_entry_mutation_required;
    bool recovery_media_required;
    bool previous_configuration_required;
    bool existing_os_preserved;
    bool confirmed;
    bool complete;
    char identity[INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH];
    char recovery_path[INSTALLER_BOOT_RECOVERY_PATH_LENGTH];
};

struct installer_boot_recovery_preview {
    uint32_t version;
    enum installer_boot_recovery_scenario scenario;
    uint32_t disk_id;
    uint32_t esp_partition_id;
    bool bootloader_mutation_required;
    bool efi_entry_mutation_required;
    bool existing_os_preserved;
    char text[INSTALLER_BOOT_RECOVERY_PREVIEW_LENGTH];
};

enum installer_boot_recovery_status installer_boot_recovery_assess(
    const struct installer_boot_recovery_evidence *evidence,
    enum installer_boot_recovery_scenario scenario,
    const char *recovery_path,
    const char *identity,
    struct installer_boot_recovery_plan *plan);
enum installer_boot_recovery_status installer_boot_recovery_preview(
    const struct installer_boot_recovery_plan *plan,
    struct installer_boot_recovery_preview *preview);
enum installer_boot_recovery_status installer_boot_recovery_confirm(
    struct installer_boot_recovery_plan *plan);
enum installer_boot_recovery_status installer_boot_recovery_verify_outcome(
    struct installer_boot_recovery_plan *plan,
    bool hfoc_bootloader_present,
    bool hfoc_efi_entry_present,
    bool existing_os_boot_paths_preserved);
const char *installer_boot_recovery_status_string(
    enum installer_boot_recovery_status status);

#endif
