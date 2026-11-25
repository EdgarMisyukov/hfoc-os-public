#ifndef HFOC_VIRTIO_PCI_IDENTITY_H
#define HFOC_VIRTIO_PCI_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_PCI_IDENTITY_VERSION 1U
#define VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION 1U
#define VIRTIO_PCI_SELECTION_VERSION 1U

#define VIRTIO_PCI_VENDOR_ID 0x1AF4U

#define VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL 0x1000U
#define VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL 0x1005U
#define VIRTIO_PCI_DEVICE_ID_NET_MODERN 0x1041U
#define VIRTIO_PCI_DEVICE_ID_RNG_MODERN 0x1044U

#define VIRTIO_PCI_GENERATION_MIN 1U
#define VIRTIO_PCI_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_PCI_CAPABILITY_COMMON (1U << 0U)
#define VIRTIO_PCI_CAPABILITY_NOTIFY (1U << 1U)
#define VIRTIO_PCI_CAPABILITY_ISR    (1U << 2U)
#define VIRTIO_PCI_CAPABILITY_DEVICE (1U << 3U)
#define VIRTIO_PCI_CAPABILITY_MASK \
    (VIRTIO_PCI_CAPABILITY_COMMON | \
     VIRTIO_PCI_CAPABILITY_NOTIFY | \
     VIRTIO_PCI_CAPABILITY_ISR | \
     VIRTIO_PCI_CAPABILITY_DEVICE)

enum virtio_pci_status {
    VIRTIO_PCI_OK = 0,
    VIRTIO_PCI_NULL = 1,
    VIRTIO_PCI_INVALID_IDENTITY_VERSION = 2,
    VIRTIO_PCI_INVALID_VENDOR_ID = 3,
    VIRTIO_PCI_UNSUPPORTED_DEVICE_ID = 4,
    VIRTIO_PCI_INVALID_BDF = 5,
    VIRTIO_PCI_INVALID_TRANSPORT = 6,
    VIRTIO_PCI_INVALID_CAPABILITY_VERSION = 7,
    VIRTIO_PCI_UNSUPPORTED_CAPABILITY = 8,
    VIRTIO_PCI_BAD_GENERATION = 9,
    VIRTIO_PCI_ALREADY_PUBLISHED = 10,
    VIRTIO_PCI_UNPUBLISHED = 11,
    VIRTIO_PCI_CHANGED = 12,
    VIRTIO_PCI_INVALID_SELECTION_VERSION = 13,
};

struct virtio_pci_bdf {
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

struct virtio_pci_identity {
    uint32_t version;
    uint16_t vendor_id;
    uint16_t device_id;
    struct virtio_pci_bdf bdf;
    uint8_t modern;
};

struct virtio_pci_capability_snapshot {
    uint32_t version;
    uint32_t capability_mask;
};

struct virtio_pci_selection {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct virtio_pci_identity identity;
    struct virtio_pci_capability_snapshot capability_snapshot;
};

enum virtio_pci_status virtio_pci_identity_validate(
    const struct virtio_pci_identity *identity);
enum virtio_pci_status virtio_pci_capability_snapshot_validate(
    const struct virtio_pci_capability_snapshot *snapshot);
enum virtio_pci_status virtio_pci_selection_init(
    struct virtio_pci_selection *selection,
    const struct virtio_pci_identity *identity,
    const struct virtio_pci_capability_snapshot *capability_snapshot,
    uint32_t generation);
enum virtio_pci_status virtio_pci_selection_validate_state(
    const struct virtio_pci_selection *selection,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_pci_capability_snapshot *observed_capability_snapshot,
    uint32_t observed_generation);
bool virtio_pci_selection_published(
    const struct virtio_pci_selection *selection);
const char *virtio_pci_status_string(
    enum virtio_pci_status status);

#endif
