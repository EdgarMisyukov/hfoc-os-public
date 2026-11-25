#include "installer_dual_boot.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct installer_dual_boot_sorted_partition {
    uint64_t first_lba;
    uint64_t last_lba;
};

static enum installer_dual_boot_status
installer_dual_boot_report_check(
    const struct installer_discovery_report *report)
{
    enum installer_discovery_status discovery_status;

    if (report == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return INSTALLER_DUAL_BOOT_BAD_VERSION;
    }
    if (!report->read_only) {
        return INSTALLER_DUAL_BOOT_READ_ONLY_VIOLATION;
    }
    if (!report->scan_complete || report->manual_selection_required) {
        return INSTALLER_DUAL_BOOT_BAD_STATE;
    }
    discovery_status = installer_discovery_validate(report);
    if (discovery_status == INSTALLER_DISCOVERY_AMBIGUOUS) {
        return INSTALLER_DUAL_BOOT_AMBIGUOUS;
    }
    if (discovery_status != INSTALLER_DISCOVERY_OK) {
        return INSTALLER_DUAL_BOOT_BAD_STATE;
    }
    return INSTALLER_DUAL_BOOT_OK;
}

static enum installer_dual_boot_status
installer_dual_boot_requirements_check(
    const struct installer_dual_boot_requirements *requirements)
{
    uint64_t total;

    if (requirements == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (requirements->version !=
        INSTALLER_DUAL_BOOT_REQUIREMENTS_VERSION) {
        return INSTALLER_DUAL_BOOT_BAD_VERSION;
    }
    if (requirements->system_length_lba == 0U ||
        requirements->persistence_length_lba == 0U ||
        requirements->recovery_length_lba == 0U ||
        requirements->system_length_lba %
            INSTALLER_DUAL_BOOT_ALIGN_LBA != 0U ||
        requirements->persistence_length_lba %
            INSTALLER_DUAL_BOOT_ALIGN_LBA != 0U ||
        requirements->recovery_length_lba %
            INSTALLER_DUAL_BOOT_ALIGN_LBA != 0U) {
        return INSTALLER_DUAL_BOOT_INVALID_ARGUMENT;
    }
    total = requirements->system_length_lba;
    if (total >
        UINT64_MAX - requirements->persistence_length_lba) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    total += requirements->persistence_length_lba;
    if (total > UINT64_MAX - requirements->recovery_length_lba) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    return INSTALLER_DUAL_BOOT_OK;
}

static const struct installer_discovery_disk *
installer_dual_boot_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

static bool installer_dual_boot_partition_belongs_to_os(
    const struct installer_discovery_report *report,
    const struct installer_discovery_os *os,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].id == os->partition_id) {
            return report->partitions[index].disk_id == disk_id;
        }
    }
    return false;
}

static uint32_t installer_dual_boot_existing_os_count(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint32_t count = 0U;
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        if (report->oss[index].present &&
            installer_dual_boot_partition_belongs_to_os(
                report, &report->oss[index], disk_id)) {
            ++count;
        }
    }
    return count;
}

static bool installer_dual_boot_has_esp(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t *esp_partition_id,
    uint32_t *esp_count)
{
    uint32_t count = 0U;
    uint32_t first_id = 0U;
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].disk_id == disk_id &&
            report->partitions[index].efi_system) {
            if (count == 0U) {
                first_id = report->partitions[index].id;
            }
            ++count;
        }
    }
    if (esp_partition_id != NULL) {
        *esp_partition_id = first_id;
    }
    if (esp_count != NULL) {
        *esp_count = count;
    }
    return count > 0U;
}

static bool installer_dual_boot_suitable_automatic_disk(
    const struct installer_discovery_report *report,
    const struct installer_discovery_disk *disk)
{
    uint32_t esp_count = 0U;

    if (report->firmware_mode != INSTALLER_FIRMWARE_UEFI ||
        !disk->present ||
        disk->partition_table != INSTALLER_PARTITION_TABLE_GPT ||
        !installer_dual_boot_has_esp(
            report, disk->id, NULL, &esp_count) ||
        esp_count != 1U ||
        installer_dual_boot_existing_os_count(
            report, disk->id) == 0U) {
        return false;
    }
    return true;
}

static uint64_t installer_dual_boot_align_up(uint64_t value)
{
    const uint64_t remainder =
        value % INSTALLER_DUAL_BOOT_ALIGN_LBA;

    if (remainder == 0U) {
        return value;
    }
    return value + (INSTALLER_DUAL_BOOT_ALIGN_LBA - remainder);
}

static uint32_t installer_dual_boot_collect_partitions(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_dual_boot_sorted_partition *sorted)
{
    uint32_t count = 0U;
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].disk_id == disk_id) {
            sorted[count].first_lba =
                report->partitions[index].first_lba;
            sorted[count].last_lba =
                report->partitions[index].first_lba +
                report->partitions[index].length_lba;
            ++count;
        }
    }
    return count;
}

static void installer_dual_boot_sort_partitions(
    struct installer_dual_boot_sorted_partition *partitions,
    uint32_t count)
{
    uint32_t pass;

    for (pass = 0U; pass < count; ++pass) {
        uint32_t index;

        for (index = 0U; index + 1U < count; ++index) {
            if (partitions[index].first_lba >
                partitions[index + 1U].first_lba) {
                struct installer_dual_boot_sorted_partition swap =
                    partitions[index];

                partitions[index] = partitions[index + 1U];
                partitions[index + 1U] = swap;
            }
        }
    }
}

static enum installer_dual_boot_status
installer_dual_boot_find_free_region(
    const struct installer_discovery_report *report,
    const struct installer_discovery_disk *disk,
    uint64_t required_length,
    uint64_t *free_first_lba,
    uint64_t *free_length_lba)
{
    struct installer_dual_boot_sorted_partition sorted[
        INSTALLER_DISCOVERY_PARTITION_CAPACITY];
    uint32_t partition_count;
    uint64_t usable_end;
    uint64_t cursor;
    uint64_t best_start = 0U;
    uint64_t best_length = 0U;
    uint32_t index;

    if (required_length == 0U ||
        disk->block_count <= INSTALLER_DUAL_BOOT_RESERVED_LBA) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    usable_end =
        disk->block_count - INSTALLER_DUAL_BOOT_RESERVED_LBA;
    if (usable_end < INSTALLER_DUAL_BOOT_ALIGN_LBA) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    partition_count = installer_dual_boot_collect_partitions(
        report, disk->id, sorted);
    installer_dual_boot_sort_partitions(sorted, partition_count);
    cursor = INSTALLER_DUAL_BOOT_ALIGN_LBA;
    for (index = 0U; index <= partition_count; ++index) {
        const uint64_t occupied_start =
            index < partition_count
                ? sorted[index].first_lba
                : usable_end;
        const uint64_t occupied_end =
            index < partition_count
                ? sorted[index].last_lba
                : usable_end;
        uint64_t candidate_start;
        uint64_t candidate_end;
        uint64_t candidate_length;

        if (cursor < occupied_start) {
            candidate_start = installer_dual_boot_align_up(cursor);
            candidate_end = occupied_start < usable_end
                ? occupied_start
                : usable_end;
            if (candidate_end > candidate_start) {
                candidate_length = candidate_end - candidate_start;
                if (candidate_length > best_length) {
                    best_start = candidate_start;
                    best_length = candidate_length;
                }
            }
        }
        if (index < partition_count &&
            occupied_end > cursor) {
            cursor = occupied_end;
        }
    }
    if (best_length < required_length ||
        best_length > usable_end - best_start) {
        return INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE;
    }
    *free_first_lba = best_start;
    *free_length_lba = best_length;
    return INSTALLER_DUAL_BOOT_OK;
}

static bool installer_dual_boot_identity_valid(
    const char *identity,
    size_t capacity)
{
    size_t index;

    if (identity == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        const unsigned char value = (unsigned char)identity[index];

        if (value == 0U) {
            return index > 0U;
        }
        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return false;
}

static void installer_dual_boot_copy_identity(
    char destination[INSTALLER_DISCOVERY_IDENTITY_LENGTH],
    const char *identity)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DISCOVERY_IDENTITY_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (identity == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_DISCOVERY_IDENTITY_LENGTH &&
         identity[index] != '\0';
         ++index) {
        destination[index] = identity[index];
    }
}

static enum installer_dual_boot_status
installer_dual_boot_plan_common(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_dual_boot_requirements *requirements,
    bool automatic,
    struct installer_dual_boot_plan *plan)
{
    const struct installer_discovery_disk *disk;
    enum installer_dual_boot_status status;
    uint64_t required_length;
    uint64_t free_first_lba;
    uint64_t free_length_lba;
    uint32_t esp_id;
    uint32_t esp_count = 0U;
    uint32_t preserved_count = 0U;
    uint32_t existing_os_count;
    size_t index;

    status = installer_dual_boot_report_check(report);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    status = installer_dual_boot_requirements_check(requirements);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    if (plan == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (report->firmware_mode != INSTALLER_FIRMWARE_UEFI) {
        return INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE;
    }
    disk = installer_dual_boot_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_DUAL_BOOT_INVALID_ARGUMENT;
    }
    if (disk->partition_table != INSTALLER_PARTITION_TABLE_GPT ||
        !installer_dual_boot_identity_valid(
            disk->identity, sizeof(disk->identity)) ||
        disk->block_count == 0U) {
        return INSTALLER_DUAL_BOOT_AMBIGUOUS;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (report->partitions[index].present &&
            report->partitions[index].disk_id == disk_id) {
            ++preserved_count;
        }
    }
    existing_os_count =
        installer_dual_boot_existing_os_count(report, disk_id);
    if (existing_os_count == 0U) {
        return INSTALLER_DUAL_BOOT_NO_EXISTING_OS;
    }
    (void)installer_dual_boot_has_esp(
        report, disk_id, &esp_id, &esp_count);
    if (esp_count != 1U) {
        return INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION;
    }
    required_length = requirements->system_length_lba;
    required_length += requirements->persistence_length_lba;
    required_length += requirements->recovery_length_lba;
    status = installer_dual_boot_find_free_region(
        report, disk, required_length,
        &free_first_lba, &free_length_lba);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_DUAL_BOOT_VERSION;
    plan->firmware_mode = report->firmware_mode;
    plan->disk_id = disk_id;
    plan->esp_partition_id = esp_id;
    plan->existing_os_count = existing_os_count;
    plan->preserved_partition_count = preserved_count;
    plan->system_first_lba = free_first_lba;
    plan->system_length_lba = requirements->system_length_lba;
    plan->persistence_first_lba =
        free_first_lba + requirements->system_length_lba;
    plan->persistence_length_lba =
        requirements->persistence_length_lba;
    plan->recovery_first_lba =
        plan->persistence_first_lba +
        requirements->persistence_length_lba;
    plan->recovery_length_lba = requirements->recovery_length_lba;
    plan->free_first_lba = free_first_lba;
    plan->free_length_lba = free_length_lba;
    plan->automatic = automatic;
    plan->destructive = false;
    plan->confirmed = false;
    installer_dual_boot_copy_identity(plan->identity, disk->identity);
    return INSTALLER_DUAL_BOOT_OK;
}

enum installer_dual_boot_status installer_dual_boot_plan_automatic(
    const struct installer_discovery_report *report,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan)
{
    enum installer_dual_boot_status status;
    uint32_t candidate_disk = 0U;
    uint32_t candidate_count = 0U;
    size_t index;

    status = installer_dual_boot_report_check(report);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    status = installer_dual_boot_requirements_check(requirements);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    if (plan == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (installer_dual_boot_suitable_automatic_disk(
                report, &report->disks[index])) {
            candidate_disk = report->disks[index].id;
            ++candidate_count;
        }
    }
    if (candidate_count == 0U) {
        for (index = 0U;
             index < INSTALLER_DISCOVERY_DISK_CAPACITY;
             ++index) {
            if (report->disks[index].present &&
                report->disks[index].partition_table ==
                    INSTALLER_PARTITION_TABLE_GPT &&
                report->disks[index].has_efi_system_partition) {
                return INSTALLER_DUAL_BOOT_NO_EXISTING_OS;
            }
        }
        return INSTALLER_DUAL_BOOT_AMBIGUOUS;
    }
    if (candidate_count > 1U) {
        return INSTALLER_DUAL_BOOT_AMBIGUOUS;
    }
    return installer_dual_boot_plan_common(
        report, candidate_disk, requirements, true, plan);
}

enum installer_dual_boot_status installer_dual_boot_plan_selected(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan)
{
    return installer_dual_boot_plan_common(
        report, disk_id, requirements, false, plan);
}

/*
 * ITEM-501: the install that gives up one partition the operator named.
 *
 * The common planner looks for the largest free region; a Fedora laptop has
 * none, because the installer that put Fedora there used the whole disk.  This
 * planner places the same three roles into the extent of one existing partition
 * instead.  Everything it does not name stays: the shared ESP, every other
 * partition and the table itself are the caller's to preserve, and only the
 * named entry is removed by the writer.
 */
enum installer_dual_boot_status installer_dual_boot_plan_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t partition_index,
    const struct installer_dual_boot_requirements *requirements,
    struct installer_dual_boot_plan *plan)
{
    const struct installer_discovery_disk *disk;
    const struct installer_discovery_partition *released = NULL;
    enum installer_dual_boot_status status;
    uint64_t required_length;
    uint64_t extent_end;
    uint64_t first_lba;
    uint64_t available;
    uint64_t usable_end;
    uint32_t esp_id = 0U;
    uint32_t esp_count = 0U;
    uint32_t preserved_count = 0U;
    uint32_t existing_os_count;
    size_t index;

    status = installer_dual_boot_report_check(report);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    status = installer_dual_boot_requirements_check(requirements);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    if (plan == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (report->firmware_mode != INSTALLER_FIRMWARE_UEFI) {
        return INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE;
    }
    disk = installer_dual_boot_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_DUAL_BOOT_INVALID_ARGUMENT;
    }
    if (disk->partition_table != INSTALLER_PARTITION_TABLE_GPT ||
        !installer_dual_boot_identity_valid(
            disk->identity, sizeof(disk->identity)) ||
        disk->block_count == 0U) {
        return INSTALLER_DUAL_BOOT_AMBIGUOUS;
    }
    existing_os_count =
        installer_dual_boot_existing_os_count(report, disk_id);
    if (existing_os_count == 0U) {
        return INSTALLER_DUAL_BOOT_NO_EXISTING_OS;
    }
    (void)installer_dual_boot_has_esp(report, disk_id, &esp_id, &esp_count);
    if (esp_count != 1U) {
        return INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (!partition->present || partition->disk_id != disk_id) {
            continue;
        }
        if (partition->partition_index == partition_index) {
            released = partition;
        } else {
            ++preserved_count;
        }
    }
    if (released == NULL) {
        return INSTALLER_DUAL_BOOT_INVALID_ARGUMENT;
    }
    /*
     * The shared ESP is never a candidate - removing it would take the other
     * system's loader with it - and neither is a container, which is a table
     * rather than a partition.  A volume layout (LVM, Btrfs) is exactly what
     * this mode is for: the partition is deleted whole, so what lives inside
     * it does not have to be understood.
     */
    if (released->container || released->efi_system ||
        released->layout == INSTALLER_PARTITION_LAYOUT_EXTENDED ||
        released->layout == INSTALLER_PARTITION_LAYOUT_LOGICAL ||
        released->length_lba == 0U) {
        return INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED;
    }
    if (released->first_lba < INSTALLER_DUAL_BOOT_RESERVED_LBA ||
        released->first_lba >= disk->block_count) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    usable_end = disk->block_count - INSTALLER_DUAL_BOOT_RESERVED_LBA;
    if (released->length_lba > disk->block_count - released->first_lba) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    extent_end = released->first_lba + released->length_lba;
    if (extent_end > usable_end) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    required_length = requirements->system_length_lba;
    if (required_length >
            UINT64_MAX - requirements->persistence_length_lba) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    required_length += requirements->persistence_length_lba;
    if (required_length > UINT64_MAX - requirements->recovery_length_lba) {
        return INSTALLER_DUAL_BOOT_OUT_OF_RANGE;
    }
    required_length += requirements->recovery_length_lba;
    first_lba = installer_dual_boot_align_up(released->first_lba);
    if (first_lba >= extent_end) {
        return INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE;
    }
    available = extent_end - first_lba;
    if (available < required_length) {
        return INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE;
    }
    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_DUAL_BOOT_VERSION;
    plan->firmware_mode = report->firmware_mode;
    plan->disk_id = disk_id;
    plan->esp_partition_id = esp_id;
    plan->existing_os_count = existing_os_count;
    plan->preserved_partition_count = preserved_count;
    plan->system_first_lba = first_lba;
    plan->system_length_lba = requirements->system_length_lba;
    plan->persistence_first_lba =
        first_lba + requirements->system_length_lba;
    plan->persistence_length_lba =
        requirements->persistence_length_lba;
    plan->recovery_first_lba =
        plan->persistence_first_lba +
        requirements->persistence_length_lba;
    plan->recovery_length_lba = requirements->recovery_length_lba;
    plan->free_first_lba = first_lba;
    plan->free_length_lba = available;
    plan->automatic = false;
    plan->destructive = false;
    plan->confirmed = false;
    plan->releases_partition = true;
    plan->released_index = released->partition_index;
    plan->released_partition_id = released->id;
    plan->released_layout = released->layout;
    plan->released_owner = released->owner;
    plan->released_first_lba = released->first_lba;
    plan->released_length_lba = released->length_lba;
    installer_dual_boot_copy_identity(plan->identity, disk->identity);
    return INSTALLER_DUAL_BOOT_OK;
}

static bool installer_dual_boot_plan_valid(
    const struct installer_dual_boot_plan *plan)
{
    if (plan == NULL ||
        plan->version != INSTALLER_DUAL_BOOT_VERSION ||
        plan->disk_id == 0U ||
        plan->existing_os_count == 0U ||
        plan->preserved_partition_count == 0U ||
        plan->system_length_lba == 0U ||
        plan->persistence_length_lba == 0U ||
        plan->recovery_length_lba == 0U ||
        plan->destructive ||
        !installer_dual_boot_identity_valid(
            plan->identity, sizeof(plan->identity))) {
        return false;
    }
    if (plan->releases_partition) {
        uint64_t released_end;
        uint64_t recovery_end;

        if (plan->released_index == 0U || plan->released_length_lba == 0U ||
            plan->released_length_lba >
                UINT64_MAX - plan->released_first_lba ||
            plan->recovery_length_lba >
                UINT64_MAX - plan->recovery_first_lba) {
            return false;
        }
        released_end = plan->released_first_lba +
                       plan->released_length_lba;
        recovery_end = plan->recovery_first_lba +
                       plan->recovery_length_lba;
        if (plan->system_first_lba < plan->released_first_lba ||
            recovery_end > released_end) {
            return false;
        }
    }
    return true;
}

static void installer_dual_boot_append_char(
    char *buffer,
    size_t capacity,
    size_t *length,
    char value)
{
    if (*length + 1U < capacity) {
        buffer[*length] = value;
        ++*length;
        buffer[*length] = '\0';
    }
}

static void installer_dual_boot_append_text(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        installer_dual_boot_append_char(
            buffer, capacity, length, text[index]);
    }
}

static void installer_dual_boot_append_uint64(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint64_t value)
{
    char reversed[20U];
    size_t count = 0U;

    if (value == 0U) {
        installer_dual_boot_append_char(
            buffer, capacity, length, '0');
        return;
    }
    while (value != 0U && count < sizeof(reversed)) {
        reversed[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count > 0U) {
        --count;
        installer_dual_boot_append_char(
            buffer, capacity, length, reversed[count]);
    }
}

static const char *installer_dual_boot_firmware_text(
    enum installer_firmware_mode firmware_mode)
{
    if (firmware_mode == INSTALLER_FIRMWARE_UEFI) {
        return "UEFI";
    }
    if (firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
        return "BIOS";
    }
    return "unknown";
}

enum installer_dual_boot_status installer_dual_boot_preview(
    const struct installer_dual_boot_plan *plan,
    struct installer_dual_boot_preview *preview)
{
    size_t length = 0U;

    if (preview == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (!installer_dual_boot_plan_valid(plan)) {
        return INSTALLER_DUAL_BOOT_BAD_STATE;
    }
    memset(preview, 0, sizeof(*preview));
    preview->version = INSTALLER_DUAL_BOOT_VERSION;
    preview->disk_id = plan->disk_id;
    preview->firmware_mode = plan->firmware_mode;
    preview->existing_os_count = plan->existing_os_count;
    preview->preserved_partition_count =
        plan->preserved_partition_count;
    preview->added_partition_count = 3U;
    preview->destructive = plan->destructive;
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->identity);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->releases_partition ? " release " : " dual-boot ");
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        installer_dual_boot_firmware_text(plan->firmware_mode));
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        " preserve=");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->preserved_partition_count);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        " os=");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->existing_os_count);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        " add=3 destructive=0 system=");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->system_first_lba);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length, ":");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->system_length_lba);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        " persistence=");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->persistence_first_lba);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length, ":");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->persistence_length_lba);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length,
        " recovery=");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->recovery_first_lba);
    installer_dual_boot_append_text(
        preview->text, sizeof(preview->text), &length, ":");
    installer_dual_boot_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->recovery_length_lba);
    if (plan->releases_partition) {
        installer_dual_boot_append_text(
            preview->text, sizeof(preview->text), &length,
            " released=");
        installer_dual_boot_append_uint64(
            preview->text, sizeof(preview->text), &length,
            plan->released_index);
        installer_dual_boot_append_text(
            preview->text, sizeof(preview->text), &length, ":");
        installer_dual_boot_append_uint64(
            preview->text, sizeof(preview->text), &length,
            plan->released_first_lba);
        installer_dual_boot_append_text(
            preview->text, sizeof(preview->text), &length, ":");
        installer_dual_boot_append_uint64(
            preview->text, sizeof(preview->text), &length,
            plan->released_length_lba);
    }
    return INSTALLER_DUAL_BOOT_OK;
}

enum installer_dual_boot_status installer_dual_boot_confirm(
    struct installer_dual_boot_plan *plan)
{
    if (!installer_dual_boot_plan_valid(plan)) {
        return INSTALLER_DUAL_BOOT_BAD_STATE;
    }
    plan->confirmed = true;
    return INSTALLER_DUAL_BOOT_OK;
}

static bool installer_dual_boot_path_valid(
    const char *path,
    size_t capacity)
{
    size_t index;

    if (path == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        const unsigned char value = (unsigned char)path[index];

        if (value == 0U) {
            return index > 0U;
        }
        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return false;
}

static enum installer_dual_boot_entry_kind
installer_dual_boot_entry_kind_for_os(
    enum installer_os_kind os_kind)
{
    if (os_kind == INSTALLER_OS_LINUX) {
        return INSTALLER_DUAL_BOOT_ENTRY_LINUX;
    }
    if (os_kind == INSTALLER_OS_WINDOWS) {
        return INSTALLER_DUAL_BOOT_ENTRY_WINDOWS;
    }
    if (os_kind == INSTALLER_OS_OTHER) {
        return INSTALLER_DUAL_BOOT_ENTRY_OTHER;
    }
    return INSTALLER_DUAL_BOOT_ENTRY_UNKNOWN;
}

static void installer_dual_boot_copy_string(
    char destination[INSTALLER_DUAL_BOOT_PATH_LENGTH],
    const char *source)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DUAL_BOOT_PATH_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (source == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_DUAL_BOOT_PATH_LENGTH &&
         source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

static void installer_dual_boot_copy_label(
    char destination[INSTALLER_DUAL_BOOT_LABEL_LENGTH],
    const char *source)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_DUAL_BOOT_LABEL_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (source == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_DUAL_BOOT_LABEL_LENGTH &&
         source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

enum installer_dual_boot_status installer_dual_boot_menu_build(
    const struct installer_discovery_report *report,
    const char *hfoc_boot_path,
    const char *recovery_boot_path,
    struct installer_dual_boot_menu *menu)
{
    enum installer_dual_boot_status status;
    uint32_t existing_count = 0U;
    uint32_t slot = 1U;
    size_t index;

    status = installer_dual_boot_report_check(report);
    if (status != INSTALLER_DUAL_BOOT_OK) {
        return status;
    }
    if (!installer_dual_boot_path_valid(
            hfoc_boot_path, INSTALLER_DUAL_BOOT_PATH_LENGTH) ||
        !installer_dual_boot_path_valid(
            recovery_boot_path, INSTALLER_DUAL_BOOT_PATH_LENGTH)) {
        return INSTALLER_DUAL_BOOT_INVALID_ARGUMENT;
    }
    if (menu == NULL) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        if (report->oss[index].present) {
            ++existing_count;
        }
    }
    if (existing_count == 0U) {
        return INSTALLER_DUAL_BOOT_NO_EXISTING_OS;
    }
    if (existing_count + 2U > INSTALLER_DUAL_BOOT_MENU_CAPACITY) {
        return INSTALLER_DUAL_BOOT_CAPACITY;
    }
    memset(menu, 0, sizeof(*menu));
    menu->version = INSTALLER_DUAL_BOOT_VERSION;
    menu->firmware_mode = report->firmware_mode;
    menu->entries[0U].id = 1U;
    menu->entries[0U].present = true;
    menu->entries[0U].kind = INSTALLER_DUAL_BOOT_ENTRY_HFOC_OS;
    menu->entries[0U].os_kind = INSTALLER_OS_UNKNOWN;
    installer_dual_boot_copy_label(menu->entries[0U].label, "HFOC OS");
    installer_dual_boot_copy_string(
        menu->entries[0U].boot_path, hfoc_boot_path);
    for (index = 0U;
         index < INSTALLER_DISCOVERY_OS_CAPACITY;
         ++index) {
        const struct installer_discovery_os *os = &report->oss[index];
        struct installer_dual_boot_menu_entry *entry;
        const char *label;

        if (!os->present) {
            continue;
        }
        if (!os->has_efi_entry &&
            os->boot_path[0] == '\0') {
            return INSTALLER_DUAL_BOOT_UNKNOWN_BOOT_PATH;
        }
        if (slot + 1U >= INSTALLER_DUAL_BOOT_MENU_CAPACITY) {
            return INSTALLER_DUAL_BOOT_CAPACITY;
        }
        entry = &menu->entries[slot];
        entry->id = slot + 1U;
        entry->present = true;
        entry->kind = installer_dual_boot_entry_kind_for_os(os->kind);
        entry->os_kind = os->kind;
        entry->os_id = os->id;
        entry->partition_id = os->partition_id;
        entry->has_efi_entry = os->has_efi_entry;
        if (os->kind == INSTALLER_OS_LINUX) {
            label = "Linux";
        } else if (os->kind == INSTALLER_OS_WINDOWS) {
            label = "Windows Boot Manager";
        } else {
            label = "Other";
        }
        installer_dual_boot_copy_label(entry->label, label);
        installer_dual_boot_copy_string(entry->boot_path, os->boot_path);
        ++slot;
    }
    menu->entries[slot].id = slot + 1U;
    menu->entries[slot].present = true;
    menu->entries[slot].kind = INSTALLER_DUAL_BOOT_ENTRY_RECOVERY;
    menu->entries[slot].os_kind = INSTALLER_OS_UNKNOWN;
    installer_dual_boot_copy_label(
        menu->entries[slot].label, "Recovery");
    installer_dual_boot_copy_string(
        menu->entries[slot].boot_path, recovery_boot_path);
    menu->entry_count = slot + 1U;
    return INSTALLER_DUAL_BOOT_OK;
}

enum installer_dual_boot_status installer_dual_boot_menu_render(
    const struct installer_dual_boot_menu *menu,
    char *text,
    size_t capacity)
{
    size_t length = 0U;
    uint32_t index;

    if (menu == NULL || text == NULL || capacity == 0U) {
        return INSTALLER_DUAL_BOOT_NULL;
    }
    if (menu->version != INSTALLER_DUAL_BOOT_VERSION ||
        menu->entry_count == 0U ||
        menu->entry_count > INSTALLER_DUAL_BOOT_MENU_CAPACITY) {
        return INSTALLER_DUAL_BOOT_BAD_STATE;
    }
    text[0] = '\0';
    installer_dual_boot_append_text(
        text, capacity, &length, "HFOC APPLIANCE\n\n");
    for (index = 0U; index < menu->entry_count; ++index) {
        const struct installer_dual_boot_menu_entry *entry =
            &menu->entries[index];

        if (!entry->present ||
            entry->label[0] == '\0') {
            return INSTALLER_DUAL_BOOT_BAD_STATE;
        }
        installer_dual_boot_append_text(
            text, capacity, &length, "> ");
        installer_dual_boot_append_text(
            text, capacity, &length, entry->label);
        installer_dual_boot_append_text(
            text, capacity, &length, "\n");
    }
    installer_dual_boot_append_text(
        text, capacity, &length, "\nUp/Down Select  Enter Boot");
    return INSTALLER_DUAL_BOOT_OK;
}

const char *installer_dual_boot_status_string(
    enum installer_dual_boot_status status)
{
    switch (status) {
    case INSTALLER_DUAL_BOOT_OK:
        return "INSTALLER_DUAL_BOOT_OK";
    case INSTALLER_DUAL_BOOT_NULL:
        return "INSTALLER_DUAL_BOOT_NULL";
    case INSTALLER_DUAL_BOOT_BAD_STATE:
        return "INSTALLER_DUAL_BOOT_BAD_STATE";
    case INSTALLER_DUAL_BOOT_BAD_VERSION:
        return "INSTALLER_DUAL_BOOT_BAD_VERSION";
    case INSTALLER_DUAL_BOOT_INVALID_ARGUMENT:
        return "INSTALLER_DUAL_BOOT_INVALID_ARGUMENT";
    case INSTALLER_DUAL_BOOT_OUT_OF_RANGE:
        return "INSTALLER_DUAL_BOOT_OUT_OF_RANGE";
    case INSTALLER_DUAL_BOOT_AMBIGUOUS:
        return "INSTALLER_DUAL_BOOT_AMBIGUOUS";
    case INSTALLER_DUAL_BOOT_CAPACITY:
        return "INSTALLER_DUAL_BOOT_CAPACITY";
    case INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE:
        return "INSTALLER_DUAL_BOOT_UNSUPPORTED_FIRMWARE";
    case INSTALLER_DUAL_BOOT_NO_EXISTING_OS:
        return "INSTALLER_DUAL_BOOT_NO_EXISTING_OS";
    case INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION:
        return "INSTALLER_DUAL_BOOT_NO_EFI_SYSTEM_PARTITION";
    case INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE:
        return "INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE";
    case INSTALLER_DUAL_BOOT_UNKNOWN_BOOT_PATH:
        return "INSTALLER_DUAL_BOOT_UNKNOWN_BOOT_PATH";
    case INSTALLER_DUAL_BOOT_READ_ONLY_VIOLATION:
        return "INSTALLER_DUAL_BOOT_READ_ONLY_VIOLATION";
    case INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED:
        return "INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED";
    default:
        return NULL;
    }
}
