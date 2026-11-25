#include "icmp_reachability.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool icmp_reachability_policy_valid(
    const struct icmp_reachability_policy *policy)
{
    return policy != NULL &&
           policy->version == ICMP_REACHABILITY_VERSION &&
           policy->rate_budget >= ICMP_REACHABILITY_RATE_BUDGET_MIN &&
           policy->rate_budget <= ICMP_REACHABILITY_RATE_BUDGET_MAX &&
           policy->rate_window_ticks >= ICMP_REACHABILITY_WINDOW_MIN &&
           policy->rate_window_ticks <= ICMP_REACHABILITY_WINDOW_MAX;
}

static bool icmp_reachability_engine_valid(
    const struct icmp_reachability_engine *engine)
{
    return engine != NULL &&
           engine->published &&
           engine->version == ICMP_REACHABILITY_VERSION &&
           icmp_reachability_policy_valid(&engine->policy) &&
           engine->window_count <= engine->policy.rate_budget;
}

static uint16_t icmp_reachability_checksum(
    const uint8_t *data, size_t length)
{
    uint32_t sum = 0U;
    size_t offset;

    for (offset = 0U; offset + 1U < length; offset += 2U) {
        sum += ((uint32_t)data[offset] << 8U) |
               (uint32_t)data[offset + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static bool icmp_reachability_checksum_valid(
    const uint8_t *data, size_t length)
{
    return icmp_reachability_checksum(data, length) == 0U;
}

static bool icmp_reachability_ranges_overlap(
    const void *first,
    size_t first_length,
    const void *second,
    size_t second_length)
{
    uintptr_t first_start = (uintptr_t)first;
    uintptr_t first_end = first_start + first_length;
    uintptr_t second_start = (uintptr_t)second;
    uintptr_t second_end = second_start + second_length;

    return first_start < second_end && second_start < first_end;
}

enum icmp_reachability_status icmp_reachability_engine_init(
    struct icmp_reachability_engine *engine,
    const struct icmp_reachability_policy *policy)
{
    if (engine == NULL || policy == NULL) {
        return ICMP_REACHABILITY_NULL;
    }
    if (engine->published) {
        return ICMP_REACHABILITY_ALREADY_PUBLISHED;
    }
    if (policy->version != ICMP_REACHABILITY_VERSION) {
        return ICMP_REACHABILITY_BAD_VERSION;
    }
    if (policy->rate_budget < ICMP_REACHABILITY_RATE_BUDGET_MIN ||
        policy->rate_budget > ICMP_REACHABILITY_RATE_BUDGET_MAX) {
        return ICMP_REACHABILITY_BAD_BUDGET;
    }
    if (policy->rate_window_ticks < ICMP_REACHABILITY_WINDOW_MIN ||
        policy->rate_window_ticks > ICMP_REACHABILITY_WINDOW_MAX) {
        return ICMP_REACHABILITY_BAD_WINDOW;
    }

    engine->version = ICMP_REACHABILITY_VERSION;
    engine->policy = *policy;
    engine->window_start = 0U;
    engine->window_count = 0U;
    engine->published = true;
    return ICMP_REACHABILITY_OK;
}

enum icmp_reachability_status icmp_reachability_handle_request(
    struct icmp_reachability_engine *engine,
    const void *request,
    size_t request_length,
    bool local_management_authorized,
    uint32_t now_ticks,
    void *reply,
    size_t reply_capacity,
    size_t *reply_length)
{
    const uint8_t *input;
    uint8_t *output;
    uint32_t window;
    uint16_t checksum;
    size_t offset;

    if (engine == NULL || request == NULL ||
        reply == NULL || reply_length == NULL) {
        return ICMP_REACHABILITY_NULL;
    }
    if (!engine->published) {
        return ICMP_REACHABILITY_UNPUBLISHED;
    }
    if (!icmp_reachability_engine_valid(engine)) {
        return ICMP_REACHABILITY_BAD_STATE;
    }
    if (!local_management_authorized) {
        return ICMP_REACHABILITY_DENIED;
    }
    if (request_length < ICMP_REACHABILITY_HEADER_SIZE) {
        return ICMP_REACHABILITY_TOO_SHORT;
    }
    if (request_length > ICMP_REACHABILITY_MESSAGE_MAX) {
        return ICMP_REACHABILITY_TOO_LONG;
    }

    input = (const uint8_t *)request;
    if (input[0] != (uint8_t)ICMP_REACHABILITY_TYPE_ECHO_REQUEST) {
        return ICMP_REACHABILITY_BAD_TYPE;
    }
    if (input[1] != 0U) {
        return ICMP_REACHABILITY_BAD_CODE;
    }
    if (!icmp_reachability_checksum_valid(
            input, request_length)) {
        return ICMP_REACHABILITY_BAD_CHECKSUM;
    }
    if (reply_capacity < request_length) {
        return ICMP_REACHABILITY_OUTPUT_TOO_SMALL;
    }
    if (icmp_reachability_ranges_overlap(
            request, request_length, reply, request_length)) {
        return ICMP_REACHABILITY_OVERLAPPING_BUFFERS;
    }

    window = engine->policy.rate_window_ticks;
    if (engine->window_count != 0U &&
        (uint32_t)(now_ticks - engine->window_start) < window) {
        if (engine->window_count >= engine->policy.rate_budget) {
            return ICMP_REACHABILITY_RATE_LIMITED;
        }
    }

    output = (uint8_t *)reply;
    output[0] = (uint8_t)ICMP_REACHABILITY_TYPE_ECHO_REPLY;
    output[1] = 0U;
    output[2] = 0U;
    output[3] = 0U;
    output[4] = input[4];
    output[5] = input[5];
    output[6] = input[6];
    output[7] = input[7];
    for (offset = ICMP_REACHABILITY_HEADER_SIZE;
         offset < request_length; ++offset) {
        output[offset] = input[offset];
    }

    checksum = icmp_reachability_checksum(output, request_length);
    output[2] = (uint8_t)(checksum >> 8U);
    output[3] = (uint8_t)(checksum & 0xFFU);
    *reply_length = request_length;

    if (engine->window_count != 0U &&
        (uint32_t)(now_ticks - engine->window_start) < window) {
        ++engine->window_count;
    } else {
        engine->window_start = now_ticks;
        engine->window_count = 1U;
    }
    return ICMP_REACHABILITY_OK;
}

bool icmp_reachability_published(
    const struct icmp_reachability_engine *engine)
{
    return icmp_reachability_engine_valid(engine);
}

uint32_t icmp_reachability_window_count(
    const struct icmp_reachability_engine *engine)
{
    return icmp_reachability_engine_valid(engine) ?
        engine->window_count : 0U;
}

const char *icmp_reachability_status_string(
    enum icmp_reachability_status status)
{
    switch (status) {
    case ICMP_REACHABILITY_OK:
        return "ICMP_REACHABILITY_OK";
    case ICMP_REACHABILITY_NULL:
        return "ICMP_REACHABILITY_NULL";
    case ICMP_REACHABILITY_BAD_STATE:
        return "ICMP_REACHABILITY_BAD_STATE";
    case ICMP_REACHABILITY_BAD_VERSION:
        return "ICMP_REACHABILITY_BAD_VERSION";
    case ICMP_REACHABILITY_BAD_BUDGET:
        return "ICMP_REACHABILITY_BAD_BUDGET";
    case ICMP_REACHABILITY_BAD_WINDOW:
        return "ICMP_REACHABILITY_BAD_WINDOW";
    case ICMP_REACHABILITY_ALREADY_PUBLISHED:
        return "ICMP_REACHABILITY_ALREADY_PUBLISHED";
    case ICMP_REACHABILITY_UNPUBLISHED:
        return "ICMP_REACHABILITY_UNPUBLISHED";
    case ICMP_REACHABILITY_DENIED:
        return "ICMP_REACHABILITY_DENIED";
    case ICMP_REACHABILITY_RATE_LIMITED:
        return "ICMP_REACHABILITY_RATE_LIMITED";
    case ICMP_REACHABILITY_MALFORMED:
        return "ICMP_REACHABILITY_MALFORMED";
    case ICMP_REACHABILITY_TOO_SHORT:
        return "ICMP_REACHABILITY_TOO_SHORT";
    case ICMP_REACHABILITY_TOO_LONG:
        return "ICMP_REACHABILITY_TOO_LONG";
    case ICMP_REACHABILITY_BAD_CHECKSUM:
        return "ICMP_REACHABILITY_BAD_CHECKSUM";
    case ICMP_REACHABILITY_BAD_TYPE:
        return "ICMP_REACHABILITY_BAD_TYPE";
    case ICMP_REACHABILITY_BAD_CODE:
        return "ICMP_REACHABILITY_BAD_CODE";
    case ICMP_REACHABILITY_OUTPUT_TOO_SMALL:
        return "ICMP_REACHABILITY_OUTPUT_TOO_SMALL";
    case ICMP_REACHABILITY_OVERLAPPING_BUFFERS:
        return "ICMP_REACHABILITY_OVERLAPPING_BUFFERS";
    default:
        return NULL;
    }
}
