#ifndef HFOC_APPLIANCE_PRODUCTION_SERVICE_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_SERVICE_BOOT_H

#include "appliance_management_tls.h"
#include "appliance_production_service_activation.h"
#include "appliance_state_loader.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-364 ordinary-boot production service/HTTPS activation glue.
 *
 * This is the caller that owns the ITEM-313 service manager, the ITEM-312
 * appliance_https_service boundary, the ITEM-353 production composition, the
 * ITEM-364 activation record, and (ITEM-375) the management TLS lifecycle.
 * It binds the persisted service evidence from the ITEM-360 state loader and
 * the explicit network/entropy readiness supplied by ordinary boot.
 *
 * V1 runs the fixed HTTPS service in the kernel image: the frozen baseline has
 * no ordinary-boot process supervisor, so the production backend starts and
 * stops the single fixed service through the existing manager and HTTPS
 * lifecycle boundaries instead of spawning a kernel process. The activation
 * never fabricates readiness: absent network, entropy, or certificate evidence
 * leaves the record in an explicit awaiting or degraded state.
 *
 * ITEM-375 makes the certificate bit a real owner instead of a literal: boot
 * initializes the management TLS lifecycle, declares the certificate the
 * persisted hostname needs, and publishes readiness from the lifecycle.  No
 * certificate is fabricated and no private key is ever held here, so an
 * unprovisioned appliance honestly reports AWAITING_CERTIFICATE.
 */

bool appliance_production_service_boot_init(
    const struct appliance_state_loader *loader);
void appliance_production_service_boot_poll(
    bool network_active,
    bool entropy_ready,
    uint32_t now_ms);
/* ITEM-374: production entry point that also requires a validated clock. */
void appliance_production_service_boot_poll_clock(
    bool network_active,
    bool entropy_ready,
    bool clock_synchronized,
    uint32_t now_ms,
    uint32_t now_epoch_seconds);
const struct appliance_production_service_activation *
appliance_production_service_boot_activation(void);
/* ITEM-375: the management TLS lifecycle owner bound to this boot. */
const struct appliance_management_tls *
appliance_production_service_boot_management_tls(void);

#endif
