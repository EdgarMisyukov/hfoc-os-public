#ifndef HFOC_VIRTIO_NET_LIFECYCLE_H
#define HFOC_VIRTIO_NET_LIFECYCLE_H

#include "virtio_net_interrupt.h"
#include "virtio_net_link_state.h"
#include "virtio_net_queue_composition.h"
#include "virtio_net_rx.h"
#include "virtio_net_tx.h"
#include "virtio_pci_device_status.h"

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_LIFECYCLE_VERSION 1U
#define VIRTIO_NET_LIFECYCLE_GENERATION_MIN 1U
#define VIRTIO_NET_LIFECYCLE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

enum virtio_net_lifecycle_state {
    VIRTIO_NET_LIFECYCLE_STATE_UNPUBLISHED = 0,
    VIRTIO_NET_LIFECYCLE_STATE_ACTIVE = 1,
    VIRTIO_NET_LIFECYCLE_STATE_STOPPED = 2,
};

struct virtio_net_notification_io {
    bool (*quiesce)(void *context);
    void *context;
};

struct virtio_net_lifecycle {
    bool published;
    uint32_t version;
    uint32_t generation;
    enum virtio_net_lifecycle_state state;
    struct virtio_net_link_state *link_state;
    struct virtio_net_interrupt *interrupt;
    struct virtio_net_rx *rx;
    struct virtio_net_tx *tx;
    struct virtio_net_queue_composition *composition;
    struct virtio_pci_device_status_controller *device_status;
    struct virtio_net_notification_io notifications;
};

enum virtio_net_lifecycle_status {
    VIRTIO_NET_LIFECYCLE_OK = 0,
    VIRTIO_NET_LIFECYCLE_NULL = 1,
    VIRTIO_NET_LIFECYCLE_INVALID_VERSION = 2,
    VIRTIO_NET_LIFECYCLE_INVALID_GENERATION = 3,
    VIRTIO_NET_LIFECYCLE_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_LIFECYCLE_UNPUBLISHED = 5,
    VIRTIO_NET_LIFECYCLE_INVALID_NOTIFICATIONS = 6,
    VIRTIO_NET_LIFECYCLE_INVALID_LINK_STATE = 7,
    VIRTIO_NET_LIFECYCLE_INVALID_INTERRUPT = 8,
    VIRTIO_NET_LIFECYCLE_INVALID_RX = 9,
    VIRTIO_NET_LIFECYCLE_INVALID_TX = 10,
    VIRTIO_NET_LIFECYCLE_INVALID_COMPOSITION = 11,
    VIRTIO_NET_LIFECYCLE_INVALID_DEVICE_STATUS = 12,
    VIRTIO_NET_LIFECYCLE_DEVICE_NOT_LIVE = 13,
    VIRTIO_NET_LIFECYCLE_GENERATION_MISMATCH = 14,
    VIRTIO_NET_LIFECYCLE_NOTIFICATIONS_FAILED = 15,
    VIRTIO_NET_LIFECYCLE_ADMISSION_STOP_FAILED = 16,
    VIRTIO_NET_LIFECYCLE_RX_TEARDOWN_FAILED = 17,
    VIRTIO_NET_LIFECYCLE_TX_TEARDOWN_FAILED = 18,
    VIRTIO_NET_LIFECYCLE_DEVICE_RESET_FAILED = 19,
    VIRTIO_NET_LIFECYCLE_QUEUE_TEARDOWN_FAILED = 20,
    VIRTIO_NET_LIFECYCLE_INVALID_STATE = 21,
};

enum virtio_net_lifecycle_status virtio_net_lifecycle_activate(
    struct virtio_net_lifecycle *lifecycle,
    struct virtio_net_link_state *link_state,
    struct virtio_net_interrupt *interrupt,
    struct virtio_net_rx *rx,
    struct virtio_net_tx *tx,
    struct virtio_net_queue_composition *composition,
    struct virtio_pci_device_status_controller *device_status,
    const struct virtio_net_notification_io *notifications,
    uint32_t generation);
enum virtio_net_lifecycle_status virtio_net_lifecycle_stop(
    struct virtio_net_lifecycle *lifecycle);
bool virtio_net_lifecycle_published(
    const struct virtio_net_lifecycle *lifecycle);
bool virtio_net_lifecycle_stopped(
    const struct virtio_net_lifecycle *lifecycle);
uint32_t virtio_net_lifecycle_generation(
    const struct virtio_net_lifecycle *lifecycle);
const char *virtio_net_lifecycle_status_string(
    enum virtio_net_lifecycle_status status);

#endif
