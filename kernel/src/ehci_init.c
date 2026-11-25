/*
 * ITEM-561: the initialization sequence with the capability and operational
 * spaces kept apart.  See kernel/include/ehci_init.h for what went wrong when
 * they were not: the first operational writes landed at the BAR base because
 * CAPLENGTH had not been read yet, the reset never happened, and the firmware's
 * periodic schedule stayed enabled under the driver.
 */

#include "ehci_init.h"

#include <stddef.h>

const char *ehci_init_cause_string(enum ehci_init_cause cause) {
    switch (cause) {
    case EHCI_INIT_OK:
        return "OK";
    case EHCI_INIT_INVALID:
        return "INVALID";
    case EHCI_INIT_CAPLENGTH:
        return "CAPLENGTH";
    case EHCI_INIT_PORTS:
        return "PORTS";
    case EHCI_INIT_WINDOW:
        return "WINDOW";
    case EHCI_INIT_HALT_FAILED:
        return "HALT_FAILED";
    case EHCI_INIT_RESET_TIMEOUT:
        return "RESET_TIMEOUT";
    case EHCI_INIT_PERIODIC_STOP_TIMEOUT:
        return "PERIODIC_STOP";
    case EHCI_INIT_CONFIGFLAG_TIMEOUT:
        return "CONFIGFLAG";
    case EHCI_INIT_HCH_TIMEOUT:
        return "HCH";
    case EHCI_INIT_ASS_TIMEOUT:
        return "ASS";
    case EHCI_INIT_PERIODIC_ON:
        return "PERIODIC_ON";
    }
    return "UNKNOWN";
}

static bool ehci_init_usable(const struct ehci_init_ops *ops) {
    return ops != NULL && ops->read != NULL && ops->write != NULL &&
           ops->barrier != NULL && ops->deadline != NULL &&
           ops->expired != NULL && ops->pause != NULL;
}

/* One operational register, relative to the CAPLENGTH this run read. */
static uint32_t ehci_init_read(const struct ehci_init_ops *ops,
                               const struct ehci_init_result *result,
                               uint32_t offset) {
    return ops->read(ops->context, result->caplength + offset);
}

static void ehci_init_write(const struct ehci_init_ops *ops,
                            const struct ehci_init_result *result,
                            uint32_t offset,
                            uint32_t value) {
    ops->write(ops->context, result->caplength + offset, value);
}

/* Wait for `(read(offset) & mask) == expected`, bounded. */
static bool ehci_init_wait(const struct ehci_init_ops *ops,
                           const struct ehci_init_result *result,
                           uint32_t offset,
                           uint32_t mask,
                           uint32_t expected,
                           uint32_t budget_us,
                           uint32_t *last) {
    uint64_t deadline = ops->deadline(ops->context, budget_us);
    uint32_t spins = 0U;

    for (;;) {
        const uint32_t value = ehci_init_read(ops, result, offset);

        if (last != NULL) {
            *last = value;
        }
        if ((value & mask) == expected) {
            return true;
        }
        if (deadline != 0U ? ops->expired(ops->context, deadline)
                           : ++spins > 4000000U) {
            return false;
        }
        ops->pause(ops->context);
    }
}

/*
 * The periodic schedule must be off before the driver starts an asynchronous
 * one: PSE is the command bit, PSS is the controller's status that follows it
 * at the next frame boundary, so PSE = 0 and PSS = 1 is a normal transient -
 * it is waited out, not treated as a refusal.  The refusal is only for a PSS
 * that survives the whole budget.
 */
static void ehci_init_periodic_stop(const struct ehci_init_ops *ops,
                                    uint32_t stop_budget_us,
                                    struct ehci_init_result *result) {
    result->pse_was_set =
        (result->usbcmd_after_reset & EHCI_USBCMD_PSE) != 0U;
    result->pss_was_set =
        (result->usbsts_after_reset & EHCI_USBSTS_PSS) != 0U;
    if (!result->pse_was_set && !result->pss_was_set) {
        result->pss_stopped = true;
        return;
    }
    /* The interrupt threshold is the only bit worth keeping; run, periodic and
       the doorbell are written as clear. */
    result->pse_clear_word =
        result->usbcmd_after_reset & EHCI_USBCMD_ITC_MASK;
    ehci_init_write(ops, result, EHCI_OP_USBCMD, result->pse_clear_word);
    result->pse_clear_readback = ehci_init_read(ops, result, EHCI_OP_USBCMD);
    if (!ehci_init_wait(ops, result, EHCI_OP_USBSTS, EHCI_USBSTS_PSS, 0U,
                        stop_budget_us, NULL)) {
        result->cause = EHCI_INIT_PERIODIC_STOP_TIMEOUT;
        return;
    }
    result->pss_stopped = true;
}

void ehci_init_run(const struct ehci_init_ops *ops,
                   uint32_t mmio_bytes,
                   bool supports_64bit,
                   uint32_t periodiclistbase,
                   uint32_t asynclistaddr,
                   uint32_t stop_budget_us,
                   uint32_t reset_budget_us,
                   uint32_t start_budget_us,
                   struct ehci_init_result *result) {
    uint32_t cap;

    if (result == NULL) {
        return;
    }
    *result = (struct ehci_init_result){0};
    result->cause = EHCI_INIT_INVALID;
    if (!ehci_init_usable(ops)) {
        return;
    }
    /* From here on the sequence runs; only a refusal changes the cause. */
    result->cause = EHCI_INIT_OK;

    /*
     * CAPLENGTH first: it is the offset of every operational register this
     * sequence touches.  Reading it after the first ehci_op_* access is exactly
     * the defect this module removes - those accesses landed at the BAR base
     * and the reset was never issued.
     */
    cap = ops->read(ops->context, EHCI_CAP_CAPLENGTH);
    result->caplength = cap & EHCI_CAPLENGTH_MASK;
    if (result->caplength < 0x20U || result->caplength > 0x80U ||
        (result->caplength % 4U) != 0U ||
        result->caplength + EHCI_OP_PORTSC > mmio_bytes) {
        result->cause = EHCI_INIT_CAPLENGTH;
        return;
    }
    result->hcsparams = ops->read(ops->context, EHCI_CAP_HCSPARAMS);
    result->hccparams = ops->read(ops->context, EHCI_CAP_HCCPARAMS);
    result->ports = result->hcsparams & 0x0000000FU;
    if (result->ports == 0U) {
        result->cause = EHCI_INIT_PORTS;
        return;
    }
    if (result->caplength + EHCI_OP_PORTSC +
            result->ports * EHCI_OP_PORTSC_STRIDE >
        mmio_bytes) {
        result->cause = EHCI_INIT_WINDOW;
        return;
    }

    /* 0. This driver polls, and nothing but a masked controller can be
       polled without the controller's own interrupt machinery in the way. */
    result->usbintr_before = ehci_init_read(ops, result, EHCI_OP_USBINTR);
    ehci_init_write(ops, result, EHCI_OP_USBINTR, 0U);
    result->usbintr_after = ehci_init_read(ops, result, EHCI_OP_USBINTR);

    /* 1. Stop, and require the controller to say it did. */
    result->usbcmd_before = ehci_init_read(ops, result, EHCI_OP_USBCMD);
    result->usbsts_before = ehci_init_read(ops, result, EHCI_OP_USBSTS);
    ehci_init_write(ops, result, EHCI_OP_USBCMD, 0U);
    if (!ehci_init_wait(ops, result, EHCI_OP_USBSTS, EHCI_USBSTS_HCH,
                        EHCI_USBSTS_HCH, stop_budget_us, NULL)) {
        result->cause = EHCI_INIT_HALT_FAILED;
        return;
    }
    result->halted_before = true;

    /* 2. Reset, and require the reset bit to clear. */
    ehci_init_write(ops, result, EHCI_OP_USBCMD, EHCI_USBCMD_HCRESET);
    if (!ehci_init_wait(ops, result, EHCI_OP_USBCMD, EHCI_USBCMD_HCRESET,
                        0U, reset_budget_us, NULL)) {
        result->cause = EHCI_INIT_RESET_TIMEOUT;
        return;
    }
    result->usbcmd_after_reset =
        ehci_init_read(ops, result, EHCI_OP_USBCMD);
    result->usbsts_after_reset =
        ehci_init_read(ops, result, EHCI_OP_USBSTS);

    /* 3. The periodic schedule has to be off, and that is waited out. */
    ehci_init_periodic_stop(ops, stop_budget_us, result);
    if (result->cause != EHCI_INIT_OK) {
        return;
    }

    /* 4. Route every port to this controller. */
    result->configflag = ehci_init_read(ops, result, EHCI_OP_CONFIGFLAG);
    ehci_init_write(ops, result, EHCI_OP_CONFIGFLAG,
                    result->configflag | EHCI_CONFIGFLAG_CF);
    if (!ehci_init_wait(ops, result, EHCI_OP_CONFIGFLAG, EHCI_CONFIGFLAG_CF,
                        EHCI_CONFIGFLAG_CF, stop_budget_us,
                        &result->configflag)) {
        result->cause = EHCI_INIT_CONFIGFLAG_TIMEOUT;
        return;
    }

    /* 5. The schedules this driver actually uses. */
    ehci_init_write(ops, result, EHCI_OP_PERIODICLISTBASE, periodiclistbase);
    result->periodiclistbase =
        ehci_init_read(ops, result, EHCI_OP_PERIODICLISTBASE);
    if (supports_64bit) {
        ehci_init_write(ops, result, EHCI_OP_CTRLDSSEGMENT, 0U);
        result->ctrldssegment =
            ehci_init_read(ops, result, EHCI_OP_CTRLDSSEGMENT);
        result->ctrldssegment_written = true;
    }
    ehci_init_write(ops, result, EHCI_OP_ASYNCLISTADDR, asynclistaddr);
    result->asynclistaddr =
        ehci_init_read(ops, result, EHCI_OP_ASYNCLISTADDR);
    /* Publish the ring before the controller is told to walk it. */
    ops->barrier(ops->context);

    /* 6. Start with an explicit word, then require both schedules to report. */
    result->start_word =
        ehci_ring_usbcmd_start(ehci_init_read(ops, result, EHCI_OP_USBCMD));
    ehci_init_write(ops, result, EHCI_OP_USBCMD, result->start_word);
    if (!ehci_init_wait(ops, result, EHCI_OP_USBSTS, EHCI_USBSTS_HCH, 0U,
                        start_budget_us, NULL)) {
        result->cause = EHCI_INIT_HCH_TIMEOUT;
        return;
    }
    if (!ehci_init_wait(ops, result, EHCI_OP_USBSTS, EHCI_USBSTS_ASS,
                        EHCI_USBSTS_ASS, start_budget_us,
                        &result->usbsts_after_start)) {
        result->cause = EHCI_INIT_ASS_TIMEOUT;
        return;
    }
    result->async_on = true;
    result->start_readback = ehci_init_read(ops, result, EHCI_OP_USBCMD);
    /* 7. And confirm the periodic schedule really is off. */
    if (!ehci_init_wait(ops, result, EHCI_OP_USBSTS, EHCI_USBSTS_PSS, 0U,
                        start_budget_us, NULL)) {
        result->cause = EHCI_INIT_PERIODIC_ON;
        return;
    }
    if ((ehci_init_read(ops, result, EHCI_OP_USBCMD) & EHCI_USBCMD_PSE) != 0U) {
        result->cause = EHCI_INIT_PERIODIC_ON;
        return;
    }
    result->periodic_off = true;
    result->cause = EHCI_INIT_OK;
}
