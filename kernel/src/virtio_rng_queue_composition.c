#include "virtio_rng_queue_composition.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_rng_queue_composition_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_RNG_QUEUE_COMPOSITION_GENERATION_MIN &&
           generation <= VIRTIO_RNG_QUEUE_COMPOSITION_GENERATION_MAX;
}

static bool virtio_rng_queue_composition_queue_size_valid(uint32_t queue_size)
{
    return queue_size >= VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_SIZE_MIN &&
           queue_size <= VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_SIZE_MAX &&
           (queue_size & (queue_size - 1U)) == 0U;
}

static bool virtio_rng_queue_composition_dma_capacity_valid(
    uint32_t capacity)
{
    return capacity >= VIRTIO_RNG_QUEUE_COMPOSITION_DMA_CAPACITY_MIN &&
           capacity <= VIRTIO_RNG_QUEUE_COMPOSITION_DMA_CAPACITY_MAX;
}

static bool virtio_rng_queue_composition_dma_alignment_valid(
    uint32_t alignment)
{
    return alignment >= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MIN &&
           alignment <= VIRTIO_DMA_REGION_WINDOW_ALIGNMENT_MAX &&
           (alignment & (alignment - 1U)) == 0U;
}

static bool virtio_rng_queue_composition_profile_valid(
    const struct virtio_rng_feature_profile *profile)
{
    struct virtio_rng_topology topology;
    enum virtio_rng_feature_profile_status status;

    if (profile == NULL ||
        !profile->published ||
        profile->version != VIRTIO_RNG_FEATURE_PROFILE_VERSION ||
        profile->request_queue_count != VIRTIO_RNG_REQUEST_QUEUE_COUNT_MAX ||
        profile->buffer_writer != VIRTIO_RNG_BUFFER_WRITER_DEVICE) {
        return false;
    }

    memset(&topology, 0, sizeof(topology));
    topology.version = VIRTIO_RNG_TOPOLOGY_VERSION;
    topology.request_queue_count = profile->request_queue_count;
    topology.buffer_writer = profile->buffer_writer;
    status = virtio_rng_feature_profile_validate_state(
        profile,
        &profile->identity,
        &profile->offered,
        &topology,
        profile->generation);
    return status == VIRTIO_RNG_FEATURE_PROFILE_OK;
}

static enum virtio_rng_queue_composition_status
virtio_rng_queue_config_validate(
    const struct virtio_rng_queue_config *config)
{
    uint64_t region_end;
    uint64_t window_end;

    if (config == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_NULL;
    }
    if (config->queue_index != VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX;
    }
    if (!virtio_rng_queue_composition_queue_size_valid(
            config->queue_size)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE;
    }
    if (config->available_ring == NULL || config->used_ring == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING;
    }
    if (config->barrier.fence == NULL || config->barrier.context == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER;
    }
    if (config->reclaim.reclaim == NULL || config->reclaim.context == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE;
    }
    if (config->max_used_length >
        VIRTIO_RNG_QUEUE_COMPOSITION_MAX_USED_LENGTH) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH;
    }
    if (!virtio_rng_queue_composition_dma_capacity_valid(
            config->dma_capacity)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA;
    }
    if (config->dma_zeroize.zeroize == NULL ||
        config->dma_zeroize.context == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA;
    }
    if (!virtio_rng_queue_composition_dma_alignment_valid(
            config->dma_window.alignment) ||
        config->dma_window.length == 0U ||
        config->dma_window.base > UINT64_MAX - config->dma_window.length ||
        config->dma_window.base % config->dma_window.alignment != 0U) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA;
    }
    if (config->region_length == 0U ||
        config->region_base > UINT64_MAX - config->region_length ||
        config->region_base % config->dma_window.alignment != 0U) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT;
    }

    region_end = config->region_base + config->region_length;
    window_end = config->dma_window.base + config->dma_window.length;
    if (config->region_base < config->dma_window.base ||
        region_end > window_end) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA;
    }
    return VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

static bool virtio_rng_queue_teardown(struct virtio_rng_queue *queue)
{
    if (queue == NULL) {
        return false;
    }
    if (queue->dma.published) {
        if (virtio_dma_region_bound_count(&queue->dma) > 0U &&
            virtio_dma_region_unbind(
                &queue->dma, &queue->dma_token) != VIRTIO_DMA_REGION_OK) {
            return false;
        }
        if (virtio_dma_region_teardown(&queue->dma) != VIRTIO_DMA_REGION_OK) {
            return false;
        }
    }
    memset(queue, 0, sizeof(*queue));
    return true;
}

static enum virtio_rng_queue_composition_status
virtio_rng_queue_initialize(
    struct virtio_rng_queue *queue,
    const struct virtio_rng_queue_config *config,
    uint32_t generation)
{
    struct virtio_rng_queue draft;
    struct virtio_queue_layout_policy layout_policy;
    struct virtio_dma_region_request request;
    enum virtio_rng_queue_composition_status status;
    enum virtio_queue_layout_status layout_status;

    memset(&draft, 0, sizeof(draft));
    status = virtio_rng_queue_config_validate(config);
    if (status != VIRTIO_RNG_QUEUE_COMPOSITION_OK) {
        return status;
    }

    draft.queue_index = config->queue_index;
    draft.queue_size = config->queue_size;
    draft.version = VIRTIO_RNG_QUEUE_COMPOSITION_VERSION;

    layout_policy.queue_size = config->queue_size;
    layout_policy.event_index = config->event_index;
    layout_status = virtio_queue_layout_calculate(
        &layout_policy, config->region_base, config->region_length,
        &draft.layout);
    if (layout_status != VIRTIO_QUEUE_LAYOUT_OK) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT;
    }

    if (virtio_available_publication_init(
            &draft.available,
            &draft.layout,
            config->available_ring,
            &config->barrier,
            generation) != VIRTIO_AVAILABLE_PUBLICATION_OK) {
        (void)virtio_rng_queue_teardown(&draft);
        return VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED;
    }
    if (virtio_descriptor_ownership_init(
            &draft.descriptors, config->queue_size) !=
        VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        (void)virtio_rng_queue_teardown(&draft);
        return VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED;
    }
    if (virtio_used_consumption_init(
            &draft.used,
            &draft.layout,
            config->used_ring,
            &config->reclaim,
            generation,
            config->max_used_length) != VIRTIO_USED_CONSUMPTION_OK) {
        (void)virtio_rng_queue_teardown(&draft);
        return VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED;
    }
    if (virtio_dma_region_init(
            &draft.dma,
            &config->dma_window,
            &config->dma_zeroize,
            config->dma_capacity) != VIRTIO_DMA_REGION_OK) {
        (void)virtio_rng_queue_teardown(&draft);
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA;
    }

    memset(&request, 0, sizeof(request));
    request.physical_base = config->region_base;
    request.length = draft.layout.total_size;
    request.direction = VIRTIO_DMA_DIRECTION_BIDIRECTIONAL;
    request.lifetime = VIRTIO_DMA_LIFETIME_QUEUE;
    request.zeroization = VIRTIO_DMA_ZEROIZATION_ON_RELEASE;
    if (virtio_dma_region_bind(
            &draft.dma,
            &request,
            &draft.dma_token,
            &draft.dma_region) != VIRTIO_DMA_REGION_OK) {
        (void)virtio_rng_queue_teardown(&draft);
        return VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED;
    }

    draft.published = true;
    *queue = draft;
    return VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

static enum virtio_rng_queue_composition_status
virtio_rng_queue_composition_validate(
    const struct virtio_rng_queue_composition *composition)
{
    const struct virtio_rng_queue *queue;

    if (composition == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_NULL;
    }
    if (!composition->published) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED;
    }
    if (composition->version != VIRTIO_RNG_QUEUE_COMPOSITION_VERSION) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION;
    }
    if (!virtio_rng_queue_composition_generation_valid(
            composition->generation)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION;
    }
    if (!virtio_rng_queue_composition_profile_valid(
            &composition->profile)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE;
    }

    queue = &composition->queue;
    if (!queue->published ||
        queue->version != VIRTIO_RNG_QUEUE_COMPOSITION_VERSION ||
        queue->queue_index != VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INDEX ||
        !virtio_rng_queue_composition_queue_size_valid(
            queue->queue_size)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE;
    }
    if (virtio_queue_layout_result_validate(&queue->layout) !=
            VIRTIO_QUEUE_LAYOUT_OK ||
        !virtio_descriptor_ownership_published(&queue->descriptors) ||
        !virtio_available_publication_published(&queue->available) ||
        !virtio_used_consumption_published(&queue->used) ||
        !virtio_dma_region_published(&queue->dma) ||
        virtio_dma_region_bound_count(&queue->dma) != 1U ||
        queue->dma_region.physical_base != queue->layout.region_base ||
        queue->dma_region.length != queue->layout.total_size ||
        queue->dma_region.direction != VIRTIO_DMA_DIRECTION_BIDIRECTIONAL ||
        queue->dma_region.lifetime != VIRTIO_DMA_LIFETIME_QUEUE ||
        queue->dma_region.zeroization != VIRTIO_DMA_ZEROIZATION_ON_RELEASE ||
        queue->dma_token.index >= queue->dma.capacity ||
        queue->dma_token.generation !=
            queue->dma.slots[queue->dma_token.index].generation ||
        !queue->dma.slots[queue->dma_token.index].bound) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE;
    }
    return VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

enum virtio_rng_queue_composition_status
virtio_rng_queue_composition_activate(
    struct virtio_rng_queue_composition *composition,
    const struct virtio_rng_feature_profile *profile,
    const struct virtio_rng_queue_config *config,
    uint32_t generation)
{
    struct virtio_rng_queue_composition draft;
    enum virtio_rng_queue_composition_status status;

    if (composition == NULL || profile == NULL || config == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_NULL;
    }
    if (composition->published) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED;
    }
    if (composition->version != 0U) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION;
    }
    if (!virtio_rng_queue_composition_profile_valid(profile)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE;
    }
    if (!virtio_rng_queue_composition_generation_valid(generation)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION;
    }

    memset(&draft, 0, sizeof(draft));
    status = virtio_rng_queue_initialize(
        &draft.queue, config, generation);
    if (status != VIRTIO_RNG_QUEUE_COMPOSITION_OK) {
        if (!virtio_rng_queue_teardown(&draft.queue)) {
            return VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED;
        }
        return status;
    }

    draft.profile = *profile;
    draft.generation = generation;
    draft.version = VIRTIO_RNG_QUEUE_COMPOSITION_VERSION;
    draft.published = true;
    *composition = draft;
    return VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

enum virtio_rng_queue_composition_status
virtio_rng_queue_composition_teardown(
    struct virtio_rng_queue_composition *composition)
{
    struct virtio_rng_queue_composition draft;
    enum virtio_rng_queue_composition_status status;

    if (composition == NULL) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_NULL;
    }
    status = virtio_rng_queue_composition_validate(composition);
    if (status != VIRTIO_RNG_QUEUE_COMPOSITION_OK) {
        return status;
    }
    draft = *composition;
    if (!virtio_rng_queue_teardown(&draft.queue)) {
        return VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED;
    }
    memset(composition, 0, sizeof(*composition));
    return VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

bool virtio_rng_queue_composition_published(
    const struct virtio_rng_queue_composition *composition)
{
    return virtio_rng_queue_composition_validate(composition) ==
           VIRTIO_RNG_QUEUE_COMPOSITION_OK;
}

uint32_t virtio_rng_queue_composition_generation(
    const struct virtio_rng_queue_composition *composition)
{
    if (virtio_rng_queue_composition_validate(composition) !=
        VIRTIO_RNG_QUEUE_COMPOSITION_OK) {
        return 0U;
    }
    return composition->generation;
}

const struct virtio_rng_queue *
virtio_rng_queue_composition_queue(
    const struct virtio_rng_queue_composition *composition)
{
    if (virtio_rng_queue_composition_validate(composition) !=
        VIRTIO_RNG_QUEUE_COMPOSITION_OK) {
        return NULL;
    }
    return &composition->queue;
}

const char *virtio_rng_queue_composition_status_string(
    enum virtio_rng_queue_composition_status status)
{
    switch (status) {
    case VIRTIO_RNG_QUEUE_COMPOSITION_OK:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_OK";
    case VIRTIO_RNG_QUEUE_COMPOSITION_NULL:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_NULL";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_VERSION";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_GENERATION";
    case VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_ALREADY_PUBLISHED";
    case VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_UNPUBLISHED";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_PROFILE";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_INDEX";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_QUEUE_SIZE";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_LAYOUT";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RING";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_BARRIER";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_RELEASE";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_MAX_LENGTH";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_DMA";
    case VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_DMA_BIND_FAILED";
    case VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_QUEUE_INIT_FAILED";
    case VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_TEARDOWN_FAILED";
    case VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE:
        return "VIRTIO_RNG_QUEUE_COMPOSITION_INVALID_STATE";
    default:
        return NULL;
    }
}
