#include "link_state.h"

#include <stddef.h>
#include <stdint.h>

static bool network_link_generation_valid(uint32_t generation)
{
    return generation >= NETWORK_LINK_GENERATION_MIN &&
           generation <= NETWORK_LINK_GENERATION_MAX;
}

enum network_link_status network_link_state_validate(
    const struct network_link_state_record *state)
{
    if (state == NULL) {
        return NETWORK_LINK_NULL;
    }
    if (state->version != NETWORK_LINK_STATE_VERSION) {
        return NETWORK_LINK_INVALID_VERSION;
    }
    if (state->device_id < NETWORK_DEVICE_ID_MIN ||
        state->device_id > NETWORK_DEVICE_ID_MAX) {
        return NETWORK_LINK_INVALID_DEVICE_ID;
    }
    if ((unsigned int)state->state >= NETWORK_LINK_STATE_COUNT) {
        return NETWORK_LINK_INVALID_STATE;
    }
    if (!network_link_generation_valid(state->generation)) {
        return NETWORK_LINK_INVALID_GENERATION;
    }
    return NETWORK_LINK_OK;
}

enum network_link_status network_link_selection_init(
    struct network_link_selection *selection,
    const struct network_link_state_record *state,
    uint32_t generation)
{
    enum network_link_status status;

    if (selection == NULL || state == NULL) {
        return NETWORK_LINK_NULL;
    }
    if (selection->published) {
        return NETWORK_LINK_ALREADY_PUBLISHED;
    }
    status = network_link_state_validate(state);
    if (status != NETWORK_LINK_OK) {
        return status;
    }
    if (!network_link_generation_valid(generation)) {
        return NETWORK_LINK_INVALID_GENERATION;
    }

    selection->published = true;
    selection->version = NETWORK_LINK_SELECTION_VERSION;
    selection->generation = generation;
    selection->link = *state;
    return NETWORK_LINK_OK;
}

bool network_link_selection_published(
    const struct network_link_selection *selection)
{
    return selection != NULL && selection->published;
}

enum network_link_status network_link_path_status(
    const struct network_link_selection *selection,
    const struct network_device_snapshot *device_snapshot,
    uint32_t observed_device_generation,
    uint32_t observed_link_generation)
{
    enum network_link_status status;
    enum network_device_status device_status;

    if (selection == NULL || device_snapshot == NULL) {
        return NETWORK_LINK_NULL;
    }
    if (!selection->published) {
        return NETWORK_LINK_UNPUBLISHED;
    }
    if (selection->version != NETWORK_LINK_SELECTION_VERSION) {
        return NETWORK_LINK_INVALID_VERSION;
    }
    if (!network_link_generation_valid(selection->generation)) {
        return NETWORK_LINK_INVALID_GENERATION;
    }
    status = network_link_state_validate(&selection->link);
    if (status != NETWORK_LINK_OK) {
        return status;
    }

    device_status = network_device_snapshot_validate(device_snapshot);
    if (device_status != NETWORK_DEVICE_OK) {
        return NETWORK_LINK_INVALID_DEVICE_SNAPSHOT;
    }
    if ((device_snapshot->capability_mask &
         NETWORK_DEVICE_CAPABILITY_LINK_STATE) == 0U) {
        return NETWORK_LINK_UNSUPPORTED;
    }
    if (device_snapshot->device_id != selection->link.device_id) {
        return NETWORK_LINK_DEVICE_MISMATCH;
    }
    if (!network_link_generation_valid(observed_device_generation) ||
        !network_link_generation_valid(observed_link_generation)) {
        return NETWORK_LINK_INVALID_GENERATION;
    }
    if (observed_device_generation != selection->generation) {
        return NETWORK_LINK_GENERATION_MISMATCH;
    }
    if (observed_link_generation != selection->link.generation) {
        return NETWORK_LINK_GENERATION_MISMATCH;
    }
    if (selection->link.state != NETWORK_LINK_UP) {
        return NETWORK_LINK_NOT_UP;
    }
    return NETWORK_LINK_OK;
}

bool network_link_path_enabled(
    const struct network_link_selection *selection,
    const struct network_device_snapshot *device_snapshot,
    uint32_t observed_device_generation,
    uint32_t observed_link_generation)
{
    return network_link_path_status(
               selection, device_snapshot,
               observed_device_generation,
               observed_link_generation) == NETWORK_LINK_OK;
}

const char *network_link_status_string(
    enum network_link_status status)
{
    switch (status) {
    case NETWORK_LINK_OK:
        return "NETWORK_LINK_OK";
    case NETWORK_LINK_NULL:
        return "NETWORK_LINK_NULL";
    case NETWORK_LINK_INVALID_VERSION:
        return "NETWORK_LINK_INVALID_VERSION";
    case NETWORK_LINK_INVALID_DEVICE_ID:
        return "NETWORK_LINK_INVALID_DEVICE_ID";
    case NETWORK_LINK_INVALID_STATE:
        return "NETWORK_LINK_INVALID_STATE";
    case NETWORK_LINK_INVALID_GENERATION:
        return "NETWORK_LINK_INVALID_GENERATION";
    case NETWORK_LINK_ALREADY_PUBLISHED:
        return "NETWORK_LINK_ALREADY_PUBLISHED";
    case NETWORK_LINK_UNPUBLISHED:
        return "NETWORK_LINK_UNPUBLISHED";
    case NETWORK_LINK_INVALID_DEVICE_SNAPSHOT:
        return "NETWORK_LINK_INVALID_DEVICE_SNAPSHOT";
    case NETWORK_LINK_DEVICE_MISMATCH:
        return "NETWORK_LINK_DEVICE_MISMATCH";
    case NETWORK_LINK_GENERATION_MISMATCH:
        return "NETWORK_LINK_GENERATION_MISMATCH";
    case NETWORK_LINK_UNSUPPORTED:
        return "NETWORK_LINK_UNSUPPORTED";
    case NETWORK_LINK_NOT_UP:
        return "NETWORK_LINK_NOT_UP";
    default:
        return NULL;
    }
}
