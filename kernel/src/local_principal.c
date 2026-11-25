#include "local_principal.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool local_principal_name_valid(
    const struct local_principal *principal)
{
    size_t index = 0U;

    while (index <= LOCAL_PRINCIPAL_NAME_MAX) {
        unsigned char byte =
            (unsigned char)principal->name[index];

        if (byte == '\0') {
            break;
        }
        if (byte < 0x20U || byte > 0x7EU) {
            return false;
        }
        ++index;
    }
    if (index == 0U || index > LOCAL_PRINCIPAL_NAME_MAX ||
        principal->name[0] == ' ' ||
        principal->name[index - 1U] == ' ') {
        return false;
    }
    while (index <= LOCAL_PRINCIPAL_NAME_MAX) {
        if (principal->name[index] != '\0') {
            return false;
        }
        ++index;
    }
    return true;
}

enum local_principal_status local_principal_validate(
    const struct local_principal *principal)
{
    uint32_t index;
    uint32_t previous;

    if (principal == NULL) {
        return LOCAL_PRINCIPAL_NULL;
    }
    if (principal->id < LOCAL_PRINCIPAL_ID_MIN ||
        principal->id > LOCAL_PRINCIPAL_ID_MAX) {
        return LOCAL_PRINCIPAL_INVALID_ID;
    }
    if (principal->version != LOCAL_PRINCIPAL_DESCRIPTOR_VERSION) {
        return LOCAL_PRINCIPAL_INVALID_VERSION;
    }
    if (!local_principal_name_valid(principal)) {
        return LOCAL_PRINCIPAL_INVALID_NAME;
    }
    if (principal->state != LOCAL_PRINCIPAL_STATE_DISABLED &&
        principal->state != LOCAL_PRINCIPAL_STATE_ENABLED) {
        return LOCAL_PRINCIPAL_INVALID_STATE;
    }
    if (principal->role_count > LOCAL_PRINCIPAL_ROLE_MAX) {
        return LOCAL_PRINCIPAL_INVALID_ROLE_COUNT;
    }
    for (index = 0U; index < principal->role_count; ++index) {
        if (principal->roles[index] < LOCAL_PRINCIPAL_ROLE_ID_MIN) {
            return LOCAL_PRINCIPAL_INVALID_ROLE_ID;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (principal->roles[index] == principal->roles[previous]) {
                return LOCAL_PRINCIPAL_DUPLICATE_ROLE;
            }
        }
    }
    return LOCAL_PRINCIPAL_OK;
}

enum local_principal_status local_principal_init(
    struct local_principal *published,
    const struct local_principal *principal)
{
    enum local_principal_status status;

    if (published == NULL || principal == NULL) {
        return LOCAL_PRINCIPAL_NULL;
    }
    status = local_principal_validate(principal);
    if (status != LOCAL_PRINCIPAL_OK) {
        return status;
    }
    if (published != principal) {
        *published = *principal;
    }
    return LOCAL_PRINCIPAL_OK;
}

const char *local_principal_status_string(
    enum local_principal_status status)
{
    switch (status) {
    case LOCAL_PRINCIPAL_OK:
        return "LOCAL_PRINCIPAL_OK";
    case LOCAL_PRINCIPAL_NULL:
        return "LOCAL_PRINCIPAL_NULL";
    case LOCAL_PRINCIPAL_INVALID_ID:
        return "LOCAL_PRINCIPAL_INVALID_ID";
    case LOCAL_PRINCIPAL_INVALID_VERSION:
        return "LOCAL_PRINCIPAL_INVALID_VERSION";
    case LOCAL_PRINCIPAL_INVALID_NAME:
        return "LOCAL_PRINCIPAL_INVALID_NAME";
    case LOCAL_PRINCIPAL_INVALID_STATE:
        return "LOCAL_PRINCIPAL_INVALID_STATE";
    case LOCAL_PRINCIPAL_INVALID_ROLE_COUNT:
        return "LOCAL_PRINCIPAL_INVALID_ROLE_COUNT";
    case LOCAL_PRINCIPAL_INVALID_ROLE_ID:
        return "LOCAL_PRINCIPAL_INVALID_ROLE_ID";
    case LOCAL_PRINCIPAL_DUPLICATE_ROLE:
        return "LOCAL_PRINCIPAL_DUPLICATE_ROLE";
    default:
        return NULL;
    }
}

enum local_principal_set_status local_principal_set_validate(
    const struct local_principal *principals,
    uint32_t principal_count)
{
    uint32_t index;
    uint32_t previous;

    if (principal_count > LOCAL_PRINCIPAL_SET_MAX) {
        return LOCAL_PRINCIPAL_SET_BAD_CAPACITY;
    }
    if (principal_count > 0U && principals == NULL) {
        return LOCAL_PRINCIPAL_SET_NULL;
    }
    if (principal_count == 0U) {
        return LOCAL_PRINCIPAL_SET_OK;
    }
    for (index = 0U; index < principal_count; ++index) {
        if (local_principal_validate(&principals[index]) !=
            LOCAL_PRINCIPAL_OK) {
            return LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR;
        }
    }
    for (index = 0U; index < principal_count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (principals[index].id == principals[previous].id) {
                return LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL;
            }
        }
    }
    return LOCAL_PRINCIPAL_SET_OK;
}

const char *local_principal_set_status_string(
    enum local_principal_set_status status)
{
    switch (status) {
    case LOCAL_PRINCIPAL_SET_OK:
        return "LOCAL_PRINCIPAL_SET_OK";
    case LOCAL_PRINCIPAL_SET_NULL:
        return "LOCAL_PRINCIPAL_SET_NULL";
    case LOCAL_PRINCIPAL_SET_BAD_CAPACITY:
        return "LOCAL_PRINCIPAL_SET_BAD_CAPACITY";
    case LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR:
        return "LOCAL_PRINCIPAL_SET_INVALID_DESCRIPTOR";
    case LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL:
        return "LOCAL_PRINCIPAL_SET_DUPLICATE_PRINCIPAL";
    default:
        return NULL;
    }
}
