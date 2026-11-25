#include "network_config_transaction.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static void network_config_transaction_clear_pending(
    struct network_config_transaction *transaction)
{
    memset(&transaction->pending, 0, sizeof(transaction->pending));
    transaction->has_pending = false;
}

static bool network_config_transaction_active_valid(
    const struct network_config_transaction *transaction)
{
    return transaction != NULL &&
           transaction->version == NETWORK_CONFIG_TRANSACTION_VERSION &&
           transaction->has_active;
}

enum network_config_transaction_status network_config_transaction_init(
    struct network_config_transaction *transaction,
    const struct network_ipv4_config *initial)
{
    enum network_ipv4_status status;

    if (transaction == NULL || initial == NULL) {
        return NETWORK_CONFIG_TRANSACTION_NULL;
    }
    if (transaction->has_active) {
        return NETWORK_CONFIG_TRANSACTION_BAD_STATE;
    }
    status = network_ipv4_config_validate(initial);
    if (status != NETWORK_IPV4_OK) {
        return NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG;
    }

    memset(transaction, 0, sizeof(*transaction));
    transaction->version = NETWORK_CONFIG_TRANSACTION_VERSION;
    transaction->active = *initial;
    transaction->has_active = true;
    return NETWORK_CONFIG_TRANSACTION_OK;
}

enum network_config_transaction_status network_config_transaction_stage(
    struct network_config_transaction *transaction,
    const char *input,
    size_t input_length)
{
    struct network_ipv4_config candidate;
    enum network_ipv4_status status;

    if (transaction == NULL || input == NULL) {
        return NETWORK_CONFIG_TRANSACTION_NULL;
    }
    if (!network_config_transaction_active_valid(transaction)) {
        return NETWORK_CONFIG_TRANSACTION_BAD_STATE;
    }

    network_config_transaction_clear_pending(transaction);
    if (input_length > NETWORK_IPV4_INPUT_MAX) {
        return NETWORK_CONFIG_TRANSACTION_PARSE_FAILED;
    }
    status = network_ipv4_config_parse(
        input, input_length, &candidate);
    if (status != NETWORK_IPV4_OK) {
        return NETWORK_CONFIG_TRANSACTION_PARSE_FAILED;
    }

    transaction->pending = candidate;
    transaction->has_pending = true;
    return NETWORK_CONFIG_TRANSACTION_OK;
}

enum network_config_transaction_status network_config_transaction_commit(
    struct network_config_transaction *transaction)
{
    enum network_ipv4_status status;

    if (transaction == NULL) {
        return NETWORK_CONFIG_TRANSACTION_NULL;
    }
    if (!network_config_transaction_active_valid(transaction)) {
        return NETWORK_CONFIG_TRANSACTION_BAD_STATE;
    }
    if (!transaction->has_pending) {
        return NETWORK_CONFIG_TRANSACTION_NO_PENDING;
    }

    status = network_ipv4_config_validate(&transaction->pending);
    if (status != NETWORK_IPV4_OK) {
        network_config_transaction_clear_pending(transaction);
        return NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG;
    }

    transaction->active = transaction->pending;
    network_config_transaction_clear_pending(transaction);
    return NETWORK_CONFIG_TRANSACTION_OK;
}

enum network_config_transaction_status network_config_transaction_rollback(
    struct network_config_transaction *transaction)
{
    if (transaction == NULL) {
        return NETWORK_CONFIG_TRANSACTION_NULL;
    }
    if (!network_config_transaction_active_valid(transaction)) {
        return NETWORK_CONFIG_TRANSACTION_BAD_STATE;
    }
    if (!transaction->has_pending) {
        return NETWORK_CONFIG_TRANSACTION_NO_PENDING;
    }

    network_config_transaction_clear_pending(transaction);
    return NETWORK_CONFIG_TRANSACTION_OK;
}

const struct network_ipv4_config *network_config_transaction_active(
    const struct network_config_transaction *transaction)
{
    return transaction != NULL && transaction->has_active
        ? &transaction->active
        : NULL;
}

bool network_config_transaction_has_pending(
    const struct network_config_transaction *transaction)
{
    return transaction != NULL && transaction->has_pending;
}

const char *network_config_transaction_status_string(
    enum network_config_transaction_status status)
{
    switch (status) {
    case NETWORK_CONFIG_TRANSACTION_OK:
        return "NETWORK_CONFIG_TRANSACTION_OK";
    case NETWORK_CONFIG_TRANSACTION_NULL:
        return "NETWORK_CONFIG_TRANSACTION_NULL";
    case NETWORK_CONFIG_TRANSACTION_BAD_STATE:
        return "NETWORK_CONFIG_TRANSACTION_BAD_STATE";
    case NETWORK_CONFIG_TRANSACTION_NO_PENDING:
        return "NETWORK_CONFIG_TRANSACTION_NO_PENDING";
    case NETWORK_CONFIG_TRANSACTION_PARSE_FAILED:
        return "NETWORK_CONFIG_TRANSACTION_PARSE_FAILED";
    case NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG:
        return "NETWORK_CONFIG_TRANSACTION_INVALID_CONFIG";
    default:
        return NULL;
    }
}
