#include "appliance_persistent_storage_namespace.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct appliance_persistent_storage_namespace
    appliance_persistent_storage_namespace_catalogue[] = {
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        "system.config",
        3U,
        {"v1:system.", "v1:network.", "v1:security."},
        {10U, 11U, 12U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "validated system, network and security configuration",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        "service.data",
        1U,
        {"v1:service.", NULL},
        {11U, 0U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "validated service enablement and service state",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        "certificate",
        1U,
        {"v1:certificate.", NULL},
        {15U, 0U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "persistent certificate material",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        "secret",
        1U,
        {"v1:secret.", NULL},
        {10U, 0U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET,
        "persistent secret material and protection metadata",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
        "diagnostic",
        1U,
        {"v1:diagnostic.", NULL},
        {14U, 0U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "bounded runtime diagnostics and health evidence",
    },
    {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
        "recovery.metadata",
        1U,
        {"v1:recovery.", NULL},
        {12U, 0U},
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE,
        "recovery and rollback metadata",
    },
};

static bool appliance_persistent_storage_namespace_bytes_equal(
    const char *left,
    const char *right,
    size_t length)
{
    size_t index;

    for (index = 0U; index < length; ++index) {
        if ((unsigned char)left[index] !=
            (unsigned char)right[index]) {
            return false;
        }
    }
    return true;
}

static bool appliance_persistent_storage_namespace_key_has_prefix(
    const char *key,
    size_t key_length,
    const char *prefix,
    uint32_t prefix_length)
{
    return key_length >= (size_t)prefix_length &&
           prefix != NULL &&
           appliance_persistent_storage_namespace_bytes_equal(
               key,
               prefix,
               (size_t)prefix_length);
}

size_t appliance_persistent_storage_namespace_count(void)
{
    return sizeof(appliance_persistent_storage_namespace_catalogue) /
        sizeof(appliance_persistent_storage_namespace_catalogue[0]);
}

enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_catalogue_at(
    size_t index,
    const struct appliance_persistent_storage_namespace **namespace_out)
{
    if (namespace_out == NULL) {
        return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL;
    }
    if (index >= appliance_persistent_storage_namespace_count()) {
        *namespace_out = NULL;
        return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE;
    }

    *namespace_out = &appliance_persistent_storage_namespace_catalogue[index];
    return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK;
}

enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_describe(
    enum appliance_persistent_storage_namespace_id id,
    const struct appliance_persistent_storage_namespace **namespace_out)
{
    size_t index;

    if (namespace_out == NULL) {
        return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL;
    }
    for (index = 0U;
         index < appliance_persistent_storage_namespace_count();
         ++index) {
        if (appliance_persistent_storage_namespace_catalogue[index].id ==
            id) {
            *namespace_out =
                &appliance_persistent_storage_namespace_catalogue[index];
            return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK;
        }
    }

    *namespace_out = NULL;
    return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID;
}

enum appliance_persistent_storage_namespace_status
appliance_persistent_storage_namespace_classify_record_key(
    const char *key,
    size_t key_length,
    enum appliance_persistent_storage_namespace_id *namespace_out)
{
    size_t namespace_index;
    uint32_t prefix_index;

    if (key == NULL || namespace_out == NULL) {
        return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL;
    }
    if (key_length == 0U) {
        *namespace_out = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
        return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY;
    }

    for (namespace_index = 0U;
         namespace_index < appliance_persistent_storage_namespace_count();
         ++namespace_index) {
        const struct appliance_persistent_storage_namespace *entry =
            &appliance_persistent_storage_namespace_catalogue[
                namespace_index];

        for (prefix_index = 0U;
             prefix_index < entry->record_prefix_count &&
                 prefix_index <
                     APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_RECORD_PREFIX_MAX;
             ++prefix_index) {
            if (appliance_persistent_storage_namespace_key_has_prefix(
                    key,
                    key_length,
                    entry->record_prefixes[prefix_index],
                    entry->record_prefix_lengths[prefix_index])) {
                if (key_length ==
                    (size_t)entry->record_prefix_lengths[prefix_index]) {
                    *namespace_out =
                        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
                    return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY;
                }
                *namespace_out = entry->id;
                return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK;
            }
        }
    }

    *namespace_out = APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE;
    return APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE;
}

const char *appliance_persistent_storage_namespace_id_string(
    enum appliance_persistent_storage_namespace_id id)
{
    switch (id) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_NONE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA";
    default:
        return NULL;
    }
}

const char *appliance_persistent_storage_namespace_sensitivity_string(
    enum appliance_persistent_storage_namespace_sensitivity sensitivity)
{
    switch (sensitivity) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_NONE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_GENERAL:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_GENERAL";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_PRIVATE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_SENSITIVITY_SECRET";
    default:
        return NULL;
    }
}

const char *appliance_persistent_storage_namespace_status_string(
    enum appliance_persistent_storage_namespace_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OK";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_NULL";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_ID";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_OUT_OF_RANGE";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_BAD_KEY";
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE:
        return "APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_UNKNOWN_NAMESPACE";
    default:
        return NULL;
    }
}
