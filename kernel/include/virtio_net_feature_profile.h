#ifndef HFOC_VIRTIO_NET_FEATURE_PROFILE_H
#define HFOC_VIRTIO_NET_FEATURE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"

#define VIRTIO_NET_FEATURE_PROFILE_VERSION 1U
#define VIRTIO_NET_CONFIGURATION_VERSION 1U
#define VIRTIO_NET_PROFILE_GENERATION_MIN 1U
#define VIRTIO_NET_PROFILE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_NET_MAC_LENGTH 6U
#define VIRTIO_NET_QUEUE_PAIRS_MIN 1U
#define VIRTIO_NET_QUEUE_PAIRS_MAX 1U

#define VIRTIO_NET_HEADER_MODE_LEGACY 1U
#define VIRTIO_NET_HEADER_MODE_V1 2U
#define VIRTIO_NET_HEADER_SIZE_LEGACY 10U
#define VIRTIO_NET_HEADER_SIZE_V1 12U
#define VIRTIO_NET_HEADER_FLAGS_MASK 0U
#define VIRTIO_NET_HEADER_GSO_MASK 0U

#define VIRTIO_NET_STATUS_LINK_UP 1U
#define VIRTIO_NET_STATUS_ANNOUNCE 2U
#define VIRTIO_NET_STATUS_MASK \
    (VIRTIO_NET_STATUS_LINK_UP | VIRTIO_NET_STATUS_ANNOUNCE)

#define VIRTIO_NET_F_CSUM 0U
#define VIRTIO_NET_F_GUEST_CSUM 1U
#define VIRTIO_NET_F_CTRL_GUEST_OFFLOADS 2U
#define VIRTIO_NET_F_MTU 3U
#define VIRTIO_NET_F_MAC 5U
#define VIRTIO_NET_F_GSO 6U
#define VIRTIO_NET_F_GUEST_TSO4 7U
#define VIRTIO_NET_F_GUEST_TSO6 8U
#define VIRTIO_NET_F_GUEST_ECN 9U
#define VIRTIO_NET_F_GUEST_UFO 10U
#define VIRTIO_NET_F_HOST_TSO4 11U
#define VIRTIO_NET_F_HOST_TSO6 12U
#define VIRTIO_NET_F_HOST_ECN 13U
#define VIRTIO_NET_F_HOST_UFO 14U
#define VIRTIO_NET_F_MRG_RXBUF 15U
#define VIRTIO_NET_F_STATUS 16U
#define VIRTIO_NET_F_CTRL_VQ 17U
#define VIRTIO_NET_F_CTRL_RX 18U
#define VIRTIO_NET_F_CTRL_VLAN 19U
#define VIRTIO_NET_F_CTRL_RX_EXTRA 20U
#define VIRTIO_NET_F_GUEST_ANNOUNCE 21U
#define VIRTIO_NET_F_MQ 22U
#define VIRTIO_NET_F_CTRL_MAC_ADDR 23U
#define VIRTIO_NET_F_DEVICE_STATS 50U
#define VIRTIO_NET_F_VQ_NOTF_COAL 52U
#define VIRTIO_NET_F_NOTF_COAL 53U
#define VIRTIO_NET_F_GUEST_USO4 54U
#define VIRTIO_NET_F_GUEST_USO6 55U
#define VIRTIO_NET_F_HOST_USO 56U
#define VIRTIO_NET_F_HASH_REPORT 57U
#define VIRTIO_NET_F_GUEST_HDRLEN 59U
#define VIRTIO_NET_F_RSS 60U
#define VIRTIO_NET_F_RSC_EXT 61U
#define VIRTIO_NET_F_STANDBY 62U
#define VIRTIO_NET_F_SPEED_DUPLEX 63U

#define VIRTIO_NET_KNOWN_LO_NET_MASK UINT32_C(0xFFFFEF)
#define VIRTIO_NET_KNOWN_HI_NET_MASK UINT32_C(0xF83F4000)
#define VIRTIO_NET_REQUIRED_FEATURE_LO_MASK \
    ((UINT32_C(1) << VIRTIO_NET_F_MAC) | \
     (UINT32_C(1) << VIRTIO_NET_F_STATUS))
#define VIRTIO_NET_FORBIDDEN_FEATURE_LO_MASK \
    (VIRTIO_NET_KNOWN_LO_NET_MASK & ~VIRTIO_NET_REQUIRED_FEATURE_LO_MASK)
#define VIRTIO_NET_FORBIDDEN_FEATURE_HI_MASK \
    VIRTIO_NET_KNOWN_HI_NET_MASK
#define VIRTIO_NET_FEATURE_KNOWN_LO_MASK \
    (VIRTIO_PCI_FEATURE_KNOWN_LO_MASK | VIRTIO_NET_KNOWN_LO_NET_MASK)
#define VIRTIO_NET_FEATURE_KNOWN_HI_MASK \
    (VIRTIO_PCI_FEATURE_KNOWN_HI_MASK | VIRTIO_NET_KNOWN_HI_NET_MASK)
#define VIRTIO_NET_VERSION_1_HIGH_BIT \
    (UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U))

struct virtio_net_feature_bitset {
    uint32_t low;
    uint32_t high;
};

struct virtio_net_configuration {
    uint32_t version;
    uint8_t mac_address[VIRTIO_NET_MAC_LENGTH];
    uint32_t status;
};

struct virtio_net_topology {
    uint32_t max_virtqueue_pairs;
    uint32_t receive_queue_count;
    uint32_t transmit_queue_count;
    bool control_queue_present;
};

struct virtio_net_feature_profile {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_pci_identity identity;
    uint32_t header_mode;
    uint32_t header_size;
    uint32_t header_flags_mask;
    uint32_t header_gso_mask;
    uint8_t mac_address[VIRTIO_NET_MAC_LENGTH];
    uint32_t status;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_feature_bitset accepted;
    uint32_t max_virtqueue_pairs;
    uint32_t receive_queue_count;
    uint32_t transmit_queue_count;
    bool control_queue_present;
};

enum virtio_net_feature_profile_status {
    VIRTIO_NET_FEATURE_PROFILE_OK = 0,
    VIRTIO_NET_FEATURE_PROFILE_NULL = 1,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION = 2,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION = 3,
    VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED = 5,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY = 6,
    VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE = 7,
    VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE = 8,
    VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE = 9,
    VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE = 10,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION = 11,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS = 12,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS = 13,
    VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY = 14,
    VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE = 15,
    VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE = 16,
    VIRTIO_NET_FEATURE_PROFILE_CHANGED = 17,
};

enum virtio_net_feature_profile_status
virtio_net_feature_profile_activate(
    struct virtio_net_feature_profile *profile,
    const struct virtio_pci_identity *identity,
    const struct virtio_net_feature_bitset *offered,
    const struct virtio_net_configuration *configuration,
    const struct virtio_net_topology *topology,
    uint32_t generation);
enum virtio_net_feature_profile_status
virtio_net_feature_profile_validate_state(
    const struct virtio_net_feature_profile *profile,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_net_feature_bitset *observed_offered,
    const struct virtio_net_configuration *observed_configuration,
    const struct virtio_net_topology *observed_topology,
    uint32_t observed_generation);
bool virtio_net_feature_profile_published(
    const struct virtio_net_feature_profile *profile);
const char *virtio_net_feature_profile_status_string(
    enum virtio_net_feature_profile_status status);

#endif
