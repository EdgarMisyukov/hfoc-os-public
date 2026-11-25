#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_H

/*
 * ITEM-612: one composition for the network frame-path counters, so the
 * diagnostic snapshot carries the numbers the heartbeat already had.
 *
 * The owner's 2026-11-15 export (build 5950d5e9b1e6, session 00000003) proved
 * link-up, autonegotiation and an acquired DHCP lease while the peer's ARP,
 * ICMP echo and TCP SYN never reached the SSH listener (`accept=0`).  The
 * frame counters that locate the loss - transmit submissions and reclaims,
 * receive-frame/drop/error/arming state, and the per-class ARP/ICMP/TCP
 * counts - were written to the boot journal's `[PRODUCTION-LOOP]` record but
 * were absent from `kernel_production_status_report()`.  The fifty-two-record
 * `diag save` therefore could not say whether the peer's request arrived and
 * was refused, arrived and was answered, or never arrived at all.
 *
 * These two records are the same numbers in the shared status composition:
 *
 *   [NET-IO]    available, transmit ring counters and the receive ring's
 *               arming state;
 *   [NET-FRAME] the frame path by traffic class, including the frames lwIP
 *               accepted, so admission and arrival are not one number.
 *
 * Both are composed here and published by `kernel_production_status_report()`,
 * so `network`, `diag save` and the SSH `hfoc diag` stream cannot disagree
 * with the journal.  The composition refuses a line that does not fit whole;
 * it never publishes a cut record or a fabricated zero for a backend that
 * never existed.
 */

#include "appliance_production_network_boot.h"

#include <stddef.h>
#include <stdint.h>

/* The longest [NET-IO] form is the ten ring fields at their uint32 maximum. */
#define APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_MAX 384U
/* The longest [NET-FRAME] form is the fifteen class fields at uint32 maximum. */
#define APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX 512U
/* ITEM-643: the RX snapshot carries twelve 0x-prefixed uint32 fields. */
#define APPLIANCE_PRODUCTION_NETWORK_RX_RECORD_MAX 512U
/* "AA:BB:CC:DD:EE:FF" plus the terminator. */
#define APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX 18U

/*
 * Compose one `[NET-IO] available=1 ...` line from the counters the driver
 * published.  Returns the line length, or 0 with an empty buffer when the
 * counters are unavailable or the line does not fit: an absent backend is the
 * named `[NET-IO] available=0 reason=NO_BACKEND` record, not a row of zeroes.
 */
size_t appliance_production_network_io_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size);

/*
 * Compose one `[NET-FRAME] available=1 ...` line.  An unavailable frame path
 * returns 0 with an empty buffer; it is not an all-zero traffic claim.
 */
size_t appliance_production_network_frame_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size);

/*
 * ITEM-643: compose one `[NET-RX] available=1 ...` line from the live receive
 * register/descriptor snapshot.  An unavailable snapshot is the named
 * `[NET-RX] available=0 reason=...` record, not a row of zero registers.
 * The line is composed by the same bounded builder as the other records and
 * is refused whole if it does not fit.
 */
size_t appliance_production_network_rx_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size);

/*
 * Format the active netif's MAC as uppercase colon-separated pairs.  A NULL
 * pointer or an all-zero address is not an identity this record may claim, so
 * both return 0 with an empty buffer.
 */
size_t appliance_production_network_mac_text(
    const uint8_t *mac,
    char *buffer,
    size_t size);

#endif
