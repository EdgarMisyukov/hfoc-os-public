#ifdef HFOC_APPLIANCE_PRODUCTION_SERVICE_HTTPS_QEMU_TEST

#include "appliance_https_service.h"
#include "appliance_network_config.h"
#include "appliance_network_lifecycle.h"
#include "appliance_persistent_service_config.h"
#include "appliance_production_service_https.h"
#include "appliance_service_manager.h"
#include "block_device.h"
#include "data_region.h"
#include "https_service.h"
#include "https_service_lifecycle.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "network_device.h"
#include "serial.h"
#include "service.h"
#include "service_supervisor.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROD_SVC_QEMU_BLOCK_SIZE 512U
#define PROD_SVC_QEMU_BLOCK_COUNT 8U
#define PROD_SVC_QEMU_CAPACITY \
    (PROD_SVC_QEMU_BLOCK_SIZE * PROD_SVC_QEMU_BLOCK_COUNT)
#define PROD_SVC_QEMU_REGION_OFFSET 1U
#define PROD_SVC_QEMU_REGION_COUNT 7U

static unsigned char prod_svc_qemu_storage[PROD_SVC_QEMU_CAPACITY];

struct prod_svc_qemu_manager_fake {
    uint32_t start_calls;
    uint32_t stop_calls;
    struct service_supervisor_snapshot supervisor;
};

struct prod_svc_qemu_lifecycle_fake {
    bool stop_result;
    bool drain_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool abort_results[HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY];
    bool zeroize_result;
    bool http_result;
    bool packet_result;
    bool restart_result;
};

struct prod_svc_qemu_transport_state {
    uint32_t activate_calls;
    uint32_t deactivate_calls;
};

static void prod_svc_qemu_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void prod_svc_qemu_copy_bytes(
    void *destination,
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

static void prod_svc_qemu_store_le64(
    unsigned char *bytes,
    uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void prod_svc_qemu_failure(const char *message)
    __attribute__((noreturn));

static void prod_svc_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[PROD-SVC] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void prod_svc_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        prod_svc_qemu_failure(message);
    }
}

static void prod_svc_qemu_copy_name(
    char *destination,
    const char *source)
{
    uint32_t index = 0U;

    while (source[index] != '\0' && index < SERVICE_NAME_MAX) {
        destination[index] = source[index];
        ++index;
    }
    destination[index] = '\0';
}

static struct service_descriptor prod_svc_qemu_service_descriptor(void)
{
    struct service_descriptor descriptor;

    prod_svc_qemu_zero_bytes(&descriptor, sizeof(descriptor));
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.version = SERVICE_DESCRIPTOR_VERSION;
    descriptor.enabled = true;
    descriptor.restart_limit = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_timeout_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    prod_svc_qemu_copy_name(descriptor.name, HTTPS_SERVICE_NAME);
    return descriptor;
}

static struct https_service_descriptor prod_svc_qemu_https_descriptor(void)
{
    struct https_service_descriptor descriptor;

    prod_svc_qemu_zero_bytes(&descriptor, sizeof(descriptor));
    descriptor.version = HTTPS_SERVICE_DESCRIPTOR_VERSION;
    descriptor.id = HTTPS_SERVICE_ID;
    descriptor.restart_budget = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    descriptor.readiness_deadline_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    descriptor.envelope.connection_capacity =
        HTTPS_SERVICE_CONNECTION_CAPACITY;
    descriptor.envelope.tls_context_capacity =
        HTTPS_SERVICE_TLS_CONTEXT_CAPACITY;
    descriptor.envelope.http_parser_capacity =
        HTTPS_SERVICE_HTTP_PARSER_CAPACITY;
    descriptor.envelope.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    prod_svc_qemu_copy_name(descriptor.name, HTTPS_SERVICE_NAME);
    return descriptor;
}

static struct https_service_lifecycle_config
prod_svc_qemu_lifecycle_config(void)
{
    struct https_service_lifecycle_config config;

    prod_svc_qemu_zero_bytes(&config, sizeof(config));
    config.version = HTTPS_SERVICE_LIFECYCLE_VERSION;
    config.drain_timeout_ms = 100U;
    config.connection_capacity =
        HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
    config.tls_context_capacity =
        HTTPS_SERVICE_LIFECYCLE_TLS_CONTEXT_CAPACITY;
    config.http_parser_capacity =
        HTTPS_SERVICE_LIFECYCLE_HTTP_PARSER_CAPACITY;
    config.packet_pool_capacity =
        HTTPS_SERVICE_PACKET_POOL_CAPACITY_DEFAULT;
    return config;
}

static void prod_svc_qemu_entry(void) {
}

static enum service_supervisor_status prod_svc_qemu_start(
    void *context,
    const struct service_graph_snapshot *snapshot,
    const struct service_supervisor_entry *entries,
    uint32_t entry_count)
{
    struct prod_svc_qemu_manager_fake *fake = context;

    (void)snapshot;
    (void)entries;
    (void)entry_count;
    ++fake->start_calls;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status prod_svc_qemu_stop(
    void *context,
    const struct service_graph_snapshot *snapshot)
{
    struct prod_svc_qemu_manager_fake *fake = context;

    (void)snapshot;
    ++fake->stop_calls;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status prod_svc_qemu_restart(
    void *context,
    uint32_t service_id,
    uint64_t now_ticks,
    const struct service_graph_snapshot *snapshot,
    uint32_t *exit_status_out,
    bool *restarted)
{
    (void)context;
    (void)service_id;
    (void)now_ticks;
    (void)snapshot;
    (void)exit_status_out;
    (void)restarted;
    return SERVICE_SUPERVISOR_OK;
}

static enum service_supervisor_status prod_svc_qemu_manager_snapshot(
    void *context,
    struct service_supervisor_snapshot *snapshot)
{
    struct prod_svc_qemu_manager_fake *fake = context;

    *snapshot = fake->supervisor;
    return SERVICE_SUPERVISOR_OK;
}

static void prod_svc_qemu_reset_manager_fake(
    struct prod_svc_qemu_manager_fake *fake,
    const struct service_descriptor *descriptor)
{
    struct service_supervisor_snapshot_entry *entry;

    prod_svc_qemu_zero_bytes(fake, sizeof(*fake));
    entry = &fake->supervisor.entries[0];
    fake->supervisor.published = true;
    fake->supervisor.version = SERVICE_SUPERVISOR_SNAPSHOT_VERSION;
    fake->supervisor.generation = 1U;
    fake->supervisor.active_count = 1U;
    fake->supervisor.entry_count = 1U;
    prod_svc_qemu_require(
        service_descriptor_init(&entry->descriptor, descriptor) ==
            SERVICE_DESCRIPTOR_OK,
        "supervisor snapshot descriptor init");
    entry->state = SERVICE_SUPERVISOR_STATE_RUNNING;
    entry->health_state = SERVICE_HEALTH_READY;
}

static struct appliance_service_manager_ops prod_svc_qemu_manager_ops(
    struct prod_svc_qemu_manager_fake *fake)
{
    struct appliance_service_manager_ops ops;

    prod_svc_qemu_zero_bytes(&ops, sizeof(ops));
    ops.start_ordered = prod_svc_qemu_start;
    ops.stop_ordered = prod_svc_qemu_stop;
    ops.restart_contained = prod_svc_qemu_restart;
    ops.snapshot = prod_svc_qemu_manager_snapshot;
    ops.context = fake;
    return ops;
}

static bool prod_svc_qemu_stop_admission(
    void *context,
    uint32_t generation)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;

    (void)generation;
    return fake->stop_result;
}

static bool prod_svc_qemu_drain_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;
    bool result;

    (void)generation;
    result = fake->drain_results[slot];
    fake->drain_results[slot] = false;
    return result;
}

static bool prod_svc_qemu_abort_connection(
    void *context,
    uint32_t generation,
    uint32_t slot)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;
    bool result;

    (void)generation;
    result = fake->abort_results[slot];
    fake->abort_results[slot] = false;
    return result;
}

static bool prod_svc_qemu_zeroize_tls(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;

    (void)generation;
    (void)count;
    return fake->zeroize_result;
}

static bool prod_svc_qemu_release_http(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;

    (void)generation;
    (void)count;
    return fake->http_result;
}

static bool prod_svc_qemu_release_packet(
    void *context,
    uint32_t generation,
    uint32_t count)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;

    (void)generation;
    (void)count;
    return fake->packet_result;
}

static bool prod_svc_qemu_restart_admission(
    void *context,
    uint32_t generation)
{
    struct prod_svc_qemu_lifecycle_fake *fake = context;

    (void)generation;
    return fake->restart_result;
}

static void prod_svc_qemu_reset_lifecycle_fake(
    struct prod_svc_qemu_lifecycle_fake *fake)
{
    uint32_t index;

    prod_svc_qemu_zero_bytes(fake, sizeof(*fake));
    fake->stop_result = true;
    fake->zeroize_result = true;
    fake->http_result = true;
    fake->packet_result = true;
    fake->restart_result = true;
    for (index = 0U;
         index < HTTPS_SERVICE_LIFECYCLE_CONNECTION_CAPACITY;
         ++index) {
        fake->drain_results[index] = true;
        fake->abort_results[index] = true;
    }
}

static struct https_service_lifecycle_ops prod_svc_qemu_lifecycle_ops(
    struct prod_svc_qemu_lifecycle_fake *fake)
{
    struct https_service_lifecycle_ops ops;

    prod_svc_qemu_zero_bytes(&ops, sizeof(ops));
    ops.stop_admission = prod_svc_qemu_stop_admission;
    ops.drain_connection = prod_svc_qemu_drain_connection;
    ops.abort_connection = prod_svc_qemu_abort_connection;
    ops.zeroize_tls = prod_svc_qemu_zeroize_tls;
    ops.release_http = prod_svc_qemu_release_http;
    ops.release_packet = prod_svc_qemu_release_packet;
    ops.restart_admission = prod_svc_qemu_restart_admission;
    ops.context = fake;
    return ops;
}

static enum block_device_status prod_svc_qemu_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(prod_svc_qemu_storage) ||
        length > sizeof(prod_svc_qemu_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    prod_svc_qemu_copy_bytes(
        buffer, prod_svc_qemu_storage + (size_t)offset, length);
    return BLOCK_DEVICE_OK;
}

static enum block_device_status prod_svc_qemu_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    (void)device;
    if (offset > sizeof(prod_svc_qemu_storage) ||
        length > sizeof(prod_svc_qemu_storage) - (size_t)offset) {
        return BLOCK_DEVICE_IO_ERROR;
    }
    prod_svc_qemu_copy_bytes(
        prod_svc_qemu_storage + (size_t)offset, buffer, length);
    return BLOCK_DEVICE_OK;
}

static void prod_svc_qemu_install_data_region(void)
{
    static const unsigned char magic[DATA_REGION_MAGIC_SIZE] =
        DATA_REGION_MAGIC_BYTES;

    prod_svc_qemu_zero_bytes(
        prod_svc_qemu_storage, sizeof(prod_svc_qemu_storage));
    prod_svc_qemu_copy_bytes(
        prod_svc_qemu_storage, magic, sizeof(magic));
    prod_svc_qemu_storage[8U] = DATA_REGION_FORMAT_VERSION;
    prod_svc_qemu_store_le64(
        prod_svc_qemu_storage + 16U,
        PROD_SVC_QEMU_REGION_OFFSET);
    prod_svc_qemu_store_le64(
        prod_svc_qemu_storage + 24U,
        PROD_SVC_QEMU_REGION_COUNT);
}

static struct appliance_persistent_service_config_values
prod_svc_qemu_enabled_values(void)
{
    struct appliance_persistent_service_config_values values;

    prod_svc_qemu_zero_bytes(&values, sizeof(values));
    values.enabled = true;
    values.restart_limit = HTTPS_SERVICE_RESTART_BUDGET_DEFAULT;
    values.readiness_timeout_ms =
        HTTPS_SERVICE_READINESS_DEADLINE_DEFAULT_MS;
    return values;
}

static bool prod_svc_qemu_transport_activate(
    void *context,
    const struct network_ipv4_config *active)
{
    struct prod_svc_qemu_transport_state *state = context;

    (void)active;
    ++state->activate_calls;
    return true;
}

static bool prod_svc_qemu_transport_deactivate(void *context)
{
    struct prod_svc_qemu_transport_state *state = context;

    ++state->deactivate_calls;
    return true;
}

static bool prod_svc_qemu_transport_noop(void *context)
{
    (void)context;
    return true;
}

static bool prod_svc_qemu_transport_dns_noop(
    void *context,
    const char *hostname,
    size_t hostname_length)
{
    (void)context;
    (void)hostname;
    (void)hostname_length;
    return true;
}

static struct network_ipv4_config prod_svc_qemu_static_config(void)
{
    struct network_ipv4_config config;

    prod_svc_qemu_zero_bytes(&config, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static struct appliance_network_config prod_svc_qemu_desired(void)
{
    struct appliance_network_config desired;

    prod_svc_qemu_zero_bytes(&desired, sizeof(desired));
    desired.version = APPLIANCE_NETWORK_CONFIG_VERSION;
    desired.mode = APPLIANCE_NETWORK_CONFIG_MODE_STATIC;
    desired.interface_length = 4U;
    desired.static_present = true;
    desired.static_config = prod_svc_qemu_static_config();
    desired.initialized = true;
    prod_svc_qemu_copy_name(desired.interface, "eth0");
    return desired;
}

static struct network_device_snapshot prod_svc_qemu_device(void)
{
    struct network_device_snapshot device;

    prod_svc_qemu_zero_bytes(&device, sizeof(device));
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

static struct network_link_state_record prod_svc_qemu_link(
    enum network_link_state state)
{
    struct network_link_state_record link;

    prod_svc_qemu_zero_bytes(&link, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = state;
    link.generation = 7U;
    return link;
}

static struct appliance_production_service_https_evidence
prod_svc_qemu_evidence(void)
{
    struct appliance_production_service_https_evidence evidence;

    prod_svc_qemu_zero_bytes(&evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = 1U;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = true;
    evidence.manager_stopped = true;
    evidence.https_enabled = true;
    evidence.https_stopped = true;
    return evidence;
}

static struct appliance_production_service_https_readiness
prod_svc_qemu_readiness(
    bool network_active,
    bool entropy_ready,
    bool certificate_ready)
{
    struct appliance_production_service_https_readiness readiness;

    prod_svc_qemu_zero_bytes(&readiness, sizeof(readiness));
    readiness.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_READINESS_VERSION;
    readiness.source_generation = 1U;
    readiness.network_active = network_active;
    readiness.entropy_ready = entropy_ready;
    readiness.certificate_ready = certificate_ready;
    return readiness;
}

static bool prod_svc_qemu_mem_equal(
    const char *text,
    const char *needle,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (text[index] != needle[index]) {
            return false;
        }
    }
    return true;
}

static bool prod_svc_qemu_str_contains_exact(
    const char *text,
    const char *needle)
{
    size_t text_length = 0U;
    size_t needle_length = 0U;
    size_t index;

    while (text[text_length] != '\0') {
        ++text_length;
    }
    while (needle[needle_length] != '\0') {
        ++needle_length;
    }
    if (needle_length > text_length) {
        return false;
    }
    for (index = 0U; index + needle_length <= text_length; ++index) {
        if (prod_svc_qemu_mem_equal(
                text + index, needle, needle_length)) {
            return true;
        }
    }
    return false;
}

void appliance_production_service_https_qemu_test_init(void)
{
    static struct block_device device;
    struct service_descriptor descriptor =
        prod_svc_qemu_service_descriptor();
    static struct service_supervisor_entry entry;
    struct appliance_persistent_service_config_values values =
        prod_svc_qemu_enabled_values();
    static struct appliance_persistent_service_config store;
    static struct prod_svc_qemu_manager_fake manager_fake;
    static struct appliance_service_manager manager;
    static struct appliance_service_manager_ops manager_ops;
    struct https_service_descriptor https_descriptor =
        prod_svc_qemu_https_descriptor();
    struct https_service_lifecycle_config lifecycle_config =
        prod_svc_qemu_lifecycle_config();
    static struct prod_svc_qemu_lifecycle_fake lifecycle_fake;
    static struct https_service_lifecycle_ops lifecycle_ops;
    static struct appliance_https_service https_service;
    static struct https_service_readiness https_readiness;
    static struct prod_svc_qemu_transport_state transport_state;
    static struct appliance_network_lifecycle_transport transport;
    static struct appliance_network_lifecycle network;
    struct appliance_network_config desired =
        prod_svc_qemu_desired();
    struct network_device_snapshot device_snapshot =
        prod_svc_qemu_device();
    struct network_link_state_record link_up =
        prod_svc_qemu_link(NETWORK_LINK_UP);
    struct network_link_state_record link_down =
        prod_svc_qemu_link(NETWORK_LINK_DOWN);
    static struct appliance_production_service_https composition;
    struct appliance_production_service_https_evidence evidence =
        prod_svc_qemu_evidence();
    struct appliance_production_service_https_readiness readiness =
        prod_svc_qemu_readiness(false, false, false);
    static struct appliance_production_service_https_snapshot snapshot;
    static char output[APPLIANCE_PRODUCTION_SERVICE_HTTPS_OUTPUT_MAX + 1U];
    static size_t output_length = 0U;

    prod_svc_qemu_zero_bytes(&device, sizeof(device));
    prod_svc_qemu_zero_bytes(&entry, sizeof(entry));
    prod_svc_qemu_zero_bytes(&store, sizeof(store));
    prod_svc_qemu_zero_bytes(&manager, sizeof(manager));
    prod_svc_qemu_zero_bytes(&https_service, sizeof(https_service));
    prod_svc_qemu_zero_bytes(&transport, sizeof(transport));
    prod_svc_qemu_zero_bytes(&network, sizeof(network));
    prod_svc_qemu_zero_bytes(&composition, sizeof(composition));
    prod_svc_qemu_install_data_region();
    entry.service_id = HTTPS_SERVICE_ID;
    entry.entry = prod_svc_qemu_entry;
    device.block_size = PROD_SVC_QEMU_BLOCK_SIZE;
    device.block_count = PROD_SVC_QEMU_BLOCK_COUNT;
    device.read_fn = prod_svc_qemu_read;
    device.write_fn = prod_svc_qemu_write;

    serial_write("[PROD-SVC] proof init\n");

    prod_svc_qemu_require(
        appliance_persistent_service_config_load(
            &store, &device, &descriptor, 1U) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
        appliance_persistent_service_config_set_config(
            &store, HTTPS_SERVICE_ID, &values) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
        appliance_persistent_service_config_source(&store) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED &&
        appliance_persistent_service_config_enabled(
            &store, HTTPS_SERVICE_ID),
        "persistent HTTPS service evidence commit");
    prod_svc_qemu_zero_bytes(&store, sizeof(store));
    prod_svc_qemu_require(
        appliance_persistent_service_config_load(
            &store, &device, &descriptor, 1U) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
        appliance_persistent_service_config_source(&store) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_SOURCE_COMMITTED,
        "fresh persistent service evidence load");
    serial_write("[PROD-SVC] persistent evidence committed\n");

    prod_svc_qemu_reset_manager_fake(&manager_fake, &descriptor);
    manager_ops = prod_svc_qemu_manager_ops(&manager_fake);
    prod_svc_qemu_require(
        appliance_service_manager_init(
            &manager, &manager_ops,
            APPLIANCE_SERVICE_MANAGER_STARTUP_ROLLBACK) ==
            APPLIANCE_SERVICE_MANAGER_OK &&
        appliance_persistent_service_config_apply(
            &store, &manager, &entry, 1U) ==
            APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK &&
        manager.state == APPLIANCE_SERVICE_MANAGER_STATE_STOPPED &&
        manager.graph.published,
        "persistent service table applied to stopped manager");

    prod_svc_qemu_reset_lifecycle_fake(&lifecycle_fake);
    lifecycle_ops = prod_svc_qemu_lifecycle_ops(&lifecycle_fake);
    prod_svc_qemu_require(
        appliance_https_service_init(
            &https_service,
            &manager,
            &https_descriptor,
            &lifecycle_config,
            &lifecycle_ops) == APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_service_state(&https_service) ==
            APPLIANCE_HTTPS_SERVICE_STATE_STOPPED,
        "HTTPS boundary initialized against configured manager");

    prod_svc_qemu_zero_bytes(&transport_state, sizeof(transport_state));
    transport.version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    transport.context = &transport_state;
    transport.activate_ipv4 = prod_svc_qemu_transport_activate;
    transport.deactivate_ipv4 = prod_svc_qemu_transport_deactivate;
    transport.dhcp_discover = prod_svc_qemu_transport_noop;
    transport.dhcp_request = prod_svc_qemu_transport_noop;
    transport.dhcp_renew = prod_svc_qemu_transport_noop;
    transport.dhcp_rebind = prod_svc_qemu_transport_noop;
    transport.dns_query = prod_svc_qemu_transport_dns_noop;
    prod_svc_qemu_require(
        appliance_network_lifecycle_init(
            &network,
            &desired,
            &device_snapshot,
            &transport) == APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_start(&network) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_link(&network, &link_up) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        appliance_network_lifecycle_active(&network),
        "network lifecycle active evidence");
    readiness = prod_svc_qemu_readiness(false, false, false);
    prod_svc_qemu_require(
        appliance_production_service_https_init(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_configure(
            &composition, &evidence) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_readiness(
            &composition, &readiness) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS &&
        appliance_production_service_https_start(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_MISSING_READINESS &&
        appliance_production_service_https_state(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_AWAITING_READINESS,
        "missing HTTPS readiness blocks composition start");
    serial_write("[PROD-SVC] missing readiness blocks https\n");

    readiness = prod_svc_qemu_readiness(
        true, true, true);
    https_readiness.version = HTTPS_SERVICE_READINESS_VERSION;
    https_readiness.available_mask =
        HTTPS_SERVICE_REQUIRED_DEPENDENCY_MASK;
    prod_svc_qemu_require(
        appliance_production_service_https_readiness(
            &composition, &readiness) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_start(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_state(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_STARTING,
        "complete readiness allows production composition start");
    prod_svc_qemu_require(
        appliance_https_service_start(
            &https_service, &https_readiness) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_service_ready(
            &https_service, &https_readiness) ==
            APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_service_state(&https_service) ==
            APPLIANCE_HTTPS_SERVICE_STATE_RUNNING &&
        manager.state == APPLIANCE_SERVICE_MANAGER_STATE_RUNNING,
        "existing HTTPS boundary starts and reaches running");
    prod_svc_qemu_require(
        appliance_production_service_https_running(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_state(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
        appliance_production_service_https_published(&composition),
        "production composition running publication");
    serial_write("[PROD-SVC] service manager started\n");
    serial_write("[PROD-SVC] https running\n");

    prod_svc_qemu_require(
        appliance_production_service_https_snapshot(
            &composition, &snapshot) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        snapshot.state ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_RUNNING &&
        snapshot.evidence_published &&
        appliance_production_service_https_render(
            &snapshot, output, sizeof(output), &output_length) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        prod_svc_qemu_str_contains_exact(
            output,
            "production_service_https v=1 state=RUNNING "
            "https=ENABLED network=ACTIVE entropy=READY "
            "certificate=READY health=PASS"),
        "canonical production composition snapshot");
    serial_write("[PROD-SVC] canonical snapshot\n");

    prod_svc_qemu_require(
        appliance_network_lifecycle_link(&network, &link_down) ==
            APPLIANCE_NETWORK_LIFECYCLE_OK &&
        !appliance_network_lifecycle_active(&network) &&
        appliance_https_service_stop_admission(
            &https_service, 200U) == APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_https_service_stop_step(
            &https_service, 201U) == APPLIANCE_HTTPS_SERVICE_OK &&
        appliance_production_service_https_fail_closed(
            &composition,
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_FAILURE_NETWORK) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK &&
        appliance_production_service_https_state(&composition) ==
            APPLIANCE_PRODUCTION_SERVICE_HTTPS_STATE_FAILED &&
        !appliance_production_service_https_published(&composition),
        "link loss drains HTTPS and fails closed");
    serial_write("[PROD-SVC] link loss fails closed\n");
    serial_write("[PROD-SVC] clean shutdown\n");
    serial_write("[PROD-SVC] proof OK\n");
}

#else
typedef int appliance_production_service_https_qemu_translation_unit_guard;
#endif
