#ifndef HFOC_VIRTIO_PCI_CAPABILITY_H
#define HFOC_VIRTIO_PCI_CAPABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "virtio_pci_identity.h"

#define VIRTIO_PCI_CAPABILITY_VERSION 1U
#define VIRTIO_PCI_CAPABILITY_RESULT_VERSION 1U

#define VIRTIO_PCI_CONFIG_SPACE_MAX 256U
#define VIRTIO_PCI_CONFIG_HEADER_LENGTH 0x38U
#define VIRTIO_PCI_CAPABILITY_POINTER_OFFSET 0x34U
#define VIRTIO_PCI_FIRST_CAPABILITY_OFFSET 0x40U

#define VIRTIO_PCI_CAPABILITY_ID_VENDOR 0x09U
#define VIRTIO_PCI_CAPABILITY_MIN_LENGTH 16U
#define VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH 18U
#define VIRTIO_PCI_NOTIFY_OFFSET_MULTIPLIER_OFFSET 16U

#define VIRTIO_PCI_CAPABILITY_TYPE_COMMON 1U
#define VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY 2U
#define VIRTIO_PCI_CAPABILITY_TYPE_ISR 3U
#define VIRTIO_PCI_CAPABILITY_TYPE_DEVICE 4U
#define VIRTIO_PCI_CAPABILITY_TYPE_PCI_CONFIG_ACCESS 5U
#define VIRTIO_PCI_CAPABILITY_TYPE_SHARED_MEMORY 6U

#define VIRTIO_PCI_CAPABILITY_ALIGNMENT 4U
#define VIRTIO_PCI_BAR_COUNT 6U
#define VIRTIO_PCI_CAPABILITY_SLOT_COUNT 4U

enum virtio_pci_capability_status {
    VIRTIO_PCI_CAPABILITY_OK = 0,
    VIRTIO_PCI_CAPABILITY_NULL = 1,
    VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH = 2,
    VIRTIO_PCI_CAPABILITY_INVALID_POINTER = 3,
    VIRTIO_PCI_CAPABILITY_TRUNCATED = 4,
    VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY = 5,
    VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE = 6,
    VIRTIO_PCI_CAPABILITY_INVALID_BAR = 7,
    VIRTIO_PCI_CAPABILITY_INVALID_RANGE = 8,
    VIRTIO_PCI_CAPABILITY_UNALIGNED = 9,
    VIRTIO_PCI_CAPABILITY_DUPLICATE = 10,
    VIRTIO_PCI_CAPABILITY_OVERLAP = 11,
    VIRTIO_PCI_CAPABILITY_MISSING = 12,
    VIRTIO_PCI_CAPABILITY_UNPUBLISHED = 13,
    VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION = 14,
    VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR = 15,
    VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT = 16,
};

struct virtio_pci_bar_range {
    bool present;
    uint64_t base;
    uint64_t length;
};

struct virtio_pci_capability_descriptor {
    uint8_t type;
    uint8_t bar;
    uint32_t offset;
    uint32_t length;
    uint16_t notify_offset_multiplier;
};

struct virtio_pci_capability_parse_result {
    bool published;
    uint32_t version;
    uint8_t count;
    struct virtio_pci_capability_descriptor
        descriptors[VIRTIO_PCI_CAPABILITY_SLOT_COUNT];
    struct virtio_pci_capability_snapshot snapshot;
};

enum virtio_pci_capability_status virtio_pci_capability_parse(
    const uint8_t *config_space,
    size_t config_space_length,
    const struct virtio_pci_bar_range
        bar_ranges[VIRTIO_PCI_BAR_COUNT],
    struct virtio_pci_capability_parse_result *result);
enum virtio_pci_capability_status virtio_pci_capability_result_validate(
    const struct virtio_pci_capability_parse_result *result);
bool virtio_pci_capability_result_published(
    const struct virtio_pci_capability_parse_result *result);
const char *virtio_pci_capability_status_string(
    enum virtio_pci_capability_status status);

#endif
