#include "virtio_pci_identity.h"

#include <stddef.h>

static bool virtio_pci_device_supported(uint16_t device_id)
{
    switch (device_id) {
    case VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL:
    case VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL:
    case VIRTIO_PCI_DEVICE_ID_NET_MODERN:
    case VIRTIO_PCI_DEVICE_ID_RNG_MODERN:
        return true;
    default:
        return false;
    }
}

static bool virtio_pci_device_modern(uint16_t device_id)
{
    return device_id == VIRTIO_PCI_DEVICE_ID_NET_MODERN ||
           device_id == VIRTIO_PCI_DEVICE_ID_RNG_MODERN;
}

static bool virtio_pci_bdf_valid(const struct virtio_pci_bdf *bdf)
{
    return bdf->device <= 0x1FU && bdf->function <= 0x7U;
}

static bool virtio_pci_identity_equal(
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

static bool virtio_pci_capability_snapshot_equal(
    const struct virtio_pci_capability_snapshot *left,
    const struct virtio_pci_capability_snapshot *right)
{
    return left->version == right->version &&
           left->capability_mask == right->capability_mask;
}

static bool virtio_pci_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_PCI_GENERATION_MIN &&
           generation <= VIRTIO_PCI_GENERATION_MAX;
}

enum virtio_pci_status virtio_pci_identity_validate(
    const struct virtio_pci_identity *identity)
{
    uint8_t expected_modern;

    if (identity == NULL) {
        return VIRTIO_PCI_NULL;
    }
    if (identity->version != VIRTIO_PCI_IDENTITY_VERSION) {
        return VIRTIO_PCI_INVALID_IDENTITY_VERSION;
    }
    if (identity->vendor_id != VIRTIO_PCI_VENDOR_ID) {
        return VIRTIO_PCI_INVALID_VENDOR_ID;
    }
    if (!virtio_pci_device_supported(identity->device_id)) {
        return VIRTIO_PCI_UNSUPPORTED_DEVICE_ID;
    }
    if (!virtio_pci_bdf_valid(&identity->bdf)) {
        return VIRTIO_PCI_INVALID_BDF;
    }

    expected_modern = virtio_pci_device_modern(identity->device_id) ? 1U : 0U;
    if (identity->modern != expected_modern) {
        return VIRTIO_PCI_INVALID_TRANSPORT;
    }
    return VIRTIO_PCI_OK;
}

enum virtio_pci_status virtio_pci_capability_snapshot_validate(
    const struct virtio_pci_capability_snapshot *snapshot)
{
    if (snapshot == NULL) {
        return VIRTIO_PCI_NULL;
    }
    if (snapshot->version != VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION) {
        return VIRTIO_PCI_INVALID_CAPABILITY_VERSION;
    }
    if (snapshot->capability_mask == 0U ||
        (snapshot->capability_mask & ~VIRTIO_PCI_CAPABILITY_MASK) != 0U) {
        return VIRTIO_PCI_UNSUPPORTED_CAPABILITY;
    }
    return VIRTIO_PCI_OK;
}

enum virtio_pci_status virtio_pci_selection_init(
    struct virtio_pci_selection *selection,
    const struct virtio_pci_identity *identity,
    const struct virtio_pci_capability_snapshot *capability_snapshot,
    uint32_t generation)
{
    enum virtio_pci_status status;

    if (selection == NULL || identity == NULL ||
        capability_snapshot == NULL) {
        return VIRTIO_PCI_NULL;
    }
    if (selection->published) {
        return VIRTIO_PCI_ALREADY_PUBLISHED;
    }
    status = virtio_pci_identity_validate(identity);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    status = virtio_pci_capability_snapshot_validate(capability_snapshot);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    if (!virtio_pci_generation_valid(generation)) {
        return VIRTIO_PCI_BAD_GENERATION;
    }

    selection->published = true;
    selection->version = VIRTIO_PCI_SELECTION_VERSION;
    selection->generation = generation;
    selection->identity = *identity;
    selection->capability_snapshot = *capability_snapshot;
    return VIRTIO_PCI_OK;
}

enum virtio_pci_status virtio_pci_selection_validate_state(
    const struct virtio_pci_selection *selection,
    const struct virtio_pci_identity *observed_identity,
    const struct virtio_pci_capability_snapshot *observed_capability_snapshot,
    uint32_t observed_generation)
{
    enum virtio_pci_status status;

    if (selection == NULL || observed_identity == NULL ||
        observed_capability_snapshot == NULL) {
        return VIRTIO_PCI_NULL;
    }
    if (!selection->published) {
        return VIRTIO_PCI_UNPUBLISHED;
    }
    if (selection->version != VIRTIO_PCI_SELECTION_VERSION) {
        return VIRTIO_PCI_INVALID_SELECTION_VERSION;
    }
    status = virtio_pci_identity_validate(&selection->identity);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    status = virtio_pci_capability_snapshot_validate(
        &selection->capability_snapshot);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    status = virtio_pci_identity_validate(observed_identity);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    status = virtio_pci_capability_snapshot_validate(
        observed_capability_snapshot);
    if (status != VIRTIO_PCI_OK) {
        return status;
    }
    if (!virtio_pci_generation_valid(observed_generation)) {
        return VIRTIO_PCI_BAD_GENERATION;
    }
    if (!virtio_pci_identity_equal(
            &selection->identity, observed_identity) ||
        !virtio_pci_capability_snapshot_equal(
            &selection->capability_snapshot,
            observed_capability_snapshot) ||
        selection->generation != observed_generation) {
        return VIRTIO_PCI_CHANGED;
    }
    return VIRTIO_PCI_OK;
}

bool virtio_pci_selection_published(
    const struct virtio_pci_selection *selection)
{
    return selection != NULL && selection->published;
}

const char *virtio_pci_status_string(enum virtio_pci_status status)
{
    switch (status) {
    case VIRTIO_PCI_OK:
        return "VIRTIO_PCI_OK";
    case VIRTIO_PCI_NULL:
        return "VIRTIO_PCI_NULL";
    case VIRTIO_PCI_INVALID_IDENTITY_VERSION:
        return "VIRTIO_PCI_INVALID_IDENTITY_VERSION";
    case VIRTIO_PCI_INVALID_VENDOR_ID:
        return "VIRTIO_PCI_INVALID_VENDOR_ID";
    case VIRTIO_PCI_UNSUPPORTED_DEVICE_ID:
        return "VIRTIO_PCI_UNSUPPORTED_DEVICE_ID";
    case VIRTIO_PCI_INVALID_BDF:
        return "VIRTIO_PCI_INVALID_BDF";
    case VIRTIO_PCI_INVALID_TRANSPORT:
        return "VIRTIO_PCI_INVALID_TRANSPORT";
    case VIRTIO_PCI_INVALID_CAPABILITY_VERSION:
        return "VIRTIO_PCI_INVALID_CAPABILITY_VERSION";
    case VIRTIO_PCI_UNSUPPORTED_CAPABILITY:
        return "VIRTIO_PCI_UNSUPPORTED_CAPABILITY";
    case VIRTIO_PCI_BAD_GENERATION:
        return "VIRTIO_PCI_BAD_GENERATION";
    case VIRTIO_PCI_ALREADY_PUBLISHED:
        return "VIRTIO_PCI_ALREADY_PUBLISHED";
    case VIRTIO_PCI_UNPUBLISHED:
        return "VIRTIO_PCI_UNPUBLISHED";
    case VIRTIO_PCI_CHANGED:
        return "VIRTIO_PCI_CHANGED";
    case VIRTIO_PCI_INVALID_SELECTION_VERSION:
        return "VIRTIO_PCI_INVALID_SELECTION_VERSION";
    default:
        return NULL;
    }
}
