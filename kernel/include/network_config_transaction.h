#ifndef HFOC_NETWORK_CONFIG_TRANSACTION_H
#define HFOC_NETWORK_CONFIG_TRANSACTION_H

#include "ipv4_config.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define NETWORK_CONFIG_TRANSACTION_VERSION 1U

enum network_config_transaction_status {
    NETWORK_CONFIG_TRANSACTION_OK = 0,
    NETWORK_CONFIG_TRANSACTION_NULL = 1,
    NETWORK_CONFIG_TRANSACTION_BAD_STATE = 2,
    NETWORK_CONFIG_TRANSACTION_NO_PENDING = 3,
    NETWORK_CONFIG_TRANSACTION_PARSE_FAILED = 4,
    NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG = 5,
};

struct network_config_transaction {
    uint32_t version;
    struct network_ipv4_config active;
    struct network_ipv4_config pending;
    bool has_active;
    bool has_pending;
};

enum network_config_transaction_status network_config_transaction_init(
    struct network_config_transaction *transaction,
    const struct network_ipv4_config *initial);
enum network_config_transaction_status network_config_transaction_stage(
    struct network_config_transaction *transaction,
    const char *input,
    size_t input_length);
enum network_config_transaction_status network_config_transaction_commit(
    struct network_config_transaction *transaction);
enum network_config_transaction_status network_config_transaction_rollback(
    struct network_config_transaction *transaction);
const struct network_ipv4_config *network_config_transaction_active(
    const struct network_config_transaction *transaction);
bool network_config_transaction_has_pending(
    const struct network_config_transaction *transaction);
const char *network_config_transaction_status_string(
    enum network_config_transaction_status status);

#endif
