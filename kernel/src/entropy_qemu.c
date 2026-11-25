#ifdef HFOC_ENTROPY_QEMU_TEST

#include "arch/x86_64/io.h"
#include "entropy_accumulator.h"
#include "entropy_diagnostics.h"
#include "entropy_health.h"
#include "entropy_restart.h"
#include "entropy_seed_fill.h"
#include "pmm.h"
#include "serial.h"
#include "virtio_available_publication.h"
#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_pci_capability.h"
#include "virtio_pci_device_status.h"
#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"
#include "virtio_queue_layout.h"
#include "virtio_rng_completion.h"
#include "virtio_rng_feature_profile.h"
#include "virtio_rng_queue_composition.h"
#include "virtio_rng_request.h"
#include "virtio_used_consumption.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define EQE_PROOF_STACK_SIZE 131072U
#define EQE_QUEUE_SIZE 64U
#define EQE_DMA_CAPACITY 64U
#define EQE_QUEUE_REGION_LENGTH 4096U
#define EQE_BLOCK_COUNT 32U
#define EQE_BLOCK_LENGTH 32U
#define EQE_DEST_OFFSET (EQE_BLOCK_COUNT * EQE_BLOCK_LENGTH)
#define EQE_PAYLOAD_LENGTH (EQE_BLOCK_COUNT * EQE_BLOCK_LENGTH * 2U)
#define EQE_PAYLOAD_PAGES 1U
#define EQE_GENERATION 1U
#define EQE_RECOVERY_GENERATION 2U
#define EQE_SOURCE_ID 260U
#define EQE_BAR_LENGTH UINT64_C(0x10000)

#define EQE_PCI_CONFIG_ADDRESS_PORT 0xCF8U
#define EQE_PCI_CONFIG_DATA_PORT 0xCFCU
#define EQE_PCI_CONFIG_ENABLE UINT32_C(0x80000000)
#define EQE_PCI_BAR_COUNT 6U

#define EQE_COMMON_DEVICE_FEATURE_SELECT 0x00U
#define EQE_COMMON_DEVICE_FEATURE 0x04U
#define EQE_COMMON_DRIVER_FEATURE_SELECT 0x08U
#define EQE_COMMON_DRIVER_FEATURE 0x0CU
#define EQE_COMMON_DEVICE_STATUS 0x14U
#define EQE_COMMON_QUEUE_SELECT 0x16U
#define EQE_COMMON_QUEUE_SIZE 0x18U
#define EQE_COMMON_QUEUE_ENABLE 0x1CU
#define EQE_COMMON_QUEUE_NOTIFY_OFF 0x1EU
#define EQE_COMMON_QUEUE_DESC 0x20U
#define EQE_COMMON_QUEUE_DRIVER 0x28U
#define EQE_COMMON_QUEUE_DEVICE 0x30U

#define EQE_DESCRIPTOR_FLAG_WRITE 2U

struct eqe_virtq_desc {
    volatile uint64_t addr;
    volatile uint32_t len;
    volatile uint16_t flags;
    volatile uint16_t next;
};

struct eqe_queue_mem {
    uint64_t physical;
    void *virtual_address;
    struct virtio_queue_layout_result layout;
    struct virtio_available_ring *available;
    struct virtio_used_ring *used;
    volatile struct eqe_virtq_desc *descriptors;
};

static uint64_t eqe_hhdm;
static volatile uint8_t *eqe_common_base;
static volatile uint8_t *eqe_notify_base;
static uint32_t eqe_notify_multiplier;
static uint32_t eqe_notify_off;
static uint64_t eqe_queue_physical;
static uint64_t eqe_payload_physical;
static void *eqe_payload_virtual;
static struct eqe_queue_mem eqe_queue;
static int eqe_io_context;
static uint8_t eqe_proof_stack[EQE_PROOF_STACK_SIZE]
    __attribute__((aligned(16)));

static const char eqe_leak_sentinel[] = "HFOC-ENTROPY-QEMU-SECRET";

static void eqe_fail(const char *message) __attribute__((noreturn));

static void eqe_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[ENTROPY] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void eqe_require(bool condition, const char *message)
{
    if (!condition) {
        eqe_fail(message);
    }
}

static void eqe_zero_bytes(void *destination, size_t length)
{
    volatile uint8_t *bytes = (volatile uint8_t *)destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static void eqe_copy_bytes(
    void *destination,
    const void *source,
    size_t length)
{
    uint8_t *destination_bytes = (uint8_t *)destination;
    const uint8_t *source_bytes = (const uint8_t *)source;
    size_t index;

    for (index = 0U; index < length; ++index) {
        destination_bytes[index] = source_bytes[index];
    }
}

static void eqe_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t eqe_inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t eqe_pci_config_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset)
{
    const uint32_t address =
        EQE_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    eqe_outl(EQE_PCI_CONFIG_ADDRESS_PORT, address);
    return eqe_inl(EQE_PCI_CONFIG_DATA_PORT);
}

static uint16_t eqe_pci_config_read16(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t offset)
{
    const uint32_t value =
        eqe_pci_config_read32(bus, device, function, offset);

    return (uint16_t)((value >> ((offset & 3U) * 8U)) & 0xFFFFU);
}

static void eqe_pci_config_read_space(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t *config)
{
    uint32_t offset;

    for (offset = 0U; offset < 256U; offset += 4U) {
        const uint32_t value =
            eqe_pci_config_read32(bus, device, function, (uint8_t)offset);

        config[offset] = (uint8_t)(value & 0xFFU);
        config[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
        config[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
        config[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
    }
}

static uint32_t eqe_mmio_read32(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static uint16_t eqe_mmio_read16(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint16_t *)(base + offset);
}

static uint8_t eqe_mmio_read8(
    volatile uint8_t *base,
    uint32_t offset)
{
    return *(volatile uint8_t *)(base + offset);
}

static void eqe_mmio_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void eqe_mmio_write16(
    volatile uint8_t *base,
    uint32_t offset,
    uint16_t value)
{
    *(volatile uint16_t *)(base + offset) = value;
}

static void eqe_mmio_write8(
    volatile uint8_t *base,
    uint32_t offset,
    uint8_t value)
{
    *(volatile uint8_t *)(base + offset) = value;
}

static void eqe_mmio_write64(
    volatile uint8_t *base,
    uint32_t offset,
    uint64_t value)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static bool eqe_status_write(void *context, uint8_t value)
{
    (void)context;
    eqe_mmio_write8(eqe_common_base, EQE_COMMON_DEVICE_STATUS, value);
    return true;
}

static bool eqe_status_read(void *context, uint8_t *value)
{
    (void)context;
    *value = eqe_mmio_read8(eqe_common_base, EQE_COMMON_DEVICE_STATUS);
    return true;
}

static bool eqe_feature_read_offered(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    eqe_mmio_write32(
        eqe_common_base, EQE_COMMON_DEVICE_FEATURE_SELECT, select);
    *value = eqe_mmio_read32(eqe_common_base, EQE_COMMON_DEVICE_FEATURE);
    return true;
}

static bool eqe_feature_write_accepted(
    void *context,
    uint32_t select,
    uint32_t value)
{
    (void)context;
    eqe_mmio_write32(
        eqe_common_base, EQE_COMMON_DRIVER_FEATURE_SELECT, select);
    eqe_mmio_write32(eqe_common_base, EQE_COMMON_DRIVER_FEATURE, value);
    return true;
}

static bool eqe_feature_read_accepted(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    eqe_mmio_write32(
        eqe_common_base, EQE_COMMON_DRIVER_FEATURE_SELECT, select);
    *value = eqe_mmio_read32(eqe_common_base, EQE_COMMON_DRIVER_FEATURE);
    return true;
}

static void eqe_barrier_fence(void *context)
{
    (void)context;
    __asm__ volatile ("" : : : "memory");
}

static bool eqe_queue_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    (void)context;
    (void)element;
    (void)token;
    return true;
}

static bool eqe_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    (void)context;
    eqe_zero_bytes(
        (void *)(uintptr_t)(eqe_hhdm + physical_base), (size_t)length);
    return true;
}

static bool eqe_find_rng_device(
    uint8_t *device_out,
    uint8_t *function_out,
    uint16_t *vendor_out,
    uint16_t *device_id_out)
{
    uint8_t device;

    for (device = 0U; device < 32U; ++device) {
        const uint16_t vendor =
            eqe_pci_config_read16(0U, device, 0U, 0U);
        const uint16_t device_id =
            eqe_pci_config_read16(0U, device, 0U, 2U);

        if (vendor == 0xFFFFU) {
            continue;
        }
        if (vendor == VIRTIO_PCI_VENDOR_ID &&
            (device_id == VIRTIO_PCI_DEVICE_ID_RNG_MODERN ||
             device_id == VIRTIO_PCI_DEVICE_ID_RNG_TRANSITIONAL)) {
            *device_out = device;
            *function_out = 0U;
            *vendor_out = vendor;
            *device_id_out = device_id;
            return true;
        }
    }
    return false;
}

static void eqe_make_bar_ranges(
    uint8_t pci_device,
    struct virtio_pci_bar_range ranges[EQE_PCI_BAR_COUNT])
{
    uint32_t bar_index;

    eqe_zero_bytes(ranges, sizeof(*ranges) * EQE_PCI_BAR_COUNT);
    for (bar_index = 0U; bar_index < EQE_PCI_BAR_COUNT; ++bar_index) {
        const uint32_t raw =
            eqe_pci_config_read32(
                0U, pci_device, 0U,
                (uint8_t)(0x10U + bar_index * 4U));
        uint64_t base;
        bool is_64;

        if (raw == 0U || (raw & 1U) != 0U) {
            continue;
        }
        base = (uint64_t)(raw & ~0xFU);
        is_64 = (raw & 0x6U) == 0x4U;
        ranges[bar_index].present = true;
        ranges[bar_index].base = base;
        ranges[bar_index].length = EQE_BAR_LENGTH;
        if (is_64 && bar_index + 1U < EQE_PCI_BAR_COUNT) {
            const uint32_t high =
                eqe_pci_config_read32(
                    0U, pci_device, 0U,
                    (uint8_t)(0x10U + (bar_index + 1U) * 4U));

            base |= (uint64_t)high << 32U;
            ranges[bar_index].base = base;
            ++bar_index;
        }
    }
}

static volatile uint8_t *eqe_map_bar(uint64_t physical_base)
{
    volatile void *mapped = NULL;
    const enum vmm_status status = vmm_map_mmio_page(
        physical_base & ~0xFFFULL, eqe_hhdm, &mapped);

    eqe_require(status == VMM_OK, "entropy BAR MMIO mapping");
    return (volatile uint8_t *)(uintptr_t)(
        (uintptr_t)mapped + (physical_base & 0xFFFULL));
}

static void eqe_write_descriptor(
    struct eqe_queue_mem *queue,
    uint32_t index,
    uint64_t address,
    uint32_t length,
    uint16_t flags,
    uint16_t next)
{
    volatile struct eqe_virtq_desc *descriptor =
        &queue->descriptors[index];

    descriptor->addr = address;
    descriptor->len = length;
    descriptor->flags = flags;
    descriptor->next = next;
}

static void eqe_program_queue(void)
{
    eqe_mmio_write16(eqe_common_base, EQE_COMMON_QUEUE_SELECT, 0U);
    eqe_require(
        eqe_mmio_read16(eqe_common_base, EQE_COMMON_QUEUE_SELECT) == 0U,
        "entropy queue select readback");
    eqe_notify_off =
        eqe_mmio_read16(eqe_common_base, EQE_COMMON_QUEUE_NOTIFY_OFF);
    eqe_mmio_write16(
        eqe_common_base, EQE_COMMON_QUEUE_SIZE,
        (uint16_t)eqe_queue.layout.queue_size);
    eqe_require(
        eqe_mmio_read16(eqe_common_base, EQE_COMMON_QUEUE_SIZE) ==
            eqe_queue.layout.queue_size,
        "entropy queue size readback");
    eqe_mmio_write64(
        eqe_common_base, EQE_COMMON_QUEUE_DESC,
        eqe_queue.layout.region_base +
            eqe_queue.layout.descriptor.offset);
    eqe_mmio_write64(
        eqe_common_base, EQE_COMMON_QUEUE_DRIVER,
        eqe_queue.layout.region_base +
            eqe_queue.layout.available.offset);
    eqe_mmio_write64(
        eqe_common_base, EQE_COMMON_QUEUE_DEVICE,
        eqe_queue.layout.region_base +
            eqe_queue.layout.used.offset);
    eqe_mmio_write16(eqe_common_base, EQE_COMMON_QUEUE_ENABLE, 1U);
    eqe_require(
        eqe_mmio_read16(eqe_common_base, EQE_COMMON_QUEUE_ENABLE) == 1U,
        "entropy queue enable readback");
}

static void eqe_notify_queue(uint16_t value)
{
    volatile uint16_t *notify_address = (volatile uint16_t *)(
        eqe_notify_base +
        eqe_notify_off * eqe_notify_multiplier);

    *notify_address = value;
}

static void eqe_wait_used_count(
    const struct virtio_used_ring *ring,
    uint32_t count)
{
    for (;;) {
        if ((uint32_t)ring->idx >= count) {
            return;
        }
        __asm__ volatile ("pause" : : : "memory");
    }
}

static void eqe_read_block(
    struct virtio_rng_queue_composition *composition,
    struct virtio_rng_request_api *api,
    uint32_t block_index,
    uint32_t sequence)
{
    struct virtio_rng_queue *queue = &composition->queue;
    struct virtio_descriptor_token descriptor;
    struct virtio_available_token available_token;
    struct virtio_rng_request request;
    struct virtio_rng_completion_result completion;
    uint16_t chain[1];
    uint8_t *destination =
        (uint8_t *)eqe_payload_virtual + EQE_DEST_OFFSET +
        block_index * EQE_BLOCK_LENGTH;
    const void *device_payload =
        (const uint8_t *)eqe_payload_virtual +
        block_index * EQE_BLOCK_LENGTH;

    eqe_require(
        block_index < EQE_BLOCK_COUNT &&
            sequence < EQE_BLOCK_COUNT * 2U,
        "entropy read bounds");
    eqe_require(
        virtio_descriptor_ownership_reserve(
            &queue->descriptors, &descriptor) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
        "entropy descriptor reserve");
    eqe_require(
        virtio_descriptor_ownership_publish(
            &queue->descriptors, &descriptor) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK,
        "entropy descriptor publish");

    chain[0] = (uint16_t)descriptor.index;
    eqe_require(
        virtio_available_publication_prepare(
            &queue->available, chain, 1U, &available_token) ==
            VIRTIO_AVAILABLE_PUBLICATION_OK,
        "entropy available prepare");
    eqe_require(
        virtio_available_publication_publish(
            &queue->available, &available_token) ==
            VIRTIO_AVAILABLE_PUBLICATION_OK,
        "entropy available publish");
    eqe_write_descriptor(
        &eqe_queue,
        descriptor.index,
        eqe_payload_physical + block_index * EQE_BLOCK_LENGTH,
        EQE_BLOCK_LENGTH,
        EQE_DESCRIPTOR_FLAG_WRITE,
        0U);

    eqe_zero_bytes(&request, sizeof(request));
    request.version = VIRTIO_RNG_REQUEST_VERSION;
    request.destination = destination;
    request.destination_length = EQE_BLOCK_LENGTH;
    request.context.kind = VIRTIO_RNG_CALLER_CONTEXT_KERNEL;
    request.context.id = EQE_SOURCE_ID;
    request.generation = api->generation;
    request.result = VIRTIO_RNG_REQUEST_RESULT_PENDING;
    eqe_require(
        virtio_rng_request_api_submit(api, &request) ==
            VIRTIO_RNG_REQUEST_API_OK,
        "entropy request submit");

    eqe_notify_queue(
        (uint16_t)virtio_available_publication_visible_index(
            &queue->available));
    eqe_wait_used_count(eqe_queue.used, sequence + 1U);
    eqe_zero_bytes(&completion, sizeof(completion));
    eqe_require(
        virtio_rng_completion_complete(
            api, composition, &descriptor, device_payload,
            &completion) == VIRTIO_RNG_COMPLETION_OK,
        "entropy completion");
    eqe_require(
        completion.bytes_written == EQE_BLOCK_LENGTH &&
            completion.request_result == VIRTIO_RNG_REQUEST_RESULT_READY,
        "entropy completion result");
}

static void eqe_read_blocks(
    struct virtio_rng_queue_composition *composition,
    struct virtio_rng_request_api *api,
    uint32_t first_sequence)
{
    uint32_t index;

    eqe_zero_bytes(eqe_payload_virtual, (size_t)EQE_PAYLOAD_LENGTH);
    for (index = 0U; index < EQE_BLOCK_COUNT; ++index) {
        eqe_read_block(composition, api, index, first_sequence + index);
    }
}

static void eqe_entropy_setup(
    struct entropy_health *health,
    struct entropy_accumulator *accumulator,
    struct entropy_seed_fill *seed_fill,
    uint32_t generation,
    uint32_t source_id)
{
    struct entropy_health_config health_config;
    struct entropy_accumulator_config accumulator_config;
    struct entropy_seed_fill_config seed_config;
    uint8_t seed_output[64];
    uint8_t fill_output[8];
    uint32_t written;
    uint32_t index;

    eqe_zero_bytes(&health_config, sizeof(health_config));
    health_config.version = ENTROPY_HEALTH_VERSION;
    health_config.block_length_min = 1U;
    health_config.block_length_max = ENTROPY_HEALTH_BLOCK_LENGTH_MAX;
    health_config.max_stall_ticks = 100U;
    eqe_zero_bytes(health, sizeof(*health));
    eqe_require(
        entropy_health_init(health, &health_config, generation, 10U) ==
            ENTROPY_HEALTH_OK,
        "entropy health init");

    eqe_zero_bytes(&accumulator_config, sizeof(accumulator_config));
    accumulator_config.version = ENTROPY_ACCUMULATOR_VERSION;
    accumulator_config.generation = generation;
    accumulator_config.source_id = source_id;
    accumulator_config.block_length_min = 1U;
    accumulator_config.block_length_max =
        ENTROPY_ACCUMULATOR_BLOCK_LENGTH_MAX;
    accumulator_config.max_stall_ticks = 100U;
    eqe_zero_bytes(accumulator, sizeof(*accumulator));
    eqe_require(
        entropy_accumulator_init(
            accumulator, &accumulator_config, 10U) ==
            ENTROPY_ACCUMULATOR_OK,
        "entropy accumulator init");

    for (index = 0U; index < EQE_BLOCK_COUNT; ++index) {
        const uint8_t *block =
            (const uint8_t *)eqe_payload_virtual + EQE_DEST_OFFSET +
            index * EQE_BLOCK_LENGTH;

        eqe_require(
            entropy_health_observe(
                health, generation, block, EQE_BLOCK_LENGTH,
                15U + index) == ENTROPY_HEALTH_OK,
            "entropy health observe");
        eqe_require(
            entropy_accumulator_mix(
                accumulator, generation, source_id, block,
                EQE_BLOCK_LENGTH, 15U + index) == ENTROPY_ACCUMULATOR_OK,
            "entropy accumulator mix");
    }
    eqe_require(entropy_health_ready(health), "entropy health ready");
    eqe_require(
        entropy_accumulator_accumulating(accumulator),
        "entropy accumulator ready");

    eqe_zero_bytes(&seed_config, sizeof(seed_config));
    seed_config.version = ENTROPY_SEED_FILL_VERSION;
    seed_config.generation = generation;
    seed_config.source_id = source_id;
    seed_config.required_credited_bytes =
        EQE_BLOCK_COUNT * EQE_BLOCK_LENGTH;
    seed_config.seed_length = EQE_BLOCK_LENGTH;
    seed_config.fill_length_max = 8U;
    seed_config.fill_count_max = 4U;
    eqe_zero_bytes(seed_fill, sizeof(*seed_fill));
    eqe_require(
        entropy_seed_fill_init(
            seed_fill, &seed_config, accumulator) ==
            ENTROPY_SEED_FILL_OK,
        "entropy seed/fill init");

    written = 0U;
    eqe_require(
        entropy_seed_fill_seed(
            seed_fill, accumulator, seed_output,
            (uint32_t)sizeof(seed_output), &written) ==
            ENTROPY_SEED_FILL_OK &&
            written == EQE_BLOCK_LENGTH &&
            entropy_seed_fill_seed_published(seed_fill),
        "entropy seed publication");
    for (index = 0U; index < 4U; ++index) {
        written = 0U;
        eqe_require(
            entropy_seed_fill_fill(
                seed_fill, accumulator, fill_output,
                (uint32_t)sizeof(fill_output), 8U, &written) ==
                ENTROPY_SEED_FILL_OK &&
                written == 8U,
            "entropy bounded fill");
    }
    eqe_require(entropy_seed_fill_ready(seed_fill), "entropy seed ready");
    eqe_zero_bytes(seed_output, sizeof(seed_output));
    eqe_zero_bytes(fill_output, sizeof(fill_output));
}

static void eqe_publish_diagnostics(
    const struct entropy_health *health,
    const struct entropy_accumulator *accumulator,
    const struct entropy_seed_fill *seed_fill)
{
    struct entropy_diagnostics_snapshot snapshot;
    char serial[ENTROPY_DIAGNOSTICS_SERIAL_MAX];
    size_t length = 0U;

    eqe_zero_bytes(&snapshot, sizeof(snapshot));
    eqe_require(
        entropy_diagnostics_snapshot(
            health, accumulator, seed_fill, &snapshot) ==
            ENTROPY_DIAGNOSTICS_OK,
        "entropy diagnostics snapshot");
    eqe_zero_bytes(serial, sizeof(serial));
    eqe_require(
        entropy_diagnostics_serial(
            &snapshot, serial, sizeof(serial), &length) ==
            ENTROPY_DIAGNOSTICS_OK &&
            length < sizeof(serial),
        "entropy diagnostics serial");
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write(serial);
        serial_write("[ENTROPY] diagnostics privacy\n");
        serial_sequence_end(serial_flags);
    }
}

static void eqe_missing_device_proof(void)
{
    struct entropy_restart restart;
    struct entropy_restart_config config;

    serial_write("[ENTROPY] proof init\n");
    eqe_zero_bytes(&config, sizeof(config));
    config.version = ENTROPY_RESTART_VERSION;
    config.restart_count_max = 2U;
    config.min_restart_interval_ticks = 1U;
    eqe_zero_bytes(&restart, sizeof(restart));
    eqe_require(
        entropy_restart_init(&restart, &config) == ENTROPY_RESTART_OK,
        "entropy missing-device lifecycle init");
    eqe_require(
        entropy_restart_activate(
            &restart, NULL, NULL, NULL, EQE_GENERATION, EQE_SOURCE_ID,
            0U) == ENTROPY_RESTART_MISSING_DEVICE &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_MISSING_DEVICE,
        "entropy missing-device denial");
    eqe_require(
        entropy_restart_stop(&restart, 1U) == ENTROPY_RESTART_OK,
        "entropy missing-device shutdown");
    serial_write("[ENTROPY] missing device denial\n");
    serial_write("[ENTROPY] clean shutdown\n");
    serial_write("[ENTROPY] proof OK\n");
}

static void eqe_healthy_proof(
    uint8_t pci_device,
    uint8_t pci_function,
    uint16_t pci_vendor,
    uint16_t pci_device_id)
{
    static uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    static struct virtio_pci_bar_range bars[EQE_PCI_BAR_COUNT];
    static struct virtio_pci_capability_parse_result capabilities;
    static struct virtio_pci_identity identity;
    static struct virtio_pci_selection selection;
    static struct virtio_pci_device_status_io status_io;
    static struct virtio_pci_device_status_controller device_status;
    static struct virtio_pci_feature_io feature_io;
    static struct virtio_pci_feature_controller feature_controller;
    static struct virtio_pci_feature_policy feature_policy;
    static struct virtio_rng_feature_bitset offered;
    static struct virtio_rng_topology topology;
    static struct virtio_rng_feature_profile profile;
    static struct virtio_queue_layout_policy layout_policy;
    static struct virtio_rng_queue_config queue_config;
    static struct virtio_rng_queue_composition composition;
    static struct virtio_rng_request_api request_api;
    static struct entropy_health health;
    static struct entropy_accumulator accumulator;
    static struct entropy_seed_fill seed_fill;
    static struct entropy_restart restart;
    static struct entropy_restart_config restart_config;
    uint64_t initial_free_pages;
    volatile uint8_t *isr_base;
    volatile uint8_t *device_base;
    uint8_t repeated[EQE_BLOCK_LENGTH];
    uint32_t index;

    serial_write("[ENTROPY] proof init\n");
    eqe_require(eqe_leak_sentinel[0] == 'H', "entropy leak sentinel");
    eqe_pci_config_read_space(
        0U, pci_device, pci_function, config);
    eqe_make_bar_ranges(pci_device, bars);
    eqe_require(
        virtio_pci_capability_parse(
            config, sizeof(config), bars, &capabilities) ==
            VIRTIO_PCI_CAPABILITY_OK &&
            virtio_pci_capability_result_validate(&capabilities) ==
                VIRTIO_PCI_CAPABILITY_OK,
        "entropy capability parse");

    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = pci_vendor;
    identity.device_id = pci_device_id;
    identity.bdf.bus = 0U;
    identity.bdf.device = pci_device;
    identity.bdf.function = pci_function;
    identity.modern =
        pci_device_id == VIRTIO_PCI_DEVICE_ID_RNG_MODERN ? 1U : 0U;
    eqe_require(
        virtio_pci_selection_init(
            &selection, &identity,
            &capabilities.snapshot, EQE_GENERATION) == VIRTIO_PCI_OK,
        "entropy identity publication");

    eqe_common_base = eqe_map_bar(
        bars[capabilities.descriptors[0].bar].base +
        capabilities.descriptors[0].offset);
    eqe_notify_base = eqe_map_bar(
        bars[capabilities.descriptors[1].bar].base +
        capabilities.descriptors[1].offset);
    isr_base = eqe_map_bar(
        bars[capabilities.descriptors[2].bar].base +
        capabilities.descriptors[2].offset);
    device_base = eqe_map_bar(
        bars[capabilities.descriptors[3].bar].base +
        capabilities.descriptors[3].offset);
    eqe_notify_multiplier =
        capabilities.descriptors[1].notify_offset_multiplier;
    (void)isr_base;
    (void)device_base;

    eqe_zero_bytes(&status_io, sizeof(status_io));
    status_io.write = eqe_status_write;
    status_io.read = eqe_status_read;
    status_io.context = &eqe_io_context;
    eqe_require(
        virtio_pci_device_status_init(
            &device_status, &status_io, EQE_GENERATION) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_reset(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_acknowledge(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK,
        "entropy device status driver sequence");

    eqe_zero_bytes(&feature_io, sizeof(feature_io));
    feature_io.read_offered = eqe_feature_read_offered;
    feature_io.write_accepted = eqe_feature_write_accepted;
    feature_io.read_accepted = eqe_feature_read_accepted;
    feature_io.context = &eqe_io_context;
    eqe_require(
        virtio_pci_feature_init(
            &feature_controller, &feature_io, EQE_GENERATION) ==
            VIRTIO_PCI_FEATURE_OK,
        "entropy feature controller init");
    feature_policy.required.low = VIRTIO_RNG_REQUIRED_FEATURE_LO_MASK;
    feature_policy.required.high =
        identity.modern != 0U ? VIRTIO_RNG_VERSION_1_HIGH_BIT : 0U;
    feature_policy.accepted = feature_policy.required;
    (void)eqe_feature_read_offered(
        &eqe_io_context, VIRTIO_PCI_FEATURE_SELECT_LO, &offered.low);
    (void)eqe_feature_read_offered(
        &eqe_io_context, VIRTIO_PCI_FEATURE_SELECT_HI, &offered.high);
    eqe_require(
        virtio_pci_feature_negotiate(
            &feature_controller, &feature_policy) ==
            VIRTIO_PCI_FEATURE_OK,
        "entropy feature negotiation");

    eqe_zero_bytes(&topology, sizeof(topology));
    topology.version = VIRTIO_RNG_TOPOLOGY_VERSION;
    topology.request_queue_count = VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX;
    topology.buffer_writer = VIRTIO_RNG_BUFFER_WRITER_DEVICE;
    eqe_zero_bytes(&profile, sizeof(profile));
    eqe_require(
        virtio_rng_feature_profile_activate(
            &profile, &identity, &offered, &topology, EQE_GENERATION) ==
            VIRTIO_RNG_FEATURE_PROFILE_OK,
        "entropy RNG feature profile");

    initial_free_pages = pmm_free_page_count();
    eqe_queue_physical = pmm_alloc_contiguous_pages(1U);
    eqe_payload_physical =
        pmm_alloc_contiguous_pages(EQE_PAYLOAD_PAGES);
    eqe_require(
        eqe_queue_physical != 0U && eqe_payload_physical != 0U,
        "entropy physical memory allocation");
    eqe_queue.physical = eqe_queue_physical;
    eqe_queue.virtual_address =
        (void *)(uintptr_t)(eqe_queue_physical + eqe_hhdm);
    eqe_payload_virtual =
        (void *)(uintptr_t)(eqe_payload_physical + eqe_hhdm);
    eqe_zero_bytes(
        eqe_queue.virtual_address, (size_t)EQE_QUEUE_REGION_LENGTH);
    eqe_zero_bytes(eqe_payload_virtual, (size_t)EQE_PAYLOAD_LENGTH);

    layout_policy.queue_size = EQE_QUEUE_SIZE;
    layout_policy.event_index = false;
    eqe_require(
        virtio_queue_layout_calculate(
            &layout_policy, eqe_queue_physical,
            EQE_QUEUE_REGION_LENGTH, &eqe_queue.layout) ==
            VIRTIO_QUEUE_LAYOUT_OK,
        "entropy queue layout");
    eqe_queue.available =
        (struct virtio_available_ring *)(
            (uintptr_t)eqe_queue.virtual_address +
            eqe_queue.layout.available.offset);
    eqe_queue.used =
        (struct virtio_used_ring *)(
            (uintptr_t)eqe_queue.virtual_address +
            eqe_queue.layout.used.offset);
    eqe_queue.descriptors =
        (volatile struct eqe_virtq_desc *)(
            (uintptr_t)eqe_queue.virtual_address +
            eqe_queue.layout.descriptor.offset);

    eqe_require(
        virtio_pci_device_status_features_ok(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK,
        "entropy features ok device status");
    eqe_program_queue();

    eqe_zero_bytes(&queue_config, sizeof(queue_config));
    queue_config.queue_index = VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX;
    queue_config.queue_size = EQE_QUEUE_SIZE;
    queue_config.event_index = false;
    queue_config.region_base = eqe_queue_physical;
    queue_config.region_length = EQE_QUEUE_REGION_LENGTH;
    queue_config.available_ring = eqe_queue.available;
    queue_config.used_ring = eqe_queue.used;
    queue_config.barrier.fence = eqe_barrier_fence;
    queue_config.barrier.context = &eqe_io_context;
    queue_config.reclaim.reclaim = eqe_queue_reclaim;
    queue_config.reclaim.context = &eqe_io_context;
    queue_config.max_used_length = EQE_BLOCK_LENGTH;
    queue_config.dma_window.base = eqe_queue_physical;
    queue_config.dma_window.length = EQE_QUEUE_REGION_LENGTH;
    queue_config.dma_window.alignment = 16U;
    queue_config.dma_zeroize.zeroize = eqe_zeroize;
    queue_config.dma_zeroize.context = &eqe_io_context;
    queue_config.dma_capacity = EQE_DMA_CAPACITY;
    eqe_zero_bytes(&composition, sizeof(composition));
    eqe_require(
        virtio_rng_queue_composition_activate(
            &composition, &profile, &queue_config, EQE_GENERATION) ==
            VIRTIO_RNG_QUEUE_COMPOSITION_OK,
        "entropy queue composition");
    eqe_require(
        virtio_pci_device_status_driver_ok(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
            virtio_pci_device_status_live(&device_status),
        "entropy driver ok device status");
    serial_write("[ENTROPY] device live\n");

    eqe_require(
        virtio_rng_request_api_init(&request_api, EQE_GENERATION) ==
            VIRTIO_RNG_REQUEST_API_OK,
        "entropy request API init");
    eqe_read_blocks(&composition, &request_api, 0U);

    eqe_zero_bytes(&restart_config, sizeof(restart_config));
    restart_config.version = ENTROPY_RESTART_VERSION;
    restart_config.restart_count_max = 4U;
    restart_config.min_restart_interval_ticks = 1U;
    eqe_zero_bytes(&restart, sizeof(restart));
    eqe_require(
        entropy_restart_init(&restart, &restart_config) ==
            ENTROPY_RESTART_OK,
        "entropy restart init");
    eqe_entropy_setup(
        &health, &accumulator, &seed_fill, EQE_GENERATION, EQE_SOURCE_ID);
    eqe_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            EQE_GENERATION, EQE_SOURCE_ID, 20U) == ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy restart ready");
    eqe_require(
        entropy_restart_check(&restart, 25U) == ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy restart check");
    serial_write("[ENTROPY] healthy seeding\n");

    eqe_publish_diagnostics(&health, &accumulator, &seed_fill);

    eqe_copy_bytes(
        repeated,
        (const uint8_t *)eqe_payload_virtual + EQE_DEST_OFFSET +
            (EQE_BLOCK_COUNT - 1U) * EQE_BLOCK_LENGTH,
        sizeof(repeated));
    eqe_require(
        entropy_health_observe(
            &health, EQE_GENERATION, repeated, sizeof(repeated), 50U) ==
            ENTROPY_HEALTH_REPEATED_BLOCK,
        "entropy repeated-block injection");
    eqe_require(
        entropy_restart_check(&restart, 50U) ==
            ENTROPY_RESTART_REPEATED_OUTPUT &&
            entropy_restart_state(&restart) ==
                ENTROPY_RESTART_STATE_FAILED &&
            entropy_restart_fault(&restart) ==
                ENTROPY_RESTART_FAULT_REPEATED_OUTPUT,
        "entropy repeated rejection");
    serial_write("[ENTROPY] injected rejection\n");

    eqe_require(
        entropy_restart_restart(&restart, 55U) == ENTROPY_RESTART_OK,
        "entropy restart policy");
    eqe_read_blocks(&composition, &request_api, EQE_BLOCK_COUNT);
    eqe_entropy_setup(
        &health, &accumulator, &seed_fill,
        EQE_RECOVERY_GENERATION, EQE_SOURCE_ID);
    eqe_require(
        entropy_restart_activate(
            &restart, &health, &accumulator, &seed_fill,
            EQE_RECOVERY_GENERATION, EQE_SOURCE_ID, 35U) ==
            ENTROPY_RESTART_OK &&
            entropy_restart_ready(&restart),
        "entropy reset recovery");
    serial_write("[ENTROPY] reset recovery\n");

    eqe_require(
        entropy_restart_stop(&restart, 40U) == ENTROPY_RESTART_OK,
        "entropy lifecycle shutdown");
    eqe_mmio_write16(eqe_common_base, EQE_COMMON_QUEUE_SELECT, 0U);
    eqe_mmio_write16(eqe_common_base, EQE_COMMON_QUEUE_ENABLE, 0U);
    eqe_require(
        virtio_rng_queue_composition_teardown(&composition) ==
            VIRTIO_RNG_QUEUE_COMPOSITION_OK,
        "entropy queue teardown");
    eqe_require(
        virtio_pci_device_status_reset(&device_status) ==
            VIRTIO_PCI_DEVICE_STATUS_OK,
        "entropy device reset");
    eqe_zero_bytes(eqe_payload_virtual, (size_t)EQE_PAYLOAD_LENGTH);
    eqe_require(
        pmm_free_contiguous_pages(eqe_queue_physical, 1U),
        "entropy queue free");
    eqe_require(
        pmm_free_contiguous_pages(
            eqe_payload_physical, EQE_PAYLOAD_PAGES),
        "entropy payload free");
    eqe_queue_physical = 0U;
    eqe_payload_physical = 0U;
    eqe_payload_virtual = NULL;
    eqe_require(
        pmm_free_page_count() == initial_free_pages,
        "entropy balanced physical ownership");
    for (index = 0U; index < sizeof(repeated); ++index) {
        repeated[index] = 0U;
    }
    serial_write("[ENTROPY] balanced ownership\n");
    serial_write("[ENTROPY] clean shutdown\n");
    serial_write("[ENTROPY] proof OK\n");
}

static void eqe_proof(uint64_t hhdm_offset)
{
    uint8_t pci_device;
    uint8_t pci_function;
    uint16_t pci_vendor;
    uint16_t pci_device_id;

    eqe_hhdm = hhdm_offset;
    if (!eqe_find_rng_device(
            &pci_device, &pci_function, &pci_vendor, &pci_device_id)) {
        eqe_missing_device_proof();
        return;
    }
    eqe_healthy_proof(
        pci_device, pci_function, pci_vendor, pci_device_id);
}

void entropy_qemu_test_init(uint64_t hhdm_offset)
{
    uint64_t *stack_top = (uint64_t *)(
        (uintptr_t)eqe_proof_stack + sizeof(eqe_proof_stack));
    uint64_t old_rsp;

    __asm__ volatile (
        "mov %%rsp, %0\n\t"
        "mov %1, %%rsp\n\t"
        "call *%2\n\t"
        "mov %0, %%rsp\n\t"
        : "=&r"(old_rsp)
        : "r"(stack_top), "r"(eqe_proof), "D"(hhdm_offset)
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
          "memory", "cc");
}

#else
typedef int entropy_qemu_translation_unit_guard;
#endif
