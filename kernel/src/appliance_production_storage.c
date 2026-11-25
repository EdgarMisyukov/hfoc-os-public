#include "appliance_production_storage.h"

#include "ahci.h"
#include "arch/x86_64/io.h"
#include "arch/x86_64/timer.h"
#include "nvme.h"
#include "pci.h"
#include "pmm.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define PRODUCTION_STORAGE_PCI_CONFIG_ADDRESS 0xCF8U
#define PRODUCTION_STORAGE_PCI_CONFIG_DATA 0xCFCU
#define PRODUCTION_STORAGE_PCI_CONFIG_ENABLE UINT32_C(0x80000000)
#define PRODUCTION_STORAGE_PCI_COMMAND_OFFSET 0x04U
#define PRODUCTION_STORAGE_PCI_COMMAND_MEMORY UINT32_C(0x0002)
#define PRODUCTION_STORAGE_PCI_COMMAND_BUS_MASTER UINT32_C(0x0004)
#define PRODUCTION_STORAGE_PAGE_SIZE 4096U
#define PRODUCTION_STORAGE_AHCI_DMA_PAGES 4U
#define PRODUCTION_STORAGE_AHCI_DMA_BYTES \
    (PRODUCTION_STORAGE_PAGE_SIZE * PRODUCTION_STORAGE_AHCI_DMA_PAGES)
#define PRODUCTION_STORAGE_COMMAND_LIST_OFFSET 0x0000U
#define PRODUCTION_STORAGE_FIS_OFFSET 0x1000U
#define PRODUCTION_STORAGE_COMMAND_TABLE_OFFSET 0x2000U
#define PRODUCTION_STORAGE_DATA_OFFSET 0x3000U
#define PRODUCTION_STORAGE_NVME_DMA_PAGES 8U
#define PRODUCTION_STORAGE_NVME_DMA_BYTES \
    (PRODUCTION_STORAGE_PAGE_SIZE * PRODUCTION_STORAGE_NVME_DMA_PAGES)
#define PRODUCTION_STORAGE_NVME_ADMIN_SQ_OFFSET 0x0000U
#define PRODUCTION_STORAGE_NVME_ADMIN_CQ_OFFSET 0x1000U
#define PRODUCTION_STORAGE_NVME_IO_SQ_OFFSET 0x2000U
#define PRODUCTION_STORAGE_NVME_IO_CQ_OFFSET 0x3000U
#define PRODUCTION_STORAGE_NVME_IDENTIFY_OFFSET 0x4000U
#define PRODUCTION_STORAGE_NVME_DATA_OFFSET 0x5000U
#define PRODUCTION_STORAGE_NVME_QUEUE_CAPACITY 8U
#define PRODUCTION_STORAGE_NVME_NAMESPACE_ID 1U
#define PRODUCTION_STORAGE_BACKEND_CAPACITY 8U
#define PRODUCTION_STORAGE_PCI_HEADER_CAPACITY 8U

struct production_storage_ahci {
    bool used;
    uint64_t mmio_base;
    uint64_t mmio_length;
    uint64_t dma_phys;
    unsigned char *dma_virtual;
    struct pci_location location;
    struct ahci_mmio_io mmio;
    struct ahci_dma_io dma;
    struct ahci_controller controller;
    struct ahci_port_map ports;
    struct ahci_ata_identity identity;
    struct ahci_block_device backend;
};

struct production_storage_nvme {
    bool used;
    uint64_t mmio_base;
    uint64_t mmio_length;
    uint64_t dma_phys;
    unsigned char *dma_virtual;
    struct nvme_mmio_io mmio;
    struct nvme_queue_memory_io dma;
    struct nvme_controller controller;
    struct nvme_controller_identity identity;
    struct nvme_queue admin_queue;
    struct nvme_queue io_queue;
    struct nvme_namespace namespace_info;
    struct nvme_block_device backend;
};

struct production_storage_state {
    bool initialized;
    bool probed;
    uint64_t hhdm_offset;
    size_t candidate_count;
    size_t ahci_count;
    size_t nvme_count;
    /* ITEM-423: what the class scan matched and where the last NVMe bring-up
       stopped, so a machine that offered fewer disks than it carries can say
       which of the three causes it was. */
    size_t pci_ahci_count;
    size_t pci_nvme_count;
    int nvme_status;
    /*
     * ITEM-429: the bring-up call the last NVMe controller stopped at, and the
     * milliseconds its last controller-ready wait cost.  `NVME_TIMEOUT` alone
     * cannot tell "the controller never reported ready" from "the driver never
     * waited long enough to know".
     */
    const char *nvme_step;
    uint32_t nvme_wait_ms;
    /*
     * ITEM-540: the probe's own state, so the evidence says whether storage
     * was never searched or the search itself failed, and what it stopped on.
     */
    bool probe_attempted;
    const char *probe_stage;
    enum pci_status probe_status;
    struct pci_scan_failure probe_failure;
    struct appliance_production_boot_backend candidates[
        APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY];
    /*
     * ITEM-394: one identity and model per candidate, in candidate order, so
     * the installer can publish the disks it found and select exactly the one
     * the medium names.
     */
    char identities[APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY]
                   [APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH];
    char models[APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY]
               [APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH];
    struct production_storage_ahci ports[
        PRODUCTION_STORAGE_BACKEND_CAPACITY];
    struct production_storage_nvme namespaces[
        PRODUCTION_STORAGE_BACKEND_CAPACITY];
};

static struct production_storage_state production_storage;

static uint32_t production_storage_pci_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset)
{
    const uint32_t address =
        PRODUCTION_STORAGE_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);
    uint32_t value;

    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(address), "Nd"((uint16_t)PRODUCTION_STORAGE_PCI_CONFIG_ADDRESS)
        : "memory");
    __asm__ volatile (
        "inl %1, %0"
        : "=a"(value)
        : "Nd"((uint16_t)PRODUCTION_STORAGE_PCI_CONFIG_DATA)
        : "memory");
    return value;
}

static void production_storage_pci_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    const uint32_t address =
        PRODUCTION_STORAGE_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(address), "Nd"((uint16_t)PRODUCTION_STORAGE_PCI_CONFIG_ADDRESS)
        : "memory");
    __asm__ volatile (
        "outl %0, %1"
        :
        : "a"(value), "Nd"((uint16_t)PRODUCTION_STORAGE_PCI_CONFIG_DATA)
        : "memory");
}

static bool production_storage_pci_read(
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
    *value = production_storage_pci_read32(
        bus, device, function, offset);
    return true;
}

static bool production_storage_pci_write(
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
    production_storage_pci_write32(
        bus, device, function, offset, value);
    return true;
}

static void production_storage_enable_pci_device(
    const struct pci_location *location)
{
    uint32_t command = production_storage_pci_read32(
        location->bus, location->device, location->function,
        PRODUCTION_STORAGE_PCI_COMMAND_OFFSET);
    const uint32_t enabled =
        command |
        PRODUCTION_STORAGE_PCI_COMMAND_MEMORY |
        PRODUCTION_STORAGE_PCI_COMMAND_BUS_MASTER;

    if (enabled != command) {
        production_storage_pci_write32(
            location->bus, location->device, location->function,
            PRODUCTION_STORAGE_PCI_COMMAND_OFFSET, enabled);
    }
}

static bool production_storage_map_mmio(
    uint64_t physical_base,
    uint64_t length)
{
    uint64_t page_count;
    uint64_t page;

    if (physical_base == 0U || length == 0U ||
        physical_base % PRODUCTION_STORAGE_PAGE_SIZE != 0U ||
        length > UINT64_MAX - physical_base) {
        return false;
    }
    page_count =
        (length + PRODUCTION_STORAGE_PAGE_SIZE - 1U) /
        PRODUCTION_STORAGE_PAGE_SIZE;
    for (page = 0U; page < page_count; ++page) {
        volatile void *mapped = NULL;

        if (vmm_map_mmio_page(
                physical_base + page * PRODUCTION_STORAGE_PAGE_SIZE,
                production_storage.hhdm_offset, &mapped) != VMM_OK) {
            return false;
        }
    }
    return true;
}

static bool production_storage_mmio_read32(
    void *context,
    uint64_t offset,
    uint32_t *value)
{
    const struct production_storage_ahci *controller =
        (const struct production_storage_ahci *)context;

    if (controller == NULL || value == NULL ||
        offset > controller->mmio_length ||
        controller->mmio_length - offset < 4U) {
        return false;
    }
    *value = *(volatile uint32_t *)(uintptr_t)(
        (controller->mmio_base + production_storage.hhdm_offset) + offset);
    return true;
}

static bool production_storage_mmio_write32(
    void *context,
    uint64_t offset,
    uint32_t value)
{
    const struct production_storage_ahci *controller =
        (const struct production_storage_ahci *)context;

    if (controller == NULL ||
        offset > controller->mmio_length ||
        controller->mmio_length - offset < 4U) {
        return false;
    }
    *(volatile uint32_t *)(uintptr_t)(
        (controller->mmio_base + production_storage.hhdm_offset) + offset) =
        value;
    return true;
}

static bool production_storage_dma_read(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length)
{
    const struct production_storage_ahci *controller =
        (const struct production_storage_ahci *)context;
    uint64_t relative;

    if (controller == NULL || buffer == NULL || length == 0U ||
        address < controller->dma_phys) {
        return false;
    }
    relative = address - controller->dma_phys;
    if (relative >= PRODUCTION_STORAGE_AHCI_DMA_BYTES ||
        length > PRODUCTION_STORAGE_AHCI_DMA_BYTES - (size_t)relative) {
        return false;
    }
    memcpy(buffer, controller->dma_virtual + (size_t)relative, length);
    return true;
}

static bool production_storage_dma_write(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    const struct production_storage_ahci *controller =
        (const struct production_storage_ahci *)context;
    uint64_t relative;

    if (controller == NULL || buffer == NULL || length == 0U ||
        address < controller->dma_phys) {
        return false;
    }
    relative = address - controller->dma_phys;
    if (relative >= PRODUCTION_STORAGE_AHCI_DMA_BYTES ||
        length > PRODUCTION_STORAGE_AHCI_DMA_BYTES - (size_t)relative) {
        return false;
    }
    memcpy(controller->dma_virtual + (size_t)relative, buffer, length);
    return true;
}

static uint64_t production_storage_identity(
    const struct pci_location *location,
    uint32_t port_index)
{
    return ((uint64_t)location->bus << 24U) |
           ((uint64_t)location->device << 16U) |
           ((uint64_t)location->function << 8U) |
           port_index;
}

static void production_storage_copy_text(
    char *destination,
    size_t capacity,
    const char *source)
{
    size_t index;

    if (destination == NULL || capacity == 0U) {
        return;
    }
    memset(destination, 0, capacity);
    if (source == NULL) {
        return;
    }
    for (index = 0U; index + 1U < capacity && source[index] != '\0';
         ++index) {
        destination[index] = source[index];
    }
}

static void production_storage_hex_digit(
    char *destination,
    unsigned int value)
{
    destination[0U] = (char)(value < 10U ? ('0' + value)
                                         : ('a' + (value - 10U)));
    destination[1U] = '\0';
}

/*
 * NVMe identity is derived from the controller's location until its Identify
 * Controller serial is parsed; the string is stable across boots and names a
 * single namespace, which is what the media declaration needs.
 */
static void production_storage_nvme_identity(
    const struct pci_location *location,
    char *destination,
    size_t capacity)
{
    char text[24U];
    size_t length = 0U;

    if (location == NULL || destination == NULL || capacity == 0U) {
        return;
    }
    memset(text, 0, sizeof(text));
    text[length++] = 'N';
    text[length++] = 'V';
    text[length++] = 'M';
    text[length++] = 'E';
    text[length++] = '-';
    production_storage_hex_digit(&text[length],
                                 (location->bus >> 4U) & 0xFU);
    length += 2U;
    production_storage_hex_digit(&text[length], location->bus & 0xFU);
    length += 2U;
    text[length++] = ':';
    production_storage_hex_digit(&text[length],
                                 (location->device >> 4U) & 0xFU);
    length += 2U;
    production_storage_hex_digit(&text[length], location->device & 0xFU);
    length += 2U;
    text[length++] = '.';
    production_storage_hex_digit(&text[length],
                                 location->function & 0xFU);
    length += 2U;
    text[length] = '\0';
    for (length = 0U; length < sizeof(text); ++length) {
        if (text[length] >= 'a' && text[length] <= 'f') {
            text[length] = (char)(text[length] - 'a' + 'A');
        }
    }
    production_storage_copy_text(destination, capacity, text);
}

static size_t production_storage_add_ahci_candidates(
    const struct pci_device_header *header)
{
    struct production_storage_ahci scan;
    size_t added = 0U;
    enum ahci_status ahci_status;
    size_t port_offset;

    if (header == NULL) {
        return 0U;
    }
    memset(&scan, 0, sizeof(scan));
    scan.location = header->location;

    ahci_status = ahci_controller_bind(header, &scan.controller);
    if (ahci_status != AHCI_OK) {
        return 0U;
    }
    production_storage_enable_pci_device(&header->location);
    scan.mmio_base = scan.controller.mmio_base;
    scan.mmio_length = header->bars[AHCI_MEMORY_BAR_INDEX].length;
    if (!production_storage_map_mmio(
            scan.mmio_base, scan.mmio_length)) {
        return 0U;
    }

    scan.mmio.context = &scan;
    scan.mmio.read32 = production_storage_mmio_read32;
    scan.mmio.write32 = production_storage_mmio_write32;
    if (ahci_controller_discover(&scan.mmio, &scan.controller) !=
            AHCI_OK ||
        ahci_controller_activate(&scan.mmio, &scan.controller) !=
            AHCI_OK ||
        ahci_controller_discover_ports(
            &scan.mmio, &scan.controller, &scan.ports) != AHCI_OK) {
        return 0U;
    }

    for (port_offset = 0U; port_offset < scan.ports.port_count; ++port_offset) {
        struct production_storage_ahci *slot;
        const uint32_t port_index = scan.ports.ports[port_offset].index;

        if (!scan.ports.ports[port_offset].device_attached ||
            !scan.ports.ports[port_offset].supported ||
            production_storage.candidate_count >=
                APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY ||
            production_storage.ahci_count >=
                PRODUCTION_STORAGE_BACKEND_CAPACITY) {
            continue;
        }
        slot = &production_storage.ports[production_storage.ahci_count];
        memset(slot, 0, sizeof(*slot));
        slot->location = header->location;
        slot->controller = scan.controller;
        slot->ports = scan.ports;
        slot->mmio_base = scan.mmio_base;
        slot->mmio_length = scan.mmio_length;
        slot->mmio.context = slot;
        slot->mmio.read32 = production_storage_mmio_read32;
        slot->mmio.write32 = production_storage_mmio_write32;
        slot->dma_phys = pmm_alloc_contiguous_pages(
            PRODUCTION_STORAGE_AHCI_DMA_PAGES);
        if (slot->dma_phys == 0U) {
            continue;
        }
        slot->dma_virtual = (unsigned char *)(uintptr_t)(
            slot->dma_phys + production_storage.hhdm_offset);
        slot->dma.context = slot;
        slot->dma.read = production_storage_dma_read;
        slot->dma.write = production_storage_dma_write;

        if (ahci_port_dma_start(
                &slot->mmio, &slot->controller, &slot->ports, port_index,
                slot->dma_phys + PRODUCTION_STORAGE_COMMAND_LIST_OFFSET,
                slot->dma_phys + PRODUCTION_STORAGE_FIS_OFFSET) !=
                AHCI_OK ||
            ahci_ata_identify(
                &slot->mmio, &slot->dma, &slot->controller, &slot->ports,
                port_index,
                slot->dma_phys + PRODUCTION_STORAGE_COMMAND_LIST_OFFSET,
                slot->dma_phys + PRODUCTION_STORAGE_COMMAND_TABLE_OFFSET,
                slot->dma_phys + PRODUCTION_STORAGE_DATA_OFFSET,
                &slot->identity) != AHCI_OK ||
            ahci_block_device_bind(
                &slot->mmio, &slot->dma, &slot->controller, &slot->ports,
                port_index,
                slot->dma_phys + PRODUCTION_STORAGE_COMMAND_LIST_OFFSET,
                slot->dma_phys + PRODUCTION_STORAGE_COMMAND_TABLE_OFFSET,
                slot->dma_phys + PRODUCTION_STORAGE_DATA_OFFSET,
                &slot->identity, &slot->backend) != AHCI_OK) {
            (void)pmm_free_contiguous_pages(
                slot->dma_phys, PRODUCTION_STORAGE_AHCI_DMA_PAGES);
            slot->dma_phys = 0U;
            slot->dma_virtual = NULL;
            continue;
        }

        slot->used = true;
        production_storage.candidates[
            production_storage.candidate_count].kind =
                APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI;
        production_storage.candidates[
            production_storage.candidate_count].identity =
                production_storage_identity(&header->location, port_index);
        production_storage.candidates[
            production_storage.candidate_count].device =
                slot->backend.device;
        production_storage_copy_text(
            production_storage.identities[
                production_storage.candidate_count],
            APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH,
            slot->identity.serial[0] != '\0' ? slot->identity.serial
                                             : "ATA-SERIAL-UNAVAILABLE");
        production_storage_copy_text(
            production_storage.models[production_storage.candidate_count],
            APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH,
            slot->identity.model);
        ++production_storage.candidate_count;
        ++production_storage.ahci_count;
        ++added;
    }
    return added;
}

static bool production_storage_nvme_mmio_read32(
    void *context,
    uint64_t offset,
    uint32_t *value)
{
    const struct production_storage_nvme *controller =
        (const struct production_storage_nvme *)context;

    if (controller == NULL || value == NULL ||
        offset > controller->mmio_length ||
        controller->mmio_length - offset < 4U) {
        return false;
    }
    *value = *(volatile uint32_t *)(uintptr_t)(
        (controller->mmio_base + production_storage.hhdm_offset) + offset);
    return true;
}

static bool production_storage_nvme_mmio_write32(
    void *context,
    uint64_t offset,
    uint32_t value)
{
    const struct production_storage_nvme *controller =
        (const struct production_storage_nvme *)context;

    if (controller == NULL ||
        offset > controller->mmio_length ||
        controller->mmio_length - offset < 4U) {
        return false;
    }
    *(volatile uint32_t *)(uintptr_t)(
        (controller->mmio_base + production_storage.hhdm_offset) + offset) =
        value;
    return true;
}

/*
 * ITEM-429: the wait the NVMe bring-up asks for.
 *
 * The controller is allowed up to 500 ms by the specification to move
 * CSTS.RDY, and the 2026-09-16 hardware run's NVMe probe ended in
 * `NVME_TIMEOUT` on the acceptance host while QEMU answers instantly: the
 * driver waited on a loop count, which is microseconds.  The wait is now the
 * driver's own bounded millisecond loop (nvme.c) and this is its clock.
 *
 * `timer_delay_ms()` owns PIT channel 0 for the wait and hands it back: the
 * probe runs after `timer_init()` but before `cpu_sti()` (no tick fires yet,
 * so the wait must read a hardware counter), and the channel it borrows is the
 * one every clock in the machine is derived from.  A controller that answers
 * immediately never calls it at all.
 */
static void production_storage_nvme_delay_ms(
    void *context,
    uint32_t milliseconds)
{
    (void)context;
    timer_delay_ms(milliseconds);
}

static bool production_storage_nvme_dma_read(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length)
{
    const struct production_storage_nvme *controller =
        (const struct production_storage_nvme *)context;
    uint64_t relative;

    if (controller == NULL || buffer == NULL || length == 0U ||
        address < controller->dma_phys) {
        return false;
    }
    relative = address - controller->dma_phys;
    if (relative >= PRODUCTION_STORAGE_NVME_DMA_BYTES ||
        length > PRODUCTION_STORAGE_NVME_DMA_BYTES - (size_t)relative) {
        return false;
    }
    memcpy(buffer, controller->dma_virtual + (size_t)relative, length);
    return true;
}

static bool production_storage_nvme_dma_write(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    const struct production_storage_nvme *controller =
        (const struct production_storage_nvme *)context;
    uint64_t relative;

    if (controller == NULL || buffer == NULL || length == 0U ||
        address < controller->dma_phys) {
        return false;
    }
    relative = address - controller->dma_phys;
    if (relative >= PRODUCTION_STORAGE_NVME_DMA_BYTES ||
        length > PRODUCTION_STORAGE_NVME_DMA_BYTES - (size_t)relative) {
        return false;
    }
    memcpy(controller->dma_virtual + (size_t)relative, buffer, length);
    return true;
}

static bool production_storage_nvme_init(
    struct production_storage_nvme *slot,
    const struct pci_device_header *header)
{
    enum nvme_status status;

    /*
     * ITEM-429: this controller's attempt starts here.  The evidence fields
     * are per-attempt, not per-boot: a controller that fails to bind must not
     * publish the previous controller's step, wait or status, and a success
     * must not leave an earlier failure's status behind.
     */
    production_storage.nvme_step = "BIND";
    production_storage.nvme_status = (int)NVME_OK;
    production_storage.nvme_wait_ms = 0U;
    if (slot == NULL || header == NULL) {
        return false;
    }
    {
        /*
         * The bind's own verdict is the status: a function the driver does not
         * recognise or whose memory BAR is missing must not be published as a
         * bring-up that succeeded.
         */
        const enum nvme_status bind_status =
            nvme_controller_bind(header, &slot->controller);

        if (bind_status != NVME_OK) {
            production_storage.nvme_status = (int)bind_status;
            return false;
        }
    }
    production_storage.nvme_step = "MAP";
    production_storage_enable_pci_device(&header->location);
    slot->mmio_base = slot->controller.mmio_base;
    slot->mmio_length = header->bars[NVME_MEMORY_BAR_INDEX].length;
    if (!production_storage_map_mmio(
            slot->mmio_base, slot->mmio_length)) {
        /* The register window could not be reached; the step names which. */
        production_storage.nvme_status = (int)NVME_IO_ERROR;
        return false;
    }
    slot->mmio.context = slot;
    slot->mmio.read32 = production_storage_nvme_mmio_read32;
    slot->mmio.write32 = production_storage_nvme_mmio_write32;
    /* ITEM-429: the wait the controller-ready transitions are given. */
    slot->mmio.delay_ms = production_storage_nvme_delay_ms;

    production_storage.nvme_step = "DMA";
    slot->dma_phys = pmm_alloc_contiguous_pages(
        PRODUCTION_STORAGE_NVME_DMA_PAGES);
    if (slot->dma_phys == 0U) {
        production_storage.nvme_status = (int)NVME_IO_ERROR;
        return false;
    }
    slot->dma_virtual = (unsigned char *)(uintptr_t)(
        slot->dma_phys + production_storage.hhdm_offset);
    slot->dma.context = slot;
    slot->dma.read = production_storage_nvme_dma_read;
    slot->dma.write = production_storage_nvme_dma_write;

    /*
     * ITEM-429: every step is named before it runs, so the record's
     * `nvme_step=` is the call that stopped, not a guess from the status.
     */
    production_storage.nvme_step = "RESET";
    status = nvme_controller_reset(&slot->mmio, &slot->controller);
    if (status == NVME_OK) {
        production_storage.nvme_step = "QUEUE";
        status = nvme_queue_bind(
            &slot->admin_queue, NVME_ADMIN_QUEUE_ID,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_ADMIN_SQ_OFFSET,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_ADMIN_CQ_OFFSET,
            PRODUCTION_STORAGE_NVME_QUEUE_CAPACITY);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "ADMIN_QUEUE";
        status = nvme_admin_queue_configure(
            &slot->mmio, &slot->admin_queue);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "ENABLE";
        status = nvme_controller_enable(
            &slot->mmio, &slot->controller);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "IDENTIFY";
        status = nvme_identify_controller(
            &slot->mmio, &slot->dma, &slot->admin_queue,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_IDENTIFY_OFFSET,
            &slot->identity);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "NAMESPACE";
        status = nvme_namespace_discover(
            &slot->mmio, &slot->dma, &slot->admin_queue,
            PRODUCTION_STORAGE_NVME_NAMESPACE_ID,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_IDENTIFY_OFFSET,
            &slot->namespace_info);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "IO_QUEUE";
        status = nvme_queue_bind(
            &slot->io_queue, 1U,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_IO_SQ_OFFSET,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_IO_CQ_OFFSET,
            PRODUCTION_STORAGE_NVME_QUEUE_CAPACITY);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "IO_QUEUE_CONFIGURE";
        status = nvme_io_queue_configure(
            &slot->mmio, &slot->dma, &slot->admin_queue,
            &slot->io_queue);
    }
    if (status == NVME_OK) {
        production_storage.nvme_step = "BLOCK_DEVICE";
        status = nvme_namespace_bind_block_device(
            &slot->mmio, &slot->dma, &slot->io_queue,
            &slot->namespace_info,
            slot->dma_phys + PRODUCTION_STORAGE_NVME_DATA_OFFSET,
            &slot->backend);
    }
    /*
     * ITEM-429: what the ready transition cost, whatever the verdict - the
     * wait that ran last is on the controller even when a later step failed.
     */
    production_storage.nvme_wait_ms = slot->controller.ready_wait_ms;
    if (status != NVME_OK) {
        /* ITEM-423: the step, not just the failure. */
        production_storage.nvme_status = (int)status;
        (void)pmm_free_contiguous_pages(
            slot->dma_phys, PRODUCTION_STORAGE_NVME_DMA_PAGES);
        slot->dma_phys = 0U;
        slot->dma_virtual = NULL;
        return false;
    }
    production_storage.nvme_step = "READY";
    slot->used = true;
    return true;
}

static size_t production_storage_add_nvme_candidates(
    const struct pci_device_header *header)
{
    struct production_storage_nvme *slot;

    if (header == NULL ||
        production_storage.candidate_count >=
            APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY ||
        production_storage.nvme_count >=
            PRODUCTION_STORAGE_BACKEND_CAPACITY) {
        return 0U;
    }
    slot = &production_storage.namespaces[
        production_storage.nvme_count];
    memset(slot, 0, sizeof(*slot));
    if (!production_storage_nvme_init(slot, header)) {
        return 0U;
    }

    production_storage.candidates[
        production_storage.candidate_count].kind =
            APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME;
    production_storage.candidates[
        production_storage.candidate_count].identity =
            ((uint64_t)header->location.bus << 16U) |
            ((uint64_t)header->location.device << 8U) |
            header->location.function;
    production_storage.candidates[
        production_storage.candidate_count].device =
            slot->backend.device;
    production_storage_nvme_identity(
        &header->location,
        production_storage.identities[production_storage.candidate_count],
        APPLIANCE_PRODUCTION_STORAGE_IDENTITY_LENGTH);
    production_storage_copy_text(
        production_storage.models[production_storage.candidate_count],
        APPLIANCE_PRODUCTION_STORAGE_MODEL_LENGTH,
        "NVME-CONTROLLER");
    ++production_storage.candidate_count;
    ++production_storage.nvme_count;
    return 1U;
}

void appliance_production_storage_boot_init(uint64_t hhdm_offset)
{
    memset(&production_storage, 0, sizeof(production_storage));
    production_storage.initialized = true;
    production_storage.hhdm_offset = hhdm_offset;
}

/*
 * ITEM-540: record what the search stopped on, so the evidence and the refusal
 * page carry the primary error instead of folding every failure into the same
 * "not probed".  `failure` is NULL when the stop was a capacity verdict rather
 * than a device.
 */
static void production_storage_note_probe_failure(
    const char *stage,
    enum pci_status status,
    const struct pci_scan_failure *failure)
{
    production_storage.probe_attempted = true;
    production_storage.probe_stage = stage;
    production_storage.probe_status = status;
    if (failure != NULL) {
        production_storage.probe_failure = *failure;
    } else {
        memset(&production_storage.probe_failure, 0,
               sizeof(production_storage.probe_failure));
        production_storage.probe_failure.status = status;
        production_storage.probe_failure.bar_index = PCI_BAR_INDEX_NONE;
    }
}

enum appliance_production_boot_probe_status
appliance_production_storage_probe(
    void *context,
    struct appliance_production_boot_backend *backends,
    size_t capacity,
    size_t *count)
{
    struct pci_config_io io;
    struct pci_device_header headers[PRODUCTION_STORAGE_PCI_HEADER_CAPACITY];
    struct pci_scan_failure scan_failure;
    size_t header_count = 0U;
    size_t index;
    enum pci_status pci_status;

    (void)context;
    if (!production_storage.initialized || backends == NULL ||
        count == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_PROBE_IO_ERROR;
    }
    if (capacity < APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY) {
        return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
    }
    if (production_storage.probed) {
        for (index = 0U;
             index < production_storage.candidate_count; ++index) {
            backends[index] = production_storage.candidates[index];
        }
        *count = production_storage.candidate_count;
        return APPLIANCE_PRODUCTION_BOOT_PROBE_OK;
    }

    production_storage.probe_attempted = true;
    production_storage.probe_stage = "AHCI_SCAN";
    production_storage.probe_status = PCI_OK;
    memset(&io, 0, sizeof(io));
    io.read32 = production_storage_pci_read;
    io.write32 = production_storage_pci_write;
    /*
     * ITEM-543: the controller's ABAR at BAR5 is what AHCI is brought up
     * through, so that is the BAR the scan has to decode.  The L875's legacy
     * I/O BAR0 explains no size and must not hide the controller; a BAR5 that
     * does not decode is still the named refusal of this device.
     */
    pci_status = pci_scan_class_required(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_SATA,
        PCI_PROGIF_AHCI, PCI_BAR_REQUIRED(AHCI_MEMORY_BAR_INDEX), headers,
        PRODUCTION_STORAGE_PCI_HEADER_CAPACITY, &header_count,
        &scan_failure);
    if (pci_status == PCI_CAPACITY) {
        production_storage_note_probe_failure(
            "AHCI_SCAN", pci_status, NULL);
        return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
    }
    if (pci_status != PCI_OK) {
        production_storage_note_probe_failure(
            "AHCI_SCAN", pci_status, &scan_failure);
        return APPLIANCE_PRODUCTION_BOOT_PROBE_IO_ERROR;
    }

    /* ITEM-423: what the class scan matched, before anything is brought up. */
    production_storage.pci_ahci_count = header_count;
    for (index = 0U; index < header_count; ++index) {
        if (production_storage.candidate_count >=
            APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY) {
            return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
        }
        (void)production_storage_add_ahci_candidates(&headers[index]);
    }

    header_count = 0U;
    production_storage.probe_stage = "NVME_SCAN";
    /* ITEM-543: NVMe is brought up through BAR0, so BAR0 is what must decode. */
    pci_status = pci_scan_class_required(
        &io, PCI_CLASS_MASS_STORAGE, PCI_SUBCLASS_NVME,
        PCI_PROGIF_NVME, PCI_BAR_REQUIRED(NVME_MEMORY_BAR_INDEX), headers,
        PRODUCTION_STORAGE_PCI_HEADER_CAPACITY, &header_count,
        &scan_failure);
    if (pci_status == PCI_CAPACITY) {
        production_storage_note_probe_failure(
            "NVME_SCAN", pci_status, NULL);
        return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
    }
    if (pci_status != PCI_OK) {
        production_storage_note_probe_failure(
            "NVME_SCAN", pci_status, &scan_failure);
        return APPLIANCE_PRODUCTION_BOOT_PROBE_IO_ERROR;
    }

    production_storage.probe_stage = "READY";
    production_storage.probe_status = PCI_OK;
    production_storage.pci_nvme_count = header_count;
    for (index = 0U; index < header_count; ++index) {
        if (production_storage.candidate_count >=
            APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY) {
            return APPLIANCE_PRODUCTION_BOOT_PROBE_CAPACITY;
        }
        (void)production_storage_add_nvme_candidates(&headers[index]);
    }

    production_storage.probed = true;
    for (index = 0U; index < production_storage.candidate_count; ++index) {
        backends[index] = production_storage.candidates[index];
    }
    *count = production_storage.candidate_count;
    return APPLIANCE_PRODUCTION_BOOT_PROBE_OK;
}

/*
 * ITEM-394: bounded publication of the probed disks.  The list is only
 * available after a successful probe; an unprobed or failed probe reports no
 * disks instead of inventing one, so the installer refuses rather than
 * selecting a device it never identified.
 */
void appliance_production_storage_evidence(
    struct appliance_production_storage_evidence *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->pci_ahci = production_storage.pci_ahci_count;
    out->pci_nvme = production_storage.pci_nvme_count;
    out->ahci = production_storage.ahci_count;
    out->nvme = production_storage.nvme_count;
    out->nvme_status = production_storage.nvme_status;
    /* ITEM-429: the step and the wait that go with the status. */
    out->nvme_step = production_storage.nvme_step != NULL
                         ? production_storage.nvme_step
                         : "NONE";
    out->nvme_wait_ms = production_storage.nvme_wait_ms;
    /* ITEM-540: the probe's own verdict, not only the disks it kept. */
    out->probe_attempted = production_storage.probe_attempted;
    out->probe_stage = production_storage.probe_stage != NULL
                           ? production_storage.probe_stage
                           : "NONE";
    out->probe_status = production_storage.probe_status;
    out->probe_failure = production_storage.probe_failure;
}

/*
 * ITEM-518: turn the evidence the probe already keeps into the operator report.
 *
 * Every disk the probe brought up gets a row, and the row names the one reason
 * it cannot be used when it cannot: an identity the plan could never write by,
 * no geometry, a block size the installer does not read, or a block device that
 * does not validate.  A machine the probe never reached publishes `probed=0`,
 * so the caller prints "not probed" instead of a zero that looks like a finding.
 */
void appliance_production_storage_report(struct appliance_storage_report *out)
{
    struct appliance_production_storage_evidence evidence;
    size_t index;

    if (out == NULL) {
        return;
    }
    appliance_storage_report_init(out);
    if (!production_storage.initialized) {
        return;
    }
    appliance_production_storage_evidence(&evidence);
    out->class_ahci = evidence.pci_ahci;
    out->class_nvme = evidence.pci_nvme;
    out->brought_up = evidence.ahci + evidence.nvme;
    out->nvme_status = evidence.nvme_status;
    out->nvme_step = evidence.nvme_step;
    out->nvme_wait_ms = evidence.nvme_wait_ms;
    /*
     * ITEM-540: a probe that ran and failed keeps its primary error in the
     * report even though it brought no disk up.
     */
    out->probe_attempted = evidence.probe_attempted;
    out->probe_stage = evidence.probe_stage;
    out->probe_status = pci_status_string(evidence.probe_status);
    if (evidence.probe_attempted && evidence.probe_status != PCI_OK) {
        out->failure_known = evidence.probe_failure.status != PCI_OK;
        out->failure_bus = evidence.probe_failure.location.bus;
        out->failure_device = evidence.probe_failure.location.device;
        out->failure_function = evidence.probe_failure.location.function;
        out->failure_bar = evidence.probe_failure.bar_index;
        out->failure_vendor_id = evidence.probe_failure.vendor_id;
        out->failure_device_id = evidence.probe_failure.device_id;
    }
    if (!production_storage.probed) {
        return;
    }
    out->probed = true;
    for (index = 0U;
         index < production_storage.candidate_count &&
         index < APPLIANCE_STORAGE_REPORT_DISK_CAPACITY;
         ++index) {
        struct appliance_production_storage_disk disk;
        struct appliance_storage_report_disk *row = &out->disks[index];
        const char *controller = appliance_production_boot_backend_kind_string(
            production_storage.candidates[index].kind);

        row->present = true;
        row->controller = controller != NULL ? controller : "UNKNOWN";
        if (!appliance_production_storage_disk_info(index, &disk) ||
            !disk.present || disk.device == NULL) {
            row->reason = APPLIANCE_STORAGE_REPORT_INVALID_DEVICE;
            ++out->disk_count;
            continue;
        }
        production_storage_copy_text(row->identity, sizeof(row->identity),
                                     disk.identity);
        production_storage_copy_text(row->model, sizeof(row->model),
                                     disk.model);
        row->block_size = disk.device->block_size;
        row->block_count = disk.device->block_count;
        if (row->identity[0] == '\0') {
            row->reason = APPLIANCE_STORAGE_REPORT_EMPTY_IDENTITY;
        } else if (row->block_count == 0U) {
            row->reason = APPLIANCE_STORAGE_REPORT_ZERO_GEOMETRY;
        } else if (row->block_size != BLOCK_DEVICE_BLOCK_SIZE_MIN) {
            row->reason = APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE;
        } else {
            row->reason = APPLIANCE_STORAGE_REPORT_OK;
        }
        if (appliance_storage_report_reason_usable(row->reason)) {
            ++out->accepted;
        }
        ++out->disk_count;
    }
}

size_t appliance_production_storage_disk_count(void)
{
    if (!production_storage.initialized || !production_storage.probed) {
        return 0U;
    }
    return production_storage.candidate_count;
}

bool appliance_production_storage_disk_info(
    size_t index,
    struct appliance_production_storage_disk *info)
{
    if (info == NULL) {
        return false;
    }
    memset(info, 0, sizeof(*info));
    if (!production_storage.initialized || !production_storage.probed ||
        index >= production_storage.candidate_count) {
        return false;
    }
    info->present = true;
    info->kind = production_storage.candidates[index].kind;
    info->device = &production_storage.candidates[index].device;
    production_storage_copy_text(
        info->identity, sizeof(info->identity),
        production_storage.identities[index]);
    production_storage_copy_text(
        info->model, sizeof(info->model),
        production_storage.models[index]);
    return true;
}
