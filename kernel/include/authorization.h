#ifndef HFOC_AUTHORIZATION_H
#define HFOC_AUTHORIZATION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AUTHORIZATION_VERSION 1U
/*
 * ITEM-516 added the seventeenth managed entry (the gate mutation), so the
 * capacity moves with it: the matrix refuses a policy above this bound.
 */
#define AUTHORIZATION_MATRIX_CAPACITY_MAX 17U
#define AUTHORIZATION_COMMAND_ID_MIN 1U
#define AUTHORIZATION_COMMAND_ID_MAX 255U
#define AUTHORIZATION_ROLE_ID_MIN 1U
#define AUTHORIZATION_ROLE_ID_MAX 255U
#define AUTHORIZATION_CAPABILITY_ID_MIN 1U
#define AUTHORIZATION_CAPABILITY_ID_MAX 255U
#define AUTHORIZATION_ROLE_SET_MAX 8U
#define AUTHORIZATION_CAPABILITY_SET_MAX 8U

#define AUTHORIZATION_ROLE_ADMIN 1U
#define AUTHORIZATION_ROLE_OPERATOR 2U
#define AUTHORIZATION_ROLE_AUDITOR 3U
#define AUTHORIZATION_ROLE_INTEGRATION 4U

#define AUTHORIZATION_CAPABILITY_STATUS_READ 1U
#define AUTHORIZATION_CAPABILITY_CONFIG_WRITE 2U
#define AUTHORIZATION_CAPABILITY_AUDIT_READ 3U
#define AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION 4U

enum authorization_status {
    AUTHORIZATION_OK = 0,
    AUTHORIZATION_NULL = 1,
    AUTHORIZATION_BAD_CAPACITY = 2,
    AUTHORIZATION_BAD_VERSION = 3,
    AUTHORIZATION_UNKNOWN_COMMAND = 4,
    AUTHORIZATION_INVALID_DESCRIPTOR = 5,
    AUTHORIZATION_DUPLICATE_COMMAND = 6,
    AUTHORIZATION_INVALID_ROLE_SET = 7,
    AUTHORIZATION_INVALID_CAPABILITY_SET = 8,
    AUTHORIZATION_MISSING_ROLE = 9,
    AUTHORIZATION_MISSING_CAPABILITY = 10,
    AUTHORIZATION_BAD_STATE = 11,
};

struct authorization_entry {
    uint32_t version;
    uint32_t command_id;
    uint32_t role_count;
    uint8_t roles[AUTHORIZATION_ROLE_SET_MAX];
    uint32_t capability_count;
    uint8_t capabilities[AUTHORIZATION_CAPABILITY_SET_MAX];
};

struct authorization_matrix {
    uint32_t version;
    size_t count;
    bool initialized;
    struct authorization_entry entries[
        AUTHORIZATION_MATRIX_CAPACITY_MAX];
};

enum authorization_status authorization_matrix_init(
    struct authorization_matrix *matrix,
    const struct authorization_entry *entries,
    size_t count);
enum authorization_status authorization_check(
    const struct authorization_matrix *matrix,
    uint32_t command_id,
    const uint8_t *roles,
    size_t role_count,
    const uint8_t *capabilities,
    size_t capability_count);
const char *authorization_status_string(
    enum authorization_status status);

#endif
