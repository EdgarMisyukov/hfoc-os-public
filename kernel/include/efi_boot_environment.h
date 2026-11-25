#ifndef HFOC_EFI_BOOT_ENVIRONMENT_H
#define HFOC_EFI_BOOT_ENVIRONMENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

#define EFI_BOOT_ENVIRONMENT_VERSION 1U
#define EFI_BOOT_VARIABLE_NAME_MAX 32U
#define EFI_BOOT_VARIABLE_GUID_LENGTH 16U
#define EFI_BOOT_DEVICE_PATH_MAX 512U

enum efi_boot_firmware_platform {
    EFI_BOOT_FIRMWARE_UNKNOWN = 0,
    EFI_BOOT_FIRMWARE_UEFI = 1,
    EFI_BOOT_FIRMWARE_BIOS_LEGACY = 2,
};

enum efi_boot_environment_status {
    EFI_BOOT_ENVIRONMENT_OK = 0,
    EFI_BOOT_ENVIRONMENT_NULL = 1,
    EFI_BOOT_ENVIRONMENT_INVALID = 2,
    EFI_BOOT_ENVIRONMENT_UNAVAILABLE = 3,
    EFI_BOOT_ENVIRONMENT_BAD_STATE = 4,
    EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM = 5,
    EFI_BOOT_ENVIRONMENT_BAD_DEVICE = 6,
};

enum efi_boot_variable_io_status {
    EFI_BOOT_VARIABLE_IO_OK = 0,
    EFI_BOOT_VARIABLE_IO_NOT_FOUND = 1,
    EFI_BOOT_VARIABLE_IO_ACCESS_DENIED = 2,
    EFI_BOOT_VARIABLE_IO_BUFFER_TOO_SMALL = 3,
    EFI_BOOT_VARIABLE_IO_ERROR = 4,
};

struct efi_boot_variable_key {
    unsigned char guid[EFI_BOOT_VARIABLE_GUID_LENGTH];
    char name[EFI_BOOT_VARIABLE_NAME_MAX];
};

typedef enum efi_boot_variable_io_status (*efi_boot_variable_read_fn)(
    void *context,
    const struct efi_boot_variable_key *key,
    unsigned char *data,
    size_t capacity,
    size_t *length);
typedef enum efi_boot_variable_io_status (*efi_boot_variable_write_fn)(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length);

struct efi_boot_environment {
    uint32_t version;
    enum efi_boot_firmware_platform platform;
    bool installed;
    struct block_device *block_device;
    void *variable_context;
    efi_boot_variable_read_fn variable_read;
    efi_boot_variable_write_fn variable_write;
    /* The caller retains ownership of the device-path storage. */
    const unsigned char *boot_device_path;
    size_t boot_device_path_length;
};

enum efi_boot_environment_status efi_boot_environment_init(
    struct efi_boot_environment *environment,
    enum efi_boot_firmware_platform platform,
    struct block_device *block_device,
    void *variable_context,
    efi_boot_variable_read_fn variable_read,
    efi_boot_variable_write_fn variable_write);
enum efi_boot_environment_status efi_boot_environment_set_device_path(
    struct efi_boot_environment *environment,
    const unsigned char *device_path,
    size_t device_path_length);
enum efi_boot_environment_status efi_boot_environment_validate(
    const struct efi_boot_environment *environment);
enum efi_boot_environment_status efi_boot_environment_status(
    const struct efi_boot_environment *environment);
const char *efi_boot_environment_status_string(
    enum efi_boot_environment_status status);

#endif
