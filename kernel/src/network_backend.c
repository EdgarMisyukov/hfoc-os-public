#include "network_backend.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool network_backend_generation_valid(uint32_t generation)
{
    return generation >= NETWORK_BACKEND_GENERATION_MIN &&
           generation <= NETWORK_BACKEND_GENERATION_MAX;
}

static bool network_backend_mac_zero(
    const uint8_t mac_address[NETWORK_BACKEND_MAC_LENGTH])
{
    size_t index;

    for (index = 0U; index < NETWORK_BACKEND_MAC_LENGTH; ++index) {
        if (mac_address[index] != 0U) {
            return false;
        }
    }
    return true;
}

static bool network_backend_readiness_ready(
    const struct network_backend_readiness *readiness)
{
    return readiness != NULL &&
           readiness->receive_path_ready &&
           readiness->transmit_path_ready;
}

static bool network_backend_io_valid(
    const struct network_backend_link_io *io)
{
    return io != NULL && io->read_link_up != NULL;
}

static enum network_backend_status network_backend_device_validate(
    const struct network_device_snapshot *device_snapshot)
{
    enum network_device_status device_status;

    if (device_snapshot == NULL) {
        return NETWORK_BACKEND_NULL;
    }
    device_status = network_device_snapshot_validate(device_snapshot);
    if (device_status != NETWORK_DEVICE_OK) {
        return device_status == NETWORK_DEVICE_INVALID_MAC_ADDRESS
            ? NETWORK_BACKEND_INVALID_MAC_ADDRESS
            : NETWORK_BACKEND_INVALID_DEVICE_SNAPSHOT;
    }
    if ((device_snapshot->capability_mask &
         NETWORK_BACKEND_CAPABILITY_MASK) !=
        NETWORK_BACKEND_CAPABILITY_MASK) {
        return NETWORK_BACKEND_UNSUPPORTED_CAPABILITY;
    }
    if (network_backend_mac_zero(device_snapshot->mac_address)) {
        return NETWORK_BACKEND_INVALID_MAC_ADDRESS;
    }
    if ((device_snapshot->mac_address[0] & 1U) != 0U) {
        return NETWORK_BACKEND_MULTICAST_MAC_ADDRESS;
    }
    return NETWORK_BACKEND_OK;
}

static enum network_backend_status network_backend_link_validate(
    const struct network_link_state_record *link_record,
    const struct network_device_snapshot *device_snapshot,
    bool ready)
{
    enum network_link_status link_status;

    if (link_record == NULL) {
        return NETWORK_BACKEND_NULL;
    }
    link_status = network_link_state_validate(link_record);
    if (link_status != NETWORK_LINK_OK) {
        return NETWORK_BACKEND_INVALID_LINK_STATE;
    }
    if (link_record->device_id != device_snapshot->device_id) {
        return NETWORK_BACKEND_DEVICE_MISMATCH;
    }
    switch (link_record->state) {
    case NETWORK_LINK_UP:
    case NETWORK_LINK_INITIALIZING:
        if (!ready) {
            return NETWORK_BACKEND_READINESS_MISMATCH;
        }
        return NETWORK_BACKEND_OK;
    case NETWORK_LINK_DOWN:
        return NETWORK_BACKEND_OK;
    case NETWORK_LINK_FAILED:
        if (ready) {
            return NETWORK_BACKEND_READINESS_MISMATCH;
        }
        return NETWORK_BACKEND_OK;
    default:
        return NETWORK_BACKEND_INVALID_LINK_STATE;
    }
}

/*
 * The published link record and the device's own link signal must agree.  A
 * backend that reports a link the device does not have, or hides a link it
 * does have, is refused instead of published.
 */
static enum network_backend_status network_backend_link_agreement(
    enum network_link_state state,
    bool observed_link_up)
{
    switch (state) {
    case NETWORK_LINK_UP:
        return observed_link_up ? NETWORK_BACKEND_OK
                                : NETWORK_BACKEND_LINK_DISAGREEMENT;
    case NETWORK_LINK_INITIALIZING:
    case NETWORK_LINK_DOWN:
        return observed_link_up ? NETWORK_BACKEND_LINK_DISAGREEMENT
                                : NETWORK_BACKEND_OK;
    case NETWORK_LINK_FAILED:
        return NETWORK_BACKEND_OK;
    default:
        return NETWORK_BACKEND_INVALID_LINK_STATE;
    }
}

enum network_backend_status network_backend_bind(
    struct network_backend_binding *binding,
    const struct network_device_snapshot *device_snapshot,
    const struct network_link_state_record *link_record,
    const struct network_backend_readiness *readiness,
    const struct network_backend_link_io *io,
    uint32_t generation)
{
    struct network_backend_binding draft;
    struct network_device_selection device_selection;
    struct network_link_selection link_selection;
    enum network_backend_status status;
    enum network_link_status link_status;
    bool observed_link_up = false;

    if (binding == NULL || device_snapshot == NULL ||
        link_record == NULL || readiness == NULL || io == NULL) {
        return NETWORK_BACKEND_NULL;
    }
    if (binding->published) {
        return NETWORK_BACKEND_ALREADY_PUBLISHED;
    }
    if (binding->version != 0U) {
        return NETWORK_BACKEND_INVALID_VERSION;
    }
    if (!network_backend_generation_valid(generation)) {
        return NETWORK_BACKEND_INVALID_GENERATION;
    }
    if (!network_backend_io_valid(io)) {
        return NETWORK_BACKEND_INVALID_IO;
    }
    status = network_backend_device_validate(device_snapshot);
    if (status != NETWORK_BACKEND_OK) {
        return status;
    }
    status = network_backend_link_validate(
        link_record, device_snapshot,
        network_backend_readiness_ready(readiness));
    if (status != NETWORK_BACKEND_OK) {
        return status;
    }
    if (!io->read_link_up(io->context, &observed_link_up)) {
        return NETWORK_BACKEND_IO_FAILED;
    }
    status = network_backend_link_agreement(
        link_record->state, observed_link_up);
    if (status != NETWORK_BACKEND_OK) {
        return status;
    }

    memset(&device_selection, 0, sizeof(device_selection));
    memset(&link_selection, 0, sizeof(link_selection));
    if (network_device_selection_init(
            &device_selection, device_snapshot, generation) !=
        NETWORK_DEVICE_OK) {
        return NETWORK_BACKEND_PUBLICATION_FAILED;
    }
    link_status = network_link_selection_init(
        &link_selection, link_record, generation);
    if (link_status != NETWORK_LINK_OK) {
        return NETWORK_BACKEND_PUBLICATION_FAILED;
    }

    memset(&draft, 0, sizeof(draft));
    draft.device_snapshot = *device_snapshot;
    draft.device = device_selection;
    draft.link = link_selection;
    draft.io = *io;
    draft.version = NETWORK_BACKEND_VERSION;
    draft.generation = generation;
    draft.published = true;
    *binding = draft;
    return NETWORK_BACKEND_OK;
}

bool network_backend_published(
    const struct network_backend_binding *binding)
{
    if (binding == NULL ||
        !binding->published ||
        binding->version != NETWORK_BACKEND_VERSION ||
        !network_backend_generation_valid(binding->generation)) {
        return false;
    }
    if (network_backend_device_validate(&binding->device_snapshot) !=
        NETWORK_BACKEND_OK) {
        return false;
    }
    if (network_device_selection_validate_state(
            &binding->device, &binding->device_snapshot,
            binding->generation) != NETWORK_DEVICE_OK) {
        return false;
    }
    return network_link_selection_published(&binding->link) &&
           binding->link.link.device_id ==
               binding->device_snapshot.device_id;
}

bool network_backend_link_up(
    const struct network_backend_binding *binding,
    bool *link_up)
{
    if (link_up == NULL) {
        return false;
    }
    *link_up = false;
    if (!network_backend_published(binding) ||
        !network_backend_io_valid(&binding->io)) {
        return false;
    }
    return binding->io.read_link_up(binding->io.context, link_up);
}

bool network_backend_path_enabled(
    const struct network_backend_binding *binding)
{
    if (!network_backend_published(binding)) {
        return false;
    }
    return network_link_path_status(
               &binding->link,
               &binding->device_snapshot,
               binding->link.generation,
               binding->link.link.generation) == NETWORK_LINK_OK;
}

const struct network_device_selection *network_backend_device_selection(
    const struct network_backend_binding *binding)
{
    if (!network_backend_published(binding)) {
        return NULL;
    }
    return &binding->device;
}

const struct network_link_selection *network_backend_link_selection(
    const struct network_backend_binding *binding)
{
    if (!network_backend_published(binding)) {
        return NULL;
    }
    return &binding->link;
}

const char *network_backend_status_string(
    enum network_backend_status status)
{
    switch (status) {
    case NETWORK_BACKEND_OK:
        return "NETWORK_BACKEND_OK";
    case NETWORK_BACKEND_NULL:
        return "NETWORK_BACKEND_NULL";
    case NETWORK_BACKEND_INVALID_VERSION:
        return "NETWORK_BACKEND_INVALID_VERSION";
    case NETWORK_BACKEND_INVALID_GENERATION:
        return "NETWORK_BACKEND_INVALID_GENERATION";
    case NETWORK_BACKEND_ALREADY_PUBLISHED:
        return "NETWORK_BACKEND_ALREADY_PUBLISHED";
    case NETWORK_BACKEND_UNPUBLISHED:
        return "NETWORK_BACKEND_UNPUBLISHED";
    case NETWORK_BACKEND_INVALID_DEVICE_SNAPSHOT:
        return "NETWORK_BACKEND_INVALID_DEVICE_SNAPSHOT";
    case NETWORK_BACKEND_UNSUPPORTED_CAPABILITY:
        return "NETWORK_BACKEND_UNSUPPORTED_CAPABILITY";
    case NETWORK_BACKEND_INVALID_MAC_ADDRESS:
        return "NETWORK_BACKEND_INVALID_MAC_ADDRESS";
    case NETWORK_BACKEND_MULTICAST_MAC_ADDRESS:
        return "NETWORK_BACKEND_MULTICAST_MAC_ADDRESS";
    case NETWORK_BACKEND_INVALID_LINK_STATE:
        return "NETWORK_BACKEND_INVALID_LINK_STATE";
    case NETWORK_BACKEND_DEVICE_MISMATCH:
        return "NETWORK_BACKEND_DEVICE_MISMATCH";
    case NETWORK_BACKEND_INVALID_IO:
        return "NETWORK_BACKEND_INVALID_IO";
    case NETWORK_BACKEND_IO_FAILED:
        return "NETWORK_BACKEND_IO_FAILED";
    case NETWORK_BACKEND_READINESS_MISMATCH:
        return "NETWORK_BACKEND_READINESS_MISMATCH";
    case NETWORK_BACKEND_LINK_DISAGREEMENT:
        return "NETWORK_BACKEND_LINK_DISAGREEMENT";
    case NETWORK_BACKEND_PUBLICATION_FAILED:
        return "NETWORK_BACKEND_PUBLICATION_FAILED";
    default:
        return NULL;
    }
}
