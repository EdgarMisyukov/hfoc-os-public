#include "pci_io.h"

#include "arch/x86_64/io.h"

#include <stddef.h>

/*
 * ITEM-492: the kernel's one PCI configuration provider.
 *
 * The legacy mechanism: write the address (enable bit, bus, device, function
 * and the dword-aligned register offset) to 0xCF8 and read or write the data
 * port 0xCFC.  It is the same sequence the per-driver copies performed; the
 * difference is that a consumer with no business owning a PCI driver - the
 * installer's network discovery among them - can now reach the bus.
 */

#define PCI_IO_CONFIG_ADDRESS UINT16_C(0x0CF8)
#define PCI_IO_CONFIG_DATA UINT16_C(0x0CFC)
#define PCI_IO_CONFIG_ENABLE UINT32_C(0x80000000)

static void pci_io_address(uint8_t bus,
                           uint8_t device,
                           uint8_t function,
                           uint16_t offset)
{
    const uint32_t address =
        PCI_IO_CONFIG_ENABLE | ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) | ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    __asm__ volatile ("outl %0, %1" : : "a"(address),
                      "Nd"(PCI_IO_CONFIG_ADDRESS));
}

uint32_t pci_io_read32(uint8_t bus,
                       uint8_t device,
                       uint8_t function,
                       uint16_t offset)
{
    uint32_t value;

    pci_io_address(bus, device, function, offset);
    __asm__ volatile ("inl %1, %0" : "=a"(value)
                      : "Nd"(PCI_IO_CONFIG_DATA));
    return value;
}

void pci_io_write32(uint8_t bus,
                    uint8_t device,
                    uint8_t function,
                    uint16_t offset,
                    uint32_t value)
{
    pci_io_address(bus, device, function, offset);
    __asm__ volatile ("outl %0, %1" : : "a"(value),
                      "Nd"(PCI_IO_CONFIG_DATA));
}

static bool pci_io_read(void *context,
                        uint8_t bus,
                        uint8_t device,
                        uint8_t function,
                        uint16_t offset,
                        uint32_t *value)
{
    (void)context;
    if (value == NULL || (offset % 4U) != 0U) {
        return false;
    }
    *value = pci_io_read32(bus, device, function, offset);
    return true;
}

static bool pci_io_write(void *context,
                         uint8_t bus,
                         uint8_t device,
                         uint8_t function,
                         uint16_t offset,
                         uint32_t value)
{
    (void)context;
    if ((offset % 4U) != 0U) {
        return false;
    }
    pci_io_write32(bus, device, function, offset, value);
    return true;
}

struct pci_config_io *pci_io_kernel(void)
{
    static struct pci_config_io io;

    io.context = NULL;
    io.read32 = pci_io_read;
    io.write32 = pci_io_write;
    return &io;
}
