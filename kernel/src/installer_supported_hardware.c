#include "installer_supported_hardware.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_CPU_CORES 1U
#define INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_RAM_BYTES \
    2147483648ULL
#define INSTALLER_SUPPORTED_HARDWARE_DEFAULT_BLOCK_SIZE 512U
#define INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_DISK_BLOCKS 12288ULL

static bool installer_supported_hardware_firmware_valid(
    enum installer_firmware_mode firmware)
{
    return firmware == INSTALLER_FIRMWARE_UEFI ||
           firmware == INSTALLER_FIRMWARE_BIOS_LEGACY;
}

static bool installer_supported_hardware_controller_allowed(
    const struct installer_supported_hardware_policy *policy,
    enum installer_disk_controller_kind controller)
{
    if (controller < INSTALLER_DISK_CONTROLLER_VIRTIO_BLK ||
        controller > INSTALLER_DISK_CONTROLLER_NVME) {
        return false;
    }
    return (policy->allowed_controllers &
            INSTALLER_SUPPORTED_HARDWARE_CONTROLLER_BIT(controller)) !=
           0U;
}

static void installer_supported_hardware_detail_text(
    char *detail,
    size_t capacity,
    const char *label,
    uint64_t value)
{
    static const char hex_digits[] = "0123456789abcdef";
    char digits[24U];
    size_t digit_count = 0U;
    size_t length = 0U;
    size_t index;

    if (capacity == 0U) {
        return;
    }
    detail[0U] = '\0';
    for (index = 0U; label[index] != '\0' && length + 1U < capacity;
         ++index) {
        detail[length++] = label[index];
    }
    if (length + 1U < capacity) {
        detail[length++] = '=';
    }
    if (value == 0U) {
        digits[digit_count++] = '0';
    }
    while (value != 0U && digit_count < sizeof(digits)) {
        digits[digit_count++] = hex_digits[value & 0x0FU];
        value >>= 4U;
    }
    while (digit_count != 0U && length + 1U < capacity) {
        detail[length++] = digits[--digit_count];
    }
    detail[length] = '\0';
}

static void installer_supported_hardware_set_reason(
    struct installer_supported_hardware_verdict *verdict,
    enum installer_supported_hardware_reason reason,
    const char *label,
    uint64_t value)
{
    if (verdict == NULL) {
        return;
    }
    verdict->reason = reason;
    installer_supported_hardware_detail_text(
        verdict->detail, sizeof(verdict->detail), label, value);
}

static enum installer_supported_hardware_status
installer_supported_hardware_policy_check(
    const struct installer_supported_hardware_policy *policy)
{
    if (policy == NULL) {
        return INSTALLER_SUPPORTED_HARDWARE_NULL;
    }
    if (policy->version != INSTALLER_SUPPORTED_HARDWARE_VERSION) {
        return INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION;
    }
    if (!installer_supported_hardware_firmware_valid(
            policy->required_firmware) ||
        policy->minimum_cpu_cores == 0U ||
        policy->minimum_ram_bytes == 0U ||
        policy->allowed_controllers == 0U ||
        policy->required_block_size == 0U ||
        policy->minimum_disk_blocks == 0U ||
        policy->maximum_disk_count == 0U) {
        return INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY;
    }
    return INSTALLER_SUPPORTED_HARDWARE_OK;
}

enum installer_supported_hardware_status
installer_supported_hardware_policy_default(
    struct installer_supported_hardware_policy *policy)
{
    if (policy == NULL) {
        return INSTALLER_SUPPORTED_HARDWARE_NULL;
    }
    memset(policy, 0, sizeof(*policy));
    policy->version = INSTALLER_SUPPORTED_HARDWARE_VERSION;
    policy->required_firmware = INSTALLER_FIRMWARE_UEFI;
    /* ITEM-517: legacy BIOS is admitted by default, never assumed. */
    policy->allow_bios_legacy = true;
    policy->minimum_cpu_cores =
        INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_CPU_CORES;
    policy->minimum_ram_bytes =
        INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_RAM_BYTES;
    policy->allowed_controllers =
        INSTALLER_SUPPORTED_HARDWARE_CONTROLLER_BIT(
            INSTALLER_DISK_CONTROLLER_VIRTIO_BLK) |
        INSTALLER_SUPPORTED_HARDWARE_CONTROLLER_BIT(
            INSTALLER_DISK_CONTROLLER_SATA) |
        INSTALLER_SUPPORTED_HARDWARE_CONTROLLER_BIT(
            INSTALLER_DISK_CONTROLLER_NVME);
    policy->required_block_size =
        INSTALLER_SUPPORTED_HARDWARE_DEFAULT_BLOCK_SIZE;
    policy->minimum_disk_blocks =
        INSTALLER_SUPPORTED_HARDWARE_DEFAULT_MIN_DISK_BLOCKS;
    policy->maximum_disk_count = INSTALLER_DISCOVERY_DISK_CAPACITY;
    policy->require_efi_system_partition = false;
    return INSTALLER_SUPPORTED_HARDWARE_OK;
}

enum installer_supported_hardware_status
installer_supported_hardware_evaluate(
    const struct installer_discovery_report *report,
    uint32_t target_disk_id,
    const struct installer_supported_hardware_policy *policy,
    struct installer_supported_hardware_verdict *verdict)
{
    const struct installer_discovery_disk *target = NULL;
    enum installer_supported_hardware_status policy_status;
    uint32_t present_count = 0U;
    uint32_t matches = 0U;
    uint32_t index;

    if (verdict != NULL) {
        memset(verdict, 0, sizeof(*verdict));
        verdict->version = INSTALLER_SUPPORTED_HARDWARE_VERSION;
        verdict->disk_id = target_disk_id;
    }
    if (report == NULL || verdict == NULL) {
        return INSTALLER_SUPPORTED_HARDWARE_NULL;
    }
    policy_status = installer_supported_hardware_policy_check(policy);
    if (policy_status != INSTALLER_SUPPORTED_HARDWARE_OK) {
        switch (policy_status) {
        case INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION:
            installer_supported_hardware_set_reason(
                verdict, INSTALLER_HARDWARE_REASON_BAD_VERSION,
                "policy", (uint64_t)policy->version);
            break;
        case INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY:
            installer_supported_hardware_set_reason(
                verdict, INSTALLER_HARDWARE_REASON_INVALID_REPORT,
                "policy", 0U);
            break;
        default:
            installer_supported_hardware_set_reason(
                verdict, INSTALLER_HARDWARE_REASON_NULL, "policy", 0U);
            break;
        }
        return policy_status;
    }

    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION ||
        !report->read_only) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_INVALID_REPORT,
            "report", (uint64_t)report->version);
        return INSTALLER_SUPPORTED_HARDWARE_INVALID_REPORT;
    }
    if (!report->scan_complete) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_INCOMPLETE_SCAN,
            "scan", 0U);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (report->firmware_mode != policy->required_firmware &&
        !(policy->allow_bios_legacy &&
          report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY)) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED,
            "firmware", (uint64_t)report->firmware_mode);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (!report->cpu_present ||
        report->cpu_cores < policy->minimum_cpu_cores) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_CPU_ABSENT,
            "cores", (uint64_t)report->cpu_cores);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (!report->ram_present ||
        report->ram_bytes < policy->minimum_ram_bytes) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT,
            "ram", report->ram_bytes);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (report->disk_count > policy->maximum_disk_count) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_TOO_MANY_DISKS,
            "disks", (uint64_t)report->disk_count);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }

    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        const struct installer_discovery_disk *disk =
            &report->disks[index];

        if (!disk->present || disk->id == 0U) {
            continue;
        }
        ++present_count;
        if (disk->id == target_disk_id) {
            ++matches;
            target = disk;
        }
    }
    if (matches > 1U) {
        installer_supported_hardware_set_reason(
            verdict,
            INSTALLER_HARDWARE_REASON_TARGET_DISK_AMBIGUOUS,
            "target", (uint64_t)target_disk_id);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (present_count == 0U || target_disk_id == 0U || target == NULL) {
        installer_supported_hardware_set_reason(
            verdict, INSTALLER_HARDWARE_REASON_TARGET_DISK_MISSING,
            "target", (uint64_t)target_disk_id);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (!installer_supported_hardware_controller_allowed(
            policy, target->controller)) {
        installer_supported_hardware_set_reason(
            verdict,
            INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED,
            "controller", (uint64_t)target->controller);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (target->block_size != policy->required_block_size) {
        installer_supported_hardware_set_reason(
            verdict,
            INSTALLER_HARDWARE_REASON_BLOCK_SIZE_UNSUPPORTED,
            "block", (uint64_t)target->block_size);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (target->block_count < policy->minimum_disk_blocks) {
        installer_supported_hardware_set_reason(
            verdict,
            INSTALLER_HARDWARE_REASON_CAPACITY_INSUFFICIENT,
            "blocks", target->block_count);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }
    if (policy->require_efi_system_partition &&
        !target->has_efi_system_partition) {
        installer_supported_hardware_set_reason(
            verdict,
            INSTALLER_HARDWARE_REASON_EFI_SYSTEM_PARTITION_MISSING,
            "esp", 0U);
        return INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED;
    }

    verdict->supported = true;
    verdict->reason = INSTALLER_HARDWARE_REASON_SUPPORTED;
    installer_supported_hardware_detail_text(
        verdict->detail, sizeof(verdict->detail), "disk",
        (uint64_t)target_disk_id);
    return INSTALLER_SUPPORTED_HARDWARE_OK;
}

bool installer_supported_hardware_verdict_supported(
    const struct installer_supported_hardware_verdict *verdict)
{
    return verdict != NULL &&
           verdict->version == INSTALLER_SUPPORTED_HARDWARE_VERSION &&
           verdict->supported &&
           verdict->reason == INSTALLER_HARDWARE_REASON_SUPPORTED;
}

const char *installer_supported_hardware_reason_string(
    enum installer_supported_hardware_reason reason)
{
    switch (reason) {
    case INSTALLER_HARDWARE_REASON_SUPPORTED:
        return "INSTALLER_HARDWARE_REASON_SUPPORTED";
    case INSTALLER_HARDWARE_REASON_NULL:
        return "INSTALLER_HARDWARE_REASON_NULL";
    case INSTALLER_HARDWARE_REASON_BAD_VERSION:
        return "INSTALLER_HARDWARE_REASON_BAD_VERSION";
    case INSTALLER_HARDWARE_REASON_INCOMPLETE_SCAN:
        return "INSTALLER_HARDWARE_REASON_INCOMPLETE_SCAN";
    case INSTALLER_HARDWARE_REASON_INVALID_REPORT:
        return "INSTALLER_HARDWARE_REASON_INVALID_REPORT";
    case INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED:
        return "INSTALLER_HARDWARE_REASON_FIRMWARE_UNSUPPORTED";
    case INSTALLER_HARDWARE_REASON_CPU_ABSENT:
        return "INSTALLER_HARDWARE_REASON_CPU_ABSENT";
    case INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT:
        return "INSTALLER_HARDWARE_REASON_MEMORY_INSUFFICIENT";
    case INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED:
        return "INSTALLER_HARDWARE_REASON_CONTROLLER_UNSUPPORTED";
    case INSTALLER_HARDWARE_REASON_BLOCK_SIZE_UNSUPPORTED:
        return "INSTALLER_HARDWARE_REASON_BLOCK_SIZE_UNSUPPORTED";
    case INSTALLER_HARDWARE_REASON_CAPACITY_INSUFFICIENT:
        return "INSTALLER_HARDWARE_REASON_CAPACITY_INSUFFICIENT";
    case INSTALLER_HARDWARE_REASON_TARGET_DISK_MISSING:
        return "INSTALLER_HARDWARE_REASON_TARGET_DISK_MISSING";
    case INSTALLER_HARDWARE_REASON_TARGET_DISK_AMBIGUOUS:
        return "INSTALLER_HARDWARE_REASON_TARGET_DISK_AMBIGUOUS";
    case INSTALLER_HARDWARE_REASON_TOO_MANY_DISKS:
        return "INSTALLER_HARDWARE_REASON_TOO_MANY_DISKS";
    case INSTALLER_HARDWARE_REASON_EFI_SYSTEM_PARTITION_MISSING:
        return "INSTALLER_HARDWARE_REASON_EFI_SYSTEM_PARTITION_MISSING";
    default:
        return NULL;
    }
}

const char *installer_supported_hardware_status_string(
    enum installer_supported_hardware_status status)
{
    switch (status) {
    case INSTALLER_SUPPORTED_HARDWARE_OK:
        return "INSTALLER_SUPPORTED_HARDWARE_OK";
    case INSTALLER_SUPPORTED_HARDWARE_NULL:
        return "INSTALLER_SUPPORTED_HARDWARE_NULL";
    case INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION:
        return "INSTALLER_SUPPORTED_HARDWARE_BAD_VERSION";
    case INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY:
        return "INSTALLER_SUPPORTED_HARDWARE_INVALID_POLICY";
    case INSTALLER_SUPPORTED_HARDWARE_INVALID_REPORT:
        return "INSTALLER_SUPPORTED_HARDWARE_INVALID_REPORT";
    case INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED:
        return "INSTALLER_SUPPORTED_HARDWARE_UNSUPPORTED";
    default:
        return NULL;
    }
}
