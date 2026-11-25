#include "metrics.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define METRICS_PATH_MASK_HEALTH 1U
#define METRICS_PATH_MASK_STATE 2U
#define METRICS_PATH_MASK_SERVICE 4U
#define METRICS_PATH_MASK_UPDATE 8U
#define METRICS_PATH_MASK_ALL 0xFU

static void metrics_zero_bytes(unsigned char *bytes, size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        bytes[index] = 0U;
    }
}

static bool metrics_health_values_valid(
    const struct metrics_health_values *values)
{
    if (values->version != METRICS_VERSION ||
        values->check_count > METRICS_HEALTH_CHECK_MAX) {
        return false;
    }
    if (values->ring_published) {
        if (values->ring_count > METRICS_EVENT_RING_CAPACITY ||
            values->ring_available > METRICS_EVENT_RING_CAPACITY ||
            values->ring_count + values->ring_available !=
                METRICS_EVENT_RING_CAPACITY) {
            return false;
        }
        return values->ring_utilization_percent ==
            (values->ring_count * 100U) /
                METRICS_EVENT_RING_CAPACITY;
    }
    return values->ring_count == 0U &&
           values->ring_available == 0U &&
           values->ring_utilization_percent == 0U;
}

static bool metrics_state_values_valid(
    const struct metrics_state_values *values)
{
    return values->version == METRICS_VERSION &&
           values->schema_version >=
               METRICS_STATE_SCHEMA_VERSION_MIN &&
           values->schema_version <=
               METRICS_STATE_SCHEMA_VERSION_MAX;
}

static bool metrics_service_values_valid(
    const struct metrics_service_values *values)
{
    if (values->version != METRICS_VERSION ||
        values->service_count > METRICS_SERVICE_CAPACITY ||
        values->ready_count + values->pending_count +
            values->failed_count != values->service_count) {
        return false;
    }
    return values->exhausted_count <= values->failed_count &&
           values->dependency_blocked_count <=
               values->service_count;
}

static bool metrics_update_values_valid(
    const struct metrics_update_values *values)
{
    if (values->version != METRICS_VERSION ||
        values->slot_count > METRICS_SLOT_CAPACITY ||
        values->known_good_slot > METRICS_SLOT_CAPACITY ||
        values->pending_slot > METRICS_SLOT_CAPACITY ||
        values->rollback_slot > METRICS_SLOT_CAPACITY) {
        return false;
    }
    if (values->slot_count == 0U &&
        (values->known_good_slot != 0U ||
         values->pending_slot != 0U ||
         values->rollback_slot != 0U)) {
        return false;
    }
    return true;
}

static bool metrics_paths_valid(
    const struct metrics_paths *paths)
{
    return metrics_health_values_valid(&paths->health) &&
           metrics_state_values_valid(&paths->state) &&
           metrics_service_values_valid(&paths->service) &&
           metrics_update_values_valid(&paths->update);
}

enum metrics_status metrics_init(struct metrics_store *store)
{
    if (store == NULL) {
        return METRICS_NULL;
    }
    if (store->initialized) {
        return METRICS_BAD_STATE;
    }

    metrics_zero_bytes((unsigned char *)store, sizeof(*store));
    store->version = METRICS_VERSION;
    store->initialized = true;
    return METRICS_OK;
}

enum metrics_status metrics_begin_update(struct metrics_store *store)
{
    if (store == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (store->mutation_open) {
        return METRICS_TRANSACTION_OPEN;
    }

    metrics_zero_bytes(
        (unsigned char *)&store->pending_paths,
        sizeof(store->pending_paths));
    store->pending_path_mask = 0U;
    store->mutation_open = true;
    return METRICS_OK;
}

enum metrics_status metrics_set_health(
    struct metrics_store *store,
    const struct metrics_health_values *values)
{
    if (store == NULL || values == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }
    if (values->version != METRICS_VERSION) {
        return METRICS_BAD_VERSION;
    }
    if (!metrics_health_values_valid(values)) {
        return METRICS_BAD_VALUE;
    }

    store->pending_paths.health = *values;
    store->pending_path_mask |= METRICS_PATH_MASK_HEALTH;
    return METRICS_OK;
}

enum metrics_status metrics_set_state(
    struct metrics_store *store,
    const struct metrics_state_values *values)
{
    if (store == NULL || values == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }
    if (values->version != METRICS_VERSION) {
        return METRICS_BAD_VERSION;
    }
    if (!metrics_state_values_valid(values)) {
        return METRICS_BAD_VALUE;
    }

    store->pending_paths.state = *values;
    store->pending_path_mask |= METRICS_PATH_MASK_STATE;
    return METRICS_OK;
}

enum metrics_status metrics_set_service(
    struct metrics_store *store,
    const struct metrics_service_values *values)
{
    if (store == NULL || values == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }
    if (values->version != METRICS_VERSION) {
        return METRICS_BAD_VERSION;
    }
    if (!metrics_service_values_valid(values)) {
        return METRICS_BAD_VALUE;
    }

    store->pending_paths.service = *values;
    store->pending_path_mask |= METRICS_PATH_MASK_SERVICE;
    return METRICS_OK;
}

enum metrics_status metrics_set_update(
    struct metrics_store *store,
    const struct metrics_update_values *values)
{
    if (store == NULL || values == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }
    if (values->version != METRICS_VERSION) {
        return METRICS_BAD_VERSION;
    }
    if (!metrics_update_values_valid(values)) {
        return METRICS_BAD_VALUE;
    }

    store->pending_paths.update = *values;
    store->pending_path_mask |= METRICS_PATH_MASK_UPDATE;
    return METRICS_OK;
}

enum metrics_status metrics_commit(struct metrics_store *store)
{
    if (store == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }
    if (store->pending_path_mask != METRICS_PATH_MASK_ALL) {
        return METRICS_INCOMPLETE;
    }
    if (!metrics_paths_valid(&store->pending_paths)) {
        return METRICS_BAD_VALUE;
    }
    if (store->generation == UINT64_MAX) {
        return METRICS_GENERATION_OVERFLOW;
    }

    ++store->generation;
    store->published_paths = store->pending_paths;
    store->published = true;
    store->pending_path_mask = 0U;
    store->mutation_open = false;
    return METRICS_OK;
}

enum metrics_status metrics_rollback(struct metrics_store *store)
{
    if (store == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->mutation_open) {
        return METRICS_NO_TRANSACTION;
    }

    metrics_zero_bytes(
        (unsigned char *)&store->pending_paths,
        sizeof(store->pending_paths));
    store->pending_path_mask = 0U;
    store->mutation_open = false;
    return METRICS_OK;
}

enum metrics_status metrics_snapshot(
    const struct metrics_store *store,
    struct metrics_snapshot *snapshot_out)
{
    if (store == NULL || snapshot_out == NULL) {
        return METRICS_NULL;
    }
    if (!store->initialized || store->version != METRICS_VERSION) {
        return METRICS_BAD_STATE;
    }
    if (!store->published || store->generation == 0U) {
        return METRICS_NOT_PUBLISHED;
    }
    if (!metrics_paths_valid(&store->published_paths)) {
        return METRICS_BAD_STATE;
    }

    metrics_zero_bytes(
        (unsigned char *)snapshot_out, sizeof(*snapshot_out));
    snapshot_out->version = METRICS_VERSION;
    snapshot_out->generation = store->generation;
    snapshot_out->paths = store->published_paths;
    return METRICS_OK;
}

const char *metrics_status_string(enum metrics_status status)
{
    switch (status) {
    case METRICS_OK:
        return "METRICS_OK";
    case METRICS_NULL:
        return "METRICS_NULL";
    case METRICS_BAD_STATE:
        return "METRICS_BAD_STATE";
    case METRICS_BAD_VERSION:
        return "METRICS_BAD_VERSION";
    case METRICS_NOT_PUBLISHED:
        return "METRICS_NOT_PUBLISHED";
    case METRICS_NO_TRANSACTION:
        return "METRICS_NO_TRANSACTION";
    case METRICS_TRANSACTION_OPEN:
        return "METRICS_TRANSACTION_OPEN";
    case METRICS_BAD_VALUE:
        return "METRICS_BAD_VALUE";
    case METRICS_INCOMPLETE:
        return "METRICS_INCOMPLETE";
    case METRICS_GENERATION_OVERFLOW:
        return "METRICS_GENERATION_OVERFLOW";
    default:
        return NULL;
    }
}

const char *metrics_schema_id(void)
{
    return METRICS_SCHEMA_ID;
}
