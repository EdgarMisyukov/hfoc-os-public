#ifndef HFOC_APPLIANCE_STATE_LOADER_H
#define HFOC_APPLIANCE_STATE_LOADER_H

#include "appliance_key_custody.h"
#include "appliance_persistent_config.h"
#include "appliance_persistent_network_config.h"
#include "appliance_persistent_secret_config.h"
#include "appliance_persistent_service_config.h"
#include "appliance_state_block_device.h"
#include "appliance_state_encryption.h"
#include "appliance_state_layout.h"
#include "block_device.h"

#include <stdbool.h>
#include <stdint.h>

#define APPLIANCE_STATE_LOADER_VERSION 1U
#define APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT 4U

enum appliance_state_loader_status {
    APPLIANCE_STATE_LOADER_OK = 0,
    APPLIANCE_STATE_LOADER_NULL = 1,
    APPLIANCE_STATE_LOADER_BAD_STATE = 2,
    APPLIANCE_STATE_LOADER_BAD_DEVICE = 3,
    APPLIANCE_STATE_LOADER_BAD_LAYOUT = 4,
    APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR = 5,
    APPLIANCE_STATE_LOADER_PARTITION_AMBIGUOUS = 6,
    APPLIANCE_STATE_LOADER_PARTITION_NOT_FOUND = 7,
    APPLIANCE_STATE_LOADER_PARTITION_BIND_FAILED = 8,
    APPLIANCE_STATE_LOADER_STORE_BIND_FAILED = 9,
    APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED = 10,
    APPLIANCE_STATE_LOADER_STORE_CORRUPT = 11,
    APPLIANCE_STATE_LOADER_KEY_REQUIRED = 12,
    APPLIANCE_STATE_LOADER_KEY_REJECTED = 13,
    APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE = 14,
    APPLIANCE_STATE_LOADER_ENCRYPTION_CORRUPT = 15,
};

struct appliance_state_loader_store_state {
    enum appliance_persistent_storage_namespace_id namespace_id;
    uint32_t store_id;
    bool loaded;
    enum appliance_state_loader_status status;
};

struct appliance_state_loader {
    bool initialized;
    bool ready;
    enum appliance_state_loader_status status;
    struct appliance_state_layout layout;
    struct appliance_state_block_device partition;
    struct appliance_state_block_device stores[
        APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT];
    struct appliance_state_loader_store_state store_states[
        APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT];
    struct appliance_persistent_config system_config;
    struct appliance_persistent_network_config network_config;
    struct appliance_persistent_secret_config secret_config;
    struct appliance_persistent_service_config service_config;
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    bool encryption_sealed;
    struct appliance_state_encryption_volume volume;
    struct appliance_state_encryption_store encrypted_stores[
        APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT];
#endif
};

enum appliance_state_loader_status appliance_state_loader_init(
    struct appliance_state_loader *loader);
enum appliance_state_loader_status appliance_state_loader_load(
    struct appliance_state_loader *loader,
    struct block_device *partition_device,
    const struct appliance_state_layout *layout);
enum appliance_state_loader_status appliance_state_loader_load_with_key(
    struct appliance_state_loader *loader,
    struct block_device *partition_device,
    const struct appliance_state_layout *layout,
    const struct appliance_key_custody_material *key_material);
bool appliance_state_loader_ready(
    const struct appliance_state_loader *loader);
const struct appliance_state_layout *
appliance_state_loader_layout(
    const struct appliance_state_loader *loader);
const struct block_device *appliance_state_loader_partition_device(
    const struct appliance_state_loader *loader);
const struct appliance_persistent_config *
appliance_state_loader_system_config(
    const struct appliance_state_loader *loader);
const struct appliance_persistent_network_config *
appliance_state_loader_network_config(
    const struct appliance_state_loader *loader);
const struct appliance_persistent_secret_config *
appliance_state_loader_secret_config(
    const struct appliance_state_loader *loader);
const struct appliance_persistent_service_config *
appliance_state_loader_service_config(
    const struct appliance_state_loader *loader);
const char *appliance_state_loader_status_string(
    enum appliance_state_loader_status status);

#endif
