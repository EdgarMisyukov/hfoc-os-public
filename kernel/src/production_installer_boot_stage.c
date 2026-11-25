#include "production_installer_boot_stage.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "console.h"
#include "efi_fat32_esp.h"
#include "production_installer_efi.h"
#include "production_installer_esp.h"
#include "serial.h"

#define PRODUCTION_BOOT_ESP_SECTORS_PER_CLUSTER 1U
#define PRODUCTION_BOOT_HEX_DIGEST 16U

static bool production_boot_media_valid(
    const struct production_installer_boot_media *media)
{
    return media != NULL &&
           media->version ==
               PRODUCTION_INSTALLER_BOOT_STAGE_VERSION &&
           media->payload != NULL && media->payload_size != 0U &&
           media->payload_size <= PRODUCTION_INSTALLER_BOOT_PAYLOAD_MAX &&
           media->config != NULL && media->config_size != 0U &&
           media->config_size <= PRODUCTION_INSTALLER_BOOT_CONFIG_MAX &&
           media->kernel != NULL && media->kernel_size != 0U &&
           media->kernel_size <= PRODUCTION_INSTALLER_BOOT_KERNEL_MAX &&
           media->loader != NULL && media->loader_size != 0U &&
           media->loader_size <= PRODUCTION_INSTALLER_BOOT_LOADER_MAX &&
           media->manifest != NULL && media->manifest_size != 0U &&
           media->manifest_size <=
               PRODUCTION_INSTALLER_BOOT_MANIFEST_MAX &&
           /*
            * ITEM-623: the initramfs is optional for the host fixtures but
            * bounded whenever a caller carries one; a non-empty size with no
            * bytes (or the reverse) is a caller defect, not an absent root.
            */
           (media->rootfs == NULL
                ? media->rootfs_size == 0U
                : media->rootfs_size != 0U &&
                      media->rootfs_size <=
                          PRODUCTION_INSTALLER_BOOT_ROOTFS_MAX) &&
           /*
            * ITEM-409: the menu is the installed configuration whenever the
            * caller offers one, so the offer has to be complete - the bytes, the
            * entry count the record publishes and the timeout the configuration
            * declares.  A menu with no entry is not a menu, and a menu nobody can
            * read is not the owner's rule.  A caller with no menu at all (the
            * sealed fallback) declares none of the three.
            */
           (media->menu == NULL
                ? media->menu_size == 0U && media->menu_entries == 0U
                : media->menu_size != 0U &&
                      media->menu_size <=
                          PRODUCTION_INSTALLER_BOOT_CONFIG_MAX &&
                      media->menu_entries != 0U &&
                      media->menu_timeout_seconds != 0U);
}

/* The generated menu is the configuration the loader reads, when there is one. */
static const unsigned char *production_boot_installed_config(
    const struct production_installer_boot_media *media,
    size_t *length)
{
    if (media->menu != NULL && media->menu_size != 0U) {
        *length = media->menu_size;
        return media->menu;
    }
    *length = media->config_size;
    return media->config;
}

static bool production_boot_has_rootfs(
    const struct production_installer_boot_media *media)
{
    return media->rootfs != NULL && media->rootfs_size != 0U;
}

static size_t production_boot_set_size(
    const struct production_installer_boot_media *media)
{
    size_t count = PRODUCTION_INSTALLER_BOOT_SET_COUNT;

    if (production_boot_has_rootfs(media)) {
        ++count;
    }
    if (media->menu != NULL && media->menu_size != 0U) {
        ++count;
    }
    return count;
}

/*
 * The set, in the sealed ITEM-371 source order: the boot payload, the loader
 * configuration and the kernel are the three components the media manifest
 * binds, so the digest the writer returns over that prefix is the sealed
 * source digest.
 */
static void production_boot_set_build(
    const struct production_installer_boot_media *media,
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U])
{
    size_t index = 0U;

    memset(set, 0, (PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U) *
                       sizeof(set[0U]));
    set[0U].path = PRODUCTION_INSTALLER_BOOT_PAYLOAD_PATH;
    set[0U].data = media->payload;
    set[0U].size = media->payload_size;
    set[1U].path = PRODUCTION_INSTALLER_BOOT_CONFIG_PATH;
    set[1U].data = media->config;
    set[1U].size = media->config_size;
    set[2U].path = PRODUCTION_INSTALLER_BOOT_KERNEL_PATH;
    set[2U].data = media->kernel;
    set[2U].size = media->kernel_size;
    set[3U].path = PRODUCTION_INSTALLER_BOOT_LOADER_PATH;
    set[3U].data = media->loader;
    set[3U].size = media->loader_size;
    set[4U].path = PRODUCTION_INSTALLER_BOOT_MANIFEST_PATH;
    set[4U].data = media->manifest;
    set[4U].size = media->manifest_size;
    index = PRODUCTION_INSTALLER_BOOT_SET_COUNT;
    if (production_boot_has_rootfs(media)) {
        /* Appended after the sealed five, so the sealed prefix order holds. */
        set[index].path = PRODUCTION_INSTALLER_BOOT_ROOTFS_PATH;
        set[index].data = media->rootfs;
        set[index].size = media->rootfs_size;
        ++index;
    }
    if (media->menu != NULL && media->menu_size != 0U) {
        set[index].path = PRODUCTION_INSTALLER_BOOT_MENU_PATH;
        set[index].data = media->menu;
        set[index].size = media->menu_size;
    }
}

/*
 * The staged configuration must locate the kernel it was staged with.  The
 * comparison is a bounded search for the declared reference line, not a parse
 * of the whole file, so a configuration that names a different kernel (or
 * names none) is refused instead of being reported as a bootable system.
 */
static bool production_boot_config_references_kernel(
    const unsigned char *config,
    size_t config_length)
{
    static const char reference[] =
        PRODUCTION_INSTALLER_BOOT_CONFIG_REFERENCE;
    const size_t reference_length = sizeof(reference) - 1U;
    size_t start;

    if (config == NULL || config_length < reference_length) {
        return false;
    }
    for (start = 0U;
         start + reference_length <= config_length; ++start) {
        if (memcmp(config + start, reference,
                   reference_length) == 0) {
            return true;
        }
    }
    return false;
}

static enum production_installer_status production_boot_check_esp(
    const struct production_installer_boot_esp *esp)
{
    if (esp == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    if (!esp->present || esp->length_lba == 0U) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    if (esp->first_lba > UINT64_MAX - esp->length_lba) {
        return PRODUCTION_INSTALLER_INVALID_ARGUMENT;
    }
    return PRODUCTION_INSTALLER_OK;
}

static enum production_installer_status production_boot_map_fat(
    enum efi_fat32_esp_status status)
{
    switch (status) {
    case EFI_FAT32_ESP_OK:
        return PRODUCTION_INSTALLER_OK;
    case EFI_FAT32_ESP_CAPACITY:
        return PRODUCTION_INSTALLER_ESP_TOO_SMALL;
    case EFI_FAT32_ESP_OUT_OF_RANGE:
        return PRODUCTION_INSTALLER_CAPACITY;
    case EFI_FAT32_ESP_IO_ERROR:
    case EFI_FAT32_ESP_FLUSH_ERROR:
        return PRODUCTION_INSTALLER_IO_ERROR;
    case EFI_FAT32_ESP_NULL:
        return PRODUCTION_INSTALLER_NULL;
    case EFI_FAT32_ESP_SET_TOO_LARGE:
        return PRODUCTION_INSTALLER_CAPACITY;
    default:
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
}

enum production_installer_status
production_installer_boot_stage_requirements(
    const struct production_installer *installer,
    const struct production_installer_boot_esp *esp,
    const struct production_installer_boot_media *media,
    struct production_installer_boot_outcome *outcome)
{
    struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U];
    struct efi_fat32_esp_requirement requirement;
    enum efi_fat32_esp_status fat_status;
    enum production_installer_status status;
    size_t installed_config_size = 0U;

    if (installer == NULL || esp == NULL || media == NULL ||
        outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    memset(outcome, 0, sizeof(*outcome));
    outcome->version = PRODUCTION_INSTALLER_BOOT_STAGE_VERSION;
    if (installer->version != PRODUCTION_INSTALLER_VERSION) {
        return PRODUCTION_INSTALLER_BAD_VERSION;
    }
    /*
     * The gate is a planning check: it must be callable straight after the
     * controller confirmed the install and before the layout is executed, so
     * that a disk which cannot hold the boot set is refused before its first
     * write.  The writer below still requires the executing stage.
     */
    if (!installer->confirmed ||
        installer->stage == PRODUCTION_INSTALLER_STAGE_UNPREPARED) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    if (!production_boot_media_valid(media)) {
        return PRODUCTION_INSTALLER_SOURCE_UNAVAILABLE;
    }
    status = production_boot_check_esp(esp);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    production_boot_set_build(media, set);
    memset(&requirement, 0, sizeof(requirement));
    fat_status = efi_fat32_esp_payload_requirement(
        set, production_boot_set_size(media),
        PRODUCTION_BOOT_ESP_SECTORS_PER_CLUSTER, &requirement);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_boot_map_fat(fat_status);
    }
    outcome->esp_first_lba = esp->first_lba;
    outcome->esp_length_lba = esp->length_lba;
    outcome->required_sectors = requirement.required_sectors;
    outcome->payload_count = (uint32_t)production_boot_set_size(media);
    /*
     * The declared role must be large enough for the set.  A reused ESP is not
     * in the plan at all, so this bound is the one the discovery measured and
     * the free-cluster pre-flight in `_run()` is the gate that actually holds
     * for it.
     */
    if (esp->length_lba < requirement.required_sectors) {
        return PRODUCTION_INSTALLER_ESP_TOO_SMALL;
    }
    /*
     * The configuration the loader will read - the generated menu when there is
     * one, the sealed configuration otherwise.  The two calls are sequenced on
     * purpose: `installed_config_size` is an output of the first, and reading it
     * as an argument of the second would be an unsequenced read (the argument
     * order is unspecified), which is how the length was seen as zero.
     */
    {
        const unsigned char *installed_config =
            production_boot_installed_config(media, &installed_config_size);

        if (!production_boot_config_references_kernel(
                installed_config, installed_config_size)) {
            return PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID;
        }
    }
    return PRODUCTION_INSTALLER_OK;
}

static bool production_boot_variable_available(
    const struct efi_boot_environment *environment)
{
    if (environment == NULL ||
        efi_boot_environment_validate(environment) !=
            EFI_BOOT_ENVIRONMENT_OK) {
        return false;
    }
    return environment->platform == EFI_BOOT_FIRMWARE_UEFI &&
           environment->variable_read != NULL &&
           environment->variable_write != NULL &&
           environment->boot_device_path != NULL &&
           environment->boot_device_path_length != 0U;
}

enum production_installer_status production_installer_boot_stage_run(
    struct production_installer *installer,
    const struct efi_boot_environment *environment,
    const struct production_installer_boot_esp *esp_target,
    const struct production_installer_boot_media *media,
    struct production_installer_boot_outcome *outcome)
{
    static struct efi_fat32_esp_payload
        set[PRODUCTION_INSTALLER_BOOT_SET_COUNT + 2U];
    static unsigned char config[PRODUCTION_INSTALLER_BOOT_CONFIG_MAX];
    struct efi_fat32_esp esp;
    struct efi_fat32_esp_requirement requirement;
    enum efi_fat32_esp_status fat_status;
    enum production_installer_status status;
    uint64_t sealed_length = 0U;
    uint32_t free_clusters = 0U;
    size_t config_length = 0U;
    const unsigned char *installed_config = NULL;
    size_t installed_config_size = 0U;

    if (installer == NULL || environment == NULL || esp_target == NULL ||
        media == NULL || outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
    status = production_installer_boot_stage_requirements(
        installer, esp_target, media, outcome);
    if (status != PRODUCTION_INSTALLER_OK) {
        return status;
    }
    if (installer->stage !=
        PRODUCTION_INSTALLER_STAGE_EXECUTING) {
        return PRODUCTION_INSTALLER_BAD_STATE;
    }
    /*
     * Fail closed on the sealed media: without the ITEM-371 source digest the
     * staged bytes cannot be compared with what the media declared, so the
     * disk is not reported as bootable.
     */
    if (!installer->media_verified || !installer->source_digest_ready) {
        return PRODUCTION_INSTALLER_MEDIA_UNVERIFIED;
    }
    memset(&esp, 0, sizeof(esp));
    /*
     * A shared ESP belongs to the system that was already on the machine: it is
     * bound as it is and never formatted, whatever state it is in - a volume
     * that does not fit its partition is refused here, before the first write.
     * A whole-disk install owns the ESP role its own layout declared, so that
     * role is formatted before it is bound (ITEM-405), and a stale filesystem
     * the replaced partition table left in the range is never written into.
     */
    status = production_installer_esp_bind(
        environment, esp_target->first_lba, esp_target->length_lba,
        !esp_target->reused, &esp);
    if (status != PRODUCTION_INSTALLER_OK) {
        return esp_target->reused
                   ? PRODUCTION_INSTALLER_ESP_UNSUITABLE
                   : status;
    }
    production_boot_set_build(media, set);
    /*
     * The pre-write gate for a volume somebody else owns: the free clusters are
     * read first, so an ESP that cannot hold the set is refused with zero writes
     * instead of a partial set that cannot boot.
     */
    if (esp_target->reused) {
        fat_status = efi_fat32_esp_free_clusters(&esp, &free_clusters);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return production_boot_map_fat(fat_status);
        }
        fat_status = efi_fat32_esp_payload_requirement(
            set, production_boot_set_size(media),
            esp.sectors_per_cluster, &requirement);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return production_boot_map_fat(fat_status);
        }
        if ((uint64_t)free_clusters < requirement.required_clusters) {
            return PRODUCTION_INSTALLER_ESP_TOO_SMALL;
        }
    }
    /*
     * ITEM-398: before a byte is written, look at what the set is about to take
     * over.  The loader path is the removable-media fallback a firmware tries
     * when no boot entry matches, so on a shared ESP it is frequently the other
     * system's own fallback loader; the menu path is the loader's first
     * configuration candidate, so a foreign file there would shadow the menu
     * this install writes.  Either one is moved to its documented name - same
     * clusters, same bytes - and the record says what was found and what
     * happened to it.  A path that is free is recorded as free.
     */
    {
        bool present = false;
        uint32_t size = 0U;

        fat_status = efi_fat32_esp_probe_file(
            &esp, PRODUCTION_INSTALLER_BOOT_LOADER_PATH, &present,
            &size);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return production_boot_map_fat(fat_status);
        }
        if (present) {
            outcome->loader_path_occupied = true;
            outcome->loader_preserved_size = size;
            fat_status = efi_fat32_esp_rename_file(
                &esp, PRODUCTION_INSTALLER_BOOT_LOADER_PATH,
                PRODUCTION_INSTALLER_BOOT_PRESERVED_LOADER_PATH);
            if (fat_status != EFI_FAT32_ESP_OK) {
                return production_boot_map_fat(fat_status);
            }
            outcome->loader_preserved = true;
        }
        present = false;
        size = 0U;
        fat_status = efi_fat32_esp_probe_file(
            &esp, PRODUCTION_INSTALLER_BOOT_MENU_PATH, &present,
            &size);
        if (fat_status != EFI_FAT32_ESP_OK) {
            return production_boot_map_fat(fat_status);
        }
        if (present) {
            outcome->menu_path_occupied = true;
            outcome->menu_preserved_size = size;
            fat_status = efi_fat32_esp_rename_file(
                &esp, PRODUCTION_INSTALLER_BOOT_MENU_PATH,
                PRODUCTION_INSTALLER_BOOT_PRESERVED_MENU_PATH);
            if (fat_status != EFI_FAT32_ESP_OK) {
                return production_boot_map_fat(fat_status);
            }
            outcome->menu_preserved = true;
        }
    }
    fat_status = efi_fat32_esp_install_payload_set(
        &esp, set, production_boot_set_size(media));
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_boot_map_fat(fat_status);
    }
    outcome->payload_staged = true;

    fat_status = efi_fat32_esp_verify_payload_set(
        &esp, set, PRODUCTION_INSTALLER_BOOT_SET_COUNT, 3U,
        outcome->sealed_digest, &sealed_length);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_boot_map_fat(fat_status);
    }
    if (sealed_length != installer->source_length ||
        memcmp(outcome->sealed_digest, installer->source_digest,
               SHA256_DIGEST_SIZE) != 0) {
        return PRODUCTION_INSTALLER_VERIFICATION_FAILED;
    }
    outcome->sealed_length = sealed_length;
    outcome->payload_verified = true;
    outcome->menu_installed =
        media->menu != NULL && media->menu_size != 0U;
    /*
     * ITEM-409: what the loader reads for this install, how many entries it
     * offers and the timeout it declares.  The record states all three, so a
     * machine that carries nothing but this appliance still shows a menu and
     * the record says what that menu is.
     */
    outcome->config_source =
        outcome->menu_installed
            ? PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED
            : PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_SEALED;
    outcome->menu_entries =
        outcome->menu_installed ? media->menu_entries : 0U;
    outcome->menu_timeout_seconds = media->menu_timeout_seconds;

    /*
     * The staged configuration is read back from the ESP and must name the
     * staged kernel: the record then describes the configuration that is on
     * the disk, not the one the media carried.
     */
    installed_config = production_boot_installed_config(
        media, &installed_config_size);
    fat_status = efi_fat32_esp_read_file(
        &esp,
        media->menu != NULL && media->menu_size != 0U
            ? PRODUCTION_INSTALLER_BOOT_MENU_PATH
            : PRODUCTION_INSTALLER_BOOT_CONFIG_PATH,
        config, sizeof(config), &config_length);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return production_boot_map_fat(fat_status);
    }
    if (!production_boot_config_references_kernel(
            config, config_length) ||
        config_length != installed_config_size ||
        memcmp(config, installed_config, config_length) != 0) {
        return PRODUCTION_INSTALLER_BOOT_CONFIG_INVALID;
    }

    /*
     * The platform boot entry is created where the running environment can
     * create one.  Everywhere else the removable-media path is the mechanism
     * that makes the disk boot, and the record says so instead of claiming a
     * boot entry that does not exist.
     */
    if (production_boot_variable_available(environment)) {
        outcome->variable_attempted = true;
        status = production_installer_efi_apply_boot(
            installer, environment);
        outcome->variable_created =
            status == PRODUCTION_INSTALLER_OK;
        outcome->boot_entry_created = outcome->variable_created;
        outcome->mechanism = outcome->variable_created
            ? PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE
            : PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH;
    } else {
        outcome->mechanism =
            PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH;
    }
    (void)production_installer_boot_stage_publish(
        outcome, PRODUCTION_INSTALLER_OK);
    return PRODUCTION_INSTALLER_OK;
}

const char *production_installer_boot_mechanism_string(
    enum production_installer_boot_mechanism mechanism)
{
    switch (mechanism) {
    case PRODUCTION_INSTALLER_BOOT_MECHANISM_REMOVABLE_MEDIA_PATH:
        return "REMOVABLE_MEDIA_PATH";
    case PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE:
        return "EFI_BOOT_VARIABLE";
    case PRODUCTION_INSTALLER_BOOT_MECHANISM_BIOS_MBR:
        return "BIOS_MBR";
    case PRODUCTION_INSTALLER_BOOT_MECHANISM_NONE:
    default:
        return "NONE";
    }
}

enum production_installer_status production_installer_boot_stage_publish(
    const struct production_installer_boot_outcome *outcome,
    enum production_installer_status status)
{
    char record[PRODUCTION_INSTALLER_BOOT_RECORD_LENGTH];
    size_t length = 0U;
    uint64_t serial_flags;
    size_t index;

    if (outcome == NULL) {
        return PRODUCTION_INSTALLER_NULL;
    }
#define PRODUCTION_BOOT_APPEND(text) do { \
        static const char fragment[] = text; \
        const size_t fragment_length = sizeof(fragment) - 1U; \
        if (length + fragment_length >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        memcpy(record + length, fragment, fragment_length); \
        length += fragment_length; \
    } while (0)
#define PRODUCTION_BOOT_APPEND_TEXT(value) do { \
        const char *runtime_text = (value); \
        size_t runtime_length = 0U; \
        if (runtime_text == NULL) { \
            return PRODUCTION_INSTALLER_NULL; \
        } \
        while (runtime_text[runtime_length] != '\0') { \
            ++runtime_length; \
        } \
        if (length + runtime_length >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        memcpy(record + length, runtime_text, runtime_length); \
        length += runtime_length; \
    } while (0)
#define PRODUCTION_BOOT_APPEND_U64(value) do { \
        uint64_t digits = (uint64_t)(value); \
        char digits_buffer[20U]; \
        size_t digit_count = 0U; \
        if (digits == 0U) { \
            digits_buffer[digit_count++] = '0'; \
        } \
        while (digits != 0U) { \
            digits_buffer[digit_count++] = \
                (char)('0' + (digits % 10U)); \
            digits /= 10U; \
        } \
        if (length + digit_count >= sizeof(record)) { \
            return PRODUCTION_INSTALLER_CAPACITY; \
        } \
        while (digit_count > 0U) { \
            record[length++] = digits_buffer[--digit_count]; \
        } \
    } while (0)

    PRODUCTION_BOOT_APPEND("[PRODUCTION-INSTALL] boot-entry ");
    if (status == PRODUCTION_INSTALLER_OK &&
        outcome->payload_staged && outcome->payload_verified) {
        PRODUCTION_BOOT_APPEND("state=AVAILABLE mechanism=");
        PRODUCTION_BOOT_APPEND_TEXT(
            production_installer_boot_mechanism_string(
                outcome->mechanism));
        PRODUCTION_BOOT_APPEND(" path=");
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->mechanism ==
                    PRODUCTION_INSTALLER_BOOT_MECHANISM_EFI_BOOT_VARIABLE
                ? PRODUCTION_INSTALLER_BOOT_PAYLOAD_PATH
                : PRODUCTION_INSTALLER_BOOT_LOADER_PATH);
        PRODUCTION_BOOT_APPEND(" kernel=");
        PRODUCTION_BOOT_APPEND(
            PRODUCTION_INSTALLER_BOOT_KERNEL_PATH);
        PRODUCTION_BOOT_APPEND(" config=");
        PRODUCTION_BOOT_APPEND(
            PRODUCTION_INSTALLER_BOOT_CONFIG_PATH);
        PRODUCTION_BOOT_APPEND(" menu=");
        /*
         * ITEM-409: the menu is the installed configuration for every install,
         * so the record names the configuration the loader reads, how many
         * entries it offers, the timeout it declares and which of the two
         * configurations it is.  The sealed configuration is what the loader
         * reads only when no menu could be built at all, and this field is
         * where an operator (and the acceptance run) reads which happened.
         */
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->menu_installed ? PRODUCTION_INSTALLER_BOOT_MENU_PATH
                                    : PRODUCTION_INSTALLER_BOOT_CONFIG_PATH);
        PRODUCTION_BOOT_APPEND(" entries=");
        PRODUCTION_BOOT_APPEND_U64(
            outcome->menu_installed ? (uint64_t)outcome->menu_entries : 1U);
        if (outcome->menu_installed) {
            PRODUCTION_BOOT_APPEND(" timeout=");
            PRODUCTION_BOOT_APPEND_U64(
                (uint64_t)outcome->menu_timeout_seconds);
            PRODUCTION_BOOT_APPEND("s");
        }
        PRODUCTION_BOOT_APPEND(" source=");
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->config_source ==
                    PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED
                ? "GENERATED"
                : "SEALED");
        if (outcome->config_source !=
            PRODUCTION_INSTALLER_BOOT_CONFIG_SOURCE_GENERATED) {
            /*
             * The record says why the sealed configuration is the installed
             * one instead of leaving an operator to infer it from the fields
             * that are missing.
             */
            PRODUCTION_BOOT_APPEND(" menu_reason=MENU_UNAVAILABLE");
        }
        PRODUCTION_BOOT_APPEND(" payload_set=");
        /*
         * ITEM-398: the number of files this run staged, not the sealed
         * constant.  A generated menu is one more entry in the set, and a
         * record that said "5" while six files went to the disk would describe
         * a set the ESP does not carry.  The sealed prefix (the first
         * sealed five entries, and the first three for
         * the digest) is unchanged.
         */
        PRODUCTION_BOOT_APPEND_U64((uint64_t)outcome->payload_count);
        PRODUCTION_BOOT_APPEND(" required_sectors=");
        PRODUCTION_BOOT_APPEND_U64(outcome->required_sectors);
        PRODUCTION_BOOT_APPEND(" esp_first_lba=");
        PRODUCTION_BOOT_APPEND_U64(outcome->esp_first_lba);
        PRODUCTION_BOOT_APPEND(" esp_sectors=");
        PRODUCTION_BOOT_APPEND_U64(outcome->esp_length_lba);
        PRODUCTION_BOOT_APPEND(" sealed_bytes=");
        PRODUCTION_BOOT_APPEND_U64(outcome->sealed_length);
        PRODUCTION_BOOT_APPEND(" sealed_digest=");
        for (index = 0U; index < PRODUCTION_BOOT_HEX_DIGEST;
             ++index) {
            static const char hex[] = "0123456789abcdef";
            if (length + 2U >= sizeof(record)) {
                return PRODUCTION_INSTALLER_CAPACITY;
            }
            record[length++] =
                hex[(outcome->sealed_digest[index] >> 4U) & 0x0FU];
            record[length++] =
                hex[outcome->sealed_digest[index] & 0x0FU];
        }
        /*
         * ITEM-398: what the install found at the loader and configuration
         * paths, and what it did with it.  A shared ESP usually carries the
         * other system's fallback loader at the first path and, when that
         * loader is a Limine, its configuration at the second - which would
         * otherwise shadow the menu this install writes.
         */
        PRODUCTION_BOOT_APPEND(" existing_loader=");
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->loader_path_occupied ? "PRESERVED" : "FREE");
        if (outcome->loader_path_occupied) {
            PRODUCTION_BOOT_APPEND(" existing_loader_bytes=");
            PRODUCTION_BOOT_APPEND_U64(
                outcome->loader_preserved_size);
            PRODUCTION_BOOT_APPEND(" existing_loader_kept_at=");
            PRODUCTION_BOOT_APPEND(
                PRODUCTION_INSTALLER_BOOT_PRESERVED_LOADER_PATH);
        }
        PRODUCTION_BOOT_APPEND(" existing_config=");
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->menu_path_occupied ? "PRESERVED" : "FREE");
        if (outcome->menu_path_occupied) {
            PRODUCTION_BOOT_APPEND(" existing_config_bytes=");
            PRODUCTION_BOOT_APPEND_U64(
                outcome->menu_preserved_size);
            PRODUCTION_BOOT_APPEND(" existing_config_kept_at=");
            PRODUCTION_BOOT_APPEND(
                PRODUCTION_INSTALLER_BOOT_PRESERVED_MENU_PATH);
        }
        PRODUCTION_BOOT_APPEND(" verify=SEALED_MEDIA variable=");
        PRODUCTION_BOOT_APPEND_TEXT(
            !outcome->variable_attempted
                ? "UNAVAILABLE"
                : (outcome->variable_created ? "CREATED" : "FAILED"));
        PRODUCTION_BOOT_APPEND(" boot_entry=");
        PRODUCTION_BOOT_APPEND_TEXT(
            outcome->boot_entry_created ? "CREATED" : "NONE");
    } else {
        PRODUCTION_BOOT_APPEND("state=UNAVAILABLE reason=");
        PRODUCTION_BOOT_APPEND_TEXT(
            production_installer_status_string(status) != NULL
                ? production_installer_status_string(status)
                : "PRODUCTION_INSTALLER_UNKNOWN");
        PRODUCTION_BOOT_APPEND(" required_sectors=");
        PRODUCTION_BOOT_APPEND_U64(outcome->required_sectors);
        PRODUCTION_BOOT_APPEND(" esp_first_lba=");
        PRODUCTION_BOOT_APPEND_U64(outcome->esp_first_lba);
        PRODUCTION_BOOT_APPEND(" esp_sectors=");
        PRODUCTION_BOOT_APPEND_U64(outcome->esp_length_lba);
    }
    PRODUCTION_BOOT_APPEND("\n");
#undef PRODUCTION_BOOT_APPEND_U64
#undef PRODUCTION_BOOT_APPEND_TEXT
#undef PRODUCTION_BOOT_APPEND

    serial_flags = serial_sequence_begin();
    serial_write(record);
    serial_sequence_end(serial_flags);
    /*
     * ITEM-405: the same record used to be painted on the local console for an
     * acceptance host with no serial capture.  ITEM-478 sends it to the durable
     * journal after the serial sequence, so the serial record stays one
     * contiguous IRQ-off critical section and the journal path never touches
     * the framebuffer; `BOOTLOG` reads the reason back on demand.  The journal
     * path is additive - it never writes to serial.
     */
    console_publish_journal(record, length);
    return PRODUCTION_INSTALLER_OK;
}
