#ifndef HFOC_INSTALLER_SUPPORTED_HARDWARE_H
#define HFOC_INSTALLER_SUPPORTED_HARDWARE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "installer_hardware_discovery.h"

/*
 * ITEM-371 fail-closed supported-hardware gate.
 *
 * The installer refuses to plan or perform any write on hardware that is
 * not covered by the declared policy: unknown firmware, an absent CPU, too
 * little RAM, an unsupported storage controller, an unsupported block size,
 * a disk below the minimum capacity, a missing or ambiguous target disk and
 * an ambiguous discovery report all fail closed.  The verdict carries the
 * exact reason and a short auditable detail string; it is never rendered as
 * a pass when the policy is not satisfied.
 */

#define INSTALLER_SUPPORTED_HARDWARE_VERSION 1U
#define INSTALLER_SUPPORTED_HARDWARE_DETAIL_LENGTH 64U
#define INSTALLER_SUPPORTED_HARDWARE_CONTROLLER_BIT(kind) \
    (1U << (uint32_t)(kind))

enum installer_supported_hardware_status {
    INSTALLER_SUPPORTED_HARDWARE_OK = 0,
    INSTALLER_SUPPORTED_HARDWARE_NULL = 1,
    INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION = 2,
    INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY = 3,
    INSTALLER_SUPPORTED_HARDWARE_INVALID_REPORT = 4,
    INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED = 5,
};

enum installer_supported_hardware_reason {
    INSTALLER_HARDWARE_REASON_SUPPORTED = 0,
    INSTALLER_HARDWARE_REASON_NULL = 1,
    INSTALLER_HARDWARE_REASON_BAD_VERSION = 2,
    INSTALLER_HARDWARE_REASON_INCOMPLETE_SCAN = 3,
    INSTALLER_HARDWARE_REASON_INVALID_REPORT = 4,
    INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED = 5,
    INSTALLER_HARDWARE_REASON_CPU_ABSENT = 6,
    INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT = 7,
    INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED = 8,
    INSTALLER_HARDWARE_REASON_BLOCK_SIZE_UNSUPPORTED = 9,
    INSTALLER_HARDWARE_REASON_CAPACITY_INSUFFICIENT = 10,
    INSTALLER_HARDWARE_REASON_TARGET_DISK_MISSING = 11,
    INSTALLER_HARDWARE_REASON_TARGET_DISK_AMBIGUOUS = 12,
    INSTALLER_HARDWARE_REASON_TOO_MANY_DISKS = 13,
    INSTALLER_HARDWARE_REASON_EFI_SYSTEM_PARTITION_MISSING = 14,
};

struct installer_supported_hardware_policy {
    uint32_t version;
    enum installer_firmware_mode required_firmware;
    /*
     * ITEM-517: the appliance installs on machines without UEFI, so a policy
     * admits the legacy-BIOS boot unless it says otherwise.  The declared
     * firmware stays the declared default (`required_firmware`); this is the
     * one documented exception, and an unknown firmware is never admitted.
     */
    bool allow_bios_legacy;
    uint32_t minimum_cpu_cores;
    uint64_t minimum_ram_bytes;
    uint32_t allowed_controllers;
    uint32_t required_block_size;
    uint64_t minimum_disk_blocks;
    uint32_t maximum_disk_count;
    bool require_efi_system_partition;
};

struct installer_supported_hardware_verdict {
    uint32_t version;
    bool supported;
    uint32_t disk_id;
    enum installer_supported_hardware_reason reason;
    char detail[INSTALLER_SUPPORTED_HARDWARE_DETAIL_LENGTH];
};

enum installer_supported_hardware_status
installer_supported_hardware_policy_default(
    struct installer_supported_hardware_policy *policy);
enum installer_supported_hardware_status
installer_supported_hardware_evaluate(
    const struct installer_discovery_report *report,
    uint32_t target_disk_id,
    const struct installer_supported_hardware_policy *policy,
    struct installer_supported_hardware_verdict *verdict);
bool installer_supported_hardware_verdict_supported(
    const struct installer_supported_hardware_verdict *verdict);
const char *installer_supported_hardware_reason_string(
    enum installer_supported_hardware_reason reason);
const char *installer_supported_hardware_status_string(
    enum installer_supported_hardware_status status);

#endif
