#ifndef HFOC_NETWORK_BACKEND_H
#define HFOC_NETWORK_BACKEND_H

#include "link_state.h"
#include "network_device.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-389 backend-neutral production network binding.
 *
 * ITEM-362A bound the production lwIP path to two virtio-owned records: the
 * virtio feature profile (whose validation requires a virtio PCI identity) and
 * the virtio link state (whose readiness names a virtqueue composition and an
 * interrupt).  A second production backend for the on-board Realtek NIC has
 * neither a virtqueue nor an interrupt, and claiming them would be a false
 * publication.
 *
 * This module is therefore the deliberate backend-neutral extension of that
 * surface.  network_device.h stays the single device-identity model: the
 * binding carries the same validated network_device_snapshot plus a published
 * network_link_selection, and its readiness names what a poll-driven backend
 * really publishes - an enabled receive path and an enabled transmit path.
 * The observed link state is a plain boolean read from the backend's own link
 * register, not a virtio status word.
 */

#define NETWORK_BACKEND_VERSION 1U
#define NETWORK_BACKEND_GENERATION_MIN 1U
#define NETWORK_BACKEND_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define NETWORK_BACKEND_MAC_LENGTH NETWORK_DEVICE_MAC_LENGTH
#define NETWORK_BACKEND_DEVICE_ID NETWORK_DEVICE_ID_MIN
#define NETWORK_BACKEND_CAPABILITY_MASK \
    (NETWORK_DEVICE_CAPABILITY_RX | \
     NETWORK_DEVICE_CAPABILITY_TX | \
     NETWORK_DEVICE_CAPABILITY_LINK_STATE)

struct network_backend_link_io {
    /* Read the device's own link-up signal. */
    bool (*read_link_up)(void *context, bool *link_up);
    void *context;
};

/*
 * What the backend has actually published before the link is claimed up.  A
 * poll-driven backend asserts its receive and transmit paths are programmed
 * and enabled; it publishes no virtqueue and no interrupt.
 */
struct network_backend_readiness {
    bool receive_path_ready;
    bool transmit_path_ready;
};

struct network_backend_binding {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_device_snapshot device_snapshot;
    struct network_device_selection device;
    struct network_link_selection link;
    struct network_backend_link_io io;
};

enum network_backend_status {
    NETWORK_BACKEND_OK = 0,
    NETWORK_BACKEND_NULL = 1,
    NETWORK_BACKEND_INVALID_VERSION = 2,
    NETWORK_BACKEND_INVALID_GENERATION = 3,
    NETWORK_BACKEND_ALREADY_PUBLISHED = 4,
    NETWORK_BACKEND_UNPUBLISHED = 5,
    NETWORK_BACKEND_INVALID_DEVICE_SNAPSHOT = 6,
    NETWORK_BACKEND_UNSUPPORTED_CAPABILITY = 7,
    NETWORK_BACKEND_INVALID_MAC_ADDRESS = 8,
    NETWORK_BACKEND_MULTICAST_MAC_ADDRESS = 9,
    NETWORK_BACKEND_INVALID_LINK_STATE = 10,
    NETWORK_BACKEND_DEVICE_MISMATCH = 11,
    NETWORK_BACKEND_INVALID_IO = 12,
    NETWORK_BACKEND_IO_FAILED = 13,
    NETWORK_BACKEND_READINESS_MISMATCH = 14,
    NETWORK_BACKEND_LINK_DISAGREEMENT = 15,
    NETWORK_BACKEND_PUBLICATION_FAILED = 16,
};

enum network_backend_status network_backend_bind(
    struct network_backend_binding *binding,
    const struct network_device_snapshot *device_snapshot,
    const struct network_link_state_record *link_record,
    const struct network_backend_readiness *readiness,
    const struct network_backend_link_io *io,
    uint32_t generation);
bool network_backend_published(
    const struct network_backend_binding *binding);
bool network_backend_link_up(
    const struct network_backend_binding *binding,
    bool *link_up);
bool network_backend_path_enabled(
    const struct network_backend_binding *binding);
const struct network_device_selection *network_backend_device_selection(
    const struct network_backend_binding *binding);
const struct network_link_selection *network_backend_link_selection(
    const struct network_backend_binding *binding);
const char *network_backend_status_string(
    enum network_backend_status status);

#endif
