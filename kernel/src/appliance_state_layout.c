#include "appliance_state_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static bool appliance_state_layout_namespace_valid(
    enum appliance_persistent_storage_namespace_id id)
{
    return id >= APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG &&
           id <=
               APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA;
}

static bool appliance_state_layout_namespace_required(
    const struct appliance_state_layout *layout,
    enum appliance_persistent_storage_namespace_id id)
{
    uint32_t index;

    for (index = 0U; index < layout->entry_count; ++index) {
        if (layout->entries[index].namespace_id == id) {
            return true;
        }
    }
    return false;
}

static bool appliance_state_layout_required_namespaces_present(
    const struct appliance_state_layout *layout)
{
    static const enum appliance_persistent_storage_namespace_id required[] = {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
    };
    size_t index;

    for (index = 0U;
         index < APPLIANCE_STATE_LAYOUT_NAMESPACE_COUNT_REQUIRED;
         ++index) {
        if (!appliance_state_layout_namespace_required(
                layout, required[index])) {
            return false;
        }
    }
    return true;
}

enum appliance_state_layout_status appliance_state_layout_default(
    struct appliance_state_layout *layout)
{
    struct appliance_state_layout_entry entry;

    if (layout == NULL) {
        return APPLIANCE_STATE_LAYOUT_NULL;
    }
    entry.block_offset = 0U;
    entry.block_count = APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT;
    entry.format_version = APPLIANCE_STATE_LAYOUT_ENTRY_VERSION;
    layout->version = APPLIANCE_STATE_LAYOUT_VERSION;
    layout->block_capacity =
        APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS;
    layout->entry_count = 0U;

#define APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(ns, store) do { \
        entry.namespace_id = ns; \
        entry.store_id = store; \
        layout->entries[layout->entry_count] = entry; \
        ++layout->entry_count; \
        entry.block_offset += entry.block_count; \
    } while (0)

    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        APPLIANCE_STATE_STORE_ID_SYSTEM_NETWORK);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        APPLIANCE_STATE_STORE_ID_SERVICE_ENABLEMENT);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        APPLIANCE_STATE_STORE_ID_CERTIFICATE);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_STATE_STORE_ID_SECRET);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
        APPLIANCE_STATE_STORE_ID_DIAGNOSTIC);
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
        APPLIANCE_STATE_STORE_ID_RECOVERY_METADATA);
    /*
     * ITEM-504 increment 6: the facade's route table lives in the namespace
     * that already owns the machine's own declared parameters, and it is
     * appended last so that every store an already-written volume carries
     * keeps its offset: the new record simply reads as absent there.
     */
    APPLIANCE_STATE_LAYOUT_DEFAULT_ADD(
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES);

#undef APPLIANCE_STATE_LAYOUT_DEFAULT_ADD

    return APPLIANCE_STATE_LAYOUT_OK;
}

enum appliance_state_layout_status appliance_state_layout_validate(
    const struct appliance_state_layout *layout)
{
    uint32_t outer;
    uint32_t inner;

    if (layout == NULL) {
        return APPLIANCE_STATE_LAYOUT_NULL;
    }
    if (layout->version != APPLIANCE_STATE_LAYOUT_VERSION) {
        return APPLIANCE_STATE_LAYOUT_UNSUPPORTED_VERSION;
    }
    if (layout->block_capacity == 0U ||
        layout->block_capacity > UINT64_MAX) {
        return APPLIANCE_STATE_LAYOUT_BAD_CAPACITY;
    }
    if (layout->entry_count == 0U ||
        layout->entry_count > APPLIANCE_STATE_LAYOUT_ENTRY_COUNT_MAX) {
        return APPLIANCE_STATE_LAYOUT_BAD_ENTRY;
    }
    for (outer = 0U; outer < layout->entry_count; ++outer) {
        const struct appliance_state_layout_entry *entry =
            &layout->entries[outer];

        if (!appliance_state_layout_namespace_valid(entry->namespace_id) ||
            entry->store_id == 0U ||
            entry->format_version != APPLIANCE_STATE_LAYOUT_ENTRY_VERSION) {
            return APPLIANCE_STATE_LAYOUT_BAD_ENTRY;
        }
        if (entry->block_count == 0U) {
            return APPLIANCE_STATE_LAYOUT_ZERO_LENGTH;
        }
        if (entry->block_offset >= layout->block_capacity ||
            entry->block_count >
                layout->block_capacity - entry->block_offset) {
            return APPLIANCE_STATE_LAYOUT_OUT_OF_RANGE;
        }
        for (inner = 0U; inner < outer; ++inner) {
            const struct appliance_state_layout_entry *previous =
                &layout->entries[inner];

            if (previous->namespace_id == entry->namespace_id &&
                previous->store_id == entry->store_id) {
                return APPLIANCE_STATE_LAYOUT_DUPLICATE_STORE;
            }
            if (entry->block_offset <=
                    previous->block_offset + previous->block_count - 1U &&
                entry->block_offset + entry->block_count - 1U >=
                    previous->block_offset) {
                return APPLIANCE_STATE_LAYOUT_OVERLAP;
            }
        }
    }
    if (!appliance_state_layout_required_namespaces_present(layout)) {
        return APPLIANCE_STATE_LAYOUT_MISSING_NAMESPACE;
    }
    return APPLIANCE_STATE_LAYOUT_OK;
}

enum appliance_state_layout_status appliance_state_layout_find(
    const struct appliance_state_layout *layout,
    enum appliance_persistent_storage_namespace_id namespace_id,
    uint32_t store_id,
    const struct appliance_state_layout_entry **entry_out)
{
    uint32_t index;

    if (layout == NULL || entry_out == NULL) {
        return APPLIANCE_STATE_LAYOUT_NULL;
    }
    if (appliance_state_layout_validate(layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        return APPLIANCE_STATE_LAYOUT_BAD_ENTRY;
    }
    *entry_out = NULL;
    for (index = 0U; index < layout->entry_count; ++index) {
        if (layout->entries[index].namespace_id == namespace_id &&
            layout->entries[index].store_id == store_id) {
            *entry_out = &layout->entries[index];
            return APPLIANCE_STATE_LAYOUT_OK;
        }
    }
    return APPLIANCE_STATE_LAYOUT_NOT_FOUND;
}

const char *appliance_state_layout_status_string(
    enum appliance_state_layout_status status)
{
    switch (status) {
    case APPLIANCE_STATE_LAYOUT_OK:
        return "APPLIANCE_STATE_LAYOUT_OK";
    case APPLIANCE_STATE_LAYOUT_NULL:
        return "APPLIANCE_STATE_LAYOUT_NULL";
    case APPLIANCE_STATE_LAYOUT_UNSUPPORTED_VERSION:
        return "APPLIANCE_STATE_LAYOUT_UNSUPPORTED_VERSION";
    case APPLIANCE_STATE_LAYOUT_BAD_CAPACITY:
        return "APPLIANCE_STATE_LAYOUT_BAD_CAPACITY";
    case APPLIANCE_STATE_LAYOUT_BAD_ENTRY:
        return "APPLIANCE_STATE_LAYOUT_BAD_ENTRY";
    case APPLIANCE_STATE_LAYOUT_MISSING_NAMESPACE:
        return "APPLIANCE_STATE_LAYOUT_MISSING_NAMESPACE";
    case APPLIANCE_STATE_LAYOUT_DUPLICATE_STORE:
        return "APPLIANCE_STATE_LAYOUT_DUPLICATE_STORE";
    case APPLIANCE_STATE_LAYOUT_OVERLAP:
        return "APPLIANCE_STATE_LAYOUT_OVERLAP";
    case APPLIANCE_STATE_LAYOUT_OUT_OF_RANGE:
        return "APPLIANCE_STATE_LAYOUT_OUT_OF_RANGE";
    case APPLIANCE_STATE_LAYOUT_ZERO_LENGTH:
        return "APPLIANCE_STATE_LAYOUT_ZERO_LENGTH";
    case APPLIANCE_STATE_LAYOUT_NOT_FOUND:
        return "APPLIANCE_STATE_LAYOUT_NOT_FOUND";
    default:
        return NULL;
    }
}
