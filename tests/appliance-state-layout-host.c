#include "appliance_state_layout.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_APPLIANCE_STATE_LAYOUT_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static void test_default_and_validation(void)
{
    struct appliance_state_layout layout;
    const struct appliance_state_layout_entry *entry = NULL;
    struct appliance_state_layout broken;

    CHECK(appliance_state_layout_default(NULL) ==
              APPLIANCE_STATE_LAYOUT_NULL,
          "default layout rejects null");
    CHECK(appliance_state_layout_validate(NULL) ==
              APPLIANCE_STATE_LAYOUT_NULL,
          "validate rejects null layout");
    CHECK(appliance_state_layout_find(NULL,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME, &entry) ==
              APPLIANCE_STATE_LAYOUT_NULL,
          "find rejects null layout");

    memset(&layout, 0, sizeof(layout));
    CHECK(appliance_state_layout_default(&layout) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              layout.version == APPLIANCE_STATE_LAYOUT_VERSION &&
              layout.entry_count == 8U &&
              appliance_state_layout_validate(&layout) ==
                  APPLIANCE_STATE_LAYOUT_OK,
          "canonical six-namespace layout validates");

    CHECK(appliance_state_layout_find(&layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              APPLIANCE_STATE_STORE_ID_SYSTEM_ROUTES, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL &&
              entry->block_offset == 448U &&
              entry->block_count ==
                  APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT,
          "ITEM-504: the route store is appended after the seven others");

    CHECK(appliance_state_layout_find(&layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL &&
              entry->block_offset == 0U &&
              entry->block_count ==
                  APPLIANCE_STATE_LAYOUT_STORE_BLOCKS_DEFAULT,
          "find returns canonical hostname store");
    CHECK(appliance_state_layout_find(&layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
              APPLIANCE_STATE_STORE_ID_RECOVERY_METADATA, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL &&
              entry->block_offset == 384U,
          "find returns recovery metadata store");

    broken = layout;
    broken.version = APPLIANCE_STATE_LAYOUT_VERSION + 1U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_UNSUPPORTED_VERSION,
          "unsupported layout version is rejected");

    broken = layout;
    broken.block_capacity = 0U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_BAD_CAPACITY,
          "zero layout capacity is rejected");

    broken = layout;
    broken.entry_count = 0U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_BAD_ENTRY,
          "empty store map is rejected");

    broken = layout;
    /*
     * ITEM-504 increment 6: the last entry is the route store, which lives in
     * the namespace the hostname already covers, so dropping it would still
     * leave every required namespace present.  The recovery store is the one
     * re-pointed instead, and its store id moves with it so the map is
     * rejected for the missing namespace and not for a duplicate store.
     */
    broken.entry_count -= 1U;
    broken.entries[broken.entry_count - 1U].namespace_id =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG;
    broken.entries[broken.entry_count - 1U].store_id = 9U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_MISSING_NAMESPACE,
          "missing namespace is rejected");

    broken = layout;
    broken.entries[1].namespace_id =
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG;
    broken.entries[1].store_id = APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_DUPLICATE_STORE,
          "duplicate namespace store is rejected");

    broken = layout;
    broken.entries[1].block_offset = 0U;
    broken.entries[1].block_count = 1U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_OVERLAP,
          "overlapping stores are rejected");

    broken = layout;
    broken.entries[1].block_count = 0U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_ZERO_LENGTH,
          "zero-length store is rejected");

    broken = layout;
    broken.entries[1].block_offset = broken.block_capacity;
    broken.entries[1].block_count = 1U;
    CHECK(appliance_state_layout_validate(&broken) ==
              APPLIANCE_STATE_LAYOUT_OUT_OF_RANGE,
          "out-of-range store is rejected");

    CHECK(appliance_state_layout_find(&layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              APPLIANCE_STATE_STORE_ID_SYSTEM_NETWORK, &entry) ==
              APPLIANCE_STATE_LAYOUT_OK &&
              entry != NULL,
          "find returns network store");
    entry = NULL;
    CHECK(appliance_state_layout_find(&layout,
              APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
              UINT32_C(0x7fffffff), &entry) ==
              APPLIANCE_STATE_LAYOUT_NOT_FOUND &&
              entry == NULL,
          "find rejects unknown store");

    CHECK(appliance_state_layout_status_string(
              APPLIANCE_STATE_LAYOUT_OK) != NULL &&
              strcmp(appliance_state_layout_status_string(
                         APPLIANCE_STATE_LAYOUT_OK),
                     "APPLIANCE_STATE_LAYOUT_OK") == 0,
          "layout status string is exact");
    CHECK(appliance_state_layout_status_string(
              (enum appliance_state_layout_status)99U) == NULL,
          "layout rejects undefined status string");
}

int main(void)
{
    test_default_and_validation();

    if (failures != 0) {
        fprintf(stderr,
                "Appliance state layout host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    puts("Appliance state layout host tests: ALL OK");
    return 0;
}

#endif
