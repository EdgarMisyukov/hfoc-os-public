#include "appliance_time_wire.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void time_wire_write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)value;
}

static uint16_t time_wire_load_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint32_t time_wire_checksum_add(
    uint32_t sum,
    const uint8_t *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index + 1U < length; index += 2U) {
        sum += ((uint32_t)bytes[index] << 8U) | bytes[index + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)bytes[length - 1U] << 8U;
    }
    return sum;
}

static uint16_t time_wire_checksum_finish(uint32_t sum)
{
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static bool time_wire_endpoint_valid(
    const struct appliance_time_wire_endpoint *endpoint)
{
    return endpoint != NULL && endpoint->source_port != 0U &&
           endpoint->destination_port != 0U;
}

enum appliance_time_wire_status appliance_time_wire_build(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length_out)
{
    uint8_t *ip;
    uint8_t *udp;
    size_t frame_length;
    uint16_t udp_length;
    uint32_t udp_sum = 0U;

    if (endpoint == NULL || payload == NULL || frame == NULL ||
        frame_length_out == NULL) {
        return APPLIANCE_TIME_WIRE_NULL;
    }
    if (!time_wire_endpoint_valid(endpoint) || payload_length == 0U) {
        return APPLIANCE_TIME_WIRE_BAD_ARGUMENT;
    }
    frame_length = APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH +
                   APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH +
                   APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH +
                   payload_length;
    udp_length = (uint16_t)(APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH +
                            payload_length);
    if (frame_length > frame_capacity) {
        return APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL;
    }
    if (frame_length > UINT16_MAX) {
        return APPLIANCE_TIME_WIRE_BAD_ARGUMENT;
    }

    memset(frame, 0, frame_length);
    memcpy(frame, endpoint->next_hop_mac,
           APPLIANCE_TIME_WIRE_MAC_LENGTH);
    memcpy(frame + APPLIANCE_TIME_WIRE_MAC_LENGTH, endpoint->local_mac,
           APPLIANCE_TIME_WIRE_MAC_LENGTH);
    time_wire_write_be16(
        frame + 12U, APPLIANCE_TIME_WIRE_ETHERTYPE_IPV4);

    ip = frame + APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH;
    ip[0] = 0x45U;
    ip[1] = 0x00U;
    time_wire_write_be16(
        ip + 2U,
        (uint16_t)(APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH + udp_length));
    time_wire_write_be16(ip + 4U, 0U);
    time_wire_write_be16(ip + 6U, 0x4000U);
    ip[8] = 64U;
    ip[9] = APPLIANCE_TIME_WIRE_PROTOCOL_UDP;
    memcpy(ip + 12U, endpoint->local_address,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    memcpy(ip + 16U, endpoint->next_hop_address,
           APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    time_wire_write_be16(
        ip + 10U, time_wire_checksum_finish(time_wire_checksum_add(
                     0U, ip, APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH)));

    udp = ip + APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH;
    time_wire_write_be16(udp, endpoint->source_port);
    time_wire_write_be16(udp + 2U, endpoint->destination_port);
    time_wire_write_be16(udp + 4U, udp_length);
    memcpy(udp + APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH, payload,
           payload_length);

    udp_sum = time_wire_checksum_add(0U, endpoint->local_address,
                                     APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    udp_sum = time_wire_checksum_add(
        udp_sum, endpoint->next_hop_address,
        APPLIANCE_TIME_WIRE_ADDRESS_LENGTH);
    udp_sum += APPLIANCE_TIME_WIRE_PROTOCOL_UDP;
    udp_sum += udp_length;
    udp_sum = time_wire_checksum_add(udp_sum, udp, udp_length);
    time_wire_write_be16(udp + 6U, time_wire_checksum_finish(udp_sum));

    *frame_length_out = frame_length;
    return APPLIANCE_TIME_WIRE_OK;
}

enum appliance_time_wire_status appliance_time_wire_parse(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *frame,
    size_t frame_length,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_length_out)
{
    const uint8_t *ip;
    const uint8_t *udp;
    size_t ip_header_length;
    size_t udp_length;
    size_t payload_length;
    uint16_t ethertype;

    if (endpoint == NULL || frame == NULL || payload == NULL ||
        payload_length_out == NULL) {
        return APPLIANCE_TIME_WIRE_NULL;
    }
    if (!time_wire_endpoint_valid(endpoint)) {
        return APPLIANCE_TIME_WIRE_BAD_ARGUMENT;
    }
    if (frame_length < APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH +
                           APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH +
                           APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    ethertype = time_wire_load_be16(frame + 12U);
    if (ethertype != APPLIANCE_TIME_WIRE_ETHERTYPE_IPV4) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    ip = frame + APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH;
    if ((ip[0] >> 4U) != 4U || (ip[0] & 0x0FU) != 5U ||
        ip[9] != APPLIANCE_TIME_WIRE_PROTOCOL_UDP) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    ip_header_length = (size_t)(ip[0] & 0x0FU) * 4U;
    if (ip_header_length < APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH ||
        frame_length <
            APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH +
                ip_header_length +
                APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    if (memcmp(ip + 16U, endpoint->local_address,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) != 0) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    /*
     * ITEM-654: accept the reply only from the source this client selected.
     * An explicit server is not silently substituted by any other host that
     * happens to answer on UDP/123.
     */
    if (memcmp(ip + 12U, endpoint->next_hop_address,
               APPLIANCE_TIME_WIRE_ADDRESS_LENGTH) != 0) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    udp = ip + ip_header_length;
    if (time_wire_load_be16(udp + 2U) != endpoint->source_port ||
        time_wire_load_be16(udp) != endpoint->destination_port) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    udp_length = time_wire_load_be16(udp + 4U);
    if (udp_length < APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH ||
        udp_length > frame_length -
                         APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH -
                         ip_header_length) {
        return APPLIANCE_TIME_WIRE_BAD_FRAME;
    }
    payload_length = udp_length - APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH;
    if (payload_length > payload_capacity) {
        return APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL;
    }
    memcpy(payload, udp + APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH,
           payload_length);
    *payload_length_out = payload_length;
    return APPLIANCE_TIME_WIRE_OK;
}

const char *appliance_time_wire_status_string(
    enum appliance_time_wire_status status)
{
    switch (status) {
    case APPLIANCE_TIME_WIRE_OK:
        return "OK";
    case APPLIANCE_TIME_WIRE_NULL:
        return "NULL";
    case APPLIANCE_TIME_WIRE_BAD_ARGUMENT:
        return "BAD_ARGUMENT";
    case APPLIANCE_TIME_WIRE_BAD_FRAME:
        return "BAD_FRAME";
    case APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL:
        return "BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}
