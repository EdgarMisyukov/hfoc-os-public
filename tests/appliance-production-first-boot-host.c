#include "appliance_production_first_boot.h"

#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_state_loader.h"
#include "block_device.h"
#include "data_region.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_PRODUCTION_FIRST_BOOT_HOST_TEST

#define TEST_BLOCK_SIZE 512U
#define TEST_STORE_BLOCKS 32U
#define TEST_DATA_REGION_BLOCKS 16U
#define TEST_STORE_CAPACITY (TEST_BLOCK_SIZE * TEST_STORE_BLOCKS)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct store_fixture {
    unsigned char storage[TEST_STORE_CAPACITY];
};

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static enum block_device_status store_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct store_fixture *store = device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
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
    struct store_fixture *store = device->context;

    if (offset > sizeof(store->storage) ||
        length > sizeof(store->storage) - (size_t)offset) {
        return BLOCK_DEVICE_OUT_OF_RANGE;
    }
    memcpy(store->storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status store_flush(struct block_device *device)
{
    (void)device;
    return BLOCK_DEVICE_OK;
}

static struct block_device store_device(struct store_fixture *store)
{
    struct block_device device;

    memset(&device, 0, sizeof(device));
    device.block_size = TEST_BLOCK_SIZE;
    device.block_count = TEST_STORE_BLOCKS;
    device.context = store;
    device.read_fn = store_read;
    device.write_fn = store_write;
    device.flush_fn = store_flush;
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
    store_le64(store->storage + 24U, TEST_DATA_REGION_BLOCKS);
}

static struct network_device_snapshot make_device(void)
{
    struct network_device_snapshot device;

    memset(&device, 0, sizeof(device));
    device.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device.device_id = 1U;
    device.vendor_id = 0x1AF4U;
    device.product_id = 0x1041U;
    device.bus_address = 0x0100U;
    device.mac_address[0] = 0x52U;
    device.mac_address[1] = 0x54U;
    device.mac_address[2] = 0x00U;
    device.mac_address[3] = 0x12U;
    device.mac_address[4] = 0x34U;
    device.mac_address[5] = 0x56U;
    device.capability_mask =
        NETWORK_DEVICE_CAPABILITY_RX |
        NETWORK_DEVICE_CAPABILITY_TX |
        NETWORK_DEVICE_CAPABILITY_LINK_STATE;
    return device;
}

static struct network_ipv4_config make_runtime(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static void make_credential(
    uint8_t credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH])
{
    size_t index;

    for (index = 0U; index < APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH;
         ++index) {
        credential[index] = (uint8_t)(0x11U + index);
    }
}

static void load_loader(
    struct appliance_state_loader *loader,
    struct store_fixture *system_store,
    struct store_fixture *network_store,
    struct store_fixture *secret_store)
{
    static struct block_device system_device;
    static struct block_device network_device;
    static struct block_device secret_device;

    system_device = store_device(system_store);
    network_device = store_device(network_store);
    secret_device = store_device(secret_store);

    memset(loader, 0, sizeof(*loader));
    loader->initialized = true;
    loader->ready = true;
    CHECK(appliance_persistent_config_load(
              &loader->system_config, &system_device) ==
              APPLIANCE_PERSISTENT_CONFIG_OK,
          "first boot fixture loads system store");
    CHECK(appliance_persistent_network_config_load(
              &loader->network_config, &network_device) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
          "first boot fixture loads network store");
    CHECK(appliance_persistent_secret_config_load(
              &loader->secret_config, &secret_device) ==
              APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
          "first boot fixture loads secret store");
}

static void test_first_boot_provision(void)
{
    struct store_fixture system_store;
    struct store_fixture network_store;
    struct store_fixture secret_store;
    struct appliance_state_loader loader;
    struct appliance_production_first_boot first_boot;

    memset(&first_boot, 0, sizeof(first_boot));
    struct network_device_snapshot device = make_device();
    struct network_ipv4_config runtime = make_runtime();
    uint8_t credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH];
    char output[APPLIANCE_PRODUCTION_FIRST_BOOT_OUTPUT_MAX];
    size_t output_length = 0U;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];

    install_store_header(&system_store);
    install_store_header(&network_store);
    install_store_header(&secret_store);
    load_loader(&loader, &system_store, &network_store, &secret_store);
    make_credential(credential);

    CHECK(appliance_production_first_boot_init(
              &first_boot, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              appliance_production_first_boot_pending(&first_boot),
          "fresh storage publishes first boot pending");

    CHECK(appliance_production_first_boot_run(
              &first_boot, "hfoc-test", 9U, NULL, false, false,
              credential) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_NETWORK_UNAVAILABLE &&
              first_boot.state ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_AWAITING_NETWORK,
          "first boot waits for the production network");

    CHECK(appliance_production_first_boot_run(
              &first_boot, "hfoc-test", 9U, &runtime, true, true,
              credential) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              first_boot.state ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED &&
              appliance_production_first_boot_provisioned(&first_boot),
          "first boot provisions hostname, network, and admin");
    CHECK(appliance_persistent_config_source(&loader.system_config) ==
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
          "first boot commits the hostname record");
    CHECK(appliance_persistent_network_config_source(
              &loader.network_config) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED,
          "first boot commits the network record");
    CHECK(appliance_persistent_secret_config_admin(
              &loader.secret_config, fingerprint) &&
              first_boot.ready_published,
          "first boot commits the admin fingerprint and publishes ready");
    CHECK(appliance_production_first_boot_render(
              &first_boot, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              output_length > 0U &&
              strstr(output, "first_boot") != NULL &&
              strstr(output, "state=PROVISIONED") != NULL,
          "first boot renders a canonical provisioning record");
}

static void test_first_boot_deferred_admin(void)
{
    struct store_fixture system_store;
    struct store_fixture network_store;
    struct store_fixture secret_store;
    struct appliance_state_loader loader;
    struct appliance_production_first_boot first_boot;

    memset(&first_boot, 0, sizeof(first_boot));
    struct network_device_snapshot device = make_device();
    struct network_ipv4_config runtime = make_runtime();
    char output[APPLIANCE_PRODUCTION_FIRST_BOOT_OUTPUT_MAX];
    size_t output_length = 0U;
    uint8_t fingerprint[SHA256_DIGEST_SIZE];

    install_store_header(&system_store);
    install_store_header(&network_store);
    install_store_header(&secret_store);
    load_loader(&loader, &system_store, &network_store, &secret_store);

    CHECK(appliance_production_first_boot_init(
              &first_boot, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              appliance_production_first_boot_pending(&first_boot),
          "deferred first boot starts pending");
    CHECK(appliance_production_first_boot_run_deferred(
              &first_boot, "hfoc-deferred", 13U, &runtime, true, true) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              first_boot.state ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_PROVISIONED &&
              appliance_production_first_boot_provisioned(&first_boot),
          "deferred first boot provisions hostname and network");
    CHECK(!appliance_production_first_boot_admin_provisioned(&first_boot),
          "deferred first boot publishes no administrator credential");
    CHECK(!appliance_persistent_secret_config_admin(
              &loader.secret_config, fingerprint),
          "deferred first boot leaves the credential record absent");
    /*
     * ITEM-506 increment 5: the serving gate's own reading of that state.  The
     * volume is ready and the structured reader finds no account, so the
     * management plane withholds the listener until the console conversation
     * sets one - the deferred path never silently serves.
     */
    {
        struct appliance_admin_credential readback;

        memset(&readback, 0, sizeof(readback));
        CHECK(appliance_state_loader_ready(&loader) &&
                  appliance_persistent_secret_config_admin_credential(
                      &loader.secret_config, &readback) !=
                      APPLIANCE_PERSISTENT_SECRET_CONFIG_OK,
              "deferred first boot leaves the serving gate without an account");
    }
    CHECK(appliance_persistent_config_source(&loader.system_config) ==
              APPLIANCE_CONFIG_VALIDATION_SOURCE_COMMITTED,
          "deferred first boot commits the hostname record");
    CHECK(appliance_persistent_network_config_source(
              &loader.network_config) ==
              APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED,
          "deferred first boot commits the network record");
    CHECK(first_boot.ready_published,
          "deferred first boot publishes ready");
    CHECK(appliance_production_first_boot_render(
              &first_boot, output, sizeof(output), &output_length) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              strstr(output, "state=PROVISIONED") != NULL,
          "deferred first boot renders the provisioning record");
}

static void test_reboot_reload(void)
{
    struct store_fixture system_store;
    struct store_fixture network_store;
    struct store_fixture secret_store;
    struct appliance_state_loader loader;
    struct appliance_production_first_boot first_boot;

    memset(&first_boot, 0, sizeof(first_boot));
    struct network_device_snapshot device = make_device();
    struct network_ipv4_config runtime = make_runtime();
    uint8_t credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH];
    char hostname[APPLIANCE_PRODUCTION_FIRST_BOOT_HOSTNAME_MAX + 1U];
    size_t hostname_length = 0U;

    install_store_header(&system_store);
    install_store_header(&network_store);
    install_store_header(&secret_store);
    make_credential(credential);
    {
        struct appliance_state_loader first_loader;
        struct appliance_production_first_boot first;

        memset(&first, 0, sizeof(first));

        load_loader(&first_loader, &system_store, &network_store,
                    &secret_store);
        CHECK(appliance_production_first_boot_init(
                  &first, &first_loader, &device) ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
                  appliance_production_first_boot_run(
                      &first, "hfoc-test", 9U, &runtime, true, true,
                      credential) ==
                      APPLIANCE_PRODUCTION_FIRST_BOOT_OK,
              "reboot fixture completes the first boot");
    }

    load_loader(&loader, &system_store, &network_store, &secret_store);
    CHECK(appliance_production_first_boot_init(
              &first_boot, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              !appliance_production_first_boot_pending(&first_boot),
          "committed storage reports no first boot pending");
    CHECK(appliance_production_first_boot_run(
              &first_boot, "ignored", 7U, &runtime, true, true,
              credential) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              first_boot.state ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_RELOADED &&
              !appliance_production_first_boot_provisioned(&first_boot),
          "reboot reloads without re-running the wizard");
    CHECK(appliance_persistent_config_hostname(
              &loader.system_config, hostname, sizeof(hostname),
              &hostname_length) == APPLIANCE_PERSISTENT_CONFIG_OK &&
              hostname_length == 9U &&
              memcmp(hostname, "hfoc-test", 9U) == 0,
          "reboot reloads the committed hostname");
}

static void test_bad_inputs(void)
{
    struct store_fixture system_store;
    struct store_fixture network_store;
    struct store_fixture secret_store;
    struct appliance_state_loader loader;
    struct appliance_production_first_boot first_boot;

    memset(&first_boot, 0, sizeof(first_boot));
    struct network_device_snapshot device = make_device();
    struct network_ipv4_config runtime = make_runtime();
    uint8_t credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH];
    uint8_t zero_credential[APPLIANCE_PRODUCTION_FIRST_BOOT_CREDENTIAL_LENGTH];

    install_store_header(&system_store);
    install_store_header(&network_store);
    install_store_header(&secret_store);
    load_loader(&loader, &system_store, &network_store, &secret_store);
    make_credential(credential);
    memset(zero_credential, 0, sizeof(zero_credential));

    CHECK(appliance_production_first_boot_init(
              NULL, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_NULL,
          "first boot init rejects a null owner");
    CHECK(appliance_production_first_boot_init(
              &first_boot, NULL, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_NULL,
          "first boot init rejects a null loader");
    device.mac_address[0] = 0U;
    device.mac_address[1] = 0U;
    device.mac_address[2] = 0U;
    device.mac_address[3] = 0U;
    device.mac_address[4] = 0U;
    device.mac_address[5] = 0U;
    CHECK(appliance_production_first_boot_init(
              &first_boot, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_DEVICE,
          "first boot init rejects an invalid NIC snapshot");
    device = make_device();
    CHECK(appliance_production_first_boot_init(
              &first_boot, &loader, &device) ==
              APPLIANCE_PRODUCTION_FIRST_BOOT_OK &&
              appliance_production_first_boot_run(
                  &first_boot, "hfoc-test", 9U, &runtime, true, true,
                  zero_credential) ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_BAD_CREDENTIAL &&
              first_boot.state ==
                  APPLIANCE_PRODUCTION_FIRST_BOOT_STATE_FAILED,
          "first boot rejects empty admin credential material");
}

int main(void)
{
    test_first_boot_provision();
    test_first_boot_deferred_admin();
    test_reboot_reload();
    test_bad_inputs();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance production first boot host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance production first boot host tests: ALL OK\n");
    return 0;
}

#else
typedef int appliance_production_first_boot_host_translation_unit_guard;
#endif
