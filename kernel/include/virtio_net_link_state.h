#ifndef HFOC_VIRTIO_NET_LINK_STATE_H
#define HFOC_VIRTIO_NET_LINK_STATE_H

#include "link_state.h"
#include "network_device.h"
#include "virtio_net_feature_profile.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_LINK_STATE_VERSION 1U
#define VIRTIO_NET_LINK_STATE_GENERATION_MIN 1U
#define VIRTIO_NET_LINK_STATE_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define VIRTIO_NET_LINK_DEVICE_ID 1U
#define VIRTIO_NET_LINK_CAPABILITY_MASK \
    (NETWORK_DEVICE_CAPABILITY_RX | \
     NETWORK_DEVICE_CAPABILITY_TX | \
     NETWORK_DEVICE_CAPABILITY_LINK_STATE)
#define VIRTIO_NET_LINK_STATUS_MASK VIRTIO_NET_STATUS_MASK

struct virtio_net_link_io {
    bool (*read_config_status)(void *context, uint32_t *status);
    void *context;
};

struct virtio_net_link_readiness {
    bool profile_published;
    bool queue_composition_published;
    bool interrupt_published;
};

struct virtio_net_link_state {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_device_snapshot device_snapshot;
    struct network_link_selection link;
    struct virtio_net_link_io io;
};

enum virtio_net_link_status {
    VIRTIO_NET_LINK_OK = 0,
    VIRTIO_NET_LINK_NULL = 1,
    VIRTIO_NET_LINK_INVALID_VERSION = 2,
    VIRTIO_NET_LINK_INVALID_GENERATION = 3,
    VIRTIO_NET_LINK_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_LINK_UNPUBLISHED = 5,
    VIRTIO_NET_LINK_INVALID_IO = 6,
    VIRTIO_NET_LINK_IO_FAILED = 7,
    VIRTIO_NET_LINK_INVALID_DEVICE_STATUS = 8,
    VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH = 9,
    VIRTIO_NET_LINK_INVALID_STATE = 10,
    VIRTIO_NET_LINK_DEVICE_MISMATCH = 11,
    VIRTIO_NET_LINK_PUBLICATION_FAILED = 12,
    VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT = 13,
};

enum virtio_net_link_status virtio_net_link_state_activate(
    struct virtio_net_link_state *binding,
    const struct network_device_snapshot *device_snapshot,
    const struct virtio_net_link_readiness *readiness,
    const struct virtio_net_link_io *io,
    const struct network_link_state_record *state,
    uint32_t observed_device_generation,
    uint32_t generation);
enum virtio_net_link_status virtio_net_link_state_transition(
    struct virtio_net_link_state *binding,
    const struct virtio_net_link_readiness *readiness,
    const struct network_link_state_record *state,
    uint32_t observed_device_generation);
bool virtio_net_link_state_published(
    const struct virtio_net_link_state *binding);
const struct network_link_selection *
virtio_net_link_state_selection(
    const struct virtio_net_link_state *binding);
bool virtio_net_link_state_path_enabled(
    const struct virtio_net_link_state *binding);
const char *virtio_net_link_status_string(
    enum virtio_net_link_status status);

#endif
