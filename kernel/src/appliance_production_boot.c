#include "appliance_production_boot.h"

#include "appliance_gpt_discovery.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static enum appliance_production_boot_status
appliance_production_boot_probe_backends(
    struct appliance_production_boot *boot,
    appliance_production_boot_probe_fn probe,
    void *probe_context,
    const struct appliance_state_layout *layout)
{
    struct appliance_production_boot_backend backends[
        APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY];
    enum appliance_production_boot_probe_status probe_status;
    struct appliance_production_boot_backend *selected = NULL;
    size_t backend_index;
    size_t count = 0U;

    memset(backends, 0, sizeof(backends));
    probe_status = probe(
        probe_context, backends,
        APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY, &count);
    if (probe_status != APPLIANCE_PRODUCTION_BOOT_PROBE_OK ||
        count > APPLIANCE_PRODUCTION_BOOT_BACKEND_CAPACITY) {
        boot->status = APPLIANCE_PRODUCTION_BOOT_PROBE_FAILED;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    }
    boot->discovered_backend_count = count;

    for (backend_index = 0U; backend_index < count; ++backend_index) {
        struct appliance_production_boot_backend *candidate =
            &backends[backend_index];
        struct appliance_gpt_hfoc_partition partition;

        if ((candidate->kind !=
                 APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI &&
             candidate->kind !=
                 APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME) ||
            block_device_validate(&candidate->device) !=
                BLOCK_DEVICE_OK) {
            continue;
        }
        memset(&partition, 0, sizeof(partition));
        if (appliance_gpt_find_hfoc_partition(
                &candidate->device, &partition) ==
                APPLIANCE_GPT_DISCOVERY_OK &&
            appliance_gpt_validate_hfoc_partition(&partition) ==
                APPLIANCE_GPT_DISCOVERY_OK) {
            selected = candidate;
            ++boot->valid_backend_count;
        }
    }

    if (boot->valid_backend_count == 0U) {
        boot->status = APPLIANCE_PRODUCTION_BOOT_NO_BACKEND;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED;
        return boot->status;
    }
    if (boot->valid_backend_count != 1U) {
        boot->status =
            APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    }

    boot->selected_kind = selected->kind;
    boot->selected_identity = selected->identity;
    if (appliance_state_loader_init(&boot->state) !=
        APPLIANCE_STATE_LOADER_OK) {
        boot->status =
            APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    }

    if (appliance_state_loader_load_with_key(
            &boot->state, &selected->device, layout,
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
            boot->state_key
#else
            NULL
#endif
            ) == APPLIANCE_STATE_LOADER_OK) {
        boot->status = APPLIANCE_PRODUCTION_BOOT_OK;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_READY;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_NORMAL;
        return boot->status;
    }

    switch (boot->state.status) {
    case APPLIANCE_STATE_LOADER_KEY_REQUIRED:
        /* A sealed state volume without key material boots degraded with an
         * honest LOCKED status instead of a fabricated ready. */
        boot->status = APPLIANCE_PRODUCTION_BOOT_STATE_LOCKED;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED;
        return boot->status;
    case APPLIANCE_STATE_LOADER_KEY_REJECTED:
    case APPLIANCE_STATE_LOADER_KEY_UNAVAILABLE:
    case APPLIANCE_STATE_LOADER_ENCRYPTION_CORRUPT:
        /* A wrong, unavailable or corrupt key never opens the stores and
         * never publishes ready; the state stays fail-closed. */
        boot->status =
            APPLIANCE_PRODUCTION_BOOT_STATE_KEY_REJECTED;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    default:
        break;
    }

    boot->status = APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED;
    boot->readiness =
        APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
    boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
    return boot->status;
}

enum appliance_production_boot_status appliance_production_boot_init(
    struct appliance_production_boot *boot)
{
    if (boot == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_NULL;
    }
    memset(boot, 0, sizeof(*boot));
    boot->initialized = true;
    boot->attestation = APPLIANCE_PRODUCTION_BOOT_ATTESTATION_NONE;
    boot->attestation_detail = NULL;
    boot->status = APPLIANCE_PRODUCTION_BOOT_NO_BACKEND;
    boot->readiness =
        APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
    boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED;
    return APPLIANCE_PRODUCTION_BOOT_OK;
}

enum appliance_production_boot_status appliance_production_boot_run(
    struct appliance_production_boot *boot,
    appliance_production_boot_probe_fn probe,
    void *probe_context)
{
    struct appliance_state_layout layout;

    if (boot == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_NULL;
    }
    /* A provisioned chain that did not verify refuses before any probing:
     * the gate is not conditional on a probe being supplied. */
    if (boot->attestation ==
        APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED) {
        boot->status = APPLIANCE_PRODUCTION_BOOT_UNATTESTED;
        boot->readiness =
            APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    }
    if (probe == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_NULL;
    }
    if (!boot->initialized ||
        boot->readiness ==
            APPLIANCE_PRODUCTION_BOOT_READINESS_READY) {
        return APPLIANCE_PRODUCTION_BOOT_BAD_STATE;
    }

    memset(&boot->state, 0, sizeof(boot->state));
    boot->status = APPLIANCE_PRODUCTION_BOOT_NO_BACKEND;
    boot->readiness =
        APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
    boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED;
    boot->discovered_backend_count = 0U;
    boot->valid_backend_count = 0U;
    boot->selected_kind = APPLIANCE_PRODUCTION_BOOT_BACKEND_NONE;
    boot->selected_identity = 0U;

    if (appliance_state_layout_default(&layout) !=
        APPLIANCE_STATE_LAYOUT_OK) {
        boot->status =
            APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED;
        boot->mode = APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
        return boot->status;
    }
    return appliance_production_boot_probe_backends(
        boot, probe, probe_context, &layout);
}

enum appliance_production_boot_status
appliance_production_boot_set_attestation(
    struct appliance_production_boot *boot,
    enum appliance_production_boot_attestation attestation,
    const char *detail)
{
    if (boot == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_NULL;
    }
    if (!boot->initialized ||
        attestation < APPLIANCE_PRODUCTION_BOOT_ATTESTATION_NONE ||
        attestation > APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED) {
        return APPLIANCE_PRODUCTION_BOOT_BAD_STATE;
    }
    boot->attestation = attestation;
    boot->attestation_detail =
        attestation == APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED
            ? detail
            : NULL;
    return APPLIANCE_PRODUCTION_BOOT_OK;
}

enum appliance_production_boot_status
appliance_production_boot_set_state_key(
    struct appliance_production_boot *boot,
    const struct appliance_key_custody_material *material)
{
    if (boot == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_NULL;
    }
    if (!boot->initialized) {
        return APPLIANCE_PRODUCTION_BOOT_BAD_STATE;
    }
#if defined(APPLIANCE_STATE_ENCRYPTION_ENABLED)
    boot->state_key = material;
    return APPLIANCE_PRODUCTION_BOOT_OK;
#else
    (void)material;
    return APPLIANCE_PRODUCTION_BOOT_BAD_STATE;
#endif
}

enum appliance_production_boot_attestation
appliance_production_boot_attestation(
    const struct appliance_production_boot *boot)
{
    if (boot == NULL) {
        return APPLIANCE_PRODUCTION_BOOT_ATTESTATION_NONE;
    }
    return boot->attestation;
}

const char *appliance_production_boot_attestation_detail(
    const struct appliance_production_boot *boot)
{
    if (boot == NULL ||
        boot->attestation !=
            APPLIANCE_PRODUCTION_BOOT_ATTESTATION_REFUSED) {
        return NULL;
    }
    return boot->attestation_detail;
}

enum appliance_production_boot_readiness appliance_production_boot_readiness(
    const struct appliance_production_boot *boot)
{
    return boot != NULL
        ? boot->readiness
        : APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE;
}

enum appliance_production_boot_mode appliance_production_boot_mode(
    const struct appliance_production_boot *boot)
{
    return boot != NULL
        ? boot->mode
        : APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED;
}

const struct appliance_state_loader *
appliance_production_boot_state_loader(
    const struct appliance_production_boot *boot)
{
    if (boot == NULL ||
        boot->readiness !=
            APPLIANCE_PRODUCTION_BOOT_READINESS_READY) {
        return NULL;
    }
    return &boot->state;
}

uint32_t appliance_production_boot_configured_backend_count(
    const struct appliance_production_boot *boot)
{
    return boot != NULL &&
            boot->readiness ==
                APPLIANCE_PRODUCTION_BOOT_READINESS_READY
        ? 1U
        : 0U;
}

const char *appliance_production_boot_backend_kind_string(
    enum appliance_production_boot_backend_kind kind)
{
    switch (kind) {
    case APPLIANCE_PRODUCTION_BOOT_BACKEND_NONE:
        return "NONE";
    case APPLIANCE_PRODUCTION_BOOT_BACKEND_AHCI:
        return "AHCI";
    case APPLIANCE_PRODUCTION_BOOT_BACKEND_NVME:
        return "NVME";
    default:
        return NULL;
    }
}

const char *appliance_production_boot_readiness_string(
    enum appliance_production_boot_readiness readiness)
{
    switch (readiness) {
    case APPLIANCE_PRODUCTION_BOOT_READINESS_UNAVAILABLE:
        return "UNAVAILABLE";
    case APPLIANCE_PRODUCTION_BOOT_READINESS_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_BOOT_READINESS_READY:
        return "READY";
    default:
        return NULL;
    }
}

const char *appliance_production_boot_mode_string(
    enum appliance_production_boot_mode mode)
{
    switch (mode) {
    case APPLIANCE_PRODUCTION_BOOT_MODE_FAIL_CLOSED:
        return "FAIL_CLOSED";
    case APPLIANCE_PRODUCTION_BOOT_MODE_DEGRADED:
        return "DEGRADED";
    case APPLIANCE_PRODUCTION_BOOT_MODE_NORMAL:
        return "NORMAL";
    default:
        return NULL;
    }
}

const char *appliance_production_boot_status_string(
    enum appliance_production_boot_status status)
{
    switch (status) {
    case APPLIANCE_PRODUCTION_BOOT_OK:
        return "APPLIANCE_PRODUCTION_BOOT_OK";
    case APPLIANCE_PRODUCTION_BOOT_NULL:
        return "APPLIANCE_PRODUCTION_BOOT_NULL";
    case APPLIANCE_PRODUCTION_BOOT_BAD_STATE:
        return "APPLIANCE_PRODUCTION_BOOT_BAD_STATE";
    case APPLIANCE_PRODUCTION_BOOT_PROBE_FAILED:
        return "APPLIANCE_PRODUCTION_BOOT_PROBE_FAILED";
    case APPLIANCE_PRODUCTION_BOOT_NO_BACKEND:
        return "APPLIANCE_PRODUCTION_BOOT_NO_BACKEND";
    case APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND:
        return "APPLIANCE_PRODUCTION_BOOT_AMBIGUOUS_BACKEND";
    case APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED:
        return "APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED";
    case APPLIANCE_PRODUCTION_BOOT_UNATTESTED:
        return "APPLIANCE_PRODUCTION_BOOT_UNATTESTED";
    case APPLIANCE_PRODUCTION_BOOT_STATE_LOCKED:
        return "APPLIANCE_PRODUCTION_BOOT_STATE_LOCKED";
    case APPLIANCE_PRODUCTION_BOOT_STATE_KEY_REJECTED:
        return "APPLIANCE_PRODUCTION_BOOT_STATE_KEY_REJECTED";
    default:
        return NULL;
    }
}
