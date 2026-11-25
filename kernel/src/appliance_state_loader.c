#include "appliance_state_loader.h"

#include "appliance_gpt_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum appliance_state_loader_status
appliance_state_loader_map_gpt(
    enum appliance_gpt_discovery_status status)
{
    switch (status) {
    case APPLIANCE_GPT_DISCOVERY_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_GPT_DISCOVERY_NULL:
    case APPLIANCE_GPT_DISCOVERY_BAD_DEVICE:
    case APPLIANCE_GPT_DISCOVERY_UNSUPPORTED_BLOCK_SIZE:
    case APPLIANCE_GPT_DISCOVERY_BAD_SIGNATURE:
    case APPLIANCE_GPT_DISCOVERY_BAD_HEADER:
    case APPLIANCE_GPT_DISCOVERY_BAD_ENTRY:
    case APPLIANCE_GPT_DISCOVERY_OVERLAP:
    case APPLIANCE_GPT_DISCOVERY_OUT_OF_RANGE:
    case APPLIANCE_GPT_DISCOVERY_CAPACITY:
        return APPLIANCE_STATE_LOADER_BAD_DEVICE;
    case APPLIANCE_GPT_DISCOVERY_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    case APPLIANCE_GPT_DISCOVERY_AMBIGUOUS:
        return APPLIANCE_STATE_LOADER_PARTITION_AMBIGUOUS;
    case APPLIANCE_GPT_DISCOVERY_NOT_FOUND:
        return APPLIANCE_STATE_LOADER_PARTITION_NOT_FOUND;
    default:
        return APPLIANCE_STATE_LOADER_BAD_DEVICE;
    }
}

static enum appliance_state_loader_status
appliance_state_loader_map_persistent_config(
    enum appliance_persistent_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_CONFIG_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_PERSISTENT_CONFIG_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    case APPLIANCE_PERSISTENT_CONFIG_CORRUPT:
        return APPLIANCE_STATE_LOADER_STORE_CORRUPT;
    case APPLIANCE_PERSISTENT_CONFIG_NULL:
    case APPLIANCE_PERSISTENT_CONFIG_BAD_STATE:
    case APPLIANCE_PERSISTENT_CONFIG_BAD_DEVICE:
    case APPLIANCE_PERSISTENT_CONFIG_FULL:
    case APPLIANCE_PERSISTENT_CONFIG_BAD_RECORD:
    case APPLIANCE_PERSISTENT_CONFIG_BAD_HOSTNAME:
    case APPLIANCE_PERSISTENT_CONFIG_BUFFER_TOO_SMALL:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    default:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    }
}

static enum appliance_state_loader_status
appliance_state_loader_map_persistent_network(
    enum appliance_persistent_network_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_CORRUPT:
        return APPLIANCE_STATE_LOADER_STORE_CORRUPT;
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_NULL:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_STATE:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_DEVICE:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_FULL:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_RECORD:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BAD_CONFIG:
    case APPLIANCE_PERSISTENT_NETWORK_CONFIG_BUFFER_TOO_SMALL:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    default:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    }
}

static enum appliance_state_loader_status
appliance_state_loader_map_persistent_secret(
    enum appliance_persistent_secret_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_CORRUPT:
        return APPLIANCE_STATE_LOADER_STORE_CORRUPT;
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_NULL:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_STATE:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_DEVICE:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_FULL:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_RECORD:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_BAD_VALUE:
    case APPLIANCE_PERSISTENT_SECRET_CONFIG_UNSUPPORTED:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    default:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    }
}

static enum appliance_state_loader_status
appliance_state_loader_map_persistent_service(
    enum appliance_persistent_service_config_status status)
{
    switch (status) {
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_CORRUPT:
        return APPLIANCE_STATE_LOADER_STORE_CORRUPT;
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_NULL:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_STATE:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_DEVICE:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_FULL:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_RECORD:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_BAD_CONFIG:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_UNKNOWN_SERVICE:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_ENTRY_UNAVAILABLE:
    case APPLIANCE_PERSISTENT_SERVICE_CONFIG_MANAGER_BAD_STATE:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    default:
        return APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED;
    }
}

enum appliance_state_loader_status appliance_state_loader_init(
    struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return APPLIANCE_STATE_LOADER_NULL;
    }
    if (loader->initialized && loader->ready) {
        return APPLIANCE_STATE_LOADER_BAD_STATE;
    }
    memset(loader, 0, sizeof(*loader));
    loader->initialized = true;
    loader->status = APPLIANCE_STATE_LOADER_OK;
    return APPLIANCE_STATE_LOADER_OK;
}

#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
static enum appliance_state_loader_status
appliance_state_loader_map_encryption(
    enum appliance_state_encryption_status status)
{
    switch (status) {
    case APPLIANCE_STATE_ENCRYPTION_OK:
        return APPLIANCE_STATE_LOADER_OK;
    case APPLIANCE_STATE_ENCRYPTION_KEY_REQUIRED:
        return APPLIANCE_STATE_LOADER_KEY_REQUIRED;
    case APPLIANCE_STATE_ENCRYPTION_WRONG_KEY:
        return APPLIANCE_STATE_LOADER_KEY_REJECTED;
    case APPLIANCE_STATE_ENCRYPTION_TPM_UNAVAILABLE:
        return APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE;
    case APPLIANCE_STATE_ENCRYPTION_IO_ERROR:
        return APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR;
    default:
        return APPLIANCE_STATE_LOADER_ENCRYPTION_CORRUPT;
    }
}
#endif

static struct block_device *appliance_state_loader_store_view(
    struct appliance_state_loader *loader,
    uint32_t index)
{
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    if (loader->encryption_sealed) {
        return &loader->encrypted_stores[index].device;
    }
#endif
    return &loader->stores[index].device;
}

enum appliance_state_loader_status appliance_state_loader_load_with_key(
    struct appliance_state_loader *loader,
    struct block_device *partition_device,
    const struct appliance_state_layout *layout_in,
    const struct appliance_key_custody_material *key_material)
{
    enum appliance_state_layout_status layout_status;
    enum block_device_status device_status;
    enum appliance_gpt_discovery_status gpt_status;
    struct appliance_gpt_hfoc_partition partition;
    enum appliance_state_block_device_status bind_status;
    enum appliance_state_loader_status owner_status;
    const struct appliance_state_layout_entry *entry = NULL;
    uint32_t store_index;
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    enum appliance_state_encryption_status encryption_status;
#else
    if (key_material != NULL) {
        return APPLIANCE_STATE_LOADER_BAD_STATE;
    }
#endif

    if (loader == NULL || partition_device == NULL ||
        layout_in == NULL) {
        return APPLIANCE_STATE_LOADER_NULL;
    }
    if (!loader->initialized || loader->ready) {
        return APPLIANCE_STATE_LOADER_BAD_STATE;
    }
    layout_status = appliance_state_layout_validate(layout_in);
    if (layout_status != APPLIANCE_STATE_LAYOUT_OK) {
        loader->status = APPLIANCE_STATE_LOADER_BAD_LAYOUT;
        return loader->status;
    }
    device_status = block_device_validate(partition_device);
    if (device_status != BLOCK_DEVICE_OK ||
        partition_device->block_size != APPLIANCE_STATE_BLOCK_SIZE) {
        loader->status = APPLIANCE_STATE_LOADER_BAD_DEVICE;
        return loader->status;
    }

    loader->layout = *layout_in;
    memset(&partition, 0, sizeof(partition));
    gpt_status = appliance_gpt_find_hfoc_partition(
        partition_device, &partition);
    loader->status = appliance_state_loader_map_gpt(gpt_status);
    if (loader->status != APPLIANCE_STATE_LOADER_OK) {
        return loader->status;
    }
    if (appliance_gpt_validate_hfoc_partition(&partition) !=
        APPLIANCE_GPT_DISCOVERY_OK) {
        loader->status = APPLIANCE_STATE_LOADER_BAD_DEVICE;
        return loader->status;
    }

    bind_status = appliance_state_block_device_bind_range(
        partition_device, partition.first_lba, partition.block_count,
        &loader->partition);
    if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
        loader->status = APPLIANCE_STATE_LOADER_PARTITION_BIND_FAILED;
        return loader->status;
    }

#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    loader->encryption_sealed = false;
    memset(&loader->volume, 0, sizeof(loader->volume));
    encryption_status = appliance_state_encryption_volume_probe(
        &loader->volume, &loader->partition.device);
    if (encryption_status == APPLIANCE_STATE_ENCRYPTION_OK) {
        encryption_status = appliance_state_encryption_volume_unlock(
            &loader->volume, key_material);
        if (encryption_status != APPLIANCE_STATE_ENCRYPTION_OK) {
            loader->status =
                appliance_state_loader_map_encryption(encryption_status);
            return loader->status;
        }
        loader->encryption_sealed = true;
    } else if (encryption_status !=
               APPLIANCE_STATE_ENCRYPTION_NOT_ENCRYPTED) {
        loader->status =
            appliance_state_loader_map_encryption(encryption_status);
        return loader->status;
    }
#endif

    for (store_index = 0U;
         store_index < APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT;
         ++store_index) {
        loader->store_states[store_index].loaded = false;
    }

    static const struct loader_store_key {
        enum appliance_persistent_storage_namespace_id namespace_id;
        uint32_t store_id;
    } keys[APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT] = {
        {
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
            APPLIANCE_STATE_STORE_ID_SYSTEM_HOSTNAME
        },
        {
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
            APPLIANCE_STATE_STORE_ID_SYSTEM_NETWORK
        },
        {
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
            APPLIANCE_STATE_STORE_ID_SERVICE_ENABLEMENT
        },
        {
            APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
            APPLIANCE_STATE_STORE_ID_SECRET
        },
    };

    for (store_index = 0U;
         store_index < APPLIANCE_STATE_LOADER_CURRENT_STORE_COUNT;
         ++store_index) {
        entry = NULL;
        if (appliance_state_layout_find(
                &loader->layout,
                keys[store_index].namespace_id,
                keys[store_index].store_id,
                &entry) != APPLIANCE_STATE_LAYOUT_OK) {
            loader->status = APPLIANCE_STATE_LOADER_BAD_LAYOUT;
            return loader->status;
        }
        bind_status = appliance_state_block_device_bind(
            &loader->partition.device, entry,
            &loader->stores[store_index]);
        if (bind_status != APPLIANCE_STATE_BLOCK_DEVICE_OK) {
            loader->status = APPLIANCE_STATE_LOADER_STORE_BIND_FAILED;
            return loader->status;
        }
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
        if (loader->encryption_sealed &&
            appliance_state_encryption_store_bind(
                &loader->volume, entry->namespace_id, entry->store_id,
                &loader->stores[store_index].device,
                &loader->encrypted_stores[store_index]) !=
                APPLIANCE_STATE_ENCRYPTION_OK) {
            loader->status = APPLIANCE_STATE_LOADER_STORE_BIND_FAILED;
            return loader->status;
        }
#endif
        loader->store_states[store_index].namespace_id =
            entry->namespace_id;
        loader->store_states[store_index].store_id = entry->store_id;
    }

    owner_status = appliance_state_loader_map_persistent_config(
        appliance_persistent_config_load(
            &loader->system_config,
            appliance_state_loader_store_view(loader, 0U)));
    loader->status = owner_status;
    if (owner_status == APPLIANCE_STATE_LOADER_OK) {
        loader->store_states[0].loaded = true;
    } else {
        return loader->status;
    }

    owner_status = appliance_state_loader_map_persistent_network(
        appliance_persistent_network_config_load(
            &loader->network_config,
            appliance_state_loader_store_view(loader, 1U)));
    loader->status = owner_status;
    if (owner_status == APPLIANCE_STATE_LOADER_OK) {
        loader->store_states[1].loaded = true;
    } else {
        return loader->status;
    }

    owner_status = appliance_state_loader_map_persistent_service(
        appliance_persistent_service_config_load(
            &loader->service_config,
            appliance_state_loader_store_view(loader, 2U), NULL, 0U));
    loader->status = owner_status;
    if (owner_status == APPLIANCE_STATE_LOADER_OK) {
        loader->store_states[2].loaded = true;
    } else {
        return loader->status;
    }

    owner_status = appliance_state_loader_map_persistent_secret(
        appliance_persistent_secret_config_load(
            &loader->secret_config,
            appliance_state_loader_store_view(loader, 3U)));
    loader->status = owner_status;
    if (owner_status == APPLIANCE_STATE_LOADER_OK) {
        loader->store_states[3].loaded = true;
    } else {
        return loader->status;
    }

    loader->ready = true;
    loader->status = APPLIANCE_STATE_LOADER_OK;
    return APPLIANCE_STATE_LOADER_OK;
}

enum appliance_state_loader_status appliance_state_loader_load(
    struct appliance_state_loader *loader,
    struct block_device *partition_device,
    const struct appliance_state_layout *layout)
{
    return appliance_state_loader_load_with_key(
        loader, partition_device, layout, NULL);
}

bool appliance_state_loader_ready(
    const struct appliance_state_loader *loader)
{
    return loader != NULL && loader->initialized && loader->ready;
}

const struct appliance_state_layout *
appliance_state_loader_layout(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->layout;
}

const struct block_device *appliance_state_loader_partition_device(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->partition.device;
}

const struct appliance_persistent_config *
appliance_state_loader_system_config(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->system_config;
}

const struct appliance_persistent_network_config *
appliance_state_loader_network_config(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->network_config;
}

const struct appliance_persistent_secret_config *
appliance_state_loader_secret_config(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->secret_config;
}

const struct appliance_persistent_service_config *
appliance_state_loader_service_config(
    const struct appliance_state_loader *loader)
{
    if (loader == NULL) {
        return NULL;
    }
    return &loader->service_config;
}

const char *appliance_state_loader_status_string(
    enum appliance_state_loader_status status)
{
    switch (status) {
    case APPLIANCE_STATE_LOADER_OK:
        return "APPLIANCE_STATE_LOADER_OK";
    case APPLIANCE_STATE_LOADER_NULL:
        return "APPLIANCE_STATE_LOADER_NULL";
    case APPLIANCE_STATE_LOADER_BAD_STATE:
        return "APPLIANCE_STATE_LOADER_BAD_STATE";
    case APPLIANCE_STATE_LOADER_BAD_DEVICE:
        return "APPLIANCE_STATE_LOADER_BAD_DEVICE";
    case APPLIANCE_STATE_LOADER_BAD_LAYOUT:
        return "APPLIANCE_STATE_LOADER_BAD_LAYOUT";
    case APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR:
        return "APPLIANCE_STATE_LOADER_STORAGE_IO_ERROR";
    case APPLIANCE_STATE_LOADER_PARTITION_AMBIGUOUS:
        return "APPLIANCE_STATE_LOADER_PARTITION_AMBIGUOUS";
    case APPLIANCE_STATE_LOADER_PARTITION_NOT_FOUND:
        return "APPLIANCE_STATE_LOADER_PARTITION_NOT_FOUND";
    case APPLIANCE_STATE_LOADER_PARTITION_BIND_FAILED:
        return "APPLIANCE_STATE_LOADER_PARTITION_BIND_FAILED";
    case APPLIANCE_STATE_LOADER_STORE_BIND_FAILED:
        return "APPLIANCE_STATE_LOADER_STORE_BIND_FAILED";
    case APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED:
        return "APPLIANCE_STATE_LOADER_STORE_LOAD_FAILED";
    case APPLIANCE_STATE_LOADER_STORE_CORRUPT:
        return "APPLIANCE_STATE_LOADER_STORE_CORRUPT";
    case APPLIANCE_STATE_LOADER_KEY_REQUIRED:
        return "KEY_REQUIRED";
    case APPLIANCE_STATE_LOADER_KEY_REJECTED:
        return "KEY_REJECTED";
    case APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE:
        return "KEY_UNAVAILABLE";
    case APPLIANCE_STATE_LOADER_ENCRYPTION_CORRUPT:
        return "ENCRYPTION_CORRUPT";
    default:
        return NULL;
    }
}
