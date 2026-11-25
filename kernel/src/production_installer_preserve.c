#include "production_installer_preserve.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "console.h"
#include "serial.h"

static enum production_installer_status production_preserve_map_table_status(
    enum installer_gpt_preserve_status status);

static bool production_preserve_report_valid(
    const struct installer_discovery_report *report)
{
    return report != NULL &&
           report->version == INSTALLER_HARDWARE_DISCOVERY_VERSION &&
           (report->firmware_mode == INSTALLER_FIRMWARE_UEFI ||
            report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) &&
           report->read_only &&
           report->scan_complete;
}

static const struct installer_discovery_disk *production_preserve_find_disk(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (report == NULL) {
        return NULL;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        if (report->disks[index].present &&
            report->disks[index].id == disk_id) {
            return &report->disks[index];
        }
    }
    return NULL;
}

static const struct installer_discovery_partition *production_preserve_find_esp(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    if (report == NULL) {
        return NULL;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            partition->efi_system) {
            return partition;
        }
    }
    return NULL;
}

/*
 * ITEM-398: which disks' loaders the menu may offer.
 *
 * A preserving install keeps what is on the target disk, so the target's
 * systems are offered.  A whole-disk install erases the target disk, so
 * offering a system that lives there would offer exactly what this run is about
 * to destroy: the target contributes no loader and only the other disks'
 * systems are menu material (owner Case B).  The other disks are still only
 * read, never written.
 */
struct production_preserve_menu_context {
    installer_boot_menu_device_fn device_fn;
    void *device_context;
    uint32_t target_disk_id;
    bool drop_target;
};

static struct block_device *production_preserve_menu_device(
    void *context,
    uint32_t disk_id)
{
    const struct production_preserve_menu_context *menu_context =
        (const struct production_preserve_menu_context *)context;

    if (menu_context == NULL || menu_context->device_fn == NULL) {
        return NULL;
    }
    if (menu_context->drop_target &&
        disk_id == menu_context->target_disk_id) {
        return NULL;
    }
    return menu_context->device_fn(menu_context->device_context, disk_id);
}

static enum production_installer_status production_preserve_map_menu_status(
    enum installer_boot_menu_status status)
{
    switch (status) {
    case INSTALLER_BOOT_MENU_OK:
        return PRODUCTION_INSTALLER_OK;
    case INSTALLER_BOOT_MENU_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case INSTALLER_BOOT_MENU_BAD_VERSION:
    case INSTALLER_BOOT_MENU_BAD_STATE:
    case INSTALLER_BOOT_MENU_NO_EXISTING_SYSTEM:
        return PRODUCTION_INSTALLER_BAD_STATE;
    case INSTALLER_BOOT_MENU_INVALID_ARGUMENT:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    case INSTALLER_BOOT_MENU_CAPACITY:
        return PRODUCTION_INSTALLER_CAPACITY;
    case INSTALLER_BOOT_MENU_UNSUPPORTED_FIRMWARE:
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    case INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH:
        /*
         * A system was found on the disk whose loader this install cannot
         * identify, so the menu could not keep it selectable: the install
         * refuses instead of writing a menu that would lose it.
         */
        return PRODUCTION_INSTALLER_MENU_UNAVAILABLE;
    default:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
}

enum production_installer_status production_installer_preserve_esp(
    const struct production_installer *installer,
    const struct installer_discovery_report *report,
    uint64_t *first_lba,
    uint64_t *length_lba)
{
    const struct installer_discovery_partition *esp;

    if (installer == NULL || report == NULL || first_lba == NULL ||
        length_lba == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->mode != PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!production_preserve_report_valid(report)) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    if (production_preserve_find_disk(report, installer->disk_id) == NULL) {
        return PRODUCTION_INSTALLER_UNSAFE_EVIDENCE;
    }
    esp = production_preserve_find_esp(report, installer->disk_id);
    if (esp == NULL || esp->length_lba == 0U) {
        /*
         * A dual-boot install on a disk with no EFI System Partition has
         * nowhere to put the appliance's loader: that ESP is what makes the
         * install dual boot at all.
         */
        return PRODUCTION_INSTALLER_ESP_UNSUITABLE;
    }
    *first_lba = esp->first_lba;
    *length_lba = esp->length_lba;
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_preserve_reuse(
    const struct production_installer *installer,
    struct installer_gpt_preserve_reuse *reuse)
{
    size_t index;

    if (installer == NULL || reuse == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (!installer->layout_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    memset(reuse, 0, sizeof(*reuse));
    for (index = 0U; index < INSTALLER_LAYOUT_PARTITION_CAPACITY;
         ++index) {
        const struct installer_layout_contract_role *role =
            &installer->layout.roles[index];

        if (!role->present || !role->reused ||
            role->role != INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
            continue;
        }
        reuse->present = true;
        reuse->role = role->role;
        reuse->first_lba = role->first_lba;
        reuse->length_lba = role->length_lba;
        break;
    }
    return PRODUCTION_INSTALLER_OK;
}

enum production_installer_status production_installer_plan_preserve(
    struct production_installer *installer,
    const struct installer_discovery_report *report,
    installer_boot_menu_device_fn device_fn,
    void *device_context,
    const struct installer_boot_menu_entry_source *appliance,
    const struct installer_boot_menu_entry_source *recovery,
    struct installer_boot_menu *menu,
    struct installer_boot_menu_inventory *inventory)
{
    enum installer_boot_menu_status menu_status;
    enum production_installer_status status;
    const struct installer_discovery_partition *esp;
    struct production_preserve_menu_context menu_context;

    if (installer == NULL || report == NULL || menu == NULL ||
        inventory == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    /*
     * ITEM-398: the menu is not a preserve-mode feature.  A whole-disk install
     * whose target disk is entirely ours still has to offer the system that
     * lives on *another* disk (owner Case B), so the menu is planned in both
     * modes; only the preservation summary differs.
     */
    if ((installer->mode != PRODUCTION_INSTALLER_MODE_DUAL_BOOT &&
         installer->mode != PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK) ||
        installer->stage != PRODUCTION_INSTALLER_STAGE_PLAN_BUILT ||
        !installer->layout_ready || installer->preserve_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!production_preserve_report_valid(report)) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    if (production_preserve_find_disk(report, installer->disk_id) == NULL) {
        return PRODUCTION_INSTALLER_UNSAFE_EVIDENCE;
    }
    esp = production_preserve_find_esp(report, installer->disk_id);
    if (esp == NULL) {
        /*
         * A dual-boot install on a disk with no EFI System Partition has
         * nowhere to put the appliance's loader: that ESP is what makes the
         * install dual boot at all.
         *
         * A whole-disk install owns the target disk and its own plan creates
         * the ESP, so there is nothing to reuse and nothing on the target to
         * offer.  What the menu can still offer is a system that lives on
         * another disk (owner Case B: Debian on the machine's other disk,
         * chainloaded through its GPT partition GUID), so this is not an error
         * there - the target disk simply contributes no loader, and the
         * summary below reports the ESP the plan declares.
         */
        if (installer->mode == PRODUCTION_INSTALLER_MODE_DUAL_BOOT) {
            return PRODUCTION_INSTALLER_ESP_UNSUITABLE;
        }
    }
    /*
     * Read-only inventory of every discovered disk's ESP, so a system on another
     * disk (the acceptance host's Debian disk, the owner's Fedora disk) can be
     * offered in the appliance's menu without that disk ever being written.
     */
    memset(&menu_context, 0, sizeof(menu_context));
    menu_context.device_fn = device_fn;
    menu_context.device_context = device_context;
    menu_context.target_disk_id = installer->disk_id;
    menu_context.drop_target =
        installer->mode == PRODUCTION_INSTALLER_MODE_CLEAN_WHOLE_DISK;
    menu_status = installer_boot_menu_scan(
        report, installer->disk_id, production_preserve_menu_device,
        &menu_context, inventory);
    if (menu_status != INSTALLER_BOOT_MENU_OK) {
        return production_preserve_map_menu_status(menu_status);
    }
    memset(menu, 0, sizeof(*menu));
    menu_status = installer_boot_menu_build(
        report, inventory, appliance, recovery, menu);
    if (menu_status != INSTALLER_BOOT_MENU_OK) {
        memset(menu, 0, sizeof(*menu));
        return production_preserve_map_menu_status(menu_status);
    }
    memset(&installer->preserve, 0, sizeof(installer->preserve));
    installer->preserve.version = PRODUCTION_INSTALLER_PRESERVE_VERSION;
    installer->preserve.preserved_count =
        installer->layout.existing_partition_count;
    /*
     * ITEM-501: a release run keeps every entry but one - the partition the
     * operator gave up is removed, so it is not counted as preserved.
     */
    if (installer->release_partition &&
        installer->preserve.preserved_count != 0U) {
        installer->preserve.preserved_count -= 1U;
    }
    /*
     * The reused ESP is one of the four contracted roles but it is not added:
     * the plan carries three requests, and the count the preview publishes is
     * the number of entries this install creates.
     */
    for (size_t role_index = 0U;
         role_index < INSTALLER_LAYOUT_PARTITION_CAPACITY;
         ++role_index) {
        const struct installer_layout_contract_role *role =
            &installer->layout.roles[role_index];

        if (role->present && !role->reused) {
            ++installer->preserve.added_count;
        }
    }
    installer->preserve.loader_count = menu->loader_count;
    installer->preserve.menu_entry_count = menu->entry_count;
    installer->preserve.menu_missing_count = menu->missing_count;
    installer->preserve.unreadable_disk_count =
        menu->unreadable_disk_count;
    installer->preserve.esp_reused = installer->layout.esp_reused;
    if (esp != NULL) {
        installer->preserve.esp_first_lba = esp->first_lba;
        installer->preserve.esp_length_lba = esp->length_lba;
    } else {
        /*
         * A whole-disk target has no ESP in the report: the one the plan
         * declares is the volume this install writes its loader to, and the
         * preview quotes that.
         */
        for (size_t role_index = 0U;
             role_index < INSTALLER_LAYOUT_PARTITION_CAPACITY;
             ++role_index) {
            const struct installer_layout_contract_role *role =
                &installer->layout.roles[role_index];

            if (role->present &&
                role->role == INSTALLER_LAYOUT_ROLE_EFI_SYSTEM) {
                installer->preserve.esp_first_lba = role->first_lba;
                installer->preserve.esp_length_lba = role->length_lba;
                break;
            }
        }
    }
    installer->preserve.free_first_lba = installer->layout.free_first_lba;
    installer->preserve.free_length_lba = installer->layout.free_length_lba;
    installer->preserve.existing_first_lba =
        installer->layout.existing_first_lba;
    installer->preserve.existing_length_lba =
        installer->layout.existing_length_lba;
    installer->preserve_ready = true;
    status = PRODUCTION_INSTALLER_OK;
    return status;
}

enum production_installer_status
production_installer_publish_preserve_record(
    struct production_installer *installer,
    const struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table,
    const struct installer_boot_menu *menu,
    const struct installer_boot_menu_inventory *inventory)
{
    char table_record[INSTALLER_GPT_PRESERVE_RECORD_LENGTH];
    enum installer_gpt_preserve_status preserve_status;
    char menu_record[INSTALLER_BOOT_MENU_RECORD_LENGTH];
    size_t table_length = 0U;
    size_t menu_length = 0U;
    uint64_t serial_flags;

    if (installer == NULL || plan == NULL || table == NULL ||
        menu == NULL || inventory == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (!installer->preserve_ready ||
        (installer->stage !=
             PRODUCTION_INSTALLER_STAGE_PREVIEW_PUBLISHED &&
         installer->stage != PRODUCTION_INSTALLER_STAGE_CONFIRMED)) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (menu->version != INSTALLER_BOOT_MENU_VERSION ||
        menu->text[0] == '\0') {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    /*
     * The publication is the plan the writer will execute, so it is checked
     * against the table that is on the disk first - read-only, and the same
     * check the write repeats.
     */
    preserve_status = installer_gpt_preserve_check(
        table, plan, reuse);
    if (preserve_status != INSTALLER_GPT_PRESERVE_OK) {
        return production_preserve_map_table_status(preserve_status);
    }
    if (installer_gpt_preserve_render_record(
            table, plan, menu->entry_count, table_record,
            sizeof(table_record), &table_length) !=
            INSTALLER_GPT_PRESERVE_OK ||
        table_length == 0U ||
        installer_boot_menu_render_record(
            menu, inventory, menu_record, sizeof(menu_record),
            &menu_length) != INSTALLER_BOOT_MENU_OK ||
        menu_length == 0U) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    /*
     * One serial sequence, so an interrupt-context trace cannot split the
     * record the acceptance run quotes.
     */
    serial_flags = serial_sequence_begin();
    serial_write(table_record);
    serial_write(menu_record);
    serial_write("[INSTALLER-BOOT-MENU] config-begin\n");
    serial_write(menu->text);
    serial_write("[INSTALLER-BOOT-MENU] config-end\n");
    serial_sequence_end(serial_flags);
    /*
     * ITEM-405: the preservation and menu records used to be painted on the
     * local console after the serial sequence.  ITEM-478 sends both to the
     * durable journal: the serial stream keeps every byte it had, the loader
     * configuration that follows is not scrolled across the screen, and
     * `BOOTLOG` shows what is preserved and what the menu offers on demand.
     */
    console_publish_journal(table_record, table_length);
    console_publish_journal(menu_record, menu_length);
    return PRODUCTION_INSTALLER_OK;
}

static enum production_installer_status
production_preserve_map_table_status(
    enum installer_gpt_preserve_status status)
{
    switch (status) {
    case INSTALLER_GPT_PRESERVE_OK:
        return PRODUCTION_INSTALLER_OK;
    case INSTALLER_GPT_PRESERVE_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case INSTALLER_GPT_PRESERVE_BAD_VERSION:
        return PRODUCTION_INSTALLER_BAD_VERSION;
    case INSTALLER_GPT_PRESERVE_BAD_STATE:
    case INSTALLER_GPT_PRESERVE_ALREADY_EXECUTED:
    case INSTALLER_GPT_PRESERVE_NOT_CONFIRMED:
        return PRODUCTION_INSTALLER_BAD_STATE;
    case INSTALLER_GPT_PRESERVE_INVALID_ARGUMENT:
    case INSTALLER_GPT_PRESERVE_INVALID_BLOCK_DEVICE:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    case INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE:
        return PRODUCTION_INSTALLER_TABLE_UNREADABLE;
    case INSTALLER_GPT_PRESERVE_CORRUPT_TABLE:
    case INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE:
        return PRODUCTION_INSTALLER_TABLE_CORRUPT;
    case INSTALLER_GPT_PRESERVE_NO_FREE_SPACE:
        return PRODUCTION_INSTALLER_NO_FREE_SPACE;
    case INSTALLER_GPT_PRESERVE_EXISTING_SYSTEM_OVERLAP:
        return PRODUCTION_INSTALLER_EXISTING_SYSTEM_CONFLICT;
    case INSTALLER_GPT_PRESERVE_OUT_OF_RANGE:
    case INSTALLER_GPT_PRESERVE_CAPACITY:
        return PRODUCTION_INSTALLER_CAPACITY;
    case INSTALLER_GPT_PRESERVE_IO_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    case INSTALLER_GPT_PRESERVE_VERIFICATION_FAILED:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    default:
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
}

enum production_installer_status
production_installer_execute_layout_preserving(
    struct production_installer *installer,
    struct block_device *device,
    struct installer_layout_plan *plan,
    const struct installer_gpt_preserve_reuse *reuse,
    struct installer_gpt_preserve_table *table)
{
    enum installer_gpt_preserve_status preserve_status;

    if (installer == NULL || device == NULL || plan == NULL ||
        table == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    if (installer->stage != PRODUCTION_INSTALLER_STAGE_CONFIRMED ||
        !installer->confirmed || !installer->preserve_ready) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!installer->media_verified) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    if (!installer->hardware_supported) {
        return PRODUCTION_INSTALLER_UNSUPPORTED_HARDWARE;
    }
    /*
     * The same plan contract the destructive path enforces: the plan belongs to
     * this controller, this disk and this identity, and it is confirmed.
     */
    if (plan->version != INSTALLER_LAYOUT_VERSION ||
        !plan->confirmed || plan->executed ||
        plan->destructive ||
        plan->disk_id != installer->disk_id ||
        strcmp(plan->identity, installer->identity) != 0 ||
        block_device_validate(device) != BLOCK_DEVICE_OK ||
        device->block_size != plan->block_size ||
        device->block_count != plan->block_count) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    installer->stage = PRODUCTION_INSTALLER_STAGE_EXECUTING;
    preserve_status = installer_gpt_preserve_write(
        device, plan, reuse, table);
    if (preserve_status != INSTALLER_GPT_PRESERVE_OK) {
        installer->stage = PRODUCTION_INSTALLER_STAGE_FAILED;
        return production_preserve_map_table_status(preserve_status);
    }
    return PRODUCTION_INSTALLER_OK;
}
