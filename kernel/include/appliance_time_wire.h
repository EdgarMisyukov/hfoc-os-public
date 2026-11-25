#ifndef HFOC_APPLIANCE_TIME_WIRE_H
#define HFOC_APPLIANCE_TIME_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-374 SNTP wire framing.
 *
 * The time client is transport-agnostic; this module owns the Ethernet,
 * IPv4 and UDP framing for its 48-byte SNTP payloads so the production wire
 * owner only has to resolve the next hop and transmit the frame.  A reply is
 * accepted only when the frame is IPv4/UDP, addressed to the local endpoint
 * and sourced from the SNTP port; every malformed frame is refused.
 */

#define APPLIANCE_TIME_WIRE_VERSION 1U
#define APPLIANCE_TIME_WIRE_FRAME_MAX 128U
#define APPLIANCE_TIME_WIRE_ADDRESS_LENGTH 4U
#define APPLIANCE_TIME_WIRE_MAC_LENGTH 6U
#define APPLIANCE_TIME_WIRE_ETHERTYPE_IPV4 0x0800U
#define APPLIANCE_TIME_WIRE_PROTOCOL_UDP 17U
#define APPLIANCE_TIME_WIRE_IPV4_HEADER_LENGTH 20U
#define APPLIANCE_TIME_WIRE_UDP_HEADER_LENGTH 8U
#define APPLIANCE_TIME_WIRE_ETHERNET_HEADER_LENGTH 14U
#define APPLIANCE_TIME_WIRE_SOURCE_PORT_MIN 49152U
#define APPLIANCE_TIME_WIRE_SOURCE_PORT_MAX 65535U

enum appliance_time_wire_status {
    APPLIANCE_TIME_WIRE_OK = 0,
    APPLIANCE_TIME_WIRE_NULL = 1,
    APPLIANCE_TIME_WIRE_BAD_ARGUMENT = 2,
    APPLIANCE_TIME_WIRE_BAD_FRAME = 3,
    APPLIANCE_TIME_WIRE_BUFFER_TOO_SMALL = 4,
};

struct appliance_time_wire_endpoint {
    uint8_t local_mac[APPLIANCE_TIME_WIRE_MAC_LENGTH];
    uint8_t local_address[APPLIANCE_TIME_WIRE_ADDRESS_LENGTH];
    uint8_t next_hop_mac[APPLIANCE_TIME_WIRE_MAC_LENGTH];
    uint8_t next_hop_address[APPLIANCE_TIME_WIRE_ADDRESS_LENGTH];
    uint16_t source_port;
    uint16_t destination_port;
};

enum appliance_time_wire_status appliance_time_wire_build(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *payload,
    size_t payload_length,
    uint8_t *frame,
    size_t frame_capacity,
    size_t *frame_length_out);
enum appliance_time_wire_status appliance_time_wire_parse(
    const struct appliance_time_wire_endpoint *endpoint,
    const uint8_t *frame,
    size_t frame_length,
    uint8_t *payload,
    size_t payload_capacity,
    size_t *payload_length_out);
const char *appliance_time_wire_status_string(
    enum appliance_time_wire_status status);

#endif
