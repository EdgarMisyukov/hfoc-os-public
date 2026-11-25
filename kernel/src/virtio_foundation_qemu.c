#ifdef HFOC_VIRTIO_FOUNDATION_QEMU_TEST

#include "serial.h"
#include "virtio_available_publication.h"
#include "virtio_descriptor_ownership.h"
#include "virtio_dma_region.h"
#include "virtio_pci_capability.h"
#include "virtio_pci_device_status.h"
#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"
#include "virtio_queue_layout.h"
#include "virtio_used_consumption.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define VF_QEMU_GENERATION 17U
#define VF_QEMU_QUEUE_SIZE 8U
#define VF_QEMU_MAX_LENGTH 128U
#define VF_QEMU_WINDOW_BASE UINT64_C(0x100000)
#define VF_QEMU_WINDOW_LENGTH UINT64_C(0x100000)
#define VF_QEMU_WINDOW_ALIGNMENT 16U

static struct virtio_available_ring vf_available_ring;
static struct virtio_used_ring vf_used_ring;

static void vf_qemu_fail(const char *message) __attribute__((noreturn));

static void vf_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[VIRTIO-FOUNDATION] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void vf_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        vf_qemu_fail(message);
    }
}

static void vf_qemu_put_le16(uint8_t *buffer, size_t offset, uint16_t value)
{
    buffer[offset] = (uint8_t)(value & 0xFFU);
    buffer[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
}

static void vf_qemu_put_le32(uint8_t *buffer, size_t offset, uint32_t value)
{
    vf_qemu_put_le16(buffer, offset, (uint16_t)(value & 0xFFFFU));
    vf_qemu_put_le16(buffer, offset + 2U,
                     (uint16_t)((value >> 16U) & 0xFFFFU));
}

static void vf_qemu_install_non_vendor_capability(
    uint8_t *config,
    size_t offset,
    uint8_t next)
{
    config[offset] = 0x10U;
    config[offset + 1U] = next;
}

static void vf_qemu_install_vendor_capability(
    uint8_t *config,
    size_t offset,
    uint8_t type,
    uint8_t next,
    uint8_t capability_length,
    uint8_t bar,
    uint32_t capability_offset,
    uint32_t capability_length_value,
    uint16_t notify_multiplier)
{
    config[offset] = VIRTIO_PCI_CAPABILITY_ID_VENDOR;
    config[offset + 1U] = next;
    config[offset + 2U] = capability_length;
    config[offset + 3U] = type;
    config[offset + 4U] = bar;
    config[offset + 5U] = 0U;
    config[offset + 6U] = 0U;
    config[offset + 7U] = 0U;
    vf_qemu_put_le32(config, offset + 8U, capability_offset);
    vf_qemu_put_le32(config, offset + 12U, capability_length_value);
    if (type == VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY) {
        vf_qemu_put_le16(config, offset + 16U, notify_multiplier);
    }
}

static void vf_qemu_make_valid_config(uint8_t *config)
{
    memset(config, 0, VIRTIO_PCI_CONFIG_SPACE_MAX);
    vf_qemu_put_le16(
        config, VIRTIO_PCI_CAPABILITY_POINTER_OFFSET,
        VIRTIO_PCI_FIRST_CAPABILITY_OFFSET);
    vf_qemu_install_non_vendor_capability(config, 0x40U, 0x48U);
    vf_qemu_install_vendor_capability(
        config, 0x48U, VIRTIO_PCI_CAPABILITY_TYPE_COMMON,
        0x60U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 0U, 0x0U,
        0x1000U, 0U);
    vf_qemu_install_vendor_capability(
        config, 0x60U, VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY,
        0x78U, VIRTIO_PCI_NOTIFY_CAPABILITY_MIN_LENGTH, 1U,
        0x0U, 0x1000U, 4U);
    vf_qemu_install_vendor_capability(
        config, 0x78U, VIRTIO_PCI_CAPABILITY_TYPE_ISR,
        0x88U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 2U, 0x0U,
        0x100U, 0U);
    vf_qemu_install_vendor_capability(
        config, 0x88U, VIRTIO_PCI_CAPABILITY_TYPE_DEVICE,
        0U, VIRTIO_PCI_CAPABILITY_MIN_LENGTH, 3U, 0x0U,
        0x100U, 0U);
}

static void vf_qemu_make_bar_ranges(
    struct virtio_pci_bar_range ranges[VIRTIO_PCI_BAR_COUNT])
{
    static const uint64_t bases[VIRTIO_PCI_BAR_COUNT] = {
        UINT64_C(0x10000000),
        UINT64_C(0x20000000),
        UINT64_C(0x30000000),
        UINT64_C(0x40000000),
        UINT64_C(0x50000000),
        UINT64_C(0x60000000),
    };
    unsigned int index;

    for (index = 0U; index < VIRTIO_PCI_BAR_COUNT; ++index) {
        ranges[index].present = true;
        ranges[index].base = bases[index];
        ranges[index].length = UINT64_C(0x10000);
    }
}

static void vf_qemu_make_identity(
    struct virtio_pci_identity *identity,
    uint16_t device_id,
    uint8_t modern)
{
    memset(identity, 0, sizeof(*identity));
    identity->version = VIRTIO_PCI_IDENTITY_VERSION;
    identity->vendor_id = VIRTIO_PCI_VENDOR_ID;
    identity->device_id = device_id;
    identity->bdf.bus = 0U;
    identity->bdf.device = 0U;
    identity->bdf.function = 0U;
    identity->modern = modern;
}

struct vf_status_device {
    uint8_t register_value;
    bool write_fail;
    bool read_fail;
    bool read_override;
    uint8_t read_override_value;
};

static bool vf_status_write(void *context, uint8_t value)
{
    struct vf_status_device *device = (struct vf_status_device *)context;

    if (device->write_fail) {
        return false;
    }
    device->register_value = value;
    return true;
}

static bool vf_status_read(void *context, uint8_t *value)
{
    struct vf_status_device *device = (struct vf_status_device *)context;

    if (device->read_fail) {
        return false;
    }
    *value = device->read_override ?
                 device->read_override_value : device->register_value;
    return true;
}

struct vf_feature_device {
    uint32_t offered_low;
    uint32_t offered_high;
    uint32_t accepted_low;
    uint32_t accepted_high;
    bool read_offered_fail;
    bool write_accepted_fail;
    bool read_accepted_fail;
};

static bool vf_feature_read_offered(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    struct vf_feature_device *device =
        (struct vf_feature_device *)context;

    if (device->read_offered_fail) {
        return false;
    }
    *value = (select == VIRTIO_PCI_FEATURE_SELECT_LO) ?
                 device->offered_low : device->offered_high;
    return true;
}

static bool vf_feature_write_accepted(
    void *context,
    uint32_t select,
    uint32_t value)
{
    struct vf_feature_device *device =
        (struct vf_feature_device *)context;

    if (device->write_accepted_fail) {
        return false;
    }
    if (select == VIRTIO_PCI_FEATURE_SELECT_LO) {
        device->accepted_low = value;
    } else {
        device->accepted_high = value;
    }
    return true;
}

static bool vf_feature_read_accepted(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    struct vf_feature_device *device =
        (struct vf_feature_device *)context;

    if (device->read_accepted_fail) {
        return false;
    }
    *value = (select == VIRTIO_PCI_FEATURE_SELECT_LO) ?
                 device->accepted_low : device->accepted_high;
    return true;
}

struct vf_barrier_state {
    unsigned int count;
};

static void vf_barrier_fence(void *context)
{
    struct vf_barrier_state *state = (struct vf_barrier_state *)context;

    ++state->count;
}

struct vf_reclaim_state {
    struct virtio_descriptor_ownership *descriptors;
    unsigned int calls;
};

static bool vf_used_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    struct vf_reclaim_state *state = (struct vf_reclaim_state *)context;
    struct virtio_descriptor_token descriptor_token;

    (void)token;
    if (element->id >= state->descriptors->capacity) {
        return false;
    }
    descriptor_token.index = element->id;
    descriptor_token.generation =
        state->descriptors->generations[element->id];
    ++state->calls;
    return virtio_descriptor_ownership_consume(
               state->descriptors, &descriptor_token) ==
           VIRTIO_DESCRIPTOR_OWNERSHIP_OK;
}

struct vf_dma_state {
    unsigned int calls;
    uint64_t last_base;
    uint64_t last_length;
};

static bool vf_dma_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    struct vf_dma_state *state = (struct vf_dma_state *)context;

    ++state->calls;
    state->last_base = physical_base;
    state->last_length = length;
    return true;
}

void virtio_foundation_qemu_test_init(void)
{
    static uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    static struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    static struct virtio_pci_capability_parse_result capability_result;
    static struct virtio_pci_capability_parse_result bad_result;
    static struct virtio_pci_identity identity;
    static struct virtio_pci_identity bad_identity;
    static struct virtio_pci_selection selection;
    static struct virtio_pci_selection bad_selection;
    static struct virtio_pci_device_status_io status_io;
    static struct virtio_pci_device_status_controller status_controller;
    static struct vf_status_device status_device;
    static struct virtio_pci_feature_io feature_io;
    static struct virtio_pci_feature_controller feature_controller;
    static struct virtio_pci_feature_policy feature_policy;
    static struct vf_feature_device feature_device;
    static struct virtio_queue_layout_policy layout_policy;
    static struct virtio_queue_layout_result layout;
    static struct vf_barrier_state barrier_state;
    static struct virtio_available_barrier barrier;
    static struct virtio_available_publication available;
    static struct virtio_descriptor_ownership descriptors;
    static struct virtio_descriptor_token descriptor_token;
    static struct virtio_descriptor_token rollback_token;
    static struct virtio_available_token available_token;
    static struct virtio_available_token bad_available_token;
    static struct virtio_used_reclaim used_reclaim;
    static struct vf_reclaim_state reclaim_state;
    static struct virtio_used_consumption used;
    static struct virtio_used_token used_token;
    static struct virtio_used_element used_entry;
    static struct virtio_dma_window_policy window;
    static struct virtio_dma_zeroize zeroize;
    static struct virtio_dma_region_registry registry;
    static struct virtio_dma_region_request queue_region_request;
    static struct virtio_dma_region_request payload_region_request;
    static struct virtio_dma_region_request alias_region_request;
    static struct virtio_dma_region_token queue_region_token;
    static struct virtio_dma_region_token payload_region_token;
    static struct virtio_dma_region queue_region;
    static struct virtio_dma_region payload_region;
    static struct vf_dma_state dma_state;
    static uint16_t chain[1] = {0U};
    static uint16_t duplicate_chain[2] = {0U, 0U};
    static uint8_t observed_status;
    static enum virtio_pci_feature_status feature_status;
    static enum virtio_available_publication_status available_status;
    static enum virtio_used_consumption_status used_status;
    static unsigned int index;

    serial_write("[VIRTIO-FOUNDATION] proof init\n");

    vf_qemu_make_valid_config(config);
    vf_qemu_make_bar_ranges(bars);
    memset(&capability_result, 0, sizeof(capability_result));
    vf_qemu_require(
        virtio_pci_capability_parse(
            config, sizeof(config), bars, &capability_result) ==
            VIRTIO_PCI_CAPABILITY_OK &&
        virtio_pci_capability_result_published(&capability_result) &&
        capability_result.count == VIRTIO_PCI_CAPABILITY_SLOT_COUNT &&
        capability_result.snapshot.capability_mask ==
            VIRTIO_PCI_CAPABILITY_MASK,
        "valid virtio capability discovery");

    vf_qemu_make_identity(
        &identity, VIRTIO_PCI_DEVICE_ID_RNG_MODERN, 1U);
    memset(&selection, 0, sizeof(selection));
    vf_qemu_require(
        virtio_pci_selection_init(
            &selection, &identity, &capability_result.snapshot,
            VF_QEMU_GENERATION) == VIRTIO_PCI_OK &&
        virtio_pci_selection_published(&selection) &&
        virtio_pci_selection_validate_state(
            &selection, &identity, &capability_result.snapshot,
            VF_QEMU_GENERATION) == VIRTIO_PCI_OK,
        "virtio identity publication");

    vf_qemu_make_identity(
        &bad_identity, 0x1001U, 1U);
    memset(&bad_selection, 0, sizeof(bad_selection));
    vf_qemu_require(
        virtio_pci_selection_init(
            &bad_selection, &bad_identity, &capability_result.snapshot,
            VF_QEMU_GENERATION) == VIRTIO_PCI_UNSUPPORTED_DEVICE_ID &&
        !virtio_pci_selection_published(&bad_selection),
        "unsupported virtio identity rejection");

    vf_qemu_make_valid_config(config);
    config[0x88U + 3U] = 0U;
    memset(&bad_result, 0, sizeof(bad_result));
    vf_qemu_require(
        virtio_pci_capability_parse(
            config, sizeof(config), bars, &bad_result) ==
            VIRTIO_PCI_CAPABILITY_UNSUPPORTED_TYPE &&
        !virtio_pci_capability_result_published(&bad_result),
        "malformed capability rejection");
    serial_write("[VIRTIO-FOUNDATION] discovery published\n");

    memset(&feature_device, 0, sizeof(feature_device));
    feature_device.offered_high =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    feature_io.read_offered = vf_feature_read_offered;
    feature_io.write_accepted = vf_feature_write_accepted;
    feature_io.read_accepted = vf_feature_read_accepted;
    feature_io.context = &feature_device;
    memset(&feature_controller, 0, sizeof(feature_controller));
    vf_qemu_require(
        virtio_pci_feature_init(
            &feature_controller, &feature_io, VF_QEMU_GENERATION) ==
            VIRTIO_PCI_FEATURE_OK,
        "feature controller init");
    memset(&feature_policy, 0, sizeof(feature_policy));
    feature_policy.required.low = UINT32_C(0x80000000);
    feature_policy.accepted.high =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    feature_status = virtio_pci_feature_negotiate(
        &feature_controller, &feature_policy);
    vf_qemu_require(
        feature_status == VIRTIO_PCI_FEATURE_UNKNOWN_REQUIRED &&
        !virtio_pci_feature_published(&feature_controller) &&
        feature_device.accepted_low == 0U &&
        feature_device.accepted_high == 0U,
        "unknown feature requirement rejection");
    serial_write("[VIRTIO-FOUNDATION] feature failure rejected\n");

    layout_policy.queue_size = VF_QEMU_QUEUE_SIZE;
    layout_policy.event_index = false;
    memset(&layout, 0, sizeof(layout));
    vf_qemu_require(
        virtio_queue_layout_calculate(
            &layout_policy, 0U, UINT64_C(0x100000), &layout) ==
            VIRTIO_QUEUE_LAYOUT_OK &&
        virtio_queue_layout_result_published(&layout) &&
        layout.queue_size == VF_QEMU_QUEUE_SIZE,
        "queue layout calculation");

    memset(&vf_available_ring, 0, sizeof(vf_available_ring));
    vf_available_ring.idx = 0U;
    for (index = 0U; index < VF_QEMU_QUEUE_SIZE; ++index) {
        vf_available_ring.ring[index] = UINT16_MAX;
    }
    memset(&barrier_state, 0, sizeof(barrier_state));
    barrier.fence = vf_barrier_fence;
    barrier.context = &barrier_state;
    memset(&available, 0, sizeof(available));
    vf_qemu_require(
        virtio_available_publication_init(
            &available, &layout, &vf_available_ring, &barrier,
            VF_QEMU_GENERATION) == VIRTIO_AVAILABLE_PUBLICATION_OK &&
        virtio_available_publication_published(&available),
        "available publication init");

    memset(&descriptors, 0, sizeof(descriptors));
    vf_qemu_require(
        virtio_descriptor_ownership_init(&descriptors, VF_QEMU_QUEUE_SIZE) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
        virtio_descriptor_ownership_published(&descriptors) &&
        virtio_descriptor_ownership_free_count(&descriptors) ==
            VF_QEMU_QUEUE_SIZE,
        "descriptor ownership init");

    memset(&descriptor_token, 0, sizeof(descriptor_token));
    vf_qemu_require(
        virtio_descriptor_ownership_reserve(
            &descriptors, &descriptor_token) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
        virtio_descriptor_ownership_publish(
            &descriptors, &descriptor_token) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
        virtio_descriptor_ownership_published_count(&descriptors) == 1U,
        "descriptor publish");

    chain[0] = (uint16_t)descriptor_token.index;
    memset(&available_token, 0, sizeof(available_token));
    vf_qemu_require(
        virtio_available_publication_prepare(
            &available, chain, 1U, &available_token) ==
            VIRTIO_AVAILABLE_PUBLICATION_OK &&
        virtio_available_publication_publish(
            &available, &available_token) ==
            VIRTIO_AVAILABLE_PUBLICATION_OK &&
        vf_available_ring.idx == 1U &&
        virtio_available_publication_visible_index(&available) == 1U &&
        virtio_available_publication_published_count(&available) == 1U &&
        barrier_state.count == 2U,
        "available ring publication");
    serial_write("[VIRTIO-FOUNDATION] queue publication ok\n");

    memset(&rollback_token, 0, sizeof(rollback_token));
    vf_qemu_require(
        virtio_descriptor_ownership_reserve(
            &descriptors, &rollback_token) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
        virtio_descriptor_ownership_rollback(
            &descriptors, &rollback_token) ==
            VIRTIO_DESCRIPTOR_OWNERSHIP_OK &&
        virtio_descriptor_ownership_free_count(&descriptors) ==
            VF_QEMU_QUEUE_SIZE - 1U,
        "descriptor rollback exactness");

    duplicate_chain[0] = (uint16_t)descriptor_token.index;
    duplicate_chain[1] = (uint16_t)descriptor_token.index;
    memset(&bad_available_token, 0, sizeof(bad_available_token));
    available_status = virtio_available_publication_prepare(
        &available, duplicate_chain, 2U, &bad_available_token);
    vf_qemu_require(
        available_status == VIRTIO_AVAILABLE_PUBLICATION_DUPLICATE_DESCRIPTOR &&
        vf_available_ring.idx == 1U &&
        virtio_available_publication_published_count(&available) == 1U &&
        virtio_available_publication_pending_length(&available) == 0U,
        "failed publication exact rollback");

    memset(&vf_used_ring, 0, sizeof(vf_used_ring));
    vf_used_ring.idx = 0U;
    for (index = 0U; index < VF_QEMU_QUEUE_SIZE; ++index) {
        vf_used_ring.ring[index].id = UINT32_MAX;
        vf_used_ring.ring[index].len = UINT32_MAX;
    }
    memset(&reclaim_state, 0, sizeof(reclaim_state));
    reclaim_state.descriptors = &descriptors;
    used_reclaim.reclaim = vf_used_reclaim;
    used_reclaim.context = &reclaim_state;
    memset(&used, 0, sizeof(used));
    vf_qemu_require(
        virtio_used_consumption_init(
            &used, &layout, &vf_used_ring, &used_reclaim,
            VF_QEMU_GENERATION, VF_QEMU_MAX_LENGTH) ==
            VIRTIO_USED_CONSUMPTION_OK &&
        virtio_used_consumption_published(&used),
        "used consumption init");

    vf_used_ring.idx = 1U;
    vf_used_ring.ring[0].id = VF_QEMU_QUEUE_SIZE;
    vf_used_ring.ring[0].len = 64U;
    memset(&used_token, 0, sizeof(used_token));
    memset(&used_entry, 0, sizeof(used_entry));
    used_status = virtio_used_consumption_prepare(
        &used, &used_entry, &used_token);
    vf_qemu_require(
        used_status == VIRTIO_USED_CONSUMPTION_INVALID_HEAD &&
        virtio_used_consumption_consumed_count(&used) == 0U &&
        !used.pending,
        "invalid used head rejection");

    vf_used_ring.ring[0].id = descriptor_token.index;
    vf_used_ring.ring[0].len = 64U;
    memset(&used_entry, 0, sizeof(used_entry));
    memset(&used_token, 0, sizeof(used_token));
    vf_qemu_require(
        virtio_used_consumption_prepare(
            &used, &used_entry, &used_token) ==
            VIRTIO_USED_CONSUMPTION_OK &&
        used_entry.id == descriptor_token.index &&
        used_entry.len == 64U &&
        virtio_used_consumption_consume(
            &used, &used_token) == VIRTIO_USED_CONSUMPTION_OK &&
        virtio_used_consumption_consumed_count(&used) == 1U &&
        reclaim_state.calls == 1U &&
        virtio_descriptor_ownership_published_count(&descriptors) == 0U &&
        virtio_descriptor_ownership_free_count(&descriptors) ==
            VF_QEMU_QUEUE_SIZE,
        "used completion and reclaim");
    serial_write("[VIRTIO-FOUNDATION] queue completion ok\n");

    memset(&status_device, 0, sizeof(status_device));
    status_io.write = vf_status_write;
    status_io.read = vf_status_read;
    status_io.context = &status_device;
    memset(&status_controller, 0, sizeof(status_controller));
    vf_qemu_require(
        virtio_pci_device_status_init(
            &status_controller, &status_io, VF_QEMU_GENERATION) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_reset(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_acknowledge(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_features_ok(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver_ok(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_live(&status_controller),
        "device status live transition");

    status_device.read_override = true;
    status_device.read_override_value =
        status_controller.status_bits |
        VIRTIO_PCI_DEVICE_STATUS_BIT_DEVICE_NEEDS_RESET;
    vf_qemu_require(
        virtio_pci_device_status_observe(
            &status_controller, &observed_status) ==
            VIRTIO_PCI_DEVICE_STATUS_DEVICE_NEEDS_RESET &&
        !virtio_pci_device_status_live(&status_controller),
        "injected device reset observation");
    status_device.read_override = false;
    vf_qemu_require(
        virtio_pci_device_status_reset(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        !virtio_pci_device_status_live(&status_controller) &&
        virtio_pci_device_status_acknowledge(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_features_ok(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_driver_ok(&status_controller) ==
            VIRTIO_PCI_DEVICE_STATUS_OK &&
        virtio_pci_device_status_live(&status_controller),
        "reset recovery");
    serial_write("[VIRTIO-FOUNDATION] injected reset ok\n");
    serial_write("[VIRTIO-FOUNDATION] exact rollback ok\n");

    memset(&dma_state, 0, sizeof(dma_state));
    window.base = VF_QEMU_WINDOW_BASE;
    window.length = VF_QEMU_WINDOW_LENGTH;
    window.alignment = VF_QEMU_WINDOW_ALIGNMENT;
    zeroize.zeroize = vf_dma_zeroize;
    zeroize.context = &dma_state;
    memset(&registry, 0, sizeof(registry));
    vf_qemu_require(
        virtio_dma_region_init(
            &registry, &window, &zeroize, 4U) == VIRTIO_DMA_REGION_OK &&
        virtio_dma_region_published(&registry),
        "DMA registry init");

    memset(&queue_region_request, 0, sizeof(queue_region_request));
    queue_region_request.physical_base = VF_QEMU_WINDOW_BASE;
    queue_region_request.length = UINT64_C(0x1000);
    queue_region_request.direction = VIRTIO_DMA_DIRECTION_DEVICE_WRITE;
    queue_region_request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    queue_region_request.zeroization = VIRTIO_DMA_ZEROIZATION_NONE;
    memset(&queue_region_token, 0, sizeof(queue_region_token));
    memset(&queue_region, 0, sizeof(queue_region));
    vf_qemu_require(
        virtio_dma_region_bind(
            &registry, &queue_region_request, &queue_region_token,
            &queue_region) == VIRTIO_DMA_REGION_OK &&
        virtio_dma_region_bound_count(&registry) == 1U,
        "DMA queue region bind");

    memset(&payload_region_request, 0, sizeof(payload_region_request));
    payload_region_request.physical_base =
        VF_QEMU_WINDOW_BASE + UINT64_C(0x1000);
    payload_region_request.length = UINT64_C(0x200);
    payload_region_request.direction = VIRTIO_DMA_DIRECTION_DEVICE_READ;
    payload_region_request.lifetime = VIRTIO_DMA_LIFETIME_PAYLOAD;
    payload_region_request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    memset(&payload_region_token, 0, sizeof(payload_region_token));
    memset(&payload_region, 0, sizeof(payload_region));
    vf_qemu_require(
        virtio_dma_region_bind(
            &registry, &payload_region_request, &payload_region_token,
            &payload_region) == VIRTIO_DMA_REGION_OK &&
        virtio_dma_region_bound_count(&registry) == 2U,
        "DMA payload region bind");

    alias_region_request = payload_region_request;
    vf_qemu_require(
        virtio_dma_region_bind(
            &registry, &alias_region_request, &payload_region_token,
            &payload_region) == VIRTIO_DMA_REGION_ALIAS &&
        virtio_dma_region_bound_count(&registry) == 2U,
        "DMA alias rejection");
    vf_qemu_require(
        virtio_dma_region_teardown(&registry) ==
            VIRTIO_DMA_REGION_LIVE_OWNERS &&
        virtio_dma_region_published(&registry),
        "DMA live teardown rejection");
    vf_qemu_require(
        virtio_dma_region_unbind(
            &registry, &queue_region_token) == VIRTIO_DMA_REGION_OK &&
        virtio_dma_region_unbind(
            &registry, &payload_region_token) == VIRTIO_DMA_REGION_OK &&
        dma_state.calls == 1U &&
        dma_state.last_base == payload_region_request.physical_base &&
        dma_state.last_length == payload_region_request.length &&
        virtio_dma_region_bound_count(&registry) == 0U &&
        virtio_dma_region_teardown(&registry) == VIRTIO_DMA_REGION_OK &&
        !virtio_dma_region_published(&registry),
        "DMA exact teardown");
    serial_write("[VIRTIO-FOUNDATION] DMA teardown ok\n");

    serial_write("[VIRTIO-FOUNDATION] clean shutdown\n");
    serial_write("[VIRTIO-FOUNDATION] proof OK\n");
}

#else
/* Keep this test-only translation unit non-empty in normal kernel builds. */
int hfoc_virtio_foundation_qemu_translation_unit_marker;
#endif
