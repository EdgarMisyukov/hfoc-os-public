#include "installer_boot_menu.h"

#include "framebuffer.h"
#include "installer_gpt_preserve.h"

/*
 * The menu scan reads a foreign ESP and nothing else: it never reads or writes
 * firmware variables, so the environment it binds the volume through carries
 * two callbacks that say so instead of pretending to be a firmware variable
 * store.  `efi_fat32_esp_bind()` only needs the environment's block device.
 */
static enum efi_boot_variable_io_status installer_boot_menu_variable_read(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)capacity;
    if (length != NULL) {
        *length = 0U;
    }
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status installer_boot_menu_variable_write(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_ACCESS_DENIED;
}

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define INSTALLER_BOOT_MENU_VENDOR_CAPACITY 12U

const char *installer_boot_menu_scan_status_string(
    enum installer_boot_menu_scan scan);
static void installer_boot_menu_append(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text);
static void installer_boot_menu_copy(
    char *destination,
    size_t capacity,
    const char *source);

/*
 * ITEM-396, owner refinement: the inventory of every discovered disk's ESP.
 * The scan is read-only from end to end - a GPT probe for the partition GUID and
 * a FAT32 loader listing - so reading the Debian disk's ESP to chainload it can
 * never write it.
 */
static const struct installer_discovery_partition *
installer_boot_menu_find_esp(
    const struct installer_discovery_report *report,
    uint32_t disk_id)
{
    size_t index;

    for (index = 0U; index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        const struct installer_discovery_partition *partition =
            &report->partitions[index];

        if (partition->present && partition->disk_id == disk_id &&
            partition->efi_system && partition->length_lba != 0U) {
            return partition;
        }
    }
    return NULL;
}

static enum installer_boot_menu_scan installer_boot_menu_scan_esp(
    struct block_device *device,
    bool is_target,
    uint64_t first_lba,
    uint64_t length_lba,
    struct installer_boot_menu_disk *disk)
{
    struct efi_boot_environment environment;
    struct efi_fat32_esp esp;
    enum installer_gpt_preserve_status guid_status;
    enum efi_fat32_esp_status fat_status;
    size_t loader_count = 0U;

    if (device == NULL) {
        return INSTALLER_BOOT_MENU_SCAN_NO_DEVICE;
    }
    if (is_target) {
        /*
         * The configuration file is written to this ESP, so Limine's `boot()`
         * names it without needing a GUID.
         */
        installer_boot_menu_copy(
            disk->volume, sizeof(disk->volume), "boot()");
        disk->esp_guid[0U] = 0U;
    } else {
        static const char prefix[] = "guid(";
        static const char suffix[] = ")";
        char guid_text[INSTALLER_GPT_PRESERVE_GUID_TEXT_LENGTH];
        size_t length = 0U;

        memset(disk->esp_guid, 0, sizeof(disk->esp_guid));
        guid_status = installer_gpt_preserve_esp_guid(
            device, first_lba, length_lba, disk->esp_guid);
        if (guid_status != INSTALLER_GPT_PRESERVE_OK) {
            if (guid_status == INSTALLER_GPT_PRESERVE_UNREADABLE_TABLE) {
                return INSTALLER_BOOT_MENU_SCAN_UNREADABLE;
            }
            if (guid_status == INSTALLER_GPT_PRESERVE_UNSUPPORTED_TABLE) {
                return INSTALLER_BOOT_MENU_SCAN_UNSUPPORTED;
            }
            return INSTALLER_BOOT_MENU_SCAN_CORRUPT;
        }
        if (installer_gpt_preserve_format_guid(
                disk->esp_guid, guid_text, sizeof(guid_text)) !=
            INSTALLER_GPT_PRESERVE_OK) {
            return INSTALLER_BOOT_MENU_SCAN_CORRUPT;
        }
        disk->volume[0U] = '\0';
        installer_boot_menu_append(
            disk->volume, sizeof(disk->volume), &length, prefix);
        installer_boot_menu_append(
            disk->volume, sizeof(disk->volume), &length, guid_text);
        installer_boot_menu_append(
            disk->volume, sizeof(disk->volume), &length, suffix);
    }
    memset(&environment, 0, sizeof(environment));
    if (efi_boot_environment_init(
            &environment, EFI_BOOT_FIRMWARE_UEFI, device, NULL,
            installer_boot_menu_variable_read,
            installer_boot_menu_variable_write) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return INSTALLER_BOOT_MENU_SCAN_NO_DEVICE;
    }
    memset(&esp, 0, sizeof(esp));
    fat_status = efi_fat32_esp_bind(
        &environment, first_lba, length_lba, &esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return INSTALLER_BOOT_MENU_SCAN_UNREADABLE;
    }
    fat_status = efi_fat32_esp_list_loaders(
        &esp, disk->loaders, EFI_FAT32_ESP_LOADER_CAPACITY,
        &loader_count);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return INSTALLER_BOOT_MENU_SCAN_CORRUPT;
    }
    disk->loader_count = (uint32_t)loader_count;
    if (loader_count == 0U) {
        return INSTALLER_BOOT_MENU_SCAN_NO_LOADER;
    }
    return INSTALLER_BOOT_MENU_SCAN_OK;
}

enum installer_boot_menu_status installer_boot_menu_scan(
    const struct installer_discovery_report *report,
    uint32_t target_disk_id,
    installer_boot_menu_device_fn device_fn,
    void *context,
    struct installer_boot_menu_inventory *inventory)
{
    size_t index;

    if (report == NULL || inventory == NULL) {
        return INSTALLER_BOOT_MENU_NULL;
    }
    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION) {
        return INSTALLER_BOOT_MENU_BAD_VERSION;
    }
    if (!report->read_only || !report->scan_complete ||
        report->manual_selection_required) {
        return INSTALLER_BOOT_MENU_BAD_STATE;
    }
    if (target_disk_id == 0U ||
        report->disk_count > INSTALLER_BOOT_MENU_DISK_CAPACITY) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    memset(inventory, 0, sizeof(*inventory));
    inventory->version = INSTALLER_BOOT_MENU_VERSION;
    inventory->target_disk_id = target_disk_id;
    for (index = 0U; index < INSTALLER_DISCOVERY_DISK_CAPACITY;
         ++index) {
        const struct installer_discovery_disk *disk =
            &report->disks[index];
        const struct installer_discovery_partition *esp;
        struct installer_boot_menu_disk *record;

        if (!disk->present || disk->id == 0U) {
            continue;
        }
        record = &inventory->disks[inventory->disk_count++];
        record->disk_id = disk->id;
        /*
         * ITEM-517: a legacy-BIOS boot cannot chainload an EFI application,
         * so no ESP is scanned at all - no disk carries a volume this
         * firmware could serve a chainload entry from.  The target disk's
         * volume is still `boot()`: it names the FAT boot volume the BIOS
         * layout creates and the generated configuration is written to.  Every
         * disk is recorded NO_ESP, which is what the menu comments and the
         * record publish; the scan reaches no device in this mode.
         */
        if (report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
            if (disk->id == target_disk_id) {
                installer_boot_menu_copy(
                    record->volume, sizeof(record->volume), "boot()");
            }
            record->scan = INSTALLER_BOOT_MENU_SCAN_NO_ESP;
            continue;
        }
        esp = installer_boot_menu_find_esp(report, disk->id);
        if (esp == NULL && disk->id == target_disk_id) {
            /*
             * A whole-disk install creates its ESP: the configuration will live
             * on it, so Limine's `boot()` names it even though the report has
             * no partition yet.  There is nothing to scan on it either.
             */
            installer_boot_menu_copy(
                record->volume, sizeof(record->volume), "boot()");
        }
        if (esp == NULL) {
            /*
             * A disk with no EFI System Partition has nothing to offer, and a
             * target disk without one is refused much earlier by the layout
             * contract.
             */
            record->scan = INSTALLER_BOOT_MENU_SCAN_NO_ESP;
            continue;
        }
        record->esp_present = true;
        record->esp_first_lba = esp->first_lba;
        record->esp_length_lba = esp->length_lba;
        record->scan = installer_boot_menu_scan_esp(
            device_fn != NULL ? device_fn(context, disk->id) : NULL,
            disk->id == target_disk_id, esp->first_lba,
            esp->length_lba, record);
    }
    return INSTALLER_BOOT_MENU_OK;
}

struct installer_boot_menu_vendor {
    const char *name;
    const char *label;
    uint32_t priority;
};

/*
 * The vendor directories a UEFI system installs its loader into.  The order is
 * the order of preference when more than one loader belongs to the same system:
 * a shim and its GRUB are one system, and the shim is what the firmware boots.
 */
static const struct installer_boot_menu_vendor
installer_boot_menu_vendors[] = {
    { "DEBIAN", "Debian GNU/Linux", 1U },
    { "UBUNTU", "Ubuntu", 2U },
    { "FEDORA", "Fedora", 3U },
    { "REDHAT", "Red Hat Enterprise Linux", 4U },
    { "SUSE", "SUSE Linux", 5U },
    { "CENTOS", "CentOS", 6U },
    { "ARCH", "Arch Linux", 7U },
    { "GRUB", "Linux (GRUB)", 8U },
    { "MICROSOFT", "Windows Boot Manager", 9U },
    { "WINDOWS", "Windows Boot Manager", 10U },
    { "BOOT", "Other", 11U },
};

#define INSTALLER_BOOT_MENU_VENDOR_COUNT \
    (sizeof(installer_boot_menu_vendors) / \
     sizeof(installer_boot_menu_vendors[0U]))

static void installer_boot_menu_append(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    if (buffer == NULL || text == NULL || length == NULL) {
        return;
    }
    for (index = 0U; text[index] != '\0'; ++index) {
        if (*length + 1U >= capacity) {
            return;
        }
        buffer[*length] = text[index];
        ++*length;
        buffer[*length] = '\0';
    }
}

/*
 * ITEM-409: the same append, with the answer the configuration renderer needs.
 *
 * `installer_boot_menu_append()` stops at the buffer bound and says nothing, so
 * a configuration one byte too long would be *written truncated* - a Limine
 * configuration whose last entry is missing its `path:` line does not boot, and
 * the menu is the installed configuration for every install now.  This variant
 * reports whether the whole text fit, and the renderer refuses the menu
 * (`INSTALLER_BOOT_MENU_CAPACITY`) rather than emit a short one.  The record
 * builder keeps the plain variant: it has its own reserved-line bound and a
 * record that ends early is still a record.
 */
static bool installer_boot_menu_append_checked(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t text_length = 0U;
    size_t before;

    if (buffer == NULL || text == NULL || length == NULL) {
        return false;
    }
    while (text[text_length] != '\0') {
        ++text_length;
    }
    before = *length;
    installer_boot_menu_append(buffer, capacity, length, text);
    return *length == before + text_length;
}

static void installer_boot_menu_append_decimal(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint64_t value)
{
    char digits[24U];
    size_t count = 0U;

    do {
        digits[count] = (char)('0' + (value % 10U));
        ++count;
        value /= 10U;
    } while (value != 0U && count < sizeof(digits));
    while (count > 0U) {
        char single[2U];

        single[0U] = digits[count - 1U];
        single[1U] = '\0';
        installer_boot_menu_append(buffer, capacity, length, single);
        --count;
    }
}

static bool installer_boot_menu_printable(
    const char *text,
    size_t capacity)
{
    size_t index;

    if (text == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        const unsigned char value = (unsigned char)text[index];

        if (value == 0U) {
            return index > 0U;
        }
        if (value < 0x20U || value > 0x7EU) {
            return false;
        }
    }
    return false;
}

static void installer_boot_menu_copy(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index;

    for (index = 0U; index < capacity; ++index) {
        destination[index] = '\0';
    }
    if (source == NULL) {
        return;
    }
    for (index = 0U; index + 1U < capacity && source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

/*
 * Turn a loader path found on a disk's ESP into a Limine path: Limine writes FAT
 * paths with forward slashes, and the resource names the volume the loader is
 * on - `boot()` for the ESP that carries the configuration file, `guid(<GPT
 * partition GUID>)` for any other disk, so the entry does not depend on a
 * firmware drive number (Limine CONFIG.md, "Paths").
 */
static bool installer_boot_menu_volume_path(
    const char *volume,
    const char *loader_path,
    char *buffer,
    size_t capacity)
{
    size_t length = 0U;
    size_t index;

    if (volume == NULL || volume[0] == '\0' || loader_path == NULL ||
        buffer == NULL || capacity == 0U) {
        return false;
    }
    buffer[0] = '\0';
    installer_boot_menu_append(buffer, capacity, &length, volume);
    installer_boot_menu_append(buffer, capacity, &length, ":");
    if (loader_path[0] != '/' && loader_path[0] != '\\') {
        installer_boot_menu_append(buffer, capacity, &length, "/");
    }
    for (index = 0U; loader_path[index] != '\0'; ++index) {
        char single[2U];

        single[0U] = loader_path[index] == '\\' ? '/'
                                                : loader_path[index];
        single[1U] = '\0';
        installer_boot_menu_append(buffer, capacity, &length, single);
    }
    return length > 0U;
}

static bool installer_boot_menu_name_equals(
    const char *left,
    const char *right)
{
    size_t index;

    if (left == NULL || right == NULL) {
        return false;
    }
    for (index = 0U; index < INSTALLER_BOOT_MENU_VENDOR_CAPACITY;
         ++index) {
        const unsigned char left_value = (unsigned char)left[index];
        const unsigned char right_value = (unsigned char)right[index];

        if (left_value == 0U || right_value == 0U) {
            return left_value == right_value;
        }
        if (left_value != right_value) {
            return false;
        }
    }
    return true;
}

static const struct installer_boot_menu_vendor *
installer_boot_menu_find_vendor(const char *vendor)
{
    size_t index;

    for (index = 0U; index < INSTALLER_BOOT_MENU_VENDOR_COUNT;
         ++index) {
        if (installer_boot_menu_name_equals(
                vendor, installer_boot_menu_vendors[index].name)) {
            return &installer_boot_menu_vendors[index];
        }
    }
    return NULL;
}

static bool installer_boot_menu_kind_matches_vendor(
    enum installer_os_kind kind,
    const struct installer_boot_menu_vendor *vendor)
{
    const bool microsoft =
        vendor != NULL &&
        (installer_boot_menu_name_equals(vendor->name, "MICROSOFT") ||
         installer_boot_menu_name_equals(vendor->name, "WINDOWS"));

    if (kind == INSTALLER_OS_WINDOWS) {
        return microsoft;
    }
    if (kind == INSTALLER_OS_LINUX) {
        return !microsoft;
    }
    /* An "other" system takes whatever loader is left over. */
    return true;
}

/*
 * ITEM-398: the path the install stages its own loader to on the target disk
 * (`PRODUCTION_INSTALLER_BOOT_LOADER_PATH`).  A shared ESP very often carries
 * the previous system's removable-media fallback loader at exactly that path.
 * The install keeps its bytes - renamed to the documented `.PRE` name, with the
 * size published in the boot-entry record - but it is not a system the menu can
 * offer any more: after the install the path holds this appliance's own loader,
 * so an entry for it would chainload the menu into itself.  The exclusion is
 * scoped to the target disk, because the same path on another disk is left
 * alone and stays selectable.
 */
#define INSTALLER_BOOT_MENU_OWN_LOADER_PATH "\\EFI\\BOOT\\BOOTX64.EFI"

static bool installer_boot_menu_path_equal(
    const char *left,
    const char *right)
{
    size_t index = 0U;

    if (left == NULL || right == NULL) {
        return false;
    }
    while (left[index] != '\0' && right[index] != '\0') {
        if (left[index] != right[index]) {
            return false;
        }
        ++index;
    }
    return left[index] == right[index];
}

static bool installer_boot_menu_loader_is_own_path(
    const struct installer_boot_menu_inventory *inventory,
    uint32_t disk_index,
    uint32_t loader_index)
{
    const struct installer_boot_menu_disk *disk =
        &inventory->disks[disk_index];

    if (disk->disk_id != inventory->target_disk_id) {
        return false;
    }
    return installer_boot_menu_path_equal(
        disk->loaders[loader_index].path,
        INSTALLER_BOOT_MENU_OWN_LOADER_PATH);
}

/*
 * Pick the loader that belongs to one discovered system: the loader whose vendor
 * directory is not already claimed, whose kind matches, and whose priority is
 * highest - across every disk's ESP, because the system that has to stay
 * selectable may live on a different disk from the appliance.
 */
static bool installer_boot_menu_take_loader(
    const struct installer_boot_menu_inventory *inventory,
    enum installer_os_kind kind,
    bool claimed[INSTALLER_BOOT_MENU_DISK_CAPACITY]
                [EFI_FAT32_ESP_LOADER_CAPACITY],
    uint32_t *disk_index,
    uint32_t *loader_index)
{
    uint32_t best_priority = 0U;
    bool found = false;
    uint32_t disk;

    for (disk = 0U; disk < inventory->disk_count; ++disk) {
        uint32_t loader;

        for (loader = 0U;
             loader < inventory->disks[disk].loader_count; ++loader) {
            const struct efi_fat32_esp_loader *candidate =
                &inventory->disks[disk].loaders[loader];
            const struct installer_boot_menu_vendor *vendor =
                installer_boot_menu_find_vendor(candidate->vendor);
            uint32_t priority;

            if (claimed[disk][loader]) {
                continue;
            }
            if (installer_boot_menu_loader_is_own_path(
                    inventory, disk, loader)) {
                continue;
            }
            if (!installer_boot_menu_kind_matches_vendor(kind, vendor)) {
                continue;
            }
            priority = vendor != NULL ? vendor->priority : 100U;
            if (found && priority >= best_priority) {
                continue;
            }
            found = true;
            best_priority = priority;
            *disk_index = disk;
            *loader_index = loader;
        }
    }
    return found;
}

static void installer_boot_menu_label_for(
    enum installer_os_kind kind,
    const struct efi_fat32_esp_loader *loader,
    char *label,
    size_t capacity)
{
    const struct installer_boot_menu_vendor *vendor =
        loader != NULL
            ? installer_boot_menu_find_vendor(loader->vendor)
            : NULL;

    if (vendor != NULL) {
        installer_boot_menu_copy(label, capacity, vendor->label);
        return;
    }
    if (kind == INSTALLER_OS_WINDOWS) {
        installer_boot_menu_copy(label, capacity,
                                 "Windows Boot Manager");
        return;
    }
    if (kind == INSTALLER_OS_LINUX) {
        installer_boot_menu_copy(label, capacity, "Linux");
        return;
    }
    installer_boot_menu_copy(label, capacity, "Other");
}

/*
 * ITEM-409: the configured timeout as text, rendered once from the same
 * constant the record publishes, so the configuration an operator reads and the
 * record the acceptance run quotes cannot disagree.
 */
static void installer_boot_menu_timeout_text(char *buffer, size_t capacity)
{
    size_t length = 0U;

    if (buffer == NULL || capacity == 0U) {
        return;
    }
    buffer[0] = '\0';
    installer_boot_menu_append_decimal(
        buffer, capacity, &length,
        INSTALLER_BOOT_MENU_TIMEOUT_SECONDS);
}

static enum installer_boot_menu_status installer_boot_menu_render_text(
    struct installer_boot_menu *menu)
{
    size_t length = 0U;
    uint32_t index;
    uint32_t module;

    if (menu == NULL) {
        return INSTALLER_BOOT_MENU_NULL;
    }
    if (menu->version != INSTALLER_BOOT_MENU_VERSION ||
        menu->entry_count == 0U ||
        menu->entry_count > INSTALLER_BOOT_MENU_CAPACITY) {
        return INSTALLER_BOOT_MENU_BAD_STATE;
    }
    menu->text[0] = '\0';
    /*
     * ITEM-409: every append below is checked.  The configuration this renderer
     * produces is the installed boot configuration for every install, so a text
     * that does not fit the bound is a refusal, never a configuration with the
     * last entry cut in half.
     */
#define INSTALLER_BOOT_MENU_TEXT_APPEND(fragment) \
    do { \
        if (!installer_boot_menu_append_checked( \
                menu->text, sizeof(menu->text), &length, (fragment))) { \
            menu->text[0] = '\0'; \
            return INSTALLER_BOOT_MENU_CAPACITY; \
        } \
    } while (0)

    INSTALLER_BOOT_MENU_TEXT_APPEND("timeout: ");
    {
        char timeout[24U];

        installer_boot_menu_timeout_text(timeout, sizeof(timeout));
        INSTALLER_BOOT_MENU_TEXT_APPEND(timeout);
    }
    INSTALLER_BOOT_MENU_TEXT_APPEND("\n");
    for (index = 0U; index < menu->entry_count; ++index) {
        const struct installer_boot_menu_entry *entry =
            &menu->entries[index];

        if (!entry->present || entry->label[0] == '\0' ||
            entry->path[0] == '\0') {
            menu->text[0] = '\0';
            return INSTALLER_BOOT_MENU_BAD_STATE;
        }
        INSTALLER_BOOT_MENU_TEXT_APPEND("\n/");
        INSTALLER_BOOT_MENU_TEXT_APPEND(entry->label);
        INSTALLER_BOOT_MENU_TEXT_APPEND("\n    protocol: ");
        INSTALLER_BOOT_MENU_TEXT_APPEND(
            entry->protocol == INSTALLER_BOOT_MENU_PROTOCOL_LIMINE
                ? "limine"
                : "efi");
        /*
         * ITEM-466 (round 5): the installed entry carries no fixed mode.  ITEM-464
         * wrote `resolution: 1024x768` here so the installed firmware would come
         * down from 3840x2160, but the owner's rule is "the mode is the monitor's
         * native/preferred one, with a reasonable fallback to the largest
         * available".  The kernel's `[FB]` record names `preferred=`, the
         * `available=` list, the mode the rule would choose if the preference had
         * been refused and the automatic font scale, so the entry only carries
         * the protocol and the path.
         */
        INSTALLER_BOOT_MENU_TEXT_APPEND("\n    path: ");
        INSTALLER_BOOT_MENU_TEXT_APPEND(entry->path);
        INSTALLER_BOOT_MENU_TEXT_APPEND("\n");
        /*
         * ITEM-399: the appliance entry states the management scope the
         * operator declared on the medium.  The kernel reads the scope from
         * the boot entry's command line, so without this line an installed
         * appliance whose configuration is the generated menu would fall back
         * to the deny-by-default scope instead of the declared one.
         */
        if (entry->cmdline[0] != '\0') {
            INSTALLER_BOOT_MENU_TEXT_APPEND("    cmdline: ");
            INSTALLER_BOOT_MENU_TEXT_APPEND(entry->cmdline);
            INSTALLER_BOOT_MENU_TEXT_APPEND("\n");
        }
        for (module = 0U; module < entry->module_count; ++module) {
            if (entry->module_paths[module][0U] == '\0') {
                menu->text[0] = '\0';
                return INSTALLER_BOOT_MENU_BAD_STATE;
            }
            INSTALLER_BOOT_MENU_TEXT_APPEND("    module_path: ");
            INSTALLER_BOOT_MENU_TEXT_APPEND(
                entry->module_paths[module]);
            INSTALLER_BOOT_MENU_TEXT_APPEND("\n");
        }
    }
#undef INSTALLER_BOOT_MENU_TEXT_APPEND
    return INSTALLER_BOOT_MENU_OK;
}

enum installer_boot_menu_status installer_boot_menu_render(
    struct installer_boot_menu *menu)
{
    return installer_boot_menu_render_text(menu);
}

static const char *installer_boot_menu_kind_label(
    enum installer_os_kind kind)
{
    if (kind == INSTALLER_OS_WINDOWS) {
        return "Windows";
    }
    if (kind == INSTALLER_OS_LINUX) {
        return "Linux";
    }
    return "Other";
}

/*
 * ITEM-409: the trailing comments carry a bound of their own.  They document
 * what the menu could not offer, so they are part of the configuration an
 * operator reads - and they are appended to a text that has already been
 * rendered, which is why they report their own overflow instead of silently
 * dropping the last comment.
 */
static bool installer_boot_menu_comment(
    struct installer_boot_menu *menu,
    size_t *length,
    const char *label,
    const char *detail,
    const char *reason)
{
    const char *const parts[] = { "# ", label, detail, " reason=", reason,
                                  "\n" };
    size_t index;

    for (index = 0U; index < sizeof(parts) / sizeof(parts[0U]); ++index) {
        if (!installer_boot_menu_append_checked(
                menu->text, sizeof(menu->text), length, parts[index])) {
            return false;
        }
    }
    return true;
}

/*
 * Copy a caller-owned entry (label, path, module paths) into the menu, with the
 * bounds the renderer and the record then rely on.
 */
static enum installer_boot_menu_status installer_boot_menu_fill_owned_entry(
    struct installer_boot_menu_entry *entry,
    const struct installer_boot_menu_entry_source *source,
    enum installer_boot_menu_protocol protocol)
{
    size_t module;

    if (source == NULL || source->label == NULL ||
        source->path == NULL ||
        source->module_count > INSTALLER_BOOT_MENU_MODULE_CAPACITY) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    if (!installer_boot_menu_printable(
            source->label, INSTALLER_BOOT_MENU_LABEL_LENGTH) ||
        !installer_boot_menu_printable(
            source->path, INSTALLER_BOOT_MENU_PATH_LENGTH)) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    /*
     * ITEM-399: the entry's command line, when the caller declares one.  It is
     * validated the same way the label and the paths are, so a menu that could
     * not be written as a configuration is refused instead of rendered.
     */
    if (source->cmdline != NULL &&
        !installer_boot_menu_printable(
            source->cmdline, INSTALLER_BOOT_MENU_CMDLINE_LENGTH)) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    entry->present = true;
    entry->protocol = protocol;
    entry->os_kind = INSTALLER_OS_UNKNOWN;
    entry->module_count = (uint32_t)source->module_count;
    installer_boot_menu_copy(
        entry->label, sizeof(entry->label), source->label);
    installer_boot_menu_copy(
        entry->path, sizeof(entry->path), source->path);
    if (source->cmdline != NULL) {
        installer_boot_menu_copy(
            entry->cmdline, sizeof(entry->cmdline), source->cmdline);
    }
    for (module = 0U; module < source->module_count; ++module) {
        if (source->module_paths[module] == NULL) {
            return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
        }
        if (!installer_boot_menu_printable(
                source->module_paths[module],
                INSTALLER_BOOT_MENU_PATH_LENGTH)) {
            return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
        }
        installer_boot_menu_copy(
            entry->module_paths[module],
            sizeof(entry->module_paths[module]),
            source->module_paths[module]);
    }
    return INSTALLER_BOOT_MENU_OK;
}

enum installer_boot_menu_status installer_boot_menu_build(
    const struct installer_discovery_report *report,
    const struct installer_boot_menu_inventory *inventory,
    const struct installer_boot_menu_entry_source *appliance,
    const struct installer_boot_menu_entry_source *recovery,
    struct installer_boot_menu *menu)
{
    bool claimed[INSTALLER_BOOT_MENU_DISK_CAPACITY]
                [EFI_FAT32_ESP_LOADER_CAPACITY];
    bool os_offered[INSTALLER_DISCOVERY_OS_CAPACITY];
    uint32_t existing_count = 0U;
    uint32_t slot = 0U;
    size_t index;

    if (report == NULL || inventory == NULL || menu == NULL) {
        return INSTALLER_BOOT_MENU_NULL;
    }
    if (report->version != INSTALLER_HARDWARE_DISCOVERY_VERSION ||
        inventory->version != INSTALLER_BOOT_MENU_VERSION) {
        return INSTALLER_BOOT_MENU_BAD_VERSION;
    }
    if (!report->read_only || !report->scan_complete ||
        report->manual_selection_required) {
        return INSTALLER_BOOT_MENU_BAD_STATE;
    }
    if (inventory->disk_count > INSTALLER_BOOT_MENU_DISK_CAPACITY ||
        inventory->target_disk_id == 0U) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    if (appliance == NULL || recovery == NULL) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    if (appliance->path == NULL) {
        return INSTALLER_BOOT_MENU_INVALID_ARGUMENT;
    }
    /*
     * ITEM-517: a legacy-BIOS boot gets the generated menu too, so a machine
     * without UEFI is not left without a way to choose what to start.  The
     * chainload entries the ESP scan produces are UEFI application paths and
     * are not offered as BIOS boot targets yet; the honest BIOS menu records
     * them as unavailable instead (see the menu renderer).
     */
    if (report->firmware_mode != INSTALLER_FIRMWARE_UEFI &&
        report->firmware_mode != INSTALLER_FIRMWARE_BIOS_LEGACY) {
        return INSTALLER_BOOT_MENU_UNSUPPORTED_FIRMWARE;
    }
    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        if (report->oss[index].present) {
            ++existing_count;
        }
    }
    /*
     * ITEM-517: a legacy-BIOS menu can only carry the entries the caller owns
     * (the appliance and, when staged, recovery): no discovered system can be
     * chainloaded by BIOS Limine, so the systems do not consume entry slots.
     */
    if (report->firmware_mode != INSTALLER_FIRMWARE_BIOS_LEGACY &&
        existing_count + 2U > INSTALLER_BOOT_MENU_ENTRY_CAPACITY) {
        return INSTALLER_BOOT_MENU_CAPACITY;
    }
    memset(menu, 0, sizeof(*menu));
    memset(claimed, 0, sizeof(claimed));
    memset(os_offered, 0, sizeof(os_offered));
    menu->version = INSTALLER_BOOT_MENU_VERSION;
    menu->firmware_mode = report->firmware_mode;
    if (report->firmware_mode != INSTALLER_FIRMWARE_BIOS_LEGACY) {
        for (index = 0U; index < inventory->disk_count; ++index) {
            menu->loader_count += inventory->disks[index].loader_count;
        }
    }

    {
        const enum installer_boot_menu_status source_status =
            installer_boot_menu_fill_owned_entry(
                &menu->entries[slot], appliance,
                INSTALLER_BOOT_MENU_PROTOCOL_LIMINE);

        if (source_status != INSTALLER_BOOT_MENU_OK) {
            return source_status;
        }
    }
    ++slot;

    for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY; ++index) {
        const struct installer_discovery_os *os = &report->oss[index];
        const struct installer_boot_menu_disk *disk;
        struct installer_boot_menu_entry *entry;
        uint32_t disk_index = 0U;
        uint32_t loader_index = 0U;

        if (!os->present) {
            continue;
        }
        /*
         * ITEM-517: on a legacy-BIOS boot every discovered system is recorded
         * as unofferable and no EFI chainload entry is created for it.  The
         * comment loop below names each one with its reason.
         */
        if (report->firmware_mode == INSTALLER_FIRMWARE_BIOS_LEGACY) {
            ++menu->missing_count;
            continue;
        }
        if (slot + 1U >= INSTALLER_BOOT_MENU_ENTRY_CAPACITY) {
            return INSTALLER_BOOT_MENU_CAPACITY;
        }
        if (!installer_boot_menu_take_loader(
                inventory, os->kind, claimed, &disk_index,
                &loader_index)) {
            /*
             * Nothing on any ESP names a loader for this system.  The menu is
             * still written - the appliance must boot - and the system is
             * recorded instead of being dropped in silence.
             */
            ++menu->missing_count;
            continue;
        }
        claimed[disk_index][loader_index] = true;
        os_offered[index] = true;
        disk = &inventory->disks[disk_index];
        entry = &menu->entries[slot];
        entry->present = true;
        entry->protocol = INSTALLER_BOOT_MENU_PROTOCOL_EFI;
        entry->os_kind = os->kind;
        entry->os_id = os->id;
        installer_boot_menu_label_for(
            os->kind, &disk->loaders[loader_index], entry->label,
            sizeof(entry->label));
        if (!installer_boot_menu_volume_path(
                disk->volume, disk->loaders[loader_index].path,
                entry->path, sizeof(entry->path))) {
            return INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH;
        }
        ++slot;
    }

    /*
     * Loaders no discovered system claimed still get an entry, labelled by
     * vendor: two systems of the same kind (two Linux installations, say) are
     * one discovery entry but two loaders, and both must stay selectable.
     *
     * ITEM-517: on a legacy-BIOS boot there is no loader this firmware can
     * boot, so the loop does not run at all - even an inventory that still
     * carries loader records cannot produce an EFI entry.
     */
    for (index = 0U;
         report->firmware_mode != INSTALLER_FIRMWARE_BIOS_LEGACY &&
             index < inventory->disk_count;
         ++index) {
        const struct installer_boot_menu_disk *disk =
            &inventory->disks[index];
        uint32_t loader;

        for (loader = 0U; loader < disk->loader_count; ++loader) {
            struct installer_boot_menu_entry *entry;

            if (claimed[index][loader]) {
                continue;
            }
            if (installer_boot_menu_loader_is_own_path(
                    inventory, index, loader)) {
                /*
                 * The install takes this path over on the target disk, so the
                 * file behind it is not a system any more; its bytes stay
                 * recoverable under the documented `.PRE` name and the
                 * boot-entry record names them.  Claiming it here keeps it out
                 * of the "could not be offered" count, which is for systems the
                 * menu failed to find a loader for.
                 */
                claimed[index][loader] = true;
                continue;
            }
            if (slot + 2U >= INSTALLER_BOOT_MENU_ENTRY_CAPACITY) {
                /* The appliance and the recovery entry come first. */
                ++menu->missing_count;
                continue;
            }
            entry = &menu->entries[slot];
            entry->present = true;
            entry->protocol = INSTALLER_BOOT_MENU_PROTOCOL_EFI;
            entry->os_kind = INSTALLER_OS_OTHER;
            installer_boot_menu_label_for(
                INSTALLER_OS_OTHER, &disk->loaders[loader],
                entry->label, sizeof(entry->label));
            if (!installer_boot_menu_volume_path(
                    disk->volume, disk->loaders[loader].path,
                    entry->path, sizeof(entry->path))) {
                return INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH;
            }
            claimed[index][loader] = true;
            ++slot;
        }
    }

    /*
     * The recovery entry is optional: a flow that has no recovery image staged
     * offers none, and invents no path that would not boot.
     */
    if (recovery->path != NULL) {
        const enum installer_boot_menu_status source_status =
            installer_boot_menu_fill_owned_entry(
                &menu->entries[slot], recovery,
                INSTALLER_BOOT_MENU_PROTOCOL_LIMINE);

        if (source_status != INSTALLER_BOOT_MENU_OK) {
            return source_status;
        }
        ++slot;
    }
    menu->entry_count = slot;
    if (installer_boot_menu_render_text(menu) != INSTALLER_BOOT_MENU_OK) {
        memset(menu->text, 0, sizeof(menu->text));
        return INSTALLER_BOOT_MENU_CAPACITY;
    }
    /*
     * The comments are appended after the entries: they document every system
     * and every disk the menu could not offer, without changing the entries
     * themselves.  A Limine comment is a line of its own starting with '#'.
     *
     * ITEM-409: they are part of the configuration this install writes, so a
     * comment that does not fit refuses the menu rather than leaving a
     * half-written line in the file the loader reads.
     */
    {
        size_t length = strlen(menu->text);

        for (index = 0U; index < INSTALLER_DISCOVERY_OS_CAPACITY;
             ++index) {
            const struct installer_discovery_os *os = &report->oss[index];

            if (!os->present || os_offered[index]) {
                continue;
            }
            /*
             * ITEM-517: a legacy-BIOS boot cannot offer any discovered
             * system, and the honest reason is the missing chainload path,
             * not a missing loader: the system's own ESP may be perfectly
             * readable, this firmware just cannot boot from it.
             */
            if (!installer_boot_menu_comment(
                    menu, &length, "system ",
                    installer_boot_menu_kind_label(os->kind),
                    report->firmware_mode ==
                            INSTALLER_FIRMWARE_BIOS_LEGACY
                        ? "NO_ESP"
                        : "NO_LOADER")) {
                memset(menu->text, 0, sizeof(menu->text));
                return INSTALLER_BOOT_MENU_CAPACITY;
            }
        }
        for (index = 0U; index < inventory->disk_count; ++index) {
            const struct installer_boot_menu_disk *disk =
                &inventory->disks[index];
            char detail[INSTALLER_BOOT_MENU_COMMENT_LENGTH];
            size_t detail_length = 0U;

            if (disk->scan == INSTALLER_BOOT_MENU_SCAN_OK) {
                continue;
            }
            if (disk->esp_present) {
                ++menu->unreadable_disk_count;
            }
            installer_boot_menu_copy(detail, sizeof(detail), "id=");
            detail_length = strlen(detail);
            installer_boot_menu_append_decimal(
                detail, sizeof(detail), &detail_length, disk->disk_id);
            if (!installer_boot_menu_comment(
                    menu, &length, "disk ", detail,
                    installer_boot_menu_scan_status_string(disk->scan))) {
                memset(menu->text, 0, sizeof(menu->text));
                return INSTALLER_BOOT_MENU_CAPACITY;
            }
        }
    }
    return INSTALLER_BOOT_MENU_OK;
}

const char *installer_boot_menu_scan_status_string(
    enum installer_boot_menu_scan scan)
{
    switch (scan) {
    case INSTALLER_BOOT_MENU_SCAN_OK:
        return "OK";
    case INSTALLER_BOOT_MENU_SCAN_NO_DEVICE:
        return "NO_DEVICE";
    case INSTALLER_BOOT_MENU_SCAN_NO_ESP:
        return "NO_ESP";
    case INSTALLER_BOOT_MENU_SCAN_UNREADABLE:
        return "UNREADABLE";
    case INSTALLER_BOOT_MENU_SCAN_CORRUPT:
        return "CORRUPT";
    case INSTALLER_BOOT_MENU_SCAN_UNSUPPORTED:
        return "UNSUPPORTED";
    case INSTALLER_BOOT_MENU_SCAN_NO_LOADER:
        return "NO_LOADER";
    default:
        return "UNKNOWN";
    }
}

enum installer_boot_menu_status installer_boot_menu_render_record(
    const struct installer_boot_menu *menu,
    const struct installer_boot_menu_inventory *inventory,
    char *buffer,
    size_t capacity,
    size_t *length)
{
    size_t used = 0U;
    uint32_t disk_index;
    uint32_t index;
    uint32_t listed = 0U;

    if (menu == NULL || buffer == NULL || length == NULL ||
        capacity == 0U) {
        return INSTALLER_BOOT_MENU_NULL;
    }
    if (menu->version != INSTALLER_BOOT_MENU_VERSION ||
        (inventory != NULL &&
         inventory->version != INSTALLER_BOOT_MENU_VERSION)) {
        return INSTALLER_BOOT_MENU_BAD_VERSION;
    }
    if (menu->entry_count == 0U ||
        menu->entry_count > INSTALLER_BOOT_MENU_ENTRY_CAPACITY) {
        return INSTALLER_BOOT_MENU_BAD_STATE;
    }
    buffer[0] = '\0';
#define INSTALLER_BOOT_MENU_RESERVE() \
    do { \
        if (capacity <= used || \
            INSTALLER_BOOT_MENU_RECORD_LINE_LENGTH >= capacity - used) { \
            return INSTALLER_BOOT_MENU_CAPACITY; \
        } \
    } while (0)

    INSTALLER_BOOT_MENU_RESERVE();
    installer_boot_menu_append(
        buffer, capacity, &used, "[INSTALLER-BOOT-MENU] menu firmware=");
    installer_boot_menu_append(
        buffer, capacity, &used,
        menu->firmware_mode == INSTALLER_FIRMWARE_UEFI ? "uefi"
                                                       : "bios-legacy");
    installer_boot_menu_append(buffer, capacity, &used, " entries=");
    installer_boot_menu_append_decimal(
        buffer, capacity, &used, menu->entry_count);
    installer_boot_menu_append(buffer, capacity, &used, " loaders=");
    installer_boot_menu_append_decimal(
        buffer, capacity, &used, menu->loader_count);
    installer_boot_menu_append(buffer, capacity, &used, " missing=");
    installer_boot_menu_append_decimal(
        buffer, capacity, &used, menu->missing_count);
    installer_boot_menu_append(
        buffer, capacity, &used, " unreadable_disks=");
    installer_boot_menu_append_decimal(
        buffer, capacity, &used, menu->unreadable_disk_count);
    installer_boot_menu_append(buffer, capacity, &used, "\n");
    /*
     * Per disk: what the read-only scan found and what it could not, so the run
     * states per discovered disk which loader it offers and which one it could
     * not read, instead of dropping the existing system in silence.
     */
    for (disk_index = 0U;
         inventory != NULL && disk_index < inventory->disk_count;
         ++disk_index) {
        const struct installer_boot_menu_disk *disk =
            &inventory->disks[disk_index];
        uint32_t loader;

        INSTALLER_BOOT_MENU_RESERVE();
        installer_boot_menu_append(
            buffer, capacity, &used, "[INSTALLER-BOOT-MENU] disk id=");
        installer_boot_menu_append_decimal(
            buffer, capacity, &used, disk->disk_id);
        installer_boot_menu_append(buffer, capacity, &used, " esp=");
        if (disk->esp_present) {
            installer_boot_menu_append_decimal(
                buffer, capacity, &used, disk->esp_first_lba);
            installer_boot_menu_append(buffer, capacity, &used, "+");
            installer_boot_menu_append_decimal(
                buffer, capacity, &used, disk->esp_length_lba);
        } else {
            installer_boot_menu_append(buffer, capacity, &used, "none");
        }
        installer_boot_menu_append(buffer, capacity, &used, " volume=");
        installer_boot_menu_append(
            buffer, capacity, &used,
            disk->volume[0U] != '\0' ? disk->volume : "none");
        installer_boot_menu_append(buffer, capacity, &used, " scan=");
        installer_boot_menu_append(
            buffer, capacity, &used,
            installer_boot_menu_scan_status_string(disk->scan));
        installer_boot_menu_append(buffer, capacity, &used, " loaders=");
        installer_boot_menu_append_decimal(
            buffer, capacity, &used, disk->loader_count);
        installer_boot_menu_append(buffer, capacity, &used, "\n");
        for (loader = 0U; loader < disk->loader_count; ++loader) {
            const struct efi_fat32_esp_loader *record =
                &disk->loaders[loader];

            if (listed >= INSTALLER_BOOT_MENU_RECORD_LOADER_LINES) {
                break;
            }
            INSTALLER_BOOT_MENU_RESERVE();
            installer_boot_menu_append(
                buffer, capacity, &used,
                "[INSTALLER-BOOT-MENU] loader disk=");
            installer_boot_menu_append_decimal(
                buffer, capacity, &used, disk->disk_id);
            installer_boot_menu_append(buffer, capacity, &used, " vendor=");
            installer_boot_menu_append(
                buffer, capacity, &used, record->vendor);
            installer_boot_menu_append(buffer, capacity, &used, " path=");
            installer_boot_menu_append(
                buffer, capacity, &used, record->path);
            installer_boot_menu_append(buffer, capacity, &used, " size=");
            installer_boot_menu_append_decimal(
                buffer, capacity, &used, record->size);
            installer_boot_menu_append(buffer, capacity, &used, "\n");
            ++listed;
        }
    }
    for (index = 0U; index < menu->entry_count; ++index) {
        const struct installer_boot_menu_entry *entry =
            &menu->entries[index];

        if (!entry->present || entry->label[0] == '\0' ||
            entry->path[0] == '\0') {
            return INSTALLER_BOOT_MENU_BAD_STATE;
        }
        INSTALLER_BOOT_MENU_RESERVE();
        installer_boot_menu_append(
            buffer, capacity, &used, "[INSTALLER-BOOT-MENU] entry index=");
        installer_boot_menu_append_decimal(
            buffer, capacity, &used, index);
        installer_boot_menu_append(buffer, capacity, &used, " protocol=");
        installer_boot_menu_append(
            buffer, capacity, &used,
            installer_boot_menu_protocol_string(entry->protocol));
        installer_boot_menu_append(buffer, capacity, &used, " label=");
        installer_boot_menu_append(buffer, capacity, &used, entry->label);
        installer_boot_menu_append(buffer, capacity, &used, " path=");
        installer_boot_menu_append(buffer, capacity, &used, entry->path);
        installer_boot_menu_append(buffer, capacity, &used, " modules=");
        installer_boot_menu_append_decimal(
            buffer, capacity, &used, entry->module_count);
        /*
         * ITEM-399: the declared command line is part of what the menu
         * publishes, so the record states the scope the installed appliance
         * will publish on its own boot.
         */
        if (entry->cmdline[0] != '\0') {
            installer_boot_menu_append(buffer, capacity, &used, " cmdline=");
            installer_boot_menu_append(
                buffer, capacity, &used, entry->cmdline);
        }
        installer_boot_menu_append(buffer, capacity, &used, "\n");
    }
    *length = used;
    return INSTALLER_BOOT_MENU_OK;
#undef INSTALLER_BOOT_MENU_RESERVE
}

const char *installer_boot_menu_protocol_string(
    enum installer_boot_menu_protocol protocol)
{
    switch (protocol) {
    case INSTALLER_BOOT_MENU_PROTOCOL_LIMINE:
        return "limine";
    case INSTALLER_BOOT_MENU_PROTOCOL_EFI:
        return "efi";
    default:
        return "unknown";
    }
}

const char *installer_boot_menu_status_string(
    enum installer_boot_menu_status status)
{
    switch (status) {
    case INSTALLER_BOOT_MENU_OK:
        return "INSTALLER_BOOT_MENU_OK";
    case INSTALLER_BOOT_MENU_NULL:
        return "INSTALLER_BOOT_MENU_NULL";
    case INSTALLER_BOOT_MENU_BAD_STATE:
        return "INSTALLER_BOOT_MENU_BAD_STATE";
    case INSTALLER_BOOT_MENU_BAD_VERSION:
        return "INSTALLER_BOOT_MENU_BAD_VERSION";
    case INSTALLER_BOOT_MENU_INVALID_ARGUMENT:
        return "INSTALLER_BOOT_MENU_INVALID_ARGUMENT";
    case INSTALLER_BOOT_MENU_CAPACITY:
        return "INSTALLER_BOOT_MENU_CAPACITY";
    case INSTALLER_BOOT_MENU_UNSUPPORTED_FIRMWARE:
        return "INSTALLER_BOOT_MENU_UNSUPPORTED_FIRMWARE";
    case INSTALLER_BOOT_MENU_NO_EXISTING_SYSTEM:
        return "INSTALLER_BOOT_MENU_NO_EXISTING_SYSTEM";
    case INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH:
        return "INSTALLER_BOOT_MENU_UNKNOWN_BOOT_PATH";
    default:
        return NULL;
    }
}
