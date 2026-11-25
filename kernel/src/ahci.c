#include "ahci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum ahci_status ahci_read32(
    const struct ahci_mmio_io *io,
    uint64_t offset,
    uint32_t *value)
{
    if (io == NULL || io->read32 == NULL || value == NULL) {
        return AHCI_NULL;
    }
    if ((offset % 4U) != 0U || offset > UINT64_MAX - 4U) {
        return AHCI_BAD_STATE;
    }
    if (!io->read32(io->context, offset, value)) {
        return AHCI_IO_ERROR;
    }
    return AHCI_OK;
}

static enum ahci_status ahci_write32(
    const struct ahci_mmio_io *io,
    uint64_t offset,
    uint32_t value)
{
    if (io == NULL || io->write32 == NULL) {
        return AHCI_NULL;
    }
    if ((offset % 4U) != 0U || offset > UINT64_MAX - 4U) {
        return AHCI_BAD_STATE;
    }
    if (!io->write32(io->context, offset, value)) {
        return AHCI_IO_ERROR;
    }
    return AHCI_OK;
}

static uint32_t ahci_port_count_from_capabilities(uint32_t capabilities)
{
    return (capabilities & AHCI_CAP_NP_MASK) + 1U;
}

static uint64_t ahci_port_register_offset(
    uint32_t port_index,
    uint64_t register_offset)
{
    return AHCI_PORT_BASE_OFFSET +
           (uint64_t)port_index * AHCI_PORT_REGISTER_SIZE +
           register_offset;
}

static enum ahci_status ahci_port_read32(
    const struct ahci_mmio_io *io,
    uint32_t port_index,
    uint64_t register_offset,
    uint32_t *value)
{
    if (port_index >= AHCI_MAX_PORT_COUNT ||
        register_offset + 4U > AHCI_PORT_REGISTER_SIZE) {
        return AHCI_BAD_STATE;
    }
    return ahci_read32(
        io, ahci_port_register_offset(port_index, register_offset), value);
}

static enum ahci_status ahci_port_write32(
    const struct ahci_mmio_io *io,
    uint32_t port_index,
    uint64_t register_offset,
    uint32_t value)
{
    if (port_index >= AHCI_MAX_PORT_COUNT ||
        register_offset + 4U > AHCI_PORT_REGISTER_SIZE) {
        return AHCI_BAD_STATE;
    }
    return ahci_write32(
        io, ahci_port_register_offset(port_index, register_offset), value);
}

static const struct ahci_port_state *ahci_port_find(
    const struct ahci_port_map *map,
    uint32_t port_index)
{
    size_t index;

    for (index = 0U; index < map->port_count; ++index) {
        if (map->ports[index].present &&
            map->ports[index].index == port_index) {
            return &map->ports[index];
        }
    }
    return NULL;
}

enum ahci_status ahci_controller_bind(
    const struct pci_device_header *header,
    struct ahci_controller *controller)
{
    const struct pci_bar *bar;

    if (header == NULL || controller == NULL) {
        return AHCI_NULL;
    }
    if (!header->present ||
        header->base_class != PCI_CLASS_MASS_STORAGE ||
        header->subclass != PCI_SUBCLASS_SATA ||
        header->prog_if != PCI_PROGIF_AHCI) {
        return AHCI_UNSUPPORTED_DEVICE;
    }

    bar = &header->bars[AHCI_MEMORY_BAR_INDEX];
    if (!bar->present || bar->io_space || bar->is_64bit) {
        return AHCI_MISSING_MEMORY_BAR;
    }
    if (bar->base == 0U || bar->length < AHCI_MIN_MMIO_LENGTH ||
        bar->base % PCI_BAR_ALIGNMENT != 0U) {
        return AHCI_BAD_BAR;
    }

    memset(controller, 0, sizeof(*controller));
    controller->present = true;
    controller->location = header->location;
    controller->vendor_id = header->vendor_id;
    controller->device_id = header->device_id;
    controller->mmio_base = bar->base;
    return AHCI_OK;
}

enum ahci_status ahci_controller_discover(
    struct ahci_mmio_io *io,
    struct ahci_controller *controller)
{
    enum ahci_status status;
    uint32_t capabilities;
    uint32_t global_host_control;
    uint32_t interrupt_status;
    uint32_t ports_implemented;
    uint32_t version;
    uint32_t port_count;
    uint32_t valid_pi;
    uint32_t index;

    if (io == NULL || controller == NULL) {
        return AHCI_NULL;
    }
    if (!controller->present || controller->mmio_base == 0U) {
        return AHCI_BAD_STATE;
    }

    status = ahci_read32(
        io, AHCI_CAP_OFFSET, &capabilities);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_read32(
        io, AHCI_GHC_OFFSET, &global_host_control);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_read32(
        io, AHCI_IS_OFFSET, &interrupt_status);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_read32(
        io, AHCI_PI_OFFSET, &ports_implemented);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_read32(
        io, AHCI_VS_OFFSET, &version);
    if (status != AHCI_OK) {
        return status;
    }

    if (version < 0x00010000U) {
        return AHCI_UNSUPPORTED_VERSION;
    }
    port_count = ahci_port_count_from_capabilities(capabilities);
    if (port_count == 0U || port_count > AHCI_MAX_PORT_COUNT) {
        return AHCI_BAD_STATE;
    }
    valid_pi = 0U;
    for (index = 0U; index < port_count; ++index) {
        valid_pi |= UINT32_C(1) << index;
    }
    if (ports_implemented == 0U ||
        (ports_implemented & ~valid_pi) != 0U ||
        (ports_implemented & valid_pi) == 0U) {
        return AHCI_BAD_STATE;
    }

    controller->capabilities = capabilities;
    controller->global_host_control = global_host_control;
    controller->interrupt_status = interrupt_status;
    controller->ports_implemented = ports_implemented;
    controller->version = version;
    return AHCI_OK;
}

enum ahci_status ahci_controller_activate(
    struct ahci_mmio_io *io,
    struct ahci_controller *controller)
{
    enum ahci_status status;
    uint32_t ghc;
    uint32_t enabled_ghc;

    if (io == NULL || controller == NULL) {
        return AHCI_NULL;
    }
    if (!controller->present || controller->mmio_base == 0U) {
        return AHCI_BAD_STATE;
    }

    status = ahci_read32(io, AHCI_GHC_OFFSET, &ghc);
    if (status != AHCI_OK) {
        return status;
    }
    enabled_ghc = ghc | (UINT32_C(1) << AHCI_GHC_AE_BIT);
    if (ghc != enabled_ghc) {
        status = ahci_write32(io, AHCI_GHC_OFFSET, enabled_ghc);
        if (status != AHCI_OK) {
            return status;
        }
        status = ahci_read32(io, AHCI_GHC_OFFSET, &ghc);
        if (status != AHCI_OK) {
            return status;
        }
    }
    if ((ghc & (UINT32_C(1) << AHCI_GHC_AE_BIT)) == 0U) {
        return AHCI_BAD_STATE;
    }

    controller->global_host_control = ghc;
    controller->ahci_enabled = true;
    return AHCI_OK;
}

enum ahci_status ahci_controller_discover_ports(
    struct ahci_mmio_io *io,
    const struct ahci_controller *controller,
    struct ahci_port_map *map)
{
    enum ahci_status status;
    uint32_t port_count;
    uint32_t port_index;
    size_t found = 0U;

    if (io == NULL || controller == NULL || map == NULL) {
        return AHCI_NULL;
    }
    if (!controller->present || !controller->ahci_enabled ||
        controller->mmio_base == 0U) {
        return AHCI_BAD_STATE;
    }

    memset(map, 0, sizeof(*map));
    port_count = ahci_port_count_from_capabilities(
        controller->capabilities);
    if (port_count == 0U || port_count > AHCI_MAX_PORT_COUNT) {
        return AHCI_BAD_STATE;
    }

    for (port_index = 0U; port_index < port_count; ++port_index) {
        struct ahci_port_state *port;

        if ((controller->ports_implemented &
             (UINT32_C(1) << port_index)) == 0U) {
            continue;
        }
        port = &map->ports[found];
        port->present = true;
        port->index = port_index;

        status = ahci_port_read32(
            io, port_index, AHCI_PORT_CMD_OFFSET, &port->command);
        if (status != AHCI_OK) {
            return status;
        }
        status = ahci_port_read32(
            io, port_index, AHCI_PORT_SIG_OFFSET, &port->signature);
        if (status != AHCI_OK) {
            return status;
        }
        status = ahci_port_read32(
            io, port_index, AHCI_PORT_SSTS_OFFSET, &port->status);
        if (status != AHCI_OK) {
            return status;
        }

        if (port->signature != AHCI_PORT_SIG_NO_DEVICE &&
            (port->status & AHCI_PORT_SSTS_DET_MASK) ==
                AHCI_PORT_SSTS_DET_PRESENT) {
            port->device_attached = true;
            port->supported = port->signature == AHCI_PORT_SIG_ATA;
        }
        ++found;
    }
    if (found == 0U) {
        return AHCI_BAD_STATE;
    }

    map->port_count = found;
    map->complete = true;
    return AHCI_OK;
}

enum ahci_status ahci_port_dma_start(
    struct ahci_mmio_io *io,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t fis_phys)
{
    const struct ahci_port_state *port;
    enum ahci_status status;
    uint32_t command;
    uint32_t start_mask;
    uint32_t enabled_command;

    if (io == NULL || controller == NULL || map == NULL) {
        return AHCI_NULL;
    }
    if (!controller->present || !controller->ahci_enabled ||
        controller->mmio_base == 0U || !map->complete) {
        return AHCI_BAD_STATE;
    }
    if (command_list_phys == 0U ||
        command_list_phys % AHCI_COMMAND_LIST_ALIGNMENT != 0U ||
        fis_phys == 0U ||
        fis_phys % AHCI_FIS_ALIGNMENT != 0U) {
        return AHCI_BAD_STATE;
    }

    port = ahci_port_find(map, port_index);
    if (port == NULL || !port->present ||
        !port->device_attached || !port->supported) {
        return AHCI_BAD_STATE;
    }

    status = ahci_port_read32(
        io, port_index, AHCI_PORT_CMD_OFFSET, &command);
    if (status != AHCI_OK) {
        return status;
    }
    start_mask =
        (UINT32_C(1) << AHCI_PORT_CMD_ST_BIT) |
        (UINT32_C(1) << AHCI_PORT_CMD_FRE_BIT);
    command &= ~start_mask;
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_CMD_OFFSET, command);
    if (status != AHCI_OK) {
        return status;
    }

    status = ahci_port_write32(
        io, port_index, AHCI_PORT_CLB_OFFSET,
        (uint32_t)command_list_phys);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_CLBU_OFFSET,
        (uint32_t)(command_list_phys >> 32U));
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_FB_OFFSET,
        (uint32_t)fis_phys);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_FBU_OFFSET,
        (uint32_t)(fis_phys >> 32U));
    if (status != AHCI_OK) {
        return status;
    }

    enabled_command = command | start_mask;
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_CMD_OFFSET, enabled_command);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_port_read32(
        io, port_index, AHCI_PORT_CMD_OFFSET, &command);
    if (status != AHCI_OK) {
        return status;
    }
    if ((command & start_mask) != start_mask) {
        return AHCI_BAD_STATE;
    }
    return AHCI_OK;
}

static uint32_t ahci_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t ahci_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static void ahci_store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void ahci_store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static enum ahci_status ahci_memory_read(
    const struct ahci_dma_io *memory,
    uint64_t address,
    void *buffer,
    size_t length)
{
    if (memory == NULL || memory->read == NULL ||
        buffer == NULL) {
        return AHCI_NULL;
    }
    if (length == 0U || address > UINT64_MAX - (uint64_t)length) {
        return AHCI_BAD_STATE;
    }
    if (!memory->read(memory->context, address, buffer, length)) {
        return AHCI_IO_ERROR;
    }
    return AHCI_OK;
}

static enum ahci_status ahci_memory_write(
    const struct ahci_dma_io *memory,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    if (memory == NULL || memory->write == NULL ||
        buffer == NULL) {
        return AHCI_NULL;
    }
    if (length == 0U || address > UINT64_MAX - (uint64_t)length) {
        return AHCI_BAD_STATE;
    }
    if (!memory->write(memory->context, address, buffer, length)) {
        return AHCI_IO_ERROR;
    }
    return AHCI_OK;
}

static const struct ahci_port_state *ahci_command_port_state(
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index)
{
    if (controller == NULL || map == NULL ||
        !controller->present || !controller->ahci_enabled ||
        controller->mmio_base == 0U || !map->complete) {
        return NULL;
    }
    return ahci_port_find(map, port_index);
}

static enum ahci_status ahci_ata_command_build(
    const struct ahci_dma_io *memory,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    uint8_t command,
    uint64_t lba,
    uint32_t block_count,
    size_t data_bytes,
    bool write)
{
    enum ahci_status status;
    unsigned char header[AHCI_COMMAND_LIST_ENTRY_SIZE];
    unsigned char table[
        AHCI_COMMAND_TABLE_SIZE + AHCI_PRD_ENTRY_SIZE];
    uint32_t prdtl;
    uint32_t opts;
    uint8_t device_register;
    bool identify = command == AHCI_ATA_IDENTIFY_DEVICE;
    bool flush = command == AHCI_ATA_FLUSH_CACHE_EXT;

    if (command_list_phys == 0U ||
        command_list_phys % AHCI_COMMAND_LIST_ALIGNMENT != 0U ||
        command_table_phys == 0U ||
        command_table_phys % 128U != 0U) {
        return AHCI_BAD_ALIGNMENT;
    }
    if (data_bytes != 0U &&
        (data_bytes > AHCI_MAX_TRANSFER_BYTES ||
         (data_bytes % 512U) != 0U ||
         data_phys == 0U || data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U)) {
        return data_phys == 0U || data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U
            ? AHCI_BAD_ALIGNMENT
            : AHCI_BAD_RANGE;
    }
    if (!identify && !flush &&
        command != AHCI_ATA_READ_DMA_EXT &&
        command != AHCI_ATA_WRITE_DMA_EXT) {
        return AHCI_BAD_STATE;
    }
    if (!identify && !flush &&
        (block_count == 0U ||
         block_count > AHCI_MAX_SECTORS_PER_TRANSFER ||
         data_bytes != (size_t)block_count * 512U)) {
        return AHCI_BAD_RANGE;
    }
    if (lba > UINT64_MAX - (uint64_t)block_count) {
        return AHCI_BAD_RANGE;
    }

    if (identify || flush) {
        device_register = AHCI_ATA_DEVICE_OBS;
    } else {
        device_register = AHCI_ATA_LBA_BIT;
    }
    prdtl = data_bytes == 0U ? 0U : 1U;

    memset(header, 0, sizeof(header));
    opts = AHCI_H2D_FIS_CFL;
    if (write) {
        opts |= UINT32_C(1) << AHCI_COMMAND_HEADER_WRITE_BIT;
    }
    ahci_store_le32(header, opts | (prdtl << 16U));
    ahci_store_le64(header + 8U, command_table_phys);

    memset(table, 0, sizeof(table));
    table[0U] = 0x27U;
    table[1U] = 0x80U;
    table[2U] = command;
    table[7U] = device_register;
    if (!identify && !flush) {
        table[4U] = (unsigned char)(lba & 0xFFU);
        table[5U] = (unsigned char)((lba >> 8U) & 0xFFU);
        table[6U] = (unsigned char)((lba >> 16U) & 0xFFU);
        table[8U] = (unsigned char)((lba >> 24U) & 0xFFU);
        table[9U] = (unsigned char)((lba >> 32U) & 0xFFU);
        table[10U] = (unsigned char)((lba >> 40U) & 0xFFU);
        table[12U] = (unsigned char)(block_count & 0xFFU);
        table[13U] = (unsigned char)((block_count >> 8U) & 0xFFU);
    }

    if (data_bytes != 0U) {
        unsigned char *prd =
            table + AHCI_COMMAND_TABLE_SIZE;

        ahci_store_le64(prd, data_phys);
        ahci_store_le32(
            prd + 8U, 0U);
        ahci_store_le32(
            prd + 12U, (uint32_t)(data_bytes - 1U));
    }

    status = ahci_memory_write(
        memory, command_list_phys, header, sizeof(header));
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_memory_write(
        memory, command_table_phys, table,
        AHCI_COMMAND_TABLE_SIZE +
        (data_bytes == 0U ? 0U : AHCI_PRD_ENTRY_SIZE));
    return status;
}

static enum ahci_status ahci_ata_command_execute(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    uint8_t command,
    uint64_t lba,
    uint32_t block_count,
    size_t data_bytes,
    bool write)
{
    const struct ahci_port_state *port;
    enum ahci_status status;
    uint32_t command_register;
    uint32_t start_mask;
    uint32_t issue;
    uint32_t task_file;
    uint32_t poll;

    if (io == NULL || memory == NULL) {
        return AHCI_NULL;
    }
    port = ahci_command_port_state(controller, map, port_index);
    if (port == NULL || !port->present ||
        !port->device_attached || !port->supported) {
        return AHCI_BAD_STATE;
    }

    status = ahci_port_read32(
        io, port_index, AHCI_PORT_CMD_OFFSET, &command_register);
    if (status != AHCI_OK) {
        return status;
    }
    start_mask =
        (UINT32_C(1) << AHCI_PORT_CMD_ST_BIT) |
        (UINT32_C(1) << AHCI_PORT_CMD_FRE_BIT);
    if ((command_register & start_mask) != start_mask) {
        return AHCI_BAD_STATE;
    }

    status = ahci_ata_command_build(
        memory, command_list_phys, command_table_phys, data_phys,
        command, lba, block_count, data_bytes, write);
    if (status != AHCI_OK) {
        return status;
    }
    /* Make command-list/table writes visible before the HBA reads them. */
    __asm__ volatile ("sfence" : : : "memory");

    status = ahci_port_write32(
        io, port_index, AHCI_PORT_IS_OFFSET, UINT32_MAX);
    if (status != AHCI_OK) {
        return status;
    }
    issue = AHCI_COMMAND_SLOT_MASK;
    status = ahci_port_write32(
        io, port_index, AHCI_PORT_CI_OFFSET, issue);
    if (status != AHCI_OK) {
        return status;
    }

    for (poll = 0U; poll < AHCI_COMMAND_POLLS; ++poll) {
        status = ahci_port_read32(
            io, port_index, AHCI_PORT_CI_OFFSET, &issue);
        if (status != AHCI_OK) {
            return status;
        }
        if ((issue & AHCI_COMMAND_SLOT_MASK) == 0U) {
            break;
        }
    }
    if (poll == AHCI_COMMAND_POLLS) {
        return AHCI_TIMEOUT;
    }

    status = ahci_port_read32(
        io, port_index, AHCI_PORT_TFD_OFFSET, &task_file);
    if (status != AHCI_OK) {
        return status;
    }
    if ((task_file &
         (AHCI_TFD_ERR_BIT | AHCI_TFD_BSY_BIT |
          AHCI_TFD_DRQ_BIT)) != 0U) {
        return AHCI_FAILED_COMPLETION;
    }
    return AHCI_OK;
}

enum ahci_status ahci_ata_identify(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    struct ahci_ata_identity *identity)
{
    unsigned char identify[AHCI_ATA_IDENTIFY_SIZE];
    enum ahci_status status;

    if (identity == NULL) {
        return AHCI_NULL;
    }
    status = ahci_ata_command_execute(
        io, memory, controller, map, port_index,
        command_list_phys, command_table_phys, data_phys,
        AHCI_ATA_IDENTIFY_DEVICE, 0U, 0U,
        AHCI_ATA_IDENTIFY_SIZE, false);
    if (status != AHCI_OK) {
        return status;
    }
    status = ahci_memory_read(
        memory, data_phys, identify, sizeof(identify));
    if (status != AHCI_OK) {
        return status;
    }
    return ahci_ata_identity_parse(identify, identity);
}

enum ahci_status ahci_ata_read_blocks(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    uint64_t lba,
    uint32_t block_count)
{
    return ahci_ata_command_execute(
        io, memory, controller, map, port_index,
        command_list_phys, command_table_phys, data_phys,
        AHCI_ATA_READ_DMA_EXT, lba, block_count,
        (size_t)block_count * 512U, false);
}

enum ahci_status ahci_ata_write_blocks(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    uint64_t lba,
    uint32_t block_count)
{
    return ahci_ata_command_execute(
        io, memory, controller, map, port_index,
        command_list_phys, command_table_phys, data_phys,
        AHCI_ATA_WRITE_DMA_EXT, lba, block_count,
        (size_t)block_count * 512U, true);
}

enum ahci_status ahci_ata_flush_cache(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys)
{
    return ahci_ata_command_execute(
        io, memory, controller, map, port_index,
        command_list_phys, command_table_phys, 0U,
        AHCI_ATA_FLUSH_CACHE_EXT, 0U, 0U, 0U, false);
}

/*
 * ITEM-394: ATA strings are stored as byte-swapped word pairs and padded with
 * spaces.  Decode them into a bounded, printable, trimmed C string so the
 * installer can publish the disk it selected and refuse any other.
 */
static void ahci_ata_string(
    const unsigned char *data,
    size_t word_offset,
    size_t word_count,
    char *destination,
    size_t capacity)
{
    size_t written = 0U;
    size_t word;

    if (destination == NULL || capacity == 0U) {
        return;
    }
    for (word = 0U; word < word_count; ++word) {
        const size_t offset = word_offset + word * 2U;
        unsigned char high = data[offset + 1U];
        unsigned char low = data[offset];

        if (high == 0U) {
            high = ' ';
        }
        if (low == 0U) {
            low = ' ';
        }
        if (high < 0x20U || high > 0x7EU) {
            high = '?';
        }
        if (low < 0x20U || low > 0x7EU) {
            low = '?';
        }
        if (written + 2U < capacity) {
            destination[written++] = (char)high;
            destination[written++] = (char)low;
        }
    }
    while (written != 0U && destination[written - 1U] == ' ') {
        --written;
    }
    destination[written < capacity ? written : capacity - 1U] = '\0';
}

enum ahci_status ahci_ata_identity_parse(
    const void *identify_data,
    struct ahci_ata_identity *identity)
{
    const unsigned char *data;
    uint64_t capacity48;
    uint64_t capacity28;

    if (identify_data == NULL || identity == NULL) {
        return AHCI_NULL;
    }
    data = (const unsigned char *)identify_data;
    capacity48 = ahci_load_le64(data + 200U);
    capacity28 = ahci_load_le32(data + 120U);
    if (capacity48 == 0U && capacity28 == 0U) {
        return AHCI_BAD_IDENTIFY;
    }
    if (capacity48 == 0U) {
        capacity48 = capacity28;
    }

    memset(identity, 0, sizeof(*identity));
    identity->valid = true;
    identity->sector_count = capacity48;
    identity->block_size = 512U;
    ahci_ata_string(data, 20U, 10U, identity->serial,
                    sizeof(identity->serial));
    ahci_ata_string(data, 54U, 20U, identity->model,
                    sizeof(identity->model));
    return AHCI_OK;
}

static enum block_device_status ahci_block_map_error(
    enum ahci_status status)
{
    switch (status) {
    case AHCI_OK:
        return BLOCK_DEVICE_OK;
    case AHCI_BAD_RANGE:
        return BLOCK_DEVICE_OUT_OF_RANGE;
    case AHCI_BAD_ALIGNMENT:
    case AHCI_NULL:
    case AHCI_BAD_STATE:
        return BLOCK_DEVICE_BAD_DEVICE;
    case AHCI_TIMEOUT:
    case AHCI_FAILED_COMPLETION:
    case AHCI_IO_ERROR:
    case AHCI_CAPACITY:
    default:
        return BLOCK_DEVICE_IO_ERROR;
    }
}

static enum block_device_status ahci_block_device_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct ahci_block_device *backend =
        (struct ahci_block_device *)device->context;
    unsigned char *destination = buffer;
    size_t remaining = length;

    if (backend == NULL || backend->memory == NULL ||
        backend->memory->read == NULL ||
        buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    while (remaining != 0U) {
        size_t chunk = remaining > AHCI_MAX_TRANSFER_BYTES
            ? AHCI_MAX_TRANSFER_BYTES
            : remaining;
        uint64_t lba = offset / 512U;
        uint32_t block_count =
            (uint32_t)(chunk / 512U);
        enum ahci_status status;

        status = ahci_ata_read_blocks(
            backend->mmio, backend->memory,
            backend->controller, backend->map,
            backend->port_index, backend->command_list_phys,
            backend->command_table_phys, backend->data_phys,
            lba, block_count);
        if (status != AHCI_OK) {
            return ahci_block_map_error(status);
        }
        if (!backend->memory->read(
                backend->memory->context, backend->data_phys,
                destination, chunk)) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        destination += chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ahci_block_device_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct ahci_block_device *backend =
        (struct ahci_block_device *)device->context;
    const unsigned char *source = buffer;
    size_t remaining = length;

    if (backend == NULL || backend->memory == NULL ||
        backend->memory->write == NULL ||
        buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    while (remaining != 0U) {
        size_t chunk = remaining > AHCI_MAX_TRANSFER_BYTES
            ? AHCI_MAX_TRANSFER_BYTES
            : remaining;
        uint64_t lba = offset / 512U;
        uint32_t block_count =
            (uint32_t)(chunk / 512U);
        enum ahci_status status;

        if (!backend->memory->write(
                backend->memory->context, backend->data_phys,
                source, chunk)) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        status = ahci_ata_write_blocks(
            backend->mmio, backend->memory,
            backend->controller, backend->map,
            backend->port_index, backend->command_list_phys,
            backend->command_table_phys, backend->data_phys,
            lba, block_count);
        if (status != AHCI_OK) {
            return ahci_block_map_error(status);
        }
        source += chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status ahci_block_device_flush(
    struct block_device *device)
{
    struct ahci_block_device *backend =
        (struct ahci_block_device *)device->context;
    enum ahci_status status;

    if (backend == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    status = ahci_ata_flush_cache(
        backend->mmio, backend->memory,
        backend->controller, backend->map,
        backend->port_index, backend->command_list_phys,
        backend->command_table_phys);
    return ahci_block_map_error(status);
}

enum ahci_status ahci_block_device_bind(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    const struct ahci_ata_identity *identity,
    struct ahci_block_device *backend)
{
    const struct ahci_port_state *port;

    if (io == NULL || memory == NULL || memory->read == NULL ||
        memory->write == NULL || identity == NULL ||
        backend == NULL) {
        return AHCI_NULL;
    }
    if (!identity->valid || identity->block_size != 512U ||
        identity->sector_count == 0U ||
        identity->sector_count > BLOCK_DEVICE_BLOCK_COUNT_MAX) {
        return AHCI_CAPACITY;
    }
    port = ahci_command_port_state(controller, map, port_index);
    if (port == NULL || !port->present ||
        !port->device_attached || !port->supported) {
        return AHCI_BAD_STATE;
    }
    if (command_list_phys == 0U ||
        command_list_phys % AHCI_COMMAND_LIST_ALIGNMENT != 0U ||
        command_table_phys == 0U ||
        command_table_phys % 128U != 0U ||
        data_phys == 0U ||
        data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return AHCI_BAD_ALIGNMENT;
    }

    memset(backend, 0, sizeof(*backend));
    backend->mmio = io;
    backend->memory = memory;
    backend->controller = controller;
    backend->map = map;
    backend->port_index = port_index;
    backend->command_list_phys = command_list_phys;
    backend->command_table_phys = command_table_phys;
    backend->data_phys = data_phys;
    backend->sector_count = identity->sector_count;
    backend->block_size = 512U;
    backend->device.block_size = 512U;
    backend->device.block_count = identity->sector_count;
    backend->device.context = backend;
    backend->device.read_fn = ahci_block_device_read;
    backend->device.write_fn = ahci_block_device_write;
    backend->device.flush_fn = ahci_block_device_flush;
    return AHCI_OK;
}

const char *ahci_status_string(enum ahci_status status)
{
    switch (status) {
    case AHCI_OK:
        return "AHCI_OK";
    case AHCI_NULL:
        return "AHCI_NULL";
    case AHCI_UNSUPPORTED_DEVICE:
        return "AHCI_UNSUPPORTED_DEVICE";
    case AHCI_MISSING_MEMORY_BAR:
        return "AHCI_MISSING_MEMORY_BAR";
    case AHCI_BAD_BAR:
        return "AHCI_BAD_BAR";
    case AHCI_BAD_STATE:
        return "AHCI_BAD_STATE";
    case AHCI_IO_ERROR:
        return "AHCI_IO_ERROR";
    case AHCI_UNSUPPORTED_VERSION:
        return "AHCI_UNSUPPORTED_VERSION";
    case AHCI_CAPACITY:
        return "AHCI_CAPACITY";
    case AHCI_TIMEOUT:
        return "AHCI_TIMEOUT";
    case AHCI_FAILED_COMPLETION:
        return "AHCI_FAILED_COMPLETION";
    case AHCI_BAD_RANGE:
        return "AHCI_BAD_RANGE";
    case AHCI_BAD_ALIGNMENT:
        return "AHCI_BAD_ALIGNMENT";
    case AHCI_BAD_IDENTIFY:
        return "AHCI_BAD_IDENTIFY";
    default:
        return "AHCI_UNKNOWN";
    }
}
