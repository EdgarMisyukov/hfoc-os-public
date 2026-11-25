#include "installer_boot_recovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool installer_boot_recovery_string_valid(
    const char *value,
    size_t capacity)
{
    size_t index;

    if (value == NULL || capacity == 0U) {
        return false;
    }
    for (index = 0U; index < capacity; ++index) {
        const unsigned char byte = (unsigned char)value[index];

        if (byte == 0U) {
            return index > 0U;
        }
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
    }
    return false;
}

static void installer_boot_recovery_copy_string(
    char destination[INSTALLER_BOOT_RECOVERY_PATH_LENGTH],
    const char *source)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_BOOT_RECOVERY_PATH_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (source == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_BOOT_RECOVERY_PATH_LENGTH &&
         source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

static void installer_boot_recovery_copy_identity(
    char destination[INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH],
    const char *source)
{
    size_t index;

    for (index = 0U;
         index < INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH;
         ++index) {
        destination[index] = '\0';
    }
    if (source == NULL) {
        return;
    }
    for (index = 0U;
         index + 1U < INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH &&
         source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

static bool installer_boot_recovery_evidence_valid(
    const struct installer_boot_recovery_evidence *evidence)
{
    return evidence != NULL &&
           evidence->version == INSTALLER_BOOT_RECOVERY_VERSION &&
           evidence->firmware_mode >= INSTALLER_FIRMWARE_UNKNOWN &&
           evidence->firmware_mode <= INSTALLER_FIRMWARE_BIOS_LEGACY;
}

static bool installer_boot_recovery_scenario_valid(
    enum installer_boot_recovery_scenario scenario)
{
    return scenario >=
               INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER &&
           scenario <=
               INSTALLER_BOOT_RECOVERY_SCENARIO_EXISTING_OS_PRESERVATION;
}

static bool installer_boot_recovery_plan_valid(
    const struct installer_boot_recovery_plan *plan)
{
    return plan != NULL &&
           plan->version == INSTALLER_BOOT_RECOVERY_VERSION &&
           installer_boot_recovery_scenario_valid(plan->scenario) &&
           plan->disk_id != 0U &&
           plan->esp_partition_id != 0U &&
           installer_boot_recovery_string_valid(
               plan->identity, sizeof(plan->identity)) &&
           installer_boot_recovery_string_valid(
               plan->recovery_path, sizeof(plan->recovery_path));
}

enum installer_boot_recovery_status installer_boot_recovery_assess(
    const struct installer_boot_recovery_evidence *evidence,
    enum installer_boot_recovery_scenario scenario,
    const char *recovery_path,
    const char *identity,
    struct installer_boot_recovery_plan *plan)
{
    bool previous_config_required;
    bool bootloader_mutation = false;
    bool efi_entry_mutation = false;

    if (!installer_boot_recovery_evidence_valid(evidence)) {
        return INSTALLER_BOOT_RECOVERY_BAD_VERSION;
    }
    if (!installer_boot_recovery_scenario_valid(scenario)) {
        return INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT;
    }
    if (!installer_boot_recovery_string_valid(
            recovery_path, INSTALLER_BOOT_RECOVERY_PATH_LENGTH) ||
        !installer_boot_recovery_string_valid(
            identity, INSTALLER_BOOT_RECOVERY_IDENTITY_LENGTH)) {
        return INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT;
    }
    if (plan == NULL) {
        return INSTALLER_BOOT_RECOVERY_NULL;
    }
    if (evidence->firmware_mode != INSTALLER_FIRMWARE_UEFI) {
        return INSTALLER_BOOT_RECOVERY_UNSUPPORTED_FIRMWARE;
    }
    if (evidence->disk_id == 0U ||
        evidence->esp_partition_id == 0U ||
        !evidence->esp_present) {
        return INSTALLER_BOOT_RECOVERY_MISSING_ESP;
    }
    if (!evidence->recovery_media_available) {
        return INSTALLER_BOOT_RECOVERY_RECOVERY_MEDIA_UNAVAILABLE;
    }
    if (!evidence->existing_os_boot_paths_preserved) {
        return INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED;
    }
    previous_config_required =
        scenario !=
        INSTALLER_BOOT_RECOVERY_SCENARIO_EXISTING_OS_PRESERVATION;
    if (previous_config_required &&
        !evidence->previous_boot_configuration_available) {
        return INSTALLER_BOOT_RECOVERY_PREVIOUS_CONFIG_UNAVAILABLE;
    }

    switch (scenario) {
    case INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER:
        bootloader_mutation = true;
        break;
    case INSTALLER_BOOT_RECOVERY_SCENARIO_LOST_HFOC_EFI_ENTRY:
        efi_entry_mutation = true;
        break;
    case INSTALLER_BOOT_RECOVERY_SCENARIO_FAILED_BOOTLOADER_UPDATE:
        bootloader_mutation = true;
        break;
    case INSTALLER_BOOT_RECOVERY_SCENARIO_HFOC_BOOT_RESTORATION:
        bootloader_mutation = true;
        efi_entry_mutation = true;
        break;
    case INSTALLER_BOOT_RECOVERY_SCENARIO_EXISTING_OS_PRESERVATION:
        break;
    default:
        return INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT;
    }

    memset(plan, 0, sizeof(*plan));
    plan->version = INSTALLER_BOOT_RECOVERY_VERSION;
    plan->scenario = scenario;
    plan->disk_id = evidence->disk_id;
    plan->esp_partition_id = evidence->esp_partition_id;
    plan->bootloader_mutation_required = bootloader_mutation;
    plan->efi_entry_mutation_required = efi_entry_mutation;
    plan->recovery_media_required = true;
    plan->previous_configuration_required = previous_config_required;
    plan->existing_os_preserved =
        evidence->existing_os_boot_paths_preserved;
    plan->confirmed = false;
    plan->complete = false;
    installer_boot_recovery_copy_identity(plan->identity, identity);
    installer_boot_recovery_copy_string(
        plan->recovery_path, recovery_path);
    return INSTALLER_BOOT_RECOVERY_OK;
}

static void installer_boot_recovery_append_char(
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

static void installer_boot_recovery_append_text(
    char *buffer,
    size_t capacity,
    size_t *length,
    const char *text)
{
    size_t index;

    for (index = 0U; text[index] != '\0'; ++index) {
        installer_boot_recovery_append_char(
            buffer, capacity, length, text[index]);
    }
}

static void installer_boot_recovery_append_uint64(
    char *buffer,
    size_t capacity,
    size_t *length,
    uint64_t value)
{
    char reversed[20U];
    size_t count = 0U;

    if (value == 0U) {
        installer_boot_recovery_append_char(
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
        installer_boot_recovery_append_char(
            buffer, capacity, length, reversed[count]);
    }
}

static const char *installer_boot_recovery_scenario_text(
    enum installer_boot_recovery_scenario scenario)
{
    switch (scenario) {
    case INSTALLER_BOOT_RECOVERY_SCENARIO_DAMAGED_HFOC_BOOTLOADER:
        return "damaged bootloader";
    case INSTALLER_BOOT_RECOVERY_SCENARIO_LOST_HFOC_EFI_ENTRY:
        return "lost EFI entry";
    case INSTALLER_BOOT_RECOVERY_SCENARIO_FAILED_BOOTLOADER_UPDATE:
        return "failed update";
    case INSTALLER_BOOT_RECOVERY_SCENARIO_HFOC_BOOT_RESTORATION:
        return "boot restoration";
    case INSTALLER_BOOT_RECOVERY_SCENARIO_EXISTING_OS_PRESERVATION:
        return "existing OS preservation";
    default:
        return "unknown";
    }
}

enum installer_boot_recovery_status installer_boot_recovery_preview(
    const struct installer_boot_recovery_plan *plan,
    struct installer_boot_recovery_preview *preview)
{
    size_t length = 0U;

    if (preview == NULL) {
        return INSTALLER_BOOT_RECOVERY_NULL;
    }
    if (!installer_boot_recovery_plan_valid(plan)) {
        return INSTALLER_BOOT_RECOVERY_BAD_STATE;
    }
    memset(preview, 0, sizeof(*preview));
    preview->version = INSTALLER_BOOT_RECOVERY_VERSION;
    preview->scenario = plan->scenario;
    preview->disk_id = plan->disk_id;
    preview->esp_partition_id = plan->esp_partition_id;
    preview->bootloader_mutation_required =
        plan->bootloader_mutation_required;
    preview->efi_entry_mutation_required =
        plan->efi_entry_mutation_required;
    preview->existing_os_preserved = plan->existing_os_preserved;
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->identity);
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        " recovery ");
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        installer_boot_recovery_scenario_text(plan->scenario));
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        " bootloader_mutation=");
    installer_boot_recovery_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->bootloader_mutation_required ? 1U : 0U);
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        " efi_mutation=");
    installer_boot_recovery_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->efi_entry_mutation_required ? 1U : 0U);
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        " preserve=");
    installer_boot_recovery_append_uint64(
        preview->text, sizeof(preview->text), &length,
        plan->existing_os_preserved ? 1U : 0U);
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        " path=");
    installer_boot_recovery_append_text(
        preview->text, sizeof(preview->text), &length,
        plan->recovery_path);
    return INSTALLER_BOOT_RECOVERY_OK;
}

enum installer_boot_recovery_status installer_boot_recovery_confirm(
    struct installer_boot_recovery_plan *plan)
{
    if (!installer_boot_recovery_plan_valid(plan)) {
        return INSTALLER_BOOT_RECOVERY_BAD_STATE;
    }
    if (plan->complete) {
        return INSTALLER_BOOT_RECOVERY_BAD_STATE;
    }
    plan->confirmed = true;
    return INSTALLER_BOOT_RECOVERY_OK;
}

enum installer_boot_recovery_status installer_boot_recovery_verify_outcome(
    struct installer_boot_recovery_plan *plan,
    bool hfoc_bootloader_present,
    bool hfoc_efi_entry_present,
    bool existing_os_boot_paths_preserved)
{
    if (!installer_boot_recovery_plan_valid(plan)) {
        return INSTALLER_BOOT_RECOVERY_BAD_STATE;
    }
    if (!plan->confirmed) {
        return INSTALLER_BOOT_RECOVERY_NOT_CONFIRMED;
    }
    if (!existing_os_boot_paths_preserved) {
        return INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED;
    }
    if (plan->bootloader_mutation_required &&
        !hfoc_bootloader_present) {
        return INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR;
    }
    if (plan->efi_entry_mutation_required &&
        !hfoc_efi_entry_present) {
        return INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR;
    }
    plan->complete = true;
    return INSTALLER_BOOT_RECOVERY_OK;
}

const char *installer_boot_recovery_status_string(
    enum installer_boot_recovery_status status)
{
    switch (status) {
    case INSTALLER_BOOT_RECOVERY_OK:
        return "INSTALLER_BOOT_RECOVERY_OK";
    case INSTALLER_BOOT_RECOVERY_NULL:
        return "INSTALLER_BOOT_RECOVERY_NULL";
    case INSTALLER_BOOT_RECOVERY_BAD_VERSION:
        return "INSTALLER_BOOT_RECOVERY_BAD_VERSION";
    case INSTALLER_BOOT_RECOVERY_BAD_STATE:
        return "INSTALLER_BOOT_RECOVERY_BAD_STATE";
    case INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT:
        return "INSTALLER_BOOT_RECOVERY_INVALID_ARGUMENT";
    case INSTALLER_BOOT_RECOVERY_UNSUPPORTED_FIRMWARE:
        return "INSTALLER_BOOT_RECOVERY_UNSUPPORTED_FIRMWARE";
    case INSTALLER_BOOT_RECOVERY_MISSING_ESP:
        return "INSTALLER_BOOT_RECOVERY_MISSING_ESP";
    case INSTALLER_BOOT_RECOVERY_RECOVERY_MEDIA_UNAVAILABLE:
        return "INSTALLER_BOOT_RECOVERY_RECOVERY_MEDIA_UNAVAILABLE";
    case INSTALLER_BOOT_RECOVERY_PREVIOUS_CONFIG_UNAVAILABLE:
        return "INSTALLER_BOOT_RECOVERY_PREVIOUS_CONFIG_UNAVAILABLE";
    case INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED:
        return "INSTALLER_BOOT_RECOVERY_EXISTING_OS_UNPROTECTED";
    case INSTALLER_BOOT_RECOVERY_NOT_CONFIRMED:
        return "INSTALLER_BOOT_RECOVERY_NOT_CONFIRMED";
    case INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR:
        return "INSTALLER_BOOT_RECOVERY_INCOMPLETE_REPAIR";
    default:
        return NULL;
    }
}
