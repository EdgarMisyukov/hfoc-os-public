#ifndef HFOC_INSTALLER_DUAL_BOOT_H
#define HFOC_INSTALLER_DUAL_BOOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "installer_hardware_discovery.h"

#define INSTALLER_DUAL_BOOT_VERSION 1U
#define INSTALLER_DUAL_BOOT_REQUIREMENTS_VERSION 1U
#define INSTALLER_DUAL_BOOT_ALIGN_LBA 2048U
#define INSTALLER_DUAL_BOOT_RESERVED_LBA 34U
#define INSTALLER_DUAL_BOOT_PREVIEW_LENGTH 768U
#define INSTALLER_DUAL_BOOT_RENDER_LENGTH 1024U
#define INSTALLER_DUAL_BOOT_MENU_CAPACITY 6U
#define INSTALLER_DUAL_BOOT_LABEL_LENGTH 48U
#define INSTALLER_DUAL_BOOT_PATH_LENGTH 80U

enum installer_dual_boot_status {
    INSTALLER_DUAL_BOOT_OK = 0,
    INSTALLER_DUAL_BOOT_NULL = 1,
    INSTALLER_DUAL_BOOT_BAD_STATE = 2,
    INSTALLER_DUAL_BOOT_BAD_VERSION = 3,
    INSTALLER_DUAL_BOOT_INVALID_ARGUMENT = 4,
    INSTALLER_DUAL_BOOT_OUT_OF_RANGE = 5,
    INSTALLER_DUAL_BOOT_AMBIGUOUS = 6,
    INSTALLER_DUAL_BOOT_CAPACITY = 7,
    INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE = 8,
    INSTALLER_DUAL_BOOT_NO_EXISTING_OS = 9,
    INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION = 10,
    INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE = 11,
    INSTALLER_DUAL_BOOT_UNKNOWN_BOOT_PATH = 12,
    INSTALLER_DUAL_BOOT_READ_ONLY_VIOLATION = 13,
    /*
     * ITEM-501: the partition the operator named to give up cannot be used -
     * it is not on the target disk, it is the shared EFI System Partition, or
     * it is a container rather than a partition.  Nothing is planned and
     * nothing is written.
     */
    INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED = 14,
};

enum installer_dual_boot_entry_kind {
    INSTALLER_DUAL_BOOT_ENTRY_UNKNOWN = 0,
    INSTALLER_DUAL_BOOT_ENTRY_HFOC_OS = 1,
    INSTALLER_DUAL_BOOT_ENTRY_LINUX = 2,
    INSTALLER_DUAL_BOOT_ENTRY_WINDOWS = 3,
    INSTALLER_DUAL_BOOT_ENTRY_OTHER = 4,
    INSTALLER_DUAL_BOOT_ENTRY_RECOVERY = 5,
};

struct installer_dual_boot_requirements {
    uint32_t version;
    uint64_t system_length_lba;
    uint64_t persistence_length_lba;
    uint64_t recovery_length_lba;
};

struct installer_dual_boot_plan {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    uint32_t disk_id;
    uint32_t esp_partition_id;
    uint32_t existing_os_count;
    uint32_t preserved_partition_count;
    uint64_t system_first_lba;
    uint64_t system_length_lba;
    uint64_t persistence_first_lba;
    uint64_t persistence_length_lba;
    uint64_t recovery_first_lba;
    uint64_t recovery_length_lba;
    uint64_t free_first_lba;
    uint64_t free_length_lba;
    bool automatic;
    bool destructive;
    bool confirmed;
    /*
     * ITEM-501: the "give the selected partition" path.  Instead of placing the
     * three appliance roles into the largest free region, the plan gives up one
     * existing partition the operator named and places them in the extent that
     * partition occupied.  Every other partition, including the shared EFI
     * System Partition, stays untouched, and `gpt_preserve` removes exactly the
     * released entry.  `released_index` is the discovery's own partition_index
     * (one-based), `released_partition_id` its discovery id.
     */
    bool releases_partition;
    uint32_t released_index;
    uint32_t released_partition_id;
    enum installer_partition_layout released_layout;
    enum installer_os_kind released_owner;
    uint64_t released_first_lba;
    uint64_t released_length_lba;
    char identity[INSTALLER_DISCOVERY_IDENTITY_LENGTH];
};

struct installer_dual_boot_preview {
    uint32_t version;
    uint32_t disk_id;
    enum installer_firmware_mode firmware_mode;
    uint32_t existing_os_count;
    uint32_t preserved_partition_count;
    uint32_t added_partition_count;
    bool destructive;
    char text[INSTALLER_DUAL_BOOT_PREVIEW_LENGTH];
};

struct installer_dual_boot_menu_entry {
    uint32_t id;
    bool present;
    enum installer_dual_boot_entry_kind kind;
    enum installer_os_kind os_kind;
    uint32_t os_id;
    uint32_t partition_id;
    bool has_efi_entry;
    char label[INSTALLER_DUAL_BOOT_LABEL_LENGTH];
    char boot_path[INSTALLER_DUAL_BOOT_PATH_LENGTH];
};

struct installer_dual_boot_menu {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    uint32_t entry_count;
    struct installer_dual_boot_menu_entry
        entries[INSTALLER_DUAL_BOOT_MENU_CAPACITY];
};

enum installer_dual_boot_status installer_dual_boot_plan_automatic(
    const struct installer_discovery_report *report,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan);
enum installer_dual_boot_status installer_dual_boot_plan_selected(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan);
/*
 * ITEM-501: plan the install that gives up one existing partition instead of
 * looking for free space.  The partition is named by the discovery's own
 * `partition_index` on the target disk; it must carry a Linux or other system
 * (an EFI System Partition and a container are refused), and its extent must
 * still hold the three appliance roles after alignment.  A partition that does
 * not fit is refused with INSUFFICIENT_SPACE, before any write - the caller
 * shows the numbers rather than starting a partial layout.
 */
enum installer_dual_boot_status installer_dual_boot_plan_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t partition_index,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan);
enum installer_dual_boot_status installer_dual_boot_preview(
    const struct installer_dual_boot_plan *plan,
    struct installer_dual_boot_preview *preview);
enum installer_dual_boot_status installer_dual_boot_confirm(
    struct installer_dual_boot_plan *plan);
enum installer_dual_boot_status installer_dual_boot_menu_build(
    const struct installer_discovery_report *report,
    const char *hfoc_boot_path,
    const char *recovery_boot_path,
    struct installer_dual_boot_menu *menu);
enum installer_dual_boot_status installer_dual_boot_menu_render(
    const struct installer_dual_boot_menu *menu,
    char *text,
    size_t capacity);
const char *installer_dual_boot_status_string(
    enum installer_dual_boot_status status);

#endif
