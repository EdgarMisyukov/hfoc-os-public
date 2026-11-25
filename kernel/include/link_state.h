#ifndef HFOC_LINK_STATE_H
#define HFOC_LINK_STATE_H

#include "network_device.h"

#include <stdbool.h>
#include <stdint.h>

#define NETWORK_LINK_STATE_VERSION 1U
#define NETWORK_LINK_SELECTION_VERSION 1U
#define NETWORK_LINK_GENERATION_MIN 1U
#define NETWORK_LINK_GENERATION_MAX UINT32_C(0xFFFFFFFE)

enum network_link_state {
    NETWORK_LINK_DOWN = 0,
    NETWORK_LINK_INITIALIZING = 1,
    NETWORK_LINK_UP = 2,
    NETWORK_LINK_FAILED = 3,
    NETWORK_LINK_STATE_COUNT = 4,
};

enum network_link_status {
    NETWORK_LINK_OK = 0,
    NETWORK_LINK_NULL = 1,
    NETWORK_LINK_INVALID_VERSION = 2,
    NETWORK_LINK_INVALID_DEVICE_ID = 3,
    NETWORK_LINK_INVALID_STATE = 4,
    NETWORK_LINK_INVALID_GENERATION = 5,
    NETWORK_LINK_ALREADY_PUBLISHED = 6,
    NETWORK_LINK_UNPUBLISHED = 7,
    NETWORK_LINK_INVALID_DEVICE_SNAPSHOT = 8,
    NETWORK_LINK_DEVICE_MISMATCH = 9,
    NETWORK_LINK_GENERATION_MISMATCH = 10,
    NETWORK_LINK_UNSUPPORTED = 11,
    NETWORK_LINK_NOT_UP = 12,
};

struct network_link_state_record {
    uint32_t version;
    uint32_t device_id;
    enum network_link_state state;
    uint32_t generation;
};

struct network_link_selection {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct network_link_state_record link;
};

enum network_link_status network_link_state_validate(
    const struct network_link_state_record *state);
enum network_link_status network_link_selection_init(
    struct network_link_selection *selection,
    const struct network_link_state_record *state,
    uint32_t generation);
bool network_link_selection_published(
    const struct network_link_selection *selection);
enum network_link_status network_link_path_status(
    const struct network_link_selection *selection,
    const struct network_device_snapshot *device_snapshot,
    uint32_t observed_device_generation,
    uint32_t observed_link_generation);
bool network_link_path_enabled(
    const struct network_link_selection *selection,
    const struct network_device_snapshot *device_snapshot,
    uint32_t observed_device_generation,
    uint32_t observed_link_generation);
const char *network_link_status_string(
    enum network_link_status status);

#endif
