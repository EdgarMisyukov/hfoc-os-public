#include "nvme.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NVME_HOST_TEST

#define TEST_MMIO_REGISTER_COUNT 4096U
#define TEST_BUS 1U
#define TEST_DEVICE 3U
#define TEST_FUNCTION 0U
#define TEST_MMIO_BASE UINT64_C(0xFE000000)
#define TEST_MMIO_LENGTH UINT64_C(0x1000)
#define TEST_VENDOR 0x8086U
#define TEST_NVME_DEVICE_ID 0x5845U
#define TEST_NSID 1U
#define TEST_IO_QUEUE_ID 1U
#define TEST_QUEUE_MEMORY_BASE UINT64_C(0x100000)
#define TEST_QUEUE_MEMORY_SIZE (1024U * 1024U)
#define TEST_SUBMISSION_BASE TEST_QUEUE_MEMORY_BASE
#define TEST_COMPLETION_BASE (TEST_QUEUE_MEMORY_BASE + UINT64_C(0x1000))
#define TEST_IO_SUBMISSION_BASE \
    (TEST_QUEUE_MEMORY_BASE + UINT64_C(0x2000))
#define TEST_IO_COMPLETION_BASE \
    (TEST_QUEUE_MEMORY_BASE + UINT64_C(0x3000))
#define TEST_DATA_BASE (TEST_QUEUE_MEMORY_BASE + UINT64_C(0x4000))

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static bool all_bytes(const unsigned char *buffer,
                      size_t length,
                      unsigned char value)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if (buffer[index] != value) {
            return false;
        }
    }
    return true;
}

struct mmio_fixture {
    uint32_t registers[TEST_MMIO_REGISTER_COUNT];
    uint32_t config;
    uint32_t status;
    struct memory_fixture *auto_memory;
    struct nvme_queue *auto_queue;
    bool fail_read;
    bool fail_write;
    bool ready_after_enable;
    bool auto_complete;
    uint32_t auto_completion_status;
    uint32_t auto_completion_count;
    size_t read_calls;
    size_t write_calls;
    /*
     * ITEM-429: a controller on a clock.  `fake_ms` moves only when the driver
     * asks its provider for a delay, so a controller that reports CSTS.RDY
     * after `ready_delay_ms` is invisible to a driver that counts loop
     * iterations - exactly what the 2026-09-16 hardware run met.  `ready_never`
     * is the other half: a controller that never answers still ends the wait at
     * the budget, and `delay_calls` is the wait the driver actually asked for.
     */
    uint32_t fake_ms;
    uint32_t enable_ms;
    uint32_t disable_ms;
    uint32_t ready_delay_ms;
    uint32_t ready_clear_delay_ms;
    bool ready_never;
    uint32_t delay_calls;
};

struct memory_fixture {
    unsigned char storage[TEST_QUEUE_MEMORY_SIZE];
};

static void fixture_init(struct mmio_fixture *fixture, bool initially_ready)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->registers[0U] = 0x00000001U;
    fixture->registers[1U] = 0x00000000U;
    fixture->registers[2U] = 0x00010200U;
    fixture->config = 0U;
    fixture->status = initially_ready ? 1U : 0U;
    fixture->ready_after_enable = true;
}

static bool memory_read(
    void *context,
    uint64_t address,
    void *buffer,
    size_t length)
{
    struct memory_fixture *fixture =
        (struct memory_fixture *)context;
    uint64_t relative;

    if (buffer == NULL || length == 0U ||
        address < TEST_QUEUE_MEMORY_BASE ||
        length > TEST_QUEUE_MEMORY_SIZE) {
        return false;
    }
    relative = address - TEST_QUEUE_MEMORY_BASE;
    if (relative > TEST_QUEUE_MEMORY_SIZE ||
        length > TEST_QUEUE_MEMORY_SIZE - (size_t)relative) {
        return false;
    }
    memcpy(buffer, fixture->storage + (size_t)relative, length);
    return true;
}

static bool memory_write(
    void *context,
    uint64_t address,
    const void *buffer,
    size_t length)
{
    struct memory_fixture *fixture =
        (struct memory_fixture *)context;
    uint64_t relative;

    if (buffer == NULL || length == 0U ||
        address < TEST_QUEUE_MEMORY_BASE ||
        length > TEST_QUEUE_MEMORY_SIZE) {
        return false;
    }
    relative = address - TEST_QUEUE_MEMORY_BASE;
    if (relative > TEST_QUEUE_MEMORY_SIZE ||
        length > TEST_QUEUE_MEMORY_SIZE - (size_t)relative) {
        return false;
    }
    memcpy(fixture->storage + (size_t)relative, buffer, length);
    return true;
}

static struct nvme_queue_memory_io memory_io(
    struct memory_fixture *fixture)
{
    struct nvme_queue_memory_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read = memory_read;
    io.write = memory_write;
    return io;
}

static void fixture_complete_command(
    struct memory_fixture *memory,
    struct nvme_queue *queue,
    uint16_t command_id,
    uint32_t status)
{
    struct nvme_completion completion;
    const uint64_t slot =
        queue->completion_phys +
        (uint64_t)(queue->completion_head % queue->capacity) *
            NVME_COMPLETION_SIZE;

    memset(&completion, 0, sizeof(completion));
    completion.dwords[3] =
        ((uint32_t)command_id << NVME_COMPLETION_DWORD3_CID_SHIFT) |
        (status << NVME_COMPLETION_DWORD3_STATUS_SHIFT) |
        (queue->phase << NVME_COMPLETION_DWORD3_PHASE_BIT);
    CHECK(memory_write(
              memory, slot, &completion, sizeof(completion)),
          "test completion can be published into queue memory");
}

static void fixture_complete_slot(
    struct memory_fixture *memory,
    struct nvme_queue *queue,
    uint32_t slot_index,
    uint16_t command_id,
    uint32_t status)
{
    struct nvme_completion completion;
    const uint64_t slot =
        queue->completion_phys +
        (uint64_t)slot_index * NVME_COMPLETION_SIZE;

    memset(&completion, 0, sizeof(completion));
    completion.dwords[3] =
        ((uint32_t)command_id << NVME_COMPLETION_DWORD3_CID_SHIFT) |
        (status << NVME_COMPLETION_DWORD3_STATUS_SHIFT) |
        (queue->phase << NVME_COMPLETION_DWORD3_PHASE_BIT);
    CHECK(memory_write(
              memory, slot, &completion, sizeof(completion)),
          "test completion can be published into ordered queue slot");
}

static bool fixture_read(
    void *context,
    uint64_t offset,
    uint32_t *value)
{
    struct mmio_fixture *fixture =
        (struct mmio_fixture *)context;
    const size_t index = (size_t)offset / 4U;

    ++fixture->read_calls;
    if (fixture->fail_read || index >= TEST_MMIO_REGISTER_COUNT ||
        (offset % 4U) != 0U) {
        return false;
    }
    if (offset == NVME_REG_CONTROLLER_CONFIG_OFFSET) {
        *value = fixture->config;
        return true;
    }
    if (offset == NVME_REG_CONTROLLER_STATUS_OFFSET) {
        *value = fixture->status;
        return true;
    }
    *value = fixture->registers[index];
    return true;
}

static bool fixture_write(
    void *context,
    uint64_t offset,
    uint32_t value)
{
    struct mmio_fixture *fixture =
        (struct mmio_fixture *)context;
    const size_t index = (size_t)offset / 4U;

    ++fixture->write_calls;
    if (fixture->fail_write || index >= TEST_MMIO_REGISTER_COUNT ||
        (offset % 4U) != 0U) {
        return false;
    }
    if (offset == NVME_REG_CONTROLLER_CONFIG_OFFSET) {
        /*
         * ITEM-429: only a transition arms a wait.  A write that leaves the
         * enable bit where it was is not a new enable (or disable), so it must
         * not restart the delay the driver is measuring - and a later
         * configuration write must not re-assert a ready bit the part cleared.
         */
        const bool was_enabled =
            (fixture->config & (1U << NVME_CC_ENABLE_BIT)) != 0U;
        const bool now_enabled =
            (value & (1U << NVME_CC_ENABLE_BIT)) != 0U;

        fixture->config = value;
        if (now_enabled && !was_enabled) {
            if (fixture->ready_never) {
                fixture->status = 0U;
            } else if (fixture->ready_delay_ms != 0U) {
                /* ITEM-429: ready only once the clock has moved. */
                fixture->enable_ms = fixture->fake_ms;
                fixture->status = 0U;
            } else {
                fixture->status = fixture->ready_after_enable ? 1U : 0U;
            }
        } else if (!now_enabled && was_enabled) {
            if (fixture->ready_clear_delay_ms != 0U) {
                /* ITEM-429: the part clears RDY only once the clock has. */
                fixture->disable_ms = fixture->fake_ms;
                fixture->status = 1U;
            } else {
                fixture->status = 0U;
            }
        } else if (!now_enabled) {
            fixture->status = 0U;
        }
        return true;
    }
    if (fixture->auto_complete &&
        fixture->auto_queue != NULL &&
        offset ==
            NVME_DOORBELL_BASE_OFFSET +
                2U * fixture->auto_queue->queue_id *
                    fixture->auto_queue->doorbell_stride &&
        fixture->auto_memory != NULL &&
        fixture->auto_queue->capacity != 0U) {
        const uint32_t slot =
            fixture->auto_queue->completion_head %
            fixture->auto_queue->capacity;
        const uint32_t phase = fixture->auto_queue->phase;
        const uint64_t command_address =
            fixture->auto_queue->submission_phys +
            (uint64_t)(
                fixture->auto_queue->submission_tail %
                fixture->auto_queue->capacity) *
                NVME_COMMAND_SIZE;
        const uint64_t completion_address =
            fixture->auto_queue->completion_phys +
            (uint64_t)slot * NVME_COMPLETION_SIZE;
        struct nvme_command queued;
        struct nvme_completion completion;
        uint32_t command_dword0;
        uint16_t command_id;

        if (!memory_read(
                fixture->auto_memory, command_address,
                &queued, sizeof(queued))) {
            return false;
        }
        command_dword0 = queued.dwords[0];
        command_id = (uint16_t)(
            (command_dword0 >> NVME_CDW0_CID_SHIFT) &
            NVME_CDW0_CID_MASK);
        memset(&completion, 0, sizeof(completion));
        completion.dwords[3] =
            ((uint32_t)command_id << NVME_COMPLETION_DWORD3_CID_SHIFT) |
            (fixture->auto_completion_status <<
             NVME_COMPLETION_DWORD3_STATUS_SHIFT) |
            (phase << NVME_COMPLETION_DWORD3_PHASE_BIT);
        if (!memory_write(
                fixture->auto_memory, completion_address,
                &completion, sizeof(completion))) {
            return false;
        }
        ++fixture->auto_completion_count;
    }
    fixture->registers[index] = value;
    return true;
}

/*
 * ITEM-429: the fake clock.  Time passes only because the driver asked its
 * provider to wait, so a driver that counts iterations never sees the ready
 * bit it is waiting for.
 */
static void fixture_delay_ms(void *context, uint32_t milliseconds)
{
    struct mmio_fixture *fixture = (struct mmio_fixture *)context;

    ++fixture->delay_calls;
    fixture->fake_ms += milliseconds;
    if (fixture->ready_after_enable && !fixture->ready_never &&
        fixture->ready_delay_ms != 0U &&
        (fixture->config & (1U << NVME_CC_ENABLE_BIT)) != 0U &&
        (uint32_t)(fixture->fake_ms - fixture->enable_ms) >=
            fixture->ready_delay_ms) {
        fixture->status = 1U;
    }
    if (fixture->ready_clear_delay_ms != 0U &&
        (fixture->config & (1U << NVME_CC_ENABLE_BIT)) == 0U &&
        (uint32_t)(fixture->fake_ms - fixture->disable_ms) >=
            fixture->ready_clear_delay_ms) {
        fixture->status = 0U;
    }
}

static struct nvme_mmio_io fixture_io(struct mmio_fixture *fixture)
{
    struct nvme_mmio_io io;

    memset(&io, 0, sizeof(io));
    io.context = fixture;
    io.read32 = fixture_read;
    io.write32 = fixture_write;
    /* ITEM-429: the provider's bounded delay, advancing the fake clock. */
    io.delay_ms = fixture_delay_ms;
    return io;
}

static struct pci_device_header nvme_header(bool valid_bar)
{
    struct pci_device_header header;

    memset(&header, 0, sizeof(header));
    header.present = true;
    header.location.bus = TEST_BUS;
    header.location.device = TEST_DEVICE;
    header.location.function = TEST_FUNCTION;
    header.vendor_id = TEST_VENDOR;
    header.device_id = TEST_NVME_DEVICE_ID;
    header.base_class = PCI_CLASS_MASS_STORAGE;
    header.subclass = PCI_SUBCLASS_NVME;
    header.prog_if = PCI_PROGIF_NVME;
    if (valid_bar) {
        header.bars[0].present = true;
        header.bars[0].base = TEST_MMIO_BASE;
        header.bars[0].length = TEST_MMIO_LENGTH;
    }
    return header;
}

static void store_le16(unsigned char *bytes, uint16_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
}

static void store_le32(unsigned char *bytes, uint32_t value)
{
    bytes[0] = (unsigned char)(value & 0xFFU);
    bytes[1] = (unsigned char)((value >> 8U) & 0xFFU);
    bytes[2] = (unsigned char)((value >> 16U) & 0xFFU);
    bytes[3] = (unsigned char)((value >> 24U) & 0xFFU);
}

static void store_le64(unsigned char *bytes, uint64_t value)
{
    size_t index;

    for (index = 0U; index < 8U; ++index) {
        bytes[index] = (unsigned char)(value >> (8U * index));
    }
}

static void namespace_identify(
    unsigned char *identify,
    uint64_t size_lba,
    uint64_t capacity_lba,
    uint32_t lbads,
    uint32_t format_index);
static struct nvme_queue make_queue(
    uint32_t queue_id,
    uint32_t capacity);

static void test_binding(void)
{
    struct pci_device_header header;
    struct nvme_controller controller;

    memset(&controller, 0xA5, sizeof(controller));
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK &&
              controller.present &&
              controller.location.bus == TEST_BUS &&
              controller.location.device == TEST_DEVICE &&
              controller.vendor_id == TEST_VENDOR &&
              controller.mmio_base == TEST_MMIO_BASE,
          "NVMe PCI header with memory BAR binds exact controller");

    header = nvme_header(true);
    header.bars[0].is_64bit = true;
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK &&
              controller.mmio_base == TEST_MMIO_BASE,
          "NVMe PCI header with 64-bit memory BAR binds exact controller");

    header = nvme_header(true);
    header.base_class = 0x02U;
    CHECK(nvme_controller_bind(&header, &controller) ==
              NVME_UNSUPPORTED_DEVICE,
          "non-storage PCI device is rejected");
    header = nvme_header(false);
    CHECK(nvme_controller_bind(&header, &controller) ==
              NVME_MISSING_MEMORY_BAR,
          "NVMe device without memory BAR is rejected");
    CHECK(nvme_controller_bind(NULL, &controller) == NVME_NULL,
          "NVMe bind rejects null PCI header");
    CHECK(strcmp(nvme_status_string(NVME_OK), "NVME_OK") == 0,
          "NVMe OK status string is exact");
    CHECK(strcmp(nvme_status_string(NVME_TIMEOUT), "NVME_TIMEOUT") == 0,
          "NVMe timeout status string is exact");
}

static void test_controller_reset(void)
{
    struct mmio_fixture fixture;
    struct nvme_mmio_io io;
    struct pci_device_header header;
    struct nvme_controller controller;
    enum nvme_status status;

    fixture_init(&fixture, true);
    io = fixture_io(&fixture);
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "reset fixture binds valid controller");
    status = nvme_controller_reset(&io, &controller);
    CHECK(status == NVME_OK &&
              (controller.controller_config &
               (1U << NVME_CC_ENABLE_BIT)) == 0U &&
              ((controller.controller_config >>
                NVME_CC_IOSQES_SHIFT) & 0xFU) == 6U &&
              ((controller.controller_config >>
                NVME_CC_IOCQES_SHIFT) & 0xFU) == 4U &&
              (controller.controller_status & 1U) == 0U,
          "controller reset disables and publishes disabled controller state");

    fixture_init(&fixture, false);
    io = fixture_io(&fixture);
    status = nvme_controller_reset(&io, &controller);
    CHECK(status == NVME_OK &&
              (controller.controller_config &
               (1U << NVME_CC_ENABLE_BIT)) == 0U,
          "controller reset from not-ready state stays disabled");

    fixture_init(&fixture, false);
    fixture.fail_read = true;
    io = fixture_io(&fixture);
    status = nvme_controller_reset(&io, &controller);
    CHECK(status == NVME_IO_ERROR,
          "controller reset maps MMIO read failure to stable I/O error");
    fixture_init(&fixture, false);
    fixture.fail_write = true;
    io = fixture_io(&fixture);
    status = nvme_controller_reset(&io, &controller);
    CHECK(status == NVME_IO_ERROR,
          "controller reset maps MMIO write failure to stable I/O error");
}

static void test_controller_enable_after_admin_configure(void)
{
    struct mmio_fixture mmio;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue queue;
    struct pci_device_header header;
    struct nvme_controller controller;
    enum nvme_status status;

    fixture_init(&mmio, false);
    mmio_io = fixture_io(&mmio);
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "enable fixture binds valid controller");

    status = nvme_controller_reset(&mmio_io, &controller);
    CHECK(status == NVME_OK &&
              (controller.controller_config &
               (1U << NVME_CC_ENABLE_BIT)) == 0U,
          "controller is disabled before admin queue configuration");
    status = nvme_admin_queue_configure(&mmio_io, &queue);
    CHECK(status == NVME_OK &&
              queue.doorbell_stride == NVME_DOORBELL_STRIDE_MIN &&
              mmio.registers[NVME_REG_ADMIN_QUEUE_ATTRIBUTES_OFFSET / 4U] ==
                  UINT32_C(0x00010001) &&
              mmio.registers[NVME_REG_ADMIN_SUBMISSION_QUEUE_OFFSET / 4U] ==
                  (uint32_t)TEST_SUBMISSION_BASE &&
              mmio.registers[NVME_REG_ADMIN_COMPLETION_QUEUE_OFFSET / 4U] ==
                  (uint32_t)TEST_COMPLETION_BASE,
          "admin queue is configured while the controller is disabled");

    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_OK &&
              (controller.controller_config &
               (1U << NVME_CC_ENABLE_BIT)) != 0U &&
              (controller.controller_status & 1U) != 0U,
          "controller enable publishes ready state after admin queue setup");

    fixture_init(&mmio, false);
    mmio.ready_after_enable = false;
    mmio_io = fixture_io(&mmio);
    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_TIMEOUT,
          "controller enable reports timeout when ready is never observed");
}

/*
 * ITEM-429: the ready transition is waited for on the clock.
 *
 * The 2026-09-16 hardware run's NVMe bring-up ended in `NVME_TIMEOUT` on a
 * controller that is allowed half a second by the specification and is
 * instant under QEMU.  The driver waited on a loop count - a hundred reads,
 * microseconds - so a part that answers in a few hundred milliseconds is
 * indistinguishable from one that never answers.  These three cases pin the
 * contract: a late controller is waited for, a silent controller still ends at
 * the budget, and the wait the driver spent is published for the boot record.
 */
static void test_controller_ready_wait_uses_the_clock(void)
{
    struct mmio_fixture mmio;
    struct nvme_mmio_io mmio_io;
    struct pci_device_header header;
    struct nvme_controller controller;
    enum nvme_status status;

    fixture_init(&mmio, false);
    mmio.ready_after_enable = true;
    mmio.ready_delay_ms = 300U;
    mmio_io = fixture_io(&mmio);
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "clock fixture binds valid controller");
    CHECK(nvme_controller_reset(&mmio_io, &controller) == NVME_OK,
          "clock fixture resets with the part not yet ready");
    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_OK && mmio.fake_ms >= 300U &&
              (controller.controller_status & 1U) != 0U,
          "a controller ready after 300 ms is waited for, not timed out");
    CHECK(controller.ready_wait_ms >= 300U &&
              controller.ready_wait_ms <= NVME_CONTROLLER_READY_BUDGET_MS,
          "the ready wait publishes the milliseconds it spent");

    fixture_init(&mmio, false);
    mmio.ready_after_enable = true;
    mmio.ready_delay_ms = 300U;
    mmio.ready_never = true;
    mmio_io = fixture_io(&mmio);
    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_TIMEOUT &&
              mmio.fake_ms >= NVME_CONTROLLER_READY_BUDGET_MS &&
              mmio.fake_ms < NVME_CONTROLLER_READY_BUDGET_MS +
                                NVME_CONTROLLER_READY_STEP_MS,
          "a silent controller ends the wait at the budget, not earlier");
    CHECK(controller.ready_wait_ms == NVME_CONTROLLER_READY_BUDGET_MS,
          "a timed-out wait publishes the budget it spent");

    fixture_init(&mmio, true);
    /* The part was left enabled by the firmware: the clear is the wait. */
    mmio.config = 1U << NVME_CC_ENABLE_BIT;
    mmio.ready_clear_delay_ms = 300U;
    mmio_io = fixture_io(&mmio);
    status = nvme_controller_reset(&mmio_io, &controller);
    CHECK(status == NVME_OK && mmio.fake_ms >= 300U &&
              (controller.controller_status & 1U) == 0U,
          "a controller that clears ready after 300 ms is waited for");
}

/*
 * ITEM-429: a provider that never adopted the delay seam keeps the bounded
 * spin.  It is the path every existing caller was on before this item, and it
 * must stay bounded: the seam is how the wait grows a clock, not a requirement
 * a provider can be unbounded without.
 */
static void test_controller_ready_wait_without_a_seam(void)
{
    struct mmio_fixture mmio;
    struct nvme_mmio_io mmio_io;
    struct pci_device_header header;
    struct nvme_controller controller;
    enum nvme_status status;

    fixture_init(&mmio, false);
    mmio.ready_after_enable = true;
    mmio_io = fixture_io(&mmio);
    mmio_io.delay_ms = NULL;
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "no-seam fixture binds valid controller");
    CHECK(nvme_controller_reset(&mmio_io, &controller) == NVME_OK,
          "no-seam reset still reaches a controller that is not ready");
    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_OK && mmio.fake_ms == 0U,
          "a provider without the seam still binds an instantly ready part");

    fixture_init(&mmio, false);
    mmio.ready_never = true;
    mmio_io = fixture_io(&mmio);
    mmio_io.delay_ms = NULL;
    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_TIMEOUT && mmio.read_calls > 0U &&
              controller.ready_wait_ms == 0U,
          "without a seam the wait stays a bounded spin, not a clock");
}

static void test_controller_identify_parse(void)
{
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    struct nvme_controller_identity identity;

    memset(identify, 0, sizeof(identify));
    store_le16(identify + NVME_IDENTIFY_CONTROLLER_VID_OFFSET, TEST_VENDOR);
    identify[NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET] = 4U;
    memset(&identity, 0xA5, sizeof(identity));
    CHECK(nvme_controller_parse_identify(identify, &identity) == NVME_OK &&
              identity.valid &&
              identity.vendor_id == TEST_VENDOR &&
              identity.maximum_data_transfer_shift == 4U,
          "controller identify data publishes vendor and transfer bounds");
    CHECK(nvme_controller_parse_identify(NULL, &identity) == NVME_NULL,
          "controller identify parse rejects null data");
    CHECK(nvme_controller_parse_identify(identify, NULL) == NVME_NULL,
          "controller identify parse rejects null output");
}

static void test_admin_identify_discovery(void)
{
    struct mmio_fixture mmio;
    struct memory_fixture memory;
    struct nvme_queue_memory_io queue_io;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue queue;
    struct nvme_controller_identity controller_identity;
    struct nvme_namespace ns;
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    enum nvme_status status;

    memset(&memory, 0, sizeof(memory));
    memset(identify, 0, sizeof(identify));
    store_le16(identify + NVME_IDENTIFY_CONTROLLER_VID_OFFSET, TEST_VENDOR);
    identify[NVME_IDENTIFY_CONTROLLER_MDTS_OFFSET] = 4U;
    CHECK(memory_write(
              &memory, TEST_DATA_BASE, identify, sizeof(identify)),
          "controller identify fixture data can be published to DMA memory");

    fixture_init(&mmio, false);
    mmio.auto_memory = &memory;
    mmio.auto_complete = true;
    mmio.auto_completion_status = 0U;
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 4U);
    mmio.auto_queue = &queue;
    status = nvme_identify_controller(
        &mmio_io, &queue_io, &queue,
        TEST_DATA_BASE, &controller_identity);
    CHECK(status == NVME_OK &&
              controller_identity.valid &&
              controller_identity.vendor_id == TEST_VENDOR &&
              controller_identity.maximum_data_transfer_shift == 4U,
          "admin identify command discovers and parses controller identity");

    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        9U, 0U);
    CHECK(memory_write(
              &memory, TEST_DATA_BASE, identify, sizeof(identify)),
          "namespace identify fixture data can be published to DMA memory");
    fixture_init(&mmio, false);
    mmio.auto_memory = &memory;
    mmio.auto_complete = true;
    mmio.auto_completion_status = 0U;
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 4U);
    mmio.auto_queue = &queue;
    status = nvme_namespace_discover(
        &mmio_io, &queue_io, &queue, TEST_NSID,
        TEST_DATA_BASE, &ns);
    CHECK(status == NVME_OK &&
              ns.present &&
              ns.namespace_id == TEST_NSID &&
              ns.size_lba == UINT64_C(0x10000) &&
              ns.block_size == 512U,
          "admin identify command discovers and parses active namespace");
}

static void namespace_identify(
    unsigned char *identify,
    uint64_t size_lba,
    uint64_t capacity_lba,
    uint32_t lbads,
    uint32_t format_index)
{
    memset(identify, 0, NVME_IDENTIFY_DATA_SIZE);
    store_le64(
        identify + NVME_IDENTIFY_NAMESPACE_SIZE_OFFSET, size_lba);
    store_le64(
        identify + NVME_IDENTIFY_NAMESPACE_CAPACITY_OFFSET,
        capacity_lba);
    identify[26U] = (unsigned char)format_index;
    store_le32(
        identify + NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_OFFSET +
            format_index * NVME_IDENTIFY_NAMESPACE_LBA_FORMAT_SIZE,
        lbads << NVME_LBA_FORMAT_LBADS_SHIFT);
}

static void test_namespace_identify_parse(void)
{
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    struct nvme_namespace ns;

    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        9U, 0U);
    memset(&ns, 0xA5, sizeof(ns));
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_OK &&
              ns.present &&
              ns.namespace_id == TEST_NSID &&
              ns.size_lba == UINT64_C(0x10000) &&
              ns.capacity_lba == UINT64_C(0x20000) &&
              ns.block_size == 512U &&
              ns.lba_format_index == 0U,
          "valid namespace identify publishes exact 512-byte namespace");

    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        12U, 0U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_OK &&
              ns.block_size == 4096U,
          "valid 4096-byte namespace identify is accepted for adapter");

    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        7U, 0U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_UNSUPPORTED_BLOCK_SIZE,
          "unsupported 128-byte logical block is rejected");
    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        17U, 0U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_UNSUPPORTED_BLOCK_SIZE,
          "unsupported 128KiB logical block is rejected");

    namespace_identify(
        identify, UINT64_C(0x30000), UINT64_C(0x20000),
        9U, 0U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_BAD_IDENTIFY,
          "size greater than capacity is rejected");
    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        9U, 16U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_BAD_IDENTIFY,
          "out-of-range LBA format index is rejected");
    namespace_identify(
        identify, UINT64_C(0x10000), UINT64_C(0x20000),
        9U, 0U);
    CHECK(nvme_namespace_parse_identify(
              0U, identify, &ns) == NVME_BAD_ARGUMENT,
          "namespace ID zero is rejected");
}

static struct nvme_queue make_queue(
    uint32_t queue_id,
    uint32_t capacity)
{
    struct nvme_queue queue;
    const uint64_t submission_phys =
        queue_id == NVME_ADMIN_QUEUE_ID
            ? TEST_SUBMISSION_BASE
            : TEST_IO_SUBMISSION_BASE;
    const uint64_t completion_phys =
        queue_id == NVME_ADMIN_QUEUE_ID
            ? TEST_COMPLETION_BASE
            : TEST_IO_COMPLETION_BASE;

    CHECK(nvme_queue_bind(
              &queue, queue_id,
              submission_phys, completion_phys,
              capacity) == NVME_OK,
          "test queue bind succeeds");
    return queue;
}

static struct nvme_namespace test_namespace(uint64_t size_lba,
                                            uint32_t block_size)
{
    struct nvme_namespace ns;
    unsigned char identify[NVME_IDENTIFY_DATA_SIZE];
    unsigned int lbads = 0U;

    while ((1U << lbads) != block_size) {
        ++lbads;
    }
    namespace_identify(identify, size_lba, size_lba, lbads, 0U);
    CHECK(nvme_namespace_parse_identify(
              TEST_NSID, identify, &ns) == NVME_OK,
          "block test namespace identify parses");
    return ns;
}

static void read_queue_command(
    struct memory_fixture *memory,
    const struct nvme_queue *queue,
    uint32_t index,
    struct nvme_command *command)
{
    const uint64_t address =
        queue->submission_phys +
        (uint64_t)index * NVME_COMMAND_SIZE;

    CHECK(memory_read(
              memory, address, command, sizeof(*command)),
          "test can read queued NVMe command");
}

static void test_queue_binding_and_command_submission(void)
{
    struct mmio_fixture mmio;
    struct memory_fixture memory;
    struct nvme_queue_memory_io queue_io;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue queue;
    struct nvme_command command;
    struct nvme_completion completion;
    enum nvme_status status;

    CHECK(nvme_queue_bind(NULL, 0U, TEST_SUBMISSION_BASE,
                          TEST_COMPLETION_BASE, 2U) == NVME_NULL,
          "queue bind rejects null queue");
    CHECK(nvme_queue_bind(
              &queue, 0U, TEST_SUBMISSION_BASE + 512U,
              TEST_COMPLETION_BASE, 2U) == NVME_BAD_ALIGNMENT,
          "queue bind rejects unaligned submission memory");
    CHECK(nvme_queue_bind(
              &queue, 0U, TEST_SUBMISSION_BASE,
              TEST_COMPLETION_BASE, 0U) == NVME_BAD_ARGUMENT,
          "queue bind rejects zero capacity");
    CHECK(nvme_queue_bind(
              &queue, 0U, TEST_SUBMISSION_BASE,
              TEST_COMPLETION_BASE, NVME_QUEUE_CAPACITY_MAX + 1U) ==
              NVME_BAD_ARGUMENT,
          "queue bind rejects over-limit capacity");

    memset(&mmio, 0, sizeof(mmio));
    fixture_init(&mmio, false);
    memset(&memory, 0, sizeof(memory));
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    memset(&command, 0xA5, sizeof(command));
    status = nvme_command_identify_build(
        0U, NVME_IDENTIFY_CNS_CONTROLLER, TEST_DATA_BASE,
        &queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) == NVME_OPCODE_IDENTIFY &&
              ((command.dwords[0] >>
                NVME_CDW0_CID_SHIFT) & NVME_CDW0_CID_MASK) == 1U &&
              command.dwords[6] == (uint32_t)TEST_DATA_BASE,
          "identify command is built with exact opcode, CID, and PRP");

    status = nvme_queue_submit_command(
        &mmio_io, &queue_io, &queue, &command);
    CHECK(status == NVME_OK &&
              mmio.registers[NVME_DOORBELL_BASE_OFFSET / 4U] == 1U &&
              queue.submission_tail == 1U &&
              queue.outstanding == 1U &&
              queue.next_command_id == 2U,
          "queue submission publishes command and SQ tail doorbell");

    read_queue_command(&memory, &queue, 0U, &command);
    CHECK((command.dwords[0] & 0xFFU) == NVME_OPCODE_IDENTIFY,
          "queued submission memory contains identify command");

    memset(&completion, 0xA5, sizeof(completion));
    status = nvme_queue_poll_completion(
        &queue_io, &mmio_io, &queue, &completion);
    CHECK(status == NVME_TIMEOUT,
          "completion poll times out before device completion");
    fixture_complete_command(&memory, &queue, 1U, 0U);
    status = nvme_queue_poll_completion(
        &queue_io, &mmio_io, &queue, &completion);
    CHECK(status == NVME_OK &&
              ((completion.dwords[3] >>
                NVME_COMPLETION_DWORD3_CID_SHIFT) &
               NVME_CDW0_CID_MASK) == 1U &&
              queue.outstanding == 0U &&
              queue.completion_head == 1U,
          "valid completion is consumed and command ID is exact");
    CHECK(nvme_completion_status(&completion) == NVME_OK,
          "successful completion status is exact");

    queue = make_queue(TEST_IO_QUEUE_ID, 2U);
    queue.io_queue_created = true;
    status = nvme_command_identify_build(
        0U, NVME_IDENTIFY_CNS_CONTROLLER, TEST_DATA_BASE,
        &queue, &command);
    CHECK(status == NVME_OK,
          "I/O queue command fixture builds identify command");
    status = nvme_queue_submit_command(
        &mmio_io, &queue_io, &queue, &command);
    CHECK(status == NVME_OK &&
              mmio.registers[
                  (NVME_DOORBELL_BASE_OFFSET +
                   2U * TEST_IO_QUEUE_ID *
                       NVME_DOORBELL_STRIDE_MIN) /
                  4U] == 1U &&
              queue.submission_tail == 1U,
          "I/O queue submission publishes its SQ tail doorbell");
}

static void test_block_command_build_and_bounds(void)
{
    struct nvme_queue queue;
    struct nvme_namespace ns;
    struct nvme_command command;
    enum nvme_status status;

    queue = make_queue(NVME_ADMIN_QUEUE_ID, 4U);
    ns = test_namespace(UINT64_C(0x10000), 512U);

    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, UINT64_C(0x1234), 1U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) == NVME_OPCODE_READ &&
              command.dwords[1] == TEST_NSID &&
              command.dwords[10] == UINT64_C(0x1234) &&
              command.dwords[12] == 0U,
          "single-block read command has exact NSID, LBA, and NLB");

    status = nvme_command_io_build(
        NVME_OPCODE_WRITE, &ns, 0U, 8U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) == NVME_OPCODE_WRITE &&
              command.dwords[12] == 7U,
          "bounded multi-block write command publishes zero-based NLB");

    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, UINT64_C(0xFFFF), 2U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_BAD_RANGE,
          "out-of-range block request is rejected before submission");
    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, 0U, 9U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_BAD_RANGE,
          "over-limit transfer length is rejected before submission");
    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, 0U, 1U,
        TEST_DATA_BASE + 512U, &queue, &command);
    CHECK(status == NVME_BAD_ALIGNMENT,
          "unaligned DMA data address is rejected before submission");
    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, 0U, 0U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_BAD_ARGUMENT,
          "zero block count is rejected before submission");

    status = nvme_command_flush_build(
        &ns, &queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) == NVME_OPCODE_FLUSH &&
              command.dwords[1] == TEST_NSID,
          "flush command has exact opcode and namespace ID");

    memset(&queue, 0, sizeof(queue));
    status = nvme_command_io_build(
        NVME_OPCODE_READ, &ns, 0U, 1U,
        TEST_DATA_BASE, &queue, &command);
    CHECK(status == NVME_QUEUE_NOT_READY,
          "I/O command build rejects queue that was never bound");
}

static void test_io_queue_command_build(void)
{
    struct nvme_queue admin_queue;
    struct nvme_queue io_queue;
    struct nvme_command command;
    enum nvme_status status;

    admin_queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    io_queue = make_queue(TEST_IO_QUEUE_ID, 4U);
    status = nvme_command_create_io_cq_build(
        &io_queue, &admin_queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) ==
                  NVME_OPCODE_CREATE_IO_CQ &&
              command.dwords[6] == (uint32_t)TEST_IO_COMPLETION_BASE &&
              (command.dwords[10] & NVME_CDW10_QID_MASK) ==
                  TEST_IO_QUEUE_ID &&
              ((command.dwords[10] >> NVME_CDW10_QSIZE_SHIFT) &
               NVME_CDW10_QSIZE_MASK) == 3U &&
              (command.dwords[11] & (1U << NVME_CDW11_PC_BIT)) != 0U,
          "create I/O CQ command has exact QID, QSIZE, PRP, and PC");

    status = nvme_command_create_io_sq_build(
        &io_queue, &admin_queue, &command);
    CHECK(status == NVME_OK &&
              (command.dwords[0] & 0xFFU) ==
                  NVME_OPCODE_CREATE_IO_SQ &&
              command.dwords[6] == (uint32_t)TEST_IO_SUBMISSION_BASE &&
              (command.dwords[10] & NVME_CDW10_QID_MASK) ==
                  TEST_IO_QUEUE_ID &&
              ((command.dwords[10] >> NVME_CDW10_QSIZE_SHIFT) &
               NVME_CDW10_QSIZE_MASK) == 3U &&
              ((command.dwords[11] >> NVME_CDW11_CQID_SHIFT) &
               NVME_CDW10_QID_MASK) == TEST_IO_QUEUE_ID,
          "create I/O SQ command targets its completion queue");

    admin_queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    io_queue = make_queue(NVME_ADMIN_QUEUE_ID, 4U);
    CHECK(nvme_command_create_io_cq_build(
              &io_queue, &admin_queue, &command) == NVME_BAD_ARGUMENT,
          "create I/O CQ rejects the admin queue as the I/O queue");
}

static void test_queue_failure_and_ordering(void)
{
    struct mmio_fixture mmio;
    struct memory_fixture memory;
    struct nvme_queue_memory_io queue_io;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue queue;
    struct nvme_namespace ns;
    struct nvme_command first;
    struct nvme_command second;
    struct nvme_completion completion;
    enum nvme_status status;

    fixture_init(&mmio, false);
    memset(&memory, 0, sizeof(memory));
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 1U);
    ns = test_namespace(UINT64_C(0x10000), 512U);
    CHECK(nvme_command_io_build(
              NVME_OPCODE_READ, &ns, 0U, 1U,
              TEST_DATA_BASE, &queue, &first) == NVME_OK,
          "first queue command builds for full-queue fixture");
    CHECK(nvme_queue_submit_command(
              &mmio_io, &queue_io, &queue, &first) == NVME_OK,
          "first queue command submits into capacity-one queue");
    CHECK(nvme_command_io_build(
              NVME_OPCODE_WRITE, &ns, 0U, 1U,
              TEST_DATA_BASE, &queue, &second) == NVME_OK,
          "second command builds before queue fullness is checked");
    CHECK(nvme_queue_submit_command(
              &mmio_io, &queue_io, &queue, &second) == NVME_QUEUE_FULL,
          "full queue rejects additional submission");

    fixture_complete_command(&memory, &queue, 1U, 0x4U);
    status = nvme_queue_poll_completion(
        &queue_io, &mmio_io, &queue, &completion);
    CHECK(status == NVME_FAILED_COMPLETION,
          "nonzero NVMe completion status is mapped to stable failure");
    CHECK(nvme_completion_status(&completion) ==
              NVME_FAILED_COMPLETION,
          "completed failure status helper is exact");

    memset(&memory, 0, sizeof(memory));
    queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    CHECK(nvme_command_io_build(
              NVME_OPCODE_READ, &ns, 0U, 1U,
              TEST_DATA_BASE, &queue, &first) == NVME_OK &&
              ((first.dwords[0] >>
                NVME_CDW0_CID_SHIFT) & NVME_CDW0_CID_MASK) == 1U,
          "ordering fixture builds first command with CID 1");
    CHECK(nvme_queue_submit_command(
              &mmio_io, &queue_io, &queue, &first) == NVME_OK,
          "ordering fixture submits first command");
    CHECK(nvme_command_flush_build(
              &ns, &queue, &second) == NVME_OK &&
              ((second.dwords[0] >>
                NVME_CDW0_CID_SHIFT) & NVME_CDW0_CID_MASK) == 2U,
          "ordering fixture builds second command with CID 2");
    CHECK(nvme_queue_submit_command(
              &mmio_io, &queue_io, &queue, &second) == NVME_OK,
          "ordering fixture submits second command");
    fixture_complete_slot(&memory, &queue, 0U, 1U, 0U);
    fixture_complete_slot(&memory, &queue, 1U, 2U, 0U);
    status = nvme_queue_poll_completion(
        &queue_io, &mmio_io, &queue, &completion);
    CHECK(status == NVME_OK &&
              ((completion.dwords[3] >>
                NVME_COMPLETION_DWORD3_CID_SHIFT) &
               NVME_CDW0_CID_MASK) == 1U,
          "first completion is consumed before second");
    status = nvme_queue_poll_completion(
        &queue_io, &mmio_io, &queue, &completion);
    CHECK(status == NVME_OK &&
              ((completion.dwords[3] >>
                NVME_COMPLETION_DWORD3_CID_SHIFT) &
               NVME_CDW0_CID_MASK) == 2U &&
              queue.phase == 0U,
          "second completion is consumed and queue phase wraps");
}

static void test_admin_configure_and_io_wrappers(void)
{
    struct mmio_fixture mmio;
    struct memory_fixture memory;
    struct nvme_queue_memory_io queue_io;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue admin_queue;
    struct nvme_queue io_queue;
    struct pci_device_header header;
    struct nvme_controller controller;
    struct nvme_namespace ns;
    enum nvme_status status;

    fixture_init(&mmio, false);
    memset(&memory, 0, sizeof(memory));
    mmio.auto_memory = &memory;
    mmio.auto_complete = true;
    mmio.auto_completion_status = 0U;
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    admin_queue = make_queue(NVME_ADMIN_QUEUE_ID, 2U);
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "I/O wrapper fixture binds valid controller");
    CHECK(nvme_controller_reset(&mmio_io, &controller) == NVME_OK,
          "I/O wrapper fixture resets controller to disabled state");
    status = nvme_admin_queue_configure(&mmio_io, &admin_queue);
    CHECK(status == NVME_OK &&
              mmio.registers[NVME_REG_ADMIN_QUEUE_ATTRIBUTES_OFFSET / 4U] ==
                  UINT32_C(0x00010001) &&
              mmio.registers[NVME_REG_ADMIN_SUBMISSION_QUEUE_OFFSET / 4U] ==
                  (uint32_t)TEST_SUBMISSION_BASE &&
              mmio.registers[NVME_REG_ADMIN_COMPLETION_QUEUE_OFFSET / 4U] ==
                  (uint32_t)TEST_COMPLETION_BASE,
          "admin queue configure publishes AQA and queue physical addresses");

    status = nvme_controller_enable(&mmio_io, &controller);
    CHECK(status == NVME_OK,
          "controller enables after admin queue configuration");
    status = nvme_admin_queue_configure(&mmio_io, &admin_queue);
    CHECK(status == NVME_BAD_STATE,
          "admin queue configure rejects an enabled controller");

    io_queue = make_queue(TEST_IO_QUEUE_ID, 4U);
    mmio.auto_queue = &admin_queue;
    mmio.auto_completion_count = 0U;
    status = nvme_io_queue_configure(
        &mmio_io, &queue_io, &admin_queue, &io_queue);
    CHECK(status == NVME_OK &&
              io_queue.io_queue_created &&
              mmio.auto_completion_count == 2U,
          "I/O completion and submission queues are created through admin queue");

    ns = test_namespace(UINT64_C(0x10000), 512U);
    mmio.auto_queue = &io_queue;
    status = nvme_namespace_read_blocks(
        &mmio_io, &queue_io, &io_queue, &ns,
        0U, 1U, TEST_DATA_BASE);
    CHECK(status == NVME_OK &&
              io_queue.outstanding == 0U &&
              io_queue.completion_head == 1U,
          "bounded read block wrapper consumes successful completion");

    status = nvme_namespace_write_blocks(
        &mmio_io, &queue_io, &io_queue, &ns,
        0U, 1U, TEST_DATA_BASE);
    CHECK(status == NVME_OK,
          "bounded write block wrapper consumes successful completion");

    status = nvme_namespace_flush(
        &mmio_io, &queue_io, &io_queue, &ns);
    CHECK(status == NVME_OK,
          "flush wrapper consumes successful completion");

    status = nvme_namespace_read_blocks(
        &mmio_io, &queue_io, &admin_queue, &ns,
        0U, 1U, TEST_DATA_BASE);
    CHECK(status == NVME_QUEUE_NOT_READY,
          "block I/O wrapper rejects the admin queue");

    io_queue = make_queue(TEST_IO_QUEUE_ID, 4U);
    mmio.auto_queue = &admin_queue;
    mmio.auto_completion_status = 0U;
    CHECK(nvme_io_queue_configure(
              &mmio_io, &queue_io, &admin_queue, &io_queue) == NVME_OK,
          "second I/O queue is configured for error mapping");
    mmio.auto_queue = &io_queue;
    mmio.auto_completion_status = 0x4U;
    status = nvme_namespace_read_blocks(
        &mmio_io, &queue_io, &io_queue, &ns,
        0U, 1U, TEST_DATA_BASE);
    CHECK(status == NVME_FAILED_COMPLETION,
          "read block wrapper maps controller error completion to failure");
}

static void test_nvme_block_device_adapter(void)
{
    struct mmio_fixture mmio;
    struct memory_fixture memory;
    struct nvme_queue_memory_io queue_io;
    struct nvme_mmio_io mmio_io;
    struct nvme_queue admin_queue;
    struct nvme_queue io_queue;
    struct pci_device_header header;
    struct nvme_controller controller;
    struct nvme_namespace ns;
    struct nvme_block_device backend;
    unsigned char buffer[512U];
    unsigned char write_data[512U];
    unsigned char staged[512U];
    enum nvme_status status;

    memset(&memory, 0, sizeof(memory));
    memset(buffer, 0xA5, sizeof(buffer));
    memset(staged, 0, sizeof(staged));
    memset(memory.storage, 0x77, sizeof(memory.storage));
    fixture_init(&mmio, false);
    mmio.auto_memory = &memory;
    mmio.auto_complete = true;
    mmio.auto_completion_status = 0U;
    queue_io = memory_io(&memory);
    mmio_io = fixture_io(&mmio);
    admin_queue = make_queue(NVME_ADMIN_QUEUE_ID, 8U);
    io_queue = make_queue(TEST_IO_QUEUE_ID, 8U);
    header = nvme_header(true);
    CHECK(nvme_controller_bind(&header, &controller) == NVME_OK,
          "block adapter fixture binds valid controller");
    CHECK(nvme_controller_reset(&mmio_io, &controller) == NVME_OK &&
              nvme_admin_queue_configure(&mmio_io, &admin_queue) == NVME_OK &&
              nvme_controller_enable(&mmio_io, &controller) == NVME_OK,
          "block adapter fixture configures and enables the admin queue");
    mmio.auto_queue = &admin_queue;
    CHECK(nvme_io_queue_configure(
              &mmio_io, &queue_io, &admin_queue, &io_queue) == NVME_OK,
          "block adapter fixture creates the I/O queue");
    mmio.auto_queue = &io_queue;
    ns = test_namespace(UINT64_C(0x100), 512U);

    status = nvme_namespace_bind_block_device(
        &mmio_io, &queue_io, &io_queue, &ns,
        TEST_DATA_BASE, &backend);
    CHECK(status == NVME_OK &&
              backend.device.block_size == 512U &&
              backend.device.block_count == UINT64_C(0x100) &&
              backend.device.flush_fn != NULL,
          "NVMe namespace binds as a 512-byte HFOC block device");

    CHECK(block_device_read(
              &backend.device, 0U, buffer, sizeof(buffer)) ==
              BLOCK_DEVICE_OK &&
              all_bytes(buffer, sizeof(buffer), 0x77U),
          "NVMe-backed block device read publishes exact data");

    memset(write_data, 0x44, sizeof(write_data));
    CHECK(block_device_write(
              &backend.device, 512U, write_data, sizeof(write_data)) ==
              BLOCK_DEVICE_OK,
          "NVMe-backed block device write publishes through DMA staging");
    CHECK(memory_read(
              &memory, TEST_DATA_BASE, staged, sizeof(staged)) &&
              all_bytes(staged, sizeof(staged), 0x44U),
          "NVMe-backed write is observable in DMA staging memory");
    CHECK(block_device_flush(&backend.device) == BLOCK_DEVICE_OK,
          "NVMe-backed block device flush reaches namespace flush");
}

int main(void)
{
    test_binding();
    test_controller_reset();
    test_controller_enable_after_admin_configure();
    test_controller_ready_wait_uses_the_clock();
    test_controller_ready_wait_without_a_seam();
    test_controller_identify_parse();
    test_admin_identify_discovery();
    test_namespace_identify_parse();
    test_queue_binding_and_command_submission();
    test_block_command_build_and_bounds();
    test_io_queue_command_build();
    test_queue_failure_and_ordering();
    test_admin_configure_and_io_wrappers();
    test_nvme_block_device_adapter();

    if (failures != 0) {
        fprintf(stderr, "NVMe host tests: %d FAILURES\n", failures);
        return 1;
    }
    puts("NVMe host tests: ALL OK");
    return 0;
}

#endif
