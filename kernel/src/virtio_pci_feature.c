#include "virtio_pci_feature.h"

#include <stddef.h>
#include <stdint.h>

static bool virtio_pci_feature_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_PCI_FEATURE_GENERATION_MIN &&
           generation <= VIRTIO_PCI_FEATURE_GENERATION_MAX;
}

static bool virtio_pci_feature_io_valid(
    const struct virtio_pci_feature_io *io)
{
    return io != NULL &&
           io->read_offered != NULL &&
           io->write_accepted != NULL &&
           io->read_accepted != NULL &&
           io->context != NULL;
}

static bool virtio_pci_feature_controller_valid(
    const struct virtio_pci_feature_controller *controller)
{
    return controller != NULL &&
           controller->version == VIRTIO_PCI_FEATURE_VERSION &&
           virtio_pci_feature_generation_valid(controller->generation);
}

static bool virtio_pci_feature_bitset_known(
    const struct virtio_pci_feature_bitset *bitset)
{
    if ((bitset->low & ~VIRTIO_PCI_FEATURE_KNOWN_LO_MASK) != 0U) {
        return false;
    }
    if ((bitset->high & ~VIRTIO_PCI_FEATURE_KNOWN_HI_MASK) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_pci_feature_bitset_subset(
    const struct virtio_pci_feature_bitset *subset,
    const struct virtio_pci_feature_bitset *superset)
{
    if ((subset->low & ~superset->low) != 0U) {
        return false;
    }
    if ((subset->high & ~superset->high) != 0U) {
        return false;
    }
    return true;
}

static bool virtio_pci_feature_dependency_missing(
    const struct virtio_pci_feature_bitset *accepted)
{
    const uint32_t version_1_bit =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    const uint32_t dependent_high_mask =
        VIRTIO_PCI_FEATURE_KNOWN_HI_MASK & ~version_1_bit;

    return (accepted->high & dependent_high_mask) != 0U &&
           (accepted->high & version_1_bit) == 0U;
}

static enum virtio_pci_feature_status
virtio_pci_feature_validate_policy(
    const struct virtio_pci_feature_bitset *offered,
    const struct virtio_pci_feature_policy *policy)
{
    if (!virtio_pci_feature_bitset_known(&policy->required)) {
        return VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED;
    }
    if (!virtio_pci_feature_bitset_known(&policy->accepted)) {
        return VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED;
    }
    if (!virtio_pci_feature_bitset_subset(
            &policy->required, offered)) {
        return VIRTIO_PCI_FEATURE_MISSING_REQUIRED;
    }
    if (!virtio_pci_feature_bitset_subset(
            &policy->required, &policy->accepted)) {
        return VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED;
    }
    if (!virtio_pci_feature_bitset_subset(
            &policy->accepted, offered)) {
        return VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED;
    }
    if (virtio_pci_feature_dependency_missing(&policy->accepted)) {
        return VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY;
    }
    return VIRTIO_PCI_FEATURE_OK;
}

static void virtio_pci_feature_rollback(
    const struct virtio_pci_feature_io *io)
{
    (void)io->write_accepted(
        io->context, VIRTIO_PCI_FEATURE_SELECT_HI, 0U);
    (void)io->write_accepted(
        io->context, VIRTIO_PCI_FEATURE_SELECT_LO, 0U);
}

enum virtio_pci_feature_status virtio_pci_feature_init(
    struct virtio_pci_feature_controller *controller,
    const struct virtio_pci_feature_io *io,
    uint32_t generation)
{
    if (controller == NULL || io == NULL) {
        return VIRTIO_PCI_FEATURE_NULL;
    }
    if (controller->published) {
        return VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED;
    }
    if (controller->version != 0U) {
        if (controller->version == VIRTIO_PCI_FEATURE_VERSION) {
            return VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED;
        }
        return VIRTIO_PCI_FEATURE_INVALID_VERSION;
    }
    if (!virtio_pci_feature_io_valid(io)) {
        return VIRTIO_PCI_FEATURE_INVALID_IO;
    }
    if (!virtio_pci_feature_generation_valid(generation)) {
        return VIRTIO_PCI_FEATURE_INVALID_GENERATION;
    }

    controller->version = VIRTIO_PCI_FEATURE_VERSION;
    controller->generation = generation;
    controller->offered.low = 0U;
    controller->offered.high = 0U;
    controller->accepted.low = 0U;
    controller->accepted.high = 0U;
    controller->io = *io;
    return VIRTIO_PCI_FEATURE_OK;
}

enum virtio_pci_feature_status virtio_pci_feature_negotiate(
    struct virtio_pci_feature_controller *controller,
    const struct virtio_pci_feature_policy *policy)
{
    enum virtio_pci_feature_status status;
    struct virtio_pci_feature_bitset offered;
    uint32_t read_low;
    uint32_t read_high;

    if (controller == NULL || policy == NULL) {
        return VIRTIO_PCI_FEATURE_NULL;
    }
    if (controller->published) {
        return VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED;
    }
    if (controller->version != VIRTIO_PCI_FEATURE_VERSION) {
        return VIRTIO_PCI_FEATURE_INVALID_VERSION;
    }
    if (!virtio_pci_feature_generation_valid(controller->generation)) {
        return VIRTIO_PCI_FEATURE_INVALID_GENERATION;
    }
    if (!virtio_pci_feature_io_valid(&controller->io)) {
        return VIRTIO_PCI_FEATURE_INVALID_IO;
    }

    if (!controller->io.read_offered(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_LO,
            &offered.low)) {
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }
    if (!controller->io.read_offered(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_HI,
            &offered.high)) {
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }

    status = virtio_pci_feature_validate_policy(&offered, policy);
    if (status != VIRTIO_PCI_FEATURE_OK) {
        return status;
    }

    if (!controller->io.write_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_LO,
            policy->accepted.low)) {
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }
    if (!controller->io.write_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_HI,
            policy->accepted.high)) {
        (void)controller->io.write_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_LO,
            0U);
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }

    if (!controller->io.read_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_LO,
            &read_low)) {
        virtio_pci_feature_rollback(&controller->io);
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }
    if (read_low != policy->accepted.low) {
        virtio_pci_feature_rollback(&controller->io);
        return VIRTIO_PCI_FEATURE_INVALID_READBACK;
    }
    if (!controller->io.read_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_HI,
            &read_high)) {
        virtio_pci_feature_rollback(&controller->io);
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }
    if (read_high != policy->accepted.high) {
        virtio_pci_feature_rollback(&controller->io);
        return VIRTIO_PCI_FEATURE_INVALID_READBACK;
    }

    controller->offered = offered;
    controller->accepted = policy->accepted;
    controller->published = true;
    return VIRTIO_PCI_FEATURE_OK;
}

enum virtio_pci_feature_status virtio_pci_feature_observe(
    const struct virtio_pci_feature_controller *controller,
    struct virtio_pci_feature_bitset *accepted_out)
{
    uint32_t read_low;
    uint32_t read_high;

    if (controller == NULL || accepted_out == NULL) {
        return VIRTIO_PCI_FEATURE_NULL;
    }
    if (controller->version != VIRTIO_PCI_FEATURE_VERSION) {
        return VIRTIO_PCI_FEATURE_INVALID_VERSION;
    }
    if (!virtio_pci_feature_generation_valid(controller->generation)) {
        return VIRTIO_PCI_FEATURE_INVALID_GENERATION;
    }
    if (!controller->published) {
        return VIRTIO_PCI_FEATURE_UNPUBLISHED;
    }
    if (!virtio_pci_feature_io_valid(&controller->io)) {
        return VIRTIO_PCI_FEATURE_INVALID_IO;
    }
    if (!controller->io.read_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_LO,
            &read_low)) {
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }
    if (!controller->io.read_accepted(
            controller->io.context,
            VIRTIO_PCI_FEATURE_SELECT_HI,
            &read_high)) {
        return VIRTIO_PCI_FEATURE_IO_FAILED;
    }

    accepted_out->low = read_low;
    accepted_out->high = read_high;
    if (read_low != controller->accepted.low ||
        read_high != controller->accepted.high) {
        return VIRTIO_PCI_FEATURE_CHANGED;
    }
    return VIRTIO_PCI_FEATURE_OK;
}

bool virtio_pci_feature_published(
    const struct virtio_pci_feature_controller *controller)
{
    return virtio_pci_feature_controller_valid(controller) &&
           controller->published;
}

const char *virtio_pci_feature_status_string(
    enum virtio_pci_feature_status status)
{
    switch (status) {
    case VIRTIO_PCI_FEATURE_OK:
        return "VIRTIO_PCI_FEATURE_OK";
    case VIRTIO_PCI_FEATURE_NULL:
        return "VIRTIO_PCI_FEATURE_NULL";
    case VIRTIO_PCI_FEATURE_INVALID_VERSION:
        return "VIRTIO_PCI_FEATURE_INVALID_VERSION";
    case VIRTIO_PCI_FEATURE_INVALID_GENERATION:
        return "VIRTIO_PCI_FEATURE_INVALID_GENERATION";
    case VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED:
        return "VIRTIO_PCI_FEATURE_ALREADY_PUBLISHED";
    case VIRTIO_PCI_FEATURE_UNPUBLISHED:
        return "VIRTIO_PCI_FEATURE_UNPUBLISHED";
    case VIRTIO_PCI_FEATURE_INVALID_IO:
        return "VIRTIO_PCI_FEATURE_INVALID_IO";
    case VIRTIO_PCI_FEATURE_IO_FAILED:
        return "VIRTIO_PCI_FEATURE_IO_FAILED";
    case VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED:
        return "VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED";
    case VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED:
        return "VIRTIO_PCI_FEATURE_UNKNOWN_ACCEPTED";
    case VIRTIO_PCI_FEATURE_MISSING_REQUIRED:
        return "VIRTIO_PCI_FEATURE_MISSING_REQUIRED";
    case VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED:
        return "VIRTIO_PCI_FEATURE_REQUIRED_NOT_ACCEPTED";
    case VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED:
        return "VIRTIO_PCI_FEATURE_UNOFFERED_ACCEPTED";
    case VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY:
        return "VIRTIO_PCI_FEATURE_MISSING_DEPENDENCY";
    case VIRTIO_PCI_FEATURE_INVALID_READBACK:
        return "VIRTIO_PCI_FEATURE_INVALID_READBACK";
    case VIRTIO_PCI_FEATURE_CHANGED:
        return "VIRTIO_PCI_FEATURE_CHANGED";
    default:
        return NULL;
    }
}
