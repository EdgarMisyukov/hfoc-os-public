#include "hfoc_mbedtls_bio.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_MBEDTLS_BIO_HOST_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

#include "mbedtls/ssl.h"

static bool hfoc_mbedtls_bio_generation_valid(uint32_t generation)
{
    return generation >= HFOC_MBEDTLS_BIO_GENERATION_MIN &&
           generation <= HFOC_MBEDTLS_BIO_GENERATION_MAX;
}

static bool hfoc_mbedtls_bio_policy_valid(
    const struct hfoc_mbedtls_bio_policy *policy)
{
    return policy != NULL &&
           policy->version == HFOC_MBEDTLS_BIO_VERSION &&
           policy->max_send_bytes >= HFOC_MBEDTLS_BIO_MAX_LEN_MIN &&
           policy->max_send_bytes <= HFOC_MBEDTLS_BIO_MAX_LEN_MAX &&
           policy->max_recv_bytes >= HFOC_MBEDTLS_BIO_MAX_LEN_MIN &&
           policy->max_recv_bytes <= HFOC_MBEDTLS_BIO_MAX_LEN_MAX;
}

static bool hfoc_mbedtls_bio_transport_valid(
    const struct hfoc_mbedtls_bio_transport *transport)
{
    return transport != NULL &&
           transport->send != NULL &&
           transport->recv != NULL;
}

static bool hfoc_mbedtls_bio_entry_valid(
    const struct hfoc_mbedtls_bio_entry *entry)
{
    if (entry == NULL) {
        return false;
    }
    if (!entry->active) {
        return entry->owner == NULL &&
               entry->token.index == 0U &&
               entry->token.generation == 0U &&
               entry->generation == 0U &&
               entry->state == HFOC_MBEDTLS_BIO_STATE_FREE &&
               entry->policy.version == 0U &&
               entry->transport.send == NULL &&
               entry->transport.recv == NULL &&
               entry->transport.ctx == NULL &&
               entry->send_calls == 0U &&
               entry->recv_calls == 0U &&
               entry->want_read_count == 0U &&
               entry->want_write_count == 0U &&
               entry->short_send_count == 0U &&
               entry->short_recv_count == 0U &&
               entry->send_bytes == 0U &&
               entry->recv_bytes == 0U &&
               entry->rejected_count == 0U &&
               entry->closed_count == 0U;
    }
    return entry->owner != NULL &&
           hfoc_mbedtls_bio_generation_valid(entry->generation) &&
           entry->token.index < HFOC_MBEDTLS_BIO_SLOT_COUNT &&
           entry->token.generation == entry->generation &&
           hfoc_mbedtls_bio_policy_valid(&entry->policy) &&
           hfoc_mbedtls_bio_transport_valid(&entry->transport);
}

static bool hfoc_mbedtls_bio_valid(
    const struct hfoc_mbedtls_bio *bio)
{
    uint32_t active_count = 0U;
    uint32_t index;

    if (bio == NULL ||
        !bio->published ||
        bio->version != HFOC_MBEDTLS_BIO_VERSION ||
        bio->active_count > HFOC_MBEDTLS_BIO_SLOT_COUNT) {
        return false;
    }
    for (index = 0U;
         index < HFOC_MBEDTLS_BIO_SLOT_COUNT;
         ++index) {
        const struct hfoc_mbedtls_bio_entry *entry =
            &bio->entries[index];

        if (!hfoc_mbedtls_bio_entry_valid(entry)) {
            return false;
        }
        if (entry->active) {
            if (entry->owner != bio ||
                entry->token.index != index) {
                return false;
            }
            ++active_count;
        }
    }
    return active_count == bio->active_count;
}

static enum hfoc_mbedtls_bio_status
hfoc_mbedtls_bio_validate_token(
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token,
    struct hfoc_mbedtls_bio_entry **entry_out)
{
    struct hfoc_mbedtls_bio_entry *entry;

    if (bio == NULL || token == NULL || entry_out == NULL) {
        return HFOC_MBEDTLS_BIO_NULL;
    }
    if (!hfoc_mbedtls_bio_valid(bio)) {
        return HFOC_MBEDTLS_BIO_BAD_STATE;
    }
    if (token->index >= HFOC_MBEDTLS_BIO_SLOT_COUNT ||
        !hfoc_mbedtls_bio_generation_valid(token->generation)) {
        return HFOC_MBEDTLS_BIO_BAD_TOKEN;
    }
    entry = &bio->entries[token->index];
    if (!entry->active) {
        return HFOC_MBEDTLS_BIO_NOT_ATTACHED;
    }
    if (entry->generation != token->generation) {
        return HFOC_MBEDTLS_BIO_STALE_TOKEN;
    }
    *entry_out = entry;
    return HFOC_MBEDTLS_BIO_OK;
}

static bool hfoc_mbedtls_bio_callback_valid(
    void *context,
    struct hfoc_mbedtls_bio_entry **entry_out)
{
    struct hfoc_mbedtls_bio_entry *entry = context;

    if (entry == NULL ||
        entry_out == NULL ||
        !hfoc_mbedtls_bio_valid(entry->owner)) {
        return false;
    }
    if (!entry->active ||
        entry->owner == NULL ||
        entry->token.index >= HFOC_MBEDTLS_BIO_SLOT_COUNT ||
        &entry->owner->entries[entry->token.index] != entry) {
        return false;
    }
    *entry_out = entry;
    return true;
}

static void hfoc_mbedtls_bio_fail(
    struct hfoc_mbedtls_bio_entry *entry)
{
    entry->state = HFOC_MBEDTLS_BIO_STATE_FAILED;
    ++entry->rejected_count;
}

enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_init(
    struct hfoc_mbedtls_bio *bio)
{
    if (bio == NULL) {
        return HFOC_MBEDTLS_BIO_NULL;
    }
    if (bio->published) {
        return HFOC_MBEDTLS_BIO_BAD_STATE;
    }
    memset(bio, 0, sizeof(*bio));
    bio->version = HFOC_MBEDTLS_BIO_VERSION;
    bio->published = true;
    return HFOC_MBEDTLS_BIO_OK;
}

enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_attach(
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token,
    const struct hfoc_mbedtls_bio_policy *policy,
    const struct hfoc_mbedtls_bio_transport *transport)
{
    struct hfoc_mbedtls_bio_entry *entry;

    if (bio == NULL || token == NULL || policy == NULL ||
        transport == NULL) {
        return HFOC_MBEDTLS_BIO_NULL;
    }
    if (!hfoc_mbedtls_bio_valid(bio)) {
        return HFOC_MBEDTLS_BIO_BAD_STATE;
    }
    if (token->index >= HFOC_MBEDTLS_BIO_SLOT_COUNT ||
        !hfoc_mbedtls_bio_generation_valid(token->generation)) {
        return HFOC_MBEDTLS_BIO_BAD_TOKEN;
    }
    if (!hfoc_mbedtls_bio_policy_valid(policy)) {
        return HFOC_MBEDTLS_BIO_INVALID_POLICY;
    }
    if (!hfoc_mbedtls_bio_transport_valid(transport)) {
        return HFOC_MBEDTLS_BIO_INVALID_TRANSPORT;
    }

    entry = &bio->entries[token->index];
    if (entry->active) {
        return entry->generation == token->generation
            ? HFOC_MBEDTLS_BIO_ALREADY_ATTACHED
            : HFOC_MBEDTLS_BIO_STALE_TOKEN;
    }

    memset(entry, 0, sizeof(*entry));
    entry->owner = bio;
    entry->token = *token;
    entry->active = true;
    entry->generation = token->generation;
    entry->state = HFOC_MBEDTLS_BIO_STATE_ATTACHED;
    entry->policy = *policy;
    entry->transport = *transport;
    ++bio->active_count;
    return HFOC_MBEDTLS_BIO_OK;
}

enum hfoc_mbedtls_bio_status hfoc_mbedtls_bio_detach(
    struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token)
{
    struct hfoc_mbedtls_bio_entry *entry;
    enum hfoc_mbedtls_bio_status status;

    status = hfoc_mbedtls_bio_validate_token(bio, token, &entry);
    if (status != HFOC_MBEDTLS_BIO_OK) {
        return status;
    }
    memset(entry, 0, sizeof(*entry));
    --bio->active_count;
    return HFOC_MBEDTLS_BIO_OK;
}

int hfoc_mbedtls_bio_send_cb(
    void *context,
    const unsigned char *buf,
    size_t len)
{
    struct hfoc_mbedtls_bio_entry *entry;
    size_t bytes_accepted = 0U;
    enum hfoc_mbedtls_bio_transport_status status;

    if (!hfoc_mbedtls_bio_callback_valid(context, &entry)) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    if (buf == NULL ||
        len == 0U ||
        len > entry->policy.max_send_bytes) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    status = entry->transport.send(
        entry->transport.ctx,
        buf,
        len,
        &bytes_accepted);
    ++entry->send_calls;
    switch (status) {
    case HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED:
        if (bytes_accepted == 0U ||
            bytes_accepted > len) {
            hfoc_mbedtls_bio_fail(entry);
            return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        }
        entry->state = HFOC_MBEDTLS_BIO_STATE_ATTACHED;
        entry->send_bytes += (uint32_t)bytes_accepted;
        if (bytes_accepted < len) {
            ++entry->short_send_count;
        }
        return (int)bytes_accepted;
    case HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED:
        if (bytes_accepted != 0U) {
            hfoc_mbedtls_bio_fail(entry);
            return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        }
        entry->state = HFOC_MBEDTLS_BIO_STATE_WANT_WRITE;
        ++entry->want_write_count;
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    case HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED:
        entry->state = HFOC_MBEDTLS_BIO_STATE_CLOSED;
        ++entry->closed_count;
        return MBEDTLS_ERR_SSL_CONN_EOF;
    case HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED:
        hfoc_mbedtls_bio_fail(entry);
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    default:
        hfoc_mbedtls_bio_fail(entry);
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
}

int hfoc_mbedtls_bio_recv_cb(
    void *context,
    unsigned char *buf,
    size_t len)
{
    struct hfoc_mbedtls_bio_entry *entry;
    size_t bytes_received = 0U;
    enum hfoc_mbedtls_bio_transport_status status;

    if (!hfoc_mbedtls_bio_callback_valid(context, &entry)) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
    if (buf == NULL ||
        len == 0U ||
        len > entry->policy.max_recv_bytes) {
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }

    status = entry->transport.recv(
        entry->transport.ctx,
        buf,
        len,
        &bytes_received);
    ++entry->recv_calls;
    switch (status) {
    case HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED:
        if (bytes_received == 0U ||
            bytes_received > len) {
            hfoc_mbedtls_bio_fail(entry);
            return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        }
        entry->state = HFOC_MBEDTLS_BIO_STATE_ATTACHED;
        entry->recv_bytes += (uint32_t)bytes_received;
        if (bytes_received < len) {
            ++entry->short_recv_count;
        }
        return (int)bytes_received;
    case HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED:
        if (bytes_received != 0U) {
            hfoc_mbedtls_bio_fail(entry);
            return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
        }
        entry->state = HFOC_MBEDTLS_BIO_STATE_WANT_READ;
        ++entry->want_read_count;
        return MBEDTLS_ERR_SSL_WANT_READ;
    case HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED:
        entry->state = HFOC_MBEDTLS_BIO_STATE_CLOSED;
        ++entry->closed_count;
        return 0;
    case HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED:
        hfoc_mbedtls_bio_fail(entry);
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    default:
        hfoc_mbedtls_bio_fail(entry);
        return MBEDTLS_ERR_SSL_BAD_INPUT_DATA;
    }
}

bool hfoc_mbedtls_bio_published(
    const struct hfoc_mbedtls_bio *bio)
{
    return hfoc_mbedtls_bio_valid(bio);
}

uint32_t hfoc_mbedtls_bio_active_count(
    const struct hfoc_mbedtls_bio *bio)
{
    return hfoc_mbedtls_bio_valid(bio)
        ? bio->active_count
        : 0U;
}

enum hfoc_mbedtls_bio_state hfoc_mbedtls_bio_state(
    const struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token)
{
    const struct hfoc_mbedtls_bio_entry *entry;

    if (!hfoc_mbedtls_bio_valid(bio) ||
        token == NULL ||
        token->index >= HFOC_MBEDTLS_BIO_SLOT_COUNT ||
        !hfoc_mbedtls_bio_generation_valid(token->generation)) {
        return HFOC_MBEDTLS_BIO_STATE_FREE;
    }
    entry = &bio->entries[token->index];
    if (!entry->active ||
        entry->generation != token->generation) {
        return HFOC_MBEDTLS_BIO_STATE_FREE;
    }
    return entry->state;
}

uint32_t hfoc_mbedtls_bio_generation(
    const struct hfoc_mbedtls_bio *bio,
    const struct hfoc_mbedtls_bio_token *token)
{
    const struct hfoc_mbedtls_bio_entry *entry;

    if (!hfoc_mbedtls_bio_valid(bio) ||
        token == NULL ||
        token->index >= HFOC_MBEDTLS_BIO_SLOT_COUNT ||
        !hfoc_mbedtls_bio_generation_valid(token->generation)) {
        return 0U;
    }
    entry = &bio->entries[token->index];
    return entry->active &&
               entry->generation == token->generation
        ? entry->generation
        : 0U;
}

const char *hfoc_mbedtls_bio_state_string(
    enum hfoc_mbedtls_bio_state state)
{
    switch (state) {
    case HFOC_MBEDTLS_BIO_STATE_FREE:
        return "HFOC_MBEDTLS_BIO_STATE_FREE";
    case HFOC_MBEDTLS_BIO_STATE_ATTACHED:
        return "HFOC_MBEDTLS_BIO_STATE_ATTACHED";
    case HFOC_MBEDTLS_BIO_STATE_WANT_READ:
        return "HFOC_MBEDTLS_BIO_STATE_WANT_READ";
    case HFOC_MBEDTLS_BIO_STATE_WANT_WRITE:
        return "HFOC_MBEDTLS_BIO_STATE_WANT_WRITE";
    case HFOC_MBEDTLS_BIO_STATE_CLOSED:
        return "HFOC_MBEDTLS_BIO_STATE_CLOSED";
    case HFOC_MBEDTLS_BIO_STATE_FAILED:
        return "HFOC_MBEDTLS_BIO_STATE_FAILED";
    default:
        return NULL;
    }
}

const char *hfoc_mbedtls_bio_status_string(
    enum hfoc_mbedtls_bio_status status)
{
    switch (status) {
    case HFOC_MBEDTLS_BIO_OK:
        return "HFOC_MBEDTLS_BIO_OK";
    case HFOC_MBEDTLS_BIO_NULL:
        return "HFOC_MBEDTLS_BIO_NULL";
    case HFOC_MBEDTLS_BIO_BAD_STATE:
        return "HFOC_MBEDTLS_BIO_BAD_STATE";
    case HFOC_MBEDTLS_BIO_BAD_TOKEN:
        return "HFOC_MBEDTLS_BIO_BAD_TOKEN";
    case HFOC_MBEDTLS_BIO_STALE_TOKEN:
        return "HFOC_MBEDTLS_BIO_STALE_TOKEN";
    case HFOC_MBEDTLS_BIO_ALREADY_ATTACHED:
        return "HFOC_MBEDTLS_BIO_ALREADY_ATTACHED";
    case HFOC_MBEDTLS_BIO_INVALID_POLICY:
        return "HFOC_MBEDTLS_BIO_INVALID_POLICY";
    case HFOC_MBEDTLS_BIO_INVALID_TRANSPORT:
        return "HFOC_MBEDTLS_BIO_INVALID_TRANSPORT";
    case HFOC_MBEDTLS_BIO_NOT_ATTACHED:
        return "HFOC_MBEDTLS_BIO_NOT_ATTACHED";
    default:
        return NULL;
    }
}

const char *hfoc_mbedtls_bio_transport_status_string(
    enum hfoc_mbedtls_bio_transport_status status)
{
    switch (status) {
    case HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED:
        return "HFOC_MBEDTLS_BIO_TRANSPORT_ACCEPTED";
    case HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED:
        return "HFOC_MBEDTLS_BIO_TRANSPORT_RETAINED";
    case HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED:
        return "HFOC_MBEDTLS_BIO_TRANSPORT_CLOSED";
    case HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED:
        return "HFOC_MBEDTLS_BIO_TRANSPORT_REJECTED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_mbedtls_bio_production_empty;

#endif
