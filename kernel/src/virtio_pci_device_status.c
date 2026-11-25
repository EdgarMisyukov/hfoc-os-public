#include "virtio_pci_device_status.h"

#include <stddef.h>
#include <stdint.h>

static bool virtio_pci_device_status_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_PCI_DEVICE_STATUS_GENERATION_MIN &&
           generation <= VIRTIO_PCI_DEVICE_STATUS_GENERATION_MAX;
}

static bool virtio_pci_device_status_state_valid(
    enum virtio_pci_device_state state)
{
    return (unsigned int)state <
           (unsigned int)VIRTIO_PCI_DEVICE_STATE_COUNT;
}

static enum virtio_pci_device_status_status
virtio_pci_device_status_controller_valid(
    const struct virtio_pci_device_status_controller *controller)
{
    if (controller == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    if (!controller->published) {
        return VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED;
    }
    if (controller->version != VIRTIO_PCI_DEVICE_STATUS_VERSION) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION;
    }
    if (!virtio_pci_device_status_state_valid(controller->state)) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE;
    }
    return VIRTIO_PCI_DEVICE_STATUS_OK;
}

static enum virtio_pci_device_status_status
virtio_pci_device_status_io_valid(
    const struct virtio_pci_device_status_io *io)
{
    if (io == NULL || io->write == NULL || io->read == NULL ||
        io->context == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_IO;
    }
    return VIRTIO_PCI_DEVICE_STATUS_OK;
}

static uint8_t virtio_pci_device_status_state_bits(
    enum virtio_pci_device_state state)
{
    switch (state) {
    case VIRTIO_PCI_DEVICE_STATE_RESET:
        return 0U;
    case VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE:
        return VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE;
    case VIRTIO_PCI_DEVICE_STATE_DRIVER:
        return VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
               VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER;
    case VIRTIO_PCI_DEVICE_STATE_FEATURES_OK:
        return VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
               VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
               VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK;
    case VIRTIO_PCI_DEVICE_STATE_DRIVER_OK:
        return VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
               VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
               VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
               VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK;
    case VIRTIO_PCI_DEVICE_STATE_FAILED:
        return VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED;
    default:
        return VIRTIO_PCI_DEVICE_STATUS_BYTE_MASK;
    }
}

static enum virtio_pci_device_status_status
virtio_pci_device_status_transition_readback(
    uint8_t expected,
    uint8_t observed)
{
    if (observed == expected) {
        return VIRTIO_PCI_DEVICE_STATUS_OK;
    }
    if ((observed & ~VIRTIO_PCI_DEVICE_STATUS_KNOWN_BITS) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK;
    }
    if ((observed & VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET;
    }
    if ((observed & VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_FAILED;
    }
    return VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK;
}

static enum virtio_pci_device_status_status
virtio_pci_device_status_observe_readback(
    const struct virtio_pci_device_status_controller *controller,
    uint8_t observed)
{
    if ((observed & ~VIRTIO_PCI_DEVICE_STATUS_KNOWN_BITS) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK;
    }
    if (observed == controller->status_bits) {
        return VIRTIO_PCI_DEVICE_STATUS_OK;
    }
    if ((observed & VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET;
    }
    if ((observed & VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED) != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_FAILED;
    }
    return VIRTIO_PCI_DEVICE_STATUS_CHANGED;
}

static enum virtio_pci_device_status_status
virtio_pci_device_status_transition(
    struct virtio_pci_device_status_controller *controller,
    enum virtio_pci_device_state required_state,
    enum virtio_pci_device_state next_state,
    uint8_t expected_bits)
{
    enum virtio_pci_device_status_status status;
    uint8_t observed;

    if (controller == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    status = virtio_pci_device_status_controller_valid(controller);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    status = virtio_pci_device_status_io_valid(&controller->io);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    if (required_state != VIRTIO_PCI_DEVICE_STATE_COUNT &&
        controller->state != required_state) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION;
    }

    if (!controller->io.write(controller->io.context, expected_bits)) {
        return VIRTIO_PCI_DEVICE_STATUS_IO_FAILED;
    }
    if (!controller->io.read(controller->io.context, &observed)) {
        return VIRTIO_PCI_DEVICE_STATUS_IO_FAILED;
    }
    status = virtio_pci_device_status_transition_readback(
        expected_bits, observed);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }

    controller->state = next_state;
    controller->status_bits = expected_bits;
    controller->observed_bits = observed;
    return VIRTIO_PCI_DEVICE_STATUS_OK;
}

enum virtio_pci_device_status_status virtio_pci_device_status_init(
    struct virtio_pci_device_status_controller *controller,
    const struct virtio_pci_device_status_io *io,
    uint32_t generation)
{
    enum virtio_pci_device_status_status status;

    if (controller == NULL || io == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    if (controller->published) {
        return VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED;
    }
    if (controller->version != 0U) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION;
    }
    status = virtio_pci_device_status_io_valid(io);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    if (!virtio_pci_device_status_generation_valid(generation)) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION;
    }

    controller->published = true;
    controller->version = VIRTIO_PCI_DEVICE_STATUS_VERSION;
    controller->generation = generation;
    controller->state = VIRTIO_PCI_DEVICE_STATE_RESET;
    controller->status_bits = 0U;
    controller->observed_bits = 0U;
    controller->io = *io;
    return VIRTIO_PCI_DEVICE_STATUS_OK;
}

enum virtio_pci_device_status_status virtio_pci_device_status_reset(
    struct virtio_pci_device_status_controller *controller)
{
    enum virtio_pci_device_status_status status;

    if (controller == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    status = virtio_pci_device_status_controller_valid(controller);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_COUNT,
        VIRTIO_PCI_DEVICE_STATE_RESET, 0U);
}

enum virtio_pci_device_status_status virtio_pci_device_status_acknowledge(
    struct virtio_pci_device_status_controller *controller)
{
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_RESET,
        VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE,
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE);
}

enum virtio_pci_device_status_status virtio_pci_device_status_driver(
    struct virtio_pci_device_status_controller *controller)
{
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_ACKNOWLEDGE,
        VIRTIO_PCI_DEVICE_STATE_DRIVER,
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER);
}

enum virtio_pci_device_status_status virtio_pci_device_status_features_ok(
    struct virtio_pci_device_status_controller *controller)
{
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_DRIVER,
        VIRTIO_PCI_DEVICE_STATE_FEATURES_OK,
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
        VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK);
}

enum virtio_pci_device_status_status virtio_pci_device_status_driver_ok(
    struct virtio_pci_device_status_controller *controller)
{
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_FEATURES_OK,
        VIRTIO_PCI_DEVICE_STATE_DRIVER_OK,
        VIRTIO_PCI_DEVICE_STATUS_BIT_ACKNOWLEDGE |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER |
        VIRTIO_PCI_DEVICE_STATUS_BIT_FEATURES_OK |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DRIVER_OK);
}

enum virtio_pci_device_status_status virtio_pci_device_status_failed(
    struct virtio_pci_device_status_controller *controller)
{
    enum virtio_pci_device_status_status status;
    uint8_t failed_bits;

    if (controller == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    status = virtio_pci_device_status_controller_valid(controller);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    if (controller->state == VIRTIO_PCI_DEVICE_STATE_FAILED) {
        return VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION;
    }
    failed_bits = (uint8_t)(
        virtio_pci_device_status_state_bits(controller->state) |
        VIRTIO_PCI_DEVICE_STATUS_BIT_FAILED);
    return virtio_pci_device_status_transition(
        controller, VIRTIO_PCI_DEVICE_STATE_COUNT,
        VIRTIO_PCI_DEVICE_STATE_FAILED, failed_bits);
}

enum virtio_pci_device_status_status virtio_pci_device_status_observe(
    const struct virtio_pci_device_status_controller *controller,
    uint8_t *observed_status_out)
{
    enum virtio_pci_device_status_status status;
    uint8_t observed;

    if (controller == NULL || observed_status_out == NULL) {
        return VIRTIO_PCI_DEVICE_STATUS_NULL;
    }
    status = virtio_pci_device_status_controller_valid(controller);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    status = virtio_pci_device_status_io_valid(&controller->io);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return status;
    }
    if (!controller->io.read(controller->io.context, &observed)) {
        return VIRTIO_PCI_DEVICE_STATUS_IO_FAILED;
    }
    *observed_status_out = observed;
    return virtio_pci_device_status_observe_readback(controller, observed);
}

bool virtio_pci_device_status_live(
    const struct virtio_pci_device_status_controller *controller)
{
    enum virtio_pci_device_status_status status;
    uint8_t observed;

    if (controller == NULL) {
        return false;
    }
    status = virtio_pci_device_status_controller_valid(controller);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK ||
        controller->state != VIRTIO_PCI_DEVICE_STATE_DRIVER_OK) {
        return false;
    }
    status = virtio_pci_device_status_io_valid(&controller->io);
    if (status != VIRTIO_PCI_DEVICE_STATUS_OK) {
        return false;
    }
    if (!controller->io.read(controller->io.context, &observed)) {
        return false;
    }
    return observed == controller->status_bits;
}

const char *virtio_pci_device_status_status_string(
    enum virtio_pci_device_status_status status)
{
    switch (status) {
    case VIRTIO_PCI_DEVICE_STATUS_OK:
        return "VIRTIO_PCI_DEVICE_STATUS_OK";
    case VIRTIO_PCI_DEVICE_STATUS_NULL:
        return "VIRTIO_PCI_DEVICE_STATUS_NULL";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_VERSION";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_GENERATION";
    case VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED:
        return "VIRTIO_PCI_DEVICE_STATUS_ALREADY_PUBLISHED";
    case VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED:
        return "VIRTIO_PCI_DEVICE_STATUS_UNPUBLISHED";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_STATE";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_TRANSITION";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_IO:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_IO";
    case VIRTIO_PCI_DEVICE_STATUS_IO_FAILED:
        return "VIRTIO_PCI_DEVICE_STATUS_IO_FAILED";
    case VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK:
        return "VIRTIO_PCI_DEVICE_STATUS_INVALID_READBACK";
    case VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET:
        return "VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET";
    case VIRTIO_PCI_DEVICE_STATUS_FAILED:
        return "VIRTIO_PCI_DEVICE_STATUS_FAILED";
    case VIRTIO_PCI_DEVICE_STATUS_CHANGED:
        return "VIRTIO_PCI_DEVICE_STATUS_CHANGED";
    default:
        return NULL;
    }
}
