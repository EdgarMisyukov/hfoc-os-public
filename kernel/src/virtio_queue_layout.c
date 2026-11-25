#include "virtio_queue_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool virtio_queue_layout_queue_size_valid(uint32_t queue_size)
{
    return queue_size >= VIRTIO_QUEUE_SIZE_MIN &&
           queue_size <= VIRTIO_QUEUE_SIZE_MAX &&
           (queue_size & (queue_size - 1U)) == 0U;
}

static bool virtio_queue_layout_align_up(
    uint64_t value,
    uint64_t alignment,
    uint64_t *aligned_out)
{
    uint64_t aligned;
    uint64_t remainder;

    remainder = value % alignment;
    aligned = value;
    if (remainder != 0U) {
        uint64_t padding = alignment - remainder;

        if (aligned > UINT64_MAX - padding) {
            return false;
        }
        aligned += padding;
    }
    *aligned_out = aligned;
    return true;
}

static enum virtio_queue_layout_status virtio_queue_layout_build(
    const struct virtio_queue_layout_policy *policy,
    uint64_t region_base,
    uint64_t region_length,
    struct virtio_queue_layout_result *result)
{
    struct virtio_queue_layout_result draft;
    uint64_t descriptor_size;
    uint64_t available_size;
    uint64_t used_size;
    uint64_t used_offset;
    uint64_t total_raw;
    uint64_t total_size;
    uint64_t available_raw;

    if (policy == NULL || result == NULL) {
        return VIRTIO_QUEUE_LAYOUT_NULL;
    }
    if (!virtio_queue_layout_queue_size_valid(policy->queue_size)) {
        return VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE;
    }
    if (region_length == 0U) {
        return VIRTIO_QUEUE_LAYOUT_INVALID_RANGE;
    }
    if (region_base > UINT64_MAX - region_length) {
        return VIRTIO_QUEUE_LAYOUT_OVERFLOW;
    }
    if (region_base % VIRTIO_QUEUE_LAYOUT_ALIGNMENT != 0U) {
        return VIRTIO_QUEUE_LAYOUT_UNALIGNED;
    }

    descriptor_size =
        (uint64_t)policy->queue_size *
        VIRTIO_QUEUE_DESCRIPTOR_ELEMENT_SIZE;
    available_size =
        VIRTIO_QUEUE_RING_FIXED_SIZE +
        (uint64_t)policy->queue_size *
            VIRTIO_QUEUE_AVAILABLE_ELEMENT_SIZE +
        (policy->event_index ? VIRTIO_QUEUE_EVENT_INDEX_SIZE : 0U);
    used_size =
        VIRTIO_QUEUE_RING_FIXED_SIZE +
        (uint64_t)policy->queue_size *
            VIRTIO_QUEUE_USED_ELEMENT_SIZE +
        (policy->event_index ? VIRTIO_QUEUE_EVENT_INDEX_SIZE : 0U);

    available_raw = descriptor_size + available_size;
    if (available_raw < descriptor_size ||
        !virtio_queue_layout_align_up(
            available_raw,
            VIRTIO_QUEUE_USED_ALIGNMENT,
            &used_offset)) {
        return VIRTIO_QUEUE_LAYOUT_OVERFLOW;
    }
    if (used_offset > UINT64_MAX - used_size) {
        return VIRTIO_QUEUE_LAYOUT_OVERFLOW;
    }
    total_raw = used_offset + used_size;
    if (!virtio_queue_layout_align_up(
            total_raw,
            VIRTIO_QUEUE_LAYOUT_ALIGNMENT,
            &total_size)) {
        return VIRTIO_QUEUE_LAYOUT_OVERFLOW;
    }
    if (region_length < total_size) {
        return VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL;
    }

    draft.published = true;
    draft.version = VIRTIO_QUEUE_LAYOUT_RESULT_VERSION;
    draft.queue_size = policy->queue_size;
    draft.event_index = policy->event_index;
    draft.region_base = region_base;
    draft.region_length = region_length;
    draft.descriptor.offset = 0U;
    draft.descriptor.size = descriptor_size;
    draft.descriptor.alignment = VIRTIO_QUEUE_DESCRIPTOR_ALIGNMENT;
    draft.available.offset = descriptor_size;
    draft.available.size = available_size;
    draft.available.alignment = VIRTIO_QUEUE_AVAILABLE_ALIGNMENT;
    draft.used.offset = used_offset;
    draft.used.size = used_size;
    draft.used.alignment = VIRTIO_QUEUE_USED_ALIGNMENT;
    draft.total_size = total_size;
    *result = draft;
    return VIRTIO_QUEUE_LAYOUT_OK;
}

enum virtio_queue_layout_status virtio_queue_layout_calculate(
    const struct virtio_queue_layout_policy *policy,
    uint64_t region_base,
    uint64_t region_length,
    struct virtio_queue_layout_result *result)
{
    struct virtio_queue_layout_result draft;

    if (policy == NULL || result == NULL) {
        return VIRTIO_QUEUE_LAYOUT_NULL;
    }
    {
        enum virtio_queue_layout_status status =
            virtio_queue_layout_build(
                policy,
                region_base,
                region_length,
                &draft);

        if (status != VIRTIO_QUEUE_LAYOUT_OK) {
            return status;
        }
    }
    *result = draft;
    return VIRTIO_QUEUE_LAYOUT_OK;
}

enum virtio_queue_layout_status virtio_queue_layout_result_validate(
    const struct virtio_queue_layout_result *result)
{
    struct virtio_queue_layout_policy policy;
    struct virtio_queue_layout_result draft;
    enum virtio_queue_layout_status status;

    if (result == NULL) {
        return VIRTIO_QUEUE_LAYOUT_NULL;
    }
    if (!result->published) {
        return VIRTIO_QUEUE_LAYOUT_UNPUBLISHED;
    }
    if (result->version != VIRTIO_QUEUE_LAYOUT_RESULT_VERSION) {
        return VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION;
    }

    policy.queue_size = result->queue_size;
    policy.event_index = result->event_index;
    status = virtio_queue_layout_build(
        &policy,
        result->region_base,
        result->region_length,
        &draft);
    if (status != VIRTIO_QUEUE_LAYOUT_OK) {
        return status;
    }

    if (draft.queue_size != result->queue_size ||
        draft.event_index != result->event_index ||
        draft.region_base != result->region_base ||
        draft.region_length != result->region_length ||
        draft.descriptor.offset != result->descriptor.offset ||
        draft.descriptor.size != result->descriptor.size ||
        draft.descriptor.alignment != result->descriptor.alignment ||
        draft.available.offset != result->available.offset ||
        draft.available.size != result->available.size ||
        draft.available.alignment != result->available.alignment ||
        draft.used.offset != result->used.offset ||
        draft.used.size != result->used.size ||
        draft.used.alignment != result->used.alignment ||
        draft.total_size != result->total_size) {
        return VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT;
    }
    return VIRTIO_QUEUE_LAYOUT_OK;
}

bool virtio_queue_layout_result_published(
    const struct virtio_queue_layout_result *result)
{
    return result != NULL && result->published;
}

const char *virtio_queue_layout_status_string(
    enum virtio_queue_layout_status status)
{
    switch (status) {
    case VIRTIO_QUEUE_LAYOUT_OK:
        return "VIRTIO_QUEUE_LAYOUT_OK";
    case VIRTIO_QUEUE_LAYOUT_NULL:
        return "VIRTIO_QUEUE_LAYOUT_NULL";
    case VIRTIO_QUEUE_LAYOUT_UNPUBLISHED:
        return "VIRTIO_QUEUE_LAYOUT_UNPUBLISHED";
    case VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION:
        return "VIRTIO_QUEUE_LAYOUT_INVALID_RESULT_VERSION";
    case VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE:
        return "VIRTIO_QUEUE_LAYOUT_INVALID_QUEUE_SIZE";
    case VIRTIO_QUEUE_LAYOUT_INVALID_RANGE:
        return "VIRTIO_QUEUE_LAYOUT_INVALID_RANGE";
    case VIRTIO_QUEUE_LAYOUT_OVERFLOW:
        return "VIRTIO_QUEUE_LAYOUT_OVERFLOW";
    case VIRTIO_QUEUE_LAYOUT_UNALIGNED:
        return "VIRTIO_QUEUE_LAYOUT_UNALIGNED";
    case VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL:
        return "VIRTIO_QUEUE_LAYOUT_RANGE_TOO_SMALL";
    case VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT:
        return "VIRTIO_QUEUE_LAYOUT_INVALID_LAYOUT";
    default:
        return NULL;
    }
}
