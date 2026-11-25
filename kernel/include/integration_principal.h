#ifndef HFOC_INTEGRATION_PRINCIPAL_H
#define HFOC_INTEGRATION_PRINCIPAL_H

#include "authorization.h"

#include <stdint.h>

#define INTEGRATION_PRINCIPAL_VERSION 1U

enum integration_principal_status {
    INTEGRATION_PRINCIPAL_OK = 0,
    INTEGRATION_PRINCIPAL_NULL = 1,
    INTEGRATION_PRINCIPAL_BAD_VERSION = 2,
    INTEGRATION_PRINCIPAL_INVALID_SCOPE = 3,
    INTEGRATION_PRINCIPAL_MISSING_ROLE = 4,
    INTEGRATION_PRINCIPAL_MISSING_CAPABILITY = 5,
    INTEGRATION_PRINCIPAL_BAD_STATE = 6,
};

struct integration_principal_scope {
    uint32_t version;
    uint8_t role;
    uint8_t capability;
};

enum integration_principal_status integration_principal_scope_validate(
    const struct integration_principal_scope *scope);
enum integration_principal_status integration_principal_scope_init(
    struct integration_principal_scope *published,
    const struct integration_principal_scope *scope);
enum integration_principal_status integration_principal_scope_check(
    const struct integration_principal_scope *scope,
    const struct authorization_matrix *matrix,
    uint32_t command_id);
const char *integration_principal_status_string(
    enum integration_principal_status status);

#endif
