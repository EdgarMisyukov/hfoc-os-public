#include "installer_layout_contract.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-391 product install layout contract.
 *
 * This module owns the four declared role sizes and turns them into a plan
 * through the existing ITEM-340 planner (`installer_layout_plan_whole_disk`)
 * and, for a dual-boot target, through the existing ITEM-341 planner
 * (`installer_dual_boot_plan_selected`).  It adds no second planner: every
 * number it returns was produced or validated by an existing module, and the
 * summary it publishes is the same arithmetic the plan carries.
 */

static const struct {
    enum installer_layout_role role;
    uint64_t length_lba;
} installer_layout_contract_roles[
    INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT] = {
    { INSTALLER_LAYOUT_ROLE_EFI_SYSTEM,
      INSTALLER_LAYOUT_CONTRACT_EFI_SYSTEM_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM,
      INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE,
      INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
      INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA },
};

/*
 * ITEM-517: the legacy-BIOS role table, in the exact order
 * `installer_layout_plan_whole_disk_bios()` places them (BIOS boot first, then
 * the FAT boot volume the payload is staged on, then the three HFOC roles).
 * There is no EFI System Partition here on purpose.
 */
static const struct {
    enum installer_layout_role role;
    uint64_t length_lba;
} installer_layout_contract_bios_roles[
    INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT] = {
    { INSTALLER_LAYOUT_ROLE_BIOS_BOOT,
      INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_BOOT_VOLUME,
      INSTALLER_LAYOUT_CONTRACT_BIOS_BOOT_VOLUME_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM,
      INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE,
      INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA },
    { INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY,
      INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA },
};

uint64_t installer_layout_contract_mib(uint64_t length_lba)
{
    return length_lba / (uint64_t)INSTALLER_LAYOUT_CONTRACT_MIB_LBA;
}

const char *installer_layout_contract_role_string(
    enum installer_layout_role role)
{
    switch (role) {
    case INSTALLER_LAYOUT_ROLE_EFI_SYSTEM:
        return "efi-system";
    case INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM:
        return "hfoc-system";
    case INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE:
        return "hfoc-persistence";
    case INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY:
        return "hfoc-recovery";
    case INSTALLER_LAYOUT_ROLE_BIOS_BOOT:
        return "bios-boot";
    case INSTALLER_LAYOUT_ROLE_BOOT_VOLUME:
        return "boot-volume";
    default:
        return NULL;
    }
}

const char *installer_layout_contract_mode_string(
    enum installer_layout_contract_mode mode)
{
    switch (mode) {
    case INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK:
        return "clean-whole-disk";
    case INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT:
        return "dual-boot";
    case INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE:
        return "release";
    case INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS:
        return "clean-whole-disk-bios";
    default:
        return NULL;
    }
}

const char *installer_layout_contract_status_string(
    enum installer_layout_contract_status status)
{
    switch (status) {
    case INSTALLER_LAYOUT_CONTRACT_OK:
        return "INSTALLER_LAYOUT_CONTRACT_OK";
    case INSTALLER_LAYOUT_CONTRACT_NULL:
        return "INSTALLER_LAYOUT_CONTRACT_NULL";
    case INSTALLER_LAYOUT_CONTRACT_BAD_VERSION:
        return "INSTALLER_LAYOUT_CONTRACT_BAD_VERSION";
    case INSTALLER_LAYOUT_CONTRACT_BAD_MODE:
        return "INSTALLER_LAYOUT_CONTRACT_BAD_MODE";
    case INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT:
        return "INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT";
    case INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL:
        return "INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL";
    case INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP:
        return "INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP";
    case INSTALLER_LAYOUT_CONTRACT_NO_RESERVE:
        return "INSTALLER_LAYOUT_CONTRACT_NO_RESERVE";
    case INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED:
        return "INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED";
    default:
        return NULL;
    }
}

static const struct installer_discovery_disk *
installer_layout_contract_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (report == NULL ||
        report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return NULL;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY; ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

static bool installer_layout_contract_existing_span(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t *count_out,
    uint64_t *first_out,
    uint64_t *length_out)
{
    uint32_t count = 0U;
    uint64_t first = 0U;
    uint64_t end = 0U;
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];
        uint64_t partition_end;

        if (!partition->present || partition->disk_id != disk_id) {
            continue;
        }
        if (partition->length_lba == 0U ||
            partition->first_lba >
                UINT64_MAX - partition->length_lba) {
            return false;
        }
        partition_end = partition->first_lba + partition->length_lba;
        if (count == 0U || partition->first_lba < first) {
            first = partition->first_lba;
        }
        if (partition_end > end) {
            end = partition_end;
        }
        ++count;
    }
    *count_out = count;
    *first_out = count == 0U ? 0U : first;
    *length_out = count == 0U ? 0U : end - first;
    return true;
}

static bool installer_layout_contract_intervals_overlap(
    uint64_t left_first,
    uint64_t left_length,
    uint64_t right_first,
    uint64_t right_length)
{
    if (left_length == 0U || right_length == 0U) {
        return false;
    }
    if (left_first > UINT64_MAX - left_length ||
        right_first > UINT64_MAX - right_length) {
        return true;
    }
    return left_first < right_first + right_length &&
           right_first < left_first + left_length;
}

static bool installer_layout_contract_existing_overlap(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint64_t first_lba,
    uint64_t length_lba)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (!partition->present || partition->disk_id != disk_id) {
            continue;
        }
        if (installer_layout_contract_intervals_overlap(
                partition->first_lba, partition->length_lba,
                first_lba, length_lba)) {
            return true;
        }
    }
    return false;
}

static enum installer_layout_contract_status
installer_layout_contract_summary_validate(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_contract_summary *summary)
{
    uint64_t cursor;
    uint64_t footprint = 0U;
    uint64_t usable_end;
    uint64_t expected_minimum;
    uint32_t index;

    if (summary->version != INSTALLER_LAYOUT_CONTRACT_VERSION) {
        return INSTALLER_LAYOUT_CONTRACT_BAD_VERSION;
    }
    if (report != NULL &&
        installer_layout_contract_find_disk(report, disk_id) == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (summary->mode !=
            INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK &&
        summary->mode != INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT &&
        summary->mode != INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE &&
        summary->mode !=
            INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS) {
        return INSTALLER_LAYOUT_CONTRACT_BAD_MODE;
    }
    if (summary->role_count == 0U ||
        summary->role_count > INSTALLER_LAYOUT_PARTITION_CAPACITY) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    /*
     * ITEM-517: a legacy-BIOS summary is measured against the BIOS floor, not
     * the UEFI one.  Every other mode keeps the UEFI floor it always had.
     */
    expected_minimum =
        summary->mode ==
                INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS
            ? INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA
            : INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA;
    if (summary->media_lba < expected_minimum ||
        summary->minimum_media_lba != expected_minimum ||
        summary->declared_lba == 0U) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    if (summary->media_lba < INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    usable_end =
        summary->media_lba - INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA;
    if (summary->first_lba != INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
        !((summary->mode ==
               INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT ||
           summary->mode ==
               INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE) &&
          summary->first_lba >= INSTALLER_LAYOUT_CONTRACT_FIRST_LBA &&
          summary->first_lba <= usable_end)) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    for (index = 0U; index < summary->role_count; ++index) {
        const struct installer_layout_contract_role *role =
            &summary->roles[index];

        if (!role->present || role->length_lba == 0U ||
            installer_layout_contract_role_string(role->role) == NULL ||
            role->first_lba % INSTALLER_LAYOUT_ALIGN_LBA != 0U ||
            role->first_lba < INSTALLER_LAYOUT_CONTRACT_FIRST_LBA ||
            role->first_lba > usable_end ||
            role->length_lba > usable_end - role->first_lba ||
            (role->reused &&
             summary->mode !=
                 INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT &&
             summary->mode !=
                 INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE)) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
    }
    for (index = 0U; index < summary->role_count; ++index) {
        const struct installer_layout_contract_role *role =
            &summary->roles[index];
        uint32_t other;

        for (other = index + 1U; other < summary->role_count; ++other) {
            if (summary->roles[other].role == role->role ||
                installer_layout_contract_intervals_overlap(
                    role->first_lba, role->length_lba,
                    summary->roles[other].first_lba,
                    summary->roles[other].length_lba)) {
                return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            }
        }
    }
    cursor = summary->first_lba;
    for (index = 0U; index < summary->role_count; ++index) {
        const struct installer_layout_contract_role *role =
            &summary->roles[index];

        if (role->reused) {
            continue;
        }
        if (role->first_lba != cursor ||
            footprint > UINT64_MAX - role->length_lba ||
            cursor > UINT64_MAX - role->length_lba) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
        footprint += role->length_lba;
        cursor += role->length_lba;
    }
    if (footprint == 0U || footprint != summary->footprint_lba ||
        summary->first_lba > UINT64_MAX - summary->footprint_lba) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (summary->free_first_lba !=
            summary->first_lba + summary->footprint_lba ||
        cursor != summary->free_first_lba ||
        summary->free_first_lba > usable_end) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE) {
        /*
         * ITEM-501: the room this layout may use ends with the partition it
         * gives up, not with the disk: the free region is the remainder of that
         * extent after the three roles.
         */
        uint64_t released_end;

        if (summary->released_length_lba == 0U ||
            summary->released_length_lba >
                UINT64_MAX - summary->released_first_lba) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
        released_end = summary->released_first_lba +
                       summary->released_length_lba;
        if (released_end > usable_end ||
            released_end < summary->free_first_lba ||
            summary->free_length_lba !=
                released_end - summary->free_first_lba) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
    } else if (summary->free_length_lba !=
               usable_end - summary->free_first_lba) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT ||
        summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE) {
        uint64_t existing_last;

        if (report == NULL || summary->existing_partition_count == 0U ||
            summary->existing_length_lba == 0U ||
            summary->existing_first_lba >
                UINT64_MAX - summary->existing_length_lba) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
        existing_last = summary->existing_first_lba +
                        summary->existing_length_lba;
        if (existing_last > usable_end) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
        for (index = 0U; index < summary->role_count; ++index) {
            const struct installer_layout_contract_role *role =
                &summary->roles[index];

            if (!role->reused) {
                continue;
            }
            if (role->first_lba < summary->existing_first_lba ||
                role->first_lba > existing_last ||
                role->length_lba > existing_last - role->first_lba) {
                return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            }
        }
        if (summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT) {
            if (installer_layout_contract_existing_overlap(
                    report, disk_id, summary->first_lba,
                    summary->footprint_lba)) {
                return INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP;
            }
        } else {
            /*
             * ITEM-501: exactly one partition is given up.  It must be on this
             * disk, it must be the extent the summary names, it must not be the
             * shared ESP or a container, and no other partition may overlap the
             * extent that is about to be written.  The three roles must lie
             * inside the extent that was released.
             */
            const struct installer_discovery_partition *released = NULL;
            uint64_t released_end =
                summary->released_first_lba +
                summary->released_length_lba;

            if (summary->released_index == 0U) {
                return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            }
            for (index = 0U;
                 index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
                 ++index) {
                const struct installer_discovery_partition *partition =
                    &report->partitions[index];

                if (!partition->present ||
                    partition->disk_id != disk_id) {
                    continue;
                }
                if (partition->partition_index ==
                    summary->released_index) {
                    if (partition->first_lba !=
                            summary->released_first_lba ||
                        partition->length_lba !=
                            summary->released_length_lba ||
                        partition->efi_system || partition->container) {
                        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
                    }
                    released = partition;
                    continue;
                }
                if (installer_layout_contract_intervals_overlap(
                        partition->first_lba, partition->length_lba,
                        summary->released_first_lba,
                        summary->released_length_lba)) {
                    return INSTALLER_LAYOUT_CONTRACT_EXISTING_SYSTEM_OVERLAP;
                }
            }
            if (released == NULL ||
                summary->first_lba < summary->released_first_lba) {
                return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            }
            for (index = 0U; index < summary->role_count; ++index) {
                const struct installer_layout_contract_role *role =
                    &summary->roles[index];

                if (role->reused) {
                    continue;
                }
                if (role->first_lba < summary->released_first_lba ||
                    role->first_lba >= released_end ||
                    role->length_lba >
                        released_end - role->first_lba) {
                    return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
                }
            }
        }
        if (summary->free_length_lba == 0U) {
            return INSTALLER_LAYOUT_CONTRACT_NO_RESERVE;
        }
    }
    return INSTALLER_LAYOUT_CONTRACT_OK;
}

enum installer_layout_contract_status
installer_layout_contract_validate_summary(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    const struct installer_layout_contract_summary *summary)
{
    if (summary == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    if (report == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    return installer_layout_contract_summary_validate(
        report, disk_id, summary);
}

static void installer_layout_contract_summary_init(
    struct installer_layout_contract_summary *summary,
    enum installer_layout_contract_mode mode,
    uint32_t disk_id,
    uint64_t media_lba)
{
    memset(summary, 0, sizeof(*summary));
    summary->version = INSTALLER_LAYOUT_CONTRACT_VERSION;
    summary->mode = mode;
    summary->disk_id = disk_id;
    summary->media_lba = media_lba;
    summary->minimum_media_lba =
        INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA;
    summary->declared_lba = INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    summary->first_lba = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA;
}

static enum installer_layout_contract_status
installer_layout_contract_finish(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_layout_contract_summary *summary)
{
    enum installer_layout_contract_status status;

    status = installer_layout_contract_summary_validate(
        report, disk_id, summary);
    if (status != INSTALLER_LAYOUT_CONTRACT_OK) {
        return status;
    }
    return INSTALLER_LAYOUT_CONTRACT_OK;
}

enum installer_layout_contract_status installer_layout_contract_plan_clean(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_layout_plan *plan,
    struct installer_layout_contract_summary *summary)
{
    struct installer_layout_partition requests[
        INSTALLER_LAYOUT_PARTITION_CAPACITY];
    const struct installer_discovery_disk *disk;
    enum installer_layout_status layout_status;
    uint64_t free_first;
    uint64_t free_length;
    size_t index;

    if (report == NULL || plan == NULL || summary == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    disk = installer_layout_contract_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (disk->block_count <
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    installer_layout_contract_summary_init(
        summary, INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK,
        disk_id, disk->block_count);
    memset(requests, 0, sizeof(requests));
    for (index = 0U;
         index < INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT;
         ++index) {
        requests[index].present = true;
        requests[index].role =
            installer_layout_contract_roles[index].role;
        requests[index].length_lba =
            installer_layout_contract_roles[index].length_lba;
    }
    layout_status = installer_layout_plan_whole_disk(
        report, disk_id, requests,
        INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT, plan);
    if (layout_status != INSTALLER_LAYOUT_OK) {
        summary->layout_status = layout_status;
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }

    free_first = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                 INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    free_length = disk->block_count -
                  INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA - free_first;
    summary->first_lba = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA;
    summary->footprint_lba = INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    summary->available_lba = free_length + INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    summary->required_lba = INSTALLER_LAYOUT_CONTRACT_FOOTPRINT_LBA;
    summary->free_first_lba = free_first;
    summary->free_length_lba = free_length;
    summary->role_count = INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT;
    for (index = 0U;
         index < INSTALLER_LAYOUT_CONTRACT_CLEAN_ROLE_COUNT;
         ++index) {
        summary->roles[index].present =
            plan->partitions[index].present;
        summary->roles[index].reused = false;
        summary->roles[index].role = plan->partitions[index].role;
        summary->roles[index].first_lba =
            plan->partitions[index].first_lba;
        summary->roles[index].length_lba =
            plan->partitions[index].length_lba;
    }
    return installer_layout_contract_finish(report, disk_id, summary);
}

enum installer_layout_contract_status
installer_layout_contract_plan_clean_bios(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_layout_plan *plan,
    struct installer_layout_contract_summary *summary)
{
    struct installer_layout_partition requests[
        INSTALLER_LAYOUT_PARTITION_CAPACITY];
    const struct installer_discovery_disk *disk;
    enum installer_layout_status layout_status;
    uint64_t free_first;
    uint64_t free_length;
    size_t index;

    if (report == NULL || plan == NULL || summary == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    disk = installer_layout_contract_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (disk->block_count <
            INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    installer_layout_contract_summary_init(
        summary,
        INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS,
        disk_id, disk->block_count);
    summary->minimum_media_lba =
        INSTALLER_LAYOUT_CONTRACT_BIOS_MINIMUM_MEDIA_LBA;
    summary->declared_lba =
        INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA;
    memset(requests, 0, sizeof(requests));
    for (index = 0U;
         index < INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT;
         ++index) {
        requests[index].present = true;
        requests[index].role =
            installer_layout_contract_bios_roles[index].role;
        requests[index].length_lba =
            installer_layout_contract_bios_roles[index].length_lba;
    }
    layout_status = installer_layout_plan_whole_disk_bios(
        report, disk_id, requests,
        INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT, plan);
    if (layout_status != INSTALLER_LAYOUT_OK) {
        summary->layout_status = layout_status;
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }

    free_first = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA +
                 INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA;
    free_length = disk->block_count -
                  INSTALLER_LAYOUT_CONTRACT_GPT_TAIL_LBA - free_first;
    summary->first_lba = INSTALLER_LAYOUT_CONTRACT_FIRST_LBA;
    summary->footprint_lba =
        INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA;
    summary->available_lba =
        free_length + INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA;
    summary->required_lba =
        INSTALLER_LAYOUT_CONTRACT_BIOS_FOOTPRINT_LBA;
    summary->free_first_lba = free_first;
    summary->free_length_lba = free_length;
    summary->role_count = INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT;
    for (index = 0U;
         index < INSTALLER_LAYOUT_CONTRACT_BIOS_ROLE_COUNT;
         ++index) {
        summary->roles[index].present =
            plan->partitions[index].present;
        summary->roles[index].reused = false;
        summary->roles[index].role = plan->partitions[index].role;
        summary->roles[index].first_lba =
            plan->partitions[index].first_lba;
        summary->roles[index].length_lba =
            plan->partitions[index].length_lba;
    }
    return installer_layout_contract_finish(report, disk_id, summary);
}

enum installer_layout_contract_status
installer_layout_contract_dual_boot_requirements(
    struct installer_dual_boot_requirements *requirements)
{
    if (requirements == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    memset(requirements, 0, sizeof(*requirements));
    requirements->version = INSTALLER_DUAL_BOOT_REQUIREMENTS_VERSION;
    requirements->system_length_lba =
        INSTALLER_LAYOUT_CONTRACT_HFOC_SYSTEM_LENGTH_LBA;
    requirements->persistence_length_lba =
        INSTALLER_LAYOUT_CONTRACT_HFOC_PERSISTENCE_LENGTH_LBA;
    requirements->recovery_length_lba =
        INSTALLER_LAYOUT_CONTRACT_HFOC_RECOVERY_LENGTH_LBA;
    return INSTALLER_LAYOUT_CONTRACT_OK;
}

enum installer_layout_contract_status
installer_layout_contract_plan_dual_boot(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    struct installer_dual_boot_plan *dual_boot_plan,
    struct installer_layout_contract_summary *summary)
{
    struct installer_dual_boot_requirements requirements;
    const struct installer_discovery_disk *disk;
    const struct installer_discovery_partition *esp = NULL;
    enum installer_dual_boot_status dual_status;
    uint64_t reserved_first;
    uint64_t reserved_end;
    uint32_t existing_count = 0U;
    uint64_t existing_first = 0U;
    uint64_t existing_length = 0U;
    size_t index;

    if (report == NULL || dual_boot_plan == NULL || summary == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    disk = installer_layout_contract_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (disk->block_count <
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            partition->efi_system) {
            esp = partition;
            break;
        }
    }
    if (esp == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }
    if (!installer_layout_contract_existing_span(
            report, disk_id, &existing_count,
            &existing_first, &existing_length)) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (installer_layout_contract_dual_boot_requirements(
            &requirements) != INSTALLER_LAYOUT_CONTRACT_OK) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    memset(dual_boot_plan, 0, sizeof(*dual_boot_plan));
    dual_status = installer_dual_boot_plan_selected(
        report, disk_id, &requirements, dual_boot_plan);
    if (dual_status != INSTALLER_DUAL_BOOT_OK) {
        if (dual_status == INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE) {
            return INSTALLER_LAYOUT_CONTRACT_NO_RESERVE;
        }
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }

    installer_layout_contract_summary_init(
        summary, INSTALLER_LAYOUT_CONTRACT_MODE_DUAL_BOOT,
        disk_id, disk->block_count);
    summary->esp_reused = true;
    summary->first_lba = dual_boot_plan->system_first_lba;
    summary->footprint_lba =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA;
    summary->existing_partition_count = existing_count;
    summary->existing_first_lba = existing_first;
    summary->existing_length_lba = existing_length;
    summary->role_count =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_ROLE_COUNT;
    summary->roles[0U].present = true;
    summary->roles[0U].reused = true;
    summary->roles[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    summary->roles[0U].first_lba = esp->first_lba;
    summary->roles[0U].length_lba = esp->length_lba;
    summary->roles[1U].present = true;
    summary->roles[1U].reused = false;
    summary->roles[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    summary->roles[1U].first_lba = dual_boot_plan->system_first_lba;
    summary->roles[1U].length_lba = dual_boot_plan->system_length_lba;
    summary->roles[2U].present = true;
    summary->roles[2U].reused = false;
    summary->roles[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    summary->roles[2U].first_lba =
        dual_boot_plan->persistence_first_lba;
    summary->roles[2U].length_lba =
        dual_boot_plan->persistence_length_lba;
    summary->roles[3U].present = true;
    summary->roles[3U].reused = false;
    summary->roles[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    summary->roles[3U].first_lba = dual_boot_plan->recovery_first_lba;
    summary->roles[3U].length_lba = dual_boot_plan->recovery_length_lba;
    reserved_first = dual_boot_plan->recovery_first_lba +
                     dual_boot_plan->recovery_length_lba;
    reserved_end = dual_boot_plan->free_first_lba +
                   dual_boot_plan->free_length_lba;
    if (reserved_first > reserved_end) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    summary->free_first_lba = reserved_first;
    summary->free_length_lba = reserved_end - reserved_first;
    summary->available_lba = dual_boot_plan->free_length_lba;
    summary->required_lba =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA;
    if (summary->free_length_lba == 0U) {
        return INSTALLER_LAYOUT_CONTRACT_NO_RESERVE;
    }
    return installer_layout_contract_finish(report, disk_id, summary);
}

enum installer_layout_contract_status
installer_layout_contract_plan_release(
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    uint32_t partition_index,
    struct installer_dual_boot_plan *dual_boot_plan,
    struct installer_layout_contract_summary *summary)
{
    struct installer_dual_boot_requirements requirements;
    const struct installer_discovery_disk *disk;
    const struct installer_discovery_partition *esp = NULL;
    enum installer_dual_boot_status dual_status;
    uint64_t reserved_first;
    uint64_t released_end;
    uint32_t existing_count = 0U;
    uint64_t existing_first = 0U;
    uint64_t existing_length = 0U;
    size_t index;

    if (report == NULL || dual_boot_plan == NULL || summary == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    disk = installer_layout_contract_find_disk(report, disk_id);
    if (disk == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (disk->block_count <
            INSTALLER_LAYOUT_CONTRACT_MINIMUM_MEDIA_LBA) {
        return INSTALLER_LAYOUT_CONTRACT_MEDIA_TOO_SMALL;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            partition->efi_system) {
            esp = partition;
            break;
        }
    }
    if (esp == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }
    if (!installer_layout_contract_existing_span(
            report, disk_id, &existing_count,
            &existing_first, &existing_length)) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    if (installer_layout_contract_dual_boot_requirements(
            &requirements) != INSTALLER_LAYOUT_CONTRACT_OK) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    memset(dual_boot_plan, 0, sizeof(*dual_boot_plan));
    dual_status = installer_dual_boot_plan_release(
        report, disk_id, partition_index, &requirements, dual_boot_plan);
    if (dual_status != INSTALLER_DUAL_BOOT_OK) {
        if (dual_status == INSTALLER_DUAL_BOOT_INSUFFICIENT_SPACE) {
            return INSTALLER_LAYOUT_CONTRACT_NO_RESERVE;
        }
        if (dual_status == INSTALLER_DUAL_BOOT_PARTITION_UNSUPPORTED ||
            dual_status == INSTALLER_DUAL_BOOT_INVALID_ARGUMENT) {
            return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
        }
        return INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
    }

    installer_layout_contract_summary_init(
        summary, INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE,
        disk_id, disk->block_count);
    summary->esp_reused = true;
    summary->first_lba = dual_boot_plan->system_first_lba;
    summary->footprint_lba =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA;
    summary->available_lba = dual_boot_plan->free_length_lba;
    summary->required_lba =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_FOOTPRINT_LBA;
    summary->released_index = dual_boot_plan->released_index;
    summary->released_first_lba = dual_boot_plan->released_first_lba;
    summary->released_length_lba = dual_boot_plan->released_length_lba;
    summary->existing_partition_count = existing_count;
    summary->existing_first_lba = existing_first;
    summary->existing_length_lba = existing_length;
    summary->role_count =
        INSTALLER_LAYOUT_CONTRACT_DUAL_BOOT_ROLE_COUNT;
    summary->roles[0U].present = true;
    summary->roles[0U].reused = true;
    summary->roles[0U].role = INSTALLER_LAYOUT_ROLE_EFI_SYSTEM;
    summary->roles[0U].first_lba = esp->first_lba;
    summary->roles[0U].length_lba = esp->length_lba;
    summary->roles[1U].present = true;
    summary->roles[1U].reused = false;
    summary->roles[1U].role = INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM;
    summary->roles[1U].first_lba = dual_boot_plan->system_first_lba;
    summary->roles[1U].length_lba = dual_boot_plan->system_length_lba;
    summary->roles[2U].present = true;
    summary->roles[2U].reused = false;
    summary->roles[2U].role = INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE;
    summary->roles[2U].first_lba =
        dual_boot_plan->persistence_first_lba;
    summary->roles[2U].length_lba =
        dual_boot_plan->persistence_length_lba;
    summary->roles[3U].present = true;
    summary->roles[3U].reused = false;
    summary->roles[3U].role = INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY;
    summary->roles[3U].first_lba = dual_boot_plan->recovery_first_lba;
    summary->roles[3U].length_lba = dual_boot_plan->recovery_length_lba;
    reserved_first = dual_boot_plan->recovery_first_lba +
                     dual_boot_plan->recovery_length_lba;
    if (dual_boot_plan->released_length_lba >
            UINT64_MAX - dual_boot_plan->released_first_lba) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    released_end = dual_boot_plan->released_first_lba +
                   dual_boot_plan->released_length_lba;
    if (reserved_first > released_end) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    summary->free_first_lba = reserved_first;
    summary->free_length_lba = released_end - reserved_first;
    if (summary->free_length_lba == 0U) {
        return INSTALLER_LAYOUT_CONTRACT_NO_RESERVE;
    }
    return installer_layout_contract_finish(report, disk_id, summary);
}

/* ------------------------------------------------------------- rendering -- */

struct installer_layout_contract_text {
    char *buffer;
    size_t capacity;
    size_t length;
    bool full;
};

static void installer_layout_contract_append_char(
    struct installer_layout_contract_text *text,
    char value)
{
    if (text->length + 1U >= text->capacity) {
        text->full = true;
        return;
    }
    text->buffer[text->length++] = value;
    text->buffer[text->length] = '\0';
}

static void installer_layout_contract_append_text(
    struct installer_layout_contract_text *text,
    const char *value)
{
    size_t index;

    for (index = 0U; value[index] != '\0'; ++index) {
        installer_layout_contract_append_char(text, value[index]);
    }
}

static void installer_layout_contract_append_u64(
    struct installer_layout_contract_text *text,
    uint64_t value)
{
    char reversed[20U];
    size_t count = 0U;

    if (value == 0U) {
        installer_layout_contract_append_char(text, '0');
        return;
    }
    while (value != 0U && count < sizeof(reversed)) {
        reversed[count++] = (char)('0' + (value % 10U));
        value /= 10U;
    }
    while (count > 0U) {
        installer_layout_contract_append_char(
            text, reversed[--count]);
    }
}

static enum installer_layout_contract_status
installer_layout_contract_render_begin(
    const struct installer_layout_contract_summary *summary,
    char *buffer,
    size_t capacity,
    size_t *length,
    struct installer_layout_contract_text *text)
{
    if (summary == NULL || buffer == NULL || length == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_NULL;
    }
    if (summary->version != INSTALLER_LAYOUT_CONTRACT_VERSION) {
        return INSTALLER_LAYOUT_CONTRACT_BAD_VERSION;
    }
    if (capacity < 2U) {
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    text->buffer = buffer;
    text->capacity = capacity;
    text->length = 0U;
    text->full = false;
    buffer[0U] = '\0';
    *length = 0U;
    return INSTALLER_LAYOUT_CONTRACT_OK;
}

static enum installer_layout_contract_status
installer_layout_contract_render_finish(
    struct installer_layout_contract_text *text,
    size_t *length)
{
    if (text->full) {
        *length = 0U;
        return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
    }
    *length = text->length;
    return INSTALLER_LAYOUT_CONTRACT_OK;
}

enum installer_layout_contract_status
installer_layout_contract_render_record(
    const struct installer_layout_contract_summary *summary,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct installer_layout_contract_text text;
    enum installer_layout_contract_status status;
    uint32_t index;

    status = installer_layout_contract_render_begin(
        summary, buffer, capacity, length, &text);
    if (status != INSTALLER_LAYOUT_CONTRACT_OK) {
        return status;
    }
    if (installer_layout_contract_mode_string(summary->mode) == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_BAD_MODE;
    }
    installer_layout_contract_append_text(
        &text, "[INSTALLER-LAYOUT] plan mode=");
    installer_layout_contract_append_text(
        &text, installer_layout_contract_mode_string(summary->mode));
    installer_layout_contract_append_text(&text, " disk=");
    installer_layout_contract_append_u64(&text, summary->disk_id);
    installer_layout_contract_append_text(&text, " media_lba=");
    installer_layout_contract_append_u64(&text, summary->media_lba);
    installer_layout_contract_append_text(&text, " media_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->media_lba));
    installer_layout_contract_append_text(
        &text, " minimum_media_lba=");
    installer_layout_contract_append_u64(
        &text, summary->minimum_media_lba);
    installer_layout_contract_append_text(
        &text, " minimum_media_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(
                   summary->minimum_media_lba));
    installer_layout_contract_append_text(&text, " declared_lba=");
    installer_layout_contract_append_u64(
        &text, summary->declared_lba);
    installer_layout_contract_append_text(&text, " declared_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->declared_lba));
    installer_layout_contract_append_text(&text, " first_lba=");
    installer_layout_contract_append_u64(&text, summary->first_lba);
    installer_layout_contract_append_text(&text, " footprint_lba=");
    installer_layout_contract_append_u64(
        &text, summary->footprint_lba);
    installer_layout_contract_append_text(&text, " footprint_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(
                   summary->footprint_lba));
    installer_layout_contract_append_text(&text, " free_first_lba=");
    installer_layout_contract_append_u64(
        &text, summary->free_first_lba);
    installer_layout_contract_append_text(&text, " free_lba=");
    installer_layout_contract_append_u64(
        &text, summary->free_length_lba);
    installer_layout_contract_append_text(&text, " free_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(
                   summary->free_length_lba));
    /*
     * ITEM-501: the two numbers a refusal is made of - the room the previous
     * system offered and the room the appliance needs - plus the partition this
     * layout gives up, when it gives one up.
     */
    installer_layout_contract_append_text(&text, " available_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->available_lba));
    installer_layout_contract_append_text(&text, " required_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->required_lba));
    if (summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE) {
        installer_layout_contract_append_text(&text, " released_index=");
        installer_layout_contract_append_u64(
            &text, summary->released_index);
        installer_layout_contract_append_text(
            &text, " released_first_lba=");
        installer_layout_contract_append_u64(
            &text, summary->released_first_lba);
        installer_layout_contract_append_text(&text, " released_lba=");
        installer_layout_contract_append_u64(
            &text, summary->released_length_lba);
    }
    installer_layout_contract_append_text(
        &text, " existing_partitions=");
    installer_layout_contract_append_u64(
        &text, summary->existing_partition_count);
    installer_layout_contract_append_text(
        &text, " existing_first_lba=");
    installer_layout_contract_append_u64(
        &text, summary->existing_first_lba);
    installer_layout_contract_append_text(&text, " existing_lba=");
    installer_layout_contract_append_u64(
        &text, summary->existing_length_lba);
    installer_layout_contract_append_text(
        &text, summary->esp_reused ? " esp_reused=1\n"
                                   : " esp_reused=0\n");
    for (index = 0U; index < summary->role_count; ++index) {
        const struct installer_layout_contract_role *role =
            &summary->roles[index];
        const char *role_text;

        if (!role->present) {
            continue;
        }
        role_text = installer_layout_contract_role_string(role->role);
        if (role_text == NULL) {
            return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        }
        installer_layout_contract_append_text(
            &text, "[INSTALLER-LAYOUT] role=");
        installer_layout_contract_append_text(&text, role_text);
        installer_layout_contract_append_text(&text, " first_lba=");
        installer_layout_contract_append_u64(&text, role->first_lba);
        installer_layout_contract_append_text(&text, " length_lba=");
        installer_layout_contract_append_u64(&text, role->length_lba);
        installer_layout_contract_append_text(&text, " length_mib=");
        installer_layout_contract_append_u64(
            &text, installer_layout_contract_mib(role->length_lba));
        installer_layout_contract_append_text(
            &text, role->reused ? " reused=1\n" : " reused=0\n");
    }
    return installer_layout_contract_render_finish(&text, length);
}

enum installer_layout_contract_status
installer_layout_contract_render_preview(
    const struct installer_layout_contract_summary *summary,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    struct installer_layout_contract_text text;
    enum installer_layout_contract_status status;
    uint32_t index;

    status = installer_layout_contract_render_begin(
        summary, buffer, capacity, length, &text);
    if (status != INSTALLER_LAYOUT_CONTRACT_OK) {
        return status;
    }
    if (installer_layout_contract_mode_string(summary->mode) == NULL) {
        return INSTALLER_LAYOUT_CONTRACT_BAD_MODE;
    }
    installer_layout_contract_append_text(&text, " mode=");
    installer_layout_contract_append_text(
        &text, installer_layout_contract_mode_string(summary->mode));
    installer_layout_contract_append_text(&text, " footprint_lba=");
    installer_layout_contract_append_u64(
        &text, summary->footprint_lba);
    installer_layout_contract_append_text(&text, " footprint_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(
                   summary->footprint_lba));
    installer_layout_contract_append_text(
        &text, " minimum_media_lba=");
    installer_layout_contract_append_u64(
        &text, summary->minimum_media_lba);
    installer_layout_contract_append_text(&text, " free_first_lba=");
    installer_layout_contract_append_u64(
        &text, summary->free_first_lba);
    installer_layout_contract_append_text(&text, " free_lba=");
    installer_layout_contract_append_u64(
        &text, summary->free_length_lba);
    installer_layout_contract_append_text(&text, " free_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(
                   summary->free_length_lba));
    installer_layout_contract_append_text(&text, " available_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->available_lba));
    installer_layout_contract_append_text(&text, " required_mib=");
    installer_layout_contract_append_u64(
        &text, installer_layout_contract_mib(summary->required_lba));
    if (summary->mode == INSTALLER_LAYOUT_CONTRACT_MODE_RELEASE) {
        installer_layout_contract_append_text(&text, " released_index=");
        installer_layout_contract_append_u64(
            &text, summary->released_index);
    }
    installer_layout_contract_append_text(&text, " roles=");
    {
        bool first = true;

        for (index = 0U; index < summary->role_count; ++index) {
            const struct installer_layout_contract_role *role =
                &summary->roles[index];
            const char *role_text;

            if (!role->present) {
                continue;
            }
            role_text = installer_layout_contract_role_string(
                role->role);
            if (role_text == NULL) {
                return INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            }
            if (!first) {
                installer_layout_contract_append_char(&text, ',');
            }
            first = false;
            installer_layout_contract_append_text(&text, role_text);
            installer_layout_contract_append_char(&text, ':');
            installer_layout_contract_append_u64(
                &text, role->first_lba);
            installer_layout_contract_append_char(&text, '+');
            installer_layout_contract_append_u64(
                &text, role->length_lba);
            installer_layout_contract_append_char(&text, '=');
            installer_layout_contract_append_u64(
                &text,
                installer_layout_contract_mib(role->length_lba));
            installer_layout_contract_append_text(&text, "mib");
        }
    }
    return installer_layout_contract_render_finish(&text, length);
}
