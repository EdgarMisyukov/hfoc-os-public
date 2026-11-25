#include "virtio_net_feature_profile.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_feature_profile_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_PROFILE_GENERATION_MIN &&
           generation <= VIRTIO_NET_PROFILE_GENERATION_MAX;
}

static bool virtio_net_feature_bitset_known(
    const struct virtio_net_feature_bitset *bitset)
{
    if ((bitset->low & ~VIRTIO_NET_FEATURE_KNOWN_LO_MASK) != 0U) {
        return false;
    }
    if ((bitset->high & ~VIRTIO_NET_FEATURE_KNOWN_HI_MASK) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_net_feature_bitset_subset(
    const struct virtio_net_feature_bitset *subset,
    const struct virtio_net_feature_bitset *superset)
{
    if ((subset->low & ~superset->low) != 0U) {
        return false;
    }
    if ((subset->high & ~superset->high) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_net_feature_bitset_equal(
    const struct virtio_net_feature_bitset *left,
    const struct virtio_net_feature_bitset *right)
{
    return left->low == right->low && left->high == right->high;
}

static bool virtio_net_mac_address_valid(
    const uint8_t mac_address[VIRTIO_NET_MAC_LENGTH])
{
    size_t index;
    bool non_zero = false;

    for (index = 0U; index < VIRTIO_NET_MAC_LENGTH; ++index) {
        if (mac_address[index] != 0U) {
            non_zero = true;
            break;
        }
    }
    return non_zero;
}

static bool virtio_net_identity_is_net(
    const struct virtio_pci_identity *identity)
{
    return identity->device_id == VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL ||
           identity->device_id == VIRTIO_PCI_DEVICE_ID_NET_MODERN;
}

static struct virtio_net_feature_bitset
virtio_net_required_features(uint8_t modern)
{
    struct virtio_net_feature_bitset required;

    required.low = VIRTIO_NET_REQUIRED_FEATURE_LO_MASK;
    required.high = modern != 0U ? VIRTIO_NET_VERSION_1_HIGH_BIT : 0U;
    return required;
}

static enum virtio_net_feature_profile_status
virtio_net_feature_policy_validate(
    const struct virtio_pci_identity *identity,
    const struct virtio_net_feature_bitset *offered)
{
    struct virtio_net_feature_bitset required;

    if (!virtio_net_feature_bitset_known(offered)) {
        return VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE;
    }

    required = virtio_net_required_features(identity->modern);
    if (!virtio_net_feature_bitset_subset(&required, offered)) {
        return VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE;
    }
    if ((offered->low & VIRTIO_NET_FORBIDDEN_FEATURE_LO_MASK) != 0U ||
        (offered->high & VIRTIO_NET_FORBIDDEN_FEATURE_HI_MASK) != 0U) {
        return VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE;
    }
    if (identity->modern == 0U &&
        (offered->high & VIRTIO_NET_VERSION_1_HIGH_BIT) != 0U) {
        return VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE;
    }
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

static enum virtio_net_feature_profile_status
virtio_net_configuration_fields_validate(
    const uint8_t mac_address[VIRTIO_NET_MAC_LENGTH],
    uint32_t status)
{
    if (!virtio_net_mac_address_valid(mac_address)) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS;
    }
    if (status != VIRTIO_NET_STATUS_LINK_UP) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS;
    }
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

static enum virtio_net_feature_profile_status
virtio_net_topology_fields_validate(
    uint32_t max_virtqueue_pairs,
    uint32_t receive_queue_count,
    uint32_t transmit_queue_count,
    bool control_queue_present)
{
    if (max_virtqueue_pairs < VIRTIO_NET_QUEUE_PAIRS_MIN) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY;
    }
    if (max_virtqueue_pairs > VIRTIO_NET_QUEUE_PAIRS_MAX) {
        return VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE;
    }
    if (receive_queue_count != VIRTIO_NET_QUEUE_PAIRS_MAX ||
        transmit_queue_count != VIRTIO_NET_QUEUE_PAIRS_MAX) {
        return VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE;
    }
    if (control_queue_present) {
        return VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE;
    }
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

static enum virtio_net_feature_profile_status
virtio_net_configuration_validate(
    const struct virtio_net_configuration *configuration)
{
    if (configuration->version != VIRTIO_NET_CONFIGURATION_VERSION) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION;
    }
    return virtio_net_configuration_fields_validate(
        configuration->mac_address, configuration->status);
}

static enum virtio_net_feature_profile_status
virtio_net_topology_validate(
    const struct virtio_net_topology *topology)
{
    return virtio_net_topology_fields_validate(
        topology->max_virtqueue_pairs,
        topology->receive_queue_count,
        topology->transmit_queue_count,
        topology->control_queue_present);
}

static enum virtio_net_feature_profile_status
virtio_net_feature_profile_validate_stored(
    const struct virtio_net_feature_profile *profile)
{
    enum virtio_net_feature_profile_status status;
    enum virtio_pci_status pci_status;
    struct virtio_net_feature_bitset required;
    uint32_t expected_header_mode;
    uint32_t expected_header_size;

    if (profile->version != VIRTIO_NET_FEATURE_PROFILE_VERSION) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION;
    }
    if (!virtio_net_feature_profile_generation_valid(profile->generation)) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION;
    }

    pci_status = virtio_pci_identity_validate(&profile->identity);
    if (pci_status != VIRTIO_PCI_OK) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY;
    }
    if (!virtio_net_identity_is_net(&profile->identity)) {
        return VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE;
    }
    if (!virtio_net_feature_bitset_known(&profile->offered) ||
        !virtio_net_feature_bitset_known(&profile->accepted)) {
        return VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE;
    }

    required = virtio_net_required_features(profile->identity.modern);
    if (!virtio_net_feature_bitset_equal(
            &profile->accepted, &required)) {
        return VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE;
    }

    if (profile->identity.modern != 0U) {
        expected_header_mode = VIRTIO_NET_HEADER_MODE_V1;
        expected_header_size = VIRTIO_NET_HEADER_SIZE_V1;
    } else {
        expected_header_mode = VIRTIO_NET_HEADER_MODE_LEGACY;
        expected_header_size = VIRTIO_NET_HEADER_SIZE_LEGACY;
    }
    if (profile->header_mode != expected_header_mode ||
        profile->header_size != expected_header_size ||
        profile->header_flags_mask != VIRTIO_NET_HEADER_FLAGS_MASK ||
        profile->header_gso_mask != VIRTIO_NET_HEADER_GSO_MASK) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION;
    }

    status = virtio_net_configuration_fields_validate(
        profile->mac_address, profile->status);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    status = virtio_net_topology_fields_validate(
        profile->max_virtqueue_pairs,
        profile->receive_queue_count,
        profile->transmit_queue_count,
        profile->control_queue_present);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

enum virtio_net_feature_profile_status
virtio_net_feature_profile_activate(
    struct virtio_net_feature_profile *profile,
    const struct virtio_pci_identity *identity,
    const struct virtio_net_feature_bitset *offered,
    const struct virtio_net_configuration *configuration,
    const struct virtio_net_topology *topology,
    uint32_t generation)
{
    enum virtio_net_feature_profile_status status;
    enum virtio_pci_status pci_status;
    struct virtio_net_feature_bitset required;

    if (profile == NULL || identity == NULL || offered == NULL ||
        configuration == NULL || topology == NULL) {
        return VIRTIO_NET_FEATURE_PROFILE_NULL;
    }
    if (profile->published) {
        return VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED;
    }
    if (profile->version != 0U) {
        if (profile->version == VIRTIO_NET_FEATURE_PROFILE_VERSION) {
            return VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED;
        }
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION;
    }

    pci_status = virtio_pci_identity_validate(identity);
    if (pci_status != VIRTIO_PCI_OK) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY;
    }
    if (!virtio_net_identity_is_net(identity)) {
        return VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE;
    }

    status = virtio_net_feature_policy_validate(identity, offered);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    status = virtio_net_configuration_validate(configuration);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    status = virtio_net_topology_validate(topology);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    if (!virtio_net_feature_profile_generation_valid(generation)) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION;
    }

    required = virtio_net_required_features(identity->modern);
    profile->published = true;
    profile->version = VIRTIO_NET_FEATURE_PROFILE_VERSION;
    profile->generation = generation;
    profile->identity = *identity;
    if (identity->modern != 0U) {
        profile->header_mode = VIRTIO_NET_HEADER_MODE_V1;
        profile->header_size = VIRTIO_NET_HEADER_SIZE_V1;
    } else {
        profile->header_mode = VIRTIO_NET_HEADER_MODE_LEGACY;
        profile->header_size = VIRTIO_NET_HEADER_SIZE_LEGACY;
    }
    profile->header_flags_mask = VIRTIO_NET_HEADER_FLAGS_MASK;
    profile->header_gso_mask = VIRTIO_NET_HEADER_GSO_MASK;
    memcpy(profile->mac_address, configuration->mac_address,
           VIRTIO_NET_MAC_LENGTH);
    profile->status = configuration->status;
    profile->offered = *offered;
    profile->accepted = required;
    profile->max_virtqueue_pairs = topology->max_virtqueue_pairs;
    profile->receive_queue_count = topology->receive_queue_count;
    profile->transmit_queue_count = topology->transmit_queue_count;
    profile->control_queue_present = topology->control_queue_present;
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

enum virtio_net_feature_profile_status
virtio_net_feature_profile_validate_state(
    const struct virtio_net_feature_profile *profile,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_net_feature_bitset *observed_offered,
    const struct virtio_net_configuration *observed_configuration,
    const struct virtio_net_topology *observed_topology,
    uint32_t observed_generation)
{
    enum virtio_net_feature_profile_status status;

    if (profile == NULL || observed_identity == NULL ||
        observed_offered == NULL || observed_configuration == NULL ||
        observed_topology == NULL) {
        return VIRTIO_NET_FEATURE_PROFILE_NULL;
    }
    if (!profile->published) {
        return VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED;
    }

    status = virtio_net_feature_profile_validate_stored(profile);
    if (status != VIRTIO_NET_FEATURE_PROFILE_OK) {
        return status;
    }
    if (!virtio_net_feature_profile_generation_valid(observed_generation)) {
        return VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION;
    }
    if (observed_generation != profile->generation) {
        return VIRTIO_NET_FEATURE_PROFILE_CHANGED;
    }
    if (memcmp(observed_identity, &profile->identity,
               sizeof(*observed_identity)) != 0 ||
        !virtio_net_feature_bitset_equal(
            observed_offered, &profile->offered) ||
        memcmp(observed_configuration->mac_address, profile->mac_address,
               VIRTIO_NET_MAC_LENGTH) != 0 ||
        observed_configuration->status != profile->status ||
        observed_configuration->version !=
            VIRTIO_NET_CONFIGURATION_VERSION ||
        observed_topology->max_virtqueue_pairs !=
            profile->max_virtqueue_pairs ||
        observed_topology->receive_queue_count !=
            profile->receive_queue_count ||
        observed_topology->transmit_queue_count !=
            profile->transmit_queue_count ||
        observed_topology->control_queue_present !=
            profile->control_queue_present) {
        return VIRTIO_NET_FEATURE_PROFILE_CHANGED;
    }
    return VIRTIO_NET_FEATURE_PROFILE_OK;
}

bool virtio_net_feature_profile_published(
    const struct virtio_net_feature_profile *profile)
{
    return profile != NULL &&
           profile->published &&
           profile->version == VIRTIO_NET_FEATURE_PROFILE_VERSION;
}

const char *virtio_net_feature_profile_status_string(
    enum virtio_net_feature_profile_status status)
{
    switch (status) {
    case VIRTIO_NET_FEATURE_PROFILE_OK:
        return "VIRTIO_NET_FEATURE_PROFILE_OK";
    case VIRTIO_NET_FEATURE_PROFILE_NULL:
        return "VIRTIO_NET_FEATURE_PROFILE_NULL";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_VERSION";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_GENERATION";
    case VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED:
        return "VIRTIO_NET_FEATURE_PROFILE_ALREADY_PUBLISHED";
    case VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED:
        return "VIRTIO_NET_FEATURE_PROFILE_UNPUBLISHED";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_IDENTITY";
    case VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE:
        return "VIRTIO_NET_FEATURE_PROFILE_NOT_NET_DEVICE";
    case VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE:
        return "VIRTIO_NET_FEATURE_PROFILE_UNKNOWN_FEATURE";
    case VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE:
        return "VIRTIO_NET_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE";
    case VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE:
        return "VIRTIO_NET_FEATURE_PROFILE_UNSUPPORTED_FEATURE";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_CONFIGURATION";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_MAC_ADDRESS";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_STATUS";
    case VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY:
        return "VIRTIO_NET_FEATURE_PROFILE_INVALID_TOPOLOGY";
    case VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE:
        return "VIRTIO_NET_FEATURE_PROFILE_MULTIQUEUE";
    case VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE:
        return "VIRTIO_NET_FEATURE_PROFILE_CONTROL_QUEUE";
    case VIRTIO_NET_FEATURE_PROFILE_CHANGED:
        return "VIRTIO_NET_FEATURE_PROFILE_CHANGED";
    default:
        return NULL;
    }
}
