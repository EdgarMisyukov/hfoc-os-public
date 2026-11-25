#ifndef HFOC_PCI_IO_H
#define HFOC_PCI_IO_H

#include "pci.h"

/*
 * ITEM-492: one PCI configuration provider for the whole kernel.
 *
 * Until this item every consumer - xHCI, the storage probe, the network
 * adapter, the QEMU fixtures - carried its own copy of the same two port-I/O
 * callbacks over 0xCF8/0xCFC.  A device-independent discovery cannot be built
 * on a provider that only exists inside one driver, so the mechanism lives in
 * one place now and every consumer that has no reason to care can share it.
 *
 * The provider is stateless: it reads and writes the legacy configuration
 * mechanism directly, so a caller needs no context of its own.
 */
struct pci_config_io *pci_io_kernel(void);
/* The dword accessors the provider uses, for a caller that wants them alone. */
uint32_t pci_io_read32(uint8_t bus, uint8_t device, uint8_t function,
                       uint16_t offset);
void pci_io_write32(uint8_t bus, uint8_t device, uint8_t function,
                    uint16_t offset, uint32_t value);

#endif
