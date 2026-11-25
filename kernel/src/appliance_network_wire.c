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

#define WIRE_ETHERNET_HEADER_LENGTH 14U
#define WIRE_IPV4_MIN_HEADER_LENGTH 20U
#define WIRE_UDP_HEADER_LENGTH 8U
#define WIRE_DHCP_FIXED_LENGTH 240U
#define WIRE_DHCP_OPTION_END 255U
#define WIRE_DHCP_OPTION_PAD 0U
#define WIRE_IP_PROTOCOL_UDP 17U
#define WIRE_BROADCAST_ADDRESS UINT32_C(0xFFFFFFFF)
#define WIRE_ZERO_ADDRESS UINT32_C(0x00000000)
#define WIRE_DNS_TRANSACTION_ID 0x4F44U

struct wire_dhcp_message {
    uint8_t message_type;
    uint32_t yiaddr;
    bool has_subnet;
    uint32_t subnet_mask;
    bool has_router;
    uint32_t router;
    bool has_dns;
    uint32_t dns;
    bool has_server;
    uint32_t server_id;
    /* ITEM-408: option 51, the lease time the server itself returned. */
    bool has_lease;
    uint32_t lease_seconds;
};

struct wire_dns_message {
    uint16_t transaction_id;
    uint8_t rcode;
    bool has_answer;
    uint32_t answer;
};

static void wire_zero_bytes(void *destination, size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void wire_copy_bytes(
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

static uint16_t wire_read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t wire_read_be32(const uint8_t *bytes)
{
    return ((uint32_t)bytes[0] << 24U) |
           ((uint32_t)bytes[1] << 16U) |
           ((uint32_t)bytes[2] << 8U) |
           (uint32_t)bytes[3];
}

static void wire_write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)(value & 0xFFU);
}

static void wire_write_be32(uint8_t *bytes, uint32_t value)
{
    bytes[0] = (uint8_t)(value >> 24U);
    bytes[1] = (uint8_t)((value >> 16U) & 0xFFU);
    bytes[2] = (uint8_t)((value >> 8U) & 0xFFU);
    bytes[3] = (uint8_t)(value & 0xFFU);
}

static uint32_t wire_ip_from_address(
    const struct network_ipv4_address *address)
{
    return ((uint32_t)address->octets[0] << 24U) |
           ((uint32_t)address->octets[1] << 16U) |
           ((uint32_t)address->octets[2] << 8U) |
           (uint32_t)address->octets[3];
}

static void wire_address_from_ip(
    struct network_ipv4_address *address,
    uint32_t value)
{
    address->octets[0] = (uint8_t)(value >> 24U);
    address->octets[1] = (uint8_t)((value >> 16U) & 0xFFU);
    address->octets[2] = (uint8_t)((value >> 8U) & 0xFFU);
    address->octets[3] = (uint8_t)(value & 0xFFU);
}

static uint16_t wire_checksum(const uint8_t *data, size_t length)
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

static uint16_t wire_udp_checksum(
    uint32_t source_ip,
    uint32_t destination_ip,
    const uint8_t *udp,
    size_t udp_length)
{
    uint32_t sum = 0U;
    size_t index;

    /* Pseudo header: source, destination, zero, protocol, length. */
    sum += (source_ip >> 16U) & 0xFFFFU;
    sum += source_ip & 0xFFFFU;
    sum += (destination_ip >> 16U) & 0xFFFFU;
    sum += destination_ip & 0xFFFFU;
    sum += WIRE_IP_PROTOCOL_UDP;
    sum += (uint32_t)udp_length;
    for (index = 0U; index + 1U < udp_length; index += 2U) {
        sum += ((uint32_t)udp[index] << 8U) | udp[index + 1U];
    }
    if ((udp_length & 1U) != 0U) {
        sum += (uint32_t)udp[udp_length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    {
        const uint16_t checksum = (uint16_t)~sum;

        return checksum == 0U ? 0xFFFFU : checksum;
    }
}

static size_t wire_build_frame(
    uint8_t *frame,
    const uint8_t source_mac[NETWORK_DEVICE_MAC_LENGTH],
    const uint8_t destination_mac[NETWORK_DEVICE_MAC_LENGTH],
    uint32_t source_ip,
    uint32_t destination_ip,
    uint16_t source_port,
    uint16_t destination_port,
    const uint8_t *payload,
    size_t payload_length)
{
    const size_t ip_length = WIRE_IPV4_MIN_HEADER_LENGTH +
                             WIRE_UDP_HEADER_LENGTH + payload_length;
    uint8_t *ip = frame + WIRE_ETHERNET_HEADER_LENGTH;
    uint8_t *udp = ip + WIRE_IPV4_MIN_HEADER_LENGTH;

    wire_copy_bytes(frame, destination_mac, NETWORK_DEVICE_MAC_LENGTH);
    wire_copy_bytes(frame + 6U, source_mac, NETWORK_DEVICE_MAC_LENGTH);
    wire_write_be16(frame + 12U, 0x0800U);

    wire_zero_bytes(ip, WIRE_IPV4_MIN_HEADER_LENGTH);
    ip[0] = 0x45U;
    wire_write_be16(ip + 2U, (uint16_t)ip_length);
    wire_write_be16(ip + 4U, 0x4243U);
    wire_write_be16(ip + 6U, 0x0000U);
    ip[8] = 64U;
    ip[9] = WIRE_IP_PROTOCOL_UDP;
    wire_write_be32(ip + 12U, source_ip);
    wire_write_be32(ip + 16U, destination_ip);
    wire_write_be16(ip + 10U, wire_checksum(ip, WIRE_IPV4_MIN_HEADER_LENGTH));

    wire_write_be16(udp, source_port);
    wire_write_be16(udp + 2U, destination_port);
    wire_write_be16(udp + 4U,
        (uint16_t)(WIRE_UDP_HEADER_LENGTH + payload_length));
    wire_copy_bytes(udp + WIRE_UDP_HEADER_LENGTH, payload, payload_length);
    wire_write_be16(udp + 6U, 0U);
    wire_write_be16(udp + 6U,
        wire_udp_checksum(source_ip, destination_ip, udp,
            WIRE_UDP_HEADER_LENGTH + payload_length));
    return WIRE_ETHERNET_HEADER_LENGTH + ip_length;
}

static size_t wire_dhcp_append_option(
    uint8_t *options,
    size_t position,
    uint8_t code,
    const uint8_t *value,
    size_t value_length)
{
    options[position] = code;
    options[position + 1U] = (uint8_t)value_length;
    wire_copy_bytes(options + position + 2U, value, value_length);
    return position + 2U + value_length;
}

static size_t wire_build_dhcp_payload(
    uint8_t *payload,
    const uint8_t client_mac[NETWORK_DEVICE_MAC_LENGTH],
    uint8_t message_type,
    uint32_t ciaddr,
    uint32_t requested_address,
    uint32_t server_id)
{
    uint8_t parameter_request[3];
    uint8_t client_identifier[7];
    uint8_t message[1];
    size_t position = WIRE_DHCP_FIXED_LENGTH;

    wire_zero_bytes(payload, APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN);
    payload[0] = 1U;
    payload[1] = 1U;
    payload[2] = 6U;
    payload[3] = 0U;
    wire_write_be32(payload + 4U, APPLIANCE_NETWORK_WIRE_DHCP_XID);
    payload[11] = 0x80U;
    wire_write_be32(payload + 12U, ciaddr);
    wire_copy_bytes(payload + 28U, client_mac, NETWORK_DEVICE_MAC_LENGTH);
    wire_write_be32(payload + 236U, APPLIANCE_NETWORK_WIRE_DHCP_MAGIC);

    message[0] = message_type;
    position = wire_dhcp_append_option(
        payload, position, 53U, message, 1U);
    client_identifier[0] = 1U;
    wire_copy_bytes(client_identifier + 1U, client_mac,
                    NETWORK_DEVICE_MAC_LENGTH);
    position = wire_dhcp_append_option(
        payload, position, 61U, client_identifier, sizeof(client_identifier));
    if (requested_address != WIRE_ZERO_ADDRESS) {
        uint8_t requested[4];

        wire_write_be32(requested, requested_address);
        position = wire_dhcp_append_option(
            payload, position, 50U, requested, 4U);
    }
    if (server_id != WIRE_ZERO_ADDRESS) {
        uint8_t server[4];

        wire_write_be32(server, server_id);
        position = wire_dhcp_append_option(
            payload, position, 54U, server, 4U);
    }
    parameter_request[0] = 1U;
    parameter_request[1] = 3U;
    parameter_request[2] = 6U;
    position = wire_dhcp_append_option(
        payload, position, 55U, parameter_request,
        sizeof(parameter_request));
    payload[position] = WIRE_DHCP_OPTION_END;
    ++position;
    if (position < APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN) {
        position = APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN;
    }
    return position;
}

static bool wire_parse_dhcp(
    const uint8_t *payload,
    size_t payload_length,
    const uint8_t client_mac[NETWORK_DEVICE_MAC_LENGTH],
    struct wire_dhcp_message *message)
{
    size_t position = WIRE_DHCP_FIXED_LENGTH;

    if (payload_length < WIRE_DHCP_FIXED_LENGTH + 1U ||
        payload[0] != 2U || payload[1] != 1U || payload[2] != 6U ||
        wire_read_be32(payload + 4U) != APPLIANCE_NETWORK_WIRE_DHCP_XID ||
        wire_read_be32(payload + 236U) !=
            APPLIANCE_NETWORK_WIRE_DHCP_MAGIC) {
        return false;
    }
    for (position = 0U; position < NETWORK_DEVICE_MAC_LENGTH; ++position) {
        if (payload[28U + position] != client_mac[position]) {
            return false;
        }
    }
    position = WIRE_DHCP_FIXED_LENGTH;

    wire_zero_bytes(message, sizeof(*message));
    message->yiaddr = wire_read_be32(payload + 16U);
    while (position < payload_length) {
        const uint8_t code = payload[position];
        uint8_t option_length;

        if (code == WIRE_DHCP_OPTION_END) {
            break;
        }
        if (code == WIRE_DHCP_OPTION_PAD) {
            ++position;
            continue;
        }
        if (position + 2U > payload_length) {
            return false;
        }
        option_length = payload[position + 1U];
        if (position + 2U + option_length > payload_length) {
            return false;
        }
        switch (code) {
        case 53U:
            if (option_length != 1U) {
                return false;
            }
            message->message_type = payload[position + 2U];
            break;
        case 1U:
            if (option_length != 4U) {
                return false;
            }
            message->subnet_mask = wire_read_be32(payload + position + 2U);
            message->has_subnet = true;
            break;
        case 3U:
            if (option_length != 4U) {
                return false;
            }
            message->router = wire_read_be32(payload + position + 2U);
            message->has_router = true;
            break;
        case 6U:
            if (option_length < 4U) {
                return false;
            }
            message->dns = wire_read_be32(payload + position + 2U);
            message->has_dns = true;
            break;
        case 54U:
            if (option_length != 4U) {
                return false;
            }
            message->server_id = wire_read_be32(payload + position + 2U);
            message->has_server = true;
            break;
        case 51U:
            /*
             * ITEM-408: the lease time is what the renewal schedule is
             * computed from.  A malformed option is refused instead of being
             * guessed at, exactly like every other option this parser reads.
             */
            if (option_length != 4U) {
                return false;
            }
            message->lease_seconds = wire_read_be32(payload + position + 2U);
            message->has_lease = true;
            break;
        default:
            break;
        }
        position += 2U + option_length;
    }
    return message->message_type != 0U;
}

static bool wire_prefix_from_mask(uint32_t mask, uint8_t *prefix)
{
    uint32_t expected = 0U;
    uint8_t length = 0U;
    uint8_t bit;

    for (bit = 0U; bit < 32U; ++bit) {
        if ((mask & (UINT32_C(1) << (31U - bit))) != 0U) {
            ++length;
        } else {
            break;
        }
    }
    for (bit = 0U; bit < length; ++bit) {
        expected |= UINT32_C(1) << (31U - bit);
    }
    if (expected != mask) {
        return false;
    }
    *prefix = length;
    return true;
}

static bool wire_dhcp_lease_config(
    const struct wire_dhcp_message *message,
    struct network_ipv4_config *config)
{
    uint8_t prefix = 0U;

    if (message->yiaddr == WIRE_ZERO_ADDRESS ||
        !message->has_subnet || !message->has_router || !message->has_dns) {
        return false;
    }
    if (!wire_prefix_from_mask(message->subnet_mask, &prefix)) {
        return false;
    }
    wire_zero_bytes(config, sizeof(*config));
    config->schema_version = NETWORK_IPV4_CONFIG_VERSION;
    wire_address_from_ip(&config->address, message->yiaddr);
    config->prefix_length = prefix;
    wire_address_from_ip(&config->gateway, message->router);
    wire_address_from_ip(&config->dns, message->dns);
    return network_ipv4_config_validate(config) == NETWORK_IPV4_OK;
}

static size_t wire_build_dns_query(
    uint8_t *payload,
    size_t capacity,
    uint16_t transaction_id,
    const char *hostname,
    size_t hostname_length)
{
    size_t position = 12U;
    size_t label_start = 0U;
    size_t index;

    if (hostname_length == 0U || hostname_length > capacity) {
        return 0U;
    }
    wire_zero_bytes(payload, capacity);
    wire_write_be16(payload, transaction_id);
    wire_write_be16(payload + 2U, 0x0100U);
    wire_write_be16(payload + 4U, 1U);
    for (index = 0U; index <= hostname_length; ++index) {
        if (index == hostname_length || hostname[index] == '.') {
            const size_t label_length = index - label_start;

            if (label_length == 0U || label_length > 63U ||
                position + 1U + label_length > capacity) {
                return 0U;
            }
            payload[position] = (uint8_t)label_length;
            wire_copy_bytes(payload + position + 1U,
                            hostname + label_start, label_length);
            position += 1U + label_length;
            label_start = index + 1U;
        }
    }
    if (position + 5U > capacity) {
        return 0U;
    }
    payload[position] = 0U;
    ++position;
    wire_write_be16(payload + position, 1U);
    wire_write_be16(payload + position + 2U, 1U);
    position += 4U;
    return position;
}

static bool wire_skip_dns_name(
    const uint8_t *payload,
    size_t payload_length,
    size_t *position)
{
    size_t cursor = *position;

    for (;;) {
        uint8_t length;

        if (cursor >= payload_length) {
            return false;
        }
        length = payload[cursor];
        if (length == 0U) {
            ++cursor;
            break;
        }
        if ((length & 0xC0U) == 0xC0U) {
            if (cursor + 2U > payload_length) {
                return false;
            }
            cursor += 2U;
            break;
        }
        if (length > 63U || cursor + 1U + length > payload_length) {
            return false;
        }
        cursor += 1U + length;
    }
    *position = cursor;
    return true;
}

static bool wire_parse_dns(
    const uint8_t *payload,
    size_t payload_length,
    struct wire_dns_message *message)
{
    uint16_t question_count;
    uint16_t answer_count;
    size_t position = 12U;
    uint16_t index;

    if (payload_length < 12U) {
        return false;
    }
    wire_zero_bytes(message, sizeof(*message));
    message->transaction_id = wire_read_be16(payload);
    if ((wire_read_be16(payload + 2U) & 0x8000U) == 0U) {
        return false;
    }
    message->rcode = (uint8_t)(wire_read_be16(payload + 2U) & 0x0FU);
    question_count = wire_read_be16(payload + 4U);
    answer_count = wire_read_be16(payload + 6U);
    for (index = 0U; index < question_count; ++index) {
        if (!wire_skip_dns_name(payload, payload_length, &position) ||
            position + 4U > payload_length) {
            return false;
        }
        position += 4U;
    }
    for (index = 0U; index < answer_count; ++index) {
        uint16_t type;
        uint16_t record_class;
        uint16_t data_length;

        if (!wire_skip_dns_name(payload, payload_length, &position) ||
            position + 10U > payload_length) {
            return false;
        }
        type = wire_read_be16(payload + position);
        record_class = wire_read_be16(payload + position + 2U);
        data_length = wire_read_be16(payload + position + 8U);
        position += 10U;
        if (position + data_length > payload_length) {
            return false;
        }
        if (!message->has_answer && type == 1U && record_class == 1U &&
            data_length == 4U) {
            message->answer = wire_read_be32(payload + position);
            message->has_answer = true;
        }
        position += data_length;
    }
    return true;
}

static bool wire_is_on_link(
    const struct network_ipv4_config *active,
    uint32_t destination)
{
    const uint32_t mask = active->prefix_length == 0U
        ? 0U
        : (UINT32_C(0xFFFFFFFF) << (32U - active->prefix_length));
    const uint32_t local = wire_ip_from_address(&active->address);

    return (local & mask) == (destination & mask);
}

static bool wire_resolve_next_hop(
    struct appliance_network_wire *wire,
    uint32_t destination,
    uint8_t mac_out[NETWORK_DEVICE_MAC_LENGTH])
{
    const struct network_ipv4_config *active;
    struct network_ipv4_address next_hop;

    if (wire->io.link.resolve == NULL) {
        return false;
    }
    active = appliance_network_lifecycle_active_config(&wire->lifecycle);
    if (active == NULL) {
        return false;
    }
    if (wire_is_on_link(active, destination)) {
        wire_address_from_ip(&next_hop, destination);
    } else {
        next_hop = active->gateway;
    }
    return wire->io.link.resolve(wire->io.link.context, &next_hop, mac_out);
}

static bool wire_transmit(
    struct appliance_network_wire *wire,
    const uint8_t *frame,
    size_t length)
{
    if (wire->io.transmit(wire->io.transmit_context, frame, length)) {
        ++wire->transmitted_count;
        return true;
    }
    return false;
}

static bool wire_send_dhcp(
    struct appliance_network_wire *wire,
    uint8_t message_type,
    uint32_t ciaddr,
    uint32_t requested_address,
    uint32_t server_id)
{
    static const uint8_t broadcast[NETWORK_DEVICE_MAC_LENGTH] =
        {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    uint8_t payload[APPLIANCE_NETWORK_WIRE_DHCP_PAYLOAD_MIN];
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    const size_t payload_length = wire_build_dhcp_payload(
        payload, wire->local_mac, message_type, ciaddr,
        requested_address, server_id);
    const size_t frame_length = wire_build_frame(
        frame, wire->local_mac, broadcast, ciaddr, WIRE_BROADCAST_ADDRESS,
        APPLIANCE_NETWORK_WIRE_DHCP_CLIENT_PORT,
        APPLIANCE_NETWORK_WIRE_DHCP_SERVER_PORT,
        payload, payload_length);

    if (!wire_transmit(wire, frame, frame_length)) {
        /*
         * ITEM-428: the frame never reached the wire.  Counted here, where the
         * frame is known to be a DHCP frame, so the boot record can say
         * `refused=` next to the attempts it explains.
         */
        ++wire->dhcp_send_refusals;
        return false;
    }
    if (message_type == 1U) {
        ++wire->dhcp_discovers_sent;
    } else {
        ++wire->dhcp_requests_sent;
    }
    return true;
}

static bool wire_send_dns(struct appliance_network_wire *wire)
{
    const struct network_ipv4_config *active =
        appliance_network_lifecycle_active_config(&wire->lifecycle);
    uint8_t payload[256U];
    uint8_t frame[APPLIANCE_NETWORK_WIRE_FRAME_MAX];
    uint8_t next_hop_mac[NETWORK_DEVICE_MAC_LENGTH];
    size_t payload_length;
    size_t frame_length;
    uint32_t destination;

    if (active == NULL || wire->dns_hostname_length == 0U) {
        return false;
    }
    payload_length = wire_build_dns_query(
        payload, sizeof(payload), wire->dns_transaction_id,
        wire->dns_hostname, wire->dns_hostname_length);
    if (payload_length == 0U) {
        return false;
    }
    destination = wire_ip_from_address(&active->dns);
    if (!wire_resolve_next_hop(wire, destination, next_hop_mac)) {
        return false;
    }
    frame_length = wire_build_frame(
        frame, wire->local_mac, next_hop_mac,
        wire_ip_from_address(&active->address), destination,
        APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT,
        APPLIANCE_NETWORK_WIRE_DNS_PORT,
        payload, payload_length);
    return wire_transmit(wire, frame, frame_length);
}

static void wire_set_dhcp_deadline(struct appliance_network_wire *wire)
{
    wire->dhcp_deadline_ms =
        wire->now_ms + APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
    wire->dhcp_deadline_set = true;
    wire->dhcp_kick_ms = wire->now_ms;
}

static void wire_set_dns_deadline(struct appliance_network_wire *wire)
{
    wire->dns_deadline_ms =
        wire->now_ms + APPLIANCE_NETWORK_WIRE_DNS_TIMEOUT_MS;
    wire->dns_deadline_set = true;
}

static bool wire_due(uint32_t now_ms, uint32_t deadline_ms)
{
    return (int32_t)(now_ms - deadline_ms) >= 0;
}

/*
 * ITEM-408: arm the renewal from the lease time the server returned, on the
 * monotonic clock the tick is driven with.  Half the lease is the canonical
 * T1; the floor keeps a pathological lease from producing a renewal storm and
 * the ceiling keeps a long lease from waiting for its whole lifetime.  A
 * server that returned no lease time arms nothing, because there is nothing
 * honest to compute T1 from.
 */
static void wire_arm_renewal(
    struct appliance_network_wire *wire,
    uint32_t now_ms,
    const struct wire_dhcp_message *message)
{
    uint64_t half;

    if (message == NULL || !message->has_lease ||
        message->lease_seconds == 0U) {
        wire->renew_scheduled = false;
        wire->lease_time_published = false;
        wire->lease_seconds = 0U;
        return;
    }
    wire->lease_time_published = true;
    wire->lease_seconds = message->lease_seconds;
    half = ((uint64_t)message->lease_seconds * 1000ULL) / 2ULL;
    if (half < APPLIANCE_NETWORK_WIRE_RENEW_FLOOR_MS) {
        half = APPLIANCE_NETWORK_WIRE_RENEW_FLOOR_MS;
    }
    if (half > APPLIANCE_NETWORK_WIRE_RENEW_CEILING_MS) {
        half = APPLIANCE_NETWORK_WIRE_RENEW_CEILING_MS;
    }
    wire->renew_at_ms = now_ms + (uint32_t)half;
    wire->renew_scheduled = true;
}

static void wire_arm_renewal_retry(
    struct appliance_network_wire *wire,
    uint32_t now_ms)
{
    wire->renew_at_ms = now_ms + APPLIANCE_NETWORK_WIRE_RENEW_RETRY_MS;
    wire->renew_scheduled = wire->lease_time_published;
}

static void wire_arm_dns_probe_retry(
    struct appliance_network_wire *wire,
    uint32_t now_ms)
{
    wire->dns_probe_at_ms = now_ms + APPLIANCE_NETWORK_WIRE_DNS_PROBE_RETRY_MS;
    wire->dns_probe_scheduled = true;
    ++wire->dns_probe_failures;
}

/* --- lifecycle transport callbacks ------------------------------------ */

static bool wire_transport_activate(
    void *context,
    const struct network_ipv4_config *config)
{
    struct appliance_network_wire *wire = context;

    if (wire == NULL || config == NULL) {
        return false;
    }
    return wire->io.activate.activate_ipv4(
        wire->io.activate.context, config);
}

static bool wire_transport_deactivate(void *context)
{
    struct appliance_network_wire *wire = context;

    if (wire == NULL || wire->io.activate.deactivate_ipv4 == NULL) {
        return true;
    }
    return wire->io.activate.deactivate_ipv4(wire->io.activate.context);
}

static bool wire_transport_dhcp_discover(void *context)
{
    struct appliance_network_wire *wire = context;

    if (!wire_send_dhcp(wire, 1U, WIRE_ZERO_ADDRESS,
                        WIRE_ZERO_ADDRESS, WIRE_ZERO_ADDRESS)) {
        return false;
    }
    wire_set_dhcp_deadline(wire);
    return true;
}

static bool wire_transport_dhcp_request(void *context)
{
    struct appliance_network_wire *wire = context;

    if (!wire_send_dhcp(wire, 3U, WIRE_ZERO_ADDRESS,
                        wire->offered_address,
                        wire->server_present ? wire->offered_server
                                             : WIRE_ZERO_ADDRESS)) {
        return false;
    }
    wire_set_dhcp_deadline(wire);
    return true;
}

static bool wire_transport_dhcp_renew(void *context)
{
    struct appliance_network_wire *wire = context;
    const struct network_ipv4_config *active =
        appliance_network_lifecycle_active_config(&wire->lifecycle);
    uint32_t address = WIRE_ZERO_ADDRESS;

    if (active != NULL) {
        address = wire_ip_from_address(&active->address);
    }
    if (!wire_send_dhcp(wire, 3U, address, address,
                        wire->server_present ? wire->offered_server
                                             : WIRE_ZERO_ADDRESS)) {
        return false;
    }
    wire_set_dhcp_deadline(wire);
    return true;
}

static bool wire_transport_dhcp_rebind(void *context)
{
    return wire_transport_dhcp_renew(context);
}

static bool wire_transport_dns_query(
    void *context,
    const char *hostname,
    size_t hostname_length)
{
    struct appliance_network_wire *wire = context;

    if (wire == NULL || hostname == NULL ||
        hostname_length == 0U ||
        hostname_length > APPLIANCE_NETWORK_WIRE_HOSTNAME_MAX) {
        return false;
    }
    wire_copy_bytes(wire->dns_hostname, hostname, hostname_length);
    wire->dns_hostname[hostname_length] = '\0';
    wire->dns_hostname_length = hostname_length;
    (void)wire_send_dns(wire);
    wire_set_dns_deadline(wire);
    return true;
}

/* --- public API -------------------------------------------------------- */

bool appliance_network_wire_published(
    const struct appliance_network_wire *wire)
{
    return wire != NULL &&
           wire->initialized &&
           wire->version == APPLIANCE_NETWORK_WIRE_VERSION;
}

enum appliance_network_wire_status appliance_network_wire_init(
    struct appliance_network_wire *wire,
    const struct appliance_network_config *desired,
    const struct network_device_snapshot *device,
    const struct appliance_network_wire_io *io)
{
    struct appliance_network_lifecycle_transport transport;
    enum appliance_network_lifecycle_status lifecycle_status;

    if (wire == NULL || desired == NULL || device == NULL) {
        return APPLIANCE_NETWORK_WIRE_NULL;
    }
    if (io == NULL || io->transmit == NULL ||
        io->activate.activate_ipv4 == NULL) {
        return APPLIANCE_NETWORK_WIRE_BAD_IO;
    }
    if (appliance_network_config_validate(desired) !=
        APPLIANCE_NETWORK_CONFIG_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_CONFIG;
    }
    if (network_device_snapshot_validate(device) != NETWORK_DEVICE_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_DEVICE;
    }

    wire_zero_bytes(wire, sizeof(*wire));
    wire->version = APPLIANCE_NETWORK_WIRE_VERSION;
    wire->initialized = true;
    wire->io = *io;
    wire_copy_bytes(wire->local_mac, device->mac_address,
                    NETWORK_DEVICE_MAC_LENGTH);
    wire->device_id = device->device_id;
    wire->dns_transaction_id = WIRE_DNS_TRANSACTION_ID;

    wire_zero_bytes(&transport, sizeof(transport));
    transport.version = APPLIANCE_NETWORK_LIFECYCLE_VERSION;
    transport.context = wire;
    transport.activate_ipv4 = wire_transport_activate;
    transport.deactivate_ipv4 = wire_transport_deactivate;
    transport.dhcp_discover = wire_transport_dhcp_discover;
    transport.dhcp_request = wire_transport_dhcp_request;
    transport.dhcp_renew = wire_transport_dhcp_renew;
    transport.dhcp_rebind = wire_transport_dhcp_rebind;
    transport.dns_query = wire_transport_dns_query;

    lifecycle_status = appliance_network_lifecycle_init(
        &wire->lifecycle, desired, device, &transport);
    if (lifecycle_status != APPLIANCE_NETWORK_LIFECYCLE_OK) {
        wire_zero_bytes(wire, sizeof(*wire));
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    return APPLIANCE_NETWORK_WIRE_OK;
}

enum appliance_network_wire_status appliance_network_wire_start(
    struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    if (wire->started) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    if (appliance_network_lifecycle_start(&wire->lifecycle) !=
        APPLIANCE_NETWORK_LIFECYCLE_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    wire->started = true;
    return APPLIANCE_NETWORK_WIRE_OK;
}

enum appliance_network_wire_status appliance_network_wire_link_up(
    struct appliance_network_wire *wire,
    const struct network_link_state_record *link,
    uint32_t now_ms)
{
    if (!appliance_network_wire_published(wire) || link == NULL) {
        return APPLIANCE_NETWORK_WIRE_NULL;
    }
    if (!wire->started) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    wire->now_ms = now_ms;
    if (network_link_state_validate(link) != NETWORK_LINK_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_LINK;
    }
    if (appliance_network_lifecycle_link(&wire->lifecycle, link) !=
        APPLIANCE_NETWORK_LIFECYCLE_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_LINK;
    }
    wire->link_generation = link->generation;
    wire->link_record_published = true;
    wire->link_observed_up = link->state == NETWORK_LINK_UP;
    wire->link_down_samples = 0U;
    wire->link_sample_taken = true;
    wire->link_sample_ms = now_ms;
    return APPLIANCE_NETWORK_WIRE_OK;
}

/*
 * ITEM-408: one live link sample.
 *
 * The lifecycle's link record is the appliance's own decision, not a raw
 * register read: a momentary down sample - the PHY renegotiating, a cable
 * being reseated, one flaky read - must not take a working lease away, while
 * a link that stays down is reported honestly.  The sampler is therefore
 * paced, and it publishes a down record only after
 * APPLIANCE_NETWORK_WIRE_LINK_DOWN_SAMPLES consecutive samples agreed; the
 * first up sample ends the streak and restores the path.
 */
enum appliance_network_wire_status appliance_network_wire_link_sample(
    struct appliance_network_wire *wire,
    bool link_up,
    uint32_t now_ms)
{
    struct network_link_state_record link;

    if (!appliance_network_wire_published(wire)) {
        return APPLIANCE_NETWORK_WIRE_NULL;
    }
    if (!wire->started || !wire->link_record_published) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    if (wire->link_sample_taken &&
        (uint32_t)(now_ms - wire->link_sample_ms) <
            APPLIANCE_NETWORK_WIRE_LINK_SAMPLE_MS) {
        return APPLIANCE_NETWORK_WIRE_OK;
    }
    wire->link_sample_taken = true;
    wire->link_sample_ms = now_ms;

    if (link_up) {
        wire->link_down_samples = 0U;
        if (wire->link_observed_up) {
            return APPLIANCE_NETWORK_WIRE_OK;
        }
    } else {
        if (wire->link_down_samples <
            APPLIANCE_NETWORK_WIRE_LINK_DOWN_SAMPLES) {
            ++wire->link_down_samples;
        }
        if (wire->link_down_samples <
                APPLIANCE_NETWORK_WIRE_LINK_DOWN_SAMPLES ||
            !wire->link_observed_up) {
            return APPLIANCE_NETWORK_WIRE_OK;
        }
    }

    wire_zero_bytes(&link, sizeof(link));
    link.version = NETWORK_LINK_STATE_VERSION;
    link.device_id = wire->device_id;
    link.state = link_up ? NETWORK_LINK_UP : NETWORK_LINK_DOWN;
    link.generation = wire->link_generation;
    return appliance_network_wire_link_up(wire, &link, now_ms);
}

enum appliance_network_wire_status appliance_network_wire_resolve(
    struct appliance_network_wire *wire,
    const char *hostname,
    size_t hostname_length,
    uint32_t now_ms)
{
    if (!appliance_network_wire_published(wire) || hostname == NULL) {
        return APPLIANCE_NETWORK_WIRE_NULL;
    }
    wire->now_ms = now_ms;
    if (appliance_network_lifecycle_dns_query(
            &wire->lifecycle, hostname, hostname_length) !=
        APPLIANCE_NETWORK_LIFECYCLE_OK) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    return APPLIANCE_NETWORK_WIRE_OK;
}

enum appliance_network_wire_input_result appliance_network_wire_input(
    struct appliance_network_wire *wire,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms)
{
    const uint8_t *ip;
    const uint8_t *udp;
    const uint8_t *payload;
    size_t header_length;
    size_t payload_length;
    uint16_t source_port;
    uint16_t destination_port;

    if (!appliance_network_wire_published(wire) || frame == NULL ||
        frame_length < WIRE_ETHERNET_HEADER_LENGTH + WIRE_IPV4_MIN_HEADER_LENGTH +
                         WIRE_UDP_HEADER_LENGTH) {
        return APPLIANCE_NETWORK_WIRE_INPUT_IGNORED;
    }
    wire->now_ms = now_ms;
    if (wire_read_be16(frame + 12U) != 0x0800U) {
        return APPLIANCE_NETWORK_WIRE_INPUT_IGNORED;
    }
    ip = frame + WIRE_ETHERNET_HEADER_LENGTH;
    if ((ip[0] >> 4U) != 4U || (ip[0] & 0x0FU) < 5U ||
        ip[9] != WIRE_IP_PROTOCOL_UDP) {
        return APPLIANCE_NETWORK_WIRE_INPUT_IGNORED;
    }
    header_length = (size_t)(ip[0] & 0x0FU) * 4U;
    if (frame_length < WIRE_ETHERNET_HEADER_LENGTH + header_length +
                        WIRE_UDP_HEADER_LENGTH) {
        return APPLIANCE_NETWORK_WIRE_INPUT_IGNORED;
    }
    udp = ip + header_length;
    source_port = wire_read_be16(udp);
    destination_port = wire_read_be16(udp + 2U);
    payload = udp + WIRE_UDP_HEADER_LENGTH;
    payload_length = frame_length - WIRE_ETHERNET_HEADER_LENGTH -
                     header_length - WIRE_UDP_HEADER_LENGTH;

    if (destination_port == APPLIANCE_NETWORK_WIRE_DHCP_CLIENT_PORT &&
        source_port == APPLIANCE_NETWORK_WIRE_DHCP_SERVER_PORT) {
        struct wire_dhcp_message message;

        ++wire->received_count;
        if (!wire_parse_dhcp(payload, payload_length,
                             wire->local_mac, &message)) {
            ++wire->malformed_count;
            return APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED;
        }
        switch (message.message_type) {
        case 2U: {
            struct network_ipv4_config candidate;

            wire->offered_address = message.yiaddr;
            wire->offer_present = true;
            if (message.has_server) {
                wire->offered_server = message.server_id;
                wire->server_present = true;
            }
            /* ITEM-428: an offer that arrived is counted whether or not the
             * lifecycle could use it, so the record separates "no answer"
             * from "an answer this appliance refused". */
            ++wire->dhcp_offers_received;
            if (!wire_dhcp_lease_config(&message, &candidate) ||
                appliance_network_lifecycle_dhcp_offer(
                    &wire->lifecycle, &candidate) !=
                    APPLIANCE_NETWORK_LIFECYCLE_OK) {
                ++wire->malformed_count;
            }
            break;
        }
        case 5U: {
            struct network_ipv4_config lease;

            ++wire->dhcp_acks_received;
            if (!wire_dhcp_lease_config(&message, &lease) ||
                appliance_network_lifecycle_dhcp_ack(
                    &wire->lifecycle, &lease) !=
                    APPLIANCE_NETWORK_LIFECYCLE_OK) {
                ++wire->malformed_count;
            } else {
                wire->dhcp_deadline_set = false;
                /*
                 * ITEM-408: the acknowledgement carries the lease time the
                 * appliance must renew within, and it is the same deadline
                 * whether the acknowledgement ended an acquisition or a
                 * renewal: the point of the reply is that the address is
                 * ours until then.
                 */
                wire_arm_renewal(wire, now_ms, &message);
            }
            break;
        }
        case 6U:
            (void)appliance_network_lifecycle_dhcp_nak(
                &wire->lifecycle);
            break;
        default:
            break;
        }
        return APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED;
    }
    if (destination_port == APPLIANCE_NETWORK_WIRE_DNS_SOURCE_PORT &&
        source_port == APPLIANCE_NETWORK_WIRE_DNS_PORT) {
        struct wire_dns_message message;

        ++wire->received_count;
        if (!wire_parse_dns(payload, payload_length, &message) ||
            message.transaction_id != wire->dns_transaction_id) {
            ++wire->malformed_count;
            return APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED;
        }
        if (message.rcode != 0U) {
            wire->dns_deadline_set = false;
            (void)appliance_network_lifecycle_dns_server_failure(
                &wire->lifecycle);
        } else if (message.has_answer) {
            struct network_ipv4_address answer;

            wire_address_from_ip(&answer, message.answer);
            wire->dns_deadline_set = false;
            (void)appliance_network_lifecycle_dns_response(
                &wire->lifecycle, &answer);
        }
        return APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED;
    }
    return APPLIANCE_NETWORK_WIRE_INPUT_IGNORED;
}

static enum appliance_network_lifecycle_dhcp_timeout
wire_dhcp_timeout_kind(const struct appliance_network_wire *wire)
{
    const struct appliance_dhcp_client *dhcp =
        appliance_network_lifecycle_dhcp(&wire->lifecycle);

    if (dhcp == NULL) {
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_NONE;
    }
    switch (appliance_dhcp_client_state(dhcp)) {
    case APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING:
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_DISCOVER;
    case APPLIANCE_DHCP_CLIENT_STATE_REQUESTING:
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REQUEST;
    case APPLIANCE_DHCP_CLIENT_STATE_RENEWING:
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_RENEW;
    case APPLIANCE_DHCP_CLIENT_STATE_REBINDING:
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_REBIND;
    default:
        return APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_NONE;
    }
}

/*
 * ITEM-408: keep the acquisition and refresh cycle moving.
 *
 * Every send arms the bounded deadline that drives the retry, but a send the
 * transport refused - or a hold that re-entered addressing - would otherwise
 * leave the cycle parked with no deadline and no retry.  The kick is paced by
 * the same bounded deadline the retries use, so a healthy path never sees it
 * and a parked cycle retries once per second instead of once per event-loop
 * turn.
 */
static void wire_dhcp_kick(struct appliance_network_wire *wire)
{
    const struct appliance_dhcp_client *dhcp =
        appliance_network_lifecycle_dhcp(&wire->lifecycle);
    bool sent = false;

    if (dhcp == NULL || wire->dhcp_deadline_set ||
        (uint32_t)(wire->now_ms - wire->dhcp_kick_ms) <
            APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS) {
        return;
    }
    wire->dhcp_kick_ms = wire->now_ms;
    switch (appliance_dhcp_client_state(dhcp)) {
    case APPLIANCE_DHCP_CLIENT_STATE_DISCOVERING:
        sent = wire_transport_dhcp_discover(wire);
        break;
    case APPLIANCE_DHCP_CLIENT_STATE_REQUESTING:
        sent = wire_transport_dhcp_request(wire);
        break;
    case APPLIANCE_DHCP_CLIENT_STATE_RENEWING:
        sent = wire_transport_dhcp_renew(wire);
        break;
    case APPLIANCE_DHCP_CLIENT_STATE_REBINDING:
        sent = wire_transport_dhcp_rebind(wire);
        break;
    default:
        /* No state owns a frame here: nothing was asked of the wire. */
        return;
    }
    if (sent) {
        return;
    }
    /*
     * ITEM-428: the frame the kick wanted to send was refused, and this was
     * the one path where that used to be silent - no deadline was armed, no
     * reason was published, and the attempt counter stopped for good, which is
     * exactly the shape the 2026-09-16 boot log ended on.  The refusal is now
     * published as the reason the cycle is not moving, and the deadline is
     * armed anyway: the cycle keeps its one-second cadence, the bounded
     * attempt budget still runs out into a stated verdict, and a transmit path
     * that comes back resumes the acquisition instead of finding it parked.
     */
    (void)appliance_network_lifecycle_dhcp_transport_refused(
        &wire->lifecycle);
    wire->dhcp_deadline_ms =
        wire->now_ms + APPLIANCE_NETWORK_WIRE_DHCP_TIMEOUT_MS;
    wire->dhcp_deadline_set = true;
}

/*
 * ITEM-408: renew before the lease the server granted runs out.  The renewal
 * is a lifecycle transition on the live address, so a renewal that fails
 * never takes the address away: the bounded renew/rebind cycle reports it and
 * the wire arms the next attempt.
 */
static void wire_dhcp_renew_tick(struct appliance_network_wire *wire)
{
    if (!wire->renew_scheduled ||
        !wire_due(wire->now_ms, wire->renew_at_ms)) {
        return;
    }
    wire->renew_scheduled = false;
    if (appliance_network_lifecycle_dhcp_renew(&wire->lifecycle) !=
        APPLIANCE_NETWORK_LIFECYCLE_OK) {
        /*
         * The lifecycle was not in a state that owns a renewal (it is
         * re-acquiring, or it holds no lease), so nothing was sent: retry on
         * the bounded interval instead of leaving the schedule empty.
         */
        wire_arm_renewal_retry(wire, wire->now_ms);
    }
}

void appliance_network_wire_tick(
    struct appliance_network_wire *wire,
    uint32_t now_ms)
{
    if (!appliance_network_wire_published(wire)) {
        return;
    }
    wire->now_ms = now_ms;

    if (wire->dhcp_deadline_set && now_ms >= wire->dhcp_deadline_ms) {
        const enum appliance_network_lifecycle_dhcp_timeout timeout =
            wire_dhcp_timeout_kind(wire);

        if (timeout != APPLIANCE_NETWORK_LIFECYCLE_DHCP_TIMEOUT_NONE) {
            wire->dhcp_deadline_set = false;
            ++wire->retry_count;
            (void)appliance_network_lifecycle_dhcp_timeout(
                &wire->lifecycle, timeout);
        } else {
            wire->dhcp_deadline_set = false;
        }
    }
    wire_dhcp_renew_tick(wire);
    wire_dhcp_kick(wire);

    if (wire->dns_deadline_set && now_ms >= wire->dns_deadline_ms) {
        const struct appliance_dns_resolver *dns =
            appliance_network_lifecycle_dns(&wire->lifecycle);

        wire->dns_deadline_set = false;
        ++wire->retry_count;
        if (dns != NULL &&
            appliance_dns_resolver_state(dns) ==
                APPLIANCE_DNS_RESOLVER_STATE_QUERYING) {
            (void)appliance_network_lifecycle_dns_timeout(
                &wire->lifecycle);
            dns = appliance_network_lifecycle_dns(&wire->lifecycle);
            if (dns != NULL &&
                appliance_dns_resolver_state(dns) ==
                    APPLIANCE_DNS_RESOLVER_STATE_QUERYING) {
                (void)wire_send_dns(wire);
                wire_set_dns_deadline(wire);
            } else {
                /*
                 * ITEM-408: the probe ran out of attempts.  The lifecycle
                 * published the reason and kept the address; the probe itself
                 * is retried on a longer interval instead of being abandoned.
                 */
                wire_arm_dns_probe_retry(wire, now_ms);
            }
        }
    }
    if (wire->dns_probe_scheduled &&
        wire_due(now_ms, wire->dns_probe_at_ms)) {
        wire->dns_probe_scheduled = false;
        if (appliance_network_lifecycle_dns_probe_retry(
                &wire->lifecycle) == APPLIANCE_NETWORK_LIFECYCLE_OK &&
            wire->dns_hostname_length != 0U) {
            (void)appliance_network_wire_resolve(
                wire, wire->dns_hostname, wire->dns_hostname_length,
                now_ms);
        }
    }
}

enum appliance_network_wire_status appliance_network_wire_stop(
    struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return APPLIANCE_NETWORK_WIRE_BAD_STATE;
    }
    wire->dhcp_deadline_set = false;
    wire->dns_deadline_set = false;
    (void)appliance_network_lifecycle_stop(&wire->lifecycle);
    return APPLIANCE_NETWORK_WIRE_OK;
}

enum appliance_network_lifecycle_state appliance_network_wire_state(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return APPLIANCE_NETWORK_LIFECYCLE_STATE_NONE;
    }
    return appliance_network_lifecycle_state(&wire->lifecycle);
}

const struct network_ipv4_config *
appliance_network_wire_active_config(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return NULL;
    }
    return appliance_network_lifecycle_active_config(&wire->lifecycle);
}

const struct appliance_dhcp_client *
appliance_network_wire_dhcp(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return NULL;
    }
    return appliance_network_lifecycle_dhcp(&wire->lifecycle);
}

const struct appliance_dns_resolver *
appliance_network_wire_dns(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return NULL;
    }
    return appliance_network_lifecycle_dns(&wire->lifecycle);
}

const struct network_ipv4_address *
appliance_network_wire_dns_answer(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return NULL;
    }
    return appliance_dns_resolver_answer(
        appliance_network_lifecycle_dns(&wire->lifecycle));
}

const struct appliance_network_lifecycle *
appliance_network_wire_lifecycle(
    const struct appliance_network_wire *wire)
{
    if (!appliance_network_wire_published(wire)) {
        return NULL;
    }
    return &wire->lifecycle;
}

bool appliance_network_wire_dhcp_attempts(
    const struct appliance_network_wire *wire,
    uint32_t *attempt,
    uint32_t *budget)
{
    const struct appliance_dhcp_client *dhcp;

    if (attempt == NULL || budget == NULL ||
        !appliance_network_wire_published(wire)) {
        return false;
    }
    dhcp = appliance_network_lifecycle_dhcp(&wire->lifecycle);
    if (dhcp == NULL) {
        return false;
    }
    *attempt = appliance_dhcp_client_attempt_count(dhcp);
    *budget = appliance_dhcp_client_attempt_budget(dhcp);
    return true;
}

bool appliance_network_wire_dhcp_counters(
    const struct appliance_network_wire *wire,
    struct appliance_network_wire_dhcp_counters *counters)
{
    if (counters == NULL || !appliance_network_wire_published(wire)) {
        return false;
    }
    counters->discovers_sent = wire->dhcp_discovers_sent;
    counters->requests_sent = wire->dhcp_requests_sent;
    counters->offers_received = wire->dhcp_offers_received;
    counters->acks_received = wire->dhcp_acks_received;
    counters->send_refusals = wire->dhcp_send_refusals;
    return true;
}

bool appliance_network_wire_lease_time(
    const struct appliance_network_wire *wire,
    uint32_t *lease_seconds_out)
{
    if (!appliance_network_wire_published(wire) || lease_seconds_out == NULL ||
        !wire->lease_time_published) {
        return false;
    }
    *lease_seconds_out = wire->lease_seconds;
    return true;
}

const char *appliance_network_wire_status_string(
    enum appliance_network_wire_status status)
{
    switch (status) {
    case APPLIANCE_NETWORK_WIRE_OK:
        return "OK";
    case APPLIANCE_NETWORK_WIRE_NULL:
        return "NULL";
    case APPLIANCE_NETWORK_WIRE_BAD_STATE:
        return "BAD_STATE";
    case APPLIANCE_NETWORK_WIRE_BAD_VERSION:
        return "BAD_VERSION";
    case APPLIANCE_NETWORK_WIRE_BAD_CONFIG:
        return "BAD_CONFIG";
    case APPLIANCE_NETWORK_WIRE_BAD_DEVICE:
        return "BAD_DEVICE";
    case APPLIANCE_NETWORK_WIRE_BAD_IO:
        return "BAD_IO";
    case APPLIANCE_NETWORK_WIRE_BAD_LINK:
        return "BAD_LINK";
    case APPLIANCE_NETWORK_WIRE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    case APPLIANCE_NETWORK_WIRE_TRANSPORT_FAILED:
        return "TRANSPORT_FAILED";
    default:
        return NULL;
    }
}

const char *appliance_network_wire_input_result_string(
    enum appliance_network_wire_input_result result)
{
    switch (result) {
    case APPLIANCE_NETWORK_WIRE_INPUT_IGNORED:
        return "IGNORED";
    case APPLIANCE_NETWORK_WIRE_INPUT_CONSUMED:
        return "CONSUMED";
    default:
        return NULL;
    }
}
