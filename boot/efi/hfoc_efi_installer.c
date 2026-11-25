#include "hfoc_efi_installer.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "appliance_gpt_discovery.h"
#include "efi_boot_variables.h"
#include "efi_fat32_esp.h"
#include "installer_hardware_discovery.h"

static enum hfoc_efi_installer_status
hfoc_efi_installer_scan_esp(
    struct block_device *device,
    struct hfoc_efi_installer_target *target)
{
    struct installer_discovery_report report;
    const struct installer_discovery_partition *esp = NULL;
    size_t esp_count = 0U;
    size_t index;

    if (installer_discovery_init(&report) !=
            INSTALLER_DISCOVERY_OK ||
        installer_discovery_scan_block_device(
            &report, device, 1U,
            INSTALLER_DISK_CONTROLLER_UNKNOWN,
            "hfoc-efi-target", sizeof("hfoc-efi-target") - 1U) !=
            INSTALLER_DISCOVERY_OK) {
        return HFOC_EFI_INSTALLER_INVALID_DEVICE;
    }
    for (index = 0U;
         index < INSTALLER_DISCOVERY_PARTITION_CAPACITY;
         ++index) {
        if (!report.partitions[index].present ||
            !report.partitions[index].efi_system) {
            continue;
        }
        ++esp_count;
        esp = &report.partitions[index];
    }
    if (esp_count == 0U) {
        return HFOC_EFI_INSTALLER_NO_ESP;
    }
    if (esp_count != 1U || esp == NULL) {
        return HFOC_EFI_INSTALLER_AMBIGUOUS_ESP;
    }
    target->esp_first_lba = esp->first_lba;
    target->esp_block_count = esp->length_lba;
    return HFOC_EFI_INSTALLER_OK;
}

enum hfoc_efi_installer_status hfoc_efi_installer_select_target(
    struct block_device *devices,
    size_t device_count,
    struct hfoc_efi_installer_target *target)
{
    struct hfoc_efi_installer_target selected;
    size_t index;

    if (devices == NULL || target == NULL) {
        return HFOC_EFI_INSTALLER_NULL;
    }
    memset(&selected, 0, sizeof(selected));
    selected.version = HFOC_EFI_INSTALLER_VERSION;
    for (index = 0U; index < device_count; ++index) {
        struct appliance_gpt_hfoc_partition partition;
        enum appliance_gpt_discovery_status discovery_status;
        enum hfoc_efi_installer_status status;

        if (block_device_validate(&devices[index]) != BLOCK_DEVICE_OK) {
            return HFOC_EFI_INSTALLER_INVALID_DEVICE;
        }
        memset(&partition, 0, sizeof(partition));
        discovery_status = appliance_gpt_find_hfoc_partition(
            &devices[index], &partition);
        if (discovery_status == APPLIANCE_GPT_DISCOVERY_NOT_FOUND ||
            discovery_status == APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE) {
            continue;
        }
        if (discovery_status ==
            APPLIANCE_GPT_DISCOVERY_AMBIGUOUS) {
            return HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET;
        }
        if (discovery_status != APPLIANCE_GPT_DISCOVERY_OK ||
            !partition.present) {
            return HFOC_EFI_INSTALLER_INVALID_DEVICE;
        }
        if (selected.device != NULL) {
            return HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET;
        }
        selected.device = &devices[index];
        selected.hfoc_first_lba = partition.first_lba;
        selected.hfoc_block_count = partition.block_count;
        status = hfoc_efi_installer_scan_esp(
            &devices[index], &selected);
        if (status != HFOC_EFI_INSTALLER_OK) {
            return status;
        }
    }
    if (selected.device == NULL) {
        return HFOC_EFI_INSTALLER_NO_TARGET;
    }
    *target = selected;
    return HFOC_EFI_INSTALLER_OK;
}

enum hfoc_efi_installer_status hfoc_efi_installer_apply(
    const struct efi_boot_environment *environment,
    const struct hfoc_efi_installer_target *target,
    const unsigned char *payload,
    size_t payload_size)
{
    struct efi_fat32_esp esp;
    struct efi_boot_variables_snapshot snapshot;
    struct efi_boot_variables_plan plan;
    enum efi_fat32_esp_status fat_status;
    enum efi_boot_variables_status variables_status;

    if (environment == NULL || target == NULL || payload == NULL) {
        return HFOC_EFI_INSTALLER_NULL;
    }
    if (target->version != HFOC_EFI_INSTALLER_VERSION ||
        target->device == NULL ||
        target->esp_first_lba == 0U ||
        target->esp_block_count == 0U ||
        target->hfoc_first_lba == 0U ||
        target->hfoc_block_count == 0U) {
        return HFOC_EFI_INSTALLER_BAD_VERSION;
    }
    if (payload_size == 0U ||
        payload_size > EFI_FAT32_ESP_PAYLOAD_MAX) {
        return HFOC_EFI_INSTALLER_INVALID_PAYLOAD;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return HFOC_EFI_INSTALLER_EFI_ENVIRONMENT;
    }

    if (efi_boot_variables_snapshot(
            environment, &snapshot) != EFI_BOOT_VARIABLES_OK) {
        return HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR;
    }
    if (efi_boot_variables_plan(
            environment, &plan) != EFI_BOOT_VARIABLES_OK) {
        return HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR;
    }
    if (efi_boot_variables_confirm(&plan) !=
        EFI_BOOT_VARIABLES_OK) {
        return HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR;
    }

    memset(&esp, 0, sizeof(esp));
    fat_status = efi_fat32_esp_bind(
        environment, target->esp_first_lba,
        target->esp_block_count, &esp);
    if (fat_status != EFI_FAT32_ESP_OK) {
        return HFOC_EFI_INSTALLER_ESP_ERROR;
    }
    fat_status = efi_fat32_esp_install_payload(
        &esp, payload, payload_size);
    if (fat_status == EFI_FAT32_ESP_OK) {
        fat_status = efi_fat32_esp_verify_payload(&esp);
    }
    if (fat_status != EFI_FAT32_ESP_OK) {
        return HFOC_EFI_INSTALLER_ESP_ERROR;
    }

    variables_status = efi_boot_variables_apply(
        environment, &snapshot, &plan);
    if (variables_status == EFI_BOOT_VARIABLES_OK) {
        variables_status = efi_boot_variables_verify(
            environment, &plan);
    }
    if (variables_status != EFI_BOOT_VARIABLES_OK) {
        return HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR;
    }
    return HFOC_EFI_INSTALLER_OK;
}

const char *hfoc_efi_installer_status_string(
    enum hfoc_efi_installer_status status)
{
    switch (status) {
    case HFOC_EFI_INSTALLER_OK:
        return "HFOC_EFI_INSTALLER_OK";
    case HFOC_EFI_INSTALLER_NULL:
        return "HFOC_EFI_INSTALLER_NULL";
    case HFOC_EFI_INSTALLER_BAD_VERSION:
        return "HFOC_EFI_INSTALLER_BAD_VERSION";
    case HFOC_EFI_INSTALLER_INVALID_DEVICE:
        return "HFOC_EFI_INSTALLER_INVALID_DEVICE";
    case HFOC_EFI_INSTALLER_NO_TARGET:
        return "HFOC_EFI_INSTALLER_NO_TARGET";
    case HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET:
        return "HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET";
    case HFOC_EFI_INSTALLER_NO_ESP:
        return "HFOC_EFI_INSTALLER_NO_ESP";
    case HFOC_EFI_INSTALLER_AMBIGUOUS_ESP:
        return "HFOC_EFI_INSTALLER_AMBIGUOUS_ESP";
    case HFOC_EFI_INSTALLER_INVALID_PAYLOAD:
        return "HFOC_EFI_INSTALLER_INVALID_PAYLOAD";
    case HFOC_EFI_INSTALLER_EFI_ENVIRONMENT:
        return "HFOC_EFI_INSTALLER_EFI_ENVIRONMENT";
    case HFOC_EFI_INSTALLER_ESP_ERROR:
        return "HFOC_EFI_INSTALLER_ESP_ERROR";
    case HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR:
        return "HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR";
    default:
        return NULL;
    }
}
