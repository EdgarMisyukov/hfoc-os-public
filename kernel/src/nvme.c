#include "nvme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static uint16_t nvme_load_le16(const unsigned char *bytes)
{
    return (uint16_t)bytes[0] |
           ((uint16_t)bytes[1] << 8U);
}

static uint32_t nvme_load_le32(const unsigned char *bytes)
{
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8U) |
           ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static uint64_t nvme_load_le64(const unsigned char *bytes)
{
    uint64_t value = 0U;
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        value |= (uint64_t)bytes[index] << (8U * index);
    }
    return value;
}

static enum nvme_status nvme_queue_state_valid(
    const struct nvme_queue *queue);

static enum nvme_status nvme_mmio_read32(
    const struct nvme_mmio_io *io,
    uint64_t offset,
    uint32_t *value)
{
    if (io == NULL || io->read32 == NULL || value == NULL) {
        return NVME_NULL;
    }
    if (!io->read32(io->context, offset, value)) {
        return NVME_IO_ERROR;
    }
    return NVME_OK;
}

static enum nvme_status nvme_mmio_write32(
    const struct nvme_mmio_io *io,
    uint64_t offset,
    uint32_t value)
{
    if (io == NULL || io->write32 == NULL) {
        return NVME_NULL;
    }
    if (!io->write32(io->context, offset, value)) {
        return NVME_IO_ERROR;
    }
    return NVME_OK;
}

static enum nvme_status nvme_read_ready_state(
    const struct nvme_mmio_io *io,
    const struct nvme_controller *controller,
    bool *ready)
{
    enum nvme_status status;
    uint32_t status_register;

    if (io == NULL || controller == NULL || ready == NULL) {
        return NVME_NULL;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_STATUS_OFFSET, &status_register);
    if (status != NVME_OK) {
        return status;
    }
    *ready = (status_register & (1U << NVME_CSTS_READY_BIT)) != 0U;
    return NVME_OK;
}

/*
 * ITEM-429: wait for a controller-ready transition on the clock, not on a loop
 * count.
 *
 * The specification allows a controller up to 500 ms to set CSTS.RDY after
 * CC.EN is written (and to clear it after CC.EN is cleared).  The 2026-09-16
 * hardware run's NVMe probe ended in `nvme_status=NVME_TIMEOUT` on the
 * acceptance host while QEMU brings the same controller up instantly: the
 * driver waited for a hundred MMIO reads, microseconds, so "this part needs a
 * few hundred milliseconds" and "this part never answers" were the same
 * result.  The wait is now bounded in milliseconds and spent in steps the
 * provider's `delay_ms` can honour; a provider without that seam keeps the
 * bounded spin, which is short but never unbounded.  The milliseconds actually
 * spent are published on the controller so the boot record can say which of
 * the two it was.
 */
static enum nvme_status nvme_wait_controller_ready(
    const struct nvme_mmio_io *io,
    struct nvme_controller *controller,
    bool want_ready)
{
    uint32_t waited_ms = 0U;
    uint32_t spins = 0U;

    if (io == NULL || controller == NULL) {
        return NVME_NULL;
    }
    for (;;) {
        bool ready = false;
        const enum nvme_status status =
            nvme_read_ready_state(io, controller, &ready);

        if (status != NVME_OK) {
            return status;
        }
        if (ready == want_ready) {
            controller->ready_wait_ms = waited_ms;
            return NVME_OK;
        }
        if (io->delay_ms != NULL) {
            if (waited_ms >= NVME_CONTROLLER_READY_BUDGET_MS) {
                break;
            }
            io->delay_ms(io->context, NVME_CONTROLLER_READY_STEP_MS);
            waited_ms += NVME_CONTROLLER_READY_STEP_MS;
            continue;
        }
        if (++spins >= NVME_CONTROLLER_READY_SPIN_FALLBACK) {
            break;
        }
    }
    controller->ready_wait_ms = waited_ms;
    return NVME_TIMEOUT;
}

enum nvme_status nvme_controller_bind(
    const struct pci_device_header *header,
    struct nvme_controller *controller)
{
    const struct pci_bar *bar;

    if (header == NULL || controller == NULL) {
        return NVME_NULL;
    }
    if (header->base_class != PCI_CLASS_MASS_STORAGE ||
        header->subclass != PCI_SUBCLASS_NVME ||
        header->prog_if != PCI_PROGIF_NVME) {
        return NVME_UNSUPPORTED_DEVICE;
    }

    bar = &header->bars[NVME_MEMORY_BAR_INDEX];
    if (!bar->present || bar->io_space) {
        return NVME_MISSING_MEMORY_BAR;
    }
    if (bar->base == 0U || bar->length < 4096U) {
        return NVME_MISSING_MEMORY_BAR;
    }

    memset(controller, 0, sizeof(*controller));
    controller->present = true;
    controller->location = header->location;
    controller->vendor_id = header->vendor_id;
    controller->device_id = header->device_id;
    controller->mmio_base = bar->base;
    return NVME_OK;
}

enum nvme_status nvme_controller_reset(
    struct nvme_mmio_io *io,
    struct nvme_controller *controller)
{
    enum nvme_status status;
    uint32_t capabilities_low;
    uint32_t capabilities_high;
    uint32_t version;
    uint32_t config;
    uint32_t status_register;
    bool was_enabled;

    if (io == NULL || controller == NULL) {
        return NVME_NULL;
    }
    if (!controller->present || controller->mmio_base == 0U) {
        return NVME_BAD_STATE;
    }

    status = nvme_mmio_read32(
        io, NVME_REG_CAP_OFFSET, &capabilities_low);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_CAP_OFFSET + 4U, &capabilities_high);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_VERSION_OFFSET, &version);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_CONFIG_OFFSET, &config);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_STATUS_OFFSET, &status_register);
    if (status != NVME_OK) {
        return status;
    }

    was_enabled = (config & (1U << NVME_CC_ENABLE_BIT)) != 0U;
    if (was_enabled) {
        config &= ~(1U << NVME_CC_ENABLE_BIT);
        status = nvme_mmio_write32(
            io, NVME_REG_CONTROLLER_CONFIG_OFFSET, config);
        if (status != NVME_OK) {
            return status;
        }
        /* ITEM-429: the clearing transition is waited for on the clock. */
        status = nvme_wait_controller_ready(io, controller, false);
        if (status != NVME_OK) {
            return status;
        }
    }

    config = (config &
              ~(1U << NVME_CC_ENABLE_BIT |
                (0x7U << NVME_CC_CSS_SHIFT) |
                (0xFU << NVME_CC_MPS_SHIFT) |
                (0x7U << NVME_CC_AMS_SHIFT) |
                (0x3U << NVME_CC_SHN_SHIFT) |
                (0xFU << NVME_CC_IOSQES_SHIFT) |
                (0xFU << NVME_CC_IOCQES_SHIFT))) |
             (0U << NVME_CC_ENABLE_BIT) |
             (0U << NVME_CC_CSS_SHIFT) |
             (0U << NVME_CC_MPS_SHIFT) |
             (0U << NVME_CC_AMS_SHIFT) |
             (0U << NVME_CC_SHN_SHIFT) |
             (6U << NVME_CC_IOSQES_SHIFT) |
             (4U << NVME_CC_IOCQES_SHIFT);
    status = nvme_mmio_write32(
        io, NVME_REG_CONTROLLER_CONFIG_OFFSET, config);
    if (status != NVME_OK) {
        return status;
    }

    controller->capabilities =
        ((uint64_t)capabilities_high << 32U) | capabilities_low;
    controller->version = version;
    controller->controller_config = config;
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_STATUS_OFFSET, &status_register);
    if (status != NVME_OK) {
        return status;
    }
    controller->controller_status = status_register;
    return NVME_OK;
}

enum nvme_status nvme_controller_enable(
    struct nvme_mmio_io *io,
    struct nvme_controller *controller)
{
    enum nvme_status status;
    uint32_t config;
    uint32_t status_register;

    if (io == NULL || controller == NULL) {
        return NVME_NULL;
    }
    if (!controller->present || controller->mmio_base == 0U) {
        return NVME_BAD_STATE;
    }
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_CONFIG_OFFSET, &config);
    if (status != NVME_OK) {
        return status;
    }

    config |= 1U << NVME_CC_ENABLE_BIT;
    status = nvme_mmio_write32(
        io, NVME_REG_CONTROLLER_CONFIG_OFFSET, config);
    if (status != NVME_OK) {
        return status;
    }
    /* ITEM-429: the ready transition is waited for on the clock. */
    status = nvme_wait_controller_ready(io, controller, true);
    if (status != NVME_OK) {
        return status;
    }

    controller->controller_config = config;
    status = nvme_mmio_read32(
        io, NVME_REG_CONTROLLER_STATUS_OFFSET, &status_register);
    if (status != NVME_OK) {
        return status;
    }
    controller->controller_status = status_register;
    return NVME_OK;
}

enum nvme_status nvme_controller_parse_identify(
    const void *identify,
    struct nvme_controller_identity *identity)
{
    const unsigned char *data;

    if (identify == NULL || identity == NULL) {
        return NVME_NULL;
    }
    data = (const unsigned char *)identify;

    memset(identity, 0, sizeof(*identity));
    identity->valid = true;
    identity->vendor_id =
        nvme_load_le16(data + NVME_IDENTIFY_CONTROLLER_VID_OFFSET);
    identity->maximum_data_transfer_shift =
        data[NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET];
    return NVME_OK;
}

enum nvme_status nvme_namespace_parse_identify(
    uint32_t namespace_id,
    const void *identify,
    struct nvme_namespace *namespace_out)
{
    const unsigned char *data;
    uint64_t size_lba;
    uint64_t capacity_lba;
    uint64_t usage_lba;
    uint32_t format_index;
    uint32_t lba_format;
    uint32_t lbads;
    uint64_t block_size;

    if (identify == NULL || namespace_out == NULL) {
        return NVME_NULL;
    }
    if (namespace_id == 0U) {
        return NVME_BAD_ARGUMENT;
    }
    data = (const unsigned char *)identify;
    size_lba = nvme_load_le64(
        data + NVME_IDENTIFY_NAMESPACE_SIZE_OFFSET);
    capacity_lba = nvme_load_le64(
        data + NVME_IDENTIFY_NAMESPACE_CAPACITY_OFFSET);
    usage_lba = nvme_load_le64(
        data + NVME_IDENTIFY_NAMESPACE_USAGE_OFFSET);
    format_index =
        data[26U] & 0x7FU;

    if (size_lba == 0U || capacity_lba == 0U ||
        size_lba > capacity_lba || usage_lba > capacity_lba) {
        return NVME_BAD_IDENTIFY;
    }
    if (format_index >= NVME_NAMESPACE_MAX_LBA_FORMATS) {
        return NVME_BAD_IDENTIFY;
    }
    lba_format = nvme_load_le32(
        data + NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_OFFSET +
        format_index * NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_SIZE);
    lbads =
        (lba_format >> NVME_LBA_FORMAT_LBADS_SHIFT) &
        NVME_LBA_FORMAT_LBADS_MASK;
    if (lbads >= 64U) {
        return NVME_BAD_IDENTIFY;
    }
    block_size = UINT64_C(1) << lbads;
    if (block_size < 512U || block_size > 65536U) {
        return NVME_UNSUPPORTED_BLOCK_SIZE;
    }

    memset(namespace_out, 0, sizeof(*namespace_out));
    namespace_out->present = true;
    namespace_out->namespace_id = namespace_id;
    namespace_out->size_lba = size_lba;
    namespace_out->capacity_lba = capacity_lba;
    namespace_out->usage_lba = usage_lba;
    namespace_out->block_size = (uint32_t)block_size;
    namespace_out->lba_format_index = format_index;
    return NVME_OK;
}

enum nvme_status nvme_queue_bind(
    struct nvme_queue *queue,
    uint32_t queue_id,
    uint64_t submission_phys,
    uint64_t completion_phys,
    uint32_t capacity)
{
    if (queue == NULL) {
        return NVME_NULL;
    }
    if (submission_phys == 0U ||
        submission_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U ||
        completion_phys == 0U ||
        completion_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    if (capacity == 0U || capacity > NVME_QUEUE_CAPACITY_MAX) {
        return NVME_BAD_ARGUMENT;
    }
    if (queue_id > NVME_CDW10_QID_MASK) {
        return NVME_BAD_ARGUMENT;
    }

    memset(queue, 0, sizeof(*queue));
    queue->configured = true;
    queue->ready = true;
    queue->is_admin_queue = (queue_id == NVME_ADMIN_QUEUE_ID);
    queue->queue_id = queue_id;
    queue->capacity = capacity;
    queue->submission_phys = submission_phys;
    queue->completion_phys = completion_phys;
    queue->phase = 1U;
    queue->next_command_id = 1U;
    queue->doorbell_stride = NVME_DOORBELL_STRIDE_MIN;
    return NVME_OK;
}

enum nvme_status nvme_admin_queue_configure(
    struct nvme_mmio_io *mmio,
    struct nvme_queue *queue)
{
    enum nvme_status status;
    uint32_t capabilities_high;
    uint32_t queue_entries;
    uint32_t config;

    if (mmio == NULL || queue == NULL) {
        return NVME_NULL;
    }
    status = nvme_queue_state_valid(queue);
    if (status != NVME_OK) {
        return status;
    }
    if (queue->queue_id != NVME_ADMIN_QUEUE_ID) {
        return NVME_BAD_ARGUMENT;
    }
    status = nvme_mmio_read32(
        mmio, NVME_REG_CAP_OFFSET + 4U, &capabilities_high);
    if (status != NVME_OK) {
        return status;
    }
    queue->doorbell_stride =
        NVME_DOORBELL_STRIDE_MIN <<
        ((capabilities_high >>
          (NVME_CAP_DSTRD_SHIFT - 32U)) &
         NVME_CAP_DSTRD_MASK);
    status = nvme_mmio_read32(
        mmio, NVME_REG_CONTROLLER_CONFIG_OFFSET, &config);
    if (status != NVME_OK) {
        return status;
    }
    if ((config & (1U << NVME_CC_ENABLE_BIT)) != 0U) {
        return NVME_BAD_STATE;
    }
    queue_entries = queue->capacity - 1U;
    status = nvme_mmio_write32(
        mmio, NVME_REG_ADMIN_QUEUE_ATTRIBUTES_OFFSET,
        queue_entries | (queue_entries << 16U));
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_write32(
        mmio, NVME_REG_ADMIN_SUBMISSION_QUEUE_OFFSET,
        (uint32_t)queue->submission_phys);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_write32(
        mmio, NVME_REG_ADMIN_SUBMISSION_QUEUE_OFFSET + 4U,
        (uint32_t)(queue->submission_phys >> 32U));
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_write32(
        mmio, NVME_REG_ADMIN_COMPLETION_QUEUE_OFFSET,
        (uint32_t)queue->completion_phys);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_mmio_write32(
        mmio, NVME_REG_ADMIN_COMPLETION_QUEUE_OFFSET + 4U,
        (uint32_t)(queue->completion_phys >> 32U));
    return status;
}

static uint16_t nvme_command_id(const struct nvme_queue *queue)
{
    return queue->next_command_id;
}

static uint64_t nvme_queue_slot(
    const struct nvme_queue *queue,
    bool completion)
{
    const uint64_t base = completion
        ? queue->completion_phys
        : queue->submission_phys;
    const uint32_t slot = completion
        ? queue->completion_head % queue->capacity
        : queue->submission_tail % queue->capacity;
    const uint64_t size = completion
        ? NVME_COMPLETION_SIZE
        : NVME_COMMAND_SIZE;

    return base + (uint64_t)slot * size;
}

static uint64_t nvme_queue_doorbell_offset(
    const struct nvme_queue *queue,
    bool completion)
{
    return NVME_DOORBELL_BASE_OFFSET +
        ((uint64_t)queue->queue_id * 2U +
         (completion ? 1U : 0U)) *
            queue->doorbell_stride;
}

static enum nvme_status nvme_queue_state_valid(
    const struct nvme_queue *queue)
{
    if (queue == NULL || !queue->configured || !queue->ready) {
        return NVME_QUEUE_NOT_READY;
    }
    if (queue->capacity == 0U ||
        queue->capacity > NVME_QUEUE_CAPACITY_MAX ||
        queue->submission_phys == 0U ||
        queue->completion_phys == 0U) {
        return NVME_BAD_STATE;
    }
    return NVME_OK;
}

static enum nvme_status nvme_io_queue_state_valid(
    const struct nvme_queue *queue)
{
    enum nvme_status state_status;

    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }
    if (queue->is_admin_queue || !queue->io_queue_created) {
        return NVME_QUEUE_NOT_READY;
    }
    return NVME_OK;
}

static enum nvme_status nvme_command_create_io_queue_build(
    const struct nvme_queue *io_queue,
    struct nvme_queue *admin_queue,
    struct nvme_command *command,
    uint8_t opcode,
    uint64_t queue_phys,
    uint32_t cdw11)
{
    enum nvme_status state_status;
    uint16_t command_id;
    uint32_t queue_entries;

    if (io_queue == NULL || command == NULL) {
        return NVME_NULL;
    }
    if (io_queue->is_admin_queue || io_queue->queue_id == 0U) {
        return NVME_BAD_ARGUMENT;
    }
    if (queue_phys == 0U ||
        queue_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    if (io_queue->capacity == 0U ||
        io_queue->capacity > NVME_QUEUE_CAPACITY_MAX) {
        return NVME_BAD_ARGUMENT;
    }
    state_status = nvme_queue_state_valid(admin_queue);
    if (state_status != NVME_OK) {
        return state_status;
    }

    command_id = nvme_command_id(admin_queue);
    queue_entries = io_queue->capacity - 1U;
    memset(command, 0, sizeof(*command));
    command->dwords[0] =
        (uint32_t)opcode |
        ((uint32_t)command_id << NVME_CDW0_CID_SHIFT);
    command->dwords[6] = (uint32_t)queue_phys;
    command->dwords[7] = (uint32_t)(queue_phys >> 32U);
    command->dwords[10] =
        (io_queue->queue_id & NVME_CDW10_QID_MASK) |
        ((queue_entries & NVME_CDW10_QSIZE_MASK) <<
         NVME_CDW10_QSIZE_SHIFT);
    command->dwords[11] = cdw11;
    return NVME_OK;
}

enum nvme_status nvme_command_create_io_cq_build(
    const struct nvme_queue *io_queue,
    struct nvme_queue *admin_queue,
    struct nvme_command *command)
{
    if (io_queue == NULL) {
        return NVME_NULL;
    }
    return nvme_command_create_io_queue_build(
        io_queue, admin_queue, command,
        NVME_OPCODE_CREATE_IO_CQ,
        io_queue->completion_phys,
        1U << NVME_CDW11_PC_BIT);
}

enum nvme_status nvme_command_create_io_sq_build(
    const struct nvme_queue *io_queue,
    struct nvme_queue *admin_queue,
    struct nvme_command *command)
{
    if (io_queue == NULL) {
        return NVME_NULL;
    }
    return nvme_command_create_io_queue_build(
        io_queue, admin_queue, command,
        NVME_OPCODE_CREATE_IO_SQ,
        io_queue->submission_phys,
        (1U << NVME_CDW11_PC_BIT) |
        (io_queue->queue_id << NVME_CDW11_CQID_SHIFT));
}

enum nvme_status nvme_command_identify_build(
    uint32_t namespace_id,
    uint8_t cns,
    uint64_t data_phys,
    struct nvme_queue *queue,
    struct nvme_command *command)
{
    enum nvme_status state_status;
    uint16_t command_id;

    if (command == NULL) {
        return NVME_NULL;
    }
    if (data_phys == 0U ||
        data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    if (cns != NVME_IDENTIFY_CNS_NAMESPACE &&
        cns != NVME_IDENTIFY_CNS_CONTROLLER) {
        return NVME_BAD_ARGUMENT;
    }
    if (cns == NVME_IDENTIFY_CNS_CONTROLLER && namespace_id != 0U) {
        return NVME_BAD_ARGUMENT;
    }
    if (cns == NVME_IDENTIFY_CNS_NAMESPACE && namespace_id == 0U) {
        return NVME_BAD_ARGUMENT;
    }
    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }

    command_id = nvme_command_id(queue);
    memset(command, 0, sizeof(*command));
    command->dwords[0] =
        NVME_OPCODE_IDENTIFY |
        ((uint32_t)command_id << NVME_CDW0_CID_SHIFT);
    command->dwords[1] = namespace_id;
    command->dwords[6] = (uint32_t)data_phys;
    command->dwords[7] = (uint32_t)(data_phys >> 32U);
    command->dwords[10] =
        (uint32_t)cns & NVME_CDW10_CNS_MASK;
    return NVME_OK;
}

enum nvme_status nvme_command_io_build(
    uint8_t opcode,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys,
    struct nvme_queue *queue,
    struct nvme_command *command)
{
    enum nvme_status state_status;
    uint64_t transfer_bytes;
    uint16_t command_id;

    if (namespace_info == NULL || command == NULL) {
        return NVME_NULL;
    }
    if (opcode != NVME_OPCODE_READ &&
        opcode != NVME_OPCODE_WRITE) {
        return NVME_BAD_ARGUMENT;
    }
    if (!namespace_info->present || namespace_info->namespace_id == 0U ||
        namespace_info->block_size == 0U) {
        return NVME_BAD_STATE;
    }
    if (block_count == 0U || block_count > 0x10000U) {
        return NVME_BAD_ARGUMENT;
    }
    if (lba >= namespace_info->size_lba ||
        (uint64_t)block_count > namespace_info->size_lba - lba) {
        return NVME_BAD_RANGE;
    }
    transfer_bytes =
        (uint64_t)block_count * namespace_info->block_size;
    if (transfer_bytes == 0U ||
        transfer_bytes > NVME_MAX_TRANSFER_BYTES) {
        return NVME_BAD_RANGE;
    }
    if (data_phys == 0U ||
        data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }

    command_id = nvme_command_id(queue);
    memset(command, 0, sizeof(*command));
    command->dwords[0] =
        (uint32_t)opcode |
        ((uint32_t)command_id << NVME_CDW0_CID_SHIFT);
    command->dwords[1] = namespace_info->namespace_id;
    command->dwords[10] = (uint32_t)lba;
    command->dwords[11] = (uint32_t)(lba >> 32U);
    command->dwords[12] =
        (block_count - 1U) & NVME_CDW12_NLB_MASK;
    command->dwords[6] = (uint32_t)data_phys;
    command->dwords[7] = (uint32_t)(data_phys >> 32U);
    return NVME_OK;
}

enum nvme_status nvme_command_flush_build(
    const struct nvme_namespace *namespace_info,
    struct nvme_queue *queue,
    struct nvme_command *command)
{
    enum nvme_status state_status;
    uint16_t command_id;

    if (namespace_info == NULL || command == NULL) {
        return NVME_NULL;
    }
    if (!namespace_info->present || namespace_info->namespace_id == 0U) {
        return NVME_BAD_STATE;
    }
    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }

    command_id = nvme_command_id(queue);
    memset(command, 0, sizeof(*command));
    command->dwords[0] =
        NVME_OPCODE_FLUSH |
        ((uint32_t)command_id << NVME_CDW0_CID_SHIFT);
    command->dwords[1] = namespace_info->namespace_id;
    return NVME_OK;
}

enum nvme_status nvme_queue_submit_command(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_command *command)
{
    enum nvme_status state_status;
    enum nvme_status io_status;
    uint64_t address;
    uint32_t doorbell;
    uint32_t next_tail;
    uint16_t command_id;

    if (command == NULL || memory == NULL || memory->write == NULL) {
        return NVME_NULL;
    }
    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }
    if (queue->outstanding >= queue->capacity) {
        return NVME_QUEUE_FULL;
    }

    command_id = (uint16_t)(
        (command->dwords[0] >> NVME_CDW0_CID_SHIFT) &
        NVME_CDW0_CID_MASK);
    if (command_id != queue->next_command_id) {
        return NVME_BAD_STATE;
    }
    address = nvme_queue_slot(queue, false);
    if (!memory->write(
            memory->context, address, command, sizeof(*command))) {
        return NVME_IO_ERROR;
    }

    next_tail = (queue->submission_tail + 1U) % queue->capacity;
    doorbell = (uint32_t)nvme_queue_doorbell_offset(queue, false);
    io_status = nvme_mmio_write32(
        mmio, doorbell, next_tail);
    if (io_status != NVME_OK) {
        return io_status;
    }

    queue->submission_tail = next_tail;
    ++queue->outstanding;
    if (queue->next_command_id == UINT16_MAX) {
        queue->next_command_id = 1U;
    } else {
        ++queue->next_command_id;
    }
    return NVME_OK;
}

enum nvme_status nvme_queue_poll_completion(
    struct nvme_queue_memory_io *memory,
    struct nvme_mmio_io *mmio,
    struct nvme_queue *queue,
    struct nvme_completion *completion)
{
    enum nvme_status state_status;
    enum nvme_status io_status;
    uint32_t controller_status;
    uint32_t poll;

    if (completion == NULL || memory == NULL || memory->read == NULL) {
        return NVME_NULL;
    }
    state_status = nvme_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }
    if (queue->outstanding == 0U) {
        return NVME_QUEUE_NOT_READY;
    }

    for (poll = 0U; poll < NVME_COMPLETION_POLLS; ++poll) {
        struct nvme_completion draft;
        const uint32_t expected_phase = queue->phase;

        __asm__ volatile ("pause" : : : "memory");
        io_status = nvme_mmio_read32(
            mmio, NVME_REG_CONTROLLER_STATUS_OFFSET,
            &controller_status);
        if (io_status != NVME_OK) {
            return io_status;
        }
        memset(&draft, 0, sizeof(draft));
        if (!memory->read(
                memory->context, nvme_queue_slot(queue, true),
                &draft, sizeof(draft))) {
            return NVME_IO_ERROR;
        }
        if (((draft.dwords[3] >>
              NVME_COMPLETION_DWORD3_PHASE_BIT) & 1U) !=
            expected_phase) {
            continue;
        }
        *completion = draft;
        ++queue->completion_head;
        --queue->outstanding;
        if (queue->completion_head == queue->capacity) {
            queue->completion_head = 0U;
            queue->phase = expected_phase == 1U ? 0U : 1U;
        }
        io_status = nvme_mmio_write32(
            mmio,
            nvme_queue_doorbell_offset(queue, true),
            queue->completion_head);
        if (io_status != NVME_OK) {
            return io_status;
        }
        if (((draft.dwords[3] >>
              NVME_COMPLETION_DWORD3_STATUS_SHIFT) &
             NVME_COMPLETION_DWORD3_STATUS_MASK) != 0U) {
            return NVME_FAILED_COMPLETION;
        }
        return NVME_OK;
    }
    return NVME_TIMEOUT;
}

enum nvme_status nvme_completion_status(
    const struct nvme_completion *completion)
{
    if (completion == NULL) {
        return NVME_NULL;
    }
    if (((completion->dwords[3] >>
          NVME_COMPLETION_DWORD3_STATUS_SHIFT) &
         NVME_COMPLETION_DWORD3_STATUS_MASK) != 0U) {
        return NVME_FAILED_COMPLETION;
    }
    return NVME_OK;
}

static enum nvme_status nvme_io_execute(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_command *command)
{
    enum nvme_status status;
    struct nvme_completion completion;

    status = nvme_queue_submit_command(
        mmio, memory, queue, command);
    if (status != NVME_OK) {
        return status;
    }
    memset(&completion, 0, sizeof(completion));
    status = nvme_queue_poll_completion(
        memory, mmio, queue, &completion);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_completion_status(&completion);
}

enum nvme_status nvme_io_queue_configure(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *admin_queue,
    struct nvme_queue *io_queue)
{
    struct nvme_command command;
    enum nvme_status status;

    if (mmio == NULL || memory == NULL || admin_queue == NULL ||
        io_queue == NULL) {
        return NVME_NULL;
    }
    if (!admin_queue->is_admin_queue || io_queue->is_admin_queue) {
        return NVME_BAD_ARGUMENT;
    }
    if (io_queue->io_queue_created) {
        return NVME_BAD_STATE;
    }
    io_queue->doorbell_stride = admin_queue->doorbell_stride;

    status = nvme_command_create_io_cq_build(
        io_queue, admin_queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_io_execute(
        mmio, memory, admin_queue, &command);
    if (status != NVME_OK) {
        return status;
    }

    status = nvme_command_create_io_sq_build(
        io_queue, admin_queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_io_execute(
        mmio, memory, admin_queue, &command);
    if (status != NVME_OK) {
        return status;
    }

    io_queue->io_queue_created = true;
    return NVME_OK;
}

enum nvme_status nvme_namespace_read_blocks(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys)
{
    struct nvme_command command;
    enum nvme_status status;

    status = nvme_io_queue_state_valid(queue);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_command_io_build(
        NVME_OPCODE_READ, namespace_info, lba, block_count,
        data_phys, queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_io_execute(mmio, memory, queue, &command);
}

enum nvme_status nvme_namespace_write_blocks(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t lba,
    uint32_t block_count,
    uint64_t data_phys)
{
    struct nvme_command command;
    enum nvme_status status;

    status = nvme_io_queue_state_valid(queue);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_command_io_build(
        NVME_OPCODE_WRITE, namespace_info, lba, block_count,
        data_phys, queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_io_execute(mmio, memory, queue, &command);
}

enum nvme_status nvme_namespace_flush(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info)
{
    struct nvme_command command;
    enum nvme_status status;

    status = nvme_io_queue_state_valid(queue);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_command_flush_build(
        namespace_info, queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_io_execute(mmio, memory, queue, &command);
}

static enum block_device_status nvme_block_map_error(
    enum nvme_status status)
{
    switch (status) {
    case NVME_OK:
        return BLOCK_DEVICE_OK;
    case NVME_BAD_RANGE:
        return BLOCK_DEVICE_OUT_OF_RANGE;
    case NVME_NULL:
        return BLOCK_DEVICE_BAD_DEVICE;
    case NVME_IO_ERROR:
    case NVME_TIMEOUT:
    case NVME_QUEUE_FULL:
    case NVME_FAILED_COMPLETION:
    case NVME_BAD_STATE:
    default:
        return BLOCK_DEVICE_IO_ERROR;
    }
}

static enum block_device_status nvme_block_device_read(
    struct block_device *device,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    struct nvme_block_device *backend =
        (struct nvme_block_device *)device->context;
    unsigned char *destination = buffer;
    size_t remaining = length;

    if (backend == NULL || backend->memory == NULL ||
        backend->memory->read == NULL ||
        backend->queue == NULL || buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    while (remaining != 0U) {
        size_t chunk = remaining > NVME_MAX_TRANSFER_BYTES
            ? NVME_MAX_TRANSFER_BYTES
            : remaining;
        const uint64_t lba = offset / backend->device.block_size;
        const uint32_t block_count =
            (uint32_t)(chunk / backend->device.block_size);
        enum nvme_status nvme_status;

        nvme_status = nvme_namespace_read_blocks(
            backend->mmio, backend->memory, backend->queue,
            &backend->namespace_info, lba, block_count,
            backend->data_phys);
        if (nvme_status != NVME_OK) {
            return nvme_block_map_error(nvme_status);
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

static enum block_device_status nvme_block_device_write(
    struct block_device *device,
    uint64_t offset,
    const void *buffer,
    size_t length)
{
    struct nvme_block_device *backend =
        (struct nvme_block_device *)device->context;
    const unsigned char *source = buffer;
    size_t remaining = length;

    if (backend == NULL || backend->memory == NULL ||
        backend->memory->write == NULL ||
        backend->queue == NULL || buffer == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    while (remaining != 0U) {
        size_t chunk = remaining > NVME_MAX_TRANSFER_BYTES
            ? NVME_MAX_TRANSFER_BYTES
            : remaining;
        const uint64_t lba = offset / backend->device.block_size;
        const uint32_t block_count =
            (uint32_t)(chunk / backend->device.block_size);
        enum nvme_status nvme_status;

        if (!backend->memory->write(
                backend->memory->context, backend->data_phys,
                source, chunk)) {
            return BLOCK_DEVICE_IO_ERROR;
        }
        nvme_status = nvme_namespace_write_blocks(
            backend->mmio, backend->memory, backend->queue,
            &backend->namespace_info, lba, block_count,
            backend->data_phys);
        if (nvme_status != NVME_OK) {
            return nvme_block_map_error(nvme_status);
        }
        source += chunk;
        offset += chunk;
        remaining -= chunk;
    }
    return BLOCK_DEVICE_OK;
}

static enum block_device_status nvme_block_device_flush(
    struct block_device *device)
{
    struct nvme_block_device *backend =
        (struct nvme_block_device *)device->context;
    enum nvme_status nvme_status;

    if (backend == NULL || backend->queue == NULL) {
        return BLOCK_DEVICE_BAD_DEVICE;
    }
    nvme_status = nvme_namespace_flush(
        backend->mmio, backend->memory, backend->queue,
        &backend->namespace_info);
    return nvme_block_map_error(nvme_status);
}

enum nvme_status nvme_namespace_bind_block_device(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    const struct nvme_namespace *namespace_info,
    uint64_t data_phys,
    struct nvme_block_device *backend)
{
    enum nvme_status state_status;

    if (mmio == NULL || memory == NULL || memory->read == NULL ||
        memory->write == NULL || namespace_info == NULL ||
        backend == NULL) {
        return NVME_NULL;
    }
    if (!namespace_info->present ||
        namespace_info->block_size != 512U ||
        namespace_info->size_lba == 0U ||
        namespace_info->size_lba > BLOCK_DEVICE_BLOCK_COUNT_MAX) {
        return NVME_UNSUPPORTED_BLOCK_SIZE;
    }
    if (data_phys == 0U ||
        data_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    state_status = nvme_io_queue_state_valid(queue);
    if (state_status != NVME_OK) {
        return state_status;
    }

    memset(backend, 0, sizeof(*backend));
    backend->mmio = mmio;
    backend->memory = memory;
    backend->queue = queue;
    backend->namespace_info = *namespace_info;
    backend->data_phys = data_phys;
    backend->device.block_size = 512U;
    backend->device.block_count = namespace_info->size_lba;
    backend->device.context = backend;
    backend->device.read_fn = nvme_block_device_read;
    backend->device.write_fn = nvme_block_device_write;
    backend->device.flush_fn = nvme_block_device_flush;
    return NVME_OK;
}

static enum nvme_status nvme_read_identify_data(
    struct nvme_queue_memory_io *memory,
    uint64_t identify_phys,
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE])
{
    if (memory == NULL || memory->read == NULL || identify == NULL) {
        return NVME_NULL;
    }
    if (identify_phys == 0U ||
        identify_phys % PCI_DMA_ADDRESS_ALIGNMENT != 0U) {
        return NVME_BAD_ALIGNMENT;
    }
    if (!memory->read(
            memory->context, identify_phys, identify,
            NVME_IDENTIFY_DATA_SIZE)) {
        return NVME_IO_ERROR;
    }
    return NVME_OK;
}

enum nvme_status nvme_identify_controller(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    uint64_t identify_phys,
    struct nvme_controller_identity *identity)
{
    struct nvme_command command;
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    enum nvme_status status;

    if (identity == NULL) {
        return NVME_NULL;
    }
    if (queue == NULL || !queue->is_admin_queue) {
        return NVME_BAD_ARGUMENT;
    }
    status = nvme_command_identify_build(
        0U, NVME_IDENTIFY_CNS_CONTROLLER, identify_phys,
        queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_io_execute(mmio, memory, queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_read_identify_data(
        memory, identify_phys, identify);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_controller_parse_identify(identify, identity);
}

enum nvme_status nvme_namespace_discover(
    struct nvme_mmio_io *mmio,
    struct nvme_queue_memory_io *memory,
    struct nvme_queue *queue,
    uint32_t namespace_id,
    uint64_t identify_phys,
    struct nvme_namespace *namespace_out)
{
    struct nvme_command command;
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    enum nvme_status status;

    if (namespace_out == NULL) {
        return NVME_NULL;
    }
    if (queue == NULL || !queue->is_admin_queue) {
        return NVME_BAD_ARGUMENT;
    }
    status = nvme_command_identify_build(
        namespace_id, NVME_IDENTIFY_CNS_NAMESPACE, identify_phys,
        queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_io_execute(mmio, memory, queue, &command);
    if (status != NVME_OK) {
        return status;
    }
    status = nvme_read_identify_data(
        memory, identify_phys, identify);
    if (status != NVME_OK) {
        return status;
    }
    return nvme_namespace_parse_identify(
        namespace_id, identify, namespace_out);
}

const char *nvme_status_string(enum nvme_status status)
{
    switch (status) {
    case NVME_OK:
        return "NVME_OK";
    case NVME_NULL:
        return "NVME_NULL";
    case NVME_BAD_ARGUMENT:
        return "NVME_BAD_ARGUMENT";
    case NVME_UNSUPPORTED_DEVICE:
        return "NVME_UNSUPPORTED_DEVICE";
    case NVME_MISSING_MEMORY_BAR:
        return "NVME_MISSING_MEMORY_BAR";
    case NVME_IO_ERROR:
        return "NVME_IO_ERROR";
    case NVME_TIMEOUT:
        return "NVME_TIMEOUT";
    case NVME_BAD_IDENTIFY:
        return "NVME_BAD_IDENTIFY";
    case NVME_UNSUPPORTED_BLOCK_SIZE:
        return "NVME_UNSUPPORTED_BLOCK_SIZE";
    case NVME_CAPACITY:
        return "NVME_CAPACITY";
    case NVME_BAD_STATE:
        return "NVME_BAD_STATE";
    case NVME_QUEUE_FULL:
        return "NVME_QUEUE_FULL";
    case NVME_FAILED_COMPLETION:
        return "NVME_FAILED_COMPLETION";
    case NVME_BAD_RANGE:
        return "NVME_BAD_RANGE";
    case NVME_BAD_ALIGNMENT:
        return "NVME_BAD_ALIGNMENT";
    case NVME_QUEUE_NOT_READY:
        return "NVME_QUEUE_NOT_READY";
    default:
        return "NVME_UNKNOWN";
    }
}
