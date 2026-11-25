#ifndef HFOC_VIRTIO_RNG_FEATURE_PROFILE_H
#define HFOC_VIRTIO_RNG_FEATURE_PROFILE_H

#include <stdbool.h>
#include <stdint.h>

#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"

#define VIRTIO_RNG_FEATURE_PROFILE_VERSION 1U
#define VIRTIO_RNG_TOPOLOGY_VERSION 1U
#define VIRTIO_RNG_PROFILE_GENERATION_MIN 1U
#define VIRTIO_RNG_PROFILE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_RNG_REQUEST_QUEUE_COUNT_MIN 1U
#define VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX 1U

#define VIRTIO_RNG_BUFFER_WRITER_UNKNOWN 0U
#define VIRTIO_RNG_BUFFER_WRITER_DEVICE 1U
#define VIRTIO_RNG_BUFFER_WRITER_DRIVER 2U

#define VIRTIO_RNG_KNOWN_LO_RNG_MASK 0U
#define VIRTIO_RNG_KNOWN_HI_RNG_MASK 0U
#define VIRTIO_RNG_REQUIRED_FEATURE_LO_MASK 0U
#define VIRTIO_RNG_VERSION_1_HIGH_BIT \
    (UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U))
#define VIRTIO_RNG_REQUIRED_FEATURE_HI_MASK \
    VIRTIO_RNG_VERSION_1_HIGH_BIT

#define VIRTIO_RNG_FEATURE_KNOWN_LO_MASK \
    (VIRTIO_PCI_FEATURE_KNOWN_LO_MASK | VIRTIO_RNG_KNOWN_LO_RNG_MASK)
#define VIRTIO_RNG_FEATURE_KNOWN_HI_MASK \
    (VIRTIO_PCI_FEATURE_KNOWN_HI_MASK | VIRTIO_RNG_KNOWN_HI_RNG_MASK)

struct virtio_rng_feature_bitset {
    uint32_t low;
    uint32_t high;
};

struct virtio_rng_topology {
    uint32_t version;
    uint32_t request_queue_count;
    uint32_t buffer_writer;
};

struct virtio_rng_feature_profile {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_pci_identity identity;
    struct virtio_rng_feature_bitset offered;
    struct virtio_rng_feature_bitset accepted;
    uint32_t request_queue_count;
    uint32_t buffer_writer;
};

enum virtio_rng_feature_profile_status {
    VIRTIO_RNG_FEATURE_PROFILE_OK = 0,
    VIRTIO_RNG_FEATURE_PROFILE_NULL = 1,
    VIRTIO_RNG_FEATURE_PROFILE_INVALID_VERSION = 2,
    VIRTIO_RNG_FEATURE_PROFILE_INVALID_GENERATION = 3,
    VIRTIO_RNG_FEATURE_PROFILE_ALREADY_PUBLISHED = 4,
    VIRTIO_RNG_FEATURE_PROFILE_UNPUBLISHED = 5,
    VIRTIO_RNG_FEATURE_PROFILE_INVALID_IDENTITY = 6,
    VIRTIO_RNG_FEATURE_PROFILE_NOT_RNG_DEVICE = 7,
    VIRTIO_RNG_FEATURE_PROFILE_UNKNOWN_FEATURE = 8,
    VIRTIO_RNG_FEATURE_PROFILE_MISSING_REQUIRED_FEATURE = 9,
    VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_FEATURE = 10,
    VIRTIO_RNG_FEATURE_PROFILE_INVALID_TOPOLOGY = 11,
    VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_QUEUE_COUNT = 12,
    VIRTIO_RNG_FEATURE_PROFILE_UNSUPPORTED_BUFFER_WRITER = 13,
    VIRTIO_RNG_FEATURE_PROFILE_CHANGED = 14,
};

enum virtio_rng_feature_profile_status
virtio_rng_feature_profile_activate(
    struct virtio_rng_feature_profile *profile,
    const struct virtio_pci_identity *identity,
    const struct virtio_rng_feature_bitset *offered,
    const struct virtio_rng_topology *topology,
    uint32_t generation);
enum virtio_rng_feature_profile_status
virtio_rng_feature_profile_validate_state(
    const struct virtio_rng_feature_profile *profile,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_rng_feature_bitset *observed_offered,
    const struct virtio_rng_topology *observed_topology,
    uint32_t observed_generation);
bool virtio_rng_feature_profile_published(
    const struct virtio_rng_feature_profile *profile);
const char *virtio_rng_feature_profile_status_string(
    enum virtio_rng_feature_profile_status status);

#endif
