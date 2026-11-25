#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_state_layout.h"
#include "appliance_state_loader.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_STATE_LOADER_HOST_TEST

#define DISK_BLOCK_SIZE 512U
#define DISK_BLOCK_COUNT 8192U
#define DISK_CAPACITY (DISK_BLOCK_SIZE * DISK_BLOCK_COUNT)
#define HFOC_FIRST_LBA UINT64_C(2048)
#define HFOC_BLOCK_COUNT UINT64_C(512)
#define STORE_BLOCK_COUNT 64U
#define STORE_CAPACITY (DISK_BLOCK_SIZE * STORE_BLOCK_COUNT)
#define STORE_DATA_REGION_BLOCKS 16U

static int failures;
static unsigned char disk_storage[DISK_CAPACITY];
static bool disk_fail_read;
static size_t disk_flush_calls;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static const unsigned char hfoc_guid[16U] = {
    0xE1U, 0x44U, 0x01U, 0x4CU, 0x0DU, 0x6EU, 0x7AU, 0x4BU,
    0xB4U, 0x9DU, 0x22U, 0x10U, 0x33U, 0x3DU, 0x41U, 0x31U
};

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static uint32_t crc32_update(
    uint32_t crc,
    const unsigned char *bytes,
    size_t length)
{
    size_t byte_index;

    for (byte_index = 0U; byte_index < length; ++byte_index) {
        unsigned int bit;

        crc ^= bytes[byte_index];
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

static uint32_t crc32(const unsigned char *bytes, size_t length)
{
    return crc32_update(0xFFFFFFFFU, bytes, length) ^ 0xFFFFFFFFU;
}

static void disk_init(void)
{
    memset(disk_storage, 0, sizeof(disk_storage));
    disk_fail_read = false;
    disk_flush_calls = 0U;
}

static enum block_device_status disk_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (disk_fail_read ||
        offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, disk_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(disk_storage) ||
        length > sizeof(disk_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(disk_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status disk_flush(
    struct block_device *device)
{
    (void)device;
    ++disk_flush_calls;
    return BLOCK_DEVICE_OK;
}

static struct block_device disk_device(void)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = DISK_BLOCK_SIZE;
    device.block_count = DISK_BLOCK_COUNT;
    device.read_fn = disk_read;
    device.write_fn = disk_write;
    device.flush_fn = disk_flush;
    return device;
}

static void set_gpt_header(void)
{
    unsigned char *header = disk_storage + DISK_BLOCK_SIZE;
    static const unsigned char signature[8U] = {
        'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'
    };
    unsigned char copy[DISK_BLOCK_SIZE];

    memset(disk_storage, 0, sizeof(disk_storage));
    memcpy(header, signature, sizeof(signature));
    store_le32(header + 8U, UINT32_C(0x00010000));
    store_le32(header + 12U, 92U);
    store_le64(header + 24U, 1U);
    store_le64(header + 32U, DISK_BLOCK_COUNT - 1U);
    store_le64(header + 40U, 34U);
    store_le64(header + 48U, DISK_BLOCK_COUNT - 1U);
    store_le64(header + 72U, 2U);
    store_le32(header + 80U, 4U);
    store_le32(header + 84U, 128U);
    store_le32(header + 88U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    store_le32(header + 88U, crc32(copy, 92U));
    store_le32(header + 16U, 0U);
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void set_entry(
    uint32_t index,
    const unsigned char type_guid[16U],
    uint64_t first_lba,
    uint64_t last_lba)
{
    unsigned char *entry =
        disk_storage + 2U * DISK_BLOCK_SIZE +
        (uint64_t)index * 128U;

    if (type_guid != NULL) {
        memcpy(entry, type_guid, 16U);
    }
    store_le64(entry + 16U, index + 1U);
    store_le64(entry + 32U, first_lba);
    store_le64(entry + 40U, last_lba);
}

static void finalize_entries(void)
{
    unsigned char *header = disk_storage + DISK_BLOCK_SIZE;
    unsigned char copy[DISK_BLOCK_SIZE];
    uint32_t crc = 0xFFFFFFFFU;
    size_t index;

    for (index = 0U; index < 4U * 128U; ++index) {
        crc = crc32_update(
            crc,
            disk_storage + 2U * DISK_BLOCK_SIZE + index,
            1U);
    }
    store_le32(header + 88U, crc ^ 0xFFFFFFFFU);
    store_le32(header + 16U, 0U);
    memset(copy, 0, sizeof(copy));
    memcpy(copy, header, 92U);
    memset(copy + 16U, 0, 4U);
    store_le32(header + 16U, crc32(copy, 92U));
}

static void install_gpt(void)
{
    disk_init();
    set_gpt_header();
    set_entry(
        0U, NULL, 34U,
        HFOC_FIRST_LBA - 1U);
    set_entry(
        1U, hfoc_guid,
        HFOC_FIRST_LBA,
        HFOC_FIRST_LBA + HFOC_BLOCK_COUNT - 1U);
    finalize_entries();
}

struct store_fixture {
    unsigned char storage[STORE_CAPACITY];
};

static enum block_device_status store_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct store_fixture *store =
        (struct store_fixture *)device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(buffer, store->storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status store_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct store_fixture *store =
        (struct store_fixture *)device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    memcpy(store->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static struct block_device store_device(struct store_fixture *store)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = DISK_BLOCK_SIZE;
    device.block_count = STORE_BLOCK_COUNT;
    device.context = store;
    device.read_fn = store_read;
    device.write_fn = store_write;
    return device;
}

static void install_store_header(struct store_fixture *store)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    memset(store, 0, sizeof(*store));
    memcpy(store->storage, magic, sizeof(magic));
    store->storage[8U] = DATA_REGION_FORMAT_VERSION;
    store_le64(store->storage + 16U, 1U);
    store_le64(store->storage + 24U, STORE_DATA_REGION_BLOCKS);
}

static void build_hostname_store(struct store_fixture *store)
{
    struct appliance_persistent_config config;
    struct block_device device = store_device(store);

    install_store_header(store);
    memset(&config, 0, sizeof(config));
    CHECK(appliance_persistent_config_load(&config, &device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "hostname store initializes for committed fixture");
    CHECK(appliance_persistent_config_set_hostname(
              &config, "hfoc-prod", 9U) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "hostname store commits production hostname");
}

static void build_network_store(struct store_fixture *store)
{
    struct appliance_persistent_network_config config;
    struct block_device device = store_device(store);

    install_store_header(store);
    memset(&config, 0, sizeof(config));
    CHECK(appliance_persistent_network_config_load(
              &config, &device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "network store initializes for committed fixture");
    CHECK(appliance_persistent_network_config_set_dhcp(
              &config, "eth0", 4U) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "network store commits DHCP profile");
}

static void copy_store_to_disk(uint32_t store_block_offset,
                               const struct store_fixture *store)
{
    memcpy(
        disk_storage +
            (size_t)((HFOC_FIRST_LBA + store_block_offset) *
                     DISK_BLOCK_SIZE),
        store->storage,
        sizeof(store->storage));
}

static void install_fresh_partition(void)
{
    struct store_fixture store;

    install_gpt();
    install_store_header(&store);
    copy_store_to_disk(0U, &store);
    install_store_header(&store);
    copy_store_to_disk(64U, &store);
    install_store_header(&store);
    copy_store_to_disk(128U, &store);
    install_store_header(&store);
    copy_store_to_disk(256U, &store);
}

static void install_committed_partition(void)
{
    struct store_fixture store;

    install_gpt();
    build_hostname_store(&store);
    copy_store_to_disk(0U, &store);
    build_network_store(&store);
    copy_store_to_disk(64U, &store);
    install_store_header(&store);
    copy_store_to_disk(128U, &store);
    install_store_header(&store);
    copy_store_to_disk(256U, &store);
}

static bool hostname_is(const struct appliance_state_loader *loader,
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

static void test_fresh_safe_defaults(void)
{
    struct appliance_state_loader loader;
    struct block_device disk;
    struct appliance_state_layout layout;

    install_fresh_partition();
    disk = disk_device();
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "loader fixture has canonical layout");
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "loader init succeeds");
    CHECK(appliance_state_loader_load(&loader, &disk, &layout) ==
              APPLIANCE_STATE_LOADER_OK &&
              appliance_state_loader_ready(&loader),
          "fresh production partition publishes loader ready");
    CHECK(loader.store_states[0].loaded &&
              loader.store_states[1].loaded &&
              loader.store_states[2].loaded &&
              loader.store_states[3].loaded,
          "all current stores report loaded");
    CHECK(loader.system_config.values.source ==
              APPLIANCE_CONFIG_VALIDATION_SOURCE_DEFAULT_MISSING &&
              hostname_is(&loader, APPLIANCE_PERSISTENT_CONFIG_DEFAULT_HOSTNAME),
          "fresh hostname store publishes safe default hostname");
    CHECK(loader.network_config.source ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_DEFAULT_MISSING &&
              appliance_network_config_mode(
                  &loader.network_config.values) ==
                  APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
          "fresh network store publishes safe DHCP default");
    CHECK(loader.service_config.source ==
              APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_DEFAULT_MISSING &&
              loader.service_config.descriptor_count == 0U,
          "fresh service store publishes empty safe baseline");
    CHECK(appliance_state_block_device_flush(&loader.stores[0]) ==
              BLOCK_DEVICE_OK &&
              disk_flush_calls == 1U,
          "loader subdevice propagates flush to physical disk");
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_BAD_STATE,
          "ready loader cannot be reinitialized");
}

static void test_committed_load(void)
{
    struct appliance_state_loader loader;
    struct block_device disk;
    struct appliance_state_layout layout;

    install_committed_partition();
    disk = disk_device();
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "committed fixture layout is canonical");
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "committed fixture loader initializes");
    CHECK(appliance_state_loader_load(&loader, &disk, &layout) ==
              APPLIANCE_STATE_LOADER_OK &&
              appliance_state_loader_ready(&loader),
          "committed production partition publishes loader ready");
    CHECK(loader.system_config.values.source ==
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED &&
              hostname_is(&loader, "hfoc-prod"),
          "committed hostname survives loader");
    CHECK(loader.network_config.source ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED &&
              appliance_network_config_mode(
                  &loader.network_config.values) ==
                  APPLIANCE_NETWORK_CONFIG_MODE_DHCP,
          "committed network profile survives loader");
}

static void test_corruption_and_ambiguity(void)
{
    struct appliance_state_loader loader;
    struct block_device disk;
    struct appliance_state_layout layout;

    install_fresh_partition();
    disk_storage[(size_t)(HFOC_FIRST_LBA * DISK_BLOCK_SIZE)] = 'X';
    disk = disk_device();
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK,
          "corrupt fixture layout is canonical");
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "corrupt fixture loader initializes");
    CHECK(appliance_state_loader_load(&loader, &disk, &layout) ==
              APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED &&
              !appliance_state_loader_ready(&loader),
          "corrupt store fails closed and never publishes ready");

    disk_init();
    set_gpt_header();
    set_entry(0U, hfoc_guid, 34U, 1000U);
    set_entry(1U, hfoc_guid, 2048U, 2559U);
    finalize_entries();
    disk = disk_device();
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "ambiguous fixture loader initializes");
    CHECK(appliance_state_loader_load(&loader, &disk, &layout) ==
              APPLIANCE_STATE_LOADER_PARTITION_AMBIGUOUS &&
              !appliance_state_loader_ready(&loader),
          "multiple HFOC partitions fail closed as ambiguous");

    disk_init();
    set_gpt_header();
    finalize_entries();
    disk = disk_device();
    memset(&loader, 0, sizeof(loader));
    CHECK(appliance_state_loader_init(&loader) ==
              APPLIANCE_STATE_LOADER_OK,
          "missing partition fixture loader initializes");
    CHECK(appliance_state_loader_load(&loader, &disk, &layout) ==
              APPLIANCE_STATE_LOADER_PARTITION_NOT_FOUND &&
              !appliance_state_loader_ready(&loader),
          "missing HFOC partition fails closed");

    CHECK(appliance_state_loader_ready(NULL) == false,
          "ready rejects null loader");
    CHECK(appliance_state_loader_system_config(NULL) == NULL &&
              appliance_state_loader_network_config(NULL) == NULL &&
              appliance_state_loader_service_config(NULL) == NULL &&
              appliance_state_loader_partition_device(NULL) == NULL &&
              appliance_state_loader_layout(NULL) == NULL,
          "loader accessors reject null state");
    CHECK(appliance_state_loader_status_string(
              APPLIANCE_STATE_LOADER_OK) != NULL &&
              strcmp(appliance_state_loader_status_string(
                         APPLIANCE_STATE_LOADER_OK),
                     "APPLIANCE_STATE_LOADER_OK") == 0,
          "loader status string is exact");
    CHECK(appliance_state_loader_status_string(
              (enum appliance_state_loader_status)99U) == NULL,
          "loader rejects undefined status string");
}

int main(void)
{
    test_fresh_safe_defaults();
    test_committed_load();
    test_corruption_and_ambiguity();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance state loader host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance state loader host tests: ALL OK");
    return 0;
}

#endif
