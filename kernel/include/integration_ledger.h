#ifndef HFOC_INTEGRATION_LEDGER_H
#define HFOC_INTEGRATION_LEDGER_H

#include "integration_envelope.h"

#include <stdbool.h>
#include <stdint.h>

#define INTEGRATION_LEDGER_VERSION 1U
#define INTEGRATION_LEDGER_IDENTITY_SIZE INTEGRATION_ENVELOPE_DIGEST_SIZE
#define INTEGRATION_LEDGER_MAX_ENTRIES 64U

enum integration_ledger_status {
    INTEGRATION_LEDGER_OK = 0,
    INTEGRATION_LEDGER_NULL = 1,
    INTEGRATION_LEDGER_BAD_CAPACITY = 2,
    INTEGRATION_LEDGER_BAD_STATE = 3,
    INTEGRATION_LEDGER_INVALID_IDENTITY = 4,
    INTEGRATION_LEDGER_FULL = 5,
    INTEGRATION_LEDGER_DUPLICATE = 6,
    INTEGRATION_LEDGER_NOT_FOUND = 7,
    INTEGRATION_LEDGER_NOT_PREPARED = 8,
    INTEGRATION_LEDGER_NOT_COMMITTED = 9,
    INTEGRATION_LEDGER_ALREADY_COMMITTED = 10,
};

struct integration_ledger_entry {
    uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE];
    uint32_t result;
    bool pending;
    bool committed;
};

struct integration_ledger {
    uint32_t version;
    uint32_t capacity;
    uint32_t count;
    struct integration_ledger_entry
        entries[INTEGRATION_LEDGER_MAX_ENTRIES];
};

enum integration_ledger_status integration_ledger_init(
    struct integration_ledger *ledger,
    uint32_t capacity);
enum integration_ledger_status integration_ledger_prepare(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t result,
    uint32_t *result_out);
enum integration_ledger_status integration_ledger_commit(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t *result_out);
enum integration_ledger_status integration_ledger_replay(
    const struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE],
    uint32_t *result_out);
enum integration_ledger_status integration_ledger_rollback(
    struct integration_ledger *ledger,
    const uint8_t identity[INTEGRATION_LEDGER_IDENTITY_SIZE]);
const char *integration_ledger_status_string(
    enum integration_ledger_status status);

#endif
