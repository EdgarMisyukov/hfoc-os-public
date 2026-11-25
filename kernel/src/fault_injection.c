#include "fault_injection.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static const struct fault_injection_site fault_injection_catalogue[
    FAULT_INJECTION_CATALOGUE_CAPACITY] = {
    {FAULT_INJECTION_BLOCK_DEVICE_READ,
     "FAULT_INJECTION_BLOCK_DEVICE_READ",
     FAULT_INJECTION_SUBSYSTEM_STORAGE,
     "BLOCK_DEVICE_IO_ERROR; destination unchanged",
     true,
     true},
    {FAULT_INJECTION_BLOCK_DEVICE_WRITE,
     "FAULT_INJECTION_BLOCK_DEVICE_WRITE",
     FAULT_INJECTION_SUBSYSTEM_STORAGE,
     "BLOCK_DEVICE_IO_ERROR; storage unchanged",
     true,
     true},
    {FAULT_INJECTION_CONFIG_JOURNAL_WRITE,
     "FAULT_INJECTION_CONFIG_JOURNAL_WRITE",
     FAULT_INJECTION_SUBSYSTEM_STORAGE,
     "write fault rolls back exact active state",
     true,
     true},
    {FAULT_INJECTION_CONFIG_JOURNAL_READBACK,
     "FAULT_INJECTION_CONFIG_JOURNAL_READBACK",
     FAULT_INJECTION_SUBSYSTEM_STORAGE,
     "readback mismatch rejects publication",
     true,
     true},
    {FAULT_INJECTION_RESTORE_WRITE,
     "FAULT_INJECTION_RESTORE_WRITE",
     FAULT_INJECTION_SUBSYSTEM_STORAGE,
     "RESTORE_WRITE_FAULT; active state restored",
     true,
     true},
    {FAULT_INJECTION_SERVICE_DEPENDENCY,
     "FAULT_INJECTION_SERVICE_DEPENDENCY",
     FAULT_INJECTION_SUBSYSTEM_SERVICE,
     "dependency failure blocks service start",
     true,
     true},
    {FAULT_INJECTION_SERVICE_RESTART_EXHAUSTION,
     "FAULT_INJECTION_SERVICE_RESTART_EXHAUSTION",
     FAULT_INJECTION_SUBSYSTEM_SERVICE,
     "restart exhaustion fails service without leak",
     true,
     true},
    {FAULT_INJECTION_SERVICE_CONTROLLED_STOP,
     "FAULT_INJECTION_SERVICE_CONTROLLED_STOP",
     FAULT_INJECTION_SUBSYSTEM_SERVICE,
     "controlled stop leaves no orphan endpoint",
     true,
     true},
    {FAULT_INJECTION_LOCAL_SESSION_EXPIRY,
     "FAULT_INJECTION_LOCAL_SESSION_EXPIRY",
     FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL,
     "expired session denied; authorized recovery remains possible",
     true,
     true},
    {FAULT_INJECTION_AUTH_THROTTLE,
     "FAULT_INJECTION_AUTH_THROTTLE",
     FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL,
     "throttle blocks brute force; future authorized login remains possible",
     true,
     true},
    {FAULT_INJECTION_LOCAL_MALFORMED_COMMAND,
     "FAULT_INJECTION_LOCAL_MALFORMED_COMMAND",
     FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL,
     "malformed command rejected without state mutation",
     true,
     true},
    {FAULT_INJECTION_NETWORK_LINK_LOSS,
     "FAULT_INJECTION_NETWORK_LINK_LOSS",
     FAULT_INJECTION_SUBSYSTEM_NETWORK,
     "local operation preserved; bounded health/audit evidence",
     true,
     true},
    {FAULT_INJECTION_NETWORK_PEER_SILENCE,
     "FAULT_INJECTION_NETWORK_PEER_SILENCE",
     FAULT_INJECTION_SUBSYSTEM_NETWORK,
     "peer silence bounded and isolated; local operation preserved",
     true,
     true},
    {FAULT_INJECTION_TRUST_REVOKED_SIGNER,
     "FAULT_INJECTION_TRUST_REVOKED_SIGNER",
     FAULT_INJECTION_SUBSYSTEM_TRUST,
     "revoked signer denied; known-good state preserved",
     true,
     true},
    {FAULT_INJECTION_TRUST_TAMPERED_METADATA,
     "FAULT_INJECTION_TRUST_TAMPERED_METADATA",
     FAULT_INJECTION_SUBSYSTEM_TRUST,
     "tampered metadata denied with exact mismatch",
     true,
     true},
    {FAULT_INJECTION_TRUST_RECOVERY_PURPOSE,
     "FAULT_INJECTION_TRUST_RECOVERY_PURPOSE",
     FAULT_INJECTION_SUBSYSTEM_TRUST,
     "recovery-purpose mismatch denied",
     true,
     true},
    {FAULT_INJECTION_UPDATE_STAGED_WRITE,
     "FAULT_INJECTION_UPDATE_STAGED_WRITE",
     FAULT_INJECTION_SUBSYSTEM_UPDATE,
     "interrupted staged write never activates",
     true,
     true},
    {FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH,
     "FAULT_INJECTION_UPDATE_FIRST_BOOT_HEALTH",
     FAULT_INJECTION_SUBSYSTEM_UPDATE,
     "first-boot health failure selects known-good",
     true,
     true},
    {FAULT_INJECTION_INTEGRATION_DUPLICATE,
     "FAULT_INJECTION_INTEGRATION_DUPLICATE",
     FAULT_INJECTION_SUBSYSTEM_INTEGRATION,
     "duplicate request is idempotent",
     true,
     true},
    {FAULT_INJECTION_INTEGRATION_MALFORMED,
     "FAULT_INJECTION_INTEGRATION_MALFORMED",
     FAULT_INJECTION_SUBSYSTEM_INTEGRATION,
     "malformed request rejected and isolated",
     true,
     true},
    {FAULT_INJECTION_INTEGRATION_INCOMPATIBLE,
     "FAULT_INJECTION_INTEGRATION_INCOMPATIBLE",
     FAULT_INJECTION_SUBSYSTEM_INTEGRATION,
     "incompatible request rejected and isolated",
     true,
     true},
    {FAULT_INJECTION_INTEGRATION_UNAVAILABLE,
     "FAULT_INJECTION_INTEGRATION_UNAVAILABLE",
     FAULT_INJECTION_SUBSYSTEM_INTEGRATION,
     "unavailable/slow peer isolated without local block",
     true,
     true},
    {FAULT_INJECTION_SCHEDULER_SPAWN_NO_MEMORY,
     "FAULT_INJECTION_SCHEDULER_SPAWN_NO_MEMORY",
     FAULT_INJECTION_SUBSYSTEM_SCHEDULER,
     "spawn failure is ordinary status; no leak",
     true,
     true},
    {FAULT_INJECTION_SCHEDULER_BAD_CONTEXT,
     "FAULT_INJECTION_SCHEDULER_BAD_CONTEXT",
     FAULT_INJECTION_SUBSYSTEM_SCHEDULER,
     "invalid context is rejected; no state mutation",
     true,
     true},
};

size_t fault_injection_catalogue_count(void)
{
    return sizeof(fault_injection_catalogue) /
        sizeof(fault_injection_catalogue[0]);
}

enum fault_injection_status fault_injection_catalogue_at(
    size_t index,
    const struct fault_injection_site **site_out)
{
    if (site_out == NULL) {
        return FAULT_INJECTION_NULL;
    }
    if (index >= fault_injection_catalogue_count()) {
        *site_out = NULL;
        return FAULT_INJECTION_OUT_OF_RANGE;
    }

    *site_out = &fault_injection_catalogue[index];
    return FAULT_INJECTION_OK;
}

enum fault_injection_status fault_injection_site_by_id(
    enum fault_injection_id id,
    const struct fault_injection_site **site_out)
{
    size_t index;

    if (site_out == NULL) {
        return FAULT_INJECTION_NULL;
    }

    for (index = 0U;
         index < fault_injection_catalogue_count();
         ++index) {
        if (fault_injection_catalogue[index].id == id) {
            *site_out = &fault_injection_catalogue[index];
            return FAULT_INJECTION_OK;
        }
    }

    *site_out = NULL;
    return FAULT_INJECTION_BAD_ID;
}

const char *fault_injection_subsystem_string(
    enum fault_injection_subsystem subsystem)
{
    switch (subsystem) {
    case FAULT_INJECTION_SUBSYSTEM_STORAGE:
        return "FAULT_INJECTION_SUBSYSTEM_STORAGE";
    case FAULT_INJECTION_SUBSYSTEM_SERVICE:
        return "FAULT_INJECTION_SUBSYSTEM_SERVICE";
    case FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL:
        return "FAULT_INJECTION_SUBSYSTEM_LOCAL_CONTROL";
    case FAULT_INJECTION_SUBSYSTEM_NETWORK:
        return "FAULT_INJECTION_SUBSYSTEM_NETWORK";
    case FAULT_INJECTION_SUBSYSTEM_TRUST:
        return "FAULT_INJECTION_SUBSYSTEM_TRUST";
    case FAULT_INJECTION_SUBSYSTEM_UPDATE:
        return "FAULT_INJECTION_SUBSYSTEM_UPDATE";
    case FAULT_INJECTION_SUBSYSTEM_INTEGRATION:
        return "FAULT_INJECTION_SUBSYSTEM_INTEGRATION";
    case FAULT_INJECTION_SUBSYSTEM_SCHEDULER:
        return "FAULT_INJECTION_SUBSYSTEM_SCHEDULER";
    case FAULT_INJECTION_SUBSYSTEM_UNKNOWN:
    default:
        return NULL;
    }
}

const char *fault_injection_status_string(
    enum fault_injection_status status)
{
    switch (status) {
    case FAULT_INJECTION_OK:
        return "FAULT_INJECTION_OK";
    case FAULT_INJECTION_NULL:
        return "FAULT_INJECTION_NULL";
    case FAULT_INJECTION_BAD_VERSION:
        return "FAULT_INJECTION_BAD_VERSION";
    case FAULT_INJECTION_BAD_ID:
        return "FAULT_INJECTION_BAD_ID";
    case FAULT_INJECTION_BAD_SEQUENCE:
        return "FAULT_INJECTION_BAD_SEQUENCE";
    case FAULT_INJECTION_NOT_PERMITTED:
        return "FAULT_INJECTION_NOT_PERMITTED";
    case FAULT_INJECTION_OUT_OF_RANGE:
        return "FAULT_INJECTION_OUT_OF_RANGE";
    default:
        return NULL;
    }
}

const char *fault_injection_id_string(enum fault_injection_id id)
{
    const struct fault_injection_site *site = NULL;

    if (fault_injection_site_by_id(id, &site) != FAULT_INJECTION_OK) {
        return NULL;
    }
    return site->name;
}

#ifdef HFOC_FAULT_INJECTION_HOST_TEST
enum fault_injection_status fault_injection_activation_validate(
    const struct fault_injection_activation *activation)
{
    const struct fault_injection_site *site = NULL;

    if (activation == NULL) {
        return FAULT_INJECTION_NULL;
    }
    if (activation->version != FAULT_INJECTION_ACTIVATION_VERSION) {
        return FAULT_INJECTION_BAD_VERSION;
    }
    if (fault_injection_site_by_id(
            activation->id, &site) != FAULT_INJECTION_OK) {
        return FAULT_INJECTION_BAD_ID;
    }
    if (!site->test_only) {
        return FAULT_INJECTION_NOT_PERMITTED;
    }
    if (activation->sequence == 0U ||
        activation->sequence > FAULT_INJECTION_SEQUENCE_MAX) {
        return FAULT_INJECTION_BAD_SEQUENCE;
    }
    return FAULT_INJECTION_OK;
}
#endif
