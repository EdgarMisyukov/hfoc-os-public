#include "appliance_network_wire.h"

#include "appliance_dhcp_client.h"
#include "appliance_dns_resolver.h"
#include "appliance_network_config.h"
#include "appliance_network_lifecycle.h"
#include "ipv4_config.h"
#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_NETWORK_WIRE_HOST_TEST

#define WIRE_TEST_CAPTURE_MAX 16U

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

struct wire_capture {
    unsigned int count;
    /*
     * ITEM-428: a transmit path that refuses every frame, which is what the
     * 2026-09-16 hardware run's full transmit ring looked like from the wire.
     */
    bool refuse;
    unsigned int refused;
    size_t lengths[WIRE_TEST_CAPTURE_MAX];
    uint8_t frames[WIRE_TEST_CAPTURE_MAX][APPLIANCE_NETWORK_WIRE_FRAME_MAX];
};

struct wire_link_table {
    unsigned int resolve_calls;
    uint32_t address;
    uint8_t mac[NETWORK_DEVICE_MAC_LENGTH];
};

struct wire_activate_record {
    unsigned int activate_calls;
    unsigned int deactivate_calls;
    struct network_ipv4_config config;
    bool published;
};

static struct wire_capture capture;
static struct wire_link_table link_table;
static struct wire_activate_record activate_record;

static void reset_test_state(void)
{
    memset(&capture, 0, sizeof(capture));
    memset(&link_table, 0, sizeof(link_table));
    memset(&activate_record, 0, sizeof(activate_record));
}

static bool wire_test_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    struct wire_capture *sink = context;

    if (sink == NULL || frame == NULL || length == 0U ||
        length > APPLIANCE_NETWORK_WIRE_FRAME_MAX ||
        sink->count >= WIRE_TEST_CAPTURE_MAX) {
        return false;
    }
    if (sink->refuse) {
        ++sink->refused;
        return false;
    }
    sink->lengths[sink->count] = length;
    memcpy(sink->frames[sink->count], frame, length);
    ++sink->count;
    return true;
}

static bool wire_test_resolve(
    void *context,
    const struct network_ipv4_address *next_hop,
    uint8_t mac_out[NETWORK_DEVICE_MAC_LENGTH])
{
    struct wire_link_table *table = context;
    uint32_t address = ((uint32_t)next_hop->octets[0] << 24U) |
                       ((uint32_t)next_hop->octets[1] << 16U) |
                       ((uint32_t)next_hop->octets[2] << 8U) |
                       (uint32_t)next_hop->octets[3];

    ++table->resolve_calls;
    if (table->address == 0U) {
        return false;
    }
    if (address != table->address) {
        return false;
    }
    memcpy(mac_out, table->mac, NETWORK_DEVICE_MAC_LENGTH);
    return true;
}

static bool wire_test_activate(
    void *context,
    const struct network_ipv4_config *config)
{
    struct wire_activate_record *record = context;

    ++record->activate_calls;
    record->config = *config;
    record->published = true;
    return true;
}

static bool wire_test_deactivate(void *context)
{
    struct wire_activate_record *record = context;

    ++record->deactivate_calls;
    record->published = false;
    return true;
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

static struct appliance_network_config make_dhcp_desired(void)
{
    struct appliance_network_config desired;

    memset(&desired, 0, sizeof(desired));
    (void)appliance_network_config_publish_dhcp(
        &desired, "eth0", 4U);
    return desired;
}

static struct network_link_state_record make_link(void)
{
    struct network_link_state_record link;

    memset(&link, 0, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = 1U;
    link.state = NETWORK_LINK_UP;
    link.generation = 1U;
    return link;
}

static struct appliance_network_wire_io make_io(void)
{
    struct appliance_network_wire_io io;

    memset(&io, 0, sizeof(io));
    io.transmit = wire_test_transmit;
    io.transmit_context = &capture;
    io.link.resolve = wire_test_resolve;
    io.link.context = &link_table;
    io.activate.activate_ipv4 = wire_test_activate;
    io.activate.deactivate_ipv4 = wire_test_deactivate;
    io.activate.context = &activate_record;
    return io;
}

/* --- frame helpers ---------------------------------------------------- */

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value >> 8U);
    out[1] = (uint8_t)(value & 0xFFU);
}

static void put_u32(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)(value >> 24U);
    out[1] = (uint8_t)((value >> 16U) & 0xFFU);
    out[2] = (uint8_t)((value >> 8U) & 0xFFU);
    out[3] = (uint8_t)(value & 0xFFU);
}

static uint16_t read_u16(const uint8_t *in)
{
    return (uint16_t)(((uint16_t)in[0] << 8U) | in[1]);
}

static uint32_t read_u32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24U) |
           ((uint32_t)in[1] << 16U) |
           ((uint32_t)in[2] << 8U) |
           (uint32_t)in[3];
}

static uint32_t ip_from_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d)
{
    return ((uint32_t)a << 24U) | ((uint32_t)b << 16U) |
           ((uint32_t)c << 8U) | (uint32_t)d;
}

static uint16_t ip_checksum(const uint8_t *data, size_t length)
{
    uint32_t sum = 0U;
    size_t index;

    for (index = 0U; index + 1U < length; index += 2U) {
        sum += ((uint32_t)data[index] << 8U) | data[index + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static size_t build_udp_frame(
    uint8_t *frame,
    const uint8_t src_mac[NETWORK_DEVICE_MAC_LENGTH],
    const uint8_t dst_mac[NETWORK_DEVICE_MAC_LENGTH],
    uint32_t src_ip,
    uint32_t dst_ip,
    uint16_t src_port,
    uint16_t dst_port,
    const uint8_t *payload,
    size_t payload_length)
{
    const size_t ip_length =
        APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH +
        APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH + payload_length;
    uint8_t *ip = frame + APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH;
    uint8_t *udp = ip + APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH;

    memcpy(frame, dst_mac, NETWORK_DEVICE_MAC_LENGTH);
    memcpy(frame + 6U, src_mac, NETWORK_DEVICE_MAC_LENGTH);
    put_u16(frame + 12U, 0x0800U);

    memset(ip, 0, APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH);
    ip[0] = 0x45U;
    ip[1] = 0x00U;
    put_u16(ip + 2U, (uint16_t)ip_length);
    put_u16(ip + 4U, 0x1234U);
    put_u16(ip + 6U, 0x0000U);
    ip[8] = 64U;
    ip[9] = 17U;
    put_u32(ip + 12U, src_ip);
    put_u32(ip + 16U, dst_ip);
    put_u16(ip + 10U, ip_checksum(ip,
        APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH));

    put_u16(udp, src_port);
    put_u16(udp + 2U, dst_port);
    put_u16(udp + 4U,
        (uint16_t)(APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH +
                   payload_length));
    put_u16(udp + 6U, 0U);
    memcpy(udp + APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH,
           payload, payload_length);
    return APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH + ip_length;
}

static size_t build_dhcp_option(
    uint8_t *options,
    size_t position,
    uint8_t code,
    const uint8_t *value,
    size_t value_length)
{
    options[position] = code;
    options[position + 1U] = (uint8_t)value_length;
    memcpy(options + position + 2U, value, value_length);
    return position + 2U + value_length;
}

static size_t build_dhcp_payload(
    uint8_t *payload,
    uint8_t op,
    uint8_t message_type,
    uint32_t xid,
    const uint8_t client_mac[NETWORK_DEVICE_MAC_LENGTH],
    uint32_t yiaddr,
    uint32_t subnet_mask,
    uint32_t router,
    uint32_t dns,
    uint32_t server_id,
    uint32_t lease_seconds)
{
    size_t position = 240U;

    memset(payload, 0, APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN);
    payload[0] = op;
    payload[1] = 1U;
    payload[2] = 6U;
    payload[3] = 0U;
    put_u32(payload + 4U, xid);
    payload[10] = 0x80U;
    payload[11] = 0x00U;
    put_u32(payload + 16U, yiaddr);
    memcpy(payload + 28U, client_mac, NETWORK_DEVICE_MAC_LENGTH);
    put_u32(payload + 236U, APPLIANCE_NETWORK_WIRE_DHCP_MAGIC);

    {
        uint8_t value[4];

        value[0] = message_type;
        position = build_dhcp_option(payload, position, 53U, value, 1U);
    }
    if (subnet_mask != 0U) {
        uint8_t value[4];

        put_u32(value, subnet_mask);
        position = build_dhcp_option(payload, position, 1U, value, 4U);
    }
    if (router != 0U) {
        uint8_t value[4];

        put_u32(value, router);
        position = build_dhcp_option(payload, position, 3U, value, 4U);
    }
    if (dns != 0U) {
        uint8_t value[4];

        put_u32(value, dns);
        position = build_dhcp_option(payload, position, 6U, value, 4U);
    }
    if (server_id != 0U) {
        uint8_t value[4];

        put_u32(value, server_id);
        position = build_dhcp_option(payload, position, 54U, value, 4U);
    }
    /*
     * ITEM-408: option 51 is what the renewal schedule is computed from, and
     * a fixture that omits it proves the appliance arms no renewal at all
     * instead of inventing one.
     */
    if (lease_seconds != 0U) {
        uint8_t value[4];

        put_u32(value, lease_seconds);
        position = build_dhcp_option(payload, position, 51U, value, 4U);
    }
    payload[position] = 255U;
    ++position;
    if (position < APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN) {
        position = APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN;
    }
    return position;
}

static size_t build_dhcp_reply_lease(
    uint8_t *frame,
    uint8_t message_type,
    uint32_t yiaddr,
    uint32_t server_id,
    uint32_t lease_seconds)
{
    static const uint8_t server_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    static const uint8_t broadcast[NETWORK_DEVICE_MAC_LENGTH] =
        {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    static const uint8_t client_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    uint8_t payload[APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN];
    size_t payload_length = build_dhcp_payload(
        payload, 2U, message_type, APPLIANCE_NETWORK_WIRE_DHCP_XID,
        client_mac, yiaddr, ip_from_octets(255U, 255U, 255U, 0U),
        ip_from_octets(10U, 0U, 2U, 2U),
        ip_from_octets(10U, 0U, 2U, 3U), server_id, lease_seconds);

    return build_udp_frame(
        frame, server_mac, broadcast,
        ip_from_octets(10U, 0U, 2U, 2U),
        ip_from_octets(255U, 255U, 255U, 255U),
        APPLIANCE_NETWORK_WIRE_DHCP_SERVER_PORT,
        APPLIANCE_NETWORK_WIRE_DHCP_CLIENT_PORT,
        payload, payload_length);
}

/*
 * The fixtures that never carry a lease time keep the shorter spelling: an
 * acknowledgement without option 51 arms no renewal, which is itself part of
 * what the renewal proof has to show.
 */
static size_t build_dhcp_reply(
    uint8_t *frame,
    uint8_t message_type,
    uint32_t yiaddr,
    uint32_t server_id)
{
    return build_dhcp_reply_lease(
        frame, message_type, yiaddr, server_id, 0U);
}

static size_t build_dns_payload(
    uint8_t *payload,
    uint16_t transaction_id,
    uint16_t flags,
    bool with_answer)
{
    static const uint8_t labels[] = {
        3U, 'a', 'p', 'i', 4U, 'h', 'f', 'o', 'c', 4U, 't', 'e', 's', 't',
        0U
    };
    size_t position = 12U;

    memset(payload, 0, 512U);
    put_u16(payload, transaction_id);
    put_u16(payload + 2U, flags);
    put_u16(payload + 4U, 1U);
    put_u16(payload + 6U, with_answer ? 1U : 0U);
    memcpy(payload + position, labels, sizeof(labels));
    position += sizeof(labels);
    put_u16(payload + position, 1U);
    put_u16(payload + position + 2U, 1U);
    position += 4U;
    if (with_answer) {
        put_u16(payload + position, 0xC00CU);
        put_u16(payload + position + 2U, 1U);
        put_u16(payload + position + 4U, 1U);
        put_u32(payload + position + 6U, 60U);
        put_u16(payload + position + 10U, 4U);
        payload[position + 12U] = 10U;
        payload[position + 13U] = 0U;
        payload[position + 14U] = 2U;
        payload[position + 15U] = 15U;
        position += 16U;
    }
    return position;
}

static size_t build_dns_response(
    uint8_t *frame,
    uint16_t transaction_id)
{
    static const uint8_t client_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    static const uint8_t server_mac[NETWORK_DEVICE_MAC_LENGTH] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x03U};
    uint8_t payload[512];
    size_t payload_length =
        build_dns_payload(payload, transaction_id, 0x8180U, true);

    return build_udp_frame(
        frame, server_mac, client_mac,
        ip_from_octets(10U, 0U, 2U, 3U),
        ip_from_octets(10U, 0U, 2U, 15U),
        APPLIANCE_NETWORK_WIRE_DNS_PORT,
        APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT,
        payload, payload_length);
}

/* --- assertions ------------------------------------------------------- */

static bool frame_is_udp(
    const uint8_t *frame,
    size_t length,
    uint16_t src_port,
    uint16_t dst_port)
{
    if (length < APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH +
                     APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH +
                     APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH) {
        return false;
    }
    if (read_u16(frame + 12U) != 0x0800U) {
        return false;
    }
    {
        const uint8_t *ip = frame +
            APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH;
        const uint8_t *udp = ip +
            APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH;

        return ip[9] == 17U &&
               read_u16(udp) == src_port &&
               read_u16(udp + 2U) == dst_port;
    }
}

static int find_dhcp_message_type(
    const uint8_t *frame,
    size_t length)
{
    const uint8_t *ip = frame + APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH;
    const uint8_t *udp = ip + APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH;
    const uint8_t *payload = udp +
        APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH;
    size_t offset = 240U;

    if (length < 240U ||
        read_u32(payload + 236U) != APPLIANCE_NETWORK_WIRE_DHCP_MAGIC) {
        return -1;
    }
    while (offset < length) {
        const uint8_t code = payload[offset];
        uint8_t option_length;

        if (code == 255U) {
            return -1;
        }
        if (code == 0U) {
            ++offset;
            continue;
        }
        option_length = payload[offset + 1U];
        if (code == 53U && option_length == 1U) {
            return payload[offset + 2U];
        }
        offset += 2U + option_length;
    }
    return -1;
}

static bool dhcp_option_u32(
    const uint8_t *frame,
    size_t length,
    uint8_t code,
    uint32_t *value)
{
    const uint8_t *ip = frame + APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH;
    const uint8_t *udp = ip + APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH;
    const uint8_t *payload = udp +
        APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH;
    size_t offset = 240U;

    while (offset + 1U < length) {
        uint8_t option_code = payload[offset];
        uint8_t option_length;

        if (option_code == 255U) {
            return false;
        }
        if (option_code == 0U) {
            ++offset;
            continue;
        }
        option_length = payload[offset + 1U];
        if (offset + 2U + option_length > length) {
            return false;
        }
        if (option_code == code && option_length == 4U) {
            *value = read_u32(payload + offset + 2U);
            return true;
        }
        offset += 2U + option_length;
    }
    return false;
}

static void test_dhcp_handshake(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    size_t frame_length;
    uint32_t value = 0U;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "wire init accepts a validated DHCP desired configuration");
    CHECK(appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "wire start transitions from prepared");
    CHECK(appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "wire link up starts DHCP addressing");
    CHECK(capture.count == 1U,
          "DHCP link up transmits exactly one discover frame");
    CHECK(capture.count == 1U &&
              frame_is_udp(capture.frames[0], capture.lengths[0],
                  APPLIANCE_NETWORK_WIRE_DHCP_CLIENT_PORT,
                  APPLIANCE_NETWORK_WIRE_DHCP_SERVER_PORT),
          "DHCP discover uses the client/server UDP ports");
    CHECK(capture.count == 1U &&
              read_u16(capture.frames[0]) == 0xFFFFU &&
              find_dhcp_message_type(
                  capture.frames[0], capture.lengths[0]) == 1,
          "DHCP discover is a broadcast DHCPDISCOVER");

    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    CHECK(appliance_network_wire_input(
              &wire, frame, frame_length, 10U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED,
          "DHCP offer frame is consumed by the wire owner");
    CHECK(capture.count == 2U &&
              find_dhcp_message_type(
                  capture.frames[1], capture.lengths[1]) == 3,
          "DHCP offer triggers a DHCPREQUEST");
    CHECK(capture.count == 2U &&
              dhcp_option_u32(
                  capture.frames[1], capture.lengths[1], 50U, &value) &&
              value == ip_from_octets(10U, 0U, 2U, 15U),
          "DHCP request carries the offered address");
    CHECK(capture.count == 2U &&
              dhcp_option_u32(
                  capture.frames[1], capture.lengths[1], 54U, &value) &&
              value == ip_from_octets(10U, 0U, 2U, 2U),
          "DHCP request carries the server identifier");

    frame_length = build_dhcp_reply(
        frame, 5U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    CHECK(appliance_network_wire_input(
              &wire, frame, frame_length, 20U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED,
          "DHCP acknowledgement frame is consumed");
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "DHCP acknowledgement activates the lifecycle");
    CHECK(activate_record.activate_calls == 1U &&
              activate_record.published,
          "DHCP acknowledgement publishes the active IPv4 configuration");
    CHECK(appliance_network_wire_active_config(&wire) != NULL &&
              appliance_network_wire_active_config(&wire)->address.octets[3] ==
                  15U &&
              appliance_network_wire_active_config(&wire)->prefix_length ==
                  24U &&
              appliance_network_wire_active_config(&wire)->dns.octets[3] ==
                  3U,
          "published lease carries address, prefix, and DNS");

    link_table.address = ip_from_octets(10U, 0U, 2U, 3U);
    memcpy(link_table.mac, "\x02\x00\x00\x00\x00\x03",
           NETWORK_DEVICE_MAC_LENGTH);
    CHECK(appliance_network_wire_resolve(
              &wire, APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME,
              APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME_LENGTH, 30U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "DNS resolution starts once the lease is active");
    CHECK(capture.count == 3U &&
              frame_is_udp(capture.frames[2], capture.lengths[2],
                  APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT,
                  APPLIANCE_NETWORK_WIRE_DNS_PORT),
          "DNS query uses the resolver source and server ports");
    CHECK(capture.count == 3U &&
              read_u16(capture.frames[2] +
                  APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH +
                  APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH +
                  APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH) ==
                  wire.dns_transaction_id,
          "DNS query carries the tracked transaction id");

    frame_length = build_dns_response(frame, wire.dns_transaction_id);
    CHECK(appliance_network_wire_input(
              &wire, frame, frame_length, 40U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED,
          "DNS response frame is consumed");
    CHECK(appliance_network_wire_dns_answer(&wire) != NULL &&
              appliance_network_wire_dns_answer(&wire)->octets[0] == 10U &&
              appliance_network_wire_dns_answer(&wire)->octets[3] == 15U,
          "DNS response publishes the A record answer");

    CHECK(appliance_network_wire_stop(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "wire stop releases the lifecycle");
    CHECK(alloc_attempts == 0U && free_attempts == 0U,
          "wire codec performs no dynamic allocation");
}

static void test_dhcp_timeout(void)
{
    size_t index;
    uint32_t now_ms = APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "DHCP timeout fixture starts discovering");
    appliance_network_wire_tick(&wire, APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS);
    CHECK(wire.retry_count == 1U,
          "first DHCP timeout retransmits the discover");
    /*
     * ITEM-416: the budget is what the client declares, not what this test
     * remembers.  The switch port that is still leaving spanning-tree blocking
     * is the machine this budget exists for, so the test drains the declared
     * number of attempts instead of three.
     */
    {
        uint32_t attempt = 0U;
        uint32_t budget = 0U;

        CHECK(appliance_network_wire_dhcp_attempts(&wire, &attempt, &budget) &&
                  budget == APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS &&
                  budget > 1U,
              "the wire publishes the acquisition budget it is spending");
        /* The wire's clock is absolute: each tick is the time it is told. */
        for (index = 0U; index < budget + 2U; ++index) {
            now_ms += APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
            appliance_network_wire_tick(&wire, now_ms);
            if (appliance_network_wire_state(&wire) !=
                APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING) {
                break;
            }
        }
        (void)appliance_network_wire_dhcp_attempts(&wire, &attempt, &budget);
        CHECK(appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED,
              "bounded DHCP timeouts fail closed without a lease");
        CHECK(appliance_network_wire_active_config(&wire) == NULL,
              "failed DHCP does not publish a lease");
        CHECK(attempt > 0U && attempt <= budget,
              "the wire stops asking when the budget is spent");
        CHECK(capture.count >= 2U && capture.count <= budget,
              "DHCP retransmission is bounded by the attempt budget");
    }
}

/*
 * ITEM-428: a transmit path that refuses every DHCP frame.
 *
 * The 2026-09-16 hardware run's boot log ended on
 * `[PRODUCTION-DHCP] state=ADDRESSING lease=NONE reason=NONE attempt=11/40`
 * and nothing else: the cycle had stopped asking and nothing said why.  A
 * refused frame must never be silent - it is counted, it names the transmit
 * path as the reason the cycle is not moving, and the cycle keeps its bounded
 * cadence so the record stream continues and the budget still runs out into a
 * stated verdict.
 */
static void test_dhcp_transport_refused(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    struct appliance_network_wire_dhcp_counters counters;
    uint32_t now_ms = 0U;
    uint32_t index;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "the refusing fixture starts discovering");
    CHECK(appliance_network_wire_dhcp_counters(&wire, &counters) &&
              counters.discovers_sent == 1U &&
              counters.send_refusals == 0U &&
              counters.requests_sent == 0U &&
              counters.offers_received == 0U &&
              counters.acks_received == 0U,
          "an accepted discover is counted as sent, not refused");

    /*
     * The transmit path refuses from here on.  The deadline the accepted
     * discover armed still expires, so the lifecycle runs its timeout and
     * tries to send: that refusal is the one the wire must publish.
     */
    capture.refuse = true;
    now_ms = APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
    appliance_network_wire_tick(&wire, now_ms);
    CHECK(capture.refused >= 1U,
          "the refused attempt really was refused by the transport");
    CHECK(appliance_network_wire_dhcp_counters(&wire, &counters) &&
              counters.send_refusals == capture.refused &&
              counters.discovers_sent == 1U,
          "the wire counts the frames the transmit path refused");
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING,
          "a refused transmit does not end the addressing cycle");
    CHECK(appliance_network_wire_lifecycle(&wire) != NULL &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT,
          "a refused transmit is published as the reason the cycle is parked");

    /*
     * The cycle keeps its one-second cadence with no frame on the wire, so the
     * attempt counter advances and the record stream continues; the bounded
     * budget is what ends it, not a silent park.
     */
    {
        uint32_t attempt = 0U;
        uint32_t budget = 0U;
        uint32_t refusals_before = capture.refused;

        for (index = 0U; index < APPLIANCE_DHCP_CLIENT_MAX_ATTEMPTS + 2U;
             ++index) {
            now_ms += APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
            appliance_network_wire_tick(&wire, now_ms);
            if (appliance_network_wire_state(&wire) !=
                APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING) {
                break;
            }
        }
        CHECK(capture.refused > refusals_before,
              "every cadence tick tried the wire again");
        CHECK(appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_FAILED,
              "the bounded budget ends a refused acquisition in a verdict");
        CHECK(appliance_network_wire_lifecycle(&wire) != NULL &&
                  appliance_network_lifecycle_reason(
                      appliance_network_wire_lifecycle(&wire)) ==
                      APPLIANCE_NETWORK_LIFECYCLE_REASON_DHCP_TRANSPORT,
              "the verdict names the refused transmit, not a generic timeout");
        CHECK(appliance_network_wire_dhcp_attempts(
                  &wire, &attempt, &budget) == false ||
                  attempt <= budget,
              "the refused acquisition never exceeds its budget");
        CHECK(appliance_network_wire_active_config(&wire) == NULL,
              "a refused acquisition never publishes a fabricated lease");
    }
}

static void test_dns_timeout(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    size_t frame_length;
    unsigned int queries;
    uint32_t probe_exhausted_ms;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "DNS timeout fixture starts DHCP addressing");
    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(&wire, frame, frame_length, 1U);
    frame_length = build_dhcp_reply(
        frame, 5U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(&wire, frame, frame_length, 2U);
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "DNS timeout fixture acquires a lease");

    link_table.address = ip_from_octets(10U, 0U, 2U, 3U);
    memcpy(link_table.mac, "\x02\x00\x00\x00\x00\x03",
           NETWORK_DEVICE_MAC_LENGTH);
    CHECK(appliance_network_wire_resolve(
              &wire, APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME,
              APPLIANCE_NETWORK_WIRE_PROBE_HOSTNAME_LENGTH, 3U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "DNS timeout fixture starts a query");
    /*
     * ITEM-408: the bounded attempt budget of the probe runs out while the
     * lease it observed is live.  The lease, the applied address and the
     * serving path survive; the probe is the only thing that failed, and the
     * record says so.
     */
    probe_exhausted_ms =
        3U + 2U * APPLIANCE_NETWORK_WIRE_DNS_TIMEOUT_MS;
    appliance_network_wire_tick(
        &wire, 3U + APPLIANCE_NETWORK_WIRE_DNS_TIMEOUT_MS);
    appliance_network_wire_tick(&wire, probe_exhausted_ms);
    CHECK(appliance_network_wire_dns(&wire) != NULL &&
              appliance_network_wire_dns(&wire)->state ==
                  APPLIANCE_DNS_RESOLVER_STATE_FAILED,
          "bounded DNS timeouts exhaust the probe");
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_wire_active_config(&wire) != NULL &&
              activate_record.published &&
              activate_record.deactivate_calls == 0U,
          "DNS exhaustion keeps the live address applied");
    CHECK(appliance_network_wire_lifecycle(&wire) != NULL &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_DNS_PROBE_TIMEOUT &&
              appliance_network_lifecycle_hold_count(
                  appliance_network_wire_lifecycle(&wire)) == 1U,
          "DNS exhaustion publishes the probe reason");
    CHECK(appliance_network_wire_dns_answer(&wire) == NULL,
          "exhausted DNS publishes no answer");
    queries = capture.count;
    /*
     * The probe is retried on its own longer interval: the address is never
     * part of that decision, and the retry is what turns the record back into
     * a resolved one.
     */
    appliance_network_wire_tick(
        &wire,
        probe_exhausted_ms + APPLIANCE_NETWORK_WIRE_DNS_PROBE_RETRY_MS - 1U);
    CHECK(capture.count == queries,
          "the DNS probe retry is paced, not immediate");
    appliance_network_wire_tick(
        &wire,
        probe_exhausted_ms + APPLIANCE_NETWORK_WIRE_DNS_PROBE_RETRY_MS);
    CHECK(capture.count == queries + 1U &&
              frame_is_udp(
                  capture.frames[queries], capture.lengths[queries],
                  APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT,
                  APPLIANCE_NETWORK_WIRE_DNS_PORT),
          "the DNS probe is retried after its own interval");
    frame_length = build_dns_response(frame, wire.dns_transaction_id);
    CHECK(appliance_network_wire_input(
              &wire, frame, frame_length,
              probe_exhausted_ms +
                  APPLIANCE_NETWORK_WIRE_DNS_PROBE_RETRY_MS + 1U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED &&
              appliance_network_wire_dns_answer(&wire) != NULL &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE,
          "a retried probe resolves and clears its own reason");
}

/*
 * ITEM-408: the renewal schedule.
 *
 * The acknowledgement carries the lease time, the wire arms T1 from it on the
 * monotonic clock the tick is driven with, and the renewal that follows is a
 * transition on the live address: when the server answers nothing at all, the
 * appliance keeps the address, keeps it applied, and re-acquires instead of
 * tearing the configuration down.
 */
static void test_lease_time_and_renewal(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    size_t frame_length;
    uint32_t lease_seconds = 0U;
    uint32_t requested = 0U;
    uint32_t renew_at;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              !appliance_network_wire_lease_time(&wire, &lease_seconds),
          "renewal fixture starts without a lease time");
    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(&wire, frame, frame_length, 1U);
    frame_length = build_dhcp_reply_lease(
        frame, 5U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U), 20U);
    (void)appliance_network_wire_input(&wire, frame, frame_length, 2U);
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              appliance_network_wire_lease_time(&wire, &lease_seconds) &&
              lease_seconds == 20U,
          "the acknowledged lease time is published");
    renew_at = 2U + (20U * 1000U) / 2U;
    appliance_network_wire_tick(&wire, renew_at - 1U);
    CHECK(capture.count == 2U,
          "the renewal waits for half the acknowledged lease");
    appliance_network_wire_tick(&wire, renew_at);
    CHECK(capture.count == 3U &&
              find_dhcp_message_type(
                  capture.frames[2], capture.lengths[2]) == 3 &&
              dhcp_option_u32(
                  capture.frames[2], capture.lengths[2], 50U, &requested) &&
              requested == ip_from_octets(10U, 0U, 2U, 15U),
          "the renewal requests the held address from the server");
    CHECK(appliance_network_wire_dhcp(&wire) != NULL &&
              appliance_network_wire_dhcp(&wire)->state ==
                  APPLIANCE_DHCP_CLIENT_STATE_RENEWING,
          "the renewal enters RENEWING");

    /*
     * The server never answers: the bounded renew/rebind cycle ends without
     * an acknowledgement, and the appliance keeps serving.
     */
    appliance_network_wire_tick(
        &wire, renew_at + APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS);
    CHECK(capture.count == 4U &&
              appliance_network_wire_dhcp(&wire) != NULL &&
              appliance_network_wire_dhcp(&wire)->state ==
                  APPLIANCE_DHCP_CLIENT_STATE_REBINDING,
          "a missed renewal falls back to rebinding");
    appliance_network_wire_tick(
        &wire, renew_at + 2U * APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS);
    CHECK(appliance_network_wire_active_config(&wire) != NULL &&
              appliance_network_wire_active_config(&wire)
                      ->address.octets[3] == 15U &&
              activate_record.published &&
              activate_record.deactivate_calls == 0U,
          "an unanswered renewal keeps the address applied");
    CHECK(appliance_network_wire_lifecycle(&wire) != NULL &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_REBIND_TIMEOUT &&
              appliance_network_lifecycle_hold_count(
                  appliance_network_wire_lifecycle(&wire)) == 1U &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING,
          "an unanswered renewal is named and re-acquired, not dropped");
    CHECK(capture.count == 5U &&
              find_dhcp_message_type(
                  capture.frames[4], capture.lengths[4]) == 1,
          "the held address is re-acquired with a fresh discover");

    /* The server answers the re-acquisition: ACTIVE again, same address. */
    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(
        &wire, frame, frame_length,
        renew_at + 2U * APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS + 1U);
    frame_length = build_dhcp_reply_lease(
        frame, 5U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U), 20U);
    (void)appliance_network_wire_input(
        &wire, frame, frame_length,
        renew_at + 2U * APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS + 2U);
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              activate_record.deactivate_calls == 0U &&
              appliance_network_lifecycle_hold_count(
                  appliance_network_wire_lifecycle(&wire)) == 0U &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_NONE,
          "the re-acquisition returns to ACTIVE on the same address");
}

/*
 * ITEM-408: one down sample is not a link loss.
 *
 * The lifecycle's link record is fed from the device's own signal, paced and
 * debounced, so a momentary sample cannot take a working lease away while a
 * link that stays down is still reported honestly.
 */
static void test_link_sample_debounce(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    size_t frame_length;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "link sample fixture is addressing");
    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(&wire, frame, frame_length, 1U);
    frame_length = build_dhcp_reply(
        frame, 5U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    (void)appliance_network_wire_input(&wire, frame, frame_length, 2U);
    CHECK(appliance_network_wire_state(&wire) ==
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "link sample fixture acquires a lease");

    CHECK(appliance_network_wire_link_sample(&wire, false, 1000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_sample(&wire, false, 2000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE &&
              activate_record.deactivate_calls == 0U,
          "a momentary down sample does not take the address away");
    CHECK(appliance_network_wire_link_sample(&wire, true, 3000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              wire.link_down_samples == 0U,
          "an up sample ends the down streak");
    CHECK(appliance_network_wire_link_sample(&wire, false, 4000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_sample(&wire, false, 5000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "a down streak below the threshold still serves");
    CHECK(appliance_network_wire_link_sample(&wire, false, 6000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_WAITING_FOR_LINK &&
              appliance_network_wire_active_config(&wire) == NULL &&
              activate_record.deactivate_calls == 1U &&
              appliance_network_lifecycle_reason(
                  appliance_network_wire_lifecycle(&wire)) ==
                  APPLIANCE_NETWORK_LIFECYCLE_REASON_LINK_DOWN_SAMPLE,
          "a sustained down link is reported honestly");
    CHECK(appliance_network_wire_link_sample(&wire, true, 7000U) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_state(&wire) ==
                  APPLIANCE_NETWORK_LIFECYCLE_STATE_ADDRESSING &&
              capture.count == 3U,
          "a restored link re-addresses on the same generation");
}

static void test_input_filtering(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io;
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct network_link_state_record link = make_link();
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    uint8_t short_frame[8];
    size_t frame_length;

    reset_test_state();
    io = make_io();
    CHECK(appliance_network_wire_init(
              &wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_start(&wire) ==
              APPLIANCE_NETWORK_WIRE_OK &&
              appliance_network_wire_link_up(&wire, &link, 0U) ==
              APPLIANCE_NETWORK_WIRE_OK,
          "input filtering fixture is addressing");
    memset(short_frame, 0, sizeof(short_frame));
    CHECK(appliance_network_wire_input(
              &wire, short_frame, sizeof(short_frame), 1U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_IGNORED,
          "short non-IPv4 frames are not consumed");

    frame_length = build_dhcp_reply(
        frame, 2U, ip_from_octets(10U, 0U, 2U, 15U),
        ip_from_octets(10U, 0U, 2U, 2U));
    frame[4] = 0xAAU;
    frame[5] = 0xBBU;
    put_u32(frame + APPLIANCE_NETWORK_WIRE_ETHERNET_HEADER_LENGTH +
                APPLIANCE_NETWORK_WIRE_IPV4_HEADER_LENGTH +
                APPLIANCE_NETWORK_WIRE_UDP_HEADER_LENGTH + 4U,
            0xDEADBEEFU);
    CHECK(appliance_network_wire_input(
              &wire, frame, frame_length, 1U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED,
          "mismatched DHCP transaction is consumed but ignored");
    CHECK(appliance_network_wire_state(&wire) !=
              APPLIANCE_NETWORK_LIFECYCLE_STATE_ACTIVE,
          "mismatched DHCP transaction does not activate");
    CHECK(wire.malformed_count >= 1U,
          "mismatched DHCP transaction is counted as malformed");

    CHECK(appliance_network_wire_input(&wire, NULL, 0U, 1U) ==
              APPLIANCE_NETWORK_WIRE_INPUT_IGNORED,
          "null input is ignored");
}

static void test_validation(void)
{
    struct appliance_network_wire wire;
    struct appliance_network_wire_io io = make_io();
    struct network_device_snapshot device = make_device();
    struct appliance_network_config desired = make_dhcp_desired();
    struct appliance_network_config bad_desired;

    reset_test_state();
    memset(&bad_desired, 0, sizeof(bad_desired));
    CHECK(appliance_network_wire_init(NULL, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_NULL,
          "wire init rejects a null wire");
    CHECK(appliance_network_wire_init(&wire, NULL, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_NULL,
          "wire init rejects a null desired configuration");
    CHECK(appliance_network_wire_init(&wire, &bad_desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_BAD_CONFIG,
          "wire init rejects an invalid desired configuration");
    io.transmit = NULL;
    CHECK(appliance_network_wire_init(&wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_BAD_IO,
          "wire init rejects a missing transmit seam");
    io = make_io();
    io.activate.activate_ipv4 = NULL;
    CHECK(appliance_network_wire_init(&wire, &desired, &device, &io) ==
              APPLIANCE_NETWORK_WIRE_BAD_IO,
          "wire init rejects a missing activation seam");
}

int main(void)
{
    test_dhcp_handshake();
    test_dhcp_timeout();
    test_dhcp_transport_refused();
    test_dns_timeout();
    test_lease_time_and_renewal();
    test_link_sample_debounce();
    test_input_filtering();
    test_validation();

    if (failures != 0) {
        fprintf(stderr, "Appliance network wire host tests: %d FAILED\n",
                failures);
        return 1;
    }
    printf("Appliance network wire host tests: ALL OK\n");
    return 0;
}

#else
typedef int appliance_network_wire_host_translation_unit_guard;
#endif
