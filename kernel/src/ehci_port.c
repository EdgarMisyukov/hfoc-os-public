/*
 * ITEM-563: the root port read path, in production form.
 *
 * See kernel/include/ehci_port.h: the hardware run's `connected=0` was produced
 * by one PORTSC read, before power, without a debounce window, and the value
 * was never kept.  This module reads the port at the address the controller
 * decodes, powers it when the controller says software owns power, settles it,
 * and reports both reads.
 */

#include "ehci_port.h"

uint32_t ehci_port_offset(uint8_t caplength, uint32_t port) {
    if (port == 0U) {
        return (uint32_t)caplength + EHCI_OP_PORTSC;
    }
    return (uint32_t)caplength + EHCI_OP_PORTSC +
           (port - 1U) * EHCI_OP_PORTSC_STRIDE;
}

bool ehci_port_probe(const struct ehci_port_ops *ops,
                     uint8_t caplength,
                     uint32_t port,
                     bool ppc,
                     struct ehci_port_state *state) {
    uint32_t raw;
    uint32_t waited = 0U;
    const uint32_t offset = ehci_port_offset(caplength, port);

    if (ops == NULL || state == NULL || ops->read == NULL ||
        ops->write == NULL || ops->delay_us == NULL || port == 0U) {
        return false;
    }
    *state = (struct ehci_port_state){0};
    raw = ops->read(ops->context, offset);
    state->raw_first = raw;
    state->connected_first = ehci_port_connected(raw);
    /*
     * Port power is software's when HCSPARAMS.PPC is set.  A port that comes
     * out of a controller reset unpowered reads exactly like a port with
     * nothing on it, and that is the difference this probe exists to make.
     */
    if (ppc && (raw & EHCI_PORTSC_PP) == 0U) {
        ops->write(ops->context, offset, ehci_portsc_power_value(raw));
        state->powered = true;
        ops->delay_us(ops->context, EHCI_PORT_POWER_GOOD_US);
        raw = ops->read(ops->context, offset);
    }
    /*
     * A device that is on the port reports its connect after the USB 2.0
     * debounce, and a controller that was just routed or powered may need that
     * window before CCS is meaningful.  The window is bounded and its use is
     * recorded: an empty port says "waited 100000 us", not "no".
     */
    while (!ehci_port_connected(raw) &&
           waited + EHCI_PORT_SETTLE_STEP_US <= EHCI_PORT_SETTLE_WINDOW_US) {
        ops->delay_us(ops->context, EHCI_PORT_SETTLE_STEP_US);
        waited += EHCI_PORT_SETTLE_STEP_US;
        raw = ops->read(ops->context, offset);
        if (ehci_port_connected(raw)) {
            break;
        }
    }
    state->waited_us = waited;
    state->portsc = raw;
    state->connected = ehci_port_connected(raw);
    state->settle_full = !state->connected &&
                         waited + EHCI_PORT_SETTLE_STEP_US >
                             EHCI_PORT_SETTLE_WINDOW_US;
    return true;
}

/* ITEM-567: the root port reset. */
/* Keep the controller's words at one point of the sequence, and remember the
   first point that carried HSE. */
static void ehci_port_sample(const struct ehci_port_ops *ops,
                             uint32_t caplength,
                             struct ehci_port_reset_state *state,
                             enum ehci_port_reset_point point) {
    const uint32_t usbcmd_offset = caplength + EHCI_OP_USBCMD;
    const uint32_t usbsts_offset = caplength + EHCI_OP_USBSTS;
    uint32_t usbsts;

    state->usbcmd[point] = ops->read(ops->context, usbcmd_offset);
    usbsts = ops->read(ops->context, usbsts_offset);
    state->usbsts[point] = usbsts;
    if (state->hse_point == EHCI_PORT_RESET_POINT_NONE &&
        (usbsts & EHCI_USBSTS_HSE) != 0U) {
        state->hse_point = (uint32_t)point;
        state->hse_usbcmd = state->usbcmd[point];
        state->hse_usbsts = usbsts;
    }
}

/* A controller that reported HSE or halted cannot complete a port reset: Table
   2-16 bit 8 says it may hold PR while halted, and section 4.15.2.4 requires an
   HCReset before the controller is used again. */
static bool ehci_port_controller_halted(
    const struct ehci_port_reset_state *state,
    enum ehci_port_reset_point point) {
    return (state->usbsts[point] & (EHCI_USBSTS_HSE | EHCI_USBSTS_HCH)) != 0U;
}

enum ehci_port_reset_result ehci_port_reset_run(
    const struct ehci_port_ops *ops,
    uint8_t caplength,
    uint32_t port,
    bool ppc,
    struct ehci_port_reset_state *state) {
    uint32_t portsc;
    uint32_t waited = 0U;
    const uint32_t offset = ehci_port_offset(caplength, port);

    if (ops == NULL || state == NULL || ops->read == NULL ||
        ops->write == NULL || ops->delay_us == NULL || port == 0U) {
        return EHCI_PORT_RESET_NOT_CONNECTED;
    }
    *state = (struct ehci_port_reset_state){0};
    state->hse_point = EHCI_PORT_RESET_POINT_NONE;
    portsc = ops->read(ops->context, offset);
    if (ppc && (portsc & EHCI_PORTSC_PP) == 0U) {
        /* The port is not powered: a reset of an unpowered port is a request
           to a device that cannot answer.  Power it and give power good. */
        ops->write(ops->context, offset, ehci_portsc_power_value(portsc));
        state->powered = true;
        ops->delay_us(ops->context, EHCI_PORT_POWER_GOOD_US);
        portsc = ops->read(ops->context, offset);
    }
    state->portsc_before = portsc;
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_BEFORE);
    if (!ehci_port_connected(portsc)) {
        state->result = EHCI_PORT_RESET_NOT_CONNECTED;
        return state->result;
    }
    if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_BEFORE)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    /*
     * Assert the reset.  Table 2-16: PR is set on purpose here and on purpose
     * only here, and PE must be written zero in the same word - which is what
     * ehci_portsc_write_value() produces, since it preserves only the port's
     * power and change bits and adds the bits the caller asks to set.
     */
    state->portsc_set = ehci_portsc_write_value(portsc, EHCI_PORTSC_PR);
    ops->write(ops->context, offset, state->portsc_set);
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_AFTER_SET);
    if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_AFTER_SET)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    /* The USB 2.0 reset interval: a wait, not a poll. */
    ops->delay_us(ops->context, EHCI_PORT_RESET_HOLD_US);
    state->portsc_hold = ops->read(ops->context, offset);
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_HOLD);
    if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_HOLD)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    /* Terminate the reset.  This write is the one the specification says the
       controller must obey within 2 ms; it is not a request the driver may
       skip because the bit is "its own". */
    state->portsc_cleared = ehci_portsc_write_value(state->portsc_hold, 0U);
    ops->write(ops->context, offset, state->portsc_cleared);
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_AFTER_CLEAR);
    if (ehci_port_controller_halted(state,
                                    EHCI_PORT_RESET_POINT_AFTER_CLEAR)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    /* The completion bound itself, kept as evidence: a controller that still
       reads PR set here violated the 2 ms bound of Table 2-16. */
    ops->delay_us(ops->context, EHCI_PORT_RESET_SETTLE_US);
    waited = EHCI_PORT_RESET_SETTLE_US;
    portsc = ops->read(ops->context, offset);
    state->portsc_cleared_readback = portsc;
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_SETTLE);
    if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_SETTLE)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_POLL);
    for (;;) {
        if ((portsc & EHCI_PORTSC_PR) == 0U) {
            break;
        }
        if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_POLL)) {
            state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
            return state->result;
        }
        if (!state->cleared_twice) {
            /* The controller latched PR through the completion: drop it once
               more, and only once, before spending the rest of the window.
               This is a write of zero to a running controller - a conformant
               repetition of the terminate write - not a recovery from a
               halted one, which no write can complete. */
            state->portsc_cleared = ehci_portsc_write_value(portsc, 0U);
            ops->write(ops->context, offset, state->portsc_cleared);
            state->cleared_twice = true;
        }
        if (waited + EHCI_PORT_RESET_STEP_US > EHCI_PORT_RESET_WINDOW_US) {
            break;
        }
        ops->delay_us(ops->context, EHCI_PORT_RESET_STEP_US);
        waited += EHCI_PORT_RESET_STEP_US;
        portsc = ops->read(ops->context, offset);
    }
    state->portsc_after = portsc;
    state->waited_us = waited;
    if ((portsc & EHCI_PORTSC_PR) != 0U) {
        state->result = EHCI_PORT_RESET_PR_LATCHED;
        return state->result;
    }
    /*
     * The reset completed.  A high-speed port is enabled by the controller at
     * completion; a controller that is a step behind still gets the rest of the
     * window rather than the single instant the earlier driver sampled.
     */
    while ((portsc & EHCI_PORTSC_PE) == 0U &&
           waited + EHCI_PORT_RESET_STEP_US <= EHCI_PORT_RESET_WINDOW_US) {
        ops->delay_us(ops->context, EHCI_PORT_RESET_STEP_US);
        waited += EHCI_PORT_RESET_STEP_US;
        portsc = ops->read(ops->context, offset);
    }
    state->portsc_after = portsc;
    state->waited_us = waited;
    ehci_port_sample(ops, caplength, state, EHCI_PORT_RESET_POINT_AFTER);
    if (ehci_port_controller_halted(state, EHCI_PORT_RESET_POINT_AFTER)) {
        state->result = EHCI_PORT_RESET_CONTROLLER_HALTED;
        return state->result;
    }
    state->result = (portsc & EHCI_PORTSC_PE) != 0U
                        ? EHCI_PORT_RESET_OK
                        : EHCI_PORT_RESET_NOT_ENABLED;
    return state->result;
}