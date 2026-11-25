#include "https_admission.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool https_admission_policy_valid(
    const struct https_admission_policy *policy)
{
    return policy != NULL &&
           policy->version == HTTPS_ADMISSION_VERSION &&
           policy->address[0] == HTTPS_ADMISSION_QEMU_ADDRESS_0 &&
           policy->address[1] == HTTPS_ADMISSION_QEMU_ADDRESS_1 &&
           policy->address[2] == HTTPS_ADMISSION_QEMU_ADDRESS_2 &&
           policy->address[3] == HTTPS_ADMISSION_QEMU_ADDRESS_3 &&
           policy->port == HTTPS_ADMISSION_QEMU_PORT;
}

static bool https_admission_generation_valid(uint32_t generation)
{
    return generation >= HTTPS_ADMISSION_GENERATION_MIN &&
           generation <= HTTPS_ADMISSION_GENERATION_MAX;
}

static bool https_admission_generations_valid(
    const struct https_admission_generations *generations)
{
    return generations != NULL &&
           generations->version == HTTPS_ADMISSION_VERSION &&
           https_admission_generation_valid(
               generations->network_generation) &&
           https_admission_generation_valid(
               generations->tls_policy_generation) &&
           https_admission_generation_valid(
               generations->service_generation);
}

static bool https_admission_binding_valid(
    const struct https_admission_binding *binding)
{
    return binding != NULL &&
           binding->published &&
           binding->version == HTTPS_ADMISSION_VERSION &&
           https_admission_policy_valid(&binding->policy) &&
           https_admission_generations_valid(&binding->generations);
}

static void https_admission_clear_pending(
    struct https_admission_transaction *transaction)
{
    memset(&transaction->pending, 0, sizeof(transaction->pending));
    transaction->has_pending = false;
}

enum https_admission_status https_admission_policy_validate(
    const struct https_admission_policy *policy)
{
    if (policy == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (policy->version != HTTPS_ADMISSION_VERSION) {
        return HTTPS_ADMISSION_BAD_VERSION;
    }
    if (!https_admission_policy_valid(policy)) {
        if (policy->address[0] != HTTPS_ADMISSION_QEMU_ADDRESS_0 ||
            policy->address[1] != HTTPS_ADMISSION_QEMU_ADDRESS_1 ||
            policy->address[2] != HTTPS_ADMISSION_QEMU_ADDRESS_2 ||
            policy->address[3] != HTTPS_ADMISSION_QEMU_ADDRESS_3) {
            return HTTPS_ADMISSION_BAD_ADDRESS;
        }
        return HTTPS_ADMISSION_BAD_PORT;
    }
    return HTTPS_ADMISSION_OK;
}

enum https_admission_status https_admission_generations_validate(
    const struct https_admission_generations *generations)
{
    if (generations == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (generations->version != HTTPS_ADMISSION_VERSION) {
        return HTTPS_ADMISSION_BAD_VERSION;
    }
    if (!https_admission_generation_valid(
            generations->network_generation) ||
        !https_admission_generation_valid(
            generations->tls_policy_generation) ||
        !https_admission_generation_valid(
            generations->service_generation)) {
        return HTTPS_ADMISSION_BAD_GENERATION;
    }
    return HTTPS_ADMISSION_OK;
}

bool https_admission_binding_published(
    const struct https_admission_binding *binding)
{
    return https_admission_binding_valid(binding);
}

enum https_admission_status https_admission_transaction_init(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations)
{
    enum https_admission_status status;

    if (transaction == NULL || policy == NULL || generations == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (transaction->has_active) {
        return HTTPS_ADMISSION_ALREADY_PUBLISHED;
    }
    if (transaction->version != 0U ||
        transaction->has_pending ||
        transaction->active.published ||
        transaction->pending.published) {
        return HTTPS_ADMISSION_BAD_STATE;
    }

    status = https_admission_policy_validate(policy);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }
    status = https_admission_generations_validate(generations);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }

    memset(transaction, 0, sizeof(*transaction));
    transaction->version = HTTPS_ADMISSION_VERSION;
    transaction->active.published = true;
    transaction->active.version = HTTPS_ADMISSION_VERSION;
    transaction->active.policy = *policy;
    transaction->active.generations = *generations;
    transaction->has_active = true;
    return HTTPS_ADMISSION_OK;
}

enum https_admission_status https_admission_transaction_stage(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations)
{
    enum https_admission_status status;

    if (transaction == NULL || policy == NULL || generations == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (transaction->version != HTTPS_ADMISSION_VERSION ||
        !transaction->has_active) {
        return HTTPS_ADMISSION_UNPUBLISHED;
    }

    https_admission_clear_pending(transaction);
    status = https_admission_policy_validate(policy);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }
    status = https_admission_generations_validate(generations);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }

    memset(&transaction->pending, 0, sizeof(transaction->pending));
    transaction->pending.version = HTTPS_ADMISSION_VERSION;
    transaction->pending.policy = *policy;
    transaction->pending.generations = *generations;
    transaction->has_pending = true;
    return HTTPS_ADMISSION_OK;
}

enum https_admission_status https_admission_transaction_commit(
    struct https_admission_transaction *transaction)
{
    enum https_admission_status status;

    if (transaction == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (transaction->version != HTTPS_ADMISSION_VERSION ||
        !transaction->has_active) {
        return HTTPS_ADMISSION_UNPUBLISHED;
    }
    if (!transaction->has_pending) {
        return HTTPS_ADMISSION_NO_PENDING;
    }
    if (transaction->pending.version != HTTPS_ADMISSION_VERSION) {
        https_admission_clear_pending(transaction);
        return HTTPS_ADMISSION_BAD_VERSION;
    }
    status = https_admission_policy_validate(
        &transaction->pending.policy);
    if (status != HTTPS_ADMISSION_OK) {
        https_admission_clear_pending(transaction);
        return status;
    }
    status = https_admission_generations_validate(
        &transaction->pending.generations);
    if (status != HTTPS_ADMISSION_OK) {
        https_admission_clear_pending(transaction);
        return status;
    }

    transaction->active = transaction->pending;
    transaction->active.published = true;
    https_admission_clear_pending(transaction);
    return HTTPS_ADMISSION_OK;
}

enum https_admission_status https_admission_transaction_rollback(
    struct https_admission_transaction *transaction)
{
    if (transaction == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (transaction->version != HTTPS_ADMISSION_VERSION ||
        !transaction->has_active) {
        return HTTPS_ADMISSION_UNPUBLISHED;
    }
    if (!transaction->has_pending) {
        return HTTPS_ADMISSION_NO_PENDING;
    }
    https_admission_clear_pending(transaction);
    return HTTPS_ADMISSION_OK;
}

enum https_admission_status https_admission_transaction_validate_state(
    const struct https_admission_transaction *transaction,
    const struct https_admission_policy *observed_policy,
    const struct https_admission_generations *observed_generations)
{
    enum https_admission_status status;

    if (transaction == NULL ||
        observed_policy == NULL ||
        observed_generations == NULL) {
        return HTTPS_ADMISSION_NULL;
    }
    if (transaction->version != HTTPS_ADMISSION_VERSION ||
        !transaction->has_active ||
        !https_admission_binding_valid(&transaction->active)) {
        return HTTPS_ADMISSION_UNPUBLISHED;
    }

    if (memcmp(&transaction->active.policy, observed_policy,
               sizeof(*observed_policy)) != 0 ||
        memcmp(&transaction->active.generations, observed_generations,
               sizeof(*observed_generations)) != 0) {
        return HTTPS_ADMISSION_CHANGED;
    }
    status = https_admission_policy_validate(observed_policy);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }
    status = https_admission_generations_validate(
        observed_generations);
    if (status != HTTPS_ADMISSION_OK) {
        return status;
    }
    return HTTPS_ADMISSION_OK;
}

const struct https_admission_binding *https_admission_transaction_active(
    const struct https_admission_transaction *transaction)
{
    if (!https_admission_transaction_published(transaction)) {
        return NULL;
    }
    return &transaction->active;
}

bool https_admission_transaction_published(
    const struct https_admission_transaction *transaction)
{
    return transaction != NULL &&
           transaction->version == HTTPS_ADMISSION_VERSION &&
           transaction->has_active &&
           https_admission_binding_valid(&transaction->active);
}

bool https_admission_transaction_has_pending(
    const struct https_admission_transaction *transaction)
{
    return transaction != NULL &&
           transaction->version == HTTPS_ADMISSION_VERSION &&
           transaction->has_pending &&
           transaction->pending.version == HTTPS_ADMISSION_VERSION &&
           https_admission_policy_valid(&transaction->pending.policy) &&
           https_admission_generations_valid(
               &transaction->pending.generations);
}

const char *https_admission_status_string(
    enum https_admission_status status)
{
    switch (status) {
    case HTTPS_ADMISSION_OK:
        return "HTTPS_ADMISSION_OK";
    case HTTPS_ADMISSION_NULL:
        return "HTTPS_ADMISSION_NULL";
    case HTTPS_ADMISSION_BAD_STATE:
        return "HTTPS_ADMISSION_BAD_STATE";
    case HTTPS_ADMISSION_BAD_VERSION:
        return "HTTPS_ADMISSION_BAD_VERSION";
    case HTTPS_ADMISSION_BAD_ADDRESS:
        return "HTTPS_ADMISSION_BAD_ADDRESS";
    case HTTPS_ADMISSION_BAD_PORT:
        return "HTTPS_ADMISSION_BAD_PORT";
    case HTTPS_ADMISSION_BAD_GENERATION:
        return "HTTPS_ADMISSION_BAD_GENERATION";
    case HTTPS_ADMISSION_ALREADY_PUBLISHED:
        return "HTTPS_ADMISSION_ALREADY_PUBLISHED";
    case HTTPS_ADMISSION_UNPUBLISHED:
        return "HTTPS_ADMISSION_UNPUBLISHED";
    case HTTPS_ADMISSION_NO_PENDING:
        return "HTTPS_ADMISSION_NO_PENDING";
    case HTTPS_ADMISSION_CHANGED:
        return "HTTPS_ADMISSION_CHANGED";
    default:
        return NULL;
    }
}
