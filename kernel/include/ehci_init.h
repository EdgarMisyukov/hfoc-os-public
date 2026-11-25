#ifndef HFOC_EHCI_INIT_H
#define HFOC_EHCI_INIT_H

/*
 * ITEM-561: the controller initialization sequence, over substituted MMIO and
 * clock.
 *
 * The run of ITEM-560 stopped earlier, with RESET_TIMEOUT/PERIODIC_ON, before
 * any transfer: the driver refused to start a controller whose periodic
 * schedule bit was still set.  Reading the sequence showed why the refusal was
 * inevitable and why the earlier HSE was possible at all: the driver performed
 * its first operational-register accesses (USBINTR, USBCMD, USBSTS, HCRESET)
 * *before* it had read CAPLENGTH, so `ehci_op_*` added a zero offset and those
 * accesses landed in the capability space (BAR+0x00/0x04/0x08).  The reset was
 * never issued, the firmware's USBCMD (PSE = 1) survived, and the driver then
 * started a schedule it had not configured - the Host System Error of the
 * earlier runs.  The new PERIODIC_ON guard correctly refused that state, but
 * for the wrong reason: it could not have succeeded.
 *
 * This module owns the register sequence end to end, with the capability space
 * (CAPLENGTH, HCSPARAMS, HCCPARAMS) and the operational space distinguished:
 *
 *   1. read and validate CAPLENGTH from the capability space - before any
 *      operational access, because every later offset depends on it;
 *   2. stop: write USBCMD = 0 and require HCHalted = 1 (bounded);
 *   3. reset: write HCRESET and require the bit to clear (bounded);
 *   4. periodic stop: if PSE or PSS is still set after the reset, write the
 *      command word with PSE clear and wait (bounded) for PSS to clear - a
 *      status bit that follows the command at the next frame boundary, not
 *      immediately;
 *   5. CONFIGFLAG = 1 (bounded);
 *   6. schedules: PERIODICLISTBASE = 0 (the driver builds no periodic
 *      schedule), CTRLDSSEGMENT = 0 when the controller is 64-bit capable,
 *      ASYNCLISTADDR = the ring head, published with a barrier;
 *   7. start: write the explicit start word (threshold kept, PSE and the
 *      async-advance doorbell clear, run + async set), require HCHalted = 0
 *      and ASS = 1 (bounded), then confirm PSE = 0 and PSS = 0 (bounded).
 *
 * Every value read before a write, the word written, the read-back and the
 * post-wait state are returned, so a refusal names its stage and the record
 * keeps the evidence.
 */

#include <stdbool.h>
#include <stdint.h>

#include "ehci.h"

enum ehci_init_cause {
    EHCI_INIT_OK = 0,
    EHCI_INIT_INVALID,
    EHCI_INIT_CAPLENGTH,
    EHCI_INIT_PORTS,
    EHCI_INIT_WINDOW,
    EHCI_INIT_HALT_FAILED,
    EHCI_INIT_RESET_TIMEOUT,
    EHCI_INIT_PERIODIC_STOP_TIMEOUT,
    EHCI_INIT_CONFIGFLAG_TIMEOUT,
    EHCI_INIT_HCH_TIMEOUT,
    EHCI_INIT_ASS_TIMEOUT,
    EHCI_INIT_PERIODIC_ON
};

struct ehci_init_ops {
    void *context;
    /* One dword at an absolute offset from the BAR base. */
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void (*barrier)(void *context);
    uint64_t (*deadline)(void *context, uint32_t microseconds);
    bool (*expired)(void *context, uint64_t deadline);
    void (*pause)(void *context);
};

struct ehci_init_result {
    enum ehci_init_cause cause;
    /* Capability space: read first, and every operational offset below is
       relative to it. */
    uint32_t caplength;
    uint32_t hcsparams;
    uint32_t hccparams;
    uint32_t ports;
    uint32_t usbintr_before;
    uint32_t usbintr_after;
    /* Stop. */
    uint32_t usbcmd_before;
    uint32_t usbsts_before;
    bool halted_before;
    /* Reset. */
    uint32_t usbcmd_after_reset;
    uint32_t usbsts_after_reset;
    /* Periodic stop: PSE is the command bit, PSS the status bit that follows
       it at the next frame boundary. */
    bool pse_was_set;
    bool pss_was_set;
    uint32_t pse_clear_word;
    uint32_t pse_clear_readback;
    bool pss_stopped;
    /* Configuration and schedules. */
    uint32_t configflag;
    uint32_t periodiclistbase;
    uint32_t ctrldssegment;
    bool ctrldssegment_written;
    uint32_t asynclistaddr;
    /* Start. */
    uint32_t start_word;
    uint32_t start_readback;
    uint32_t usbsts_after_start;
    bool periodic_off;
    bool async_on;
};

const char *ehci_init_cause_string(enum ehci_init_cause cause);

void ehci_init_run(const struct ehci_init_ops *ops,
                   uint32_t mmio_bytes,
                   bool supports_64bit,
                   uint32_t periodiclistbase,
                   uint32_t asynclistaddr,
                   uint32_t stop_budget_us,
                   uint32_t reset_budget_us,
                   uint32_t start_budget_us,
                   struct ehci_init_result *result);

#endif
