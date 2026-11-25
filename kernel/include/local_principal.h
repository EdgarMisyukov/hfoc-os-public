#ifndef HFOC_LOCAL_PRINCIPAL_H
#define HFOC_LOCAL_PRINCIPAL_H

#include <stdbool.h>
#include <stdint.h>

#define LOCAL_PRINCIPAL_DESCRIPTOR_VERSION 1U
#define LOCAL_PRINCIPAL_ID_MIN 1U
#define LOCAL_PRINCIPAL_ID_MAX 255U
#define LOCAL_PRINCIPAL_NAME_MAX 31U
#define LOCAL_PRINCIPAL_ROLE_MAX 8U
#define LOCAL_PRINCIPAL_ROLE_ID_MIN 1U
#define LOCAL_PRINCIPAL_ROLE_ID_MAX 255U
#define LOCAL_PRINCIPAL_SET_MAX 16U

enum local_principal_state {
    LOCAL_PRINCIPAL_STATE_DISABLED = 0,
    LOCAL_PRINCIPAL_STATE_ENABLED = 1,
};

enum local_principal_status {
    LOCAL_PRINCIPAL_OK = 0,
    LOCAL_PRINCIPAL_NULL = 1,
    LOCAL_PRINCIPAL_INVALID_ID = 2,
    LOCAL_PRINCIPAL_INVALID_VERSION = 3,
    LOCAL_PRINCIPAL_INVALID_NAME = 4,
    LOCAL_PRINCIPAL_INVALID_STATE = 5,
    LOCAL_PRINCIPAL_INVALID_ROLE_COUNT = 6,
    LOCAL_PRINCIPAL_INVALID_ROLE_ID = 7,
    LOCAL_PRINCIPAL_DUPLICATE_ROLE = 8,
};

enum local_principal_set_status {
    LOCAL_PRINCIPAL_SET_OK = 0,
    LOCAL_PRINCIPAL_SET_NULL = 1,
    LOCAL_PRINCIPAL_SET_BAD_CAPACITY = 2,
    LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR = 3,
    LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL = 4,
};

struct local_principal {
    uint32_t id;
    uint32_t version;
    enum local_principal_state state;
    char name[LOCAL_PRINCIPAL_NAME_MAX + 1U];
    uint32_t role_count;
    uint8_t roles[LOCAL_PRINCIPAL_ROLE_MAX];
};

enum local_principal_status local_principal_validate(
    const struct local_principal *principal);
enum local_principal_status local_principal_init(
    struct local_principal *published,
    const struct local_principal *principal);
const char *local_principal_status_string(
    enum local_principal_status status);
enum local_principal_set_status local_principal_set_validate(
    const struct local_principal *principals,
    uint32_t principal_count);
const char *local_principal_set_status_string(
    enum local_principal_set_status status);

#endif
