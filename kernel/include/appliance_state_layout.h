#ifndef HFOC_APPLIANCE_STATE_LAYOUT_H
#define HFOC_APPLIANCE_STATE_LAYOUT_H

#include "appliance_persistent_storage_namespace.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_STATE_LAYOUT_VERSION 1U
#define APPLIANCE_STATE_LAYOUT_ENTRY_VERSION 1U
#define APPLIANCE_STATE_LAYOUT_ENTRY_COUNT_MAX 8U
#define APPLIANCE_STATE_LAYOUT_NAMESPACE_COUNT_REQUIRED 6U
/*
 * ITEM-504 increment 6: the eighth store is the durable route table, so the
 * default layout consumes 8 * 64 = 512 blocks.  The seven existing entries
 * keep their offsets: the new store is appended at 448 and nothing moves, so
 * a volume written by an older build still binds its own stores and simply
 * carries no route record.
 */
#define APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS 512U
#define APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT 64U

#define APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME 1U
#define APPLIANCE_STATE_STORE_ID_SYSTEM_NETWORK 2U
#define APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES 3U
#define APPLIANCE_STATE_STORE_ID_SERVICE_ENABLEMENT 1U
#define APPLIANCE_STATE_STORE_ID_CERTIFICATE 1U
#define APPLIANCE_STATE_STORE_ID_SECRET 1U
#define APPLIANCE_STATE_STORE_ID_DIAGNOSTIC 1U
#define APPLIANCE_STATE_STORE_ID_RECOVERY_METADATA 1U

enum appliance_state_layout_status {
    APPLIANCE_STATE_LAYOUT_OK = 0,
    APPLIANCE_STATE_LAYOUT_NULL = 1,
    APPLIANCE_STATE_LAYOUT_UNSUPPORTED_VERSION = 2,
    APPLIANCE_STATE_LAYOUT_BAD_CAPACITY = 3,
    APPLIANCE_STATE_LAYOUT_BAD_ENTRY = 4,
    APPLIANCE_STATE_LAYOUT_MISSING_NAMESPACE = 5,
    APPLIANCE_STATE_LAYOUT_DUPLICATE_STORE = 6,
    APPLIANCE_STATE_LAYOUT_OVERLAP = 7,
    APPLIANCE_STATE_LAYOUT_OUT_OF_RANGE = 8,
    APPLIANCE_STATE_LAYOUT_ZERO_LENGTH = 9,
    APPLIANCE_STATE_LAYOUT_NOT_FOUND = 10,
};

struct appliance_state_layout_entry {
    enum appliance_persistent_storage_namespace_id namespace_id;
    uint32_t store_id;
    uint64_t block_offset;
    uint64_t block_count;
    uint32_t format_version;
};

struct appliance_state_layout {
    uint32_t version;
    uint64_t block_capacity;
    uint32_t entry_count;
    struct appliance_state_layout_entry entries[
        APPLIANCE_STATE_LAYOUT_ENTRY_COUNT_MAX];
};

enum appliance_state_layout_status appliance_state_layout_default(
    struct appliance_state_layout *layout);
enum appliance_state_layout_status appliance_state_layout_validate(
    const struct appliance_state_layout *layout);
enum appliance_state_layout_status appliance_state_layout_find(
    const struct appliance_state_layout *layout,
    enum appliance_persistent_storage_namespace_id namespace_id,
    uint32_t store_id,
    const struct appliance_state_layout_entry **entry_out);
const char *appliance_state_layout_status_string(
    enum appliance_state_layout_status status);

#endif
