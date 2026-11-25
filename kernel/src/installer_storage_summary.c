#include "installer_storage_summary.h"

#include <stddef.h>
#include <string.h>

#include "appliance_language.h"
#include "installer_layout_contract.h"
#include "installer_storage.h"

/*
 * ITEM-514: the summary page.
 *
 * The page is built from the same `installer_layout_contract_*` planner the
 * installer executes, not from a parallel calculation: the scheme, the disk
 * and the released partition are handed to `_plan_clean`, `_plan_dual_boot` or
 * `_plan_release` - exactly the calls `production_installer_plan_layout()`
 * makes - and the contracted summary that comes back is what the page renders.
 * "What is shown is what is written" is therefore a property of the call graph
 * and not of two implementations that agree today.
 *
 * This module reads and renders; it writes nothing and reaches no device.
 */

static const struct installer_discovery_disk *installer_storage_summary_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    uint32_t index;

    if (report == NULL) {
        return NULL;
    }
    for (index = 0U; index < report->disk_count &&
                     index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (report->disks[index].present && report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

/* True when the discovered system's partition is on the named disk. */
static bool installer_storage_summary_os_on_disk(
    const struct installer_discovery_report *report,
    const struct installer_discovery_os *os,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->id == os->partition_id) {
            return partition->disk_id == disk_id;
        }
    }
    return false;
}

static const char *installer_storage_summary_role_key(
    enum installer_layout_role role)
{
    switch (role) {
    case INSTALLER_LAYOUT_ROLE_EFI_SYSTEM:
        return "installer.scheme.role.efi";
    case INSTALLER_LAYOUT_ROLE_HFOC_SYSTEM:
        return "installer.scheme.role.system";
    case INSTALLER_LAYOUT_ROLE_HFOC_PERSISTENCE:
        return "installer.scheme.role.persistence";
    case INSTALLER_LAYOUT_ROLE_HFOC_RECOVERY:
        return "installer.scheme.role.recovery";
    /*
     * ITEM-517: the legacy-BIOS roles.  They are named in the created list
     * like every other role; neither of them is an ESP.
     */
    case INSTALLER_LAYOUT_ROLE_BIOS_BOOT:
        return "installer.scheme.role.bios_boot";
    case INSTALLER_LAYOUT_ROLE_BOOT_VOLUME:
        return "installer.scheme.role.boot_volume";
    default:
        return NULL;
    }
}

const char *installer_storage_summary_status_name(
    enum installer_storage_summary_status status)
{
    switch (status) {
    case INSTALLER_STORAGE_SUMMARY_OK:
        return "OK";
    case INSTALLER_STORAGE_SUMMARY_REFUSED:
        return "REFUSED";
    case INSTALLER_STORAGE_SUMMARY_INVALID:
    default:
        return "INVALID";
    }
}

void installer_storage_summary_build(
    struct installer_storage_summary *summary,
    const struct installer_discovery_report *report,
    uint32_t disk_id,
    enum installer_storage_scheme declared,
    enum installer_storage_scheme scheme,
    uint32_t released_index)
{
    struct installer_layout_plan plan;
    struct installer_dual_boot_plan dual_boot_plan;
    enum installer_layout_contract_status status;
    uint32_t index;

    if (summary == NULL) {
        return;
    }
    memset(summary, 0, sizeof(*summary));
    summary->disk_id = disk_id;
    summary->declared = declared;
    summary->scheme = scheme;
    summary->released_index = released_index;
    if (report == NULL ||
        installer_storage_summary_disk(report, disk_id) == NULL) {
        summary->status = INSTALLER_STORAGE_SUMMARY_INVALID;
        summary->contract_status = INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
        return;
    }
    /*
     * "Kept" means survives this install.  A preserving scheme keeps every
     * discovered system, the target disk's included; the erase scheme deletes
     * the systems on the target disk, so counting them as kept would
     * contradict its own "everything on this disk" line.
     */
    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        if (!report->oss[index].present) {
            continue;
        }
        if (scheme == INSTALLER_STORAGE_SCHEME_ERASE &&
            installer_storage_summary_os_on_disk(report, &report->oss[index],
                                                 disk_id)) {
            continue;
        }
        summary->existing_system_count += 1U;
    }
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ERASE:
        /*
         * ITEM-517: the erase scheme is the whole-disk planner, and the
         * firmware mode selects which one.  A legacy-BIOS machine is shown the
         * BIOS layout - BIOS boot partition plus FAT boot volume, no ESP -
         * exactly the plan the installer will execute.
         */
        status = report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY
                     ? installer_layout_contract_plan_clean_bios(
                           report, disk_id, &plan, &summary->layout)
                     : installer_layout_contract_plan_clean(
                           report, disk_id, &plan, &summary->layout);
        break;
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        /*
         * ITEM-517: the preserving schemes reuse the previous system's ESP and
         * are UEFI-only; a legacy-BIOS machine has no such chain and the page
         * must say the plan cannot be built instead of drawing an ESP the
         * install would never create.
         */
        if (report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
            summary->status = INSTALLER_STORAGE_SUMMARY_REFUSED;
            summary->contract_status =
                INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
            return;
        }
        status = installer_layout_contract_plan_dual_boot(
            report, disk_id, &dual_boot_plan, &summary->layout);
        break;
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        if (released_index == 0U) {
            summary->status = INSTALLER_STORAGE_SUMMARY_INVALID;
            summary->contract_status =
                INSTALLER_LAYOUT_CONTRACT_INVALID_ARGUMENT;
            return;
        }
        if (report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
            summary->status = INSTALLER_STORAGE_SUMMARY_REFUSED;
            summary->contract_status =
                INSTALLER_LAYOUT_CONTRACT_PLAN_REJECTED;
            return;
        }
        status = installer_layout_contract_plan_release(
            report, disk_id, released_index, &dual_boot_plan,
            &summary->layout);
        break;
    case INSTALLER_STORAGE_SCHEME_NONE:
    default:
        summary->status = INSTALLER_STORAGE_SUMMARY_INVALID;
        summary->contract_status = INSTALLER_LAYOUT_CONTRACT_BAD_MODE;
        return;
    }
    if (status == INSTALLER_LAYOUT_CONTRACT_OK) {
        summary->status = INSTALLER_STORAGE_SUMMARY_OK;
        summary->contract_status = INSTALLER_LAYOUT_CONTRACT_OK;
        return;
    }
    summary->status = INSTALLER_STORAGE_SUMMARY_REFUSED;
    summary->contract_status = status;
}

/* The scheme's name in the catalog, or NULL when the scheme is not one. */
static const char *installer_storage_summary_scheme_name(
    enum installer_storage_scheme scheme,
    enum appliance_language language)
{
    switch (scheme) {
    case INSTALLER_STORAGE_SCHEME_ALONGSIDE:
        return appliance_language_string(language, "installer.scheme.alongside");
    case INSTALLER_STORAGE_SCHEME_FREE_SPACE:
        return appliance_language_string(language, "installer.scheme.free_space");
    case INSTALLER_STORAGE_SCHEME_ERASE:
        return appliance_language_string(language, "installer.scheme.erase");
    case INSTALLER_STORAGE_SCHEME_MANUAL:
        return appliance_language_string(language, "installer.scheme.manual");
    case INSTALLER_STORAGE_SCHEME_NONE:
    default:
        return NULL;
    }
}

size_t installer_storage_summary_lines(
    const struct installer_storage_summary *summary,
    const struct installer_discovery_report *report,
    enum appliance_language language,
    char lines[INSTALLER_STORAGE_LINES_MAX][INSTALLER_STORAGE_LINE_BYTES])
{
    const struct installer_discovery_disk *disk;
    const char *title;
    const char *scheme_prefix;
    const char *declared_prefix;
    const char *chosen_prefix;
    const char *disk_prefix;
    const char *deleted_prefix;
    const char *delete_all;
    const char *delete_none;
    const char *delete_partition;
    const char *created_prefix;
    const char *role_prefix;
    const char *reused_suffix;
    const char *esp_reused;
    const char *esp_created;
    const char *kept_prefix;
    const char *menu_added;
    const char *free_left_prefix;
    const char *mb_suffix;
    const char *not_written_yet;
    const char *footer;
    const char *refused_prefix;
    size_t count = 0U;
    uint32_t index;

    if (summary == NULL || lines == NULL) {
        return 0U;
    }
    title = appliance_language_string(language,
                                      "installer.scheme.summary_title");
    scheme_prefix = appliance_language_string(
        language, "installer.scheme.summary_scheme_prefix");
    declared_prefix = appliance_language_string(
        language, "installer.scheme.summary_declared_prefix");
    chosen_prefix = appliance_language_string(
        language, "installer.scheme.summary_chosen_prefix");
    disk_prefix = appliance_language_string(language,
                                            "installer.storage.disk_prefix");
    deleted_prefix = appliance_language_string(
        language, "installer.scheme.summary_deleted_prefix");
    delete_all = appliance_language_string(
        language, "installer.scheme.summary_delete_all");
    delete_none = appliance_language_string(
        language, "installer.scheme.summary_delete_none");
    delete_partition = appliance_language_string(
        language, "installer.scheme.summary_delete_partition");
    created_prefix = appliance_language_string(
        language, "installer.scheme.summary_created_prefix");
    role_prefix = appliance_language_string(
        language, "installer.scheme.summary_role_prefix");
    reused_suffix = appliance_language_string(
        language, "installer.scheme.summary_reused");
    esp_reused = appliance_language_string(
        language, "installer.scheme.summary_esp_reused");
    esp_created = appliance_language_string(
        language, "installer.scheme.summary_esp_created");
    kept_prefix = appliance_language_string(
        language, "installer.scheme.summary_kept_prefix");
    menu_added = appliance_language_string(
        language, "installer.scheme.summary_menu_added");
    free_left_prefix = appliance_language_string(
        language, "installer.scheme.summary_free_left_prefix");
    mb_suffix = appliance_language_string(language,
                                          "installer.storage.mb_suffix");
    not_written_yet = appliance_language_string(
        language, "installer.storage.not_written_yet");
    footer = appliance_language_string(language, "installer.scheme.summary_footer");
    refused_prefix = appliance_language_string(
        language, "installer.scheme.summary_refused_prefix");
    if (title == NULL || scheme_prefix == NULL || declared_prefix == NULL ||
        chosen_prefix == NULL || disk_prefix == NULL || deleted_prefix == NULL ||
        delete_all == NULL || delete_none == NULL || delete_partition == NULL ||
        created_prefix == NULL || role_prefix == NULL || reused_suffix == NULL ||
        esp_reused == NULL || esp_created == NULL || kept_prefix == NULL ||
        menu_added == NULL || free_left_prefix == NULL || mb_suffix == NULL ||
        not_written_yet == NULL || footer == NULL || refused_prefix == NULL) {
        struct installer_storage_line line;

        installer_storage_line_reset(&line, lines[0],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, "INSTALLER TRANSLATION MISSING");
        return 1U;
    }
    {
        struct installer_storage_line line;
        const char *name =
            installer_storage_summary_scheme_name(summary->scheme, language);

        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, title);
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, scheme_prefix);
        installer_storage_line_append(&line, name != NULL ? name : "UNKNOWN");
        /*
         * The two sources go on their own line: the scheme's own sentence is
         * long enough that one combined line would pass the 128-byte budget in
         * the Russian column, and a truncated line is half a page.
         */
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, declared_prefix);
        installer_storage_line_append(
            &line, installer_storage_scheme_text(summary->declared));
        installer_storage_line_append(&line, chosen_prefix);
        installer_storage_line_append(
            &line, installer_storage_scheme_text(summary->scheme));
    }
    disk = installer_storage_summary_disk(report, summary->disk_id);
    if (disk != NULL) {
        char size_text[INSTALLER_STORAGE_TEXT_BYTES];
        struct installer_storage_line line;
        const uint64_t bytes =
            (uint64_t)disk->block_size * (uint64_t)disk->block_count;

        installer_storage_size_text(size_text, sizeof(size_text), bytes);
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, disk_prefix);
        installer_storage_line_append(
            &line, disk->model[0] != '\0' ? disk->model : disk->identity);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(&line, size_text);
        installer_storage_line_append(&line, "  ");
        installer_storage_line_append(
            &line, installer_storage_table_name(disk->partition_table));
    }
    if (summary->status == INSTALLER_STORAGE_SUMMARY_OK) {
        struct installer_storage_line line;

        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, deleted_prefix);
        if (summary->scheme == INSTALLER_STORAGE_SCHEME_ERASE) {
            installer_storage_line_append(&line, delete_all);
        } else if (summary->scheme == INSTALLER_STORAGE_SCHEME_MANUAL) {
            installer_storage_line_append(&line, delete_partition);
            installer_storage_line_u64(&line, summary->released_index);
            installer_storage_line_append(&line, "  ");
            installer_storage_line_u64(
                &line, installer_layout_contract_mib(
                           summary->layout.released_length_lba));
            installer_storage_line_append(&line, mb_suffix);
        } else {
            installer_storage_line_append(&line, delete_none);
        }
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, created_prefix);
        for (index = 0U; index < INSTALLER_LAYOUT_PARTITION_CAPACITY;
             ++index) {
            const struct installer_layout_contract_role *role =
                &summary->layout.roles[index];
            const char *role_key;
            const char *role_name;

            if (!role->present) {
                continue;
            }
            role_key = installer_storage_summary_role_key(role->role);
            role_name = role_key != NULL
                            ? appliance_language_string(language, role_key)
                            : NULL;
            installer_storage_line_reset(&line, lines[count++],
                                         INSTALLER_STORAGE_LINE_BYTES);
            installer_storage_line_append(&line, role_prefix);
            installer_storage_line_append(
                &line,
                role_name != NULL
                    ? role_name
                    : installer_layout_contract_role_string(role->role));
            installer_storage_line_append(&line, "  ");
            installer_storage_line_u64(
                &line, installer_layout_contract_mib(role->length_lba));
            installer_storage_line_append(&line, mb_suffix);
            if (role->reused) {
                installer_storage_line_append(&line, reused_suffix);
            }
        }
        /*
         * One fact per line: the ESP, then what the menu keeps and adds.  The
         * combined sentence passed the English budget but not the Russian one.
         *
         * ITEM-517: a legacy-BIOS layout has no ESP at all, so promising one
         * here would describe an install this plan does not perform.  The BIOS
         * boot partition and the FAT boot volume are already named in the
         * created-roles list above, and that list is the whole boot story.
         */
        if (summary->layout.mode !=
            INSTALLER_LAYOUT_CONTRACT_MODE_CLEAN_WHOLE_DISK_BIOS) {
            installer_storage_line_reset(&line, lines[count++],
                                         INSTALLER_STORAGE_LINE_BYTES);
            installer_storage_line_append(
                &line,
                summary->layout.esp_reused ? esp_reused : esp_created);
        }
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, kept_prefix);
        installer_storage_line_u64(
            &line, (uint64_t)summary->existing_system_count);
        installer_storage_line_append(&line, menu_added);
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, free_left_prefix);
        installer_storage_line_u64(
            &line, installer_layout_contract_mib(
                       summary->layout.free_length_lba));
        installer_storage_line_append(&line, mb_suffix);
    } else {
        struct installer_storage_line line;

        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, refused_prefix);
        installer_storage_line_append(
            &line,
            installer_layout_contract_status_string(
                summary->contract_status) != NULL
                ? installer_layout_contract_status_string(
                      summary->contract_status)
                : installer_storage_summary_status_name(summary->status));
    }
    {
        struct installer_storage_line line;

        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, not_written_yet);
        installer_storage_line_reset(&line, lines[count++],
                                     INSTALLER_STORAGE_LINE_BYTES);
        installer_storage_line_append(&line, footer);
    }
    return count;
}

size_t installer_storage_summary_record(
    const struct installer_storage_summary *summary,
    char *buffer,
    size_t size)
{
    struct installer_storage_line line;
    char record[INSTALLER_STORAGE_RECORD_BYTES + 1U];
    const char *deleted;

    if (summary == NULL || buffer == NULL || size == 0U) {
        return 0U;
    }
    deleted = summary->scheme == INSTALLER_STORAGE_SCHEME_ERASE
                  ? "whole-disk"
                  : summary->scheme == INSTALLER_STORAGE_SCHEME_MANUAL
                        ? "released-partition"
                        : "none";
    installer_storage_line_reset(&line, record, sizeof(record));
    installer_storage_line_append(&line, "[INSTALLER-STORAGE] summary disk=");
    installer_storage_line_u64(&line, summary->disk_id);
    installer_storage_line_append(&line, " scheme=");
    installer_storage_line_append(
        &line, installer_storage_scheme_name(summary->scheme));
    installer_storage_line_append(&line, " declared=");
    installer_storage_line_append(
        &line, installer_storage_scheme_name(summary->declared));
    installer_storage_line_append(&line, " status=");
    installer_storage_line_append(
        &line, installer_storage_summary_status_name(summary->status));
    installer_storage_line_append(&line, " contract=");
    installer_storage_line_append(
        &line,
        installer_layout_contract_status_string(summary->contract_status) !=
                NULL
            ? installer_layout_contract_status_string(
                  summary->contract_status)
            : "NONE");
    installer_storage_line_append(&line, " deleted=");
    installer_storage_line_append(&line, deleted);
    if (summary->scheme == INSTALLER_STORAGE_SCHEME_MANUAL) {
        installer_storage_line_append(&line, " released_index=");
        installer_storage_line_u64(&line, summary->released_index);
    }
    installer_storage_line_append(&line, " esp_reused=");
    installer_storage_line_append(
        &line, summary->layout.esp_reused ? "1" : "0");
    installer_storage_line_append(&line, " roles=");
    installer_storage_line_u64(&line, (uint64_t)summary->layout.role_count);
    installer_storage_line_append(&line, " kept_systems=");
    installer_storage_line_u64(&line, (uint64_t)summary->existing_system_count);
    installer_storage_line_append(&line, " writes=0");
    if (line.length == 0U || line.length > INSTALLER_STORAGE_RECORD_BYTES ||
        line.length + 1U > size) {
        return 0U;
    }
    memcpy(buffer, record, line.length + 1U);
    return line.length;
}
