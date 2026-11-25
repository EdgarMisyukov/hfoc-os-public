#ifndef HFOC_EFI_INSTALLER_H
#define HFOC_EFI_INSTALLER_H

#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "efi_boot_environment.h"

#define HFOC_EFI_INSTALLER_VERSION 1U

enum hfoc_efi_installer_status {
    HFOC_EFI_INSTALLER_OK = 0,
    HFOC_EFI_INSTALLER_NULL = 1,
    HFOC_EFI_INSTALLER_BAD_VERSION = 2,
    HFOC_EFI_INSTALLER_INVALID_DEVICE = 3,
    HFOC_EFI_INSTALLER_NO_TARGET = 4,
    HFOC_EFI_INSTALLER_AMBIGUOUS_TARGET = 5,
    HFOC_EFI_INSTALLER_NO_ESP = 6,
    HFOC_EFI_INSTALLER_AMBIGUOUS_ESP = 7,
    HFOC_EFI_INSTALLER_INVALID_PAYLOAD = 8,
    HFOC_EFI_INSTALLER_EFI_ENVIRONMENT = 9,
    HFOC_EFI_INSTALLER_ESP_ERROR = 10,
    HFOC_EFI_INSTALLER_BOOT_VARIABLE_ERROR = 11,
};

struct hfoc_efi_installer_target {
    uint32_t version;
    struct block_device *device;
    uint64_t esp_first_lba;
    uint64_t esp_block_count;
    uint64_t hfoc_first_lba;
    uint64_t hfoc_block_count;
};

enum hfoc_efi_installer_status hfoc_efi_installer_select_target(
    struct block_device *devices,
    size_t device_count,
    struct hfoc_efi_installer_target *target);
enum hfoc_efi_installer_status hfoc_efi_installer_apply(
    const struct efi_boot_environment *environment,
    const struct hfoc_efi_installer_target *target,
    const unsigned char *payload,
    size_t payload_size);
const char *hfoc_efi_installer_status_string(
    enum hfoc_efi_installer_status status);

#endif
