#include "appliance_audit_chain.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-377 hash chain.  Every byte that enters a digest is written in a fixed
 * order and a fixed width, so the digest never depends on struct padding or on
 * the host byte order, and two independent implementations can reproduce it.
 */

static void appliance_audit_chain_zero(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

static bool appliance_audit_chain_valid(
    const struct appliance_audit_chain *chain)
{
    if (chain == NULL ||
        !chain->initialized ||
        chain->version != APPLIANCE_AUDIT_CHAIN_VERSION ||
        chain->generation == 0U ||
        chain->count > APPLIANCE_AUDIT_CHAIN_CAPACITY) {
        return false;
    }
    if (chain->count == 0U) {
        return chain->first_sequence == chain->next_sequence;
    }
    return chain->first_sequence != 0U &&
           chain->next_sequence ==
               chain->first_sequence + chain->count;
}

static void appliance_audit_chain_store_le32(
    uint8_t *destination,
    uint32_t value)
{
    destination[0] = (uint8_t)(value & UINT32_C(0xFF));
    destination[1] = (uint8_t)((value >> 8U) & UINT32_C(0xFF));
    destination[2] = (uint8_t)((value >> 16U) & UINT32_C(0xFF));
    destination[3] = (uint8_t)((value >> 24U) & UINT32_C(0xFF));
}

static void appliance_audit_chain_store_le64(
    uint8_t *destination,
    uint64_t value)
{
    uint32_t index;

    for (index = 0U; index < 8U; ++index) {
        destination[index] = (uint8_t)((value >> (index * 8U)) &
                                       UINT64_C(0xFF));
    }
}

static void appliance_audit_chain_canonical_record(
    const struct audit_record *record,
    uint8_t canonical[APPLIANCE_AUDIT_CHAIN_CANONICAL_SIZE])
{
    /*
     * The record's own sequence copy is covered first: the chain stamps it,
     * and a rewrite of the stored or exported copy must break the digest even
     * though the link sequence is authenticated separately.
     */
    appliance_audit_chain_zero(
        canonical, APPLIANCE_AUDIT_CHAIN_CANONICAL_SIZE);
    appliance_audit_chain_store_le64(canonical + 0U, record->sequence);
    appliance_audit_chain_store_le32(canonical + 8U, record->type);
    appliance_audit_chain_store_le32(canonical + 12U, record->operation);
    appliance_audit_chain_store_le32(canonical + 16U, record->result);
    appliance_audit_chain_store_le32(canonical + 20U, record->pid);
    appliance_audit_chain_store_le32(canonical + 24U, record->domain);
    appliance_audit_chain_store_le32(canonical + 28U, record->generation);
    appliance_audit_chain_store_le32(canonical + 32U, record->session_id);
    appliance_audit_chain_store_le32(canonical + 36U, record->principal_id);
    appliance_audit_chain_store_le32(canonical + 40U, record->command_id);
    appliance_audit_chain_store_le64(canonical + 44U, record->detail);
}

static enum appliance_audit_chain_status appliance_audit_chain_genesis(
    uint64_t generation,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    static const char tag[] = APPLIANCE_AUDIT_CHAIN_GENESIS_TAG;
    struct sha256 context;
    uint8_t encoded[8];

    if (sha256_init(&context) != SHA256_OK) {
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    appliance_audit_chain_store_le64(encoded, generation);
    if (sha256_update(&context, tag, sizeof(tag) - 1U) != SHA256_OK ||
        sha256_update(&context, encoded, sizeof(encoded)) != SHA256_OK ||
        sha256_final(&context, digest_out) != SHA256_OK) {
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

static enum appliance_audit_chain_status appliance_audit_chain_link(
    uint64_t sequence,
    const uint8_t previous[SHA256_DIGEST_SIZE],
    const struct audit_record *record,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    static const char tag[] = APPLIANCE_AUDIT_CHAIN_LINK_TAG;
    struct sha256 context;
    uint8_t encoded[8];
    uint8_t canonical[APPLIANCE_AUDIT_CHAIN_CANONICAL_SIZE];

    if (sha256_init(&context) != SHA256_OK) {
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    appliance_audit_chain_store_le64(encoded, sequence);
    appliance_audit_chain_canonical_record(record, canonical);
    if (sha256_update(&context, tag, sizeof(tag) - 1U) != SHA256_OK ||
        sha256_update(&context, encoded, sizeof(encoded)) != SHA256_OK ||
        sha256_update(&context, previous, SHA256_DIGEST_SIZE) != SHA256_OK ||
        sha256_update(&context, canonical, sizeof(canonical)) != SHA256_OK ||
        sha256_final(&context, digest_out) != SHA256_OK) {
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_init(
    struct appliance_audit_chain *chain,
    uint64_t generation)
{
    if (chain == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (generation == 0U) {
        return APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT;
    }
    memset(chain, 0, sizeof(*chain));
    chain->initialized = true;
    chain->version = APPLIANCE_AUDIT_CHAIN_VERSION;
    chain->generation = generation;
    chain->next_sequence = 1U;
    chain->first_sequence = 1U;
    chain->anchor_is_genesis = true;
    if (appliance_audit_chain_genesis(
            generation, chain->anchor_digest) != APPLIANCE_AUDIT_CHAIN_OK) {
        memset(chain, 0, sizeof(*chain));
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_append(
    struct appliance_audit_chain *chain,
    const struct audit_record *record,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    struct appliance_audit_chain_entry entry;
    const uint8_t *previous;

    if (chain == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (record == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (!appliance_audit_chain_valid(chain)) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    if (!audit_record_is_valid(record)) {
        return APPLIANCE_AUDIT_CHAIN_INVALID_RECORD;
    }
    if (chain->count == APPLIANCE_AUDIT_CHAIN_CAPACITY) {
        return APPLIANCE_AUDIT_CHAIN_FULL;
    }

    memset(&entry, 0, sizeof(entry));
    entry.sequence = chain->next_sequence;
    entry.record = *record;
    entry.record.sequence = entry.sequence;
    previous = chain->count == 0U
        ? chain->anchor_digest
        : chain->entries[chain->count - 1U].digest;
    if (appliance_audit_chain_link(
            entry.sequence, previous, &entry.record,
            entry.digest) != APPLIANCE_AUDIT_CHAIN_OK) {
        return APPLIANCE_AUDIT_CHAIN_SHA_FAILED;
    }
    chain->entries[chain->count] = entry;
    ++chain->count;
    ++chain->next_sequence;
    ++chain->appended_count;
    if (digest_out != NULL) {
        memcpy(digest_out, entry.digest, SHA256_DIGEST_SIZE);
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_verify(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_chain_verdict *verdict_out)
{
    struct appliance_audit_chain_verdict verdict;
    uint8_t running[SHA256_DIGEST_SIZE];
    uint32_t index;

    if (chain == NULL || verdict_out == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (!appliance_audit_chain_valid(chain)) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }

    memset(&verdict, 0, sizeof(verdict));
    verdict.version = APPLIANCE_AUDIT_CHAIN_VERSION;
    verdict.generation = chain->generation;
    verdict.first_sequence = chain->first_sequence;
    verdict.anchored = !chain->anchor_is_genesis;
    verdict.chain_ok = true;
    memcpy(running, chain->anchor_digest, SHA256_DIGEST_SIZE);
    for (index = 0U; index < chain->count; ++index) {
        const struct appliance_audit_chain_entry *entry =
            &chain->entries[index];
        uint8_t expected[SHA256_DIGEST_SIZE];

        if (entry->sequence != chain->first_sequence + index ||
            appliance_audit_chain_link(
                entry->sequence, running, &entry->record,
                expected) != APPLIANCE_AUDIT_CHAIN_OK ||
            memcmp(expected, entry->digest, SHA256_DIGEST_SIZE) != 0) {
            verdict.chain_ok = false;
            verdict.tamper_detected = true;
            verdict.tamper_sequence = chain->first_sequence + index;
            break;
        }
        memcpy(running, entry->digest, SHA256_DIGEST_SIZE);
        ++verdict.checked_count;
    }
    if (chain->count != 0U) {
        verdict.head_sequence = chain->next_sequence - 1U;
    } else {
        verdict.head_sequence = chain->first_sequence - 1U;
    }
    if (verdict.chain_ok) {
        memcpy(verdict.head_digest, running, SHA256_DIGEST_SIZE);
    }
    *verdict_out = verdict;
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_rotate(
    struct appliance_audit_chain *chain,
    uint32_t retire_count,
    struct appliance_audit_chain_verdict *verdict_out)
{
    uint32_t index;

    if (chain == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (!appliance_audit_chain_valid(chain)) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    if (retire_count == 0U || retire_count > chain->count) {
        return APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT;
    }

    /*
     * The newest retired digest becomes the running anchor, so the retained
     * window still verifies and an append after a full rotation continues the
     * same chain instead of starting a fresh one.
     */
    memcpy(chain->anchor_digest,
           chain->entries[retire_count - 1U].digest,
           SHA256_DIGEST_SIZE);
    chain->anchor_is_genesis = false;
    for (index = retire_count; index < chain->count; ++index) {
        chain->entries[index - retire_count] = chain->entries[index];
    }
    for (index = chain->count - retire_count;
         index < chain->count; ++index) {
        memset(&chain->entries[index], 0,
               sizeof(chain->entries[index]));
    }
    chain->count -= retire_count;
    chain->first_sequence += retire_count;
    chain->rotated_count += retire_count;
    if (verdict_out != NULL) {
        return appliance_audit_chain_verify(chain, verdict_out);
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_snapshot(
    const struct appliance_audit_chain *chain,
    struct appliance_audit_chain_snapshot *snapshot_out)
{
    struct appliance_audit_chain_verdict verdict;

    if (chain == NULL || snapshot_out == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (!appliance_audit_chain_valid(chain)) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    if (appliance_audit_chain_verify(chain, &verdict) !=
        APPLIANCE_AUDIT_CHAIN_OK) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }

    memset(snapshot_out, 0, sizeof(*snapshot_out));
    snapshot_out->version = APPLIANCE_AUDIT_CHAIN_VERSION;
    snapshot_out->published = true;
    snapshot_out->generation = chain->generation;
    snapshot_out->first_sequence = chain->first_sequence;
    snapshot_out->next_sequence = chain->next_sequence;
    snapshot_out->appended_count = chain->appended_count;
    snapshot_out->rotated_count = chain->rotated_count;
    snapshot_out->count = chain->count;
    snapshot_out->capacity = APPLIANCE_AUDIT_CHAIN_CAPACITY;
    snapshot_out->anchor_is_genesis = chain->anchor_is_genesis;
    memcpy(snapshot_out->head_digest, verdict.head_digest,
           SHA256_DIGEST_SIZE);
    if (chain->count == 0U) {
        snapshot_out->state = APPLIANCE_AUDIT_CHAIN_STATE_EMPTY;
    } else if (!verdict.chain_ok) {
        snapshot_out->state = APPLIANCE_AUDIT_CHAIN_STATE_TAMPERED;
    } else {
        snapshot_out->state = APPLIANCE_AUDIT_CHAIN_STATE_OK;
    }
    return APPLIANCE_AUDIT_CHAIN_OK;
}

bool appliance_audit_chain_head_digest(
    const struct appliance_audit_chain *chain,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    struct appliance_audit_chain_verdict verdict;

    if (digest_out == NULL ||
        appliance_audit_chain_verify(chain, &verdict) !=
            APPLIANCE_AUDIT_CHAIN_OK ||
        !verdict.chain_ok) {
        return false;
    }
    memcpy(digest_out, verdict.head_digest, SHA256_DIGEST_SIZE);
    return true;
}

uint32_t appliance_audit_chain_count(
    const struct appliance_audit_chain *chain)
{
    return appliance_audit_chain_valid(chain) ? chain->count : 0U;
}

uint64_t appliance_audit_chain_first_sequence(
    const struct appliance_audit_chain *chain)
{
    return appliance_audit_chain_valid(chain)
        ? chain->first_sequence
        : 0U;
}

uint64_t appliance_audit_chain_next_sequence(
    const struct appliance_audit_chain *chain)
{
    return appliance_audit_chain_valid(chain)
        ? chain->next_sequence
        : 0U;
}

uint64_t appliance_audit_chain_generation(
    const struct appliance_audit_chain *chain)
{
    return appliance_audit_chain_valid(chain) ? chain->generation : 0U;
}

bool appliance_audit_chain_find(
    const struct appliance_audit_chain *chain,
    uint64_t sequence,
    struct audit_record *record_out,
    uint8_t digest_out[SHA256_DIGEST_SIZE])
{
    uint32_t index;

    if (!appliance_audit_chain_valid(chain) || sequence == 0U ||
        sequence < chain->first_sequence ||
        sequence >= chain->next_sequence) {
        return false;
    }
    index = (uint32_t)(sequence - chain->first_sequence);
    if (record_out != NULL) {
        *record_out = chain->entries[index].record;
    }
    if (digest_out != NULL) {
        memcpy(digest_out, chain->entries[index].digest,
               SHA256_DIGEST_SIZE);
    }
    return true;
}

static bool appliance_audit_chain_append_text(
    char *output,
    size_t capacity,
    size_t *offset,
    const char *text)
{
    size_t index = 0U;

    if (output == NULL || offset == NULL || text == NULL) {
        return false;
    }
    while (text[index] != '\0') {
        if (*offset + 1U > capacity) {
            return false;
        }
        if (*offset < capacity) {
            output[*offset] = text[index];
        }
        ++(*offset);
        ++index;
    }
    return true;
}

static bool appliance_audit_chain_append_digest(
    char *output,
    size_t capacity,
    size_t *offset,
    const uint8_t digest[SHA256_DIGEST_SIZE])
{
    static const char hex[] = "0123456789abcdef";
    uint32_t index;

    for (index = 0U; index < SHA256_DIGEST_SIZE; ++index) {
        char pair[3];

        pair[0] = hex[(digest[index] >> 4U) & 0x0FU];
        pair[1] = hex[digest[index] & 0x0FU];
        pair[2] = '\0';
        if (!appliance_audit_chain_append_text(
                output, capacity, offset, pair)) {
            return false;
        }
    }
    return true;
}

static bool appliance_audit_chain_append_u64(
    char *output,
    size_t capacity,
    size_t *offset,
    uint64_t value)
{
    char digits[21];
    uint32_t count = 0U;

    if (value == 0U) {
        return appliance_audit_chain_append_text(
            output, capacity, offset, "0");
    }
    while (value != 0U && count < 20U) {
        digits[count] = (char)('0' + (value % 10U));
        value /= 10U;
        ++count;
    }
    while (count != 0U) {
        char single[2];

        --count;
        single[0] = digits[count];
        single[1] = '\0';
        if (!appliance_audit_chain_append_text(
                output, capacity, offset, single)) {
            return false;
        }
    }
    return true;
}

enum appliance_audit_chain_status appliance_audit_chain_render_snapshot(
    const struct appliance_audit_chain_snapshot *snapshot,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    const char *state;
    size_t offset = 0U;

    if (snapshot == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (!snapshot->published ||
        snapshot->version != APPLIANCE_AUDIT_CHAIN_VERSION ||
        snapshot->capacity != APPLIANCE_AUDIT_CHAIN_CAPACITY ||
        snapshot->count > snapshot->capacity) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    state = appliance_audit_chain_state_string(snapshot->state);
    if (state == NULL) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    appliance_audit_chain_zero(output, capacity);
    if (!appliance_audit_chain_append_text(
            output, capacity, &offset,
            "audit_chain v=1 generation=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->generation) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " state=") ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, state) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " records=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->count) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " first=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->first_sequence) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " next=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->next_sequence) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " capacity=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->capacity) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " appended=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->appended_count) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " rotated=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, snapshot->rotated_count) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " anchored=") ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset,
            snapshot->anchor_is_genesis ? "NO" : "YES")) {
        return APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL;
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_AUDIT_CHAIN_OK;
}

enum appliance_audit_chain_status appliance_audit_chain_render_verdict(
    const struct appliance_audit_chain_verdict *verdict,
    char *output,
    size_t capacity,
    size_t *output_length)
{
    size_t offset = 0U;

    if (verdict == NULL || output == NULL || output_length == NULL) {
        return APPLIANCE_AUDIT_CHAIN_NULL;
    }
    if (verdict->version != APPLIANCE_AUDIT_CHAIN_VERSION) {
        return APPLIANCE_AUDIT_CHAIN_BAD_STATE;
    }
    appliance_audit_chain_zero(output, capacity);
    if (!appliance_audit_chain_append_text(
            output, capacity, &offset, "audit_chain_verify v=1 result=") ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset,
            verdict->chain_ok ? "OK" : "TAMPERED") ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " checked=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, verdict->checked_count) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " first=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, verdict->first_sequence) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " head=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, verdict->head_sequence) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " tamper=") ||
        !appliance_audit_chain_append_u64(
            output, capacity, &offset, verdict->tamper_sequence) ||
        !appliance_audit_chain_append_text(
            output, capacity, &offset, " digest=") ||
        !appliance_audit_chain_append_digest(
            output, capacity, &offset, verdict->head_digest)) {
        return APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL;
    }
    if (offset + 1U > capacity) {
        return APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL;
    }
    output[offset] = '\0';
    *output_length = offset;
    return APPLIANCE_AUDIT_CHAIN_OK;
}

const char *appliance_audit_chain_status_string(
    enum appliance_audit_chain_status status)
{
    switch (status) {
    case APPLIANCE_AUDIT_CHAIN_OK:
        return "APPLIANCE_AUDIT_CHAIN_OK";
    case APPLIANCE_AUDIT_CHAIN_NULL:
        return "APPLIANCE_AUDIT_CHAIN_NULL";
    case APPLIANCE_AUDIT_CHAIN_BAD_VERSION:
        return "APPLIANCE_AUDIT_CHAIN_BAD_VERSION";
    case APPLIANCE_AUDIT_CHAIN_BAD_STATE:
        return "APPLIANCE_AUDIT_CHAIN_BAD_STATE";
    case APPLIANCE_AUDIT_CHAIN_INVALID_RECORD:
        return "APPLIANCE_AUDIT_CHAIN_INVALID_RECORD";
    case APPLIANCE_AUDIT_CHAIN_FULL:
        return "APPLIANCE_AUDIT_CHAIN_FULL";
    case APPLIANCE_AUDIT_CHAIN_EMPTY:
        return "APPLIANCE_AUDIT_CHAIN_EMPTY";
    case APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT:
        return "APPLIANCE_AUDIT_CHAIN_BAD_ARGUMENT";
    case APPLIANCE_AUDIT_CHAIN_SHA_FAILED:
        return "APPLIANCE_AUDIT_CHAIN_SHA_FAILED";
    case APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL:
        return "APPLIANCE_AUDIT_CHAIN_BUFFER_TOO_SMALL";
    default:
        return NULL;
    }
}

const char *appliance_audit_chain_state_string(
    enum appliance_audit_chain_state state)
{
    switch (state) {
    case APPLIANCE_AUDIT_CHAIN_STATE_NONE:
        return "NONE";
    case APPLIANCE_AUDIT_CHAIN_STATE_EMPTY:
        return "EMPTY";
    case APPLIANCE_AUDIT_CHAIN_STATE_OK:
        return "OK";
    case APPLIANCE_AUDIT_CHAIN_STATE_TAMPERED:
        return "TAMPERED";
    default:
        return NULL;
    }
}

#ifdef HFOC_APPLIANCE_AUDIT_CHAIN_HOST_TEST
bool appliance_audit_chain_host_test_tamper(
    struct appliance_audit_chain *chain,
    uint32_t index)
{
    if (!appliance_audit_chain_valid(chain) || index >= chain->count) {
        return false;
    }
    chain->entries[index].record.detail ^= UINT64_C(0x5A5A5A5A);
    return true;
}

bool appliance_audit_chain_host_test_tamper_anchor(
    struct appliance_audit_chain *chain)
{
    if (!appliance_audit_chain_valid(chain)) {
        return false;
    }
    chain->anchor_digest[0] ^= UINT8_C(0xA5);
    return true;
}
#endif
