#include "ehci_legacy.h"

#include <stddef.h>

enum ehci_legacy_result ehci_legacy_handoff_run(
    const struct ehci_legacy_ops *ops,
    uint32_t hccparams,
    struct ehci_legacy_state *state) {
    uint32_t offset;
    uint32_t capability = 0U;
    uint32_t next;
    bool found = false;

    if (ops == NULL || state == NULL || ops->read32 == NULL ||
        ops->write32 == NULL || ops->delay_us == NULL) {
        return EHCI_LEGACY_ABSENT;
    }
    *state = (struct ehci_legacy_state){0};
    state->eecp = (hccparams >> 8U) & 0xFFU;
    if (state->eecp < EHCI_LEGACY_EECP_MIN ||
        state->eecp + 8U > EHCI_LEGACY_CONFIG_BYTES ||
        (state->eecp % 4U) != 0U) {
        state->result = EHCI_LEGACY_ABSENT;
        return state->result;
    }
    /* The chain is walked by configuration-space byte offsets, with the
       pointer required to move forward and stay in the standard space. */
    offset = state->eecp;
    for (uint32_t step = 0U; step < EHCI_LEGACY_MAX_ENTRIES; ++step) {
        if (offset + 8U > EHCI_LEGACY_CONFIG_BYTES) {
            break;
        }
        capability = ops->read32(ops->context, (uint16_t)offset);
        if (state->cap_count < EHCI_LEGACY_CAPS_KEPT) {
            state->cap_offset[state->cap_count] = (uint16_t)offset;
            state->caps[state->cap_count] = capability;
            state->cap_count += 1U;
        }
        if ((capability & 0xFFU) == EHCI_LEGACY_CAPABILITY_ID) {
            found = true;
            break;
        }
        next = (capability >> 8U) & 0xFFU;
        if (next == 0U || next <= offset ||
            next + 8U > EHCI_LEGACY_CONFIG_BYTES) {
            break;
        }
        offset = next;
    }
    if (!found) {
        state->result = EHCI_LEGACY_NOT_FOUND;
        return state->result;
    }
    state->legacy_offset = (uint16_t)offset;
    state->sup_before = capability;
    if ((capability & EHCI_LEGACY_BIOS_OWNED) != 0U) {
        state->sup_written = capability | EHCI_LEGACY_OS_OWNED;
        ops->write32(ops->context, state->legacy_offset, state->sup_written);
        for (;;) {
            capability = ops->read32(ops->context, state->legacy_offset);
            if ((capability & EHCI_LEGACY_BIOS_OWNED) == 0U) {
                break;
            }
            if (state->waited_us + EHCI_LEGACY_STEP_US >
                EHCI_LEGACY_BUDGET_US) {
                break;
            }
            ops->delay_us(ops->context, EHCI_LEGACY_STEP_US);
            state->waited_us += EHCI_LEGACY_STEP_US;
        }
        if ((capability & EHCI_LEGACY_BIOS_OWNED) != 0U) {
            /* The firmware did not let go.  Force the semaphore clear and keep
               the timeout as the recorded verdict: hiding it would make the
               next run's refusal unreadable. */
            ops->write32(ops->context, state->legacy_offset,
                         capability & ~EHCI_LEGACY_BIOS_OWNED);
            capability = ops->read32(ops->context, state->legacy_offset);
            state->forced = true;
            state->result = EHCI_LEGACY_TIMEOUT;
        } else {
            state->result = EHCI_LEGACY_TAKEN;
        }
    } else {
        state->result = EHCI_LEGACY_NOT_OWNED;
    }
    state->sup_after = capability;
    /* The OS owns the controller now, so the legacy SMI enables must go: the
       firmware must not be able to take it back through an SMI .  The status
       half of the register is left as read (its bits are write-one-to-clear,
       and writing zero leaves them alone). */
    state->ctl_before = ops->read32(ops->context,
                                    (uint16_t)(state->legacy_offset + 4U));
    ops->write32(ops->context, (uint16_t)(state->legacy_offset + 4U),
                 state->ctl_before & ~EHCI_LEGACY_CTL_ENABLES);
    state->ctl_after = ops->read32(ops->context,
                                   (uint16_t)(state->legacy_offset + 4U));
    return state->result;
}
