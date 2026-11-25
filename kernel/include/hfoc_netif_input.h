#ifndef HFOC_NETIF_INPUT_H
#define HFOC_NETIF_INPUT_H

#include "ethernet_frame.h"
#include "packet_slice.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_NETIF_INPUT_VERSION 1U

enum hfoc_netif_input_result {
    HFOC_NETIF_INPUT_ACCEPTED = 0,
    HFOC_NETIF_INPUT_BACKPRESSURE = 1,
    HFOC_NETIF_INPUT_REJECTED = 2,
};

enum hfoc_netif_input_status {
    HFOC_NETIF_INPUT_OK = 0,
    HFOC_NETIF_INPUT_NULL = 1,
    HFOC_NETIF_INPUT_BAD_STATE = 2,
    HFOC_NETIF_INPUT_INVALID_VERSION = 3,
    HFOC_NETIF_INPUT_ALREADY_PUBLISHED = 4,
    HFOC_NETIF_INPUT_UNPUBLISHED = 5,
    HFOC_NETIF_INPUT_BAD_POOL = 6,
    HFOC_NETIF_INPUT_INVALID_ADAPTER = 7,
    HFOC_NETIF_INPUT_BAD_SLICE = 8,
    HFOC_NETIF_INPUT_WRONG_DIRECTION = 9,
    HFOC_NETIF_INPUT_INVALID_VIEW = 10,
    HFOC_NETIF_INPUT_RELEASE_FAILED = 11,
};

struct hfoc_netif_input_adapter {
    enum hfoc_netif_input_result (*deliver)(
        void *context,
        struct packet_slice *slice,
        const struct ethernet_frame_view *view);
    void *context;
};

struct hfoc_netif_input {
    bool published;
    uint32_t version;
    struct packet_pool_descriptor *pool;
    struct hfoc_netif_input_adapter adapter;
};

enum hfoc_netif_input_status hfoc_netif_input_init(
    struct hfoc_netif_input *input,
    struct packet_pool_descriptor *pool,
    const struct hfoc_netif_input_adapter *adapter);
enum hfoc_netif_input_status hfoc_netif_input_transfer(
    const struct hfoc_netif_input *input,
    struct packet_slice *slice,
    const struct ethernet_frame_view *view,
    enum hfoc_netif_input_result *result);
bool hfoc_netif_input_published(
    const struct hfoc_netif_input *input);
const char *hfoc_netif_input_result_string(
    enum hfoc_netif_input_result result);
const char *hfoc_netif_input_status_string(
    enum hfoc_netif_input_status status);

#endif
