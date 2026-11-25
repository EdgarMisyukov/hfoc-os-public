#ifndef HFOC_KERNEL_STATUS_REPORT_H
#define HFOC_KERNEL_STATUS_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-581: the machine's current network and panel readiness, as one bounded
 * report the console and the diagnostic export both print.
 *
 * The installed system on the owner's Toshiba had no address and no panel, and
 * nothing on the machine answered "what does the network look like right now":
 * the boot records were serial-only (and `bootlog` painted nothing), the menu
 * had no network view, and `diag save` carried the same frozen sections.  This
 * report is the one composition, so the console, the Status menu and the
 * exported file cannot disagree:
 *
 *   [NET-STATUS]  device/readiness/backend/PCI/link/mode/stage/reason - the
 *                 driver's view, honest even when no address exists;
 *   [NET-ADDR]    the addressing lifecycle: state, lease, address/mask/gateway/
 *                 DNS, the active netif's MAC, the DHCP attempt, the reason,
 *                 and the exchange counters (discover/request/offer/ack/refused
 *                 plus the wire's own receive view), so a failure names the
 *                 stage instead of the cable, the DHCP server or the chip;
 *   [NET-IO]      ITEM-612: the live frame path's ring counters and the receive
 *                 ring's arming state, or `available=0 reason=NO_BACKEND`;
 *   [NET-FRAME]   ITEM-612: the same path by traffic class - ARP request/reply,
 *                 ICMP echo/reply, TCP SYN/SYNACK/RST, other IPv4, non-IPv4,
 *                 malformed/rate-limited and the frames lwIP accepted - so
 *                 "arrived and refused" is distinguishable from "never
 *                 arrived" in the exported snapshot;
 *   [PANEL]       the serving record: state/reason, listener, entropy source,
 *                 certificate, panel publication and bind - the HTTPS gate;
 *   [RNG]     the CPUID capabilities the generator read, the source it
 *                 chose, its state and the stage a failure stopped at;
 *   [ADMIN]       the administrator enrollment state and its last status.
 *
 * The report never contains a password, a token or random material.
 */
typedef void (*kernel_status_sink_fn)(void *context, const char *line);

void kernel_production_status_report(kernel_status_sink_fn sink,
                                     void *context);

#endif
