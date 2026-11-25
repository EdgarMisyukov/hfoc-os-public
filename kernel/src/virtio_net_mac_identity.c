#include "virtio_net_mac_identity.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_mac_identity_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_MAC_IDENTITY_GENERATION_MIN &&
           generation <= VIRTIO_NET_MAC_IDENTITY_GENERATION_MAX;
}

static bool virtio_net_mac_identity_mac_zero(
    const uint8_t mac_address[NETWORK_DEVICE_MAC_LENGTH])
{
    size_t index;

    for (index = 0U;
         index < NETWORK_DEVICE_MAC_LENGTH;
         ++index) {
        if (mac_address[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum virtio_net_mac_identity_status
virtio_net_mac_identity_mac_check(
    const uint8_t mac_address[NETWORK_DEVICE_MAC_LENGTH])
{
    if (virtio_net_mac_identity_mac_zero(mac_address)) {
        return VIRTIO_NET_MAC_IDENTITY_ZERO_MAC;
    }
    if ((mac_address[0] & 1U) != 0U) {
        return VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC;
    }
    return VIRTIO_NET_MAC_IDENTITY_OK;
}

static bool virtio_net_mac_identity_device_valid(
    const struct network_device_selection *selection)
{
    if (selection == NULL ||
        !selection->published ||
        selection->version != NETWORK_DEVICE_SELECTION_VERSION) {
        return false;
    }
    return network_device_selection_validate_state(
               selection,
               &selection->device,
               selection->generation) == NETWORK_DEVICE_OK;
}

static bool virtio_net_mac_identity_profile_valid(
    const struct virtio_net_feature_profile *profile)
{
    struct virtio_net_configuration configuration;
    struct virtio_net_topology topology;
    enum virtio_net_feature_profile_status status;

    if (profile == NULL ||
        !profile->published ||
        profile->version != VIRTIO_NET_FEATURE_PROFILE_VERSION) {
        return false;
    }

    memset(&configuration, 0, sizeof(configuration));
    configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    memcpy(configuration.mac_address, profile->mac_address,
           VIRTIO_NET_MAC_LENGTH);
    configuration.status = profile->status;

    memset(&topology, 0, sizeof(topology));
    topology.max_virtqueue_pairs = profile->max_virtqueue_pairs;
    topology.receive_queue_count = profile->receive_queue_count;
    topology.transmit_queue_count = profile->transmit_queue_count;
    topology.control_queue_present = profile->control_queue_present;

    status = virtio_net_feature_profile_validate_state(
        profile,
        &profile->identity,
        &profile->offered,
        &configuration,
        &topology,
        profile->generation);
    return status == VIRTIO_NET_FEATURE_PROFILE_OK;
}

static bool virtio_net_mac_identity_sources_agree(
    const struct network_device_selection *device_selection,
    const struct virtio_net_feature_profile *profile)
{
    return device_selection->device.vendor_id ==
               profile->identity.vendor_id &&
           device_selection->device.product_id ==
               profile->identity.device_id &&
           memcmp(device_selection->device.mac_address,
                  profile->mac_address,
                  NETWORK_DEVICE_MAC_LENGTH) == 0;
}

enum virtio_net_mac_identity_status
virtio_net_mac_identity_bind(
    struct virtio_net_mac_identity *binding,
    const struct network_device_selection *device_selection,
    const struct virtio_net_feature_profile *profile,
    uint32_t generation)
{
    struct virtio_net_mac_identity draft;
    enum virtio_net_mac_identity_status mac_status;

    if (binding == NULL ||
        device_selection == NULL ||
        profile == NULL) {
        return VIRTIO_NET_MAC_IDENTITY_NULL;
    }
    if (binding->published) {
        return VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED;
    }
    if (binding->version != 0U) {
        if (binding->version == VIRTIO_NET_MAC_IDENTITY_VERSION) {
            return VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED;
        }
        return VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION;
    }
    if (!device_selection->published) {
        return VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED;
    }
    if (!profile->published) {
        return VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED;
    }

    mac_status = virtio_net_mac_identity_mac_check(
        device_selection->device.mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }
    mac_status = virtio_net_mac_identity_mac_check(
        profile->mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }

    if (!virtio_net_mac_identity_device_valid(device_selection)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE;
    }
    if (!virtio_net_mac_identity_profile_valid(profile)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE;
    }
    if (!virtio_net_mac_identity_generation_valid(generation)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION;
    }

    if (!virtio_net_mac_identity_sources_agree(
            device_selection, profile)) {
        return VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH;
    }

    memset(&draft, 0, sizeof(draft));
    draft.version = VIRTIO_NET_MAC_IDENTITY_VERSION;
    draft.generation = generation;
    draft.device = *device_selection;
    draft.profile = *profile;
    draft.published = true;
    *binding = draft;
    return VIRTIO_NET_MAC_IDENTITY_OK;
}

enum virtio_net_mac_identity_status
virtio_net_mac_identity_validate_state(
    const struct virtio_net_mac_identity *binding,
    const struct network_device_selection *observed_device_selection,
    const struct virtio_net_feature_profile *observed_profile,
    uint32_t observed_generation)
{
    enum virtio_net_mac_identity_status mac_status;

    if (binding == NULL ||
        observed_device_selection == NULL ||
        observed_profile == NULL) {
        return VIRTIO_NET_MAC_IDENTITY_NULL;
    }
    if (!binding->published) {
        return VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED;
    }
    if (binding->version != VIRTIO_NET_MAC_IDENTITY_VERSION) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION;
    }
    if (!virtio_net_mac_identity_generation_valid(binding->generation)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION;
    }
    if (!observed_device_selection->published) {
        return VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED;
    }
    if (!observed_profile->published) {
        return VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED;
    }

    mac_status = virtio_net_mac_identity_mac_check(
        binding->device.device.mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }
    mac_status = virtio_net_mac_identity_mac_check(
        binding->profile.mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }
    mac_status = virtio_net_mac_identity_mac_check(
        observed_device_selection->device.mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }
    mac_status = virtio_net_mac_identity_mac_check(
        observed_profile->mac_address);
    if (mac_status != VIRTIO_NET_MAC_IDENTITY_OK) {
        return mac_status;
    }

    if (!virtio_net_mac_identity_device_valid(
            &binding->device) ||
        !virtio_net_mac_identity_device_valid(
            observed_device_selection)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE;
    }
    if (!virtio_net_mac_identity_profile_valid(
            &binding->profile) ||
        !virtio_net_mac_identity_profile_valid(
            observed_profile)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE;
    }
    if (!virtio_net_mac_identity_sources_agree(
            &binding->device, &binding->profile)) {
        return VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH;
    }
    if (!virtio_net_mac_identity_generation_valid(
            observed_generation)) {
        return VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION;
    }
    if (observed_generation != binding->generation) {
        return VIRTIO_NET_MAC_IDENTITY_CHANGED;
    }
    if (memcmp(&binding->device,
               observed_device_selection,
               sizeof(binding->device)) != 0 ||
        memcmp(&binding->profile,
               observed_profile,
               sizeof(binding->profile)) != 0) {
        return VIRTIO_NET_MAC_IDENTITY_CHANGED;
    }
    return VIRTIO_NET_MAC_IDENTITY_OK;
}

bool virtio_net_mac_identity_published(
    const struct virtio_net_mac_identity *binding)
{
    return binding != NULL &&
           binding->published &&
           binding->version == VIRTIO_NET_MAC_IDENTITY_VERSION &&
           virtio_net_mac_identity_generation_valid(binding->generation) &&
           virtio_net_mac_identity_mac_check(
               binding->device.device.mac_address) ==
                   VIRTIO_NET_MAC_IDENTITY_OK &&
           virtio_net_mac_identity_mac_check(
               binding->profile.mac_address) ==
                   VIRTIO_NET_MAC_IDENTITY_OK &&
           virtio_net_mac_identity_device_valid(&binding->device) &&
           virtio_net_mac_identity_profile_valid(&binding->profile) &&
           virtio_net_mac_identity_sources_agree(
               &binding->device, &binding->profile);
}

const struct network_device_selection *
virtio_net_mac_identity_device_selection(
    const struct virtio_net_mac_identity *binding)
{
    if (!virtio_net_mac_identity_published(binding)) {
        return NULL;
    }
    return &binding->device;
}

const struct virtio_net_feature_profile *
virtio_net_mac_identity_profile(
    const struct virtio_net_mac_identity *binding)
{
    if (!virtio_net_mac_identity_published(binding)) {
        return NULL;
    }
    return &binding->profile;
}

const char *virtio_net_mac_identity_status_string(
    enum virtio_net_mac_identity_status status)
{
    switch (status) {
    case VIRTIO_NET_MAC_IDENTITY_OK:
        return "VIRTIO_NET_MAC_IDENTITY_OK";
    case VIRTIO_NET_MAC_IDENTITY_NULL:
        return "VIRTIO_NET_MAC_IDENTITY_NULL";
    case VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION:
        return "VIRTIO_NET_MAC_IDENTITY_INVALID_VERSION";
    case VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION:
        return "VIRTIO_NET_MAC_IDENTITY_INVALID_GENERATION";
    case VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED:
        return "VIRTIO_NET_MAC_IDENTITY_ALREADY_PUBLISHED";
    case VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED:
        return "VIRTIO_NET_MAC_IDENTITY_UNPUBLISHED";
    case VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE:
        return "VIRTIO_NET_MAC_IDENTITY_INVALID_DEVICE";
    case VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED:
        return "VIRTIO_NET_MAC_IDENTITY_DEVICE_UNPUBLISHED";
    case VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE:
        return "VIRTIO_NET_MAC_IDENTITY_INVALID_PROFILE";
    case VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED:
        return "VIRTIO_NET_MAC_IDENTITY_PROFILE_UNPUBLISHED";
    case VIRTIO_NET_MAC_IDENTITY_ZERO_MAC:
        return "VIRTIO_NET_MAC_IDENTITY_ZERO_MAC";
    case VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC:
        return "VIRTIO_NET_MAC_IDENTITY_MULTICAST_MAC";
    case VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH:
        return "VIRTIO_NET_MAC_IDENTITY_DEVICE_MISMATCH";
    case VIRTIO_NET_MAC_IDENTITY_CHANGED:
        return "VIRTIO_NET_MAC_IDENTITY_CHANGED";
    default:
        return NULL;
    }
}
