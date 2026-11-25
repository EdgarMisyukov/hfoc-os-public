#include "hfoc_tls_handshake.h"

#include "hfoc_mbedtls_alloc.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_TLS_HANDSHAKE_HOST_TEST)

#define HFOC_TLS_HANDSHAKE_HALF_RANGE UINT32_C(0x7FFFFFFF)

static bool hfoc_tls_handshake_generation_valid(uint32_t generation)
{
    return generation >= HFOC_TLS_HANDSHAKE_GENERATION_MIN &&
           generation <= HFOC_TLS_HANDSHAKE_GENERATION_MAX;
}

static bool hfoc_tls_handshake_drbg_generation_valid(
    uint32_t generation)
{
    return generation >= HFOC_TLS_HANDSHAKE_DRBG_GENERATION_MIN &&
           generation <= HFOC_TLS_HANDSHAKE_DRBG_GENERATION_MAX;
}

static bool hfoc_tls_handshake_drbg_source_valid(uint32_t source_id)
{
    return source_id >= HFOC_TLS_HANDSHAKE_DRBG_SOURCE_MIN &&
           source_id <= HFOC_TLS_HANDSHAKE_DRBG_SOURCE_MAX;
}

static bool hfoc_tls_handshake_deadline_valid(uint32_t deadline_ticks)
{
    return deadline_ticks > 0U &&
           deadline_ticks <= HFOC_TLS_HANDSHAKE_DEADLINE_MAX;
}

static bool hfoc_tls_handshake_step_limit_valid(uint32_t max_steps)
{
    return max_steps > 0U &&
           max_steps <= HFOC_TLS_HANDSHAKE_STEP_LIMIT_MAX;
}

static bool hfoc_tls_handshake_manifest_valid(
    const struct hfoc_mbedtls_manifest *manifest)
{
    return manifest != NULL &&
           manifest->published &&
           manifest->version == HFOC_MBEDTLS_MANIFEST_VERSION &&
           manifest->purpose_id ==
               HFOC_MBEDTLS_MANIFEST_PURPOSE_TLS_HANDSHAKE &&
           manifest->algorithm_id ==
               HFOC_MBEDTLS_MANIFEST_ALGORITHM_ECDSA_P256_SHA256;
}

static bool hfoc_tls_handshake_config_valid(
    const struct hfoc_tls_handshake_config *config,
    uint32_t slot_index,
    uint32_t generation)
{
    return config != NULL &&
           config->version == HFOC_TLS_HANDSHAKE_VERSION &&
           hfoc_tls_handshake_deadline_valid(config->deadline_ticks) &&
           hfoc_tls_handshake_step_limit_valid(config->max_steps) &&
           hfoc_tls_handshake_drbg_generation_valid(
               config->drbg_generation) &&
           hfoc_tls_handshake_drbg_source_valid(config->drbg_source_id) &&
           hfoc_tls_handshake_manifest_valid(config->manifest) &&
           config->bio != NULL &&
           hfoc_mbedtls_bio_published(config->bio) &&
           config->bio_token != NULL &&
           config->bio_token->index == slot_index &&
           config->bio_token->generation == generation &&
           hfoc_tls_handshake_generation_valid(
               config->bio_token->generation) &&
           config->bio_policy != NULL &&
           config->bio_transport != NULL &&
           config->step != NULL;
}

static bool hfoc_tls_handshake_entry_valid(
    const struct hfoc_tls_handshake_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->generation == 0U &&
               entry->allocator_id == 0U &&
               entry->bio_index == 0U &&
               entry->due_ticks == 0U &&
               entry->max_steps == 0U &&
               entry->step_count == 0U &&
               entry->drbg_generation == 0U &&
               entry->drbg_source_id == 0U &&
               !entry->success_published &&
               (entry->state == HFOC_TLS_HANDSHAKE_STATE_FREE ||
                entry->state == HFOC_TLS_HANDSHAKE_STATE_FAILED) &&
               (entry->state == HFOC_TLS_HANDSHAKE_STATE_FREE
                    ? entry->fault == HFOC_TLS_HANDSHAKE_FAULT_NONE
                    : entry->fault != HFOC_TLS_HANDSHAKE_FAULT_NONE) &&
               entry->manifest == NULL &&
               entry->bio == NULL &&
               entry->step == NULL &&
               entry->step_context == NULL;
    }

    return hfoc_tls_handshake_generation_valid(entry->generation) &&
           entry->allocator_id > 0U &&
           entry->allocator_id <= HFOC_MBEDTLS_CONNECTION_COUNT &&
           entry->fault == HFOC_TLS_HANDSHAKE_FAULT_NONE &&
           (entry->state == HFOC_TLS_HANDSHAKE_STATE_RESERVED ||
            entry->state == HFOC_TLS_HANDSHAKE_STATE_CONFIGURED ||
            entry->state == HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING ||
            entry->state == HFOC_TLS_HANDSHAKE_STATE_SUCCESS);
}

static bool hfoc_tls_handshake_valid(
    const struct hfoc_tls_handshake *handshake)
{
    uint32_t active_count = 0U;
    uint32_t index;

    if (handshake == NULL ||
        !handshake->published ||
        handshake->version != HFOC_TLS_HANDSHAKE_VERSION ||
        handshake->active_count > HFOC_TLS_HANDSHAKE_SLOT_COUNT) {
        return false;
    }
    for (index = 0U; index < HFOC_TLS_HANDSHAKE_SLOT_COUNT; ++index) {
        const struct hfoc_tls_handshake_entry *entry =
            &handshake->entries[index];

        if (!hfoc_tls_handshake_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            ++active_count;
        }
    }
    return active_count == handshake->active_count;
}

static enum hfoc_tls_handshake_status
hfoc_tls_handshake_validate_token(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token,
    struct hfoc_tls_handshake_entry **entry_out)
{
    struct hfoc_tls_handshake_entry *entry;

    if (handshake == NULL || token == NULL || entry_out == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    if (!hfoc_tls_handshake_valid(handshake)) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    if (token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_handshake_generation_valid(token->generation)) {
        return HFOC_TLS_HANDSHAKE_BAD_TOKEN;
    }
    entry = &handshake->entries[token->index];
    if (!entry->active) {
        return HFOC_TLS_HANDSHAKE_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_TLS_HANDSHAKE_STALE_TOKEN;
    }
    *entry_out = entry;
    return HFOC_TLS_HANDSHAKE_OK;
}

static bool hfoc_tls_handshake_deadline_reached(
    uint32_t now_ticks,
    uint32_t due_ticks)
{
    return (uint32_t)(now_ticks - due_ticks) <=
        HFOC_TLS_HANDSHAKE_HALF_RANGE;
}

static void hfoc_tls_handshake_cleanup(
    struct hfoc_tls_handshake *handshake,
    struct hfoc_tls_handshake_entry *entry,
    bool failed,
    enum hfoc_tls_handshake_fault fault)
{
    if (entry->bio != NULL) {
        (void)hfoc_mbedtls_bio_detach(
            (struct hfoc_mbedtls_bio *)entry->bio,
            &entry->bio_token);
    }
    if (entry->allocator_id != 0U) {
        (void)hfoc_mbedtls_alloc_connection_close(
            entry->allocator_id);
    }

    memset(entry, 0, sizeof(*entry));
    if (failed) {
        entry->state = HFOC_TLS_HANDSHAKE_STATE_FAILED;
        entry->fault = fault;
        ++handshake->failure_count;
    }
    --handshake->active_count;
}

static void hfoc_tls_handshake_fail(
    struct hfoc_tls_handshake *handshake,
    struct hfoc_tls_handshake_entry *entry,
    enum hfoc_tls_handshake_fault fault)
{
    hfoc_tls_handshake_cleanup(handshake, entry, true, fault);
}

enum hfoc_tls_handshake_status hfoc_tls_handshake_init(
    struct hfoc_tls_handshake *handshake)
{
    if (handshake == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    if (handshake->published) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    memset(handshake, 0, sizeof(*handshake));
    handshake->version = HFOC_TLS_HANDSHAKE_VERSION;
    handshake->published = true;
    return HFOC_TLS_HANDSHAKE_OK;
}

enum hfoc_tls_handshake_status hfoc_tls_handshake_reserve(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    struct hfoc_tls_handshake_entry *entry;
    uint32_t allocator_id = 0U;

    if (handshake == NULL || token == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    if (!hfoc_tls_handshake_valid(handshake)) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    if (token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_handshake_generation_valid(token->generation)) {
        return HFOC_TLS_HANDSHAKE_BAD_TOKEN;
    }

    entry = &handshake->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_TLS_HANDSHAKE_ALREADY_RESERVED
            : HFOC_TLS_HANDSHAKE_STALE_TOKEN;
    }
    if (!hfoc_mbedtls_alloc_connection_open(&allocator_id)) {
        return HFOC_TLS_HANDSHAKE_ALLOCATOR_UNAVAILABLE;
    }

    memset(entry, 0, sizeof(*entry));
    entry->active = true;
    entry->generation = token->generation;
    entry->allocator_id = allocator_id;
    entry->state = HFOC_TLS_HANDSHAKE_STATE_RESERVED;
    ++handshake->active_count;
    return HFOC_TLS_HANDSHAKE_OK;
}

enum hfoc_tls_handshake_status hfoc_tls_handshake_configure(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token,
    const struct hfoc_tls_handshake_config *config,
    uint32_t now_ticks)
{
    struct hfoc_tls_handshake_entry *entry;
    enum hfoc_tls_handshake_status token_status;
    enum hfoc_mbedtls_bio_status bio_status;

    if (handshake == NULL || token == NULL || config == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    token_status = hfoc_tls_handshake_validate_token(
        handshake, token, &entry);
    if (token_status != HFOC_TLS_HANDSHAKE_OK) {
        return token_status;
    }
    if (entry->state != HFOC_TLS_HANDSHAKE_STATE_RESERVED) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    if (!hfoc_tls_handshake_config_valid(
            config, token->index, token->generation)) {
        return HFOC_TLS_HANDSHAKE_INVALID_CONFIG;
    }

    bio_status = hfoc_mbedtls_bio_attach(
        (struct hfoc_mbedtls_bio *)config->bio,
        config->bio_token,
        config->bio_policy,
        config->bio_transport);
    if (bio_status != HFOC_MBEDTLS_BIO_OK) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_BIO_ATTACH_FAILED);
        return HFOC_TLS_HANDSHAKE_BIO_ATTACH_FAILED;
    }

    entry->bio_index = config->bio_token->index;
    entry->due_ticks =
        (uint32_t)(now_ticks + config->deadline_ticks);
    entry->max_steps = config->max_steps;
    entry->step_count = 0U;
    entry->drbg_generation = config->drbg_generation;
    entry->drbg_source_id = config->drbg_source_id;
    entry->manifest = config->manifest;
    entry->bio = config->bio;
    entry->bio_token = *config->bio_token;
    entry->step = config->step;
    entry->step_context = config->step_context;
    entry->state = HFOC_TLS_HANDSHAKE_STATE_CONFIGURED;
    return HFOC_TLS_HANDSHAKE_OK;
}

enum hfoc_tls_handshake_status hfoc_tls_handshake_progress(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token,
    uint32_t now_ticks,
    const uint8_t *input,
    size_t input_length,
    size_t *bytes_consumed)
{
    struct hfoc_tls_handshake_entry *entry;
    enum hfoc_tls_handshake_status token_status;
    enum hfoc_tls_handshake_step_result step_result;
    size_t consumed = 0U;

    if (handshake == NULL || token == NULL || bytes_consumed == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    token_status = hfoc_tls_handshake_validate_token(
        handshake, token, &entry);
    if (token_status != HFOC_TLS_HANDSHAKE_OK) {
        return token_status;
    }
    if (entry->state != HFOC_TLS_HANDSHAKE_STATE_CONFIGURED &&
        entry->state != HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    if (hfoc_tls_handshake_deadline_reached(
            now_ticks, entry->due_ticks)) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_DEADLINE_EXPIRED);
        return HFOC_TLS_HANDSHAKE_DEADLINE_EXPIRED;
    }
    if (entry->step_count >= entry->max_steps) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_STEP_LIMIT);
        return HFOC_TLS_HANDSHAKE_STEP_LIMIT;
    }
    if (input_length > 0U && input == NULL) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_INVALID_PROGRESS);
        return HFOC_TLS_HANDSHAKE_INVALID_PROGRESS;
    }

    step_result = entry->step(
        entry->step_context,
        input,
        input_length,
        &consumed);
    if (step_result == HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_STEP_FAILED);
        return HFOC_TLS_HANDSHAKE_STEP_FAILED;
    }
    if (consumed > input_length ||
        (input_length > 0U &&
         step_result != HFOC_TLS_HANDSHAKE_STEP_WANT_READ &&
         consumed == 0U) ||
        (input_length == 0U && consumed != 0U) ||
        (step_result != HFOC_TLS_HANDSHAKE_STEP_WANT_READ &&
         step_result != HFOC_TLS_HANDSHAKE_STEP_WANT_WRITE &&
         step_result != HFOC_TLS_HANDSHAKE_STEP_PROGRESS &&
         step_result != HFOC_TLS_HANDSHAKE_STEP_COMPLETE)) {
        hfoc_tls_handshake_fail(
            handshake,
            entry,
            HFOC_TLS_HANDSHAKE_FAULT_INVALID_PROGRESS);
        return HFOC_TLS_HANDSHAKE_INVALID_PROGRESS;
    }

    ++entry->step_count;
    *bytes_consumed = consumed;
    if (step_result == HFOC_TLS_HANDSHAKE_STEP_COMPLETE) {
        entry->state = HFOC_TLS_HANDSHAKE_STATE_SUCCESS;
        entry->success_published = true;
        ++handshake->success_count;
    } else {
        entry->state = HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING;
    }
    return HFOC_TLS_HANDSHAKE_OK;
}

enum hfoc_tls_handshake_status hfoc_tls_handshake_release(
    struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    struct hfoc_tls_handshake_entry *entry;

    if (handshake == NULL || token == NULL) {
        return HFOC_TLS_HANDSHAKE_NULL;
    }
    if (!hfoc_tls_handshake_valid(handshake)) {
        return HFOC_TLS_HANDSHAKE_BAD_STATE;
    }
    if (token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_handshake_generation_valid(token->generation)) {
        return HFOC_TLS_HANDSHAKE_BAD_TOKEN;
    }
    entry = &handshake->entries[token->index];
    if (!entry->active) {
        return HFOC_TLS_HANDSHAKE_OK;
    }
    if (entry->generation != token->generation) {
        return HFOC_TLS_HANDSHAKE_STALE_TOKEN;
    }
    hfoc_tls_handshake_cleanup(handshake, entry, false,
        HFOC_TLS_HANDSHAKE_FAULT_NONE);
    return HFOC_TLS_HANDSHAKE_OK;
}

bool hfoc_tls_handshake_published(
    const struct hfoc_tls_handshake *handshake)
{
    return hfoc_tls_handshake_valid(handshake);
}

uint32_t hfoc_tls_handshake_active_count(
    const struct hfoc_tls_handshake *handshake)
{
    return hfoc_tls_handshake_valid(handshake)
        ? handshake->active_count : 0U;
}

uint32_t hfoc_tls_handshake_success_count(
    const struct hfoc_tls_handshake *handshake)
{
    return hfoc_tls_handshake_valid(handshake)
        ? handshake->success_count : 0U;
}

uint32_t hfoc_tls_handshake_failure_count(
    const struct hfoc_tls_handshake *handshake)
{
    return hfoc_tls_handshake_valid(handshake)
        ? handshake->failure_count : 0U;
}

enum hfoc_tls_handshake_state hfoc_tls_handshake_state_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT) {
        return HFOC_TLS_HANDSHAKE_STATE_FREE;
    }
    return handshake->entries[index].state;
}

enum hfoc_tls_handshake_fault hfoc_tls_handshake_fault_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT) {
        return HFOC_TLS_HANDSHAKE_FAULT_NONE;
    }
    return handshake->entries[index].fault;
}

bool hfoc_tls_handshake_success_published_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT) {
        return false;
    }
    return handshake->entries[index].success_published;
}

uint32_t hfoc_tls_handshake_generation_at(
    const struct hfoc_tls_handshake *handshake,
    uint32_t index)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT) {
        return 0U;
    }
    return handshake->entries[index].generation;
}

enum hfoc_tls_handshake_state hfoc_tls_handshake_state(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        token == NULL ||
        token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_handshake_generation_valid(token->generation)) {
        return HFOC_TLS_HANDSHAKE_STATE_FREE;
    }
    return handshake->entries[token->index].state;
}

enum hfoc_tls_handshake_fault hfoc_tls_handshake_fault(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    return hfoc_tls_handshake_fault_at(
        handshake,
        token == NULL ? HFOC_TLS_HANDSHAKE_SLOT_COUNT : token->index);
}

bool hfoc_tls_handshake_success_published(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    if (!hfoc_tls_handshake_valid(handshake) ||
        token == NULL ||
        token->index >= HFOC_TLS_HANDSHAKE_SLOT_COUNT ||
        !hfoc_tls_handshake_generation_valid(token->generation)) {
        return false;
    }
    return handshake->entries[token->index].success_published;
}

uint32_t hfoc_tls_handshake_generation(
    const struct hfoc_tls_handshake *handshake,
    const struct hfoc_tls_handshake_token *token)
{
    return hfoc_tls_handshake_generation_at(
        handshake,
        token == NULL ? HFOC_TLS_HANDSHAKE_SLOT_COUNT : token->index);
}

const char *hfoc_tls_handshake_state_string(
    enum hfoc_tls_handshake_state state)
{
    switch (state) {
    case HFOC_TLS_HANDSHAKE_STATE_FREE:
        return "FREE";
    case HFOC_TLS_HANDSHAKE_STATE_RESERVED:
        return "RESERVED";
    case HFOC_TLS_HANDSHAKE_STATE_CONFIGURED:
        return "CONFIGURED";
    case HFOC_TLS_HANDSHAKE_STATE_HANDSHAKING:
        return "HANDSHAKING";
    case HFOC_TLS_HANDSHAKE_STATE_SUCCESS:
        return "SUCCESS";
    case HFOC_TLS_HANDSHAKE_STATE_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

const char *hfoc_tls_handshake_fault_string(
    enum hfoc_tls_handshake_fault fault)
{
    switch (fault) {
    case HFOC_TLS_HANDSHAKE_FAULT_NONE:
        return "NONE";
    case HFOC_TLS_HANDSHAKE_FAULT_ALLOCATOR_UNAVAILABLE:
        return "ALLOCATOR_UNAVAILABLE";
    case HFOC_TLS_HANDSHAKE_FAULT_BIO_ATTACH_FAILED:
        return "BIO_ATTACH_FAILED";
    case HFOC_TLS_HANDSHAKE_FAULT_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case HFOC_TLS_HANDSHAKE_FAULT_STEP_FAILED:
        return "STEP_FAILED";
    case HFOC_TLS_HANDSHAKE_FAULT_INVALID_PROGRESS:
        return "INVALID_PROGRESS";
    case HFOC_TLS_HANDSHAKE_FAULT_DEADLINE_EXPIRED:
        return "DEADLINE_EXPIRED";
    case HFOC_TLS_HANDSHAKE_FAULT_STEP_LIMIT:
        return "STEP_LIMIT";
    default:
        return NULL;
    }
}

const char *hfoc_tls_handshake_status_string(
    enum hfoc_tls_handshake_status status)
{
    switch (status) {
    case HFOC_TLS_HANDSHAKE_OK:
        return "OK";
    case HFOC_TLS_HANDSHAKE_NULL:
        return "NULL";
    case HFOC_TLS_HANDSHAKE_BAD_STATE:
        return "BAD_STATE";
    case HFOC_TLS_HANDSHAKE_BAD_TOKEN:
        return "BAD_TOKEN";
    case HFOC_TLS_HANDSHAKE_STALE_TOKEN:
        return "STALE_TOKEN";
    case HFOC_TLS_HANDSHAKE_NOT_ATTACHED:
        return "NOT_ATTACHED";
    case HFOC_TLS_HANDSHAKE_ALREADY_RESERVED:
        return "ALREADY_RESERVED";
    case HFOC_TLS_HANDSHAKE_INVALID_CONFIG:
        return "INVALID_CONFIG";
    case HFOC_TLS_HANDSHAKE_ALLOCATOR_UNAVAILABLE:
        return "ALLOCATOR_UNAVAILABLE";
    case HFOC_TLS_HANDSHAKE_BIO_ATTACH_FAILED:
        return "BIO_ATTACH_FAILED";
    case HFOC_TLS_HANDSHAKE_STEP_FAILED:
        return "STEP_FAILED";
    case HFOC_TLS_HANDSHAKE_INVALID_PROGRESS:
        return "INVALID_PROGRESS";
    case HFOC_TLS_HANDSHAKE_DEADLINE_EXPIRED:
        return "DEADLINE_EXPIRED";
    case HFOC_TLS_HANDSHAKE_STEP_LIMIT:
        return "STEP_LIMIT";
    default:
        return NULL;
    }
}

const char *hfoc_tls_handshake_step_result_string(
    enum hfoc_tls_handshake_step_result result)
{
    switch (result) {
    case HFOC_TLS_HANDSHAKE_STEP_WANT_READ:
        return "WANT_READ";
    case HFOC_TLS_HANDSHAKE_STEP_WANT_WRITE:
        return "WANT_WRITE";
    case HFOC_TLS_HANDSHAKE_STEP_PROGRESS:
        return "PROGRESS";
    case HFOC_TLS_HANDSHAKE_STEP_COMPLETE:
        return "COMPLETE";
    case HFOC_TLS_HANDSHAKE_STEP_RESULT_FAILED:
        return "FAILED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_tls_handshake_translation_unit_guard;

#endif
