#include "hfoc_lwip_tcp_deadline.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_DEADLINE_HOST_TEST) || \
    defined(HFOC_TCP_LISTENER_QEMU_TEST)

#define HFOC_LWIP_TCP_DEADLINE_HALF_RANGE UINT32_C(0x7FFFFFFF)

static bool hfoc_lwip_tcp_deadline_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_POOL_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_POOL_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_deadline_policy_valid(
    const struct hfoc_lwip_tcp_deadline_policy *policy)
{
    return policy != NULL &&
           policy->version == HFOC_LWIP_TCP_DEADLINE_VERSION &&
           policy->accept_timeout_ms > 0U &&
           policy->accept_timeout_ms <=
               HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX &&
           policy->progress_timeout_ms > 0U &&
           policy->progress_timeout_ms <=
               HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX &&
           policy->idle_timeout_ms > 0U &&
           policy->idle_timeout_ms <=
               HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX &&
           policy->close_timeout_ms > 0U &&
           policy->close_timeout_ms <=
               HFOC_LWIP_TCP_DEADLINE_TIMEOUT_MAX;
}

static bool hfoc_lwip_tcp_deadline_entry_valid(
    const struct hfoc_lwip_tcp_deadline_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U;
    }
    return hfoc_lwip_tcp_deadline_generation_valid(
               entry->generation) &&
           hfoc_lwip_tcp_deadline_policy_valid(&entry->policy);
}

static bool hfoc_lwip_tcp_deadline_valid(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler)
{
    uint32_t active_count;
    uint32_t index;

    if (scheduler == NULL ||
        !scheduler->published ||
        scheduler->version != HFOC_LWIP_TCP_DEADLINE_VERSION ||
        scheduler->active_count > HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT) {
        return false;
    }

    active_count = 0U;
    for (index = 0U; index < HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_deadline_entry *entry =
            &scheduler->entries[index];

        if (!hfoc_lwip_tcp_deadline_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == scheduler->active_count;
}

static bool hfoc_lwip_tcp_deadline_reached(
    uint32_t now_ms,
    uint32_t due_ms)
{
    return (uint32_t)(now_ms - due_ms) <=
        HFOC_LWIP_TCP_DEADLINE_HALF_RANGE;
}

static bool hfoc_lwip_tcp_deadline_before(
    uint32_t left,
    uint32_t right)
{
    return (uint32_t)(left - right) >
        HFOC_LWIP_TCP_DEADLINE_HALF_RANGE;
}

static enum hfoc_lwip_tcp_deadline_status
hfoc_lwip_tcp_deadline_validate_token(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    struct hfoc_lwip_tcp_deadline_entry **entry_out)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;

    if (scheduler == NULL || token == NULL || entry_out == NULL) {
        return HFOC_LWIP_TCP_DEADLINE_NULL;
    }
    if (!hfoc_lwip_tcp_deadline_valid(scheduler)) {
        return HFOC_LWIP_TCP_DEADLINE_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_deadline_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN;
    }
    entry = &scheduler->entries[token->index];
    if (entry->active && entry->generation != token->generation) {
        return HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN;
    }
    if (!entry->active) {
        return HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED;
    }

    *entry_out = entry;
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

static uint32_t hfoc_lwip_tcp_deadline_due(
    uint32_t now_ms,
    uint32_t timeout_ms)
{
    return (uint32_t)(now_ms + timeout_ms);
}

static enum hfoc_lwip_tcp_deadline_kind
hfoc_lwip_tcp_deadline_entry_earliest(
    const struct hfoc_lwip_tcp_deadline_entry *entry)
{
    enum hfoc_lwip_tcp_deadline_kind best =
        HFOC_LWIP_TCP_DEADLINE_KIND_NONE;
    uint32_t best_due = HFOC_LWIP_TCP_DEADLINE_NEXT_NONE;

    if (entry->closing) {
        if (entry->close_deadline_ms !=
            HFOC_LWIP_TCP_DEADLINE_DISABLED) {
            return HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE;
        }
        return HFOC_LWIP_TCP_DEADLINE_KIND_NONE;
    }
    if (entry->accept_deadline_ms !=
        HFOC_LWIP_TCP_DEADLINE_DISABLED) {
        best = HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT;
        best_due = entry->accept_deadline_ms;
    }
    if (entry->progress_deadline_ms !=
            HFOC_LWIP_TCP_DEADLINE_DISABLED &&
        (best == HFOC_LWIP_TCP_DEADLINE_KIND_NONE ||
         hfoc_lwip_tcp_deadline_before(
             entry->progress_deadline_ms, best_due))) {
        best = HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS;
        best_due = entry->progress_deadline_ms;
    }
    if (entry->idle_deadline_ms !=
            HFOC_LWIP_TCP_DEADLINE_DISABLED &&
        (best == HFOC_LWIP_TCP_DEADLINE_KIND_NONE ||
         hfoc_lwip_tcp_deadline_before(
             entry->idle_deadline_ms, best_due))) {
        best = HFOC_LWIP_TCP_DEADLINE_KIND_IDLE;
        best_due = entry->idle_deadline_ms;
    }
    return best;
}

static uint32_t hfoc_lwip_tcp_deadline_entry_due(
    const struct hfoc_lwip_tcp_deadline_entry *entry,
    enum hfoc_lwip_tcp_deadline_kind kind)
{
    switch (kind) {
    case HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT:
        return entry->accept_deadline_ms;
    case HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS:
        return entry->progress_deadline_ms;
    case HFOC_LWIP_TCP_DEADLINE_KIND_IDLE:
        return entry->idle_deadline_ms;
    case HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE:
        return entry->close_deadline_ms;
    case HFOC_LWIP_TCP_DEADLINE_KIND_NONE:
    default:
        return HFOC_LWIP_TCP_DEADLINE_NEXT_NONE;
    }
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_init(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler)
{
    if (scheduler == NULL) {
        return HFOC_LWIP_TCP_DEADLINE_NULL;
    }
    if (scheduler->published) {
        return HFOC_LWIP_TCP_DEADLINE_BAD_STATE;
    }

    memset(scheduler, 0, sizeof(*scheduler));
    scheduler->version = HFOC_LWIP_TCP_DEADLINE_VERSION;
    scheduler->published = true;
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_attach(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    const struct hfoc_lwip_tcp_deadline_policy *policy,
    uint32_t now_ms)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;

    if (scheduler == NULL || token == NULL || policy == NULL) {
        return HFOC_LWIP_TCP_DEADLINE_NULL;
    }
    if (!hfoc_lwip_tcp_deadline_valid(scheduler)) {
        return HFOC_LWIP_TCP_DEADLINE_BAD_STATE;
    }
    if (token->index >= HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT ||
        !hfoc_lwip_tcp_deadline_generation_valid(
            token->generation)) {
        return HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN;
    }
    if (!hfoc_lwip_tcp_deadline_policy_valid(policy)) {
        return HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY;
    }

    entry = &scheduler->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED
            : HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN;
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->accept_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, policy->accept_timeout_ms);
    entry->progress_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, policy->progress_timeout_ms);
    entry->idle_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, policy->idle_timeout_ms);
    entry->close_deadline_ms =
        HFOC_LWIP_TCP_DEADLINE_DISABLED;
    entry->policy = *policy;
    ++scheduler->active_count;
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_touch(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;
    enum hfoc_lwip_tcp_deadline_status token_status;

    token_status = hfoc_lwip_tcp_deadline_validate_token(
        scheduler, token, &entry);
    if (token_status != HFOC_LWIP_TCP_DEADLINE_OK) {
        return token_status;
    }
    if (entry->closing) {
        return HFOC_LWIP_TCP_DEADLINE_CLOSED;
    }

    entry->accept_deadline_ms =
        HFOC_LWIP_TCP_DEADLINE_DISABLED;
    entry->progress_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, entry->policy.progress_timeout_ms);
    entry->idle_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, entry->policy.idle_timeout_ms);
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_begin_close(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;
    enum hfoc_lwip_tcp_deadline_status token_status;

    token_status = hfoc_lwip_tcp_deadline_validate_token(
        scheduler, token, &entry);
    if (token_status != HFOC_LWIP_TCP_DEADLINE_OK) {
        return token_status;
    }
    if (entry->closing) {
        return HFOC_LWIP_TCP_DEADLINE_OK;
    }

    entry->closing = true;
    entry->accept_deadline_ms =
        HFOC_LWIP_TCP_DEADLINE_DISABLED;
    entry->progress_deadline_ms =
        HFOC_LWIP_TCP_DEADLINE_DISABLED;
    entry->idle_deadline_ms =
        HFOC_LWIP_TCP_DEADLINE_DISABLED;
    entry->close_deadline_ms =
        hfoc_lwip_tcp_deadline_due(
            now_ms, entry->policy.close_timeout_ms);
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_check(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token,
    uint32_t now_ms,
    enum hfoc_lwip_tcp_deadline_kind *expired_kind)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;
    enum hfoc_lwip_tcp_deadline_kind kind;
    enum hfoc_lwip_tcp_deadline_status token_status;

    if (expired_kind == NULL) {
        return HFOC_LWIP_TCP_DEADLINE_NULL;
    }
    token_status = hfoc_lwip_tcp_deadline_validate_token(
        scheduler, token, &entry);
    if (token_status != HFOC_LWIP_TCP_DEADLINE_OK) {
        return token_status;
    }

    kind = hfoc_lwip_tcp_deadline_entry_earliest(entry);
    if (kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE ||
        !hfoc_lwip_tcp_deadline_reached(
            now_ms,
            hfoc_lwip_tcp_deadline_entry_due(entry, kind))) {
        *expired_kind = HFOC_LWIP_TCP_DEADLINE_KIND_NONE;
        return HFOC_LWIP_TCP_DEADLINE_OK;
    }
    *expired_kind = kind;
    return HFOC_LWIP_TCP_DEADLINE_EXPIRED;
}

enum hfoc_lwip_tcp_deadline_status hfoc_lwip_tcp_deadline_cancel(
    struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_deadline_entry *entry;
    enum hfoc_lwip_tcp_deadline_status token_status;

    token_status = hfoc_lwip_tcp_deadline_validate_token(
        scheduler, token, &entry);
    if (token_status != HFOC_LWIP_TCP_DEADLINE_OK) {
        return token_status;
    }

    memset(entry, 0, sizeof(*entry));
    --scheduler->active_count;
    return HFOC_LWIP_TCP_DEADLINE_OK;
}

bool hfoc_lwip_tcp_deadline_published(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler)
{
    return hfoc_lwip_tcp_deadline_valid(scheduler);
}

uint32_t hfoc_lwip_tcp_deadline_active_count(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler)
{
    return hfoc_lwip_tcp_deadline_valid(scheduler)
        ? scheduler->active_count
        : 0U;
}

uint32_t hfoc_lwip_tcp_deadline_next_deadline_ms(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    uint32_t now_ms)
{
    uint32_t best = HFOC_LWIP_TCP_DEADLINE_NEXT_NONE;
    enum hfoc_lwip_tcp_deadline_kind best_kind =
        HFOC_LWIP_TCP_DEADLINE_KIND_NONE;
    uint32_t index;

    (void)now_ms;
    if (!hfoc_lwip_tcp_deadline_valid(scheduler)) {
        return HFOC_LWIP_TCP_DEADLINE_NEXT_NONE;
    }
    for (index = 0U; index < HFOC_LWIP_TCP_DEADLINE_SLOT_COUNT;
         ++index) {
        const struct hfoc_lwip_tcp_deadline_entry *entry =
            &scheduler->entries[index];
        enum hfoc_lwip_tcp_deadline_kind kind;
        uint32_t due;

        if (!entry->active) {
            continue;
        }
        kind = hfoc_lwip_tcp_deadline_entry_earliest(entry);
        if (kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE) {
            continue;
        }
        due = hfoc_lwip_tcp_deadline_entry_due(entry, kind);
        if (best_kind == HFOC_LWIP_TCP_DEADLINE_KIND_NONE ||
            hfoc_lwip_tcp_deadline_before(due, best)) {
            best = due;
            best_kind = kind;
        }
    }
    return best;
}

struct hfoc_lwip_tcp_deadline_snapshot
hfoc_lwip_tcp_deadline_snapshot(
    const struct hfoc_lwip_tcp_deadline_scheduler *scheduler,
    uint32_t now_ms)
{
    struct hfoc_lwip_tcp_deadline_snapshot snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!hfoc_lwip_tcp_deadline_valid(scheduler)) {
        snapshot.next_deadline_ms =
            HFOC_LWIP_TCP_DEADLINE_NEXT_NONE;
        return snapshot;
    }
    snapshot.version = HFOC_LWIP_TCP_DEADLINE_VERSION;
    snapshot.active_count = scheduler->active_count;
    snapshot.next_deadline_ms =
        hfoc_lwip_tcp_deadline_next_deadline_ms(
            scheduler, now_ms);
    return snapshot;
}

const char *hfoc_lwip_tcp_deadline_kind_string(
    enum hfoc_lwip_tcp_deadline_kind kind)
{
    switch (kind) {
    case HFOC_LWIP_TCP_DEADLINE_KIND_NONE:
        return "HFOC_LWIP_TCP_DEADLINE_KIND_NONE";
    case HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT:
        return "HFOC_LWIP_TCP_DEADLINE_KIND_ACCEPT";
    case HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS:
        return "HFOC_LWIP_TCP_DEADLINE_KIND_PROGRESS";
    case HFOC_LWIP_TCP_DEADLINE_KIND_IDLE:
        return "HFOC_LWIP_TCP_DEADLINE_KIND_IDLE";
    case HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE:
        return "HFOC_LWIP_TCP_DEADLINE_KIND_CLOSE";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_deadline_status_string(
    enum hfoc_lwip_tcp_deadline_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_DEADLINE_OK:
        return "HFOC_LWIP_TCP_DEADLINE_OK";
    case HFOC_LWIP_TCP_DEADLINE_NULL:
        return "HFOC_LWIP_TCP_DEADLINE_NULL";
    case HFOC_LWIP_TCP_DEADLINE_BAD_STATE:
        return "HFOC_LWIP_TCP_DEADLINE_BAD_STATE";
    case HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN:
        return "HFOC_LWIP_TCP_DEADLINE_BAD_TOKEN";
    case HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN:
        return "HFOC_LWIP_TCP_DEADLINE_STALE_TOKEN";
    case HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED:
        return "HFOC_LWIP_TCP_DEADLINE_NOT_ATTACHED";
    case HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED:
        return "HFOC_LWIP_TCP_DEADLINE_ALREADY_ATTACHED";
    case HFOC_LWIP_TCP_DEADLINE_CLOSED:
        return "HFOC_LWIP_TCP_DEADLINE_CLOSED";
    case HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY:
        return "HFOC_LWIP_TCP_DEADLINE_INVALID_POLICY";
    case HFOC_LWIP_TCP_DEADLINE_EXPIRED:
        return "HFOC_LWIP_TCP_DEADLINE_EXPIRED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_deadline_not_compiled;

#endif
