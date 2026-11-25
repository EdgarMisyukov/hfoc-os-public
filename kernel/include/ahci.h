#ifndef HFOC_AHCI_H
#define HFOC_AHCI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "pci.h"
#include "pci_dma.h"

#define AHCI_MEMORY_BAR_INDEX 5U
#define AHCI_MIN_MMIO_LENGTH 0x100U
#define AHCI_MAX_PORT_COUNT 32U
#define AHCI_PORT_REGISTER_SIZE 0x80U

#define AHCI_CAP_OFFSET 0x00U
#define AHCI_GHC_OFFSET 0x04U
#define AHCI_IS_OFFSET 0x08U
#define AHCI_PI_OFFSET 0x0CU
#define AHCI_VS_OFFSET 0x10U

#define AHCI_PORT_BASE_OFFSET 0x100U
#define AHCI_PORT_CLB_OFFSET 0x00U
#define AHCI_PORT_CLBU_OFFSET 0x04U
#define AHCI_PORT_FB_OFFSET 0x08U
#define AHCI_PORT_FBU_OFFSET 0x0CU
#define AHCI_PORT_CMD_OFFSET 0x18U
#define AHCI_PORT_SIG_OFFSET 0x24U
#define AHCI_PORT_SSTS_OFFSET 0x28U
#define AHCI_PORT_IS_OFFSET 0x10U
#define AHCI_PORT_IE_OFFSET 0x14U
#define AHCI_PORT_TFD_OFFSET 0x20U
#define AHCI_PORT_CI_OFFSET 0x38U

#define AHCI_CAP_NP_SHIFT 0U
#define AHCI_CAP_NP_MASK 0x1FU
#define AHCI_GHC_AE_BIT 31U
#define AHCI_GHC_IE_BIT 1U
#define AHCI_GHC_HR_BIT 0U
#define AHCI_PORT_CMD_ST_BIT 0U
#define AHCI_PORT_CMD_FRE_BIT 4U
#define AHCI_COMMAND_LIST_ALIGNMENT 1024U
#define AHCI_FIS_ALIGNMENT 256U
#define AHCI_PORT_SIG_NO_DEVICE UINT32_C(0xFFFFFFFF)
#define AHCI_PORT_SIG_ATA UINT32_C(0x00000101)
#define AHCI_PORT_SSTS_DET_MASK 0x0FU
#define AHCI_PORT_SSTS_DET_PRESENT 0x03U

#define AHCI_COMMAND_LIST_ENTRY_SIZE 32U
#define AHCI_COMMAND_TABLE_SIZE 0x80U
#define AHCI_PRD_ENTRY_SIZE 16U
#define AHCI_COMMAND_SLOT_INDEX 0U
#define AHCI_COMMAND_SLOT_MASK UINT32_C(1)
#define AHCI_COMMAND_HEADER_CFL_SHIFT 0U
#define AHCI_COMMAND_HEADER_WRITE_BIT 6U
#define AHCI_H2D_FIS_CFL 5U
#define AHCI_ATA_IDENTIFY_SIZE 512U
#define AHCI_MAX_TRANSFER_BYTES 4096U
#define AHCI_MAX_SECTORS_PER_TRANSFER \
    (AHCI_MAX_TRANSFER_BYTES / 512U)
#define AHCI_COMMAND_POLLS 100000U

#define AHCI_TFD_ERR_BIT 1U
#define AHCI_TFD_BSY_BIT 0x80U
#define AHCI_TFD_DRQ_BIT 0x08U

#define AHCI_ATA_IDENTIFY_DEVICE 0xECU
#define AHCI_ATA_READ_DMA_EXT 0x25U
#define AHCI_ATA_WRITE_DMA_EXT 0x35U
#define AHCI_ATA_FLUSH_CACHE_EXT 0xEAU
#define AHCI_ATA_LBA_BIT 0x40U
#define AHCI_ATA_DEVICE_OBS 0xA0U

enum ahci_status {
    AHCI_OK = 0,
    AHCI_NULL = 1,
    AHCI_UNSUPPORTED_DEVICE = 2,
    AHCI_MISSING_MEMORY_BAR = 3,
    AHCI_BAD_BAR = 4,
    AHCI_BAD_STATE = 5,
    AHCI_IO_ERROR = 6,
    AHCI_UNSUPPORTED_VERSION = 7,
    AHCI_CAPACITY = 8,
    AHCI_TIMEOUT = 9,
    AHCI_FAILED_COMPLETION = 10,
    AHCI_BAD_RANGE = 11,
    AHCI_BAD_ALIGNMENT = 12,
    AHCI_BAD_IDENTIFY = 13,
};

struct ahci_mmio_io;

typedef bool (*ahci_mmio_read32_fn)(
    void *context,
    uint64_t offset,
    uint32_t *value);
typedef bool (*ahci_mmio_write32_fn)(
    void *context,
    uint64_t offset,
    uint32_t value);

struct ahci_mmio_io {
    void *context;
    ahci_mmio_read32_fn read32;
    ahci_mmio_write32_fn write32;
};

struct ahci_controller {
    bool present;
    bool ahci_enabled;
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t mmio_base;
    uint32_t capabilities;
    uint32_t global_host_control;
    uint32_t interrupt_status;
    uint32_t ports_implemented;
    uint32_t version;
};

struct ahci_port_state {
    bool present;
    bool device_attached;
    bool supported;
    uint32_t index;
    uint32_t command;
    uint32_t signature;
    uint32_t status;
};

struct ahci_port_map {
    bool complete;
    size_t port_count;
    struct ahci_port_state ports[AHCI_MAX_PORT_COUNT];
};

struct ahci_dma_io;

typedef bool (*ahci_dma_read_fn)(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length);
typedef bool (*ahci_dma_write_fn)(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length);

struct ahci_dma_io {
    void *context;
    ahci_dma_read_fn read;
    ahci_dma_write_fn write;
};

/*
 * ITEM-394: the IDENTIFY DEVICE model and serial strings identify the disk to
 * the operator.  The installer media names its target by the serial and the
 * product install flow publishes both, so a machine with more than one disk
 * cannot be written by accident.  ATA pads both fields with spaces.
 */
#define AHCI_ATA_MODEL_LENGTH 41U
#define AHCI_ATA_SERIAL_LENGTH 21U

struct ahci_ata_identity {
    bool valid;
    uint64_t sector_count;
    uint32_t block_size;
    char model[AHCI_ATA_MODEL_LENGTH];
    char serial[AHCI_ATA_SERIAL_LENGTH];
};

struct ahci_block_device {
    struct block_device device;
    struct ahci_mmio_io *mmio;
    struct ahci_dma_io *memory;
    const struct ahci_controller *controller;
    const struct ahci_port_map *map;
    uint32_t port_index;
    uint64_t command_list_phys;
    uint64_t command_table_phys;
    uint64_t data_phys;
    uint64_t sector_count;
    uint32_t block_size;
};

enum ahci_status ahci_controller_bind(
    const struct pci_device_header *header,
    struct ahci_controller *controller);
enum ahci_status ahci_controller_discover(
    struct ahci_mmio_io *io,
    struct ahci_controller *controller);
enum ahci_status ahci_controller_activate(
    struct ahci_mmio_io *io,
    struct ahci_controller *controller);
enum ahci_status ahci_controller_discover_ports(
    struct ahci_mmio_io *io,
    const struct ahci_controller *controller,
    struct ahci_port_map *map);
enum ahci_status ahci_port_dma_start(
    struct ahci_mmio_io *io,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t fis_phys);
enum ahci_status ahci_ata_identify(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys,
    uint64_t data_phys,
    struct ahci_ata_identity *identity);
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
    uint32_t block_count);
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
    uint32_t block_count);
enum ahci_status ahci_ata_flush_cache(
    struct ahci_mmio_io *io,
    struct ahci_dma_io *memory,
    const struct ahci_controller *controller,
    const struct ahci_port_map *map,
    uint32_t port_index,
    uint64_t command_list_phys,
    uint64_t command_table_phys);
enum ahci_status ahci_ata_identity_parse(
    const void *identify_data,
    struct ahci_ata_identity *identity);
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
    struct ahci_block_device *backend);
const char *ahci_status_string(enum ahci_status status);

#endif
