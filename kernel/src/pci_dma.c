#include "pci_dma.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

bool pci_dma_direction_valid(enum pci_dma_direction direction)
{
    return direction == PCI_DMA_DIRECTION_DEVICE_READ ||
           direction == PCI_DMA_DIRECTION_DEVICE_WRITE ||
           direction == PCI_DMA_DIRECTION_BIDIRECTIONAL;
}

enum pci_dma_status pci_dma_buffer_publish(
    struct pci_dma_buffer *buffer,
    uint64_t physical_base,
    size_t length,
    enum pci_dma_direction direction)
{
    struct pci_dma_buffer draft;

    if (buffer == NULL) {
        return PCI_DMA_NULL;
    }
    if (physical_base == 0U ||
        physical_base % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return physical_base == 0U ? PCI_DMA_INVALID_ADDRESS
                                   : PCI_DMA_UNALIGNED;
    }
    if (length == 0U || length > PCI_DMA_LENGTH_MAX) {
        return PCI_DMA_INVALID_LENGTH;
    }
    if (physical_base > UINT64_MAX - (uint64_t)length) {
        return PCI_DMA_OVERFLOW;
    }
    if (!pci_dma_direction_valid(direction)) {
        return PCI_DMA_INVALID_DIRECTION;
    }

    memset(&draft, 0, sizeof(draft));
    draft.published = true;
    draft.physical_base = physical_base;
    draft.physical_end = physical_base + (uint64_t)length;
    draft.length = length;
    draft.direction = direction;
    *buffer = draft;
    return PCI_DMA_OK;
}

enum pci_dma_status pci_dma_buffer_slice(
    const struct pci_dma_buffer *buffer,
    uint64_t offset,
    size_t length,
    struct pci_dma_buffer *slice)
{
    uint64_t end;

    if (buffer == NULL || slice == NULL) {
        return PCI_DMA_NULL;
    }
    if (!buffer->published ||
        !pci_dma_direction_valid(buffer->direction)) {
        return PCI_DMA_INVALID_DIRECTION;
    }
    if (buffer->physical_base == 0U ||
        buffer->physical_end !=
            buffer->physical_base + (uint64_t)buffer->length) {
        return PCI_DMA_OVERFLOW;
    }
    if (length == 0U || length > PCI_DMA_LENGTH_MAX) {
        return PCI_DMA_INVALID_LENGTH;
    }
    if (offset % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return PCI_DMA_UNALIGNED;
    }
    if (offset > buffer->length ||
        (uint64_t)length > (uint64_t)buffer->length - offset) {
        return PCI_DMA_OUT_OF_RANGE;
    }
    end = buffer->physical_base + offset;
    if (end % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return PCI_DMA_UNALIGNED;
    }

    memset(slice, 0, sizeof(*slice));
    slice->published = true;
    slice->physical_base = end;
    slice->physical_end = end + (uint64_t)length;
    slice->length = length;
    slice->direction = buffer->direction;
    return PCI_DMA_OK;
}

const char *pci_dma_status_string(enum pci_dma_status status)
{
    switch (status) {
    case PCI_DMA_OK:
        return "PCI_DMA_OK";
    case PCI_DMA_NULL:
        return "PCI_DMA_NULL";
    case PCI_DMA_INVALID_ADDRESS:
        return "PCI_DMA_INVALID_ADDRESS";
    case PCI_DMA_UNALIGNED:
        return "PCI_DMA_UNALIGNED";
    case PCI_DMA_INVALID_LENGTH:
        return "PCI_DMA_INVALID_LENGTH";
    case PCI_DMA_OVERFLOW:
        return "PCI_DMA_OVERFLOW";
    case PCI_DMA_INVALID_DIRECTION:
        return "PCI_DMA_INVALID_DIRECTION";
    case PCI_DMA_OUT_OF_RANGE:
        return "PCI_DMA_OUT_OF_RANGE";
    default:
        return "PCI_DMA_UNKNOWN";
    }
}
