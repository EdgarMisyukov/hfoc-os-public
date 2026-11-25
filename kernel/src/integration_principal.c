#include "integration_principal.h"

#include <stddef.h>
#include <stdint.h>

enum integration_principal_status integration_principal_scope_validate(
    const struct integration_principal_scope *scope)
{
    if (scope == NULL) {
        return INTEGRATION_PRINCIPAL_NULL;
    }
    if (scope->version != INTEGRATION_PRINCIPAL_VERSION) {
        return INTEGRATION_PRINCIPAL_BAD_VERSION;
    }
    if (scope->role != AUTHORIZATION_ROLE_INTEGRATION ||
        scope->capability !=
            AUTHORIZATION_CAPABILITY_INTEGRATION_SESSION) {
        return INTEGRATION_PRINCIPAL_INVALID_SCOPE;
    }
    return INTEGRATION_PRINCIPAL_OK;
}

enum integration_principal_status integration_principal_scope_init(
    struct integration_principal_scope *published,
    const struct integration_principal_scope *scope)
{
    enum integration_principal_status status;

    if (published == NULL || scope == NULL) {
        return INTEGRATION_PRINCIPAL_NULL;
    }
    status = integration_principal_scope_validate(scope);
    if (status != INTEGRATION_PRINCIPAL_OK) {
        return status;
    }
    if (published != scope) {
        *published = *scope;
    }
    return INTEGRATION_PRINCIPAL_OK;
}

enum integration_principal_status integration_principal_scope_check(
    const struct integration_principal_scope *scope,
    const struct authorization_matrix *matrix,
    uint32_t command_id)
{
    enum integration_principal_status status;
    enum authorization_status authorization_status;

    if (scope == NULL || matrix == NULL) {
        return INTEGRATION_PRINCIPAL_NULL;
    }
    status = integration_principal_scope_validate(scope);
    if (status != INTEGRATION_PRINCIPAL_OK) {
        return status;
    }

    authorization_status = authorization_check(
        matrix, command_id,
        &scope->role, 1U,
        &scope->capability, 1U);
    if (authorization_status == AUTHORIZATION_OK) {
        return INTEGRATION_PRINCIPAL_OK;
    }
    if (authorization_status == AUTHORIZATION_MISSING_ROLE) {
        return INTEGRATION_PRINCIPAL_MISSING_ROLE;
    }
    if (authorization_status == AUTHORIZATION_MISSING_CAPABILITY) {
        return INTEGRATION_PRINCIPAL_MISSING_CAPABILITY;
    }
    return INTEGRATION_PRINCIPAL_BAD_STATE;
}

const char *integration_principal_status_string(
    enum integration_principal_status status)
{
    switch (status) {
    case INTEGRATION_PRINCIPAL_OK:
        return "INTEGRATION_PRINCIPAL_OK";
    case INTEGRATION_PRINCIPAL_NULL:
        return "INTEGRATION_PRINCIPAL_NULL";
    case INTEGRATION_PRINCIPAL_BAD_VERSION:
        return "INTEGRATION_PRINCIPAL_BAD_VERSION";
    case INTEGRATION_PRINCIPAL_INVALID_SCOPE:
        return "INTEGRATION_PRINCIPAL_INVALID_SCOPE";
    case INTEGRATION_PRINCIPAL_MISSING_ROLE:
        return "INTEGRATION_PRINCIPAL_MISSING_ROLE";
    case INTEGRATION_PRINCIPAL_MISSING_CAPABILITY:
        return "INTEGRATION_PRINCIPAL_MISSING_CAPABILITY";
    case INTEGRATION_PRINCIPAL_BAD_STATE:
        return "INTEGRATION_PRINCIPAL_BAD_STATE";
    default:
        return NULL;
    }
}
