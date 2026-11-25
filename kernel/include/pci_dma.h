#ifndef HFOC_PCI_DMA_H
#define HFOC_PCI_DMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PCI_DMA_ADDRESS_ALIGNMENT 4096U
#define PCI_DMA_LENGTH_MAX 1048576U

enum pci_dma_direction {
    PCI_DMA_DIRECTION_DEVICE_READ = 0,
    PCI_DMA_DIRECTION_DEVICE_WRITE = 1,
    PCI_DMA_DIRECTION_BIDIRECTIONAL = 2,
};

enum pci_dma_status {
    PCI_DMA_OK = 0,
    PCI_DMA_NULL = 1,
    PCI_DMA_INVALID_ADDRESS = 2,
    PCI_DMA_UNALIGNED = 3,
    PCI_DMA_INVALID_LENGTH = 4,
    PCI_DMA_OVERFLOW = 5,
    PCI_DMA_INVALID_DIRECTION = 6,
    PCI_DMA_OUT_OF_RANGE = 7,
};

struct pci_dma_buffer {
    bool published;
    uint64_t physical_base;
    uint64_t physical_end;
    size_t length;
    enum pci_dma_direction direction;
};

bool pci_dma_direction_valid(enum pci_dma_direction direction);
enum pci_dma_status pci_dma_buffer_publish(
    struct pci_dma_buffer *buffer,
    uint64_t physical_base,
    size_t length,
    enum pci_dma_direction direction);
enum pci_dma_status pci_dma_buffer_slice(
    const struct pci_dma_buffer *buffer,
    uint64_t offset,
    size_t length,
    struct pci_dma_buffer *slice);
const char *pci_dma_status_string(enum pci_dma_status status);

#endif
