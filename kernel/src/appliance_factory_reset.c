#include "appliance_factory_reset.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static void appliance_factory_reset_zero_bytes(
    char *destination,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        destination[index] = '\0';
    }
}

static uint32_t appliance_factory_reset_namespace_action(
    enum appliance_persistent_storage_namespace_id id)
{
    switch (id) {
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG:
        return APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT;
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA:
        return APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION;
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA:
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE:
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET:
    case APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC:
    default:
        return APPLIANCE_FACTORY_RESET_ACTION_CLEAR;
    }
}

static void appliance_factory_reset_build_plan(
    struct appliance_factory_reset_plan *plan)
{
    static const enum appliance_persistent_storage_namespace_id ids[] = {
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SYSTEM_CONFIG,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SERVICE_DATA,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_CERTIFICATE,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_SECRET,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_DIAGNOSTIC,
        APPLIANCE_PERSISTENT_STORAGE_NAMESPACE_ID_RECOVERY_METADATA,
    };
    uint32_t index;

    appliance_factory_reset_zero_bytes(
        (char *)plan, sizeof(*plan));
    plan->version = APPLIANCE_FACTORY_RESET_VERSION;
    plan->namespace_count = APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT;
    plan->local_only = true;
    plan->requires_reboot = true;
    plan->preserves_boot_foundation = true;
    plan->preserves_recovery_foundation = true;
    for (index = 0U;
         index < APPLIANCE_FACTORY_RESET_NAMESPACE_COUNT;
         ++index) {
        plan->namespaces[index].id = ids[index];
        plan->namespaces[index].action =
            appliance_factory_reset_namespace_action(ids[index]);
    }
}

enum appliance_factory_reset_status appliance_factory_reset_init(
    struct appliance_factory_reset_gate *gate)
{
    if (gate == NULL) {
        return APPLIANCE_FACTORY_RESET_NULL;
    }
    if (gate->version == APPLIANCE_FACTORY_RESET_VERSION ||
        gate->published) {
        return APPLIANCE_FACTORY_RESET_BAD_STATE;
    }
    appliance_factory_reset_zero_bytes(
        (char *)gate, sizeof(*gate));
    gate->version = APPLIANCE_FACTORY_RESET_VERSION;
    return APPLIANCE_FACTORY_RESET_OK;
}

enum appliance_factory_reset_status appliance_factory_reset_evaluate(
    struct appliance_factory_reset_gate *gate,
    const struct appliance_factory_reset_evidence *evidence,
    struct appliance_factory_reset_plan *plan_out)
{
    struct appliance_factory_reset_plan plan;
    struct appliance_factory_reset_gate draft;

    if (gate == NULL || evidence == NULL || plan_out == NULL) {
        return APPLIANCE_FACTORY_RESET_NULL;
    }
    if (gate->version != APPLIANCE_FACTORY_RESET_VERSION ||
        gate->published) {
        return APPLIANCE_FACTORY_RESET_BAD_STATE;
    }
    if (evidence->version != APPLIANCE_FACTORY_RESET_VERSION) {
        return APPLIANCE_FACTORY_RESET_BAD_REQUEST;
    }
    if (evidence->recovery_trust_active) {
        return APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE;
    }
    if (!evidence->authorization_verified) {
        return APPLIANCE_FACTORY_RESET_UNAUTHORIZED;
    }
    if (!evidence->boot_foundation_verified) {
        return APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE;
    }
    if (!evidence->storage_loader_ready) {
        return APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY;
    }

    appliance_factory_reset_build_plan(&plan);
    appliance_factory_reset_zero_bytes(
        (char *)&draft, sizeof(draft));
    draft.version = APPLIANCE_FACTORY_RESET_VERSION;
    draft.published = true;
    draft.plan = plan;

    *gate = draft;
    *plan_out = plan;
    return APPLIANCE_FACTORY_RESET_OK;
}

bool appliance_factory_reset_is_published(
    const struct appliance_factory_reset_gate *gate)
{
    return gate != NULL &&
           gate->version == APPLIANCE_FACTORY_RESET_VERSION &&
           gate->published;
}

const char *appliance_factory_reset_status_string(
    enum appliance_factory_reset_status status)
{
    switch (status) {
    case APPLIANCE_FACTORY_RESET_OK:
        return "APPLIANCE_FACTORY_RESET_OK";
    case APPLIANCE_FACTORY_RESET_NULL:
        return "APPLIANCE_FACTORY_RESET_NULL";
    case APPLIANCE_FACTORY_RESET_BAD_STATE:
        return "APPLIANCE_FACTORY_RESET_BAD_STATE";
    case APPLIANCE_FACTORY_RESET_BAD_REQUEST:
        return "APPLIANCE_FACTORY_RESET_BAD_REQUEST";
    case APPLIANCE_FACTORY_RESET_UNAUTHORIZED:
        return "APPLIANCE_FACTORY_RESET_UNAUTHORIZED";
    case APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE:
        return "APPLIANCE_FACTORY_RESET_BOOT_FOUNDATION_UNAVAILABLE";
    case APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY:
        return "APPLIANCE_FACTORY_RESET_STORAGE_NOT_READY";
    case APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE:
        return "APPLIANCE_FACTORY_RESET_RECOVERY_TRUST_ACTIVE";
    default:
        return NULL;
    }
}

const char *appliance_factory_reset_action_string(uint32_t action)
{
    switch (action) {
    case APPLIANCE_FACTORY_RESET_ACTION_NONE:
        return "APPLIANCE_FACTORY_RESET_ACTION_NONE";
    case APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT:
        return "APPLIANCE_FACTORY_RESET_ACTION_REWRITE_FACTORY_DEFAULT";
    case APPLIANCE_FACTORY_RESET_ACTION_CLEAR:
        return "APPLIANCE_FACTORY_RESET_ACTION_CLEAR";
    case APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION:
        return "APPLIANCE_FACTORY_RESET_ACTION_PRESERVE_FOUNDATION";
    default:
        return NULL;
    }
}
