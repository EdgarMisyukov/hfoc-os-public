#ifndef HFOC_APPLIANCE_PRODUCTION_ACME_ORDER_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_ACME_ORDER_BOOT_H

#include "appliance_production_acme_http01.h"
#include "appliance_production_acme_service.h"
#include "appliance_production_certificate.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ITEM-508 increment 35: the ordinary-boot composition of the order path.
 *
 * `appliance_production_acme_order` is the driver and decides nothing about
 * when an order is wanted; this module is the caller that does.  It lives in
 * its own translation unit because it binds the lwIP and mbedTLS transport
 * link, and the frozen rule is that only the TLS-profile objects see the
 * mbedTLS configuration header - the ordinary kernel loop calls three plain
 * functions instead of including the binding.
 *
 *   * the owners (the ACME service, the certificate owner and the HTTP-01
 *     listener) are published once, after they exist;
 *   * `tick` configures the driver once, when the wire has a lifecycle, with
 *     the image's own anchor, the production Let's Encrypt directory and the
 *     listener adapter, then runs at most one order at a time: the durable
 *     control's request, or the renewal policy's DUE decision.  An order is
 *     started only on the validated wall clock and only while the listener
 *     reports listening, so a challenge that could not be answered is never
 *     attempted;
 *   * the operator's request is consumed durably the moment an order starts,
 *     or refuses to start with a named record; a renewal attempt goes through
 *     the policy's own `attempt_started`/`attempt_failed` accounting, and the
 *     driver itself calls `certificate_obtained` after the chain verified and
 *     the certificate owner adopted it.
 *
 * The record getter hands out the driver's bounded record, which the ordinary
 * loop mirrors into the journal when it moves; it carries no nonce, token,
 * key or certificate byte.  NULL means the driver was never configured.
 */

void appliance_production_acme_order_boot_publish(
    struct appliance_production_acme_service *service,
    struct appliance_production_certificate *identity,
    struct appliance_production_acme_http01 *listener);

void appliance_production_acme_order_boot_tick(
    uint32_t now_ms,
    uint32_t now_epoch_seconds);

const char *appliance_production_acme_order_boot_record(void);

/*
 * ITEM-508 increment 37: a bounded `STATE/STEP` rendering for the read-only
 * panel block - "RUNNING/ORDER", "DONE/CERTIFICATE", "FAILED/CHALLENGE" -
 * or NULL before the composition configured.  The full record stays in the
 * journal; the panel value is bounded by
 * `APPLIANCE_MANAGEMENT_WEB_UI_ACME_VALUE_MAX` on the renderer's side.
 */
const char *appliance_production_acme_order_boot_state(void);

#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
/*
 * The wire proof's request: the family asks for exactly one order on the next
 * tick, because it has no operator declaration and no durable request.  The
 * production builds never define the macro, so the entry does not exist there.
 */
void appliance_production_acme_order_boot_fixture_request(void);
#endif

#endif
