#if defined(HFOC_PHYSICAL_STORAGE_QEMU_TEST) || \
    defined(HFOC_HFOCFS_QEMU_TEST) || \
    defined(HFOC_HFOCFS_WORKSPACE_QEMU_TEST)

#include "ahci.h"
#include "arch/x86_64/io.h"
#include "block_device.h"
#include "pci.h"
#include "pmm.h"
#include "serial.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define AHCI_QEMU_PCI_CONFIG_ADDRESS_PORT 0xCF8U
#define AHCI_QEMU_PCI_CONFIG_DATA_PORT 0xCFCU
#define AHCI_QEMU_PCI_CONFIG_ENABLE UINT32_C(0x80000000)
#define AHCI_QEMU_PCI_COMMAND_OFFSET 0x04U
#define AHCI_QEMU_PCI_COMMAND_MEMORY_SPACE UINT32_C(0x0002)
#define AHCI_QEMU_PCI_COMMAND_BUS_MASTER UINT32_C(0x0004)
#define AHCI_QEMU_PAGE_SIZE 4096U
#define AHCI_QEMU_DMA_PAGE_COUNT 4U
#define AHCI_QEMU_DMA_BYTES \
    (AHCI_QEMU_PAGE_SIZE * AHCI_QEMU_DMA_PAGE_COUNT)
#define AHCI_QEMU_COMMAND_LIST_OFFSET 0x0000U
#define AHCI_QEMU_FIS_OFFSET 0x1000U
#define AHCI_QEMU_COMMAND_TABLE_OFFSET 0x2000U
#define AHCI_QEMU_DATA_OFFSET 0x3000U
#define AHCI_QEMU_PROOF_LBA 1U
#define AHCI_QEMU_PROOF_BYTES 512U

#ifdef HFOC_PHYSICAL_SATA_ACCEPTANCE_TEST
#define AHCI_PHYSICAL_EVIDENCE_LBA 1024U
#define AHCI_PHYSICAL_EVIDENCE_BYTES 512U
#define AHCI_PHYSICAL_MARKER_BYTES 16U
#define AHCI_PS2_RESET_PORT 0x64U
#define AHCI_PS2_RESET_COMMAND 0xFEU
#define AHCI_PCI_RESET_PORT 0xCF9U
#define AHCI_PCI_RESET_COMMAND 0x0EU

static const unsigned char ahci_physical_phase1[AHCI_PHYSICAL_MARKER_BYTES] = {
    'H', 'F', 'O', 'C', 'P', 'H', 'Y', 'S',
    '1', 'P', '1', 'M', 'A', 'R', 'K', '\n'
};
static const unsigned char ahci_physical_phase2[AHCI_PHYSICAL_MARKER_BYTES] = {
    'H', 'F', 'O', 'C', 'P', 'H', 'Y', 'S',
    '2', 'P', '2', 'M', 'A', 'R', 'K', '\n'
};

static bool ahci_physical_marker_matches(
    const unsigned char *bytes,
    const unsigned char *marker)
{
    size_t index;

    for (index = 0U; index < AHCI_PHYSICAL_MARKER_BYTES; ++index) {
        if (bytes[index] != marker[index]) {
            return false;
        }
    }
    return true;
}

static void ahci_physical_marker_fill(
    unsigned char *bytes,
    const unsigned char *marker)
{
    size_t index;

    memset(bytes, 0, AHCI_PHYSICAL_EVIDENCE_BYTES);
    for (index = 0U; index < AHCI_PHYSICAL_MARKER_BYTES; ++index) {
        bytes[index] = marker[index];
    }
}

static void ahci_physical_warm_reset(void)
    __attribute__((noreturn));

static void ahci_physical_warm_reset(void)
{
    serial_write("[AHCI] physical warm reset\n");
    for (;;) {
        outb(AHCI_PS2_RESET_PORT, AHCI_PS2_RESET_COMMAND);
        for (uint32_t wait = 0U; wait < 10000U; ++wait) {
            __asm__ volatile ("pause" : : : "memory");
        }
        outb(AHCI_PCI_RESET_PORT, AHCI_PCI_RESET_COMMAND);
        for (uint32_t wait = 0U; wait < 100000U; ++wait) {
            __asm__ volatile ("pause" : : : "memory");
        }
    }
}
#endif

static uint64_t ahci_qemu_hhdm;
static int ahci_qemu_pci_context;
static volatile uint8_t *ahci_qemu_mmio_base;
static uint64_t ahci_qemu_mmio_length;
static uint64_t ahci_qemu_dma_physical;
static unsigned char *ahci_qemu_dma_virtual;

static void ahci_qemu_failure(const char *message)
    __attribute__((noreturn));

static void ahci_qemu_failure(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[AHCI] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void ahci_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        ahci_qemu_failure(message);
    }
}

static void ahci_qemu_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t ahci_qemu_inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t ahci_qemu_pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset)
{
    const uint32_t address =
        AHCI_QEMU_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    ahci_qemu_outl(AHCI_QEMU_PCI_CONFIG_ADDRESS_PORT, address);
    return ahci_qemu_inl(AHCI_QEMU_PCI_CONFIG_DATA_PORT);
}

static void ahci_qemu_pci_config_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    const uint32_t address =
        AHCI_QEMU_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    ahci_qemu_outl(AHCI_QEMU_PCI_CONFIG_ADDRESS_PORT, address);
    ahci_qemu_outl(AHCI_QEMU_PCI_CONFIG_DATA_PORT, value);
}

static bool ahci_qemu_pci_read(
    void *context,
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
    *value = ahci_qemu_pci_config_read32(
        bus, device, function, offset);
    return true;
}

static bool ahci_qemu_pci_write(
    void *context,
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
    ahci_qemu_pci_config_write32(
        bus, device, function, offset, value);
    return true;
}

static void ahci_qemu_enable_pci_command(
    const struct pci_location *location)
{
    uint32_t command;
    uint32_t enabled;

    command = ahci_qemu_pci_config_read32(
        location->bus, location->device, location->function,
        AHCI_QEMU_PCI_COMMAND_OFFSET);
    enabled = command |
              AHCI_QEMU_PCI_COMMAND_MEMORY_SPACE |
              AHCI_QEMU_PCI_COMMAND_BUS_MASTER;
    if (command != enabled) {
        ahci_qemu_pci_config_write32(
            location->bus, location->device, location->function,
            AHCI_QEMU_PCI_COMMAND_OFFSET, enabled);
    }
}

static volatile uint8_t *ahci_qemu_map_mmio(
    uint64_t physical_base,
    uint64_t length)
{
    uint64_t page_count;
    uint64_t page_index;
    volatile void *mapped = NULL;
    enum vmm_status status;

    ahci_qemu_require(
        physical_base != 0U && length != 0U &&
            (physical_base % AHCI_QEMU_PAGE_SIZE) == 0U,
        "AHCI BAR physical base is not page aligned");
    ahci_qemu_require(
        length <= UINT64_MAX - physical_base,
        "AHCI BAR physical range overflows");
    page_count = (length + AHCI_QEMU_PAGE_SIZE - 1U) /
                 AHCI_QEMU_PAGE_SIZE;
    for (page_index = 0U; page_index < page_count; ++page_index) {
        status = vmm_map_mmio_page(
            physical_base + page_index * AHCI_QEMU_PAGE_SIZE,
            ahci_qemu_hhdm, &mapped);
        ahci_qemu_require(
            status == VMM_OK, "AHCI BAR MMIO mapping failed");
    }
    return (volatile uint8_t *)(uintptr_t)(
        physical_base + ahci_qemu_hhdm);
}

static bool ahci_qemu_mmio_read32(
    void *context,
    uint64_t offset,
    uint32_t *value)
{
    volatile uint32_t *register_address;

    (void)context;
    if (value == NULL || offset >= ahci_qemu_mmio_length ||
        ahci_qemu_mmio_length - offset < 4U) {
        return false;
    }
    register_address = (volatile uint32_t *)(
        (uintptr_t)ahci_qemu_mmio_base + (uintptr_t)offset);
    *value = *register_address;
    return true;
}

static bool ahci_qemu_mmio_write32(
    void *context,
    uint64_t offset,
    uint32_t value)
{
    volatile uint32_t *register_address;

    (void)context;
    if (offset >= ahci_qemu_mmio_length ||
        ahci_qemu_mmio_length - offset < 4U) {
        return false;
    }
    register_address = (volatile uint32_t *)(
        (uintptr_t)ahci_qemu_mmio_base + (uintptr_t)offset);
    *register_address = value;
    return true;
}

static bool ahci_qemu_dma_read(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length)
{
    const unsigned char *source;
    unsigned char *destination = (unsigned char *)buffer;
    size_t index;
    uint64_t relative;

    (void)context;
    if (buffer == NULL || length == 0U ||
        address < ahci_qemu_dma_physical) {
        return false;
    }
    relative = address - ahci_qemu_dma_physical;
    if (relative >= AHCI_QEMU_DMA_BYTES ||
        length > AHCI_QEMU_DMA_BYTES - (size_t)relative) {
        return false;
    }
    source = ahci_qemu_dma_virtual + (size_t)relative;
    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
    return true;
}

static bool ahci_qemu_dma_write(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    const unsigned char *source = (const unsigned char *)buffer;
    unsigned char *destination;
    size_t index;
    uint64_t relative;

    (void)context;
    if (buffer == NULL || length == 0U ||
        address < ahci_qemu_dma_physical) {
        return false;
    }
    relative = address - ahci_qemu_dma_physical;
    if (relative >= AHCI_QEMU_DMA_BYTES ||
        length > AHCI_QEMU_DMA_BYTES - (size_t)relative) {
        return false;
    }
    destination = ahci_qemu_dma_virtual + (size_t)relative;
    for (index = 0U; index < length; ++index) {
        destination[index] = source[index];
    }
    return true;
}

static bool ahci_qemu_port_has_ata_device(
    const struct ahci_port_map *map)
{
    size_t index;

    for (index = 0U; index < map->port_count; ++index) {
        if (map->ports[index].device_attached &&
            map->ports[index].supported) {
            return true;
        }
    }
    return false;
}

static uint32_t ahci_qemu_first_ata_port(
    const struct ahci_port_map *map)
{
    size_t index;

    for (index = 0U; index < map->port_count; ++index) {
        if (map->ports[index].device_attached &&
            map->ports[index].supported) {
            return map->ports[index].index;
        }
    }
    ahci_qemu_failure("no ATA device found on AHCI controller");
}

#ifndef HFOC_PHYSICAL_SATA_ACCEPTANCE_TEST
static void ahci_qemu_fill_pattern(unsigned char *bytes, size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = (unsigned char)(0xA5U ^ (index & 0x7FU));
    }
}

static bool ahci_qemu_matches_pattern(
    const unsigned char *bytes,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (bytes[index] !=
            (unsigned char)(0xA5U ^ (index & 0x7FU))) {
            return false;
        }
    }
    return true;
}
#endif

static void ahci_qemu_run_proof(void)
{
    struct pci_config_io pci_io;
    struct ahci_mmio_io mmio_io;
    struct ahci_dma_io dma_io;
    struct ahci_controller controller;
    struct ahci_port_map port_map;
    struct ahci_ata_identity identity;
    struct ahci_block_device backend;
    struct block_device *device;
    struct pci_device_header header;
    struct pci_location location;
    enum ahci_status ahci_status;
    enum block_device_status block_status;
    enum pci_status pci_status;
    unsigned int bus_index;
    unsigned int device_index;
    unsigned int function_index;
    uint32_t port_index;
    bool selected = false;
#ifndef HFOC_PHYSICAL_SATA_ACCEPTANCE_TEST
    uint64_t initial_free_pages;
    size_t found;
#endif

    serial_write("[AHCI] proof init\n");
    memset(&pci_io, 0, sizeof(pci_io));
    pci_io.context = &ahci_qemu_pci_context;
    pci_io.read32 = ahci_qemu_pci_read;
    pci_io.write32 = ahci_qemu_pci_write;

    for (bus_index = 0U;
         bus_index < PCI_MAX_BUS && !selected;
         ++bus_index) {
        for (device_index = 0U;
             device_index < PCI_MAX_DEVICE && !selected;
             ++device_index) {
            for (function_index = 0U;
                 function_index < PCI_MAX_FUNCTION && !selected;
                 ++function_index) {
                const struct pci_bar *bar;

                memset(&header, 0, sizeof(header));
                location.bus = (uint8_t)bus_index;
                location.device = (uint8_t)device_index;
                location.function = (uint8_t)function_index;
                pci_status = pci_device_read(
                    &pci_io, &location, &header);
                if (pci_status == PCI_DEVICE_ABSENT) {
                    continue;
                }
                ahci_qemu_require(
                    pci_status == PCI_OK,
                    "PCI configuration read failed during AHCI scan");
                if (header.base_class != PCI_CLASS_MASS_STORAGE ||
                    header.subclass != PCI_SUBCLASS_SATA ||
                    header.prog_if != PCI_PROGIF_AHCI) {
                    continue;
                }

                ahci_status = ahci_controller_bind(
                    &header, &controller);
                if (ahci_status != AHCI_OK) {
                    continue;
                }
                ahci_qemu_enable_pci_command(&header.location);
                bar = &header.bars[AHCI_MEMORY_BAR_INDEX];
                ahci_qemu_mmio_base = ahci_qemu_map_mmio(
                    bar->base, bar->length);
                ahci_qemu_mmio_length = bar->length;
                memset(&mmio_io, 0, sizeof(mmio_io));
                mmio_io.read32 = ahci_qemu_mmio_read32;
                mmio_io.write32 = ahci_qemu_mmio_write32;

                ahci_status = ahci_controller_discover(
                    &mmio_io, &controller);
                if (ahci_status != AHCI_OK) {
                    continue;
                }
                ahci_status = ahci_controller_activate(
                    &mmio_io, &controller);
                if (ahci_status != AHCI_OK) {
                    continue;
                }
                ahci_status = ahci_controller_discover_ports(
                    &mmio_io, &controller, &port_map);
                if (ahci_status != AHCI_OK) {
                    continue;
                }
                if (!ahci_qemu_port_has_ata_device(&port_map)) {
                    continue;
                }
                selected = true;
                serial_write("[AHCI] SATA/AHCI controller selected\n");
            }
        }
    }
    ahci_qemu_require(selected, "no QEMU SATA/AHCI controller with ATA disk");

    port_index = ahci_qemu_first_ata_port(&port_map);
#ifndef HFOC_PHYSICAL_SATA_ACCEPTANCE_TEST
    initial_free_pages = pmm_free_page_count();
#endif
    ahci_qemu_dma_physical = pmm_alloc_contiguous_pages(
        AHCI_QEMU_DMA_PAGE_COUNT);
    ahci_qemu_require(
        ahci_qemu_dma_physical != 0U,
        "AHCI DMA memory allocation failed");
    ahci_qemu_dma_virtual = (unsigned char *)(uintptr_t)(
        ahci_qemu_dma_physical + ahci_qemu_hhdm);

    memset(&dma_io, 0, sizeof(dma_io));
    dma_io.read = ahci_qemu_dma_read;
    dma_io.write = ahci_qemu_dma_write;

    ahci_status = ahci_port_dma_start(
        &mmio_io, &controller, &port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_FIS_OFFSET);
    ahci_qemu_require(
        ahci_status == AHCI_OK, "AHCI port DMA start failed");
    serial_write("[AHCI] port DMA start OK\n");

    ahci_status = ahci_ata_identify(
        &mmio_io, &dma_io, &controller, &port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_TABLE_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_DATA_OFFSET,
        &identity);
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[AHCI] identify status: ");
        serial_write(ahci_status_string(ahci_status));
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    ahci_qemu_require(
        ahci_status == AHCI_OK, "AHCI IDENTIFY command failed");
    ahci_qemu_require(
        identity.valid && identity.block_size == 512U &&
            identity.sector_count != 0U,
        "AHCI IDENTIFY returned invalid capacity");
    serial_write("[AHCI] IDENTIFY OK\n");

    ahci_status = ahci_block_device_bind(
        &mmio_io, &dma_io, &controller, &port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_TABLE_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_DATA_OFFSET,
        &identity, &backend);
    ahci_qemu_require(
        ahci_status == AHCI_OK, "AHCI block-device bind failed");
    device = &backend.device;

#ifdef HFOC_PHYSICAL_SATA_ACCEPTANCE_TEST
    {
        unsigned char *evidence =
            ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET;
        bool phase2;

        ahci_qemu_require(
            identity.sector_count >= UINT64_C(1000000),
            "AHCI physical target capacity is unexpectedly small");
        memset(evidence, 0, AHCI_PHYSICAL_EVIDENCE_BYTES);
        block_status = block_device_read(
            device,
            (uint64_t)AHCI_PHYSICAL_EVIDENCE_LBA * 512U,
            evidence, AHCI_PHYSICAL_EVIDENCE_BYTES);
        ahci_qemu_require(
            block_status == BLOCK_DEVICE_OK,
            "AHCI physical evidence read failed");

        phase2 = ahci_physical_marker_matches(
            evidence, ahci_physical_phase1);
        ahci_qemu_require(
            !ahci_physical_marker_matches(
                evidence, ahci_physical_phase2),
            "AHCI physical evidence already complete");

        ahci_physical_marker_fill(
            evidence,
            phase2 ? ahci_physical_phase2 : ahci_physical_phase1);
        block_status = block_device_write(
            device,
            (uint64_t)AHCI_PHYSICAL_EVIDENCE_LBA * 512U,
            evidence, AHCI_PHYSICAL_EVIDENCE_BYTES);
        ahci_qemu_require(
            block_status == BLOCK_DEVICE_OK,
            "AHCI physical evidence write failed");
        block_status = block_device_flush(device);
        ahci_qemu_require(
            block_status == BLOCK_DEVICE_OK,
            "AHCI physical evidence flush failed");

        memset(evidence, 0, AHCI_PHYSICAL_EVIDENCE_BYTES);
        block_status = block_device_read(
            device,
            (uint64_t)AHCI_PHYSICAL_EVIDENCE_LBA * 512U,
            evidence, AHCI_PHYSICAL_EVIDENCE_BYTES);
        ahci_qemu_require(
            block_status == BLOCK_DEVICE_OK,
            "AHCI physical evidence readback failed");
        ahci_qemu_require(
            ahci_physical_marker_matches(
                evidence,
                phase2 ? ahci_physical_phase2 : ahci_physical_phase1),
            "AHCI physical evidence readback mismatch");

        serial_write(
            phase2 ?
                "[AHCI] physical boot 2 marker written\n" :
                "[AHCI] physical boot 1 marker written\n");
        serial_write("[AHCI] physical evidence proof OK\n");
        ahci_physical_warm_reset();
    }
#else
    block_status = block_device_read(
        device, (uint64_t)AHCI_QEMU_PROOF_LBA * 512U,
        ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
        AHCI_QEMU_PROOF_BYTES);
    ahci_qemu_require(
        block_status == BLOCK_DEVICE_OK,
        "AHCI initial block read failed");
    if (ahci_qemu_matches_pattern(
            ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
            AHCI_QEMU_PROOF_BYTES)) {
        serial_write("[AHCI] persistent readback OK\n");
    } else {
        serial_write("[AHCI] fresh disk detected\n");
    }

    ahci_qemu_fill_pattern(
        ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
        AHCI_QEMU_PROOF_BYTES);
    block_status = block_device_write(
        device, (uint64_t)AHCI_QEMU_PROOF_LBA * 512U,
        ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
        AHCI_QEMU_PROOF_BYTES);
    ahci_qemu_require(
        block_status == BLOCK_DEVICE_OK,
        "AHCI block write failed");
    block_status = block_device_flush(device);
    ahci_qemu_require(
        block_status == BLOCK_DEVICE_OK,
        "AHCI flush failed");
    serial_write("[AHCI] write flush OK\n");

    memset(ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET, 0,
           AHCI_QEMU_PROOF_BYTES);
    block_status = block_device_read(
        device, (uint64_t)AHCI_QEMU_PROOF_LBA * 512U,
        ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
        AHCI_QEMU_PROOF_BYTES);
    ahci_qemu_require(
        block_status == BLOCK_DEVICE_OK,
        "AHCI post-write block read failed");
    ahci_qemu_require(
        ahci_qemu_matches_pattern(
            ahci_qemu_dma_virtual + AHCI_QEMU_DATA_OFFSET,
            AHCI_QEMU_PROOF_BYTES),
        "AHCI post-write readback mismatch");
    serial_write("[AHCI] post-write readback OK\n");

    found = 0U;
    while (found < AHCI_QEMU_PROOF_BYTES) {
        ahci_qemu_dma_virtual[AHCI_QEMU_DATA_OFFSET + found] = 0U;
        ++found;
    }
    ahci_qemu_require(
        pmm_free_contiguous_pages(
            ahci_qemu_dma_physical, AHCI_QEMU_DMA_PAGE_COUNT),
        "AHCI DMA memory free failed");
    ahci_qemu_dma_physical = 0U;
    ahci_qemu_dma_virtual = NULL;
    ahci_qemu_require(
        pmm_free_page_count() == initial_free_pages,
        "AHCI DMA ownership is not balanced");
    serial_write("[AHCI] balanced ownership\n");
    serial_write("[AHCI] clean shutdown\n");
    serial_write("[AHCI] proof OK\n");
#endif
}

#if defined(HFOC_HFOCFS_QEMU_TEST) || \
    defined(HFOC_HFOCFS_WORKSPACE_QEMU_TEST)
/*
 * ITEM-630 QEMU proof: expose the same production AHCI bind used by the
 * physical-storage proof, but leave the controller/port/DMA owned by this
 * translation unit for the HFOCFS boot test.  No raw host disk is addressed
 * here: the caller receives the ordinary block_device for the QEMU ide-hd.
 */
static struct ahci_mmio_io hfocfs_mmio_io;
static struct ahci_dma_io hfocfs_dma_io;
static struct ahci_controller hfocfs_controller;
static struct ahci_port_map hfocfs_port_map;
static struct ahci_ata_identity hfocfs_identity;
static struct ahci_block_device hfocfs_backend;

int ahci_qemu_bind_hfocfs(uint64_t hhdm_offset,
                          struct block_device **device_out)
{
    struct pci_config_io pci_io;
    struct pci_device_header header;
    struct pci_location location;
    enum ahci_status ahci_status;
    enum pci_status pci_status;
    unsigned int bus_index;
    unsigned int device_index;
    unsigned int function_index;
    uint32_t port_index;
    bool selected = false;

    if (device_out == NULL) {
        return -1;
    }
    *device_out = NULL;
    ahci_qemu_hhdm = hhdm_offset;
    memset(&pci_io, 0, sizeof(pci_io));
    pci_io.context = &ahci_qemu_pci_context;
    pci_io.read32 = ahci_qemu_pci_read;
    pci_io.write32 = ahci_qemu_pci_write;

    for (bus_index = 0U;
         bus_index < PCI_MAX_BUS && !selected;
         ++bus_index) {
        for (device_index = 0U;
             device_index < PCI_MAX_DEVICE && !selected;
             ++device_index) {
            for (function_index = 0U;
                 function_index < PCI_MAX_FUNCTION && !selected;
                 ++function_index) {
                const struct pci_bar *bar;

                memset(&header, 0, sizeof(header));
                location.bus = (uint8_t)bus_index;
                location.device = (uint8_t)device_index;
                location.function = (uint8_t)function_index;
                pci_status = pci_device_read(&pci_io, &location, &header);
                if (pci_status == PCI_DEVICE_ABSENT) {
                    continue;
                }
                if (pci_status != PCI_OK ||
                    header.base_class != PCI_CLASS_MASS_STORAGE ||
                    header.subclass != PCI_SUBCLASS_SATA ||
                    header.prog_if != PCI_PROGIF_AHCI) {
                    continue;
                }
                if (ahci_controller_bind(&header, &hfocfs_controller) !=
                    AHCI_OK) {
                    continue;
                }
                ahci_qemu_enable_pci_command(&header.location);
                bar = &header.bars[AHCI_MEMORY_BAR_INDEX];
                ahci_qemu_mmio_base = ahci_qemu_map_mmio(
                    bar->base, bar->length);
                ahci_qemu_mmio_length = bar->length;
                memset(&hfocfs_mmio_io, 0, sizeof(hfocfs_mmio_io));
                hfocfs_mmio_io.read32 = ahci_qemu_mmio_read32;
                hfocfs_mmio_io.write32 = ahci_qemu_mmio_write32;

                if (ahci_controller_discover(&hfocfs_mmio_io,
                                             &hfocfs_controller) != AHCI_OK ||
                    ahci_controller_activate(&hfocfs_mmio_io,
                                             &hfocfs_controller) != AHCI_OK ||
                    ahci_controller_discover_ports(&hfocfs_mmio_io,
                                                   &hfocfs_controller,
                                                   &hfocfs_port_map) !=
                        AHCI_OK ||
                    !ahci_qemu_port_has_ata_device(&hfocfs_port_map)) {
                    continue;
                }
                selected = true;
            }
        }
    }
    if (!selected) {
        serial_write("[AHCI] no QEMU SATA/AHCI disk for HFOCFS\n");
        return -1;
    }

    port_index = ahci_qemu_first_ata_port(&hfocfs_port_map);
    ahci_qemu_dma_physical = pmm_alloc_contiguous_pages(
        AHCI_QEMU_DMA_PAGE_COUNT);
    if (ahci_qemu_dma_physical == 0U) {
        serial_write("[AHCI] HFOCFS DMA allocation failed\n");
        return -1;
    }
    ahci_qemu_dma_virtual = (unsigned char *)(uintptr_t)(
        ahci_qemu_dma_physical + ahci_qemu_hhdm);

    memset(&hfocfs_dma_io, 0, sizeof(hfocfs_dma_io));
    hfocfs_dma_io.read = ahci_qemu_dma_read;
    hfocfs_dma_io.write = ahci_qemu_dma_write;

    ahci_status = ahci_port_dma_start(
        &hfocfs_mmio_io, &hfocfs_controller, &hfocfs_port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_FIS_OFFSET);
    if (ahci_status != AHCI_OK) {
        serial_write("[AHCI] HFOCFS port DMA start failed\n");
        return -1;
    }
    ahci_status = ahci_ata_identify(
        &hfocfs_mmio_io, &hfocfs_dma_io, &hfocfs_controller,
        &hfocfs_port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_TABLE_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_DATA_OFFSET,
        &hfocfs_identity);
    if (ahci_status != AHCI_OK || !hfocfs_identity.valid ||
        hfocfs_identity.block_size != 512U ||
        hfocfs_identity.sector_count < (16U * 1024U * 1024U / 512U)) {
        serial_write("[AHCI] HFOCFS IDENTIFY failed or disk too small\n");
        return -1;
    }
    ahci_status = ahci_block_device_bind(
        &hfocfs_mmio_io, &hfocfs_dma_io, &hfocfs_controller,
        &hfocfs_port_map, port_index,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_LIST_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_COMMAND_TABLE_OFFSET,
        ahci_qemu_dma_physical + AHCI_QEMU_DATA_OFFSET,
        &hfocfs_identity, &hfocfs_backend);
    if (ahci_status != AHCI_OK) {
        serial_write("[AHCI] HFOCFS block-device bind failed\n");
        return -1;
    }
    *device_out = &hfocfs_backend.device;
    return 0;
}
#endif

void ahci_qemu_test_init(uint64_t hhdm_offset)
{
    ahci_qemu_hhdm = hhdm_offset;
    ahci_qemu_run_proof();
}

#else
/* Keep the macro-gated test translation unit non-empty in normal builds. */
int hfoc_ahci_qemu_translation_unit_marker;
#endif
