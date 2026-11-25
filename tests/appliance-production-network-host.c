/*
 * ITEM-362A focused host proof for the production NIC/lwIP backend
 * composition. The test owns a deterministic Ethernet sink and drives the
 * production bind/input path with crafted ARP and ICMP frames.
 */
#include "appliance_production_network.h"
#include "network_ipv4_profile.h"
#include "hfoc_lwip.h"
#include "hfoc_lwip_time.h"

#include "lwip/pbuf.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_PRODUCTION_NETWORK_HOST_TEST

#define TEST_FRAME_MAX HFOC_LWIP_PROTO_FRAME_MAX
#define TEST_OUTPUT_MAX 8U

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct test_output_state {
    uint32_t count;
    size_t lengths[TEST_OUTPUT_MAX];
    uint8_t frames[TEST_OUTPUT_MAX][TEST_FRAME_MAX];
};

static struct test_output_state output_state;
static uint32_t test_now_ms = 1000U;
static bool time_initialized;

u32_t sys_now(void)
{
    return test_now_ms;
}

static uint32_t test_time_source(void)
{
    return test_now_ms;
}

static bool test_output_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    struct test_output_state *state = context;

    if (state->count >= TEST_OUTPUT_MAX || length > TEST_FRAME_MAX) {
        return false;
    }
    memcpy(state->frames[state->count], frame, length);
    state->lengths[state->count] = length;
    ++state->count;
    return true;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)(value & 0xFFU);
}

static uint16_t checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0U;
    size_t offset;

    for (offset = 0U; offset + 1U < length; offset += 2U) {
        sum += ((uint32_t)data[offset] << 8U) | data[offset + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static bool mac_equal(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, NETWORK_DEVICE_MAC_LENGTH) == 0;
}

static void copy_mac(uint8_t *destination, const uint8_t *source)
{
    memcpy(destination, source, NETWORK_DEVICE_MAC_LENGTH);
}

static void pad_frame(uint8_t *frame, size_t *length, size_t minimum)
{
    while (*length < minimum) {
        frame[(*length)++] = 0U;
    }
}

static void build_arp_request(
    uint8_t *frame,
    size_t *length,
    const uint8_t peer_mac[6])
{
    size_t position = 0U;

    memset(frame, 0, TEST_FRAME_MAX);
    memset(frame + position, 0xFF, 6U);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x06U;
    write_be16(frame + position, 1U);
    position += 2U;
    write_be16(frame + position, 0x0800U);
    position += 2U;
    frame[position++] = 6U;
    frame[position++] = 4U;
    write_be16(frame + position, 1U);
    position += 2U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 2U;
    memset(frame + position, 0, 6U);
    position += 6U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 15U;
    pad_frame(frame, &position, 60U);
    *length = position;
}

static void build_icmp_echo(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6],
    uint32_t now_ms)
{
    static const uint8_t payload[] = "HFOC-PING";
    size_t position = 0U;
    size_t ip_offset;
    size_t icmp_offset;
    size_t total;

    memset(frame, 0, TEST_FRAME_MAX);
    copy_mac(frame + position, local_mac);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x00U;
    ip_offset = position;
    frame[position++] = 0x45U;
    frame[position++] = 0U;
    icmp_offset = position + 18U;
    total = 20U + 8U + sizeof(payload) - 1U;
    write_be16(frame + position, (uint16_t)total);
    position += 2U;
    write_be16(frame + position, (uint16_t)(0x1200U + (now_ms & 0xFFU)));
    position += 2U;
    write_be16(frame + position, 0x4000U);
    position += 2U;
    frame[position++] = 64U;
    frame[position++] = 1U;
    write_be16(frame + position, 0U);
    position += 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 15U;
    position = icmp_offset;
    frame[position++] = 8U;
    frame[position++] = 0U;
    write_be16(frame + position, 0U);
    position += 2U;
    write_be16(frame + position, 0xABCDU);
    position += 2U;
    write_be16(frame + position, (uint16_t)(1U + (now_ms & 0x7FFFU)));
    position += 2U;
    memcpy(frame + position, payload, sizeof(payload) - 1U);
    position += sizeof(payload) - 1U;
    write_be16(frame + icmp_offset + 2U,
               checksum(frame + icmp_offset, position - icmp_offset));
    write_be16(frame + ip_offset + 10U, checksum(frame + ip_offset, 20U));
    pad_frame(frame, &position, 60U);
    *length = position;
}

static bool arp_reply_valid(
    const uint8_t *frame,
    size_t length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    return length >= 42U &&
           mac_equal(frame, peer_mac) &&
           mac_equal(frame + 6U, local_mac) &&
           frame[12U] == 0x08U && frame[13U] == 0x06U &&
           read_be16(frame + 14U) == 1U &&
           read_be16(frame + 16U) == 0x0800U &&
           frame[18U] == 6U && frame[19U] == 4U &&
           read_be16(frame + 20U) == 2U &&
           mac_equal(frame + 22U, local_mac) &&
           frame[28U] == 10U && frame[29U] == 0U &&
           frame[30U] == 2U && frame[31U] == 15U &&
           mac_equal(frame + 32U, peer_mac) &&
           frame[38U] == 10U && frame[39U] == 0U &&
           frame[40U] == 2U && frame[41U] == 2U;
}

static bool icmp_reply_valid(
    const uint8_t *frame,
    size_t length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    const size_t ip_offset = 14U;
    size_t icmp_offset;

    if (length < 51U ||
        !mac_equal(frame, peer_mac) ||
        !mac_equal(frame + 6U, local_mac) ||
        frame[12U] != 0x08U || frame[13U] != 0x00U) {
        return false;
    }
    if (frame[ip_offset] != 0x45U ||
        frame[ip_offset + 8U] == 0U ||
        frame[ip_offset + 9U] != 1U ||
        frame[ip_offset + 12U] != 10U ||
        frame[ip_offset + 13U] != 0U ||
        frame[ip_offset + 14U] != 2U ||
        frame[ip_offset + 15U] != 15U ||
        frame[ip_offset + 16U] != 10U ||
        frame[ip_offset + 17U] != 0U ||
        frame[ip_offset + 18U] != 2U ||
        frame[ip_offset + 19U] != 2U ||
        checksum(frame + ip_offset, 20U) != 0U) {
        return false;
    }
    icmp_offset = ip_offset + 20U;
    return frame[icmp_offset] == 0U &&
           frame[icmp_offset + 1U] == 0U &&
           checksum(frame + icmp_offset, 8U + 9U) == 0U &&
           frame[icmp_offset + 4U] == 0xABU &&
           frame[icmp_offset + 5U] == 0xCDU &&
           memcmp(frame + icmp_offset + 8U, "HFOC-PING", 9U) == 0;
}

static struct network_device_snapshot make_device_snapshot(void)
{
    static const uint8_t mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    struct network_device_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    snapshot.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    snapshot.vendor_id = VIRTIO_PCI_VENDOR_ID;
    snapshot.product_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    snapshot.bus_address = 0x0010U;
    snapshot.capability_mask = VIRTIO_NET_LINK_CAPABILITY_MASK;
    memcpy(snapshot.mac_address, mac, sizeof(mac));
    return snapshot;
}

static struct virtio_net_feature_profile make_profile(void)
{
    struct virtio_pci_identity identity;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;
    struct virtio_net_feature_profile profile;

    memset(&identity, 0, sizeof(identity));
    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity.device_id = VIRTIO_PCI_DEVICE_ID_NET_MODERN;
    identity.bdf.bus = 0U;
    identity.bdf.device = 2U;
    identity.bdf.function = 0U;
    identity.modern = 1U;

    offered.low = VIRTIO_NET_REQUIRED_FEATURE_LO_MASK;
    offered.high = VIRTIO_NET_VERSION_1_HIGH_BIT;

    memset(&configuration, 0, sizeof(configuration));
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    configuration.mac_address[0] = 0x52U;
    configuration.mac_address[1] = 0x54U;
    configuration.mac_address[2] = 0x00U;
    configuration.mac_address[3] = 0x12U;
    configuration.mac_address[4] = 0x34U;
    configuration.mac_address[5] = 0x56U;
    configuration.status = VIRTIO_NET_STATUS_LINK_UP;

    memset(&topology, 0, sizeof(topology));
    topology.max_virtqueue_pairs = 1U;
    topology.receive_queue_count = 1U;
    topology.transmit_queue_count = 1U;
    topology.control_queue_present = false;

    memset(&profile, 0, sizeof(profile));
    CHECK(virtio_net_feature_profile_activate(
              &profile, &identity, &offered, &configuration, &topology,
              APPLIANCE_PRODUCTION_NETWORK_GENERATION) ==
              VIRTIO_NET_FEATURE_PROFILE_OK,
          "host fixture activates virtio-net feature profile");
    return profile;
}

static bool link_read_config_status(void *context, uint32_t *status)
{
    (void)context;
    if (status == NULL) {
        return false;
    }
    *status = VIRTIO_NET_STATUS_LINK_UP;
    return true;
}

static int link_io_context;

static enum network_link_state make_link_state(
    struct virtio_net_link_state *link_state,
    const struct network_device_snapshot *device)
{
    struct virtio_net_link_readiness readiness;
    struct virtio_net_link_io io;
    struct network_link_state_record record;

    memset(&readiness, 0, sizeof(readiness));
    readiness.profile_published = true;
    readiness.queue_composition_published = true;
    readiness.interrupt_published = true;
    memset(&io, 0, sizeof(io));
    io.read_config_status = link_read_config_status;
    io.context = &link_io_context;
    memset(&record, 0, sizeof(record));
    record.version = NETWORK_LINK_STATE_VERSION;
    record.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    record.state = NETWORK_LINK_UP;
    record.generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    memset(link_state, 0, sizeof(*link_state));
    CHECK(virtio_net_link_state_activate(
              link_state, device, &readiness, &io, &record,
              APPLIANCE_PRODUCTION_NETWORK_GENERATION,
              APPLIANCE_PRODUCTION_NETWORK_GENERATION) ==
              VIRTIO_NET_LINK_OK,
          "host fixture activates production link state");
    return record.state;
}

static void reset_fixture(void)
{
    memset(&output_state, 0, sizeof(output_state));
    test_now_ms = 1000U;
    if (!hfoc_lwip_alloc_ready()) {
        CHECK(hfoc_lwip_alloc_init(),
              "host fixture starts lwIP allocator");
    }
    if (!time_initialized) {
        CHECK(hfoc_lwip_time_init(test_time_source),
              "host fixture starts lwIP monotonic time");
        time_initialized = true;
    }
}

static void test_init_and_honest_unavailable(void)
{
    struct appliance_production_network network;

    memset(&network, 0, sizeof(network));
    CHECK(appliance_production_network_init(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "production network init");
    CHECK(!appliance_production_network_bound(&network) &&
              appliance_production_network_readiness(&network) ==
                  APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE,
          "unbound production network is honestly UNAVAILABLE");
    CHECK(appliance_production_network_device(&network) == NULL &&
              appliance_production_network_mac(&network) == NULL,
          "unbound production network publishes no identity");
    CHECK(strcmp(appliance_production_network_readiness_string(
                     APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE),
                 "UNAVAILABLE") == 0 &&
              strcmp(appliance_production_network_readiness_string(
                         APPLIANCE_PRODUCTION_NETWORK_DEGRADED),
                     "DEGRADED") == 0 &&
              strcmp(appliance_production_network_readiness_string(
                         APPLIANCE_PRODUCTION_NETWORK_READY),
                     "READY") == 0,
          "production network readiness strings");
    CHECK(appliance_production_network_bind(&network, NULL) ==
              APPLIANCE_PRODUCTION_NETWORK_NULL,
          "null bind input is rejected");
    CHECK(appliance_production_network_stop(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "stop on unbound network is a no-op");
    CHECK(strcmp(appliance_production_network_status_string(
                     APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE),
                 "NO_DEVICE") == 0,
          "production network status strings");
}

static void test_bind_and_packet_path(void)
{
    struct appliance_production_network network;
    struct network_device_snapshot device = make_device_snapshot();
    struct virtio_net_feature_profile profile = make_profile();
    struct virtio_net_link_state link_state;
    struct appliance_production_network_bind_inputs inputs;
    struct network_ipv4_config fixture_config;
    struct network_ipv4_port_policy fixture_policy;
    uint8_t frame[TEST_FRAME_MAX];
    size_t length = 0U;
    static const uint8_t peer_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

    reset_fixture();
    (void)make_link_state(&link_state, &device);
    memset(&network, 0, sizeof(network));
    CHECK(appliance_production_network_init(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "production network init before bind");
    memset(&inputs, 0, sizeof(inputs));
    inputs.link_state = &link_state;
    inputs.profile = &profile;
    /*
     * ITEM-416: this proof states the address it uses.  It used to pass no
     * configuration and rely on the production bind filling in the QEMU
     * fixture's address - which is exactly the leak the item closed, and it is
     * why the frames below are built for 10.0.2.15.  The fixture states it
     * here, and the product's rule for a caller with no configuration is proven
     * by `test_no_config_means_no_address`.
     */
    CHECK(network_ipv4_profile_qemu_fill(
              &fixture_config, &fixture_policy) ==
              NETWORK_IPV4_PROFILE_OK,
          "the packet-path proof states its own address");
    inputs.ipv4_config = &fixture_config;
    inputs.port_policy = &fixture_policy;
    inputs.output.transmit = test_output_transmit;
    inputs.output.context = &output_state;
    CHECK(appliance_production_network_bind(&network, &inputs) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "production network binds a live NIC");
    CHECK(appliance_production_network_bound(&network) &&
              appliance_production_network_ready(&network) &&
              appliance_production_network_readiness(&network) ==
                  APPLIANCE_PRODUCTION_NETWORK_READY,
          "bound production network is READY");
    CHECK(appliance_production_network_device(&network) != NULL &&
              memcmp(appliance_production_network_mac(&network),
                     device.mac_address,
                     NETWORK_DEVICE_MAC_LENGTH) == 0,
          "production network publishes device MAC");
    CHECK(appliance_production_network_input(&network, NULL, 0U,
                                             test_now_ms) ==
              APPLIANCE_PRODUCTION_NETWORK_INPUT_UNAVAILABLE,
          "null frame is unavailable");

    build_arp_request(frame, &length, peer_mac);
    CHECK(appliance_production_network_input(
              &network, frame, length, test_now_ms) ==
              APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED,
          "ARP request accepted by production lwIP path");
    CHECK(output_state.count == 1U &&
              arp_reply_valid(output_state.frames[0],
                              output_state.lengths[0],
                              device.mac_address, peer_mac),
          "production path emits exact ARP reply");

    build_icmp_echo(frame, &length, device.mac_address, peer_mac,
                    test_now_ms);
    CHECK(appliance_production_network_input(
              &network, frame, length, test_now_ms) ==
              APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED,
          "ICMP echo accepted by production lwIP path");
    CHECK(output_state.count == 2U &&
              icmp_reply_valid(output_state.frames[1],
                               output_state.lengths[1],
                               device.mac_address, peer_mac),
          "production path emits exact ICMP echo reply");

    CHECK(appliance_production_network_input(
              &network, frame, 4U, test_now_ms) ==
              APPLIANCE_PRODUCTION_NETWORK_INPUT_REJECTED,
          "short frame is rejected by production path");
    CHECK(appliance_production_network_stop(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK &&
              !appliance_production_network_bound(&network) &&
              appliance_production_network_readiness(&network) ==
                  APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE,
          "stop returns production network to UNAVAILABLE");
}

static void test_rebind_is_rejected(void)
{
    struct appliance_production_network network;
    struct network_device_snapshot device = make_device_snapshot();
    struct virtio_net_feature_profile profile = make_profile();
    struct virtio_net_link_state link_state;
    struct appliance_production_network_bind_inputs inputs;

    reset_fixture();
    (void)make_link_state(&link_state, &device);
    memset(&network, 0, sizeof(network));
    CHECK(appliance_production_network_init(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "rebind fixture init");
    memset(&inputs, 0, sizeof(inputs));
    inputs.link_state = &link_state;
    inputs.profile = &profile;
    inputs.output.transmit = test_output_transmit;
    inputs.output.context = &output_state;
    CHECK(appliance_production_network_bind(&network, &inputs) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "first bind succeeds");
    CHECK(appliance_production_network_bind(&network, &inputs) ==
              APPLIANCE_PRODUCTION_NETWORK_BAD_STATE,
          "second bind without stop is rejected");
}


/*
 * ITEM-416: a caller with no configuration gets no address.
 *
 * The product binds the hardware NIC with no configuration at all, and that
 * path used to install the QEMU fixture's address (`10.0.2.15/24`): the
 * acceptance host published its listener on it while its own network was
 * handing out `192.0.2.0/24` leases, and the panel could not be reached
 * although ICMP to the machine worked.  A machine with no configuration must
 * not answer for the fixture's address - it has none until the mode it was
 * configured for obtains one.
 */
static void test_no_config_means_no_address(void)
{
    struct appliance_production_network network;
    struct network_device_snapshot device = make_device_snapshot();
    struct virtio_net_feature_profile profile = make_profile();
    struct virtio_net_link_state link_state;
    struct appliance_production_network_bind_inputs inputs;
    uint8_t frame[TEST_FRAME_MAX];
    size_t length = 0U;
    static const uint8_t peer_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};

    reset_fixture();
    (void)make_link_state(&link_state, &device);
    memset(&network, 0, sizeof(network));
    CHECK(appliance_production_network_init(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "no-config init");
    memset(&inputs, 0, sizeof(inputs));
    inputs.link_state = &link_state;
    inputs.profile = &profile;
    inputs.ipv4_config = NULL;
    inputs.port_policy = NULL;
    inputs.output.transmit = test_output_transmit;
    inputs.output.context = &output_state;
    {
        const enum appliance_production_network_status bind_status =
            appliance_production_network_bind(&network, &inputs);

        if (bind_status != APPLIANCE_PRODUCTION_NETWORK_OK) {
            fprintf(stderr, "DEBUG bind status=%s readiness=%s\n",
                    appliance_production_network_status_string(bind_status),
                    appliance_production_network_readiness_string(
                        appliance_production_network_readiness(&network)));
        }
        CHECK(bind_status == APPLIANCE_PRODUCTION_NETWORK_OK,
              "a NIC binds with no configuration at all");
    }
    CHECK(inputs.ipv4_config == NULL && output_state.count == 0U,
          "binding with no configuration emits nothing");

    build_arp_request(frame, &length, peer_mac);
    CHECK(appliance_production_network_input(
              &network, frame, length, test_now_ms) ==
              APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED,
          "an ARP request for the fixture's address is still accepted");
    CHECK(output_state.count == 0U,
          "and the machine answers it for nobody: it holds no address");
    CHECK(appliance_production_network_stop(&network) ==
              APPLIANCE_PRODUCTION_NETWORK_OK,
          "no-config stop");
}

int main(void)
{
    printf("Appliance production network host tests\n");
    test_init_and_honest_unavailable();
    test_bind_and_packet_path();
    test_no_config_means_no_address();
    test_rebind_is_rejected();
    if (failures != 0) {
        fprintf(stderr,
                "Appliance production network host tests: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("Appliance production network host tests: ALL OK\n");
    return 0;
}

#else
typedef int appliance_production_network_host_translation_unit_guard;
#endif
