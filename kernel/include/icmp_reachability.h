#ifndef HFOC_ICMP_REACHABILITY_H
#define HFOC_ICMP_REACHABILITY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ICMP_REACHABILITY_VERSION 1U
#define ICMP_REACHABILITY_HEADER_SIZE 8U
#define ICMP_REACHABILITY_MESSAGE_MAX 1024U
#define ICMP_REACHABILITY_PAYLOAD_MAX \
    (ICMP_REACHABILITY_MESSAGE_MAX - ICMP_REACHABILITY_HEADER_SIZE)
#define ICMP_REACHABILITY_RATE_BUDGET_MIN 1U
#define ICMP_REACHABILITY_RATE_BUDGET_MAX 1024U
#define ICMP_REACHABILITY_WINDOW_MIN 1U
#define ICMP_REACHABILITY_WINDOW_MAX UINT32_C(0x7FFFFFFF)

enum icmp_reachability_type {
    ICMP_REACHABILITY_TYPE_ECHO_REPLY = 0U,
    ICMP_REACHABILITY_TYPE_ECHO_REQUEST = 8U,
};

enum icmp_reachability_status {
    ICMP_REACHABILITY_OK = 0,
    ICMP_REACHABILITY_NULL = 1,
    ICMP_REACHABILITY_BAD_STATE = 2,
    ICMP_REACHABILITY_BAD_VERSION = 3,
    ICMP_REACHABILITY_BAD_BUDGET = 4,
    ICMP_REACHABILITY_BAD_WINDOW = 5,
    ICMP_REACHABILITY_ALREADY_PUBLISHED = 6,
    ICMP_REACHABILITY_UNPUBLISHED = 7,
    ICMP_REACHABILITY_DENIED = 8,
    ICMP_REACHABILITY_RATE_LIMITED = 9,
    ICMP_REACHABILITY_MALFORMED = 10,
    ICMP_REACHABILITY_TOO_SHORT = 11,
    ICMP_REACHABILITY_TOO_LONG = 12,
    ICMP_REACHABILITY_BAD_CHECKSUM = 13,
    ICMP_REACHABILITY_BAD_TYPE = 14,
    ICMP_REACHABILITY_BAD_CODE = 15,
    ICMP_REACHABILITY_OUTPUT_TOO_SMALL = 16,
    ICMP_REACHABILITY_OVERLAPPING_BUFFERS = 17,
};

struct icmp_reachability_policy {
    uint32_t version;
    uint32_t rate_budget;
    uint32_t rate_window_ticks;
};

struct icmp_reachability_engine {
    uint32_t version;
    struct icmp_reachability_policy policy;
    uint32_t window_start;
    uint32_t window_count;
    bool published;
};

enum icmp_reachability_status icmp_reachability_engine_init(
    struct icmp_reachability_engine *engine,
    const struct icmp_reachability_policy *policy);
enum icmp_reachability_status icmp_reachability_handle_request(
    struct icmp_reachability_engine *engine,
    const void *request,
    size_t request_length,
    bool local_management_authorized,
    uint32_t now_ticks,
    void *reply,
    size_t reply_capacity,
    size_t *reply_length);
bool icmp_reachability_published(
    const struct icmp_reachability_engine *engine);
uint32_t icmp_reachability_window_count(
    const struct icmp_reachability_engine *engine);
const char *icmp_reachability_status_string(
    enum icmp_reachability_status status);

#endif
