#ifndef HFOC_VIRTIO_PCI_DEVICE_STATUS_H
#define HFOC_VIRTIO_PCI_DEVICE_STATUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define VIRTIO_PCI_DEVICE_STATUS_VERSION 1U
#define VIRTIO_PCI_DEVICE_STATUS_GENERATION_MIN 1U
#define VIRTIO_PCI_DEVICE_STATUS_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE 0x01U
#define VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER 0x02U
#define VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK 0x04U
#define VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK 0x08U
#define VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET 0x40U
#define VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED 0x80U
#define VIRTIO_PCI_DEVICE_STATUS_KNOWN_BITS \
    (VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE | \
     VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER | \
     VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK | \
     VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK | \
     VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET | \
     VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED)
#define VIRTIO_PCI_DEVICE_STATUS_BYTE_MASK 0xFFU

enum virtio_pci_device_state {
    VIRTIO_PCI_DEVICE_STATE_RESET = 0,
    VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE = 1,
    VIRTIO_PCI_DEVICE_STATE_DRIVER = 2,
    VIRTIO_PCI_DEVICE_STATE_FEATURES_OK = 3,
    VIRTIO_PCI_DEVICE_STATE_DRIVER_OK = 4,
    VIRTIO_PCI_DEVICE_STATE_FAILED = 5,
    VIRTIO_PCI_DEVICE_STATE_COUNT = 6,
};

struct virtio_pci_device_status_io {
    bool (*write)(void *context, uint8_t value);
    bool (*read)(void *context, uint8_t *value);
    void *context;
};

struct virtio_pci_device_status_controller {
    bool published;
    uint32_t version;
    uint32_t generation;
    enum virtio_pci_device_state state;
    uint8_t status_bits;
    uint8_t observed_bits;
    struct virtio_pci_device_status_io io;
};

enum virtio_pci_device_status_status {
    VIRTIO_PCI_DEVICE_STATUS_OK = 0,
    VIRTIO_PCI_DEVICE_STATUS_NULL = 1,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION = 2,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION = 3,
    VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED = 4,
    VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED = 5,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE = 6,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION = 7,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_IO = 8,
    VIRTIO_PCI_DEVICE_STATUS_IO_FAILED = 9,
    VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK = 10,
    VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET = 11,
    VIRTIO_PCI_DEVICE_STATUS_FAILED = 12,
    VIRTIO_PCI_DEVICE_STATUS_CHANGED = 13,
};

enum virtio_pci_device_status_status virtio_pci_device_status_init(
    struct virtio_pci_device_status_controller *controller,
    const struct virtio_pci_device_status_io *io,
    uint32_t generation);
enum virtio_pci_device_status_status virtio_pci_device_status_reset(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_acknowledge(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_driver(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_features_ok(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_driver_ok(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_failed(
    struct virtio_pci_device_status_controller *controller);
enum virtio_pci_device_status_status virtio_pci_device_status_observe(
    const struct virtio_pci_device_status_controller *controller,
    uint8_t *observed_status_out);
bool virtio_pci_device_status_live(
    const struct virtio_pci_device_status_controller *controller);
const char *virtio_pci_device_status_status_string(
    enum virtio_pci_device_status_status status);

#endif
