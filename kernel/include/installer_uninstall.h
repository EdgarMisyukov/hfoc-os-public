#ifndef HFOC_INSTALLER_UNINSTALL_H
#define HFOC_INSTALLER_UNINSTALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "installer_hardware_discovery.h"
#include "installer_layout.h"
#include "sha256.h"

/*
 * ITEM-371 uninstall / return-to-previous-OS contract.
 *
 * Uninstall is planned from evidence captured before the installation: the
 * HFOC footprint (partitions and UEFI boot entries), the partition-table
 * snapshot of the installer, the preserved previous-OS inventory and the
 * pre-install BootOrder.  The plan is published as an operator-visible
 * dry-run preview and only then may be confirmed and executed: the
 * partition table is restored byte for byte through the existing layout
 * rollback, every HFOC boot entry is removed and the previous boot order is
 * restored through a caller-bound boot operation boundary.
 *
 * The outcome is verified against the observed platform state: a remaining
 * HFOC partition or boot entry, an unrestored boot order, a lost previous-OS
 * boot path or an unverified partition table are all refused as an
 * incomplete uninstall and are never rendered as a pass.
 */

#define INSTALLER_UNINSTALL_VERSION 1U
#define INSTALLER_UNINSTALL_IDENTITY_LENGTH 32U
#define INSTALLER_UNINSTALL_PREVIEW_LENGTH 768U
#define INSTALLER_UNINSTALL_ORDER_CAPACITY 128U
#define INSTALLER_UNINSTALL_BOOT_ENTRY_CAPACITY 8U

enum installer_uninstall_stage {
    INSTALLER_UNINSTALL_STAGE_UNPREPARED = 0,
    INSTALLER_UNINSTALL_STAGE_PLANNED = 1,
    INSTALLER_UNINSTALL_STAGE_PREVIEW_PUBLISHED = 2,
    INSTALLER_UNINSTALL_STAGE_CONFIRMED = 3,
    INSTALLER_UNINSTALL_STAGE_EXECUTING = 4,
    INSTALLER_UNINSTALL_STAGE_COMPLETE = 5,
    INSTALLER_UNINSTALL_STAGE_FAILED = 6,
};

enum installer_uninstall_scenario {
    INSTALLER_UNINSTALL_SCENARIO_UNKNOWN = 0,
    INSTALLER_UNINSTALL_SCENARIO_RESTORE_PREVIOUS_OS = 1,
    INSTALLER_UNINSTALL_SCENARIO_ERASE_HFOC_ONLY = 2,
};

enum installer_uninstall_status {
    INSTALLER_UNINSTALL_OK = 0,
    INSTALLER_UNINSTALL_NULL = 1,
    INSTALLER_UNINSTALL_BAD_VERSION = 2,
    INSTALLER_UNINSTALL_BAD_STATE = 3,
    INSTALLER_UNINSTALL_INVALID_ARGUMENT = 4,
    INSTALLER_UNINSTALL_NO_HFOC_FOOTPRINT = 5,
    INSTALLER_UNINSTALL_NO_PREVIOUS_OS = 6,
    INSTALLER_UNINSTALL_BOOT_EVIDENCE_MISSING = 7,
    INSTALLER_UNINSTALL_SNAPSHOT_UNAVAILABLE = 8,
    INSTALLER_UNINSTALL_NOT_CONFIRMED = 9,
    INSTALLER_UNINSTALL_BOOT_OPS_UNAVAILABLE = 10,
    INSTALLER_UNINSTALL_BOOT_OPS_FAILED = 11,
    INSTALLER_UNINSTALL_LAYOUT_ERROR = 12,
    INSTALLER_UNINSTALL_OUTCOME_INCOMPLETE = 13,
    INSTALLER_UNINSTALL_PREVIOUS_OS_DAMAGED = 14,
    INSTALLER_UNINSTALL_SNAPSHOT_MISMATCH = 15,
    INSTALLER_UNINSTALL_CAPACITY = 16,
};

enum installer_uninstall_boot_status {
    INSTALLER_UNINSTALL_BOOT_OK = 0,
    INSTALLER_UNINSTALL_BOOT_UNAVAILABLE = 1,
    INSTALLER_UNINSTALL_BOOT_FAILED = 2,
    INSTALLER_UNINSTALL_BOOT_VERIFICATION_FAILED = 3,
};

struct installer_uninstall_evidence {
    uint32_t version;
    enum installer_firmware_mode firmware_mode;
    uint32_t disk_id;
    char identity[INSTALLER_UNINSTALL_IDENTITY_LENGTH];
    uint32_t hfoc_partition_count;
    uint32_t hfoc_boot_entry_count;
    uint32_t
        hfoc_boot_entry_indices[INSTALLER_UNINSTALL_BOOT_ENTRY_CAPACITY];
    uint32_t previous_os_count;
    uint32_t previous_boot_entry_count;
    bool previous_boot_order_available;
    size_t previous_boot_order_length;
    unsigned char
        previous_boot_order[INSTALLER_UNINSTALL_ORDER_CAPACITY];
    bool partition_table_snapshot_available;
    unsigned char snapshot_digest[SHA256_DIGEST_SIZE];
};

struct installer_uninstall_plan {
    uint32_t version;
    enum installer_uninstall_stage stage;
    enum installer_uninstall_scenario scenario;
    uint32_t disk_id;
    bool dry_run;
    bool confirmed;
    bool previous_os_preserved_required;
    bool restore_previous_boot_order;
    uint32_t remove_partition_count;
    uint32_t remove_boot_entry_count;
    uint32_t
        remove_boot_entry_indices[INSTALLER_UNINSTALL_BOOT_ENTRY_CAPACITY];
    uint32_t preserve_boot_entry_count;
    size_t previous_boot_order_length;
    unsigned char
        previous_boot_order[INSTALLER_UNINSTALL_ORDER_CAPACITY];
    unsigned char snapshot_digest[SHA256_DIGEST_SIZE];
    char identity[INSTALLER_UNINSTALL_IDENTITY_LENGTH];
};

struct installer_uninstall_preview {
    uint32_t version;
    enum installer_uninstall_scenario scenario;
    uint32_t disk_id;
    bool destructive;
    bool previous_os_preserved;
    uint32_t remove_partition_count;
    uint32_t preserve_boot_entry_count;
    char text[INSTALLER_UNINSTALL_PREVIEW_LENGTH];
};

struct installer_uninstall_outcome {
    uint32_t version;
    bool hfoc_partitions_remaining;
    bool hfoc_boot_entry_present;
    bool previous_boot_order_restored;
    bool previous_os_boot_paths_preserved;
    bool partition_table_restored;
};

struct installer_uninstall_boot_ops {
    void *context;
    enum installer_uninstall_boot_status (*remove_boot_entry)(
        void *context,
        uint32_t entry_index);
    enum installer_uninstall_boot_status (*restore_boot_order)(
        void *context,
        const unsigned char *order,
        size_t order_length,
        uint32_t entry_count);
    enum installer_uninstall_boot_status (*observe_outcome)(
        void *context,
        struct installer_uninstall_outcome *outcome);
};

enum installer_uninstall_status installer_uninstall_snapshot_digest(
    const struct installer_layout_snapshot *snapshot,
    unsigned char digest[SHA256_DIGEST_SIZE]);
enum installer_uninstall_status installer_uninstall_plan_build(
    const struct installer_discovery_report *report,
    const struct installer_uninstall_evidence *evidence,
    enum installer_uninstall_scenario scenario,
    struct installer_uninstall_plan *plan);
enum installer_uninstall_status installer_uninstall_preview(
    struct installer_uninstall_plan *plan,
    struct installer_uninstall_preview *preview);
enum installer_uninstall_status installer_uninstall_confirm(
    struct installer_uninstall_plan *plan);
enum installer_uninstall_status installer_uninstall_execute(
    struct installer_uninstall_plan *plan,
    struct block_device *device,
    const struct installer_layout_snapshot *snapshot,
    const struct installer_uninstall_boot_ops *boot_ops);
enum installer_uninstall_status installer_uninstall_verify_outcome(
    struct installer_uninstall_plan *plan,
    const struct installer_uninstall_outcome *outcome);
const char *installer_uninstall_stage_string(
    enum installer_uninstall_stage stage);
const char *installer_uninstall_scenario_string(
    enum installer_uninstall_scenario scenario);
const char *installer_uninstall_status_string(
    enum installer_uninstall_status status);

#endif
