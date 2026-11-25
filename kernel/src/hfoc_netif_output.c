#include "hfoc_netif_output.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool hfoc_netif_output_pool_valid(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_is_published(pool);
}

static bool hfoc_netif_output_adapter_valid(
    const struct hfoc_netif_output_adapter *adapter)
{
    return adapter != NULL && adapter->submit != NULL;
}

static bool hfoc_netif_output_valid(
    const struct hfoc_netif_output *output)
{
    return output != NULL &&
           output->published &&
           output->version == HFOC_NETIF_OUTPUT_VERSION &&
           hfoc_netif_output_pool_valid(output->pool) &&
           hfoc_netif_output_adapter_valid(&output->adapter) &&
           output->pending_count <= PACKET_POOL_COUNT_MAX;
}

static enum hfoc_netif_output_status hfoc_netif_output_validate_slice(
    const struct hfoc_netif_output *output,
    const struct packet_slice *slice)
{
    struct packet_pool_slot_descriptor current;

    if (slice == NULL) {
        return HFOC_NETIF_OUTPUT_NULL;
    }
    if (slice->pool != output->pool) {
        return HFOC_NETIF_OUTPUT_BAD_POOL;
    }
    if (!packet_slice_is_valid(slice)) {
        return HFOC_NETIF_OUTPUT_BAD_SLICE;
    }
    if (slice->slot.direction != PACKET_POOL_DIRECTION_TX) {
        return HFOC_NETIF_OUTPUT_WRONG_DIRECTION;
    }
    if (packet_pool_slot(
            output->pool, slice->slot.slot, &current) !=
        PACKET_POOL_OK) {
        return HFOC_NETIF_OUTPUT_BAD_SLICE;
    }
    if (current.owner != PACKET_POOL_OWNER_ACTIVE ||
        current.direction != PACKET_POOL_DIRECTION_TX) {
        return HFOC_NETIF_OUTPUT_WRONG_DIRECTION;
    }
    return HFOC_NETIF_OUTPUT_OK;
}

static bool hfoc_netif_output_already_submitted(
    const struct hfoc_netif_output *output,
    const struct packet_slice *slice)
{
    const struct hfoc_netif_output_pending *pending;

    if (slice->slot.slot >= PACKET_POOL_COUNT_MAX) {
        return false;
    }
    pending = &output->pending[slice->slot.slot];
    return pending->submitted &&
           pending->generation == slice->slot.generation;
}

enum hfoc_netif_output_status hfoc_netif_output_init(
    struct hfoc_netif_output *output,
    struct packet_pool_descriptor *pool,
    const struct hfoc_netif_output_adapter *adapter)
{
    if (output == NULL || adapter == NULL) {
        return HFOC_NETIF_OUTPUT_NULL;
    }
    if (output->published) {
        return HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED;
    }
    if (!hfoc_netif_output_pool_valid(pool)) {
        return HFOC_NETIF_OUTPUT_BAD_POOL;
    }
    if (!hfoc_netif_output_adapter_valid(adapter)) {
        return HFOC_NETIF_OUTPUT_INVALID_ADAPTER;
    }

    memset(output, 0, sizeof(*output));
    output->version = HFOC_NETIF_OUTPUT_VERSION;
    output->pool = pool;
    output->adapter = *adapter;
    output->published = true;
    return HFOC_NETIF_OUTPUT_OK;
}

enum hfoc_netif_output_status hfoc_netif_output_submit(
    struct hfoc_netif_output *output,
    struct packet_slice *slice,
    enum hfoc_netif_output_result *result)
{
    struct hfoc_netif_output_submission submission;
    struct hfoc_netif_output_pending *pending;
    enum hfoc_netif_output_status status;
    enum hfoc_netif_output_result verdict;

    if (result == NULL) {
        return HFOC_NETIF_OUTPUT_NULL;
    }
    if (output == NULL) {
        return HFOC_NETIF_OUTPUT_NULL;
    }
    if (!output->published) {
        return HFOC_NETIF_OUTPUT_UNPUBLISHED;
    }
    if (output->version != HFOC_NETIF_OUTPUT_VERSION) {
        return HFOC_NETIF_OUTPUT_INVALID_VERSION;
    }
    if (!hfoc_netif_output_pool_valid(output->pool)) {
        return HFOC_NETIF_OUTPUT_BAD_POOL;
    }
    if (!hfoc_netif_output_adapter_valid(&output->adapter)) {
        return HFOC_NETIF_OUTPUT_INVALID_ADAPTER;
    }

    status = hfoc_netif_output_validate_slice(output, slice);
    if (status != HFOC_NETIF_OUTPUT_OK) {
        return status;
    }
    if (hfoc_netif_output_already_submitted(output, slice)) {
        return HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED;
    }

    memset(&submission, 0, sizeof(submission));
    verdict = output->adapter.submit(
        output->adapter.context, slice, &submission);
    if (verdict == HFOC_NETIF_OUTPUT_SUBMITTED) {
        if (submission.slot != slice->slot.slot ||
            submission.generation != slice->slot.generation) {
            *result = HFOC_NETIF_OUTPUT_REJECTED;
            return HFOC_NETIF_OUTPUT_INVALID_SUBMISSION;
        }
        pending = &output->pending[submission.slot];
        pending->submitted = true;
        pending->generation = submission.generation;
        pending->submission = submission;
        ++output->pending_count;
        *result = HFOC_NETIF_OUTPUT_SUBMITTED;
        return HFOC_NETIF_OUTPUT_OK;
    }

    if (verdict == HFOC_NETIF_OUTPUT_BACKPRESSURE) {
        *result = HFOC_NETIF_OUTPUT_BACKPRESSURE;
    } else {
        *result = HFOC_NETIF_OUTPUT_REJECTED;
    }
    return HFOC_NETIF_OUTPUT_OK;
}

enum hfoc_netif_output_status hfoc_netif_output_complete(
    struct hfoc_netif_output *output,
    const struct hfoc_netif_output_submission *submission)
{
    struct hfoc_netif_output_pending *pending;

    if (output == NULL || submission == NULL) {
        return HFOC_NETIF_OUTPUT_NULL;
    }
    if (!output->published) {
        return HFOC_NETIF_OUTPUT_UNPUBLISHED;
    }
    if (output->version != HFOC_NETIF_OUTPUT_VERSION) {
        return HFOC_NETIF_OUTPUT_INVALID_VERSION;
    }
    if (!hfoc_netif_output_pool_valid(output->pool)) {
        return HFOC_NETIF_OUTPUT_BAD_POOL;
    }
    if (!hfoc_netif_output_adapter_valid(&output->adapter)) {
        return HFOC_NETIF_OUTPUT_INVALID_ADAPTER;
    }
    if (submission->slot >= PACKET_POOL_COUNT_MAX) {
        return HFOC_NETIF_OUTPUT_INVALID_COMPLETION;
    }

    pending = &output->pending[submission->slot];
    if (!pending->submitted) {
        return HFOC_NETIF_OUTPUT_NOT_SUBMITTED;
    }
    if (pending->generation != submission->generation ||
        pending->submission.slot != submission->slot ||
        pending->submission.generation != submission->generation) {
        return HFOC_NETIF_OUTPUT_INVALID_COMPLETION;
    }

    memset(pending, 0, sizeof(*pending));
    --output->pending_count;
    return HFOC_NETIF_OUTPUT_OK;
}

bool hfoc_netif_output_published(
    const struct hfoc_netif_output *output)
{
    return hfoc_netif_output_valid(output);
}

uint32_t hfoc_netif_output_pending_count(
    const struct hfoc_netif_output *output)
{
    return hfoc_netif_output_valid(output)
        ? output->pending_count
        : 0U;
}

const char *hfoc_netif_output_result_string(
    enum hfoc_netif_output_result result)
{
    switch (result) {
    case HFOC_NETIF_OUTPUT_SUBMITTED:
        return "HFOC_NETIF_OUTPUT_SUBMITTED";
    case HFOC_NETIF_OUTPUT_BACKPRESSURE:
        return "HFOC_NETIF_OUTPUT_BACKPRESSURE";
    case HFOC_NETIF_OUTPUT_REJECTED:
        return "HFOC_NETIF_OUTPUT_REJECTED";
    default:
        return NULL;
    }
}

const char *hfoc_netif_output_status_string(
    enum hfoc_netif_output_status status)
{
    switch (status) {
    case HFOC_NETIF_OUTPUT_OK:
        return "HFOC_NETIF_OUTPUT_OK";
    case HFOC_NETIF_OUTPUT_NULL:
        return "HFOC_NETIF_OUTPUT_NULL";
    case HFOC_NETIF_OUTPUT_BAD_STATE:
        return "HFOC_NETIF_OUTPUT_BAD_STATE";
    case HFOC_NETIF_OUTPUT_INVALID_VERSION:
        return "HFOC_NETIF_OUTPUT_INVALID_VERSION";
    case HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED:
        return "HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED";
    case HFOC_NETIF_OUTPUT_UNPUBLISHED:
        return "HFOC_NETIF_OUTPUT_UNPUBLISHED";
    case HFOC_NETIF_OUTPUT_BAD_POOL:
        return "HFOC_NETIF_OUTPUT_BAD_POOL";
    case HFOC_NETIF_OUTPUT_INVALID_ADAPTER:
        return "HFOC_NETIF_OUTPUT_INVALID_ADAPTER";
    case HFOC_NETIF_OUTPUT_BAD_SLICE:
        return "HFOC_NETIF_OUTPUT_BAD_SLICE";
    case HFOC_NETIF_OUTPUT_WRONG_DIRECTION:
        return "HFOC_NETIF_OUTPUT_WRONG_DIRECTION";
    case HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED:
        return "HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED";
    case HFOC_NETIF_OUTPUT_NOT_SUBMITTED:
        return "HFOC_NETIF_OUTPUT_NOT_SUBMITTED";
    case HFOC_NETIF_OUTPUT_INVALID_SUBMISSION:
        return "HFOC_NETIF_OUTPUT_INVALID_SUBMISSION";
    case HFOC_NETIF_OUTPUT_INVALID_COMPLETION:
        return "HFOC_NETIF_OUTPUT_INVALID_COMPLETION";
    default:
        return NULL;
    }
}
