#ifndef HFOC_NVME_H
#define HFOC_NVME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"
#include "pci.h"
#include "pci_dma.h"

#define NVME_CONTROLLER_CAPACITY 1U
#define NVME_NAMESPACE_CAPACITY 16U
#define NVME_IDENTIFY_DATA_SIZE 4096U
#define NVME_MEMORY_BAR_INDEX 0U
/*
 * ITEM-429: a controller-ready transition is waited for on the clock, not on
 * an iteration count.  The budget is twice the 500 ms the specification allows
 * a controller to take, and it is spent in steps the provider's delay can
 * honour.  A provider without a delay seam keeps the old bounded spin, which
 * is short but never unbounded.
 */
#define NVME_CONTROLLER_READY_BUDGET_MS 1000U
#define NVME_CONTROLLER_READY_STEP_MS 20U
#define NVME_CONTROLLER_READY_SPIN_FALLBACK 100U
#define NVME_NAMESPACE_MAX_LBA_FORMATS 16U
#define NVME_QUEUE_CAPACITY_MAX 8U
#define NVME_COMPLETION_POLLS 100000U
#define NVME_MAX_TRANSFER_BYTES 4096U
#define NVME_COMMAND_WORD_COUNT 16U
#define NVME_COMPLETION_WORD_COUNT 4U
#define NVME_COMMAND_SIZE (NVME_COMMAND_WORD_COUNT * 4U)
#define NVME_COMPLETION_SIZE (NVME_COMPLETION_WORD_COUNT * 4U)
#define NVME_DOORBELL_STRIDE_MIN 4U
#define NVME_ADMIN_QUEUE_ID 0U
#define NVME_DOORBELL_BASE_OFFSET 0x1000U

#define NVME_OPCODE_WRITE 0x01U
#define NVME_OPCODE_READ 0x02U
#define NVME_OPCODE_IDENTIFY 0x06U
#define NVME_OPCODE_FLUSH 0x08U
#define NVME_OPCODE_CREATE_IO_CQ 0x05U
#define NVME_OPCODE_CREATE_IO_SQ 0x01U

#define NVME_IDENTIFY_CNS_NAMESPACE 0x00U
#define NVME_IDENTIFY_CNS_CONTROLLER 0x01U

#define NVME_CDW0_CID_SHIFT 16U
#define NVME_CDW0_CID_MASK 0xFFFFU
#define NVME_CDW10_CNS_MASK 0xFFU
#define NVME_CDW10_QID_MASK 0xFFFFU
#define NVME_CDW10_QSIZE_SHIFT 16U
#define NVME_CDW10_QSIZE_MASK 0xFFFFU
#define NVME_CDW11_PC_BIT 0U
#define NVME_CDW11_CQID_SHIFT 16U
#define NVME_CDW12_NLB_MASK 0xFFFFU

#define NVME_COMPLETION_DWORD3_PHASE_BIT 16U
#define NVME_COMPLETION_DWORD3_STATUS_SHIFT 17U
#define NVME_COMPLETION_DWORD3_STATUS_MASK 0x7FFFU
#define NVME_COMPLETION_DWORD3_CID_SHIFT 0U

#define NVME_REG_CAP_OFFSET 0x00U
#define NVME_REG_VERSION_OFFSET 0x08U
#define NVME_REG_INTERRUPT_MASK_SET_OFFSET 0x0CU
#define NVME_REG_INTERRUPT_MASK_CLEAR_OFFSET 0x10U
#define NVME_REG_CONTROLLER_CONFIG_OFFSET 0x14U
#define NVME_REG_CONTROLLER_STATUS_OFFSET 0x1CU
#define NVME_REG_ADMIN_QUEUE_ATTRIBUTES_OFFSET 0x24U
#define NVME_REG_ADMIN_SUBMISSION_QUEUE_OFFSET 0x28U
#define NVME_REG_ADMIN_COMPLETION_QUEUE_OFFSET 0x30U

#define NVME_CC_ENABLE_BIT 0U
#define NVME_CC_CSS_SHIFT 4U
#define NVME_CC_MPS_SHIFT 7U
#define NVME_CC_AMS_SHIFT 11U
#define NVME_CC_SHN_SHIFT 14U
#define NVME_CC_IOSQES_SHIFT 16U
#define NVME_CC_IOCQES_SHIFT 20U

#define NVME_CSTS_READY_BIT 0U
#define NVME_CSTS_SHUTDOWN_STATUS_SHIFT 2U
#define NVME_CSTS_PROCESSING_PAUSED_BIT 3U

#define NVME_CAP_DSTRD_SHIFT 32U
#define NVME_CAP_DSTRD_MASK 0xFU

#define NVME_IDENTIFY_CONTROLLER_VID_OFFSET 0U
#define NVME_IDENTIFY_CONTROLLER_SN_OFFSET 4U
#define NVME_IDENTIFY_CONTROLLER_SN_SIZE 20U
#define NVME_IDENTIFY_CONTROLLER_MN_OFFSET 24U
#define NVME_IDENTIFY_CONTROLLER_MN_SIZE 40U
#define NVME_IDENTIFY_CONTROLLER_FR_OFFSET 64U
#define NVME_IDENTIFY_CONTROLLER_FR_SIZE 8U
#define NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET 77U

#define NVME_IDENTIFY_NAMESPACE_SIZE_OFFSET 0U
#define NVME_IDENTIFY_NAMESPACE_CAPACITY_OFFSET 8U
#define NVME_IDENTIFY_NAMESPACE_USAGE_OFFSET 16U
#define NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_OFFSET 128U
#define NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_SIZE 4U
#define NVME_LBA_FORMAT_LBADS_SHIFT 16U
#define NVME_LBA_FORMAT_LBADS_MASK 0xFFU

enum nvme_status {
    NVME_OK = 0,
    NVME_NULL = 1,
    NVME_BAD_ARGUMENT = 2,
    NVME_UNSUPPORTED_DEVICE = 3,
    NVME_MISSING_MEMORY_BAR = 4,
    NVME_IO_ERROR = 5,
    NVME_TIMEOUT = 6,
    NVME_BAD_IDENTIFY = 7,
    NVME_UNSUPPORTED_BLOCK_SIZE = 8,
    NVME_CAPACITY = 9,
    NVME_BAD_STATE = 10,
    NVME_QUEUE_FULL = 11,
    NVME_FAILED_COMPLETION = 12,
    NVME_BAD_RANGE = 13,
    NVME_BAD_ALIGNMENT = 14,
    NVME_QUEUE_NOT_READY = 15,
};

struct nvme_mmio_io;

typedef bool (*nvme_mmio_read32_fn)(
    void *context,
    uint64_t offset,
    uint32_t *value);
typedef bool (*nvme_mmio_write32_fn)(
    void *context,
    uint64_t offset,
    uint32_t value);
/*
 * ITEM-429: the wait a controller asks for.
 *
 * A controller-ready transition (CSTS.RDY after CC.EN changes) may take up to
 * 500 ms by the specification, and a driver that counts loop iterations instead
 * of milliseconds gives it microseconds: the 2026-09-16 hardware run's NVMe
 * bring-up ended in `nvme_status=NVME_TIMEOUT` on a controller QEMU brings up
 * instantly.  The provider supplies the bounded delay - the kernel one owns a
 * time base that works before the timer exists, a host proof one advances a
 * fake clock - so the driver never mistakes a spin count for a deadline.  A
 * provider without the seam keeps the bounded spin fallback.
 */
typedef void (*nvme_mmio_delay_ms_fn)(
    void *context,
    uint32_t milliseconds);

struct nvme_mmio_io {
    void *context;
    nvme_mmio_read32_fn read32;
    nvme_mmio_write32_fn write32;
    nvme_mmio_delay_ms_fn delay_ms;
};

struct nvme_controller {
    bool present;
    struct pci_location location;
    uint16_t vendor_id;
    uint16_t device_id;
    uint64_t mmio_base;
    uint64_t capabilities;
    uint32_t version;
    uint32_t controller_config;
    uint32_t controller_status;
    /*
     * ITEM-429: what the last controller-ready wait cost, so the boot record
     * can say whether a timeout was a controller that never answered or a
     * driver that never waited.
     */
    uint32_t ready_wait_ms;
    uint8_t maximum_data_transfer_shift;
};

struct nvme_controller_identity {
    bool valid;
    uint16_t vendor_id;
    uint8_t maximum_data_transfer_shift;
};

struct nvme_namespace {
    bool present;
    uint32_t namespace_id;
    uint64_t size_lba;
    uint64_t capacity_lba;
    uint64_t usage_lba;
    uint32_t block_size;
    uint32_t lba_format_index;
};

struct nvme_queue_memory_io;

typedef bool (*nvme_queue_memory_read_fn)(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length);
typedef bool (*nvme_queue_memory_write_fn)(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length);

struct nvme_queue_memory_io {
    void *context;
    nvme_queue_memory_read_fn read;
    nvme_queue_memory_write_fn write;
};

struct nvme_command {
    uint32_t dwords[NVME_COMMAND_WORD_COUNT];
};

struct nvme_completion {
    uint32_t dwords[NVME_COMPLETION_WORD_COUNT];
};

struct nvme_queue {
    bool configured;
    bool ready;
    bool is_admin_queue;
    bool io_queue_created;
    uint32_t queue_id;
    uint32_t capacity;
    uint64_t submission_phys;
    uint64_t completion_phys;
    uint32_t submission_tail;
    uint32_t completion_head;
    uint32_t phase;
    uint32_t outstanding;
    uint32_t doorbell_stride;
    uint16_t next_command_id;
};

struct nvme_block_device {
    struct block_device device;
    struct nvme_mmio_io *mmio;
    struct nvme_queue_memory_io *memory;
    struct nvme_queue *queue;
    struct nvme_namespace namespace_info;
    uint64_t data_phys;
};

enum nvme_status nvme_controller_bind(
    const struct pci_device_header *header,
    struct nvme_controller *controller);
enum nvme_status nvme_controller_reset(
    struct nvme_mmio_io *io,
    struct nvme_controller *controller);
enum nvme_status nvme_controller_enable(
    struct nvme_mmio_io *io,
    struct nvme_controller *controller);
enum nvme_status nvme_controller_parse_identify(
    const void *identify,
    struct nvme_controller_identity *identity);
enum nvme_status nvme_namespace_parse_identify(
    uint32_t namespace_id,
    const void *identify,
    struct nvme_namespace *namespace_out);
enum nvme_status nvme_queue_bind(
    struct nvme_queue *queue,
    uint32_t queue_id,
    uint64_t submission_phys,
    uint64_t completion_phys,
    uint32_t capacity);
enum nvme_status nvme_admin_queue_configure(
    struct nvme_mmio_io *mmio,
    struct nvme_queue *queue);
enum nvme_status nvme_command_create_io_cq_build(
    const struct nvme_queue *io_queue,
    struct nvme_queue *admin_queue,
    struct nvme_command *command);
enum nvme_status nvme_command_create_io_sq_build(
    const struct nvme_queue *io_queue,
    struct nvme_queue *admin_queue,
    struct nvme_command *command);
enum nvme_status nvme_io_queue_configure(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *admin_queue,
    struct nvme_queue *io_queue);
enum nvme_status nvme_identify_controller(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    uint64_t identify_phys,
    struct nvme_controller_identity *identity);
enum nvme_status nvme_namespace_discover(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    uint32_t namespace_id,
    uint64_t identify_phys,
    struct nvme_namespace *namespace_out);
enum nvme_status nvme_namespace_bind_block_device(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t data_phys,
    struct nvme_block_device *backend);
enum nvme_status nvme_command_identify_build(
    uint32_t namespace_id,
    uint8_t cns,
    uint64_t data_phys,
    struct nvme_queue *queue,
    struct nvme_command *command);
enum nvme_status nvme_command_io_build(
    uint8_t opcode,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys,
    struct nvme_queue *queue,
    struct nvme_command *command);
enum nvme_status nvme_command_flush_build(
    const struct nvme_namespace *namespace_info,
    struct nvme_queue *queue,
    struct nvme_command *command);
enum nvme_status nvme_queue_submit_command(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_command *command);
enum nvme_status nvme_queue_poll_completion(
    struct nvme_queue_memory_io *memory,
    struct nvme_mmio_io *mmio,
    struct nvme_queue *queue,
    struct nvme_completion *completion);
enum nvme_status nvme_completion_status(
    const struct nvme_completion *completion);
enum nvme_status nvme_namespace_read_blocks(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys);
enum nvme_status nvme_namespace_write_blocks(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys);
enum nvme_status nvme_namespace_flush(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info);
const char *nvme_status_string(enum nvme_status status);

#endif
