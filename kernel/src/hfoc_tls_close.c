#include "hfoc_tls_close.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_TLS_CLOSE_HOST_TEST)

#include "mbedtls/ssl.h"

#define HFOC_TLS_CLOSE_HALF_RANGE UINT32_C(0x7FFFFFFF)

static bool hfoc_tls_close_generation_valid(uint32_t generation)
{
    return generation >= HFOC_TLS_CLOSE_GENERATION_MIN &&
           generation <= HFOC_TLS_CLOSE_GENERATION_MAX;
}

static bool hfoc_tls_close_deadline_valid(uint32_t deadline_ticks)
{
    return deadline_ticks > 0U &&
           deadline_ticks <= HFOC_TLS_CLOSE_DEADLINE_MAX;
}

static bool hfoc_tls_close_step_limit_valid(uint32_t max_steps)
{
    return max_steps > 0U &&
           max_steps <= HFOC_TLS_CLOSE_STEP_LIMIT_MAX;
}

static bool hfoc_tls_close_config_valid(
    const struct hfoc_tls_close_config *config,
    const struct hfoc_tls_close_token *token)
{
    const struct hfoc_tls_handshake *handshake;
    const struct hfoc_tls_handshake_token *handshake_token;
    const struct hfoc_mbedtls_bio *bio;
    const struct hfoc_mbedtls_bio_token *bio_token;
    enum hfoc_tls_handshake_state handshake_state;
    enum hfoc_mbedtls_bio_state bio_state;

    if (config == NULL ||
        token == NULL ||
        config->version != HFOC_TLS_CLOSE_VERSION ||
        !hfoc_tls_close_deadline_valid(config->deadline_ticks) ||
        !hfoc_tls_close_step_limit_valid(config->max_steps) ||
        config->handshake == NULL ||
        config->handshake_token == NULL ||
        config->bio == NULL ||
        config->bio_token == NULL ||
        config->step == NULL ||
        config->zeroize == NULL) {
        return false;
    }

    handshake = config->handshake;
    handshake_token = config->handshake_token;
    bio = config->bio;
    bio_token = config->bio_token;

    if (!hfoc_tls_handshake_published(handshake) ||
        handshake_token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(handshake_token->generation) ||
        bio_token->index != handshake_token->index ||
        bio_token->generation != handshake_token->generation ||
        !hfoc_mbedtls_bio_published(bio)) {
        return false;
    }

    handshake_state = hfoc_tls_handshake_state(
        handshake, handshake_token);
    bio_state = hfoc_mbedtls_bio_state(bio, bio_token);
    return handshake_state == HFOC_TLS_HANDSHAKE_STATE_SUCCESS &&
           hfoc_tls_handshake_generation(
               handshake, handshake_token) ==
               handshake_token->generation &&
           bio_state != HFOC_MBEDTLS_BIO_STATE_FREE &&
           bio_state != HFOC_MBEDTLS_BIO_STATE_FAILED;
}

static bool hfoc_tls_close_entry_valid(
    const struct hfoc_tls_close_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        if (entry->state == HFOC_TLS_CLOSE_STATE_FREE) {
            return entry->generation == 0U &&
                   entry->due_ticks == 0U &&
                   entry->max_steps == 0U &&
                   entry->step_count == 0U &&
                   entry->outcome == HFOC_TLS_CLOSE_OUTCOME_NONE &&
                   entry->handshake == NULL &&
                   entry->step == NULL &&
                   entry->step_context == NULL &&
                   entry->zeroize == NULL &&
                   entry->zeroize_context == NULL;
        }
        return (entry->state == HFOC_TLS_CLOSE_STATE_FAILED ||
                entry->state == HFOC_TLS_CLOSE_STATE_ABORTED) &&
               hfoc_tls_close_generation_valid(entry->generation) &&
               entry->due_ticks == 0U &&
               entry->max_steps == 0U &&
               entry->step_count == 0U &&
               entry->outcome != HFOC_TLS_CLOSE_OUTCOME_NONE &&
               entry->handshake == NULL &&
               entry->step == NULL &&
               entry->step_context == NULL &&
               entry->zeroize == NULL &&
               entry->zeroize_context == NULL;
    }

    if (!hfoc_tls_close_generation_valid(entry->generation) ||
        (entry->state != HFOC_TLS_CLOSE_STATE_RESERVED &&
         entry->state != HFOC_TLS_CLOSE_STATE_CLOSING &&
         entry->state != HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT)) {
        return false;
    }
    if (entry->state == HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT) {
        return entry->outcome ==
                   HFOC_TLS_CLOSE_OUTCOME_CLOSE_NOTIFY_SENT &&
               entry->handshake != NULL &&
               entry->step != NULL &&
               entry->zeroize != NULL;
    }
    if (entry->outcome != HFOC_TLS_CLOSE_OUTCOME_NONE) {
        return false;
    }
    if (entry->state == HFOC_TLS_CLOSE_STATE_CLOSING) {
        return entry->handshake != NULL &&
               entry->step != NULL &&
               entry->zeroize != NULL;
    }
    return true;
}

static bool hfoc_tls_close_valid(
    const struct hfoc_tls_close *close)
{
    uint32_t active_count = 0U;
    uint32_t index;

    if (close == NULL ||
        !close->published ||
        close->version != HFOC_TLS_CLOSE_VERSION ||
        close->active_count > HFOC_TLS_CLOSE_SLOT_COUNT) {
        return false;
    }
    for (index = 0U; index < HFOC_TLS_CLOSE_SLOT_COUNT; ++index) {
        const struct hfoc_tls_close_entry *entry =
            &close->entries[index];

        if (!hfoc_tls_close_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == close->active_count;
}

static enum hfoc_tls_close_status
hfoc_tls_close_validate_token(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token,
    struct hfoc_tls_close_entry **entry_out)
{
    struct hfoc_tls_close_entry *entry;

    if (close == NULL || token == NULL || entry_out == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    if (!hfoc_tls_close_valid(close)) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    if (token->index >= HFOC_TLS_CLOSE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(token->generation)) {
        return HFOC_TLS_CLOSE_BAD_TOKEN;
    }
    entry = &close->entries[token->index];
    if (!entry->active) {
        return HFOC_TLS_CLOSE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_TLS_CLOSE_STALE_TOKEN;
    }
    *entry_out = entry;
    return HFOC_TLS_CLOSE_OK;
}

static bool hfoc_tls_close_deadline_reached(
    uint32_t now_ticks,
    uint32_t due_ticks)
{
    return (uint32_t)(now_ticks - due_ticks) <=
        HFOC_TLS_CLOSE_HALF_RANGE;
}

static void hfoc_tls_close_cleanup(
    struct hfoc_tls_close *close,
    struct hfoc_tls_close_entry *entry,
    enum hfoc_tls_close_state state,
    enum hfoc_tls_close_outcome outcome,
    bool count_failure,
    bool count_abort)
{
    uint32_t generation = entry->generation;

    if (entry->zeroize != NULL) {
        entry->zeroize(entry->zeroize_context);
    }
    if (entry->handshake != NULL) {
        (void)hfoc_tls_handshake_release(
            (struct hfoc_tls_handshake *)entry->handshake,
            &entry->handshake_token);
    }

    memset(entry, 0, sizeof(*entry));
    if (state != HFOC_TLS_CLOSE_STATE_FREE) {
        entry->generation = generation;
    }
    entry->state = state;
    entry->outcome = outcome;
    --close->active_count;
    if (count_failure) {
        ++close->failure_count;
    }
    if (count_abort) {
        ++close->abort_count;
    }
}

enum hfoc_tls_close_status hfoc_tls_close_init(
    struct hfoc_tls_close *close)
{
    if (close == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    if (close->published) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    memset(close, 0, sizeof(*close));
    close->version = HFOC_TLS_CLOSE_VERSION;
    close->published = true;
    return HFOC_TLS_CLOSE_OK;
}

enum hfoc_tls_close_status hfoc_tls_close_reserve(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    struct hfoc_tls_close_entry *entry;

    if (close == NULL || token == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    if (!hfoc_tls_close_valid(close)) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    if (token->index >= HFOC_TLS_CLOSE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(token->generation)) {
        return HFOC_TLS_CLOSE_BAD_TOKEN;
    }

    entry = &close->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_TLS_CLOSE_ALREADY_RESERVED
            : HFOC_TLS_CLOSE_STALE_TOKEN;
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->state = HFOC_TLS_CLOSE_STATE_RESERVED;
    ++close->active_count;
    return HFOC_TLS_CLOSE_OK;
}

enum hfoc_tls_close_status hfoc_tls_close_configure(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token,
    const struct hfoc_tls_close_config *config,
    uint32_t now_ticks)
{
    struct hfoc_tls_close_entry *entry;
    enum hfoc_tls_close_status token_status;

    if (close == NULL || token == NULL || config == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    token_status = hfoc_tls_close_validate_token(
        close, token, &entry);
    if (token_status != HFOC_TLS_CLOSE_OK) {
        return token_status;
    }
    if (entry->state != HFOC_TLS_CLOSE_STATE_RESERVED) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    if (!hfoc_tls_close_config_valid(config, token)) {
        return HFOC_TLS_CLOSE_INVALID_CONFIG;
    }

    entry->due_ticks =
        (uint32_t)(now_ticks + config->deadline_ticks);
    entry->max_steps = config->max_steps;
    entry->step_count = 0U;
    entry->handshake = config->handshake;
    entry->handshake_token = *config->handshake_token;
    entry->bio = config->bio;
    entry->bio_token = *config->bio_token;
    entry->step = config->step;
    entry->step_context = config->step_context;
    entry->zeroize = config->zeroize;
    entry->zeroize_context = config->zeroize_context;
    entry->state = HFOC_TLS_CLOSE_STATE_RESERVED;
    return HFOC_TLS_CLOSE_OK;
}

enum hfoc_tls_close_status hfoc_tls_close_progress(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token,
    uint32_t now_ticks)
{
    struct hfoc_tls_close_entry *entry;
    enum hfoc_tls_close_status token_status;
    enum hfoc_tls_close_step_result step_result;

    if (close == NULL || token == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    token_status = hfoc_tls_close_validate_token(
        close, token, &entry);
    if (token_status != HFOC_TLS_CLOSE_OK) {
        return token_status;
    }
    if (entry->state != HFOC_TLS_CLOSE_STATE_RESERVED &&
        entry->state != HFOC_TLS_CLOSE_STATE_CLOSING) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    if (entry->step == NULL || entry->handshake == NULL) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }
    if (hfoc_tls_close_deadline_reached(
            now_ticks, entry->due_ticks)) {
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_DEADLINE_EXPIRED,
            true,
            false);
        return HFOC_TLS_CLOSE_DEADLINE_EXPIRED;
    }
    if (entry->step_count >= entry->max_steps) {
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_STEP_LIMIT,
            true,
            false);
        return HFOC_TLS_CLOSE_STEP_LIMIT;
    }

    step_result = entry->step(entry->step_context);
    if (step_result != HFOC_TLS_CLOSE_STEP_WANT_READ &&
        step_result != HFOC_TLS_CLOSE_STEP_WANT_WRITE &&
        step_result != HFOC_TLS_CLOSE_STEP_PROGRESS &&
        step_result != HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT &&
        step_result != HFOC_TLS_CLOSE_STEP_TRANSPORT_CLOSED &&
        step_result != HFOC_TLS_CLOSE_STEP_TRANSPORT_REJECTED &&
        step_result != HFOC_TLS_CLOSE_STEP_FAILED) {
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_INVALID_PROGRESS,
            true,
            false);
        return HFOC_TLS_CLOSE_INVALID_PROGRESS;
    }

    ++entry->step_count;
    switch (step_result) {
    case HFOC_TLS_CLOSE_STEP_WANT_READ:
    case HFOC_TLS_CLOSE_STEP_WANT_WRITE:
    case HFOC_TLS_CLOSE_STEP_PROGRESS:
        entry->state = HFOC_TLS_CLOSE_STATE_CLOSING;
        return HFOC_TLS_CLOSE_OK;
    case HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT:
        entry->state = HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT;
        entry->outcome = HFOC_TLS_CLOSE_OUTCOME_CLOSE_NOTIFY_SENT;
        ++close->close_notify_count;
        return HFOC_TLS_CLOSE_OK;
    case HFOC_TLS_CLOSE_STEP_TRANSPORT_CLOSED:
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_CLOSED,
            true,
            false);
        return HFOC_TLS_CLOSE_TRANSPORT_CLOSED;
    case HFOC_TLS_CLOSE_STEP_TRANSPORT_REJECTED:
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_REJECTED,
            true,
            false);
        return HFOC_TLS_CLOSE_TRANSPORT_REJECTED;
    case HFOC_TLS_CLOSE_STEP_FAILED:
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FAILED,
            HFOC_TLS_CLOSE_OUTCOME_ABORTED,
            true,
            false);
        return HFOC_TLS_CLOSE_CLOSE_FAILED;
    default:
        break;
    }
    return HFOC_TLS_CLOSE_INVALID_PROGRESS;
}

enum hfoc_tls_close_status hfoc_tls_close_abort(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    struct hfoc_tls_close_entry *entry;
    enum hfoc_tls_close_status token_status;

    if (close == NULL || token == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    token_status = hfoc_tls_close_validate_token(
        close, token, &entry);
    if (token_status == HFOC_TLS_CLOSE_NOT_ATTACHED) {
        return HFOC_TLS_CLOSE_OK;
    }
    if (token_status != HFOC_TLS_CLOSE_OK) {
        return token_status;
    }
    if (entry->state == HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT) {
        return HFOC_TLS_CLOSE_BAD_STATE;
    }

    hfoc_tls_close_cleanup(
        close,
        entry,
        HFOC_TLS_CLOSE_STATE_ABORTED,
        HFOC_TLS_CLOSE_OUTCOME_ABORTED,
        false,
        true);
    return HFOC_TLS_CLOSE_ABORTED;
}

enum hfoc_tls_close_status hfoc_tls_close_release(
    struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    struct hfoc_tls_close_entry *entry;
    enum hfoc_tls_close_status token_status;

    if (close == NULL || token == NULL) {
        return HFOC_TLS_CLOSE_NULL;
    }
    token_status = hfoc_tls_close_validate_token(
        close, token, &entry);
    if (token_status == HFOC_TLS_CLOSE_NOT_ATTACHED) {
        return HFOC_TLS_CLOSE_OK;
    }
    if (token_status != HFOC_TLS_CLOSE_OK) {
        return token_status;
    }

    if (entry->state == HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT) {
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_FREE,
            HFOC_TLS_CLOSE_OUTCOME_NONE,
            false,
            false);
        return HFOC_TLS_CLOSE_OK;
    }
    if (entry->state == HFOC_TLS_CLOSE_STATE_RESERVED ||
        entry->state == HFOC_TLS_CLOSE_STATE_CLOSING) {
        hfoc_tls_close_cleanup(
            close,
            entry,
            HFOC_TLS_CLOSE_STATE_ABORTED,
            HFOC_TLS_CLOSE_OUTCOME_ABORTED,
            false,
            true);
        return HFOC_TLS_CLOSE_OK;
    }
    return HFOC_TLS_CLOSE_BAD_STATE;
}

bool hfoc_tls_close_published(
    const struct hfoc_tls_close *close)
{
    return hfoc_tls_close_valid(close);
}

uint32_t hfoc_tls_close_active_count(
    const struct hfoc_tls_close *close)
{
    return hfoc_tls_close_valid(close)
        ? close->active_count : 0U;
}

uint32_t hfoc_tls_close_close_notify_count(
    const struct hfoc_tls_close *close)
{
    return hfoc_tls_close_valid(close)
        ? close->close_notify_count : 0U;
}

uint32_t hfoc_tls_close_abort_count(
    const struct hfoc_tls_close *close)
{
    return hfoc_tls_close_valid(close)
        ? close->abort_count : 0U;
}

uint32_t hfoc_tls_close_failure_count(
    const struct hfoc_tls_close *close)
{
    return hfoc_tls_close_valid(close)
        ? close->failure_count : 0U;
}

enum hfoc_tls_close_state hfoc_tls_close_state_at(
    const struct hfoc_tls_close *close,
    uint32_t index)
{
    if (!hfoc_tls_close_valid(close) ||
        index >= HFOC_TLS_CLOSE_SLOT_COUNT) {
        return HFOC_TLS_CLOSE_STATE_FREE;
    }
    return close->entries[index].state;
}

enum hfoc_tls_close_outcome hfoc_tls_close_outcome_at(
    const struct hfoc_tls_close *close,
    uint32_t index)
{
    if (!hfoc_tls_close_valid(close) ||
        index >= HFOC_TLS_CLOSE_SLOT_COUNT) {
        return HFOC_TLS_CLOSE_OUTCOME_NONE;
    }
    return close->entries[index].outcome;
}

uint32_t hfoc_tls_close_generation_at(
    const struct hfoc_tls_close *close,
    uint32_t index)
{
    if (!hfoc_tls_close_valid(close) ||
        index >= HFOC_TLS_CLOSE_SLOT_COUNT) {
        return 0U;
    }
    return close->entries[index].generation;
}

enum hfoc_tls_close_state hfoc_tls_close_state(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    if (!hfoc_tls_close_valid(close) ||
        token == NULL ||
        token->index >= HFOC_TLS_CLOSE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(token->generation) ||
        close->entries[token->index].generation != token->generation) {
        return HFOC_TLS_CLOSE_STATE_FREE;
    }
    return close->entries[token->index].state;
}

enum hfoc_tls_close_outcome hfoc_tls_close_outcome(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    if (!hfoc_tls_close_valid(close) ||
        token == NULL ||
        token->index >= HFOC_TLS_CLOSE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(token->generation) ||
        close->entries[token->index].generation != token->generation) {
        return HFOC_TLS_CLOSE_OUTCOME_NONE;
    }
    return close->entries[token->index].outcome;
}

uint32_t hfoc_tls_close_generation(
    const struct hfoc_tls_close *close,
    const struct hfoc_tls_close_token *token)
{
    if (!hfoc_tls_close_valid(close) ||
        token == NULL ||
        token->index >= HFOC_TLS_CLOSE_SLOT_COUNT ||
        !hfoc_tls_close_generation_valid(token->generation) ||
        close->entries[token->index].generation != token->generation) {
        return 0U;
    }
    return close->entries[token->index].generation;
}

enum hfoc_tls_close_error hfoc_tls_close_error_normalize(
    int result)
{
    if (result == 0) {
        return HFOC_TLS_CLOSE_ERROR_SUCCESS;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_READ) {
        return HFOC_TLS_CLOSE_ERROR_WANT_READ;
    }
    if (result == MBEDTLS_ERR_SSL_WANT_WRITE) {
        return HFOC_TLS_CLOSE_ERROR_WANT_WRITE;
    }
    if (result == MBEDTLS_ERR_SSL_CONN_EOF) {
        return HFOC_TLS_CLOSE_ERROR_TRANSPORT_CLOSED;
    }
    if (result == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
        return HFOC_TLS_CLOSE_ERROR_PEER_CLOSE_NOTIFY;
    }
    if (result == MBEDTLS_ERR_SSL_BAD_INPUT_DATA ||
        result == MBEDTLS_ERR_SSL_ILLEGAL_PARAMETER ||
        result == MBEDTLS_ERR_SSL_BUFFER_TOO_SMALL) {
        return HFOC_TLS_CLOSE_ERROR_BAD_INPUT;
    }
    if (result == MBEDTLS_ERR_SSL_TIMEOUT) {
        return HFOC_TLS_CLOSE_ERROR_TIMEOUT;
    }

    switch (result) {
    case MBEDTLS_ERR_SSL_INVALID_MAC:
    case MBEDTLS_ERR_SSL_INVALID_RECORD:
    case MBEDTLS_ERR_SSL_DECODE_ERROR:
    case MBEDTLS_ERR_SSL_UNEXPECTED_MESSAGE:
    case MBEDTLS_ERR_SSL_UNEXPECTED_RECORD:
    case MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE:
    case MBEDTLS_ERR_SSL_BAD_CERTIFICATE:
    case MBEDTLS_ERR_SSL_BAD_PROTOCOL_VERSION:
    case MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE:
    case MBEDTLS_ERR_SSL_VERSION_MISMATCH:
    case MBEDTLS_ERR_SSL_PK_TYPE_MISMATCH:
    case MBEDTLS_ERR_SSL_UNKNOWN_IDENTITY:
    case MBEDTLS_ERR_SSL_UNSUPPORTED_EXTENSION:
        return HFOC_TLS_CLOSE_ERROR_PROTOCOL;
    case MBEDTLS_ERR_SSL_ALLOC_FAILED:
    case MBEDTLS_ERR_SSL_INTERNAL_ERROR:
    case MBEDTLS_ERR_SSL_FEATURE_UNAVAILABLE:
    case MBEDTLS_ERR_SSL_HW_ACCEL_FAILED:
    case MBEDTLS_ERR_SSL_NO_RNG:
    case MBEDTLS_ERR_SSL_CA_CHAIN_REQUIRED:
    case MBEDTLS_ERR_SSL_PRIVATE_KEY_REQUIRED:
    case MBEDTLS_ERR_SSL_BAD_CONFIG:
        return HFOC_TLS_CLOSE_ERROR_INTERNAL;
    default:
        return HFOC_TLS_CLOSE_ERROR_UNKNOWN;
    }
}

const char *hfoc_tls_close_state_string(
    enum hfoc_tls_close_state state)
{
    switch (state) {
    case HFOC_TLS_CLOSE_STATE_FREE:
        return "FREE";
    case HFOC_TLS_CLOSE_STATE_RESERVED:
        return "RESERVED";
    case HFOC_TLS_CLOSE_STATE_CLOSING:
        return "CLOSING";
    case HFOC_TLS_CLOSE_STATE_CLOSE_NOTIFY_SENT:
        return "CLOSE_NOTIFY_SENT";
    case HFOC_TLS_CLOSE_STATE_FAILED:
        return "FAILED";
    case HFOC_TLS_CLOSE_STATE_ABORTED:
        return "ABORTED";
    default:
        return NULL;
    }
}

const char *hfoc_tls_close_outcome_string(
    enum hfoc_tls_close_outcome outcome)
{
    switch (outcome) {
    case HFOC_TLS_CLOSE_OUTCOME_NONE:
        return "NONE";
    case HFOC_TLS_CLOSE_OUTCOME_CLOSE_NOTIFY_SENT:
        return "CLOSE_NOTIFY_SENT";
    case HFOC_TLS_CLOSE_OUTCOME_ABORTED:
        return "ABORTED";
    case HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_CLOSED:
        return "TRANSPORT_CLOSED";
    case HFOC_TLS_CLOSE_OUTCOME_TRANSPORT_REJECTED:
        return "TRANSPORT_REJECTED";
    case HFOC_TLS_CLOSE_OUTCOME_DEADLINE_EXPIRED:
        return "DEADLINE_EXPIRED";
    case HFOC_TLS_CLOSE_OUTCOME_STEP_LIMIT:
        return "STEP_LIMIT";
    case HFOC_TLS_CLOSE_OUTCOME_INVALID_PROGRESS:
        return "INVALID_PROGRESS";
    default:
        return NULL;
    }
}

const char *hfoc_tls_close_status_string(
    enum hfoc_tls_close_status status)
{
    switch (status) {
    case HFOC_TLS_CLOSE_OK:
        return "OK";
    case HFOC_TLS_CLOSE_NULL:
        return "NULL";
    case HFOC_TLS_CLOSE_BAD_STATE:
        return "BAD_STATE";
    case HFOC_TLS_CLOSE_BAD_TOKEN:
        return "BAD_TOKEN";
    case HFOC_TLS_CLOSE_STALE_TOKEN:
        return "STALE_TOKEN";
    case HFOC_TLS_CLOSE_NOT_ATTACHED:
        return "NOT_ATTACHED";
    case HFOC_TLS_CLOSE_ALREADY_RESERVED:
        return "ALREADY_RESERVED";
    case HFOC_TLS_CLOSE_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case HFOC_TLS_CLOSE_DEADLINE_EXPIRED:
        return "DEADLINE_EXPIRED";
    case HFOC_TLS_CLOSE_STEP_LIMIT:
        return "STEP_LIMIT";
    case HFOC_TLS_CLOSE_INVALID_PROGRESS:
        return "INVALID_PROGRESS";
    case HFOC_TLS_CLOSE_TRANSPORT_CLOSED:
        return "TRANSPORT_CLOSED";
    case HFOC_TLS_CLOSE_TRANSPORT_REJECTED:
        return "TRANSPORT_REJECTED";
    case HFOC_TLS_CLOSE_CLOSE_FAILED:
        return "CLOSE_FAILED";
    case HFOC_TLS_CLOSE_ABORTED:
        return "ABORTED";
    default:
        return NULL;
    }
}

const char *hfoc_tls_close_step_result_string(
    enum hfoc_tls_close_step_result result)
{
    switch (result) {
    case HFOC_TLS_CLOSE_STEP_WANT_READ:
        return "WANT_READ";
    case HFOC_TLS_CLOSE_STEP_WANT_WRITE:
        return "WANT_WRITE";
    case HFOC_TLS_CLOSE_STEP_PROGRESS:
        return "PROGRESS";
    case HFOC_TLS_CLOSE_STEP_CLOSE_NOTIFY_SENT:
        return "CLOSE_NOTIFY_SENT";
    case HFOC_TLS_CLOSE_STEP_TRANSPORT_CLOSED:
        return "TRANSPORT_CLOSED";
    case HFOC_TLS_CLOSE_STEP_TRANSPORT_REJECTED:
        return "TRANSPORT_REJECTED";
    case HFOC_TLS_CLOSE_STEP_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *hfoc_tls_close_error_string(
    enum hfoc_tls_close_error error)
{
    switch (error) {
    case HFOC_TLS_CLOSE_ERROR_NONE:
        return "NONE";
    case HFOC_TLS_CLOSE_ERROR_SUCCESS:
        return "SUCCESS";
    case HFOC_TLS_CLOSE_ERROR_WANT_READ:
        return "WANT_READ";
    case HFOC_TLS_CLOSE_ERROR_WANT_WRITE:
        return "WANT_WRITE";
    case HFOC_TLS_CLOSE_ERROR_TRANSPORT_CLOSED:
        return "TRANSPORT_CLOSED";
    case HFOC_TLS_CLOSE_ERROR_PEER_CLOSE_NOTIFY:
        return "PEER_CLOSE_NOTIFY";
    case HFOC_TLS_CLOSE_ERROR_BAD_INPUT:
        return "BAD_INPUT";
    case HFOC_TLS_CLOSE_ERROR_PROTOCOL:
        return "PROTOCOL";
    case HFOC_TLS_CLOSE_ERROR_TIMEOUT:
        return "TIMEOUT";
    case HFOC_TLS_CLOSE_ERROR_INTERNAL:
        return "INTERNAL";
    case HFOC_TLS_CLOSE_ERROR_UNKNOWN:
        return "UNKNOWN";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_tls_close_production_empty;

#endif
