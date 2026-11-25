#ifndef HFOC_VIRTIO_PCI_FEATURE_H
#define HFOC_VIRTIO_PCI_FEATURE_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_PCI_FEATURE_VERSION 1U
#define VIRTIO_PCI_FEATURE_GENERATION_MIN 1U
#define VIRTIO_PCI_FEATURE_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_PCI_FEATURE_SELECT_LO 0U
#define VIRTIO_PCI_FEATURE_SELECT_HI 1U
#define VIRTIO_PCI_FEATURE_BIT_COUNT 64U

#define VIRTIO_PCI_FEATURE_RING_INDIRECT_DESC 28U
#define VIRTIO_PCI_FEATURE_RING_EVENT_IDX 29U
#define VIRTIO_PCI_FEATURE_VERSION_1 32U
#define VIRTIO_PCI_FEATURE_ACCESS_PLATFORM 33U
#define VIRTIO_PCI_FEATURE_RING_PACKED 34U
#define VIRTIO_PCI_FEATURE_IN_ORDER 35U
#define VIRTIO_PCI_FEATURE_ORDER_PLATFORM 36U
#define VIRTIO_PCI_FEATURE_SR_IOV 37U
#define VIRTIO_PCI_FEATURE_NOTIFICATION_DATA 38U
#define VIRTIO_PCI_FEATURE_RING_RESET 40U

#define VIRTIO_PCI_FEATURE_KNOWN_LO_MASK UINT32_C(0x30000000)
#define VIRTIO_PCI_FEATURE_KNOWN_HI_MASK UINT32_C(0x0000017F)

struct virtio_pci_feature_bitset {
    uint32_t low;
    uint32_t high;
};

struct virtio_pci_feature_policy {
    struct virtio_pci_feature_bitset required;
    struct virtio_pci_feature_bitset accepted;
};

struct virtio_pci_feature_io {
    bool (*read_offered)(void *context, uint32_t select, uint32_t *value);
    bool (*write_accepted)(void *context, uint32_t select, uint32_t value);
    bool (*read_accepted)(void *context, uint32_t select, uint32_t *value);
    void *context;
};

struct virtio_pci_feature_controller {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_pci_feature_bitset offered;
    struct virtio_pci_feature_bitset accepted;
    struct virtio_pci_feature_io io;
};

enum virtio_pci_feature_status {
    VIRTIO_PCI_FEATURE_OK = 0,
    VIRTIO_PCI_FEATURE_NULL = 1,
    VIRTIO_PCI_FEATURE_INVALID_VERSION = 2,
    VIRTIO_PCI_FEATURE_INVALID_GENERATION = 3,
    VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED = 4,
    VIRTIO_PCI_FEATURE_UNPUBLISHED = 5,
    VIRTIO_PCI_FEATURE_INVALID_IO = 6,
    VIRTIO_PCI_FEATURE_IO_FAILED = 7,
    VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED = 8,
    VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED = 9,
    VIRTIO_PCI_FEATURE_MISSING_REQUIRED = 10,
    VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED = 11,
    VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED = 12,
    VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY = 13,
    VIRTIO_PCI_FEATURE_INVALID_READBACK = 14,
    VIRTIO_PCI_FEATURE_CHANGED = 15,
};

enum virtio_pci_feature_status virtio_pci_feature_init(
    struct virtio_pci_feature_controller *controller,
    const struct virtio_pci_feature_io *io,
    uint32_t generation);
enum virtio_pci_feature_status virtio_pci_feature_negotiate(
    struct virtio_pci_feature_controller *controller,
    const struct virtio_pci_feature_policy *policy);
enum virtio_pci_feature_status virtio_pci_feature_observe(
    const struct virtio_pci_feature_controller *controller,
    struct virtio_pci_feature_bitset *accepted_out);
bool virtio_pci_feature_published(
    const struct virtio_pci_feature_controller *controller);
const char *virtio_pci_feature_status_string(
    enum virtio_pci_feature_status status);

#endif
