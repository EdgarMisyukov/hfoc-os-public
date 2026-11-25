#include "virtio_rng_feature_profile.h"

#include <stddef.h>
#include <stdint.h>

static bool virtio_rng_feature_profile_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_RNG_PROFILE_GENERATION_MIN &&
           generation <= VIRTIO_RNG_PROFILE_GENERATION_MAX;
}

static bool virtio_rng_feature_bitset_known(
    const struct virtio_rng_feature_bitset *bitset)
{
    if ((bitset->low & ~VIRTIO_RNG_FEATURE_KNOWN_LO_MASK) != 0U) {
        return false;
    }
    if ((bitset->high & ~VIRTIO_RNG_FEATURE_KNOWN_HI_MASK) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_rng_feature_bitset_subset(
    const struct virtio_rng_feature_bitset *subset,
    const struct virtio_rng_feature_bitset *superset)
{
    if ((subset->low & ~superset->low) != 0U) {
        return false;
    }
    if ((subset->high & ~superset->high) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_rng_feature_bitset_equal(
    const struct virtio_rng_feature_bitset *left,
    const struct virtio_rng_feature_bitset *right)
{
    return left->low == right->low && left->high == right->high;
}

static bool virtio_rng_identity_is_rng(
    const struct virtio_pci_identity *identity)
{
    return identity->device_id ==
               VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL ||
           identity->device_id == VIRTIO_PCI_DEVICE_ID_RNG_MODERN;
}

static bool virtio_rng_identity_equal(
    const struct virtio_pci_identity *left,
    const struct virtio_pci_identity *right)
{
    return left->version == right->version &&
           left->vendor_id == right->vendor_id &&
           left->device_id == right->device_id &&
           left->bdf.bus == right->bdf.bus &&
           left->bdf.device == right->bdf.device &&
           left->bdf.function == right->bdf.function &&
           left->modern == right->modern;
}

static struct virtio_rng_feature_bitset
virtio_rng_required_features(uint8_t modern)
{
    struct virtio_rng_feature_bitset required;

    required.low = VIRTIO_RNG_REQUIRED_FEATURE_LO_MASK;
    required.high = modern != 0U ? VIRTIO_RNG_VERSION_1_HIGH_BIT : 0U;
    return required;
}

static enum virtio_rng_feature_profile_status
virtio_rng_feature_policy_validate(
    const struct virtio_pci_identity *identity,
    const struct virtio_rng_feature_bitset *offered)
{
    struct virtio_rng_feature_bitset required;

    if (!virtio_rng_feature_bitset_known(offered)) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE;
    }

    required = virtio_rng_required_features(identity->modern);
    if (!virtio_rng_feature_bitset_subset(&required, offered)) {
        return VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE;
    }
    if (identity->modern == 0U &&
        (offered->high & VIRTIO_RNG_VERSION_1_HIGH_BIT) != 0U) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE;
    }
    return VIRTIO_RNG_FEATURE_PROFILE_OK;
}

static enum virtio_rng_feature_profile_status
virtio_rng_topology_validate(const struct virtio_rng_topology *topology)
{
    if (topology->version != VIRTIO_RNG_TOPOLOGY_VERSION) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY;
    }
    if (topology->request_queue_count <
            VIRTIO_RNG_REQUEST_QUEUE_COUNT_MIN ||
        topology->request_queue_count >
            VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT;
    }
    if (topology->buffer_writer != VIRTIO_RNG_BUFFER_WRITER_DEVICE) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER;
    }
    return VIRTIO_RNG_FEATURE_PROFILE_OK;
}

static enum virtio_rng_feature_profile_status
virtio_rng_feature_profile_validate_stored(
    const struct virtio_rng_feature_profile *profile)
{
    enum virtio_rng_feature_profile_status status;
    enum virtio_pci_status pci_status;
    struct virtio_rng_feature_bitset required;

    if (profile->version != VIRTIO_RNG_FEATURE_PROFILE_VERSION) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION;
    }
    if (!virtio_rng_feature_profile_generation_valid(
            profile->generation)) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION;
    }

    pci_status = virtio_pci_identity_validate(&profile->identity);
    if (pci_status != VIRTIO_PCI_OK) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY;
    }
    if (!virtio_rng_identity_is_rng(&profile->identity)) {
        return VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE;
    }
    if (!virtio_rng_feature_bitset_known(&profile->offered) ||
        !virtio_rng_feature_bitset_known(&profile->accepted)) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE;
    }

    required = virtio_rng_required_features(profile->identity.modern);
    if (!virtio_rng_feature_bitset_equal(
            &profile->accepted, &required)) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE;
    }
    if (profile->request_queue_count <
            VIRTIO_RNG_REQUEST_QUEUE_COUNT_MIN ||
        profile->request_queue_count >
            VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT;
    }
    if (profile->buffer_writer != VIRTIO_RNG_BUFFER_WRITER_DEVICE) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER;
    }

    status = virtio_rng_feature_policy_validate(
        &profile->identity, &profile->offered);
    if (status != VIRTIO_RNG_FEATURE_PROFILE_OK) {
        return status;
    }
    return VIRTIO_RNG_FEATURE_PROFILE_OK;
}

enum virtio_rng_feature_profile_status
virtio_rng_feature_profile_activate(
    struct virtio_rng_feature_profile *profile,
    const struct virtio_pci_identity *identity,
    const struct virtio_rng_feature_bitset *offered,
    const struct virtio_rng_topology *topology,
    uint32_t generation)
{
    enum virtio_rng_feature_profile_status status;
    enum virtio_pci_status pci_status;
    struct virtio_rng_feature_bitset required;

    if (profile == NULL || identity == NULL || offered == NULL ||
        topology == NULL) {
        return VIRTIO_RNG_FEATURE_PROFILE_NULL;
    }
    if (profile->published) {
        return VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED;
    }
    if (profile->version != 0U) {
        if (profile->version == VIRTIO_RNG_FEATURE_PROFILE_VERSION) {
            return VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED;
        }
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION;
    }

    pci_status = virtio_pci_identity_validate(identity);
    if (pci_status != VIRTIO_PCI_OK) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY;
    }
    if (!virtio_rng_identity_is_rng(identity)) {
        return VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE;
    }

    status = virtio_rng_feature_policy_validate(identity, offered);
    if (status != VIRTIO_RNG_FEATURE_PROFILE_OK) {
        return status;
    }
    status = virtio_rng_topology_validate(topology);
    if (status != VIRTIO_RNG_FEATURE_PROFILE_OK) {
        return status;
    }
    if (!virtio_rng_feature_profile_generation_valid(generation)) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION;
    }

    required = virtio_rng_required_features(identity->modern);
    profile->published = true;
    profile->version = VIRTIO_RNG_FEATURE_PROFILE_VERSION;
    profile->generation = generation;
    profile->identity = *identity;
    profile->offered = *offered;
    profile->accepted = required;
    profile->request_queue_count = topology->request_queue_count;
    profile->buffer_writer = topology->buffer_writer;
    return VIRTIO_RNG_FEATURE_PROFILE_OK;
}

enum virtio_rng_feature_profile_status
virtio_rng_feature_profile_validate_state(
    const struct virtio_rng_feature_profile *profile,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_rng_feature_bitset *observed_offered,
    const struct virtio_rng_topology *observed_topology,
    uint32_t observed_generation)
{
    enum virtio_rng_feature_profile_status status;

    if (profile == NULL || observed_identity == NULL ||
        observed_offered == NULL || observed_topology == NULL) {
        return VIRTIO_RNG_FEATURE_PROFILE_NULL;
    }
    if (!profile->published) {
        return VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED;
    }

    status = virtio_rng_feature_profile_validate_stored(profile);
    if (status != VIRTIO_RNG_FEATURE_PROFILE_OK) {
        return status;
    }
    if (!virtio_rng_feature_profile_generation_valid(
            observed_generation)) {
        return VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION;
    }
    if (observed_generation != profile->generation) {
        return VIRTIO_RNG_FEATURE_PROFILE_CHANGED;
    }
    if (!virtio_rng_identity_equal(
            observed_identity, &profile->identity) ||
        !virtio_rng_feature_bitset_equal(
            observed_offered, &profile->offered) ||
        observed_topology->version != VIRTIO_RNG_TOPOLOGY_VERSION ||
        observed_topology->request_queue_count !=
            profile->request_queue_count ||
        observed_topology->buffer_writer != profile->buffer_writer) {
        return VIRTIO_RNG_FEATURE_PROFILE_CHANGED;
    }
    return VIRTIO_RNG_FEATURE_PROFILE_OK;
}

bool virtio_rng_feature_profile_published(
    const struct virtio_rng_feature_profile *profile)
{
    return profile != NULL &&
           profile->published &&
           profile->version == VIRTIO_RNG_FEATURE_PROFILE_VERSION;
}

const char *virtio_rng_feature_profile_status_string(
    enum virtio_rng_feature_profile_status status)
{
    switch (status) {
    case VIRTIO_RNG_FEATURE_PROFILE_OK:
        return "VIRTIO_RNG_FEATURE_PROFILE_OK";
    case VIRTIO_RNG_FEATURE_PROFILE_NULL:
        return "VIRTIO_RNG_FEATURE_PROFILE_NULL";
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION:
        return "VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION";
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION:
        return "VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION";
    case VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED:
        return "VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED";
    case VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED:
        return "VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED";
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY:
        return "VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY";
    case VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE:
        return "VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE";
    case VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE:
        return "VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE";
    case VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE:
        return "VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE";
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE:
        return "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE";
    case VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY:
        return "VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY";
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT:
        return "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT";
    case VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER:
        return "VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER";
    case VIRTIO_RNG_FEATURE_PROFILE_CHANGED:
        return "VIRTIO_RNG_FEATURE_PROFILE_CHANGED";
    default:
        return NULL;
    }
}
