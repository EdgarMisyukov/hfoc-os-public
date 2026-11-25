#include "efi_boot_environment.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum efi_boot_environment_status efi_boot_environment_init(
    struct efi_boot_environment *environment,
    enum efi_boot_firmware_platform platform,
    struct block_device *block_device,
    void *variable_context,
    efi_boot_variable_read_fn variable_read,
    efi_boot_variable_write_fn variable_write)
{
    enum block_device_status block_status;

    if (environment == NULL) {
        return EFI_BOOT_ENVIRONMENT_NULL;
    }
    if (environment->installed) {
        return EFI_BOOT_ENVIRONMENT_BAD_STATE;
    }
    if (platform != EFI_BOOT_FIRMWARE_UEFI) {
        return EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM;
    }
    if (block_device == NULL) {
        return EFI_BOOT_ENVIRONMENT_BAD_DEVICE;
    }
    block_status = block_device_validate(block_device);
    if (block_status != BLOCK_DEVICE_OK) {
        return EFI_BOOT_ENVIRONMENT_BAD_DEVICE;
    }
    if (variable_read == NULL || variable_write == NULL) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }

    environment->version = EFI_BOOT_ENVIRONMENT_VERSION;
    environment->platform = platform;
    environment->installed = true;
    environment->block_device = block_device;
    environment->variable_context = variable_context;
    environment->variable_read = variable_read;
    environment->variable_write = variable_write;
    environment->boot_device_path = NULL;
    environment->boot_device_path_length = 0U;
    return EFI_BOOT_ENVIRONMENT_OK;
}

enum efi_boot_environment_status efi_boot_environment_set_device_path(
    struct efi_boot_environment *environment,
    const unsigned char *device_path,
    size_t device_path_length)
{
    if (environment == NULL || device_path == NULL) {
        return EFI_BOOT_ENVIRONMENT_NULL;
    }
    if (efi_boot_environment_validate(environment) !=
        EFI_BOOT_ENVIRONMENT_OK) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }
    if (device_path_length < 8U ||
        device_path_length > EFI_BOOT_DEVICE_PATH_MAX) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }
    if (device_path[device_path_length - 4U] != 0x7FU ||
        device_path[device_path_length - 3U] != 0xFFU ||
        device_path[device_path_length - 2U] != 0x04U ||
        device_path[device_path_length - 1U] != 0x00U) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }
    environment->boot_device_path = device_path;
    environment->boot_device_path_length = device_path_length;
    return EFI_BOOT_ENVIRONMENT_OK;
}

enum efi_boot_environment_status efi_boot_environment_validate(
    const struct efi_boot_environment *environment)
{
    enum block_device_status block_status;

    if (environment == NULL) {
        return EFI_BOOT_ENVIRONMENT_NULL;
    }
    if (!environment->installed) {
        return EFI_BOOT_ENVIRONMENT_UNAVAILABLE;
    }
    if (environment->version != EFI_BOOT_ENVIRONMENT_VERSION) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }
    if (environment->platform != EFI_BOOT_FIRMWARE_UEFI) {
        return EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM;
    }
    if (environment->block_device == NULL) {
        return EFI_BOOT_ENVIRONMENT_BAD_DEVICE;
    }
    block_status = block_device_validate(environment->block_device);
    if (block_status != BLOCK_DEVICE_OK) {
        return EFI_BOOT_ENVIRONMENT_BAD_DEVICE;
    }
    if (environment->variable_read == NULL ||
        environment->variable_write == NULL) {
        return EFI_BOOT_ENVIRONMENT_INVALID;
    }
    return EFI_BOOT_ENVIRONMENT_OK;
}

enum efi_boot_environment_status efi_boot_environment_status(
    const struct efi_boot_environment *environment)
{
    if (environment == NULL || !environment->installed) {
        return EFI_BOOT_ENVIRONMENT_UNAVAILABLE;
    }
    return efi_boot_environment_validate(environment);
}

const char *efi_boot_environment_status_string(
    enum efi_boot_environment_status status)
{
    switch (status) {
    case EFI_BOOT_ENVIRONMENT_OK:
        return "EFI_BOOT_ENVIRONMENT_OK";
    case EFI_BOOT_ENVIRONMENT_NULL:
        return "EFI_BOOT_ENVIRONMENT_NULL";
    case EFI_BOOT_ENVIRONMENT_INVALID:
        return "EFI_BOOT_ENVIRONMENT_INVALID";
    case EFI_BOOT_ENVIRONMENT_UNAVAILABLE:
        return "EFI_BOOT_ENVIRONMENT_UNAVAILABLE";
    case EFI_BOOT_ENVIRONMENT_BAD_STATE:
        return "EFI_BOOT_ENVIRONMENT_BAD_STATE";
    case EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM:
        return "EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM";
    case EFI_BOOT_ENVIRONMENT_BAD_DEVICE:
        return "EFI_BOOT_ENVIRONMENT_BAD_DEVICE";
    default:
        return NULL;
    }
}
