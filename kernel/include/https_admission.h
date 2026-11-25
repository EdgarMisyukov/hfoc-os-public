#ifndef HFOC_HTTPS_ADMISSION_H
#define HFOC_HTTPS_ADMISSION_H

#include <stdbool.h>
#include <stdint.h>

#define HTTPS_ADMISSION_VERSION 1U
#define HTTPS_ADMISSION_GENERATION_MIN 1U
#define HTTPS_ADMISSION_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define HTTPS_ADMISSION_QEMU_ADDRESS_0 10U
#define HTTPS_ADMISSION_QEMU_ADDRESS_1 0U
#define HTTPS_ADMISSION_QEMU_ADDRESS_2 2U
#define HTTPS_ADMISSION_QEMU_ADDRESS_3 15U
#define HTTPS_ADMISSION_QEMU_ADDRESS_LENGTH 4U
#define HTTPS_ADMISSION_QEMU_ADDRESS_STRING "10.0.2.15"
#define HTTPS_ADMISSION_QEMU_PORT 443U

enum https_admission_status {
    HTTPS_ADMISSION_OK = 0,
    HTTPS_ADMISSION_NULL = 1,
    HTTPS_ADMISSION_BAD_STATE = 2,
    HTTPS_ADMISSION_BAD_VERSION = 3,
    HTTPS_ADMISSION_BAD_ADDRESS = 4,
    HTTPS_ADMISSION_BAD_PORT = 5,
    HTTPS_ADMISSION_BAD_GENERATION = 6,
    HTTPS_ADMISSION_ALREADY_PUBLISHED = 7,
    HTTPS_ADMISSION_UNPUBLISHED = 8,
    HTTPS_ADMISSION_NO_PENDING = 9,
    HTTPS_ADMISSION_CHANGED = 10,
};

struct https_admission_policy {
    uint32_t version;
    uint8_t address[HTTPS_ADMISSION_QEMU_ADDRESS_LENGTH];
    uint16_t port;
};

struct https_admission_generations {
    uint32_t version;
    uint32_t network_generation;
    uint32_t tls_policy_generation;
    uint32_t service_generation;
};

struct https_admission_binding {
    bool published;
    uint32_t version;
    struct https_admission_policy policy;
    struct https_admission_generations generations;
};

struct https_admission_transaction {
    uint32_t version;
    struct https_admission_binding active;
    struct https_admission_binding pending;
    bool has_active;
    bool has_pending;
};

enum https_admission_status https_admission_policy_validate(
    const struct https_admission_policy *policy);
enum https_admission_status https_admission_generations_validate(
    const struct https_admission_generations *generations);
bool https_admission_binding_published(
    const struct https_admission_binding *binding);
enum https_admission_status https_admission_transaction_init(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations);
enum https_admission_status https_admission_transaction_stage(
    struct https_admission_transaction *transaction,
    const struct https_admission_policy *policy,
    const struct https_admission_generations *generations);
enum https_admission_status https_admission_transaction_commit(
    struct https_admission_transaction *transaction);
enum https_admission_status https_admission_transaction_rollback(
    struct https_admission_transaction *transaction);
enum https_admission_status https_admission_transaction_validate_state(
    const struct https_admission_transaction *transaction,
    const struct https_admission_policy *observed_policy,
    const struct https_admission_generations *observed_generations);
const struct https_admission_binding *https_admission_transaction_active(
    const struct https_admission_transaction *transaction);
bool https_admission_transaction_published(
    const struct https_admission_transaction *transaction);
bool https_admission_transaction_has_pending(
    const struct https_admission_transaction *transaction);
const char *https_admission_status_string(
    enum https_admission_status status);

#endif
