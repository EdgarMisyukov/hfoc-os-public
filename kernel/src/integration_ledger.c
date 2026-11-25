#include "integration_ledger.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool integration_ledger_identity_zero(
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE])
{
    uint32_t index;

    for (index = 0U; index < INTEGRATION_LEDGER_IDENTITY_SIZE; ++index) {
        if (identity[index] != 0U) {
            return false;
        }
    }
    return true;
}

static enum integration_ledger_status integration_ledger_validate(
    const struct integration_ledger *ledger)
{
    if (ledger->capacity == 0U ||
        ledger->capacity > INTEGRATION_LEDGER_MAX_ENTRIES) {
        return INTEGRATION_LEDGER_BAD_CAPACITY;
    }
    if (ledger->version != INTEGRATION_LEDGER_VERSION ||
        ledger->count > ledger->capacity) {
        return INTEGRATION_LEDGER_BAD_STATE;
    }
    return INTEGRATION_LEDGER_OK;
}

static uint32_t integration_ledger_find(
    const struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE])
{
    uint32_t index;

    for (index = 0U; index < ledger->count; ++index) {
        if (memcmp(ledger->entries[index].identity,
                   identity,
                   INTEGRATION_LEDGER_IDENTITY_SIZE) == 0) {
            return index;
        }
    }
    return INTEGRATION_LEDGER_MAX_ENTRIES;
}

enum integration_ledger_status integration_ledger_init(
    struct integration_ledger *ledger,
    uint32_t capacity)
{
    if (ledger == NULL) {
        return INTEGRATION_LEDGER_NULL;
    }
    if (capacity == 0U ||
        capacity > INTEGRATION_LEDGER_MAX_ENTRIES) {
        return INTEGRATION_LEDGER_BAD_CAPACITY;
    }

    memset(ledger, 0, sizeof(*ledger));
    ledger->version = INTEGRATION_LEDGER_VERSION;
    ledger->capacity = capacity;
    return INTEGRATION_LEDGER_OK;
}

enum integration_ledger_status integration_ledger_prepare(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t result,
    uint32_t *result_out)
{
    enum integration_ledger_status status;
    uint32_t index;

    if (ledger == NULL || identity == NULL || result_out == NULL) {
        return INTEGRATION_LEDGER_NULL;
    }
    status = integration_ledger_validate(ledger);
    if (status != INTEGRATION_LEDGER_OK) {
        return status;
    }
    if (integration_ledger_identity_zero(identity)) {
        return INTEGRATION_LEDGER_INVALID_IDENTITY;
    }

    index = integration_ledger_find(ledger, identity);
    if (index < ledger->count) {
        *result_out = ledger->entries[index].result;
        return ledger->entries[index].committed
                   ? INTEGRATION_LEDGER_ALREADY_COMMITTED
                   : INTEGRATION_LEDGER_DUPLICATE;
    }
    if (ledger->count >= ledger->capacity) {
        return INTEGRATION_LEDGER_FULL;
    }

    memcpy(ledger->entries[ledger->count].identity,
           identity,
           INTEGRATION_LEDGER_IDENTITY_SIZE);
    ledger->entries[ledger->count].result = result;
    ledger->entries[ledger->count].pending = true;
    ledger->entries[ledger->count].committed = false;
    ++ledger->count;
    *result_out = result;
    return INTEGRATION_LEDGER_OK;
}

enum integration_ledger_status integration_ledger_commit(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t *result_out)
{
    enum integration_ledger_status status;
    uint32_t index;

    if (ledger == NULL || identity == NULL || result_out == NULL) {
        return INTEGRATION_LEDGER_NULL;
    }
    status = integration_ledger_validate(ledger);
    if (status != INTEGRATION_LEDGER_OK) {
        return status;
    }
    if (integration_ledger_identity_zero(identity)) {
        return INTEGRATION_LEDGER_INVALID_IDENTITY;
    }

    index = integration_ledger_find(ledger, identity);
    if (index >= ledger->count) {
        return INTEGRATION_LEDGER_NOT_PREPARED;
    }
    if (ledger->entries[index].committed) {
        *result_out = ledger->entries[index].result;
        return INTEGRATION_LEDGER_ALREADY_COMMITTED;
    }
    if (!ledger->entries[index].pending) {
        return INTEGRATION_LEDGER_BAD_STATE;
    }

    ledger->entries[index].pending = false;
    ledger->entries[index].committed = true;
    *result_out = ledger->entries[index].result;
    return INTEGRATION_LEDGER_OK;
}

enum integration_ledger_status integration_ledger_replay(
    const struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t *result_out)
{
    enum integration_ledger_status status;
    uint32_t index;

    if (ledger == NULL || identity == NULL || result_out == NULL) {
        return INTEGRATION_LEDGER_NULL;
    }
    status = integration_ledger_validate(ledger);
    if (status != INTEGRATION_LEDGER_OK) {
        return status;
    }
    if (integration_ledger_identity_zero(identity)) {
        return INTEGRATION_LEDGER_INVALID_IDENTITY;
    }

    index = integration_ledger_find(ledger, identity);
    if (index >= ledger->count) {
        return INTEGRATION_LEDGER_NOT_FOUND;
    }
    if (!ledger->entries[index].committed) {
        return INTEGRATION_LEDGER_NOT_COMMITTED;
    }
    *result_out = ledger->entries[index].result;
    return INTEGRATION_LEDGER_OK;
}

enum integration_ledger_status integration_ledger_rollback(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE])
{
    enum integration_ledger_status status;
    uint32_t index;

    if (ledger == NULL || identity == NULL) {
        return INTEGRATION_LEDGER_NULL;
    }
    status = integration_ledger_validate(ledger);
    if (status != INTEGRATION_LEDGER_OK) {
        return status;
    }
    if (integration_ledger_identity_zero(identity)) {
        return INTEGRATION_LEDGER_INVALID_IDENTITY;
    }

    index = integration_ledger_find(ledger, identity);
    if (index >= ledger->count) {
        return INTEGRATION_LEDGER_NOT_FOUND;
    }
    if (ledger->entries[index].committed) {
        return INTEGRATION_LEDGER_ALREADY_COMMITTED;
    }
    if (!ledger->entries[index].pending) {
        return INTEGRATION_LEDGER_BAD_STATE;
    }

    if (index + 1U < ledger->count) {
        memcpy(&ledger->entries[index],
               &ledger->entries[ledger->count - 1U],
               sizeof(ledger->entries[index]));
    }
    memset(&ledger->entries[ledger->count - 1U],
           0,
           sizeof(ledger->entries[0]));
    --ledger->count;
    return INTEGRATION_LEDGER_OK;
}

const char *integration_ledger_status_string(
    enum integration_ledger_status status)
{
    switch (status) {
    case INTEGRATION_LEDGER_OK:
        return "INTEGRATION_LEDGER_OK";
    case INTEGRATION_LEDGER_NULL:
        return "INTEGRATION_LEDGER_NULL";
    case INTEGRATION_LEDGER_BAD_CAPACITY:
        return "INTEGRATION_LEDGER_BAD_CAPACITY";
    case INTEGRATION_LEDGER_BAD_STATE:
        return "INTEGRATION_LEDGER_BAD_STATE";
    case INTEGRATION_LEDGER_INVALID_IDENTITY:
        return "INTEGRATION_LEDGER_INVALID_IDENTITY";
    case INTEGRATION_LEDGER_FULL:
        return "INTEGRATION_LEDGER_FULL";
    case INTEGRATION_LEDGER_DUPLICATE:
        return "INTEGRATION_LEDGER_DUPLICATE";
    case INTEGRATION_LEDGER_NOT_FOUND:
        return "INTEGRATION_LEDGER_NOT_FOUND";
    case INTEGRATION_LEDGER_NOT_PREPARED:
        return "INTEGRATION_LEDGER_NOT_PREPARED";
    case INTEGRATION_LEDGER_NOT_COMMITTED:
        return "INTEGRATION_LEDGER_NOT_COMMITTED";
    case INTEGRATION_LEDGER_ALREADY_COMMITTED:
        return "INTEGRATION_LEDGER_ALREADY_COMMITTED";
    default:
        return NULL;
    }
}
