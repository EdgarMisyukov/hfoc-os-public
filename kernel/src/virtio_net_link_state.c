#include "virtio_net_link_state.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_link_state_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_LINK_STATE_GENERATION_MIN &&
           generation <= VIRTIO_NET_LINK_STATE_GENERATION_MAX;
}

static bool virtio_net_link_state_io_valid(
    const struct virtio_net_link_io *io)
{
    return io != NULL &&
           io->read_config_status != NULL &&
           io->context != NULL;
}

static bool virtio_net_link_state_readiness_ready(
    const struct virtio_net_link_readiness *readiness)
{
    return readiness != NULL &&
           readiness->profile_published &&
           readiness->queue_composition_published &&
           readiness->interrupt_published;
}

static enum virtio_net_link_status
virtio_net_link_state_device_validate(
    const struct network_device_snapshot *device_snapshot)
{
    enum network_device_status device_status;

    if (device_snapshot == NULL) {
        return VIRTIO_NET_LINK_NULL;
    }
    device_status = network_device_snapshot_validate(device_snapshot);
    if (device_status != NETWORK_DEVICE_OK ||
        (device_snapshot->capability_mask &
         VIRTIO_NET_LINK_CAPABILITY_MASK) !=
            VIRTIO_NET_LINK_CAPABILITY_MASK) {
        return VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT;
    }
    return VIRTIO_NET_LINK_OK;
}

static enum virtio_net_link_status
virtio_net_link_state_agreement(
    enum network_link_state state,
    uint32_t status,
    bool ready)
{
    bool status_invalid;
    bool link_up;

    status_invalid =
        (status & ~VIRTIO_NET_LINK_STATUS_MASK) != 0U;
    link_up =
        (status & VIRTIO_NET_STATUS_LINK_UP) != 0U;

    if (status_invalid) {
        if (state == NETWORK_LINK_FAILED) {
            return VIRTIO_NET_LINK_OK;
        }
        return VIRTIO_NET_LINK_INVALID_DEVICE_STATUS;
    }

    switch (state) {
    case NETWORK_LINK_UP:
        if (!ready) {
            return VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH;
        }
        if (!link_up) {
            return VIRTIO_NET_LINK_INVALID_DEVICE_STATUS;
        }
        return VIRTIO_NET_LINK_OK;
    case NETWORK_LINK_INITIALIZING:
        if (!ready) {
            return VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH;
        }
        if (link_up) {
            return VIRTIO_NET_LINK_INVALID_DEVICE_STATUS;
        }
        return VIRTIO_NET_LINK_OK;
    case NETWORK_LINK_DOWN:
        if (link_up) {
            return VIRTIO_NET_LINK_INVALID_DEVICE_STATUS;
        }
        return VIRTIO_NET_LINK_OK;
    case NETWORK_LINK_FAILED:
        if (ready) {
            return VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH;
        }
        return VIRTIO_NET_LINK_OK;
    default:
        return VIRTIO_NET_LINK_INVALID_STATE;
    }
}

static enum virtio_net_link_status
virtio_net_link_state_validate_state(
    const struct network_link_state_record *state,
    const struct network_device_snapshot *device_snapshot)
{
    enum network_link_status link_status;

    if (state == NULL || device_snapshot == NULL) {
        return VIRTIO_NET_LINK_NULL;
    }
    link_status = network_link_state_validate(state);
    if (link_status != NETWORK_LINK_OK) {
        return VIRTIO_NET_LINK_INVALID_STATE;
    }
    if (state->device_id != device_snapshot->device_id) {
        return VIRTIO_NET_LINK_DEVICE_MISMATCH;
    }
    return VIRTIO_NET_LINK_OK;
}

enum virtio_net_link_status virtio_net_link_state_activate(
    struct virtio_net_link_state *binding,
    const struct network_device_snapshot *device_snapshot,
    const struct virtio_net_link_readiness *readiness,
    const struct virtio_net_link_io *io,
    const struct network_link_state_record *state,
    uint32_t observed_device_generation,
    uint32_t generation)
{
    struct virtio_net_link_state draft;
    enum virtio_net_link_status status;
    enum network_link_status link_status;
    uint32_t observed_status;

    if (binding == NULL ||
        device_snapshot == NULL ||
        readiness == NULL ||
        io == NULL ||
        state == NULL) {
        return VIRTIO_NET_LINK_NULL;
    }
    if (binding->published) {
        return VIRTIO_NET_LINK_ALREADY_PUBLISHED;
    }
    if (binding->version != 0U) {
        return VIRTIO_NET_LINK_INVALID_VERSION;
    }
    if (!virtio_net_link_state_generation_valid(generation) ||
        !virtio_net_link_state_generation_valid(
            observed_device_generation)) {
        return VIRTIO_NET_LINK_INVALID_GENERATION;
    }
    if (!virtio_net_link_state_io_valid(io)) {
        return VIRTIO_NET_LINK_INVALID_IO;
    }
    if (!virtio_net_link_state_readiness_ready(readiness)) {
        return VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH;
    }
    status = virtio_net_link_state_device_validate(device_snapshot);
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }
    status = virtio_net_link_state_validate_state(
        state, device_snapshot);
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }
    if (!io->read_config_status(io->context, &observed_status)) {
        return VIRTIO_NET_LINK_IO_FAILED;
    }
    status = virtio_net_link_state_agreement(
        state->state, observed_status, true);
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }

    memset(&draft, 0, sizeof(draft));
    draft.device_snapshot = *device_snapshot;
    draft.io = *io;
    draft.generation = generation;
    draft.version = VIRTIO_NET_LINK_STATE_VERSION;
    link_status = network_link_selection_init(
        &draft.link, state, observed_device_generation);
    if (link_status != NETWORK_LINK_OK) {
        return VIRTIO_NET_LINK_PUBLICATION_FAILED;
    }
    draft.published = true;
    *binding = draft;
    return VIRTIO_NET_LINK_OK;
}

enum virtio_net_link_status virtio_net_link_state_transition(
    struct virtio_net_link_state *binding,
    const struct virtio_net_link_readiness *readiness,
    const struct network_link_state_record *state,
    uint32_t observed_device_generation)
{
    struct virtio_net_link_state draft;
    enum virtio_net_link_status status;
    enum network_link_status link_status;
    uint32_t observed_status;

    if (binding == NULL ||
        readiness == NULL ||
        state == NULL) {
        return VIRTIO_NET_LINK_NULL;
    }
    if (!binding->published) {
        return VIRTIO_NET_LINK_UNPUBLISHED;
    }
    if (binding->version != VIRTIO_NET_LINK_STATE_VERSION) {
        return VIRTIO_NET_LINK_INVALID_VERSION;
    }
    if (!virtio_net_link_state_generation_valid(binding->generation) ||
        !virtio_net_link_state_generation_valid(
            observed_device_generation)) {
        return VIRTIO_NET_LINK_INVALID_GENERATION;
    }
    if (!virtio_net_link_state_io_valid(&binding->io)) {
        return VIRTIO_NET_LINK_INVALID_IO;
    }
    status = virtio_net_link_state_device_validate(
        &binding->device_snapshot);
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }
    status = virtio_net_link_state_validate_state(
        state, &binding->device_snapshot);
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }
    if (!binding->io.read_config_status(
            binding->io.context, &observed_status)) {
        return VIRTIO_NET_LINK_IO_FAILED;
    }
    status = virtio_net_link_state_agreement(
        state->state,
        observed_status,
        virtio_net_link_state_readiness_ready(readiness));
    if (status != VIRTIO_NET_LINK_OK) {
        return status;
    }

    draft = *binding;
    memset(&draft.link, 0, sizeof(draft.link));
    link_status = network_link_selection_init(
        &draft.link, state, observed_device_generation);
    if (link_status != NETWORK_LINK_OK) {
        return VIRTIO_NET_LINK_PUBLICATION_FAILED;
    }
    *binding = draft;
    return VIRTIO_NET_LINK_OK;
}

bool virtio_net_link_state_published(
    const struct virtio_net_link_state *binding)
{
    return binding != NULL &&
           binding->published &&
           binding->version == VIRTIO_NET_LINK_STATE_VERSION &&
           virtio_net_link_state_generation_valid(binding->generation);
}

const struct network_link_selection *
virtio_net_link_state_selection(
    const struct virtio_net_link_state *binding)
{
    if (!virtio_net_link_state_published(binding)) {
        return NULL;
    }
    return &binding->link;
}

bool virtio_net_link_state_path_enabled(
    const struct virtio_net_link_state *binding)
{
    if (!virtio_net_link_state_published(binding)) {
        return false;
    }
    return network_link_path_status(
               &binding->link,
               &binding->device_snapshot,
               binding->link.generation,
               binding->link.link.generation) == NETWORK_LINK_OK;
}

const char *virtio_net_link_status_string(
    enum virtio_net_link_status status)
{
    switch (status) {
    case VIRTIO_NET_LINK_OK:
        return "VIRTIO_NET_LINK_OK";
    case VIRTIO_NET_LINK_NULL:
        return "VIRTIO_NET_LINK_NULL";
    case VIRTIO_NET_LINK_INVALID_VERSION:
        return "VIRTIO_NET_LINK_INVALID_VERSION";
    case VIRTIO_NET_LINK_INVALID_GENERATION:
        return "VIRTIO_NET_LINK_INVALID_GENERATION";
    case VIRTIO_NET_LINK_ALREADY_PUBLISHED:
        return "VIRTIO_NET_LINK_ALREADY_PUBLISHED";
    case VIRTIO_NET_LINK_UNPUBLISHED:
        return "VIRTIO_NET_LINK_UNPUBLISHED";
    case VIRTIO_NET_LINK_INVALID_IO:
        return "VIRTIO_NET_LINK_INVALID_IO";
    case VIRTIO_NET_LINK_IO_FAILED:
        return "VIRTIO_NET_LINK_IO_FAILED";
    case VIRTIO_NET_LINK_INVALID_DEVICE_STATUS:
        return "VIRTIO_NET_LINK_INVALID_DEVICE_STATUS";
    case VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH:
        return "VIRTIO_NET_LINK_QUEUE_READINESS_MISMATCH";
    case VIRTIO_NET_LINK_INVALID_STATE:
        return "VIRTIO_NET_LINK_INVALID_STATE";
    case VIRTIO_NET_LINK_DEVICE_MISMATCH:
        return "VIRTIO_NET_LINK_DEVICE_MISMATCH";
    case VIRTIO_NET_LINK_PUBLICATION_FAILED:
        return "VIRTIO_NET_LINK_PUBLICATION_FAILED";
    case VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT:
        return "VIRTIO_NET_LINK_INVALID_DEVICE_SNAPSHOT";
    default:
        return NULL;
    }
}
