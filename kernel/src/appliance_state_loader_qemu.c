#ifdef HFOC_PHYSICAL_STORAGE_QEMU_TEST

#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "data_region.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define STATE_LOADER_QEMU_BLOCK_SIZE 512U
#define STATE_LOADER_QEMU_BLOCK_COUNT 1024U
#define STATE_LOADER_QEMU_CAPACITY \
    (STATE_LOADER_QEMU_BLOCK_SIZE * STATE_LOADER_QEMU_BLOCK_COUNT)
#define STATE_LOADER_QEMU_HFOC_FIRST_LBA 34U
#define STATE_LOADER_QEMU_HFOC_BLOCK_COUNT 512U
#define STATE_LOADER_QEMU_STORE_BLOCKS 64U
#define STATE_LOADER_QEMU_STORE_CAPACITY \
    (STATE_LOADER_QEMU_BLOCK_SIZE * STATE_LOADER_QEMU_STORE_BLOCKS)
#define STATE_LOADER_QEMU_JOURNAL_BLOCK_COUNT 16U
#define STATE_LOADER_QEMU_GPT_ENTRY_COUNT 4U
#define STATE_LOADER_QEMU_GPT_HEADER_SIZE 92U
#define STATE_LOADER_QEMU_GPT_ENTRY_SIZE 128U

static unsigned char state_loader_qemu_disk[STATE_LOADER_QEMU_CAPACITY];
static bool state_loader_qemu_fail_read;
static size_t state_loader_qemu_flush_calls;

static const unsigned char state_loader_qemu_hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static void state_loader_qemu_failure(const char *message)
    __attribute__((noreturn));

static void state_loader_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[STATE-LOADER] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void state_loader_qemu_require(bool condition,
                                      const char *message)
{
    if (!condition) {
        state_loader_qemu_failure(message);
    }
}

static void state_loader_qemu_store_le32(unsigned char *bytes,
                                         uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void state_loader_qemu_store_le64(unsigned char *bytes,
                                         uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t state_loader_qemu_crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        unsigned int bit;

        crc ^= bytes[index];
        for (bit = 0U; bit < 8U; ++bit) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc;
}

static uint32_t state_loader_qemu_crc32(
    const unsigned char *bytes,
    size_t length)
{
    return state_loader_qemu_crc32_update(
        0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static void state_loader_qemu_zero_disk(void)
{
    memset(state_loader_qemu_disk, 0,
           sizeof(state_loader_qemu_disk));
    state_loader_qemu_fail_read = false;
    state_loader_qemu_flush_calls = 0U;
}

static void state_loader_qemu_set_gpt_header(void)
{
    unsigned char *header =
        state_loader_qemu_disk + STATE_LOADER_QEMU_BLOCK_SIZE;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char copy[STATE_LOADER_QEMU_BLOCK_SIZE];

    memcpy(header, signature, sizeof(signature));
    state_loader_qemu_store_le32(
        header + 8U, UINT32_C(0x00010000));
    state_loader_qemu_store_le32(
        header + 12U, STATE_LOADER_QEMU_GPT_HEADER_SIZE);
    state_loader_qemu_store_le64(header + 24U, 1U);
    state_loader_qemu_store_le64(
        header + 32U, STATE_LOADER_QEMU_BLOCK_COUNT - 1U);
    state_loader_qemu_store_le64(
        header + 40U, STATE_LOADER_QEMU_HFOC_FIRST_LBA);
    state_loader_qemu_store_le64(
        header + 48U, STATE_LOADER_QEMU_BLOCK_COUNT - 1U);
    state_loader_qemu_store_le64(header + 72U, 2U);
    state_loader_qemu_store_le32(
        header + 80U, STATE_LOADER_QEMU_GPT_ENTRY_COUNT);
    state_loader_qemu_store_le32(
        header + 84U, STATE_LOADER_QEMU_GPT_ENTRY_SIZE);
    state_loader_qemu_store_le32(header + 88U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, STATE_LOADER_QEMU_GPT_HEADER_SIZE);
    state_loader_qemu_store_le32(
        header + 88U,
        state_loader_qemu_crc32(
            copy, STATE_LOADER_QEMU_GPT_HEADER_SIZE));
    state_loader_qemu_store_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, STATE_LOADER_QEMU_GPT_HEADER_SIZE);
    memset(copy + 16U, 0, 4U);
    state_loader_qemu_store_le32(
        header + 16U,
        state_loader_qemu_crc32(
            copy, STATE_LOADER_QEMU_GPT_HEADER_SIZE));
}

static void state_loader_qemu_set_entry(uint32_t index,
                                        uint64_t first_lba,
                                        uint64_t last_lba)
{
    unsigned char *entry =
        state_loader_qemu_disk +
        2U * STATE_LOADER_QEMU_BLOCK_SIZE +
        (uint64_t)index * STATE_LOADER_QEMU_GPT_ENTRY_SIZE;

    if (index == 1U) {
        memcpy(entry, state_loader_qemu_hfoc_guid,
               sizeof(state_loader_qemu_hfoc_guid));
    }
    state_loader_qemu_store_le64(entry + 16U, index + 1U);
    state_loader_qemu_store_le64(entry + 32U, first_lba);
    state_loader_qemu_store_le64(entry + 40U, last_lba);
}

static void state_loader_qemu_finalize_gpt_entries(void)
{
    unsigned char *header =
        state_loader_qemu_disk + STATE_LOADER_QEMU_BLOCK_SIZE;
    unsigned char copy[STATE_LOADER_QEMU_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t entry_bytes =
        STATE_LOADER_QEMU_GPT_ENTRY_COUNT *
        STATE_LOADER_QEMU_GPT_ENTRY_SIZE;
    size_t index;

    for (index = 0U; index < entry_bytes; ++index) {
        crc = state_loader_qemu_crc32_update(
            crc,
            state_loader_qemu_disk +
                2U * STATE_LOADER_QEMU_BLOCK_SIZE + index,
            1U);
    }
    state_loader_qemu_store_le32(
        header + 88U, crc ^ 0xFFFFFFFFU);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, STATE_LOADER_QEMU_GPT_HEADER_SIZE);
    memset(copy + 16U, 0, 4U);
    state_loader_qemu_store_le32(
        header + 16U,
        state_loader_qemu_crc32(
            copy, STATE_LOADER_QEMU_GPT_HEADER_SIZE));
}

static void state_loader_qemu_install_gpt(void)
{
    state_loader_qemu_zero_disk();
    state_loader_qemu_set_gpt_header();
    state_loader_qemu_set_entry(
        1U,
        STATE_LOADER_QEMU_HFOC_FIRST_LBA,
        STATE_LOADER_QEMU_HFOC_FIRST_LBA +
            STATE_LOADER_QEMU_HFOC_BLOCK_COUNT - 1U);
    state_loader_qemu_finalize_gpt_entries();
}

struct state_loader_qemu_store_fixture {
    unsigned char storage[STATE_LOADER_QEMU_STORE_CAPACITY];
};

static enum block_device_status state_loader_qemu_store_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct state_loader_qemu_store_fixture *store =
        (struct state_loader_qemu_store_fixture *)device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, store->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status state_loader_qemu_store_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct state_loader_qemu_store_fixture *store =
        (struct state_loader_qemu_store_fixture *)device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(store->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device state_loader_qemu_store_device(
    struct state_loader_qemu_store_fixture *store)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = STATE_LOADER_QEMU_BLOCK_SIZE;
    device.block_count = STATE_LOADER_QEMU_STORE_BLOCKS;
    device.context = store;
    device.read_fn = state_loader_qemu_store_read;
    device.write_fn = state_loader_qemu_store_write;
    return device;
}

static void state_loader_qemu_install_store_header(
    struct state_loader_qemu_store_fixture *store)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memset(store, 0, sizeof(*store));
    memcpy(store->storage, magic, sizeof(magic));
    store->storage[8U] = DATA_REGION_FORMAT_VERSION;
    state_loader_qemu_store_le64(store->storage + 16U, 1U);
    state_loader_qemu_store_le64(
        store->storage + 24U, STATE_LOADER_QEMU_JOURNAL_BLOCK_COUNT);
}

static void state_loader_qemu_build_hostname_store(
    struct state_loader_qemu_store_fixture *store)
{
    struct block_device device = state_loader_qemu_store_device(store);
    struct appliance_persistent_config config;

    state_loader_qemu_install_store_header(store);
    memset(&config, 0, sizeof(config));
    state_loader_qemu_require(
        appliance_persistent_config_load(&config, &device) ==
            APPLIANCE_PERSISTENT_CONFIG_OK,
        "committed hostname store initializes");
    state_loader_qemu_require(
        appliance_persistent_config_set_hostname(
            &config, "hfoc-prod", 9U) ==
            APPLIANCE_PERSISTENT_CONFIG_OK,
        "committed hostname store writes journal");
}

static void state_loader_qemu_build_network_store(
    struct state_loader_qemu_store_fixture *store)
{
    struct block_device device = state_loader_qemu_store_device(store);
    struct appliance_persistent_network_config config;

    state_loader_qemu_install_store_header(store);
    memset(&config, 0, sizeof(config));
    state_loader_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
        "committed network store initializes");
    state_loader_qemu_require(
        appliance_persistent_network_config_set_dhcp(
            &config, "eth0", 4U) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
        "committed network store writes journal");
}

static void state_loader_qemu_copy_store(
    uint64_t partition_block_offset,
    const struct state_loader_qemu_store_fixture *store)
{
    memcpy(
        state_loader_qemu_disk +
            (size_t)((STATE_LOADER_QEMU_HFOC_FIRST_LBA +
                      partition_block_offset) *
                     STATE_LOADER_QEMU_BLOCK_SIZE),
        store->storage,
        sizeof(store->storage));
}

static void state_loader_qemu_install_stores(bool committed)
{
    struct state_loader_qemu_store_fixture store;

    if (committed) {
        state_loader_qemu_build_hostname_store(&store);
        state_loader_qemu_copy_store(0U, &store);
        state_loader_qemu_build_network_store(&store);
        state_loader_qemu_copy_store(64U, &store);
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(128U, &store);
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(256U, &store);
    } else {
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(0U, &store);
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(64U, &store);
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(128U, &store);
        state_loader_qemu_install_store_header(&store);
        state_loader_qemu_copy_store(256U, &store);
    }
}

static enum block_device_status state_loader_qemu_disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (state_loader_qemu_fail_read ||
        offset > sizeof(state_loader_qemu_disk) ||
        length > sizeof(state_loader_qemu_disk) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, state_loader_qemu_disk + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status state_loader_qemu_disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(state_loader_qemu_disk) ||
        length > sizeof(state_loader_qemu_disk) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(state_loader_qemu_disk + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status state_loader_qemu_disk_flush(
    struct block_device *device)
{
    (void)device;
    ++state_loader_qemu_flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device state_loader_qemu_make_disk(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = STATE_LOADER_QEMU_BLOCK_SIZE;
    device.block_count = STATE_LOADER_QEMU_BLOCK_COUNT;
    device.read_fn = state_loader_qemu_disk_read;
    device.write_fn = state_loader_qemu_disk_write;
    device.flush_fn = state_loader_qemu_disk_flush;
    return device;
}

static bool state_loader_qemu_hostname_is(
    const struct appliance_state_loader *loader,
    const char *expected)
{
    char output[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    size_t length = 0U;
    size_t expected_length = strlen(expected);

    return appliance_persistent_config_hostname(
               &loader->system_config, output, sizeof(output), &length) ==
               APPLIANCE_PERSISTENT_CONFIG_OK &&
           length == expected_length &&
           memcmp(output, expected, expected_length) == 0;
}

static void state_loader_qemu_load_fresh(void)
{
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct block_device disk;

    state_loader_qemu_install_gpt();
    state_loader_qemu_install_stores(false);
    disk = state_loader_qemu_make_disk();
    memset(&layout, 0, sizeof(layout));
    memset(&loader, 0, sizeof(loader));
    state_loader_qemu_require(
        appliance_state_layout_default(&layout) ==
            APPLIANCE_STATE_LAYOUT_OK,
        "default state layout");
    state_loader_qemu_require(
        appliance_state_loader_init(&loader) ==
            APPLIANCE_STATE_LOADER_OK,
        "state loader init");
    state_loader_qemu_require(
        appliance_state_loader_load(&loader, &disk, &layout) ==
            APPLIANCE_STATE_LOADER_OK &&
            appliance_state_loader_ready(&loader),
        "fresh state loader ready");
    state_loader_qemu_require(
        loader.system_config.values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
            state_loader_qemu_hostname_is(
                &loader, APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
        "fresh hostname safe default");
    state_loader_qemu_require(
        loader.network_config.source ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING &&
            appliance_network_config_mode(
                &loader.network_config.values) ==
                APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
        "fresh network safe default");
    state_loader_qemu_require(
        loader.service_config.source ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING,
        "fresh service safe default");
    state_loader_qemu_require(
        appliance_state_block_device_flush(&loader.stores[0]) ==
            BLOCK_DEVICE_OK &&
            state_loader_qemu_flush_calls == 1U,
        "loader flush propagation");
    serial_write("[STATE-LOADER] fresh defaults and flush OK\n");
}

static void state_loader_qemu_load_committed(void)
{
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct block_device disk;

    state_loader_qemu_install_gpt();
    state_loader_qemu_install_stores(true);
    disk = state_loader_qemu_make_disk();
    memset(&layout, 0, sizeof(layout));
    memset(&loader, 0, sizeof(loader));
    state_loader_qemu_require(
        appliance_state_layout_default(&layout) ==
            APPLIANCE_STATE_LAYOUT_OK,
        "committed state layout");
    state_loader_qemu_require(
        appliance_state_loader_init(&loader) ==
            APPLIANCE_STATE_LOADER_OK,
        "committed state loader init");
    state_loader_qemu_require(
        appliance_state_loader_load(&loader, &disk, &layout) ==
            APPLIANCE_STATE_LOADER_OK &&
            appliance_state_loader_ready(&loader),
        "committed state loader ready");
    state_loader_qemu_require(
        loader.system_config.values.source ==
            APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
            state_loader_qemu_hostname_is(&loader, "hfoc-prod"),
        "committed hostname replay");
    state_loader_qemu_require(
        loader.network_config.source ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED,
        "committed network replay");
    serial_write("[STATE-LOADER] committed journal replay OK\n");
}

static void state_loader_qemu_load_corrupt(void)
{
    struct appliance_state_layout layout;
    struct appliance_state_loader loader;
    struct block_device disk;

    state_loader_qemu_install_gpt();
    state_loader_qemu_install_stores(false);
    state_loader_qemu_disk[
        STATE_LOADER_QEMU_HFOC_FIRST_LBA *
        STATE_LOADER_QEMU_BLOCK_SIZE] = (unsigned char)'X';
    disk = state_loader_qemu_make_disk();
    memset(&layout, 0, sizeof(layout));
    memset(&loader, 0, sizeof(loader));
    state_loader_qemu_require(
        appliance_state_layout_default(&layout) ==
            APPLIANCE_STATE_LAYOUT_OK,
        "corrupt state layout");
    state_loader_qemu_require(
        appliance_state_loader_init(&loader) ==
            APPLIANCE_STATE_LOADER_OK,
        "corrupt state loader init");
    state_loader_qemu_require(
        appliance_state_loader_load(&loader, &disk, &layout) !=
            APPLIANCE_STATE_LOADER_OK &&
            !appliance_state_loader_ready(&loader),
        "corrupt store fails closed");
    serial_write("[STATE-LOADER] corruption fails closed\n");
}

void appliance_state_loader_qemu_test_init(void)
{
    serial_write("[STATE-LOADER] proof init\n");
    state_loader_qemu_load_fresh();
    state_loader_qemu_load_committed();
    state_loader_qemu_load_corrupt();
    serial_write("[STATE-LOADER] clean shutdown\n");
    serial_write("[STATE-LOADER] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_appliance_state_loader_qemu_translation_unit_marker;
#endif
