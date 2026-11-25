#include "appliance_production_acme_order_boot.h"

#ifdef HFOC_PRODUCTION_TLS

#include "appliance_acme_control.h"
#include "appliance_acme_renewal.h"
#include "appliance_acme_transport_lwip.h"
#include "appliance_acme_trust.h"
#include "appliance_network_wire.h"
#include "appliance_production_acme_order.h"
#include "appliance_production_network_wire.h"
#include "appliance_production_serving.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * ITEM-508 increment 35: the order path ordinary boot runs.  This module is
 * the caller the driver deliberately does not have: it owns the one binding,
 * decides when an order is wanted, spends the operator's durable request and
 * keeps the renewal policy's attempt accounting.  The driver underneath owns
 * the order itself.
 */

/*
 * The declared production endpoint.  The driver takes the URL as a parameter,
 * so a proof passes the stub's URL instead; this is the one production literal.
 * The QEMU proof family compiles the stub's URL and port in its place and
 * asks for exactly one order through the fixture entry below, because the
 * family has no validated wall clock of its own and no operator request to
 * spend - the durable request path is proven by the increment-34 host proof.
 */
#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
#define APPLIANCE_PRODUCTION_ACME_ORDER_BOOT_DIRECTORY_URL \
    HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_URL
#else
#define APPLIANCE_PRODUCTION_ACME_ORDER_BOOT_DIRECTORY_URL \
    "https://acme-v02.api.letsencrypt.org/directory"
#endif

static struct appliance_production_acme_service *boot_service;
static struct appliance_production_certificate *boot_identity;
static struct appliance_production_acme_http01 *boot_listener;
static bool boot_published;

static struct appliance_production_acme_order boot_order;
static struct appliance_acme_transport_lwip boot_binding;
static bool boot_configured;
/* One attempt per request (or per renewal decision); cleared when it ends. */
static bool boot_attempted;
static bool boot_renewal_mode;
#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
/* The proof family asks for exactly one order; the flag is spent on that
 * attempt.  The durable request path is proven by the increment-34 host
 * proof, so the wire proof does not need an operator record. */
static bool boot_fixture_requested;

void appliance_production_acme_order_boot_fixture_request(void)
{
    boot_fixture_requested = true;
}
#endif

static bool appliance_production_acme_order_boot_listening(void *context)
{
    (void)context;
    return boot_listener != NULL &&
           appliance_production_acme_http01_listening(boot_listener);
}

static enum appliance_acme_challenge_status
appliance_production_acme_order_boot_arm(
    void *context,
    const struct appliance_acme_flow *flow,
    const struct appliance_acme_key *key,
    uint32_t now_epoch_seconds)
{
    (void)context;
    if (boot_listener == NULL) {
        return APPLIANCE_ACME_CHALLENGE_BAD_STATE;
    }
    return appliance_production_acme_http01_arm_flow(
        boot_listener, flow, key, now_epoch_seconds);
}

static void appliance_production_acme_order_boot_disarm(void *context)
{
    (void)context;
    if (boot_listener != NULL) {
        appliance_production_acme_http01_disarm(boot_listener);
    }
}

static const struct appliance_production_acme_order_listener
    appliance_production_acme_order_boot_listener = {
        .context = NULL,
        .listening = appliance_production_acme_order_boot_listening,
        .arm_flow = appliance_production_acme_order_boot_arm,
        .disarm = appliance_production_acme_order_boot_disarm,
    };

void appliance_production_acme_order_boot_publish(
    struct appliance_production_acme_service *service,
    struct appliance_production_certificate *identity,
    struct appliance_production_acme_http01 *listener)
{
    if (service == NULL || identity == NULL || listener == NULL) {
        return;
    }
    boot_service = service;
    boot_identity = identity;
    boot_listener = listener;
    boot_published = true;
}

static void appliance_production_acme_order_boot_configure(void)
{
    const struct appliance_network_wire *wire =
        appliance_production_network_wire_boot_wire();
    const struct appliance_network_lifecycle *lifecycle =
        wire != NULL ? appliance_network_wire_lifecycle(wire) : NULL;
    const struct appliance_acme_transport_link *link;
    struct appliance_acme_renewal *renewal;

    if (lifecycle == NULL) {
        return;
    }
    if (appliance_acme_transport_lwip_init(
            &boot_binding, (struct appliance_network_lifecycle *)lifecycle,
            appliance_acme_trust_anchor_der_data(),
            appliance_acme_trust_anchor_der_length()) !=
        APPLIANCE_ACME_TRANSPORT_LWIP_OK) {
        return;
    }
    link = appliance_acme_transport_lwip_link(&boot_binding);
    renewal = appliance_production_acme_service_renewal(boot_service);
    if (link == NULL || renewal == NULL) {
        return;
    }
    appliance_production_acme_order_init(&boot_order);
    if (appliance_production_acme_order_configure(
            &boot_order, link, boot_identity, renewal,
            &appliance_production_acme_order_boot_listener,
            appliance_acme_trust_anchor_der_data(),
            appliance_acme_trust_anchor_der_length(),
            APPLIANCE_PRODUCTION_ACME_ORDER_BOOT_DIRECTORY_URL) !=
        APPLIANCE_PRODUCTION_ACME_ORDER_OK) {
        return;
    }
    boot_configured = true;
}

static uint32_t appliance_production_acme_order_boot_identifiers(
    const char **names,
    size_t *lengths,
    uint32_t capacity)
{
    uint32_t count = 0U;
    uint32_t index;

    for (index = 0U; index < capacity; ++index) {
        const char *name = NULL;
        size_t name_length = 0U;

        if (!appliance_production_serving_acme_identifier(
                index, &name, &name_length)) {
            break;
        }
        names[count] = name;
        lengths[count] = name_length;
        ++count;
    }
    return count;
}

void appliance_production_acme_order_boot_tick(
    uint32_t now_ms,
    uint32_t now_epoch_seconds)
{
    const struct appliance_acme_control *control;
    struct appliance_acme_renewal *renewal;
    bool requested;
    bool durable_requested;
    bool listener_ready;

    if (!boot_published) {
        return;
    }
    if (!boot_configured) {
        appliance_production_acme_order_boot_configure();
        if (!boot_configured) {
            return;
        }
    }
    control = appliance_production_serving_acme_control();
    durable_requested =
        control != NULL && appliance_acme_control_enabled(control) &&
        appliance_acme_control_order_requested(control);
    requested = durable_requested;
#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
    if (boot_fixture_requested) {
        requested = true;
    }
#endif
    listener_ready = appliance_production_acme_http01_listening(boot_listener);
    if (appliance_production_acme_order_state(&boot_order) ==
        APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING) {
        (void)appliance_production_acme_order_tick(&boot_order, now_ms);
        return;
    }
    renewal = appliance_production_acme_service_renewal(boot_service);
    /*
     * A finished attempt is accounted once: a failed renewal releases the
     * policy's retry window, a failed manual request simply ends (the
     * operator's request was already spent when the order started).
     */
    if (boot_attempted) {
        if (boot_renewal_mode && renewal != NULL &&
            appliance_production_acme_order_state(&boot_order) ==
                APPLIANCE_PRODUCTION_ACME_ORDER_STATE_FAILED) {
            (void)appliance_acme_renewal_attempt_failed(
                renewal, now_epoch_seconds);
        }
        boot_attempted = false;
        boot_renewal_mode = false;
    }
    if (now_epoch_seconds == 0U || !listener_ready ||
        !appliance_production_certificate_available(boot_identity)) {
        /* No validated clock, no listener or no prepared account identity:
         * nothing starts; the next turn retries, and the operator's request
         * stays pending.  An order whose account key does not exist yet would
         * only refuse `KEY_REFUSED`, so it is not attempted at all. */
        return;
    }
    if (!requested) {
        if (renewal == NULL ||
            appliance_acme_renewal_evaluate(renewal, now_epoch_seconds) !=
                APPLIANCE_ACME_RENEWAL_DECISION_DUE ||
            appliance_acme_renewal_attempt_started(
                renewal, now_epoch_seconds) != APPLIANCE_ACME_RENEWAL_OK) {
            return;
        }
        boot_renewal_mode = true;
    }
    {
        const char *names[APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX];
        size_t lengths[APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX];
        uint32_t count = appliance_production_acme_order_boot_identifiers(
            names, lengths, APPLIANCE_PRODUCTION_ACME_ORDER_IDENTIFIER_MAX);

#ifdef HFOC_APPLIANCE_ACME_ORDER_BOOT_QEMU_TEST
        /*
         * The proof family has no operator declaration and no durable request;
         * it orders for the one name the stub's chain is for, and spends its
         * one request here so a later turn does not order again.
         */
        if (count == 0U && boot_fixture_requested) {
            names[0] = "hfoc-appliance";
            lengths[0] = 14U;
            count = 1U;
        }
        boot_fixture_requested = false;
#endif
        boot_attempted = true;
        (void)appliance_production_acme_order_start(
            &boot_order, names, lengths, count, now_epoch_seconds);
        if (durable_requested) {
            /*
             * The operator's request is spent the moment an order starts - or
             * refuses to start with a named record; either way the next boot
             * must not resurrect it.
             */
            (void)appliance_production_serving_acme_take_request();
        }
        if (boot_renewal_mode &&
            appliance_production_acme_order_state(&boot_order) !=
                APPLIANCE_PRODUCTION_ACME_ORDER_STATE_RUNNING) {
            (void)appliance_acme_renewal_attempt_failed(
                renewal, now_epoch_seconds);
        }
    }
}

const char *appliance_production_acme_order_boot_record(void)
{
    return boot_configured ? appliance_production_acme_order_record(&boot_order)
                           : NULL;
}

const char *appliance_production_acme_order_boot_state(void)
{
    static char state_text[40];
    const char *state;
    const char *stage;
    size_t length = 0U;

    if (!boot_configured) {
        return NULL;
    }
    state = appliance_production_acme_order_state_string(
        appliance_production_acme_order_state(&boot_order));
    stage = appliance_production_acme_order_stage_string(
        appliance_production_acme_order_stage(&boot_order));
    while (state[length] != '\0' && length + 1U < sizeof(state_text)) {
        state_text[length] = state[length];
        ++length;
    }
    if (length + 1U < sizeof(state_text)) {
        state_text[length] = '/';
        ++length;
    }
    {
        size_t index = 0U;

        while (stage[index] != '\0' && length + 1U < sizeof(state_text)) {
            state_text[length] = stage[index];
            ++length;
            ++index;
        }
    }
    state_text[length] = '\0';
    return state_text;
}

#else

/*
 * Every kernel build compiles this wildcard source, and only the TLS-serving
 * build defines the macro: the guard keeps the other translation units
 * non-empty and warning-free.
 */
typedef int appliance_production_acme_order_boot_translation_unit_guard;

#endif
