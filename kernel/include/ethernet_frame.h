#ifndef HFOC_ETHERNET_FRAME_H
#define HFOC_ETHERNET_FRAME_H

#include <stdbool.h>
#include <stdint.h>

#include "packet_slice.h"

#define ETHERNET_FRAME_VERSION 1U
#define ETHERNET_FRAME_POLICY_VERSION 1U
#define ETHERNET_FRAME_HEADER_LENGTH 14U
#define ETHERNET_FRAME_DESTINATION_LENGTH 6U
#define ETHERNET_FRAME_SOURCE_LENGTH 6U
#define ETHERNET_FRAME_MIN_LENGTH 60U
#define ETHERNET_FRAME_MAX_LENGTH 1518U
#define ETHERNET_FRAME_PAYLOAD_MAX 1500U
#define ETHERNET_FRAME_PADDING_MAX 46U
#define ETHERNET_FRAME_ALLOWED_TYPES_MAX 8U
#define ETHERNET_FRAME_ETHERTYPE_802_3_MAX 0x05DCU
#define ETHERNET_FRAME_ETHERTYPE_IPV4 0x0800U
#define ETHERNET_FRAME_ETHERTYPE_ARP 0x0806U
#define ETHERNET_FRAME_ETHERTYPE_VLAN 0x8100U
#define ETHERNET_FRAME_ETHERTYPE_QINQ 0x88A8U

enum ethernet_frame_destination {
    ETHERNET_FRAME_DESTINATION_UNICAST = 0,
    ETHERNET_FRAME_DESTINATION_BROADCAST = 1,
    ETHERNET_FRAME_DESTINATION_MULTICAST = 2,
};

enum ethernet_frame_status {
    ETHERNET_FRAME_OK = 0,
    ETHERNET_FRAME_NULL = 1,
    ETHERNET_FRAME_BAD_SLICE = 2,
    ETHERNET_FRAME_BAD_POLICY = 3,
    ETHERNET_FRAME_BAD_VERSION = 4,
    ETHERNET_FRAME_SHORT_FRAME = 5,
    ETHERNET_FRAME_OVERSIZED_FRAME = 6,
    ETHERNET_FRAME_BAD_DESTINATION = 7,
    ETHERNET_FRAME_DESTINATION_DENIED = 8,
    ETHERNET_FRAME_BAD_ETHERTYPE = 9,
    ETHERNET_FRAME_ETHERTYPE_DENIED = 10,
    ETHERNET_FRAME_BAD_PAYLOAD_LENGTH = 11,
    ETHERNET_FRAME_BAD_PADDING = 12,
};

struct ethernet_frame_policy {
    uint32_t version;
    bool accept_unicast;
    bool accept_broadcast;
    bool accept_multicast;
    const uint16_t *allowed_ether_types;
    uint16_t allowed_ether_type_count;
};

struct ethernet_frame_view {
    uint32_t version;
    enum ethernet_frame_destination destination;
    uint16_t ether_type;
    uint32_t payload_offset;
    uint32_t payload_length;
    uint32_t padding_length;
};

enum ethernet_frame_status ethernet_frame_validate(
    const struct packet_slice *slice,
    const struct ethernet_frame_policy *policy,
    uint32_t payload_length,
    struct ethernet_frame_view *view);
const char *ethernet_frame_status_string(
    enum ethernet_frame_status status);
const char *ethernet_frame_destination_string(
    enum ethernet_frame_destination destination);

#endif
