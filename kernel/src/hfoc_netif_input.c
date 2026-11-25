#include "hfoc_netif_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool hfoc_netif_input_pool_valid(
    const struct packet_pool_descriptor *pool)
{
    return packet_pool_is_published(pool);
}

static bool hfoc_netif_input_adapter_valid(
    const struct hfoc_netif_input_adapter *adapter)
{
    return adapter != NULL && adapter->deliver != NULL;
}

static bool hfoc_netif_input_valid(
    const struct hfoc_netif_input *input)
{
    return input != NULL &&
           input->published &&
           input->version == HFOC_NETIF_INPUT_VERSION &&
           hfoc_netif_input_pool_valid(input->pool) &&
           hfoc_netif_input_adapter_valid(&input->adapter);
}

static enum hfoc_netif_input_status hfoc_netif_input_validate_slice(
    const struct hfoc_netif_input *input,
    const struct packet_slice *slice)
{
    struct packet_pool_slot_descriptor current;

    if (slice == NULL) {
        return HFOC_NETIF_INPUT_NULL;
    }
    if (slice->pool != input->pool) {
        return HFOC_NETIF_INPUT_BAD_POOL;
    }
    if (!packet_slice_is_valid(slice)) {
        return HFOC_NETIF_INPUT_BAD_SLICE;
    }
    if (slice->slot.direction != PACKET_POOL_DIRECTION_RX) {
        return HFOC_NETIF_INPUT_WRONG_DIRECTION;
    }
    if (packet_pool_slot(
            input->pool, slice->slot.slot, &current) !=
        PACKET_POOL_OK) {
        return HFOC_NETIF_INPUT_BAD_SLICE;
    }
    if (current.owner != PACKET_POOL_OWNER_ACTIVE ||
        current.direction != PACKET_POOL_DIRECTION_RX) {
        return HFOC_NETIF_INPUT_WRONG_DIRECTION;
    }
    return HFOC_NETIF_INPUT_OK;
}

enum hfoc_netif_input_status hfoc_netif_input_init(
    struct hfoc_netif_input *input,
    struct packet_pool_descriptor *pool,
    const struct hfoc_netif_input_adapter *adapter)
{
    if (input == NULL || adapter == NULL) {
        return HFOC_NETIF_INPUT_NULL;
    }
    if (input->published) {
        return HFOC_NETIF_INPUT_ALREADY_PUBLISHED;
    }
    if (!hfoc_netif_input_pool_valid(pool)) {
        return HFOC_NETIF_INPUT_BAD_POOL;
    }
    if (!hfoc_netif_input_adapter_valid(adapter)) {
        return HFOC_NETIF_INPUT_INVALID_ADAPTER;
    }

    memset(input, 0, sizeof(*input));
    input->version = HFOC_NETIF_INPUT_VERSION;
    input->pool = pool;
    input->adapter = *adapter;
    input->published = true;
    return HFOC_NETIF_INPUT_OK;
}

enum hfoc_netif_input_status hfoc_netif_input_transfer(
    const struct hfoc_netif_input *input,
    struct packet_slice *slice,
    const struct ethernet_frame_view *view,
    enum hfoc_netif_input_result *result)
{
    enum hfoc_netif_input_status status;
    enum hfoc_netif_input_result verdict;

    if (result == NULL) {
        return HFOC_NETIF_INPUT_NULL;
    }
    if (input == NULL) {
        return HFOC_NETIF_INPUT_NULL;
    }
    if (!input->published) {
        return HFOC_NETIF_INPUT_UNPUBLISHED;
    }
    if (input->version != HFOC_NETIF_INPUT_VERSION) {
        return HFOC_NETIF_INPUT_INVALID_VERSION;
    }
    if (!hfoc_netif_input_pool_valid(input->pool)) {
        return HFOC_NETIF_INPUT_BAD_POOL;
    }
    if (!hfoc_netif_input_adapter_valid(&input->adapter)) {
        return HFOC_NETIF_INPUT_INVALID_ADAPTER;
    }
    if (view == NULL) {
        return HFOC_NETIF_INPUT_NULL;
    }
    if (view->version != ETHERNET_FRAME_VERSION) {
        return HFOC_NETIF_INPUT_INVALID_VIEW;
    }

    status = hfoc_netif_input_validate_slice(input, slice);
    if (status != HFOC_NETIF_INPUT_OK) {
        return status;
    }

    verdict = input->adapter.deliver(
        input->adapter.context, slice, view);
    if (verdict == HFOC_NETIF_INPUT_ACCEPTED) {
        *result = HFOC_NETIF_INPUT_ACCEPTED;
        return HFOC_NETIF_INPUT_OK;
    }

    if (packet_pool_release(
            input->pool, &slice->slot) != PACKET_POOL_OK) {
        *result = HFOC_NETIF_INPUT_REJECTED;
        return HFOC_NETIF_INPUT_RELEASE_FAILED;
    }
    if (verdict == HFOC_NETIF_INPUT_BACKPRESSURE) {
        *result = HFOC_NETIF_INPUT_BACKPRESSURE;
    } else {
        *result = HFOC_NETIF_INPUT_REJECTED;
    }
    return HFOC_NETIF_INPUT_OK;
}

bool hfoc_netif_input_published(
    const struct hfoc_netif_input *input)
{
    return hfoc_netif_input_valid(input);
}

const char *hfoc_netif_input_result_string(
    enum hfoc_netif_input_result result)
{
    switch (result) {
    case HFOC_NETIF_INPUT_ACCEPTED:
        return "HFOC_NETIF_INPUT_ACCEPTED";
    case HFOC_NETIF_INPUT_BACKPRESSURE:
        return "HFOC_NETIF_INPUT_BACKPRESSURE";
    case HFOC_NETIF_INPUT_REJECTED:
        return "HFOC_NETIF_INPUT_REJECTED";
    default:
        return NULL;
    }
}

const char *hfoc_netif_input_status_string(
    enum hfoc_netif_input_status status)
{
    switch (status) {
    case HFOC_NETIF_INPUT_OK:
        return "HFOC_NETIF_INPUT_OK";
    case HFOC_NETIF_INPUT_NULL:
        return "HFOC_NETIF_INPUT_NULL";
    case HFOC_NETIF_INPUT_BAD_STATE:
        return "HFOC_NETIF_INPUT_BAD_STATE";
    case HFOC_NETIF_INPUT_INVALID_VERSION:
        return "HFOC_NETIF_INPUT_INVALID_VERSION";
    case HFOC_NETIF_INPUT_ALREADY_PUBLISHED:
        return "HFOC_NETIF_INPUT_ALREADY_PUBLISHED";
    case HFOC_NETIF_INPUT_UNPUBLISHED:
        return "HFOC_NETIF_INPUT_UNPUBLISHED";
    case HFOC_NETIF_INPUT_BAD_POOL:
        return "HFOC_NETIF_INPUT_BAD_POOL";
    case HFOC_NETIF_INPUT_INVALID_ADAPTER:
        return "HFOC_NETIF_INPUT_INVALID_ADAPTER";
    case HFOC_NETIF_INPUT_BAD_SLICE:
        return "HFOC_NETIF_INPUT_BAD_SLICE";
    case HFOC_NETIF_INPUT_WRONG_DIRECTION:
        return "HFOC_NETIF_INPUT_WRONG_DIRECTION";
    case HFOC_NETIF_INPUT_INVALID_VIEW:
        return "HFOC_NETIF_INPUT_INVALID_VIEW";
    case HFOC_NETIF_INPUT_RELEASE_FAILED:
        return "HFOC_NETIF_INPUT_RELEASE_FAILED";
    default:
        return NULL;
    }
}
