#ifndef HFOC_NETIF_OUTPUT_H
#define HFOC_NETIF_OUTPUT_H

#include "packet_slice.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_NETIF_OUTPUT_VERSION 1U
#define HFOC_NETIF_OUTPUT_PENDING_MAX PACKET_POOL_COUNT_MAX

enum hfoc_netif_output_result {
    HFOC_NETIF_OUTPUT_SUBMITTED = 0,
    HFOC_NETIF_OUTPUT_BACKPRESSURE = 1,
    HFOC_NETIF_OUTPUT_REJECTED = 2,
};

enum hfoc_netif_output_status {
    HFOC_NETIF_OUTPUT_OK = 0,
    HFOC_NETIF_OUTPUT_NULL = 1,
    HFOC_NETIF_OUTPUT_BAD_STATE = 2,
    HFOC_NETIF_OUTPUT_INVALID_VERSION = 3,
    HFOC_NETIF_OUTPUT_ALREADY_PUBLISHED = 4,
    HFOC_NETIF_OUTPUT_UNPUBLISHED = 5,
    HFOC_NETIF_OUTPUT_BAD_POOL = 6,
    HFOC_NETIF_OUTPUT_INVALID_ADAPTER = 7,
    HFOC_NETIF_OUTPUT_BAD_SLICE = 8,
    HFOC_NETIF_OUTPUT_WRONG_DIRECTION = 9,
    HFOC_NETIF_OUTPUT_ALREADY_SUBMITTED = 10,
    HFOC_NETIF_OUTPUT_NOT_SUBMITTED = 11,
    HFOC_NETIF_OUTPUT_INVALID_SUBMISSION = 12,
    HFOC_NETIF_OUTPUT_INVALID_COMPLETION = 13,
};

struct hfoc_netif_output_submission {
    uint32_t slot;
    uint32_t generation;
};

struct hfoc_netif_output_adapter {
    enum hfoc_netif_output_result (*submit)(
        void *context,
        struct packet_slice *slice,
        struct hfoc_netif_output_submission *submission);
    void *context;
};

struct hfoc_netif_output_pending {
    bool submitted;
    uint32_t generation;
    struct hfoc_netif_output_submission submission;
};

struct hfoc_netif_output {
    bool published;
    uint32_t version;
    struct packet_pool_descriptor *pool;
    struct hfoc_netif_output_adapter adapter;
    struct hfoc_netif_output_pending
        pending[PACKET_POOL_COUNT_MAX];
    uint32_t pending_count;
};

enum hfoc_netif_output_status hfoc_netif_output_init(
    struct hfoc_netif_output *output,
    struct packet_pool_descriptor *pool,
    const struct hfoc_netif_output_adapter *adapter);
enum hfoc_netif_output_status hfoc_netif_output_submit(
    struct hfoc_netif_output *output,
    struct packet_slice *slice,
    enum hfoc_netif_output_result *result);
enum hfoc_netif_output_status hfoc_netif_output_complete(
    struct hfoc_netif_output *output,
    const struct hfoc_netif_output_submission *submission);
bool hfoc_netif_output_published(
    const struct hfoc_netif_output *output);
uint32_t hfoc_netif_output_pending_count(
    const struct hfoc_netif_output *output);
const char *hfoc_netif_output_result_string(
    enum hfoc_netif_output_result result);
const char *hfoc_netif_output_status_string(
    enum hfoc_netif_output_status status);

#endif
