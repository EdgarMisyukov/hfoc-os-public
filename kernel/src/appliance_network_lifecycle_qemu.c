#ifdef HFOC_APPLIANCE_NETWORK_CONFIG_QEMU_TEST

#include "appliance_network_lifecycle.h"
#include "appliance_persistent_network_config.h"
#include "block_device.h"
#include "data_region.h"
#include "kstring.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_LIFECYCLE_QEMU_BLOCK_SIZE 512U
#define NETWORK_LIFECYCLE_QEMU_BLOCK_COUNT 8U
#define NETWORK_LIFECYCLE_QEMU_CAPACITY \
    (NETWORK_LIFECYCLE_QEMU_BLOCK_SIZE * \
     NETWORK_LIFECYCLE_QEMU_BLOCK_COUNT)
#define NETWORK_LIFECYCLE_QEMU_REGION_OFFSET 1U
#define NETWORK_LIFECYCLE_QEMU_REGION_COUNT 7U

static unsigned char network_lifecycle_qemu_storage[
    NETWORK_LIFECYCLE_QEMU_CAPACITY];

struct network_lifecycle_qemu_transport_state {
    unsigned int activate_calls;
    unsigned int deactivate_calls;
    unsigned int discover_calls;
    unsigned int request_calls;
};

static void network_lifecycle_qemu_failure(const char *message)
    __attribute__((noreturn));

static void network_lifecycle_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[NETWORK-LIFECYCLE] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void network_lifecycle_qemu_require(
    bool condition,
    const char *message)
{
    if (!condition) {
        network_lifecycle_qemu_failure(message);
    }
}

static void network_lifecycle_qemu_zero(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void network_lifecycle_qemu_copy(
    void *destination,
    const void *source,
    size_t length)
{
    unsigned char *output = destination;
    const unsigned char *input = source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        output[index] = input[index];
    }
}

static void network_lifecycle_qemu_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] =
            (unsigned char)(value >> (8U * index));
    }
}

static void network_lifecycle_qemu_install_data_region(void)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    network_lifecycle_qemu_zero(
        network_lifecycle_qemu_storage,
        sizeof(network_lifecycle_qemu_storage));
    network_lifecycle_qemu_copy(
        network_lifecycle_qemu_storage, magic, sizeof(magic));
    network_lifecycle_qemu_storage[8U] = DATA_REGION_FORMAT_VERSION;
    network_lifecycle_qemu_store_le64(
        network_lifecycle_qemu_storage + 16U,
        NETWORK_LIFECYCLE_QEMU_REGION_OFFSET);
    network_lifecycle_qemu_store_le64(
        network_lifecycle_qemu_storage + 24U,
        NETWORK_LIFECYCLE_QEMU_REGION_COUNT);
}

static enum block_device_status network_lifecycle_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(network_lifecycle_qemu_storage) ||
        length > sizeof(network_lifecycle_qemu_storage) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    network_lifecycle_qemu_copy(
        buffer,
        network_lifecycle_qemu_storage + (size_t)offset,
        length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status network_lifecycle_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(network_lifecycle_qemu_storage) ||
        length > sizeof(network_lifecycle_qemu_storage) -
                      (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    network_lifecycle_qemu_copy(
        network_lifecycle_qemu_storage + (size_t)offset,
        buffer,
        length);
    return BLOCK_DEVICE_OK;
}

static bool network_lifecycle_qemu_transport_activate(
    void *context,
    const struct network_ipv4_config *active)
{
    struct network_lifecycle_qemu_transport_state *state = context;

    (void)active;
    ++state->activate_calls;
    return true;
}

static bool network_lifecycle_qemu_transport_deactivate(
    void *context)
{
    struct network_lifecycle_qemu_transport_state *state = context;

    ++state->deactivate_calls;
    return true;
}

static bool network_lifecycle_qemu_transport_discover(
    void *context)
{
    struct network_lifecycle_qemu_transport_state *state = context;

    ++state->discover_calls;
    return true;
}

static bool network_lifecycle_qemu_transport_request(
    void *context)
{
    struct network_lifecycle_qemu_transport_state *state = context;

    ++state->request_calls;
    return true;
}

static bool network_lifecycle_qemu_transport_renew(
    void *context)
{
    (void)context;
    return true;
}

static bool network_lifecycle_qemu_transport_rebind(
    void *context)
{
    (void)context;
    return true;
}

static bool network_lifecycle_qemu_transport_dns_query(
    void *context,
    const char *hostname,
    size_t hostname_length)
{
    (void)context;
    (void)hostname;
    (void)hostname_length;
    return true;
}

static void network_lifecycle_qemu_fill_static(
    struct network_ipv4_config *config)
{
    network_lifecycle_qemu_zero(config, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config->address.octets[0] = 10U;
    config->address.octets[2] = 2U;
    config->address.octets[3] = 15U;
    config->prefix_length = 24U;
    config->gateway.octets[0] = 10U;
    config->gateway.octets[2] = 2U;
    config->gateway.octets[3] = 2U;
    config->dns.octets[0] = 10U;
    config->dns.octets[2] = 2U;
    config->dns.octets[3] = 3U;
}

static bool network_lifecycle_qemu_str_contains(
    const char *text,
    const char *needle);

static void network_lifecycle_qemu_load_desired(
    bool static_mode,
    struct appliance_network_config *desired)
{
    struct block_device device;
    struct appliance_persistent_network_config config;
    struct network_ipv4_config static_config;

    network_lifecycle_qemu_install_data_region();
    network_lifecycle_qemu_zero(&device, sizeof(device));
    network_lifecycle_qemu_zero(&config, sizeof(config));
    network_lifecycle_qemu_zero(
        &static_config, sizeof(static_config));
    device.block_size = NETWORK_LIFECYCLE_QEMU_BLOCK_SIZE;
    device.block_count = NETWORK_LIFECYCLE_QEMU_BLOCK_COUNT;
    device.read_fn = network_lifecycle_qemu_read;
    device.write_fn = network_lifecycle_qemu_write;
    network_lifecycle_qemu_require(
        appliance_persistent_network_config_load(
            &config, &device) ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
        "persistent network config fixture load");
    if (static_mode) {
        network_lifecycle_qemu_fill_static(&static_config);
        network_lifecycle_qemu_require(
            appliance_persistent_network_config_set_static(
                &config, "eth0", 4U, &static_config) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
            "persistent STATIC fixture commit");
    } else {
        network_lifecycle_qemu_require(
            appliance_persistent_network_config_set_dhcp(
                &config, "eth0", 4U) ==
                APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK,
            "persistent DHCP fixture commit");
    }
    network_lifecycle_qemu_require(
        config.source ==
            APPLIANCE_PERSISTENT_NETWORK_CONFIG_SOURCE_COMMITTED,
        "persistent network fixture committed source");
    network_lifecycle_qemu_copy(
        desired, &config.values, sizeof(*desired));
}

static struct network_device_snapshot
network_lifecycle_qemu_make_device(void)
{
    struct network_device_snapshot device;

    network_lifecycle_qemu_zero(&device, sizeof(device));
    device.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device.device_id = 1U;
    device.vendor_id = 0x1234U;
    device.product_id = 0x5678U;
    device.bus_address = 0U;
    device.capability_mask = NETWORK_DEVICE_CAPABILITY_MASK;
    device.mac_address[0] = 0x02U;
    device.mac_address[5] = 0x01U;
    return device;
}

static struct network_link_state_record
network_lifecycle_qemu_make_link(enum network_link_state state)
{
    struct network_link_state_record link;

    network_lifecycle_qemu_zero(&link, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = state;
    link.generation = 7U;
    return link;
}

static void network_lifecycle_qemu_static_proof(void)
{
    struct appliance_network_config desired;
    struct network_device_snapshot device =
        network_lifecycle_qemu_make_device();
    struct network_link_state_record link_up =
        network_lifecycle_qemu_make_link(NETWORK_LINK_UP);
    struct network_link_state_record link_down =
        network_lifecycle_qemu_make_link(NETWORK_LINK_DOWN);
    struct network_lifecycle_qemu_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct appliance_network_lifecycle lifecycle;
    char output[APPLIANCE_NETWORK_LIFECYCLE_OUTPUT_MAX + 1U];
    size_t output_length = 0U;

    network_lifecycle_qemu_zero(
        &transport_state, sizeof(transport_state));
    network_lifecycle_qemu_zero(&transport, sizeof(transport));
    transport.version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    transport.context = &transport_state;
    transport.activate_ipv4 =
        network_lifecycle_qemu_transport_activate;
    transport.deactivate_ipv4 =
        network_lifecycle_qemu_transport_deactivate;
    transport.dhcp_discover =
        network_lifecycle_qemu_transport_discover;
    transport.dhcp_request =
        network_lifecycle_qemu_transport_request;
    transport.dhcp_renew =
        network_lifecycle_qemu_transport_renew;
    transport.dhcp_rebind =
        network_lifecycle_qemu_transport_rebind;
    transport.dns_query =
        network_lifecycle_qemu_transport_dns_query;
    network_lifecycle_qemu_load_desired(true, &desired);
    network_lifecycle_qemu_zero(&lifecycle, sizeof(lifecycle));
    network_lifecycle_qemu_require(
        appliance_network_lifecycle_init(
            &lifecycle, &desired, &device, &transport) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_start(&lifecycle) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_link(
            &lifecycle, &link_up) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_state(&lifecycle) ==
            APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
        transport_state.activate_calls == 1U &&
        appliance_network_lifecycle_reachability(
            &lifecycle,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_render(
            &lifecycle, output, sizeof(output), &output_length) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        network_lifecycle_qemu_str_contains(
            output, "state=ACTIVE interface=eth0 mode=STATIC") &&
        appliance_network_lifecycle_link(
            &lifecycle, &link_down) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        !appliance_network_lifecycle_active(&lifecycle) &&
        appliance_network_lifecycle_stop(&lifecycle) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK,
        "persisted STATIC lifecycle proof");
    serial_write(
        "[NETWORK-LIFECYCLE] persisted STATIC lifecycle OK\n");
}

static void network_lifecycle_qemu_dhcp_proof(void)
{
    struct appliance_network_config desired;
    struct network_ipv4_config lease;
    struct network_device_snapshot device =
        network_lifecycle_qemu_make_device();
    struct network_link_state_record link_up =
        network_lifecycle_qemu_make_link(NETWORK_LINK_UP);
    struct network_lifecycle_qemu_transport_state transport_state;
    struct appliance_network_lifecycle_transport transport;
    struct appliance_network_lifecycle lifecycle;

    network_lifecycle_qemu_fill_static(&lease);
    network_lifecycle_qemu_zero(
        &transport_state, sizeof(transport_state));
    network_lifecycle_qemu_zero(&transport, sizeof(transport));
    transport.version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    transport.context = &transport_state;
    transport.activate_ipv4 =
        network_lifecycle_qemu_transport_activate;
    transport.deactivate_ipv4 =
        network_lifecycle_qemu_transport_deactivate;
    transport.dhcp_discover =
        network_lifecycle_qemu_transport_discover;
    transport.dhcp_request =
        network_lifecycle_qemu_transport_request;
    transport.dhcp_renew =
        network_lifecycle_qemu_transport_renew;
    transport.dhcp_rebind =
        network_lifecycle_qemu_transport_rebind;
    transport.dns_query =
        network_lifecycle_qemu_transport_dns_query;
    network_lifecycle_qemu_load_desired(false, &desired);
    network_lifecycle_qemu_zero(&lifecycle, sizeof(lifecycle));
    network_lifecycle_qemu_require(
        appliance_network_lifecycle_init(
            &lifecycle, &desired, &device, &transport) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_start(&lifecycle) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_link(
            &lifecycle, &link_up) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        transport_state.discover_calls == 1U &&
        appliance_network_lifecycle_dhcp_offer(
            &lifecycle, &lease) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        transport_state.request_calls == 1U &&
        appliance_network_lifecycle_dhcp_ack(
            &lifecycle, &lease) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_state(&lifecycle) ==
            APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
        transport_state.activate_calls == 1U &&
        appliance_network_lifecycle_reachability(
            &lifecycle,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE,
            APPLIANCE_NETWORK_REACHABILITY_REACHABLE) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK,
        "persisted DHCP lifecycle proof");
    serial_write(
        "[NETWORK-LIFECYCLE] persisted DHCP lifecycle OK\n");
}

static bool network_lifecycle_qemu_str_contains(
    const char *text,
    const char *needle)
{
    size_t text_length = strlen(text);
    size_t needle_length = strlen(needle);
    size_t index;

    if (needle_length > text_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= text_length; ++index) {
        if (memcmp(text + index, needle, needle_length) == 0) {
            return true;
        }
    }
    return false;
}

void appliance_network_lifecycle_qemu_test_init(void)
{
    serial_write("[NETWORK-LIFECYCLE] proof init\n");
    network_lifecycle_qemu_static_proof();
    network_lifecycle_qemu_dhcp_proof();
    serial_write("[NETWORK-LIFECYCLE] clean shutdown\n");
    serial_write("[NETWORK-LIFECYCLE] proof OK\n");
}

#else
typedef int appliance_network_lifecycle_qemu_translation_unit_guard;
#endif
