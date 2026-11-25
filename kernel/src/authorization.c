#include "authorization.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum authorization_status authorization_entry_valid(
    const struct authorization_entry *entry)
{
    uint32_t index;
    uint32_t previous;

    if (entry->version != AUTHORIZATION_VERSION) {
        return AUTHORIZATION_BAD_VERSION;
    }
    if (entry->command_id < AUTHORIZATION_COMMAND_ID_MIN ||
        entry->command_id > AUTHORIZATION_COMMAND_ID_MAX) {
        return AUTHORIZATION_INVALID_DESCRIPTOR;
    }
    if (entry->role_count > AUTHORIZATION_ROLE_SET_MAX ||
        entry->capability_count > AUTHORIZATION_CAPABILITY_SET_MAX ||
        (entry->role_count == 0U && entry->capability_count == 0U)) {
        return AUTHORIZATION_INVALID_DESCRIPTOR;
    }

    for (index = 0U; index < entry->role_count; ++index) {
        if (entry->roles[index] < AUTHORIZATION_ROLE_ID_MIN) {
            return AUTHORIZATION_INVALID_DESCRIPTOR;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (entry->roles[index] == entry->roles[previous]) {
                return AUTHORIZATION_INVALID_DESCRIPTOR;
            }
        }
    }
    for (index = 0U; index < entry->capability_count; ++index) {
        if (entry->capabilities[index] <
            AUTHORIZATION_CAPABILITY_ID_MIN) {
            return AUTHORIZATION_INVALID_DESCRIPTOR;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (entry->capabilities[index] ==
                entry->capabilities[previous]) {
                return AUTHORIZATION_INVALID_DESCRIPTOR;
            }
        }
    }
    return AUTHORIZATION_OK;
}

static enum authorization_status authorization_matrix_valid(
    const struct authorization_matrix *matrix)
{
    enum authorization_status status;
    size_t index;

    if (matrix == NULL) {
        return AUTHORIZATION_NULL;
    }
    if (matrix->version != AUTHORIZATION_VERSION) {
        return AUTHORIZATION_BAD_VERSION;
    }
    if (matrix->count == 0U ||
        matrix->count > AUTHORIZATION_MATRIX_CAPACITY_MAX ||
        !matrix->initialized) {
        return AUTHORIZATION_BAD_STATE;
    }
    for (index = 0U; index < matrix->count; ++index) {
        status = authorization_entry_valid(&matrix->entries[index]);
        if (status != AUTHORIZATION_OK) {
            return AUTHORIZATION_BAD_STATE;
        }
    }
    return AUTHORIZATION_OK;
}

static enum authorization_status authorization_caller_set_valid(
    const uint8_t *items,
    size_t count,
    size_t maximum,
    uint8_t minimum,
    enum authorization_status invalid_status)
{
    size_t index;
    size_t previous;

    if (count == 0U) {
        return AUTHORIZATION_OK;
    }
    if (items == NULL) {
        return AUTHORIZATION_NULL;
    }
    if (count > maximum) {
        return invalid_status;
    }
    for (index = 0U; index < count; ++index) {
        if (items[index] < minimum) {
            return invalid_status;
        }
        for (previous = 0U; previous < index; ++previous) {
            if (items[index] == items[previous]) {
                return invalid_status;
            }
        }
    }
    return AUTHORIZATION_OK;
}

enum authorization_status authorization_matrix_init(
    struct authorization_matrix *matrix,
    const struct authorization_entry *entries,
    size_t count)
{
    enum authorization_status status;
    size_t index;
    size_t previous;

    if (matrix == NULL ||
        (count > 0U && entries == NULL)) {
        return AUTHORIZATION_NULL;
    }
    if (count == 0U ||
        count > AUTHORIZATION_MATRIX_CAPACITY_MAX) {
        return AUTHORIZATION_BAD_CAPACITY;
    }

    for (index = 0U; index < count; ++index) {
        status = authorization_entry_valid(&entries[index]);
        if (status != AUTHORIZATION_OK) {
            return status;
        }
    }
    for (index = 0U; index < count; ++index) {
        for (previous = 0U; previous < index; ++previous) {
            if (entries[index].command_id ==
                entries[previous].command_id) {
                return AUTHORIZATION_DUPLICATE_COMMAND;
            }
        }
    }

    memset(matrix, 0, sizeof(*matrix));
    for (index = 0U; index < count; ++index) {
        matrix->entries[index] = entries[index];
    }
    matrix->version = AUTHORIZATION_VERSION;
    matrix->count = count;
    matrix->initialized = true;
    return AUTHORIZATION_OK;
}

enum authorization_status authorization_check(
    const struct authorization_matrix *matrix,
    uint32_t command_id,
    const uint8_t *roles,
    size_t role_count,
    const uint8_t *capabilities,
    size_t capability_count)
{
    const struct authorization_entry *entry = NULL;
    enum authorization_status status;
    size_t index;
    size_t required_index;
    size_t caller_index;
    bool found;

    if (matrix == NULL ||
        (role_count > 0U && roles == NULL) ||
        (capability_count > 0U && capabilities == NULL)) {
        return AUTHORIZATION_NULL;
    }
    status = authorization_matrix_valid(matrix);
    if (status != AUTHORIZATION_OK) {
        return status;
    }
    if (command_id < AUTHORIZATION_COMMAND_ID_MIN ||
        command_id > AUTHORIZATION_COMMAND_ID_MAX) {
        return AUTHORIZATION_UNKNOWN_COMMAND;
    }

    for (index = 0U; index < matrix->count; ++index) {
        if (matrix->entries[index].command_id == command_id) {
            entry = &matrix->entries[index];
            break;
        }
    }
    if (entry == NULL) {
        return AUTHORIZATION_UNKNOWN_COMMAND;
    }

    status = authorization_caller_set_valid(
        roles, role_count, AUTHORIZATION_ROLE_SET_MAX,
        AUTHORIZATION_ROLE_ID_MIN,
        AUTHORIZATION_INVALID_ROLE_SET);
    if (status != AUTHORIZATION_OK) {
        return status;
    }
    status = authorization_caller_set_valid(
        capabilities, capability_count, AUTHORIZATION_CAPABILITY_SET_MAX,
        AUTHORIZATION_CAPABILITY_ID_MIN,
        AUTHORIZATION_INVALID_CAPABILITY_SET);
    if (status != AUTHORIZATION_OK) {
        return status;
    }

    for (required_index = 0U;
         required_index < entry->role_count; ++required_index) {
        found = false;
        for (caller_index = 0U; caller_index < role_count;
             ++caller_index) {
            if (roles[caller_index] ==
                entry->roles[required_index]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return AUTHORIZATION_MISSING_ROLE;
        }
    }
    for (required_index = 0U;
         required_index < entry->capability_count; ++required_index) {
        found = false;
        for (caller_index = 0U;
             caller_index < capability_count; ++caller_index) {
            if (capabilities[caller_index] ==
                entry->capabilities[required_index]) {
                found = true;
                break;
            }
        }
        if (!found) {
            return AUTHORIZATION_MISSING_CAPABILITY;
        }
    }
    return AUTHORIZATION_OK;
}

const char *authorization_status_string(
    enum authorization_status status)
{
    switch (status) {
    case AUTHORIZATION_OK:
        return "AUTHORIZATION_OK";
    case AUTHORIZATION_NULL:
        return "AUTHORIZATION_NULL";
    case AUTHORIZATION_BAD_CAPACITY:
        return "AUTHORIZATION_BAD_CAPACITY";
    case AUTHORIZATION_BAD_VERSION:
        return "AUTHORIZATION_BAD_VERSION";
    case AUTHORIZATION_UNKNOWN_COMMAND:
        return "AUTHORIZATION_UNKNOWN_COMMAND";
    case AUTHORIZATION_INVALID_DESCRIPTOR:
        return "AUTHORIZATION_INVALID_DESCRIPTOR";
    case AUTHORIZATION_DUPLICATE_COMMAND:
        return "AUTHORIZATION_DUPLICATE_COMMAND";
    case AUTHORIZATION_INVALID_ROLE_SET:
        return "AUTHORIZATION_INVALID_ROLE_SET";
    case AUTHORIZATION_INVALID_CAPABILITY_SET:
        return "AUTHORIZATION_INVALID_CAPABILITY_SET";
    case AUTHORIZATION_MISSING_ROLE:
        return "AUTHORIZATION_MISSING_ROLE";
    case AUTHORIZATION_MISSING_CAPABILITY:
        return "AUTHORIZATION_MISSING_CAPABILITY";
    case AUTHORIZATION_BAD_STATE:
        return "AUTHORIZATION_BAD_STATE";
    default:
        return NULL;
    }
}
