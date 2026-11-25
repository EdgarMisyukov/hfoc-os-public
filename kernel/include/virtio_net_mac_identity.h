#ifndef HFOC_VIRTIO_NET_MAC_IDENTITY_H
#define HFOC_VIRTIO_NET_MAC_IDENTITY_H

#include "network_device.h"
#include "virtio_net_feature_profile.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_MAC_IDENTITY_VERSION 1U
#define VIRTIO_NET_MAC_IDENTITY_GENERATION_MIN 1U
#define VIRTIO_NET_MAC_IDENTITY_GENERATION_MAX UINT32_C(0xFFFFFFFE)

struct virtio_net_mac_identity {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_device_selection device;
    struct virtio_net_feature_profile profile;
};

enum virtio_net_mac_identity_status {
    VIRTIO_NET_MAC_IDENTITY_OK = 0,
    VIRTIO_NET_MAC_IDENTITY_NULL = 1,
    VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION = 2,
    VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION = 3,
    VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED = 5,
    VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE = 6,
    VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED = 7,
    VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE = 8,
    VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED = 9,
    VIRTIO_NET_MAC_IDENTITY_ZERO_MAC = 10,
    VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC = 11,
    VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH = 12,
    VIRTIO_NET_MAC_IDENTITY_CHANGED = 13,
};

enum virtio_net_mac_identity_status
virtio_net_mac_identity_bind(
    struct virtio_net_mac_identity *binding,
    const struct network_device_selection *device_selection,
    const struct virtio_net_feature_profile *profile,
    uint32_t generation);
enum virtio_net_mac_identity_status
virtio_net_mac_identity_validate_state(
    const struct virtio_net_mac_identity *binding,
    const struct network_device_selection *observed_device_selection,
    const struct virtio_net_feature_profile *observed_profile,
    uint32_t observed_generation);
bool virtio_net_mac_identity_published(
    const struct virtio_net_mac_identity *binding);
const struct network_device_selection *
virtio_net_mac_identity_device_selection(
    const struct virtio_net_mac_identity *binding);
const struct virtio_net_feature_profile *
virtio_net_mac_identity_profile(
    const struct virtio_net_mac_identity *binding);
const char *virtio_net_mac_identity_status_string(
    enum virtio_net_mac_identity_status status);

#endif
