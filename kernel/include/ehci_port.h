#ifndef HFOC_EHCI_PORT_H
#define HFOC_EHCI_PORT_H

/*
 * ITEM-563: the root port's PORTSC, read through the address the controller
 * actually decodes, before the walk is allowed to call the port empty.
 *
 * The run on the owner's laptop reported `ports=2 connected=0` on both USB 2
 * controllers while the saved Linux capture of the same machine shows a hub on
 * the EHCI bus (a rate-matching hub with a card reader and a camera behind it).
 * The walk decided "nothing is connected" from a single PORTSC read, taken
 * before anything powered the port, with no stabilization window, and it kept
 * no record of a port that read empty - so the claim `connected=0` could not be
 * checked at all.
 *
 * This module is the production read path, kept pure enough to run on the host
 * over a substituted MMIO window and clock:
 *
 *   1. read PORTSC at BAR + CAPLENGTH + PORTSC + (port - 1) * stride, with the
 *      CAPLENGTH the initialization module validated (ITEM-561) - not zero and
 *      not a constant;
 *   2. when HCSPARAMS.PPC says software owns port power and PP reads 0, switch
 *      the port on and give the power its settling interval (the same 20 ms the
 *      USB 2.0 specification allows a port to report power good);
 *   3. when the port still reads no connect, poll for the connect debounce
 *      window - USB 2.0 section 7.1.7.3, 10 ms per step - up to a bounded
 *      budget, and keep how much of it was used;
 *   4. report both reads: the first one (what the port said before anything was
 *      done to it) and the settled one, plus whether the port was powered here.
 *
 * The point of (4) is the difference the earlier runs could not make: a port
 * that was unpowered, a port whose device is still debouncing, and a port with
 * nothing on it used to be the same single zero.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ehci.h"

/* How long a port is given to report power good after PP is set. */
#define EHCI_PORT_POWER_GOOD_US 20000U
/* One connect-debounce step (USB 2.0 section 7.1.7.3: 10 ms). */
#define EHCI_PORT_SETTLE_STEP_US 10000U
/* The whole window a port is given to report a connect after power/routing.
   This is not a new, larger timeout: the walk had no window at all for the
   question it answered from one read, and the driver's other waits are the
   same order (100 ms). */
#define EHCI_PORT_SETTLE_WINDOW_US 100000U
/* How many root ports' evidence one controller keeps (the EHCI maximum). */
#define EHCI_PORT_RECORDS 16U

/* The MMIO and clock the probe runs on: absolute offsets from BAR0. */
struct ehci_port_ops {
    void *context;
    uint32_t (*read)(void *context, uint32_t offset);
    void (*write)(void *context, uint32_t offset, uint32_t value);
    void (*delay_us)(void *context, uint32_t microseconds);
};

/* One port's answer, both reads kept. */
struct ehci_port_state {
    uint32_t portsc;        /* the settled PORTSC the walk uses */
    uint32_t raw_first;     /* PORTSC before power or any wait */
    uint32_t waited_us;     /* how much of the settle window was used */
    bool connected;         /* ehci_port_connected(portsc) */
    bool connected_first;   /* ehci_port_connected(raw_first) */
    bool powered;           /* this probe switched PP on */
    bool settle_full;       /* the whole window was used without a connect */
};

/*
 * ITEM-567: the root port reset, with its own verdict and its own evidence.
 *
 * The run on build 3264431b reported `status=PORT_RESET_TIMEOUT detail=PR` on
 * both controllers, and the receipt it left could not be read: the words of the
 * reset went to the console journal, which was unbound, so the report carried
 * only the port record measured *before* the walk - `first=0x00001800
 * portsc=0x00001005`, a port that already read connect and enable.
 *
 * The sequence below is EHCI 1.0 Table 2-16, bit 8, read literally:
 *
 *   * software sets PR from a zero and must write PE zero in the same word;
 *   * PR is held for the interval USB 2.0 section 7.1.7.5 requires (50 ms);
 *   * software then writes PR zero to terminate the reset, and "the bit status
 *     will not read as a zero until after the reset has completed";
 *   * the controller "must terminate the reset and stabilize the state of the
 *     port within 2 milliseconds" of that write, and a high-speed port "must
 *     have the port in the enabled state within 2ms".
 *
 * So PR is the controller's completion witness (QEMU models it as a stored bit
 * the software clear drops, and the hardware clears it when the reset ends),
 * and PE is the outcome that makes the port usable.  Both are needed and both
 * are read here.  What the earlier driver did not do is what this record fixes:
 * it sampled the 2 ms bound not at all, it checked PE exactly once immediately
 * after PR cleared, it never re-cleared a latched PR, and it kept none of the
 * words where the report could read them.
 */
enum ehci_port_reset_result {
    EHCI_PORT_RESET_OK = 0,
    /* Nothing was connected: a reset would be a request to an empty port. */
    EHCI_PORT_RESET_NOT_CONNECTED,
    /* The controller never completed the reset: PR stayed set past the whole
       window even after the one re-clear this sequence allows. */
    EHCI_PORT_RESET_PR_LATCHED,
    /* The controller completed the reset and left the port disabled, which is
       the port a high-speed device cannot be reached through. */
    EHCI_PORT_RESET_NOT_ENABLED,
    /* The controller was halted (USBSTS.HSE or HCH) when the reset was
       attempted, or halted while it was running.  Table 2-16 bit 8 is explicit
       about this state: "The host controller may hold Port Reset asserted to a
       one when the HCHalted bit is a one" - so a latched PR here is a symptom
       of the halted controller, not a port fault, and the sequence stops
       instead of spending its window on a controller that cannot answer. */
    EHCI_PORT_RESET_CONTROLLER_HALTED
};

/* The points of the sequence at which USBCMD/USBSTS are kept. */
enum ehci_port_reset_point {
    EHCI_PORT_RESET_POINT_BEFORE = 0,
    EHCI_PORT_RESET_POINT_AFTER_SET,
    EHCI_PORT_RESET_POINT_HOLD,
    EHCI_PORT_RESET_POINT_AFTER_CLEAR,
    EHCI_PORT_RESET_POINT_SETTLE,
    EHCI_PORT_RESET_POINT_POLL,
    EHCI_PORT_RESET_POINT_AFTER,
    EHCI_PORT_RESET_POINT_COUNT
};
#define EHCI_PORT_RESET_POINT_NONE 0xFFU

/* What the reset did, kept for the record: the words before and after, the
   words written to assert and to drop the reset, the word read at the
   specification's 2 ms completion bound, the controller's command and status
   words at each point, how much of the window was used, and whether PR had to
   be cleared twice. */
struct ehci_port_reset_state {
    uint32_t portsc_before;
    uint32_t portsc_set;
    uint32_t portsc_hold;
    uint32_t portsc_cleared;
    uint32_t portsc_cleared_readback;
    uint32_t portsc_after;
    uint32_t waited_us;
    uint32_t usbcmd[EHCI_PORT_RESET_POINT_COUNT];
    uint32_t usbsts[EHCI_PORT_RESET_POINT_COUNT];
    /* The first point at which HSE was set, with the words that carried it. */
    uint32_t hse_point;
    uint32_t hse_usbcmd;
    uint32_t hse_usbsts;
    bool powered;
    bool cleared_twice;
    enum ehci_port_reset_result result;
};

/* USB 2.0 section 7.1.7.5: the reset is driven for at least 50 ms. */
#define EHCI_PORT_RESET_HOLD_US 50000U
/* EHCI 1.0 Table 2-16 bit 8: the controller must complete and stabilize the
   port within 2 ms of software writing PR zero.  This sample records whether
   it did; the window below is how long a controller that has not answered yet
   is still given. */
#define EHCI_PORT_RESET_SETTLE_US 2000U
#define EHCI_PORT_RESET_WINDOW_US 100000U
/* One completion poll step. */
#define EHCI_PORT_RESET_STEP_US 1000U

/*
 * Reset one root port: assert PR with PE cleared and the power/change bits
 * preserved, hold it for the specification's interval, drop it explicitly, read
 * the word at the 2 ms completion bound, wait for the controller to finish (PR
 * zero) with exactly one re-clear if it latched, and then require the port to
 * be enabled - the state a high-speed device is reached through.
 */
enum ehci_port_reset_result ehci_port_reset_run(
    const struct ehci_port_ops *ops,
    uint8_t caplength,
    uint32_t port,
    bool ppc,
    struct ehci_port_reset_state *state);

/*
 * Read one root port through `caplength` (the validated CAPLENGTH) and settle
 * it.  Returns false only when the MMIO seam itself failed; an empty port is a
 * success with `connected == false` and the window's use recorded.
 */
bool ehci_port_probe(const struct ehci_port_ops *ops,
                     uint8_t caplength,
                     uint32_t port,
                     bool ppc,
                     struct ehci_port_state *state);

/* The absolute offset of a root port's PORTSC away from BAR0. */
uint32_t ehci_port_offset(uint8_t caplength, uint32_t port);

#endif
