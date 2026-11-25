#include "efi_boot_environment.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_EFI_BOOT_ENVIRONMENT_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_BLOCK_COUNT 2048U

static unsigned char fixture_storage[TEST_BLOCK_SIZE * TEST_BLOCK_COUNT];
static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static enum block_device_status fixture_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, fixture_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(fixture_storage) ||
        length > sizeof(fixture_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(fixture_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status fixture_flush(
    struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static enum efi_boot_variable_io_status fixture_read_variable(
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
    (void)length;
    return EFI_BOOT_VARIABLE_IO_NOT_FOUND;
}

static enum efi_boot_variable_io_status fixture_write_variable(
    void *context,
    const struct efi_boot_variable_key *key,
    const unsigned char *data,
    size_t length)
{
    (void)context;
    (void)key;
    (void)data;
    (void)length;
    return EFI_BOOT_VARIABLE_IO_OK;
}

static struct block_device make_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_BLOCK_COUNT;
    device.read_fn = fixture_read;
    device.write_fn = fixture_write;
    device.flush_fn = fixture_flush;
    return device;
}

static void test_unavailable_and_invalid(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_boot_environment zeroed;

    memset(&environment, 0, sizeof(environment));
    memset(&zeroed, 0, sizeof(zeroed));

    CHECK(efi_boot_environment_status(NULL) ==
              EFI_BOOT_ENVIRONMENT_UNAVAILABLE,
          "null environment reports unavailable");
    CHECK(efi_boot_environment_status(&zeroed) ==
              EFI_BOOT_ENVIRONMENT_UNAVAILABLE,
          "uninstalled environment reports unavailable");
    CHECK(efi_boot_environment_validate(NULL) ==
              EFI_BOOT_ENVIRONMENT_NULL,
          "null validation reports null");
    CHECK(efi_boot_environment_validate(&zeroed) ==
              EFI_BOOT_ENVIRONMENT_UNAVAILABLE,
          "uninstalled validation reports unavailable");

    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, NULL,
              NULL, fixture_read_variable, fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_BAD_DEVICE,
          "init rejects missing block device");
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              NULL, NULL, fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_INVALID,
          "init rejects missing variable read callback");
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_BIOS_LEGACY, &device,
              NULL, fixture_read_variable, fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM,
          "init rejects non-UEFI platform");
    device.block_size = 511U;
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              NULL, fixture_read_variable, fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_BAD_DEVICE,
          "init rejects invalid block device");
    device.block_size = TEST_BLOCK_SIZE;
}

static void test_valid_environment(void)
{
    struct block_device device = make_device();
    struct efi_boot_environment environment;
    struct efi_boot_environment copy;
    unsigned char nonaligned_path[14U] = {
        0x01U, 0x01U, 0x04U, 0x00U,
        0x03U, 0x01U, 0x06U, 0x00U,
        0x00U, 0x00U,
        0x7FU, 0xFFU, 0x04U, 0x00U
    };

    memset(&environment, 0, sizeof(environment));
    memset(&copy, 0, sizeof(copy));
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              (void *)0x1U, fixture_read_variable,
              fixture_write_variable) == EFI_BOOT_ENVIRONMENT_OK,
          "valid UEFI environment initializes");
    CHECK(environment.version == EFI_BOOT_ENVIRONMENT_VERSION &&
              environment.installed,
          "initialized environment stores exact version and state");
    CHECK(efi_boot_environment_validate(&environment) ==
              EFI_BOOT_ENVIRONMENT_OK,
          "valid UEFI environment validates");
    CHECK(efi_boot_environment_status(&environment) ==
              EFI_BOOT_ENVIRONMENT_OK,
          "valid UEFI environment status is OK");
    CHECK(efi_boot_environment_set_device_path(
              &environment, nonaligned_path,
              sizeof(nonaligned_path)) == EFI_BOOT_ENVIRONMENT_OK &&
              environment.boot_device_path == nonaligned_path &&
              environment.boot_device_path_length ==
                  sizeof(nonaligned_path),
          "valid device path length need not be four-byte aligned");
    nonaligned_path[sizeof(nonaligned_path) - 4U] = 0U;
    CHECK(efi_boot_environment_set_device_path(
              &environment, nonaligned_path,
              sizeof(nonaligned_path)) ==
              EFI_BOOT_ENVIRONMENT_INVALID,
          "device path without an end node is rejected");
    CHECK(efi_boot_environment_set_device_path(
              &environment, NULL, 0U) ==
              EFI_BOOT_ENVIRONMENT_NULL,
          "null device path is rejected");
    CHECK(efi_boot_environment_init(
              &environment, EFI_BOOT_FIRMWARE_UEFI, &device,
              NULL, fixture_read_variable, fixture_write_variable) ==
              EFI_BOOT_ENVIRONMENT_BAD_STATE,
          "already installed environment rejects reinitialization");

    copy = environment;
    copy.variable_read = NULL;
    CHECK(efi_boot_environment_validate(&copy) ==
              EFI_BOOT_ENVIRONMENT_INVALID,
          "missing callback invalidates environment copy");
    copy = environment;
    copy.version = 99U;
    CHECK(efi_boot_environment_validate(&copy) ==
              EFI_BOOT_ENVIRONMENT_INVALID,
          "version mismatch invalidates environment copy");
    copy = environment;
    copy.platform = EFI_BOOT_FIRMWARE_UNKNOWN;
    CHECK(efi_boot_environment_validate(&copy) ==
              EFI_BOOT_ENVIRONMENT_UNSUPPORTED_PLATFORM,
          "unknown platform is rejected");
    copy = environment;
    copy.block_device = NULL;
    CHECK(efi_boot_environment_validate(&copy) ==
              EFI_BOOT_ENVIRONMENT_BAD_DEVICE,
          "missing block device invalidates environment copy");
}

static void test_status_strings(void)
{
    CHECK(strcmp(efi_boot_environment_status_string(
                     EFI_BOOT_ENVIRONMENT_OK),
                 "EFI_BOOT_ENVIRONMENT_OK") == 0,
          "environment OK status string is exact");
    CHECK(strcmp(efi_boot_environment_status_string(
                     EFI_BOOT_ENVIRONMENT_UNAVAILABLE),
                 "EFI_BOOT_ENVIRONMENT_UNAVAILABLE") == 0,
          "environment unavailable status string is exact");
    CHECK(efi_boot_environment_status_string(
              (enum efi_boot_environment_status)99U) == NULL,
          "undefined environment status string is rejected");
}

int main(void)
{
    memset(fixture_storage, 0, sizeof(fixture_storage));
    test_unavailable_and_invalid();
    test_valid_environment();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr,
                "EFI boot environment host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("EFI boot environment host tests: ALL OK");
    return 0;
}

#endif
