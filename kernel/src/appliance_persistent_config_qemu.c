#ifdef HFOC_APPLIANCE_PERSISTENT_CONFIG_QEMU_TEST

#include "appliance_persistent_config.h"
#include "block_device.h"
#include "data_region.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

extern uint64_t vmm_hhdm_offset(void);

#define APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_SIZE 512U
#define APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_COUNT 8U
#define APPLIANCE_PERSISTENT_CONFIG_QEMU_CAPACITY \
    (APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_SIZE * \
     APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_COUNT)
#define APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_OFFSET 1U
#define APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_COUNT 7U
#define PERSISTENT_CONFIG_REBOOT_PHYS_ADDRESS UINT64_C(0x1f000000)
#define PERSISTENT_CONFIG_REBOOT_METADATA_BYTES 4096U
#define PERSISTENT_CONFIG_REBOOT_STATE_OFFSET 8U
#define PERSISTENT_CONFIG_REBOOT_STATE_FRESH 1U
#define PERSISTENT_CONFIG_REBOOT_STATE_COMMITTED 2U
#define PERSISTENT_CONFIG_REBOOT_STATE_INVALID 3U

static const unsigned char persistent_config_reboot_magic[8U] = {
    'H', 'F', 'O', 'C', 'R', 'E', 'B', 'T'
};

static unsigned char persistent_config_qemu_storage[
    APPLIANCE_PERSISTENT_CONFIG_QEMU_CAPACITY];
static size_t persistent_config_qemu_write_calls;

static void persistent_config_qemu_zero_bytes(void *destination,
                                              size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void persistent_config_qemu_copy_bytes(void *destination,
                                              const void *source,
                                              size_t length)
{
    unsigned char *out = destination;
    const unsigned char *in = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        out[index] = in[index];
    }
}

static bool persistent_config_qemu_equal_bytes(const void *left,
                                               const void *right,
                                               size_t length)
{
    const unsigned char *a = left;
    const unsigned char *b = right;
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (a[index] != b[index]) {
            return false;
        }
    }
    return true;
}

static void persistent_config_qemu_fill_bytes(unsigned char *bytes,
                                              unsigned char value,
                                              size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = value;
    }
}

static void persistent_config_qemu_store_le64(unsigned char *bytes,
                                              uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void persistent_config_qemu_failure(const char *message)
    __attribute__((noreturn));

static void persistent_config_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PERSISTENT-CONFIG] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void persistent_config_qemu_require(bool condition,
                                           const char *message)
{
    if (!condition) {
        persistent_config_qemu_failure(message);
    }
}

static enum block_device_status persistent_config_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    persistent_config_qemu_copy_bytes(
        buffer, persistent_config_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status persistent_config_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    ++persistent_config_qemu_write_calls;
    persistent_config_qemu_copy_bytes(
        persistent_config_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static bool persistent_config_qemu_hostname_is(
    const struct appliance_persistent_config *config,
    const char *expected)
{
    char hostname[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    size_t length = 0U;
    size_t expected_length = 0U;

    while (expected[expected_length] != '\0') {
        ++expected_length;
    }
    return appliance_persistent_config_hostname(
               config, hostname, sizeof(hostname), &length) ==
               APPLIANCE_PERSISTENT_CONFIG_OK &&
           length == expected_length &&
           persistent_config_qemu_equal_bytes(
               hostname, expected, expected_length);
}

static void persistent_config_qemu_install_data_region(void)
{
    static const unsigned char magic[8U] = { 'H', 'F', 'O', 'C',
                                              'D', 'A', 'T', 'A' };

    persistent_config_qemu_zero_bytes(
        persistent_config_qemu_storage,
        sizeof(persistent_config_qemu_storage));
    persistent_config_qemu_copy_bytes(
        persistent_config_qemu_storage, magic, sizeof(magic));
    persistent_config_qemu_storage[8U] = 1U;
    persistent_config_qemu_store_le64(
        persistent_config_qemu_storage + 16U,
        APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_OFFSET);
    persistent_config_qemu_store_le64(
        persistent_config_qemu_storage + 24U,
        APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_COUNT);
}

static unsigned char *persistent_config_reboot_region(uint64_t hhdm_offset)
{
    return (unsigned char *)(uintptr_t)(
        PERSISTENT_CONFIG_REBOOT_PHYS_ADDRESS + hhdm_offset);
}

static void persistent_config_reboot_failure(const char *message)
    __attribute__((noreturn));

static void persistent_config_reboot_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[REBOOT-CONFIG] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void persistent_config_reboot_require(bool condition,
                                             const char *message)
{
    if (!condition) {
        persistent_config_reboot_failure(message);
    }
}

static enum block_device_status persistent_config_reboot_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    const unsigned char *base = device->context;

    persistent_config_qemu_copy_bytes(
        buffer, base + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status persistent_config_reboot_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    unsigned char *base = device->context;

    persistent_config_qemu_copy_bytes(
        base + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void persistent_config_reboot_install_data_region(
    unsigned char *device_area)
{
    static const unsigned char magic[8U] = {
        'H', 'F', 'O', 'C', 'D', 'A', 'T', 'A'
    };

    persistent_config_qemu_zero_bytes(
        device_area, APPLIANCE_PERSISTENT_CONFIG_QEMU_CAPACITY);
    persistent_config_qemu_copy_bytes(
        device_area, magic, sizeof(magic));
    device_area[8U] = 1U;
    persistent_config_qemu_store_le64(
        device_area + 16U,
        APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_OFFSET);
    persistent_config_qemu_store_le64(
        device_area + 24U,
        APPLIANCE_PERSISTENT_CONFIG_QEMU_REGION_COUNT);
}

static bool persistent_config_reboot_run(uint64_t hhdm_offset)
{
    static const char invalid_record[] =
        "v1:system.hostname=bad_name\n";
    static const char reboot_hostname[] = "appliance-335";
    unsigned char *region = persistent_config_reboot_region(hhdm_offset);
    unsigned char *device_area =
        region + PERSISTENT_CONFIG_REBOOT_METADATA_BYTES;
    unsigned char state;
    struct block_device device;
    struct appliance_persistent_config config;
    enum appliance_persistent_config_status load_status;

    if (!persistent_config_qemu_equal_bytes(
            region, persistent_config_reboot_magic,
            sizeof(persistent_config_reboot_magic))) {
        return false;
    }

    state = region[PERSISTENT_CONFIG_REBOOT_STATE_OFFSET];
    if (state != PERSISTENT_CONFIG_REBOOT_STATE_FRESH &&
        state != PERSISTENT_CONFIG_REBOOT_STATE_COMMITTED &&
        state != PERSISTENT_CONFIG_REBOOT_STATE_INVALID) {
        persistent_config_reboot_failure("unknown reboot storage state");
    }

    persistent_config_qemu_zero_bytes(&device, sizeof(device));
    persistent_config_qemu_zero_bytes(&config, sizeof(config));
    device.block_size = APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_SIZE;
    device.block_count = APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_COUNT;
    device.context = device_area;
    device.read_fn = persistent_config_reboot_read;
    device.write_fn = persistent_config_reboot_write;

    if (state == PERSISTENT_CONFIG_REBOOT_STATE_FRESH) {
        persistent_config_reboot_install_data_region(device_area);
    }

    load_status = appliance_persistent_config_load(&config, &device);
    persistent_config_reboot_require(
        load_status == APPLIANCE_PERSISTENT_CONFIG_OK,
        "reboot persistent config load");

    if (state == PERSISTENT_CONFIG_REBOOT_STATE_FRESH) {
        persistent_config_reboot_require(
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
            persistent_config_qemu_hostname_is(
                &config,
                APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
            "empty reboot storage publishes safe default");
        persistent_config_reboot_require(
            appliance_persistent_config_set_hostname(
                &config, reboot_hostname,
                sizeof(reboot_hostname) - 1U) ==
                APPLIANCE_PERSISTENT_CONFIG_OK &&
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
            persistent_config_qemu_hostname_is(
                &config, reboot_hostname),
            "reboot hostname commit");
        region[PERSISTENT_CONFIG_REBOOT_STATE_OFFSET] =
            PERSISTENT_CONFIG_REBOOT_STATE_COMMITTED;
        serial_write("[REBOOT-CONFIG] phase1 committed\n");
        return true;
    }

    if (state == PERSISTENT_CONFIG_REBOOT_STATE_COMMITTED) {
        persistent_config_reboot_require(
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
            persistent_config_qemu_hostname_is(
                &config, reboot_hostname),
            "committed reboot hostname reload");
        persistent_config_reboot_require(
            config_journal_append(
                &config.journal, invalid_record,
                sizeof(invalid_record) - 1U) == CONFIG_JOURNAL_OK,
            "reboot invalid record append");
        region[PERSISTENT_CONFIG_REBOOT_STATE_OFFSET] =
            PERSISTENT_CONFIG_REBOOT_STATE_INVALID;
        serial_write(
            "[REBOOT-CONFIG] phase2 invalid committed\n");
        return true;
    }

    persistent_config_reboot_require(
        appliance_persistent_config_source(&config) ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
        persistent_config_qemu_hostname_is(
            &config,
            APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "reboot invalid record recovery");
    serial_write("[REBOOT-CONFIG] phase3 recovery OK\n");
    return true;
}

void appliance_persistent_config_qemu_test_init(void)
{
    struct block_device device;
    struct appliance_persistent_config config;
    char too_long[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 2U];
    static const char invalid_record[] =
        "v1:system.hostname=bad_name\n";
    size_t write_calls;

    if (persistent_config_reboot_run(vmm_hhdm_offset())) {
        return;
    }
    persistent_config_qemu_zero_bytes(&device, sizeof(device));
    persistent_config_qemu_zero_bytes(&config, sizeof(config));
    persistent_config_qemu_zero_bytes(
        too_long, sizeof(too_long));
    persistent_config_qemu_install_data_region();

    device.block_size = APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_SIZE;
    device.block_count = APPLIANCE_PERSISTENT_CONFIG_QEMU_BLOCK_COUNT;
    device.read_fn = persistent_config_qemu_read;
    device.write_fn = persistent_config_qemu_write;

    serial_write("[PERSISTENT-CONFIG] proof init\n");

    persistent_config_qemu_require(
        appliance_persistent_config_load(
            &config, &device) == APPLIANCE_PERSISTENT_CONFIG_OK &&
            persistent_config_qemu_hostname_is(
                &config,
                APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "empty data region publishes default hostname");
    serial_write(
        "[PERSISTENT-CONFIG] default hostname loaded\n");

    persistent_config_qemu_require(
        appliance_persistent_config_set_hostname(
            &config, "appliance-303", 13U) ==
            APPLIANCE_PERSISTENT_CONFIG_OK &&
            persistent_config_qemu_hostname_is(
                &config, "appliance-303"),
        "hostname commit");
    serial_write("[PERSISTENT-CONFIG] hostname committed\n");

    persistent_config_qemu_zero_bytes(&config, sizeof(config));
    persistent_config_qemu_require(
        appliance_persistent_config_load(
            &config, &device) == APPLIANCE_PERSISTENT_CONFIG_OK &&
            persistent_config_qemu_hostname_is(
                &config, "appliance-303"),
        "fresh load after commit");
    serial_write(
        "[PERSISTENT-CONFIG] fresh config read persisted hostname\n");

    write_calls = persistent_config_qemu_write_calls;
    persistent_config_qemu_require(
        appliance_persistent_config_set_hostname(
            &config, "bad_name", 8U) ==
            APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME &&
            persistent_config_qemu_write_calls == write_calls &&
            persistent_config_qemu_hostname_is(
                &config, "appliance-303"),
        "invalid hostname rejected without journal write");
    persistent_config_qemu_fill_bytes(
        (unsigned char *)too_long, (unsigned char)'a', sizeof(too_long));
    persistent_config_qemu_require(
        appliance_persistent_config_set_hostname(
            &config, too_long, sizeof(too_long)) ==
            APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME &&
            persistent_config_qemu_write_calls == write_calls &&
            persistent_config_qemu_hostname_is(
                &config, "appliance-303"),
        "over-limit hostname rejected without journal write");
    serial_write(
        "[PERSISTENT-CONFIG] invalid hostname rejected\n");

    write_calls = persistent_config_qemu_write_calls;
    persistent_config_qemu_require(
        config_journal_append(
            &config.journal, invalid_record,
            sizeof(invalid_record) - 1U) == CONFIG_JOURNAL_OK &&
            persistent_config_qemu_write_calls == write_calls + 1U,
        "malformed journal record append");

    persistent_config_qemu_zero_bytes(&config, sizeof(config));
    persistent_config_qemu_require(
        appliance_persistent_config_load(
            &config, &device) == APPLIANCE_PERSISTENT_CONFIG_OK &&
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_INVALID &&
            persistent_config_qemu_hostname_is(
                &config,
                APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "schema-invalid record recovers to safe default");
    serial_write(
        "[PERSISTENT-CONFIG] invalid record recovered to default\n");

    write_calls = persistent_config_qemu_write_calls;
    persistent_config_qemu_require(
        appliance_persistent_config_set_hostname(
            &config,
            APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME,
            sizeof(APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME) - 1U) ==
            APPLIANCE_PERSISTENT_CONFIG_OK &&
            persistent_config_qemu_write_calls == write_calls + 1U &&
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
            persistent_config_qemu_hostname_is(
                &config,
                APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "post-recovery default write");

    persistent_config_qemu_zero_bytes(&config, sizeof(config));
    persistent_config_qemu_require(
        appliance_persistent_config_load(
            &config, &device) == APPLIANCE_PERSISTENT_CONFIG_OK &&
            appliance_persistent_config_source(&config) ==
                APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
            persistent_config_qemu_hostname_is(
                &config,
                APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "fresh load after recovery write");
    serial_write(
        "[PERSISTENT-CONFIG] post-recovery persistence proven\n");

    serial_write("[PERSISTENT-CONFIG] clean shutdown\n");
    serial_write("[PERSISTENT-CONFIG] proof OK\n");
}

#else
typedef int appliance_persistent_config_qemu_translation_unit_guard;
#endif
