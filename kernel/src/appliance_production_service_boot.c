#include "appliance_production_service_boot.h"

#ifdef HFOC_PRODUCTION_SERVICES

#include "appliance_management_tls.h"
#include "appliance_persistent_config.h"
#include "appliance_production_service_activation.h"
#include "appliance_production_service_https.h"
#include "appliance_identity.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPLIANCE_PRODUCTION_SERVICE_BOOT_GENERATION 1U

/*
 * The frozen baseline keeps the ITEM-311 service manager and the ITEM-312
 * HTTPS lifecycle boundary host/QEMU-only, so ordinary boot owns the fixed V1
 * service state directly. The V1 profile is exactly one fixed HTTPS service
 * that is configured and stopped by the kernel baseline; the activation binds
 * that bounded state as its service evidence instead of fabricating a manager
 * that the production kernel does not link.
 */
struct appliance_production_service_state {
    bool configured;
    bool stopped;
    bool https_enabled;
    bool https_stopped;
};

struct appliance_production_service_boot {
    bool initialized;
    bool configured;
    struct appliance_production_service_state service;
    struct appliance_production_service_https composition;
    struct appliance_production_service_activation activation;
    /* ITEM-375: the certificate readiness owner for the HTTPS activation. */
    struct appliance_management_tls management_tls;
};

static struct appliance_production_service_boot
    appliance_production_service_boot_state;

static void appliance_production_service_boot_zero_bytes(
    void *destination,
    size_t length)
{
    unsigned char *bytes = destination;
    size_t index;

    for (index = 0U; index < length; ++index) {
        bytes[index] = 0U;
    }
}

/*
 * ITEM-375: declare the certificate the appliance identity needs.  Ordinary
 * boot owns no certificate and no private key, so it publishes the bounded
 * requirement and leaves the lifecycle awaiting a validated import.  ITEM-404:
 * the declared identity is the one the certificate owner will actually use -
 * the persisted hostname when it is readable and bounds-valid, and the
 * documented default hostname otherwise - so a boot whose state volume is
 * absent or empty still configures the lifecycle with the certificate it
 * serves instead of leaving it UNCONFIGURED and rejecting the certificate.
 */
static void appliance_production_service_boot_declare_certificate(
    const struct appliance_state_loader *loader)
{
    struct appliance_production_service_boot *boot =
        &appliance_production_service_boot_state;
    const struct appliance_persistent_config *config =
        loader != NULL && loader->ready
            ? appliance_state_loader_system_config(loader)
            : NULL;
    struct appliance_management_tls_ingress ingress;
    char hostname[APPLIANCE_PERSISTENT_CONFIG_HOSTNAME_MAX + 1U];
    const char *identity = APPLIANCE_IDENTITY_DEFAULT_HOSTNAME;
    size_t identity_length = APPLIANCE_IDENTITY_DEFAULT_HOSTNAME_LENGTH;
    size_t hostname_length = 0U;

    if (config != NULL) {
        appliance_production_service_boot_zero_bytes(
            hostname, sizeof(hostname));
        if (appliance_persistent_config_hostname(
                config, hostname, sizeof(hostname), &hostname_length) ==
                APPLIANCE_PERSISTENT_CONFIG_OK &&
            hostname_length != 0U &&
            hostname_length <= APPLIANCE_MANAGEMENT_TLS_IDENTITY_MAX) {
            identity = hostname;
            identity_length = hostname_length;
        }
    }

    appliance_production_service_boot_zero_bytes(
        &ingress, sizeof(ingress));
    ingress.version = APPLIANCE_MANAGEMENT_TLS_VERSION;
    ingress.kind = APPLIANCE_MANAGEMENT_TLS_INGRESS_LOCAL;
    (void)appliance_management_tls_request_certificate(
        &boot->management_tls,
        NULL,
        &ingress,
        identity,
        identity_length,
        identity,
        identity_length,
        0U);
}

bool appliance_production_service_boot_init(
    const struct appliance_state_loader *loader)
{
    struct appliance_production_service_boot *boot =
        &appliance_production_service_boot_state;
    struct appliance_production_service_https_evidence evidence;

    if (boot->initialized) {
        return true;
    }
    appliance_production_service_boot_zero_bytes(boot, sizeof(*boot));
    boot->service.configured = true;
    boot->service.stopped = true;
    boot->service.https_enabled = true;
    boot->service.https_stopped = true;

    if (appliance_production_service_https_init(&boot->composition) !=
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_OK) {
        return false;
    }
    if (appliance_production_service_activation_init(
            &boot->activation, &boot->composition) !=
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK) {
        return false;
    }
    if (appliance_management_tls_init(&boot->management_tls) !=
        APPLIANCE_MANAGEMENT_TLS_OK) {
        return false;
    }
    boot->initialized = true;

    /*
     * ITEM-404: the certificate request is declared even without a state
     * loader, because the serving path generates and serves a per-boot
     * certificate from the documented default identity in that case.  The
     * request must name the identity the certificate will actually carry, or
     * the lifecycle rejects the certificate it is served.
     */
    appliance_production_service_boot_declare_certificate(loader);

    if (loader == NULL || !loader->initialized) {
        return true;
    }

    appliance_production_service_boot_zero_bytes(
        &evidence, sizeof(evidence));
    evidence.version =
        APPLIANCE_PRODUCTION_SERVICE_HTTPS_EVIDENCE_VERSION;
    evidence.source_generation = APPLIANCE_PRODUCTION_SERVICE_BOOT_GENERATION;
    evidence.published = true;
    evidence.loaded_state_committed = true;
    evidence.manager_configured = boot->service.configured;
    evidence.manager_stopped = boot->service.stopped;
    evidence.https_enabled = boot->service.https_enabled;
    evidence.https_stopped = boot->service.https_stopped;

    if (appliance_production_service_activation_configure(
            &boot->activation, &evidence) ==
        APPLIANCE_PRODUCTION_SERVICE_ACTIVATION_OK) {
        boot->configured = true;
    }
    return true;
}

void appliance_production_service_boot_poll(
    bool network_active,
    bool entropy_ready,
    uint32_t now_ms)
{
    appliance_production_service_boot_poll_clock(
        network_active, entropy_ready, true, now_ms, 0U);
}

void appliance_production_service_boot_poll_clock(
    bool network_active,
    bool entropy_ready,
    bool clock_synchronized,
    uint32_t now_ms,
    uint32_t now_epoch_seconds)
{
    struct appliance_production_service_boot *boot =
        &appliance_production_service_boot_state;
    bool certificate_ready = false;

    if (!boot->initialized || !boot->configured) {
        return;
    }
    /*
     * ITEM-375: the certificate bit is owned by the management TLS lifecycle,
     * never by the caller.  A certificate is ready only while the active
     * certificate is policy-compliant and inside its validity window on the
     * validated wall clock; an unsynchronized clock (epoch 0) leaves it
     * unready rather than trusting an unvalidated time.
     */
    if (clock_synchronized) {
        (void)appliance_management_tls_readiness(
            &boot->management_tls, now_epoch_seconds,
            &certificate_ready);
    }
    (void)appliance_production_service_activation_evaluate_clock(
        &boot->activation,
        network_active,
        entropy_ready,
        certificate_ready,
        clock_synchronized,
        now_ms);
}

const struct appliance_production_service_activation *
appliance_production_service_boot_activation(void)
{
    struct appliance_production_service_boot *boot =
        &appliance_production_service_boot_state;

    return boot->initialized ? &boot->activation : NULL;
}

const struct appliance_management_tls *
appliance_production_service_boot_management_tls(void)
{
    struct appliance_production_service_boot *boot =
        &appliance_production_service_boot_state;

    return boot->initialized ? &boot->management_tls : NULL;
}

#else

typedef int appliance_production_service_boot_translation_unit_guard;

#endif
