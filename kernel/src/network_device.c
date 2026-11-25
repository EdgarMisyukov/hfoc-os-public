#include "network_device.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool network_device_mac_address_valid(
    const uint8_t mac_address[NETWORK_DEVICE_MAC_LENGTH])
{
    size_t index;
    bool non_zero = false;

    for (index = 0U; index < NETWORK_DEVICE_MAC_LENGTH; ++index) {
        if (mac_address[index] != 0U) {
            non_zero = true;
            break;
        }
    }
    return non_zero;
}

enum network_device_status network_device_snapshot_validate(
    const struct network_device_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return NETWORK_DEVICE_NULL;
    }
    if (snapshot->version != NETWORK_DEVICE_SNAPSHOT_VERSION) {
        return NETWORK_DEVICE_INVALID_VERSION;
    }
    if (snapshot->device_id < NETWORK_DEVICE_ID_MIN ||
        snapshot->device_id > NETWORK_DEVICE_ID_MAX) {
        return NETWORK_DEVICE_INVALID_ID;
    }
    if (snapshot->vendor_id < NETWORK_DEVICE_VENDOR_ID_MIN) {
        return NETWORK_DEVICE_INVALID_VENDOR_ID;
    }
    if (snapshot->product_id < NETWORK_DEVICE_PRODUCT_ID_MIN) {
        return NETWORK_DEVICE_INVALID_PRODUCT_ID;
    }
    if (snapshot->bus_address > NETWORK_DEVICE_BUS_ADDRESS_MAX) {
        return NETWORK_DEVICE_INVALID_BUS_ADDRESS;
    }
    if (!network_device_mac_address_valid(snapshot->mac_address)) {
        return NETWORK_DEVICE_INVALID_MAC_ADDRESS;
    }
    if (snapshot->capability_mask == 0U ||
        (snapshot->capability_mask &
         ~NETWORK_DEVICE_CAPABILITY_MASK) != 0U) {
        return NETWORK_DEVICE_UNSUPPORTED_CAPABILITY;
    }
    return NETWORK_DEVICE_OK;
}

enum network_device_status network_device_snapshot_publish(
    struct network_device_snapshot *published,
    const struct network_device_snapshot *snapshot)
{
    enum network_device_status status;

    if (published == NULL || snapshot == NULL) {
        return NETWORK_DEVICE_NULL;
    }
    status = network_device_snapshot_validate(snapshot);
    if (status != NETWORK_DEVICE_OK) {
        return status;
    }
    if (published != snapshot) {
        *published = *snapshot;
    }
    return NETWORK_DEVICE_OK;
}

enum network_device_status network_device_selection_init(
    struct network_device_selection *selection,
    const struct network_device_snapshot *snapshot,
    uint32_t generation)
{
    enum network_device_status status;

    if (selection == NULL || snapshot == NULL) {
        return NETWORK_DEVICE_NULL;
    }
    if (selection->published) {
        return NETWORK_DEVICE_ALREADY_PUBLISHED;
    }
    status = network_device_snapshot_validate(snapshot);
    if (status != NETWORK_DEVICE_OK) {
        return status;
    }
    if (generation < NETWORK_DEVICE_GENERATION_MIN ||
        generation > NETWORK_DEVICE_GENERATION_MAX) {
        return NETWORK_DEVICE_BAD_GENERATION;
    }

    selection->published = true;
    selection->version = NETWORK_DEVICE_SELECTION_VERSION;
    selection->generation = generation;
    selection->device = *snapshot;
    return NETWORK_DEVICE_OK;
}

enum network_device_status network_device_selection_validate_state(
    const struct network_device_selection *selection,
    const struct network_device_snapshot *observed_snapshot,
    uint32_t observed_generation)
{
    enum network_device_status status;

    if (selection == NULL || observed_snapshot == NULL) {
        return NETWORK_DEVICE_NULL;
    }
    if (!selection->published) {
        return NETWORK_DEVICE_UNPUBLISHED;
    }
    if (selection->version != NETWORK_DEVICE_SELECTION_VERSION) {
        return NETWORK_DEVICE_INVALID_VERSION;
    }
    status = network_device_snapshot_validate(&selection->device);
    if (status != NETWORK_DEVICE_OK) {
        return status;
    }
    status = network_device_snapshot_validate(observed_snapshot);
    if (status != NETWORK_DEVICE_OK) {
        return status;
    }
    if (observed_generation < NETWORK_DEVICE_GENERATION_MIN ||
        observed_generation > NETWORK_DEVICE_GENERATION_MAX) {
        return NETWORK_DEVICE_BAD_GENERATION;
    }
    if (selection->generation != observed_generation) {
        return NETWORK_DEVICE_CHANGED;
    }
    if (memcmp(&selection->device, observed_snapshot,
               sizeof(selection->device)) != 0) {
        return NETWORK_DEVICE_CHANGED;
    }
    return NETWORK_DEVICE_OK;
}

bool network_device_selection_published(
    const struct network_device_selection *selection)
{
    return selection != NULL && selection->published;
}

const char *network_device_status_string(
    enum network_device_status status)
{
    switch (status) {
    case NETWORK_DEVICE_OK:
        return "NETWORK_DEVICE_OK";
    case NETWORK_DEVICE_NULL:
        return "NETWORK_DEVICE_NULL";
    case NETWORK_DEVICE_INVALID_VERSION:
        return "NETWORK_DEVICE_INVALID_VERSION";
    case NETWORK_DEVICE_INVALID_ID:
        return "NETWORK_DEVICE_INVALID_ID";
    case NETWORK_DEVICE_INVALID_VENDOR_ID:
        return "NETWORK_DEVICE_INVALID_VENDOR_ID";
    case NETWORK_DEVICE_INVALID_PRODUCT_ID:
        return "NETWORK_DEVICE_INVALID_PRODUCT_ID";
    case NETWORK_DEVICE_INVALID_BUS_ADDRESS:
        return "NETWORK_DEVICE_INVALID_BUS_ADDRESS";
    case NETWORK_DEVICE_INVALID_MAC_ADDRESS:
        return "NETWORK_DEVICE_INVALID_MAC_ADDRESS";
    case NETWORK_DEVICE_UNSUPPORTED_CAPABILITY:
        return "NETWORK_DEVICE_UNSUPPORTED_CAPABILITY";
    case NETWORK_DEVICE_BAD_GENERATION:
        return "NETWORK_DEVICE_BAD_GENERATION";
    case NETWORK_DEVICE_ALREADY_PUBLISHED:
        return "NETWORK_DEVICE_ALREADY_PUBLISHED";
    case NETWORK_DEVICE_UNPUBLISHED:
        return "NETWORK_DEVICE_UNPUBLISHED";
    case NETWORK_DEVICE_CHANGED:
        return "NETWORK_DEVICE_CHANGED";
    default:
        return NULL;
    }
}
