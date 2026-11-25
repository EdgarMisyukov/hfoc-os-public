/*
 * ITEM-559/564/566: the asynchronous schedule's own verdict, with MMIO and time
 * substituted.  See kernel/include/ehci_engine.h for why the descriptor
 * classifier alone cannot name a controller that halted under the transfer, and
 * why the stop's verdict is kept apart from the transfer's.
 *
 * ITEM-566: two rules decide the pool's safety, and both are now measured
 * rather than inferred:
 *
 *   * USBSTS.ASS follows USBCMD.ASE at frame boundaries (EHCI 1.0 section
 *     4.8/2.3.2), so a schedule that was just enabled reads "not running" for
 *     up to a frame.  The transfer wait therefore gives ASE=1/ASS=0 a bounded
 *     transition window before it names the schedule stopped;
 *   * a retired descriptor chain is not evidence that the controller has left
 *     the queue head.  The pool may only be reused after the controller
 *     confirmed it (an acknowledged async-advance doorbell) or after it was
 *     stopped and HCHalted was read back.  An unanswered doorbell, a refused
 *     one and a schedule that is simply not walking our list all end in that
 *     stop-and-confirm path; only its success clears the stall.
 */

#include "ehci_engine.h"

#include <stddef.h>

const char *ehci_engine_cause_string(enum ehci_engine_cause cause) {
    switch (cause) {
    case EHCI_ENGINE_CAUSE_NONE:
        return "NONE";
    case EHCI_ENGINE_CAUSE_WAIT_EXPIRED:
        return "WAIT_EXPIRED";
    case EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR:
        return "HSE";
    case EHCI_ENGINE_CAUSE_HALTED:
        return "HALTED";
    case EHCI_ENGINE_CAUSE_ASYNC_STOPPED:
        return "ASYNC_STOPPED";
    case EHCI_ENGINE_CAUSE_ASYNC_ADVANCE:
        return "ASYNC_ADVANCE";
    case EHCI_ENGINE_CAUSE_ENGINE_STALLED:
        return "STALLED";
    }
    return "UNKNOWN";
}

const char *ehci_engine_cleanup_string(enum ehci_engine_cleanup cleanup) {
    switch (cleanup) {
    case EHCI_ENGINE_CLEANUP_NONE:
        return "NONE";
    case EHCI_ENGINE_CLEANUP_ACKNOWLEDGED:
        return "ACKNOWLEDGED";
    case EHCI_ENGINE_CLEANUP_NOT_RUNNING:
        return "NOT_RUNNING";
    case EHCI_ENGINE_CLEANUP_REFUSED:
        return "REFUSED";
    case EHCI_ENGINE_CLEANUP_IGNORED:
        return "IGNORED";
    }
    return "UNKNOWN";
}

static bool ehci_engine_usable(const struct ehci_engine_ops *ops) {
    return ops != NULL && ops->read != NULL && ops->write != NULL &&
           ops->qtd_token != NULL && ops->link_transfer_head != NULL &&
           ops->barrier != NULL && ops->deadline != NULL &&
           ops->expired != NULL && ops->pause != NULL;
}

/*
 * The controller's state as its two words describe it.  The order is the order
 * of precedence: HSE explains everything after it, a halted controller explains
 * a stopped schedule, and only a schedule whose enable bit is clear is a
 * commanded stop.  `ASE=1, ASS=0` is deliberately *not* here: that is the
 * transition the wait gives its own window (ITEM-566).
 */
static enum ehci_engine_cause ehci_engine_state_cause(uint32_t usbcmd,
                                                      uint32_t usbsts) {
    if ((usbsts & EHCI_USBSTS_HSE) != 0U) {
        return EHCI_ENGINE_CAUSE_HOST_SYSTEM_ERROR;
    }
    if ((usbsts & EHCI_USBSTS_HCH) != 0U &&
        (usbcmd & EHCI_USBCMD_RS) == 0U) {
        return EHCI_ENGINE_CAUSE_HALTED;
    }
    if ((usbcmd & EHCI_USBCMD_ASE) == 0U) {
        return EHCI_ENGINE_CAUSE_ASYNC_STOPPED;
    }
    return EHCI_ENGINE_CAUSE_NONE;
}

static void ehci_engine_snapshot(const struct ehci_engine_ops *ops,
                                 struct ehci_engine_result *result) {
    result->usbcmd = ops->read(ops->context, EHCI_OP_USBCMD);
    result->usbsts = ops->read(ops->context, EHCI_OP_USBSTS);
    result->portsc = ops->portsc_offset != 0U
                         ? ops->read(ops->context, ops->portsc_offset)
                         : 0U;
}

/* Wait for `mask` to appear in USBSTS, bounded.  Returns the last word read and
 * whether the bit appeared. */
static bool ehci_engine_wait_status(const struct ehci_engine_ops *ops,
                                    uint32_t mask,
                                    uint32_t budget_us,
                                    uint32_t *last) {
    const uint64_t deadline = ops->deadline(ops->context, budget_us);
    uint32_t spins = 0U;

    for (;;) {
        const uint32_t status = ops->read(ops->context, EHCI_OP_USBSTS);

        if (last != NULL) {
            *last = status;
        }
        if ((status & mask) == mask) {
            return true;
        }
        if (deadline != 0U ? ops->expired(ops->context, deadline)
                           : ++spins > EHCI_ENGINE_SPIN_WAIT) {
            return false;
        }
        ops->pause(ops->context);
    }
}

/*
 * Confirm that the controller is out of the pool's way: command it to stop
 * (keeping only the interrupt threshold), require HCHalted, then re-arm the
 * schedule this driver uses and require ASS to come back.  The re-arm matters:
 * the walk keeps looking after a failed port, and a halted controller would
 * refuse every later transfer.
 */
static bool ehci_engine_stop_and_confirm(const struct ehci_engine_ops *ops,
                                         uint32_t budget_us,
                                         uint32_t *usbcmd_after,
                                         uint32_t *usbsts_after,
                                         bool *restarted,
                                         bool *restart_confirmed) {
    const uint32_t word = ops->read(ops->context, EHCI_OP_USBCMD);
    uint32_t status = 0U;

    ops->write(ops->context, EHCI_OP_USBCMD,
               word & EHCI_USBCMD_ITC_MASK);
    ops->barrier(ops->context);
    if (!ehci_engine_wait_status(ops, EHCI_USBSTS_HCH, budget_us, &status)) {
        if (usbcmd_after != NULL) {
            *usbcmd_after = ops->read(ops->context, EHCI_OP_USBCMD);
        }
        if (usbsts_after != NULL) {
            *usbsts_after = status;
        }
        return false;
    }
    /*
     * ITEM-566: a doorbell the controller processed after our wait may have
     * raised IAA.  QEMU, in particular, stops advancing the asynchronous
     * schedule while IAA is set, so a halt/re-arm that leaves the bit behind
     * makes every later transfer time out.  Clear the stale acknowledgement
     * before re-arming; it belongs to no transfer that is still in flight.
     */
    ops->write(ops->context, EHCI_OP_USBSTS, EHCI_USBSTS_IAA);
    ops->barrier(ops->context);
    if (restarted != NULL) {
        *restarted = true;
    }
    ops->write(ops->context, EHCI_OP_USBCMD,
               (word & EHCI_USBCMD_ITC_MASK) | EHCI_USBCMD_RS |
                   EHCI_USBCMD_ASE);
    ops->barrier(ops->context);
    if (restart_confirmed != NULL) {
        *restart_confirmed =
            ehci_engine_wait_status(ops, EHCI_USBSTS_ASS, budget_us, &status);
    }
    if (usbcmd_after != NULL) {
        *usbcmd_after = ops->read(ops->context, EHCI_OP_USBCMD);
    }
    if (usbsts_after != NULL) {
        *usbsts_after = status;
    }
    return true;
}

void ehci_engine_run(const struct ehci_engine_ops *ops,
                     uint32_t qtd_count,
                     uint32_t budget_us,
                     uint32_t iaa_budget_us,
                     bool engine_stalled,
                     struct ehci_engine_result *result) {
    uint32_t tokens[EHCI_ENGINE_MAX_QTD];
    uint32_t spins = 0U;
    uint64_t deadline = 0U;
    uint64_t ass_deadline = 0U;
    uint32_t ass_waited_us = 0U;

    if (result == NULL) {
        return;
    }
    result->transfer = EHCI_TRANSFER_OTHER;
    result->cause = EHCI_ENGINE_CAUSE_NONE;
    result->failed_index = 0U;
    result->usbcmd = 0U;
    result->usbsts = 0U;
    result->portsc = 0U;
    result->cleanup = EHCI_ENGINE_CLEANUP_NONE;
    result->doorbell = 0U;
    result->doorbell_readback = 0U;
    result->usbcmd_after = 0U;
    result->usbsts_after = 0U;
    result->usbcmd_before_link = 0U;
    result->usbsts_before_link = 0U;
    result->usbcmd_first_refusal = 0U;
    result->usbsts_first_refusal = 0U;
    result->ass_waited_us = 0U;
    result->stale_iaa = false;
    result->schedule_running = false;
    result->ass_seen = false;
    result->halted_confirmed = false;
    result->restarted = false;
    result->restart_confirmed = false;
    result->quiesced = false;
    result->stalled = false;

    if (!ehci_engine_usable(ops) || qtd_count == 0U ||
        qtd_count > EHCI_ENGINE_MAX_QTD) {
        return;
    }
    if (engine_stalled) {
        result->cause = EHCI_ENGINE_CAUSE_ENGINE_STALLED;
        result->stalled = true;
        return;
    }

    /* ITEM-566: an IAA left by an earlier stop is not this transfer's answer.
       Clear it before the head is linked; QEMU will not advance the async
       schedule while it is set, so a leftover bit would stall every transfer. */
    {
        const uint32_t initial_status =
            ops->read(ops->context, EHCI_OP_USBSTS);

        if ((initial_status & EHCI_USBSTS_IAA) != 0U) {
            result->stale_iaa = true;
            ops->write(ops->context, EHCI_OP_USBSTS, EHCI_USBSTS_IAA);
            ops->barrier(ops->context);
        }
        result->usbsts_before_link = initial_status;
    }
    /* The state the transfer was started from, before the head is linked, so a
       schedule that stopped earlier cannot be blamed on the transfer and a
       schedule that stops later is visible as a change. */
    result->usbcmd_before_link = ops->read(ops->context, EHCI_OP_USBCMD);

    ops->link_transfer_head(ops->context, true);
    ops->barrier(ops->context);

    deadline = ops->deadline(ops->context, budget_us);
    for (;;) {
        const uint32_t token = ops->qtd_token(ops->context, qtd_count - 1U);
        const uint32_t usbcmd = ops->read(ops->context, EHCI_OP_USBCMD);
        const uint32_t usbsts = ops->read(ops->context, EHCI_OP_USBSTS);
        enum ehci_engine_cause cause;

        if ((token & EHCI_QTD_STATUS_ACTIVE) == 0U) {
            break;
        }
        cause = ehci_engine_state_cause(usbcmd, usbsts);
        if (cause != EHCI_ENGINE_CAUSE_NONE) {
            result->cause = cause;
            result->usbcmd_first_refusal = usbcmd;
            result->usbsts_first_refusal = usbsts;
            break;
        }
        /*
         * ITEM-566: ASE set and ASS clear is the schedule's start-up
         * transition (USBSTS follows USBCMD at a frame boundary), not a stop.
         * It is given a bounded window; only a schedule that stays not-running
         * for the whole window is named stopped.
         */
        if ((usbsts & EHCI_USBSTS_ASS) != 0U) {
            result->ass_seen = true;
            ass_deadline = 0U;
            ass_waited_us = 0U;
        } else if (ass_deadline == 0U) {
            ass_deadline = ops->deadline(ops->context,
                                         EHCI_ENGINE_ASS_TRANSITION_US);
        } else if (ops->expired(ops->context, ass_deadline)) {
            result->cause = EHCI_ENGINE_CAUSE_ASYNC_STOPPED;
            result->usbcmd_first_refusal = usbcmd;
            result->usbsts_first_refusal = usbsts;
            ass_waited_us = EHCI_ENGINE_ASS_TRANSITION_US;
            break;
        }
        if (deadline != 0U ? ops->expired(ops->context, deadline)
                           : ++spins > EHCI_ENGINE_SPIN_WAIT) {
            result->cause = EHCI_ENGINE_CAUSE_WAIT_EXPIRED;
            result->usbcmd_first_refusal = usbcmd;
            result->usbsts_first_refusal = usbsts;
            break;
        }
        ops->pause(ops->context);
    }
    result->ass_waited_us = ass_waited_us;

    /*
     * The snapshot is taken after the wait and before the unlink, so it
     * describes the failure and not the stop.
     */
    ehci_engine_snapshot(ops, result);

    for (uint32_t index = 0U; index < qtd_count; ++index) {
        tokens[index] = ops->qtd_token(ops->context, index);
    }
    result->transfer = ehci_classify_qtds(tokens, qtd_count,
                                          &result->failed_index);
    if (result->cause != EHCI_ENGINE_CAUSE_NONE &&
        result->transfer == EHCI_TRANSFER_OK) {
        /* The schedule failed; the descriptor's verdict cannot be "complete"
           even when the last descriptor happened to retire. */
        result->transfer = EHCI_TRANSFER_OTHER;
    }

    /*
     * The stop.  The head is removed first, then the controller is asked to
     * confirm that it has left it - by the async-advance doorbell when the
     * schedule is walking the list, or by an acknowledged halt when it is not.
     * A descriptor chain that retired says nothing about that, which is why the
     * doorbell is rung even for a completed transfer.
     */
    result->schedule_running =
        (result->usbcmd & EHCI_USBCMD_ASE) != 0U &&
        (result->usbsts & EHCI_USBSTS_ASS) != 0U &&
        (result->usbsts & (EHCI_USBSTS_HSE | EHCI_USBSTS_HCH)) == 0U;
    ops->link_transfer_head(ops->context, false);
    ops->barrier(ops->context);

    if (result->schedule_running) {
        const uint32_t status = ops->read(ops->context, EHCI_OP_USBSTS);

        if ((status & EHCI_USBSTS_IAA) != 0U) {
            result->stale_iaa = true;
            ops->write(ops->context, EHCI_OP_USBSTS, EHCI_USBSTS_IAA);
        }
        result->doorbell = ops->read(ops->context, EHCI_OP_USBCMD) |
                           EHCI_USBCMD_IAAD;
        ops->write(ops->context, EHCI_OP_USBCMD, result->doorbell);
        result->doorbell_readback =
            ops->read(ops->context, EHCI_OP_USBCMD);
        {
            /*
             * ITEM-566: the read-back is not the verdict.  A controller may
             * accept the doorbell, clear the bit immediately and raise IAA on
             * a later frame (QEMU does exactly this), so the bounded wait is
             * kept for every write.  IAA is the confirmation; a bit that is
             * still set but never answered is IGNORED, and a bit that did not
             * stick and was never answered is REFUSED.
             */
            const bool accepted =
                (result->doorbell_readback & EHCI_USBCMD_IAAD) != 0U;
            const uint64_t iaa_deadline =
                ops->deadline(ops->context, iaa_budget_us);
            uint32_t iaa_spins = 0U;
            uint32_t status = 0U;
            bool acknowledged = false;

            for (;;) {
                status = ops->read(ops->context, EHCI_OP_USBSTS);
                if ((status & EHCI_USBSTS_IAA) != 0U) {
                    ops->write(ops->context, EHCI_OP_USBSTS,
                               EHCI_USBSTS_IAA);
                    acknowledged = true;
                    break;
                }
                if (iaa_deadline != 0U
                        ? ops->expired(ops->context, iaa_deadline)
                        : ++iaa_spins > EHCI_ENGINE_SPIN_WAIT) {
                    break;
                }
                ops->pause(ops->context);
            }
            result->usbsts_after = status;
            if (acknowledged) {
                result->cleanup = EHCI_ENGINE_CLEANUP_ACKNOWLEDGED;
                result->quiesced = true;
            } else if (accepted) {
                result->cleanup = EHCI_ENGINE_CLEANUP_IGNORED;
            } else {
                result->cleanup = EHCI_ENGINE_CLEANUP_REFUSED;
            }
        }
    } else {
        /* The controller is not walking our list, so there is no advance to
           wait for - but "not walking" is a command/status reading, not a
           confirmation that it has left the queue head. */
        result->cleanup = EHCI_ENGINE_CLEANUP_NOT_RUNNING;
    }

    if (!result->quiesced) {
        /*
         * No advance was confirmed.  The pool may not be reused until the
         * controller is stopped and says so: command the halt, require
         * HCHalted, then re-arm the schedule for the transfers that follow.
         */
        result->halted_confirmed = ehci_engine_stop_and_confirm(
            ops, iaa_budget_us, &result->usbcmd_after, &result->usbsts_after,
            &result->restarted, &result->restart_confirmed);
        result->quiesced = result->halted_confirmed;
    }
    if (!result->quiesced) {
        /* Nothing confirmed that the controller left the head: the pool stays
           out of use, and this is the stop's verdict, not the transfer's. */
        result->stalled = true;
    }
    if (result->usbcmd_after == 0U) {
        result->usbcmd_after = ops->read(ops->context, EHCI_OP_USBCMD);
    }
    if (result->usbsts_after == 0U) {
        result->usbsts_after = ops->read(ops->context, EHCI_OP_USBSTS);
    }
}
