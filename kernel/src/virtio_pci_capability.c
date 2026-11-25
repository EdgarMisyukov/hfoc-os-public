#include "virtio_pci_capability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint16_t virtio_pci_capability_read_le16(
    const uint8_t *buffer)
{
    return (uint16_t)buffer[0] |
           (uint16_t)((uint16_t)buffer[1] << 8U);
}

static uint32_t virtio_pci_capability_read_le32(
    const uint8_t *buffer)
{
    return (uint32_t)buffer[0] |
           ((uint32_t)buffer[1] << 8U) |
           ((uint32_t)buffer[2] << 16U) |
           ((uint32_t)buffer[3] << 24U);
}

static bool virtio_pci_capability_type_valid(uint8_t type)
{
    switch (type) {
    case VIRTIO_PCI_CAPABILITY_TYPE_COMMON:
    case VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY:
    case VIRTIO_PCI_CAPABILITY_TYPE_ISR:
    case VIRTIO_PCI_CAPABILITY_TYPE_DEVICE:
    case VIRTIO_PCI_CAPABILITY_TYPE_PCI_CONFIG_ACCESS:
    case VIRTIO_PCI_CAPABILITY_TYPE_SHARED_MEMORY:
        return true;
    default:
        return false;
    }
}

static size_t virtio_pci_capability_type_slot(uint8_t type)
{
    return (size_t)type - 1U;
}

static bool virtio_pci_capability_bar_range_valid(
    const struct virtio_pci_bar_range *range)
{
    return range->present &&
           range->length != 0U &&
           range->base <= UINT64_MAX - range->length;
}

static bool virtio_pci_capability_range_valid(
    const struct virtio_pci_bar_range *bar,
    uint32_t offset,
    uint32_t length,
    uint64_t *absolute_start_out,
    uint64_t *absolute_end_out)
{
    uint64_t absolute_start;
    uint64_t absolute_end;
    uint64_t bar_end;

    if (length == 0U ||
        (uint64_t)offset + (uint64_t)length > UINT32_MAX ||
        offset % VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U ||
        length % VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U) {
        return false;
    }
    absolute_start = bar->base + (uint64_t)offset;
    if (absolute_start < bar->base ||
        absolute_start > UINT64_MAX - (uint64_t)length) {
        return false;
    }
    absolute_end = absolute_start + (uint64_t)length;
    bar_end = bar->base + bar->length;
    if (absolute_end > bar_end) {
        return false;
    }

    *absolute_start_out = absolute_start;
    *absolute_end_out = absolute_end;
    return true;
}

static bool virtio_pci_capability_ranges_overlap(
    uint64_t left_start,
    uint64_t left_end,
    uint64_t right_start,
    uint64_t right_end)
{
    return left_start < right_end && right_start < left_end;
}

enum virtio_pci_capability_status virtio_pci_capability_parse(
    const uint8_t *config_space,
    size_t config_space_length,
    const struct virtio_pci_bar_range
        bar_ranges[VIRTIO_PCI_BAR_COUNT],
    struct virtio_pci_capability_parse_result *result)
{
    struct virtio_pci_capability_parse_result draft;
    struct virtio_pci_capability_descriptor descriptors[
        VIRTIO_PCI_CAPABILITY_SLOT_COUNT] = {{0}};
    bool seen[VIRTIO_PCI_CAPABILITY_SLOT_COUNT] = {false};
    bool visited[VIRTIO_PCI_CONFIG_SPACE_MAX] = {false};
    uint64_t absolute_starts[VIRTIO_PCI_CAPABILITY_SLOT_COUNT] = {0};
    uint64_t absolute_ends[VIRTIO_PCI_CAPABILITY_SLOT_COUNT] = {0};
    uint16_t capability_pointer;
    size_t capability_offset;
    unsigned int bar_index;

    if (config_space == NULL || bar_ranges == NULL || result == NULL) {
        return VIRTIO_PCI_CAPABILITY_NULL;
    }
    if (config_space_length < VIRTIO_PCI_CONFIG_HEADER_LENGTH ||
        config_space_length > VIRTIO_PCI_CONFIG_SPACE_MAX) {
        return VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH;
    }
    for (bar_index = 0U; bar_index < VIRTIO_PCI_BAR_COUNT; ++bar_index) {
        if (bar_ranges[bar_index].present &&
            !virtio_pci_capability_bar_range_valid(
                &bar_ranges[bar_index])) {
            return VIRTIO_PCI_CAPABILITY_INVALID_RANGE;
        }
    }

    capability_pointer = virtio_pci_capability_read_le16(
        config_space + VIRTIO_PCI_CAPABILITY_POINTER_OFFSET);
    if (capability_pointer == 0U) {
        return VIRTIO_PCI_CAPABILITY_MISSING;
    }
    if (capability_pointer < VIRTIO_PCI_FIRST_CAPABILITY_OFFSET ||
        (size_t)capability_pointer >= config_space_length) {
        return VIRTIO_PCI_CAPABILITY_INVALID_POINTER;
    }

    capability_offset = capability_pointer;
    while (capability_offset != 0U) {
        uint8_t capability_id;
        uint8_t next;
        size_t capability_length;

        if (capability_offset >= config_space_length ||
            capability_offset + 2U > config_space_length) {
            return VIRTIO_PCI_CAPABILITY_TRUNCATED;
        }
        if (visited[capability_offset]) {
            return VIRTIO_PCI_CAPABILITY_INVALID_POINTER;
        }
        visited[capability_offset] = true;
        capability_id = config_space[capability_offset];
        next = config_space[capability_offset + 1U];
        if (next != 0U) {
            if ((size_t)next >= config_space_length ||
                (size_t)next + 2U > config_space_length) {
                return VIRTIO_PCI_CAPABILITY_TRUNCATED;
            }
        }

        if (capability_id != VIRTIO_PCI_CAPABILITY_ID_VENDOR) {
            capability_offset = next;
            continue;
        }

        capability_length = config_space[capability_offset + 2U];
        if (capability_length < VIRTIO_PCI_CAPABILITY_MIN_LENGTH) {
            return VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY;
        }
        if (capability_offset + capability_length >
            config_space_length) {
            return VIRTIO_PCI_CAPABILITY_TRUNCATED;
        }
        {
            const uint8_t type = config_space[capability_offset + 3U];
            const uint8_t bar =
                config_space[capability_offset + 4U];
            const uint32_t range_offset =
                virtio_pci_capability_read_le32(
                    config_space + capability_offset + 8U);
            const uint32_t range_length =
                virtio_pci_capability_read_le32(
                    config_space + capability_offset + 12U);
            uint64_t absolute_start;
            uint64_t absolute_end;
            unsigned int slot;

            if (!virtio_pci_capability_type_valid(type)) {
                return VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE;
            }
            if (type == VIRTIO_PCI_CAPABILITY_TYPE_PCI_CONFIG_ACCESS ||
                type == VIRTIO_PCI_CAPABILITY_TYPE_SHARED_MEMORY) {
                capability_offset = next;
                continue;
            }
            if (type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY &&
                capability_length <
                    VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH) {
                return VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY;
            }
            if (bar >= VIRTIO_PCI_BAR_COUNT ||
                !bar_ranges[bar].present) {
                return VIRTIO_PCI_CAPABILITY_INVALID_BAR;
            }
            if (range_offset %
                    VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U ||
                range_length %
                    VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U) {
                return VIRTIO_PCI_CAPABILITY_UNALIGNED;
            }
            if (!virtio_pci_capability_range_valid(
                    &bar_ranges[bar], range_offset, range_length,
                    &absolute_start, &absolute_end)) {
                return VIRTIO_PCI_CAPABILITY_INVALID_RANGE;
            }

            slot = (unsigned int)virtio_pci_capability_type_slot(type);
            if (seen[slot]) {
                return VIRTIO_PCI_CAPABILITY_DUPLICATE;
            }
            for (unsigned int index = 0U;
                 index < VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
                 ++index) {
                if (!seen[index]) {
                    continue;
                }
                if (virtio_pci_capability_ranges_overlap(
                        absolute_start, absolute_end,
                        absolute_starts[index], absolute_ends[index])) {
                    return VIRTIO_PCI_CAPABILITY_OVERLAP;
                }
            }

            descriptors[slot].type = type;
            descriptors[slot].bar = bar;
            descriptors[slot].offset = range_offset;
            descriptors[slot].length = range_length;
            if (type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY) {
                descriptors[slot].notify_offset_multiplier =
                    virtio_pci_capability_read_le16(
                        config_space + capability_offset + 16U);
            }
            absolute_starts[slot] = absolute_start;
            absolute_ends[slot] = absolute_end;
            seen[slot] = true;
        }
        capability_offset = next;
    }

    for (unsigned int index = 0U;
         index < VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
         ++index) {
        if (!seen[index]) {
            return VIRTIO_PCI_CAPABILITY_MISSING;
        }
    }

    memset(&draft, 0, sizeof(draft));
    draft.published = true;
    draft.version = VIRTIO_PCI_CAPABILITY_RESULT_VERSION;
    draft.count = VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
    for (unsigned int index = 0U;
         index < VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
         ++index) {
        draft.descriptors[index].type = descriptors[index].type;
        draft.descriptors[index].bar = descriptors[index].bar;
        draft.descriptors[index].offset = descriptors[index].offset;
        draft.descriptors[index].length = descriptors[index].length;
        draft.descriptors[index].notify_offset_multiplier =
            descriptors[index].notify_offset_multiplier;
    }
    draft.snapshot.version = VIRTIO_PCI_CAPABILITY_SNAPSHOT_VERSION;
    draft.snapshot.capability_mask = VIRTIO_PCI_CAPABILITY_MASK;
    *result = draft;
    return VIRTIO_PCI_CAPABILITY_OK;
}

enum virtio_pci_capability_status virtio_pci_capability_result_validate(
    const struct virtio_pci_capability_parse_result *result)
{
    enum virtio_pci_status identity_status;

    if (result == NULL) {
        return VIRTIO_PCI_CAPABILITY_NULL;
    }
    if (!result->published) {
        return VIRTIO_PCI_CAPABILITY_UNPUBLISHED;
    }
    if (result->version != VIRTIO_PCI_CAPABILITY_RESULT_VERSION) {
        return VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION;
    }
    if (result->count != VIRTIO_PCI_CAPABILITY_SLOT_COUNT) {
        return VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR;
    }
    for (unsigned int index = 0U;
         index < VIRTIO_PCI_CAPABILITY_SLOT_COUNT;
         ++index) {
        const struct virtio_pci_capability_descriptor *descriptor =
            &result->descriptors[index];
        const uint8_t expected_type =
            index == 0U ? VIRTIO_PCI_CAPABILITY_TYPE_COMMON :
            index == 1U ? VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY :
            index == 2U ? VIRTIO_PCI_CAPABILITY_TYPE_ISR :
                          VIRTIO_PCI_CAPABILITY_TYPE_DEVICE;

        if (descriptor->type != expected_type ||
            descriptor->bar >= VIRTIO_PCI_BAR_COUNT ||
            descriptor->offset %
                VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U ||
            descriptor->length == 0U ||
            descriptor->length %
                VIRTIO_PCI_CAPABILITY_ALIGNMENT != 0U) {
            return VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR;
        }
    }
    identity_status =
        virtio_pci_capability_snapshot_validate(&result->snapshot);
    if (identity_status != VIRTIO_PCI_OK ||
        result->snapshot.capability_mask != VIRTIO_PCI_CAPABILITY_MASK) {
        return VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT;
    }
    return VIRTIO_PCI_CAPABILITY_OK;
}

bool virtio_pci_capability_result_published(
    const struct virtio_pci_capability_parse_result *result)
{
    return result != NULL && result->published;
}

const char *virtio_pci_capability_status_string(
    enum virtio_pci_capability_status status)
{
    switch (status) {
    case VIRTIO_PCI_CAPABILITY_OK:
        return "VIRTIO_PCI_CAPABILITY_OK";
    case VIRTIO_PCI_CAPABILITY_NULL:
        return "VIRTIO_PCI_CAPABILITY_NULL";
    case VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH:
        return "VIRTIO_PCI_CAPABILITY_INVALID_CONFIG_LENGTH";
    case VIRTIO_PCI_CAPABILITY_INVALID_POINTER:
        return "VIRTIO_PCI_CAPABILITY_INVALID_POINTER";
    case VIRTIO_PCI_CAPABILITY_TRUNCATED:
        return "VIRTIO_PCI_CAPABILITY_TRUNCATED";
    case VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY:
        return "VIRTIO_PCI_CAPABILITY_INVALID_VENDOR_CAPABILITY";
    case VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE:
        return "VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE";
    case VIRTIO_PCI_CAPABILITY_INVALID_BAR:
        return "VIRTIO_PCI_CAPABILITY_INVALID_BAR";
    case VIRTIO_PCI_CAPABILITY_INVALID_RANGE:
        return "VIRTIO_PCI_CAPABILITY_INVALID_RANGE";
    case VIRTIO_PCI_CAPABILITY_UNALIGNED:
        return "VIRTIO_PCI_CAPABILITY_UNALIGNED";
    case VIRTIO_PCI_CAPABILITY_DUPLICATE:
        return "VIRTIO_PCI_CAPABILITY_DUPLICATE";
    case VIRTIO_PCI_CAPABILITY_OVERLAP:
        return "VIRTIO_PCI_CAPABILITY_OVERLAP";
    case VIRTIO_PCI_CAPABILITY_MISSING:
        return "VIRTIO_PCI_CAPABILITY_MISSING";
    case VIRTIO_PCI_CAPABILITY_UNPUBLISHED:
        return "VIRTIO_PCI_CAPABILITY_UNPUBLISHED";
    case VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION:
        return "VIRTIO_PCI_CAPABILITY_INVALID_RESULT_VERSION";
    case VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR:
        return "VIRTIO_PCI_CAPABILITY_INVALID_DESCRIPTOR";
    case VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT:
        return "VIRTIO_PCI_CAPABILITY_INVALID_SNAPSHOT";
    default:
        return NULL;
    }
}
