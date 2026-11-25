#include "ethernet_frame.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const uint8_t *ethernet_frame_slice_bytes(
    const struct packet_slice *slice)
{
    size_t slot_size =
        (size_t)slice->pool->headroom +
        (size_t)slice->pool->capacity;
    uintptr_t base = (uintptr_t)slice->pool->storage +
        (uintptr_t)slice->slot.slot * slot_size;

    return (const uint8_t *)(base + (uintptr_t)slice->start);
}

static bool ethernet_frame_ether_type_allowed_by_policy(
    const struct ethernet_frame_policy *policy,
    uint16_t ether_type)
{
    uint16_t index;

    for (index = 0U;
         index < policy->allowed_ether_type_count;
         ++index) {
        if (policy->allowed_ether_types[index] == ether_type) {
            return true;
        }
    }
    return false;
}

static bool ethernet_frame_policy_ether_type_valid(
    uint16_t ether_type)
{
    return ether_type > ETHERNET_FRAME_ETHERTYPE_802_3_MAX &&
           ether_type != ETHERNET_FRAME_ETHERTYPE_VLAN &&
           ether_type != ETHERNET_FRAME_ETHERTYPE_QINQ;
}

static bool ethernet_frame_policy_valid(
    const struct ethernet_frame_policy *policy)
{
    uint16_t first;

    if (policy->allowed_ether_type_count >
            ETHERNET_FRAME_ALLOWED_TYPES_MAX ||
        (policy->allowed_ether_type_count != 0U &&
         policy->allowed_ether_types == NULL)) {
        return false;
    }
    for (first = 0U;
         first < policy->allowed_ether_type_count;
         ++first) {
        uint16_t second;

        if (!ethernet_frame_policy_ether_type_valid(
                policy->allowed_ether_types[first])) {
            return false;
        }
        for (second = 0U; second < first; ++second) {
            if (policy->allowed_ether_types[second] ==
                policy->allowed_ether_types[first]) {
                return false;
            }
        }
    }
    return true;
}

static uint16_t ethernet_frame_read_ether_type(
    const uint8_t *frame)
{
    return (uint16_t)(
        ((uint16_t)frame[12] << 8U) |
        (uint16_t)frame[13]);
}

static bool ethernet_frame_read_destination(
    const uint8_t *frame,
    enum ethernet_frame_destination *destination)
{
    bool all_zero = true;
    bool all_ff = true;
    uint32_t index;

    for (index = 0U;
         index < ETHERNET_FRAME_DESTINATION_LENGTH;
         ++index) {
        if (frame[index] != 0U) {
            all_zero = false;
        }
        if (frame[index] != 0xFFU) {
            all_ff = false;
        }
    }
    if (all_zero) {
        return false;
    }
    if (all_ff) {
        *destination = ETHERNET_FRAME_DESTINATION_BROADCAST;
    } else if ((frame[0] & 1U) != 0U) {
        *destination = ETHERNET_FRAME_DESTINATION_MULTICAST;
    } else {
        *destination = ETHERNET_FRAME_DESTINATION_UNICAST;
    }
    return true;
}

static bool ethernet_frame_destination_allowed(
    const struct ethernet_frame_policy *policy,
    enum ethernet_frame_destination destination)
{
    switch (destination) {
    case ETHERNET_FRAME_DESTINATION_UNICAST:
        return policy->accept_unicast;
    case ETHERNET_FRAME_DESTINATION_BROADCAST:
        return policy->accept_broadcast;
    case ETHERNET_FRAME_DESTINATION_MULTICAST:
        return policy->accept_multicast;
    default:
        return false;
    }
}

enum ethernet_frame_status ethernet_frame_validate(
    const struct packet_slice *slice,
    const struct ethernet_frame_policy *policy,
    uint32_t payload_length,
    struct ethernet_frame_view *view)
{
    const uint8_t *frame;
    enum ethernet_frame_destination destination;
    uint16_t ether_type;
    uint32_t padding_length;

    if (slice == NULL || policy == NULL || view == NULL) {
        return ETHERNET_FRAME_NULL;
    }
    if (!packet_slice_is_valid(slice)) {
        return ETHERNET_FRAME_BAD_SLICE;
    }
    if (policy->version != ETHERNET_FRAME_POLICY_VERSION) {
        return ETHERNET_FRAME_BAD_VERSION;
    }
    if (!ethernet_frame_policy_valid(policy)) {
        return ETHERNET_FRAME_BAD_POLICY;
    }
    if (slice->length < ETHERNET_FRAME_MIN_LENGTH) {
        return ETHERNET_FRAME_SHORT_FRAME;
    }
    if (slice->length > ETHERNET_FRAME_MAX_LENGTH) {
        return ETHERNET_FRAME_OVERSIZED_FRAME;
    }
    if (payload_length > ETHERNET_FRAME_PAYLOAD_MAX) {
        return ETHERNET_FRAME_BAD_PAYLOAD_LENGTH;
    }

    frame = ethernet_frame_slice_bytes(slice);
    if (!ethernet_frame_read_destination(frame, &destination)) {
        return ETHERNET_FRAME_BAD_DESTINATION;
    }
    if (!ethernet_frame_destination_allowed(
            policy, destination)) {
        return ETHERNET_FRAME_DESTINATION_DENIED;
    }

    ether_type = ethernet_frame_read_ether_type(frame);
    if (ether_type <= ETHERNET_FRAME_ETHERTYPE_802_3_MAX) {
        return ETHERNET_FRAME_BAD_ETHERTYPE;
    }
    if (!ethernet_frame_ether_type_allowed_by_policy(
            policy, ether_type)) {
        return ETHERNET_FRAME_ETHERTYPE_DENIED;
    }

    if (payload_length >
        slice->length - ETHERNET_FRAME_HEADER_LENGTH) {
        return ETHERNET_FRAME_BAD_PADDING;
    }
    padding_length = slice->length -
        ETHERNET_FRAME_HEADER_LENGTH - payload_length;
    if (padding_length > ETHERNET_FRAME_PADDING_MAX) {
        return ETHERNET_FRAME_BAD_PADDING;
    }

    view->version = ETHERNET_FRAME_VERSION;
    view->destination = destination;
    view->ether_type = ether_type;
    view->payload_offset = ETHERNET_FRAME_HEADER_LENGTH;
    view->payload_length = payload_length;
    view->padding_length = padding_length;
    return ETHERNET_FRAME_OK;
}

const char *ethernet_frame_status_string(
    enum ethernet_frame_status status)
{
    switch (status) {
    case ETHERNET_FRAME_OK:
        return "ETHERNET_FRAME_OK";
    case ETHERNET_FRAME_NULL:
        return "ETHERNET_FRAME_NULL";
    case ETHERNET_FRAME_BAD_SLICE:
        return "ETHERNET_FRAME_BAD_SLICE";
    case ETHERNET_FRAME_BAD_POLICY:
        return "ETHERNET_FRAME_BAD_POLICY";
    case ETHERNET_FRAME_BAD_VERSION:
        return "ETHERNET_FRAME_BAD_VERSION";
    case ETHERNET_FRAME_SHORT_FRAME:
        return "ETHERNET_FRAME_SHORT_FRAME";
    case ETHERNET_FRAME_OVERSIZED_FRAME:
        return "ETHERNET_FRAME_OVERSIZED_FRAME";
    case ETHERNET_FRAME_BAD_DESTINATION:
        return "ETHERNET_FRAME_BAD_DESTINATION";
    case ETHERNET_FRAME_DESTINATION_DENIED:
        return "ETHERNET_FRAME_DESTINATION_DENIED";
    case ETHERNET_FRAME_BAD_ETHERTYPE:
        return "ETHERNET_FRAME_BAD_ETHERTYPE";
    case ETHERNET_FRAME_ETHERTYPE_DENIED:
        return "ETHERNET_FRAME_ETHERTYPE_DENIED";
    case ETHERNET_FRAME_BAD_PAYLOAD_LENGTH:
        return "ETHERNET_FRAME_BAD_PAYLOAD_LENGTH";
    case ETHERNET_FRAME_BAD_PADDING:
        return "ETHERNET_FRAME_BAD_PADDING";
    default:
        return NULL;
    }
}

const char *ethernet_frame_destination_string(
    enum ethernet_frame_destination destination)
{
    switch (destination) {
    case ETHERNET_FRAME_DESTINATION_UNICAST:
        return "unicast";
    case ETHERNET_FRAME_DESTINATION_BROADCAST:
        return "broadcast";
    case ETHERNET_FRAME_DESTINATION_MULTICAST:
        return "multicast";
    default:
        return NULL;
    }
}
