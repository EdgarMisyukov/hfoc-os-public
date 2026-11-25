#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_RX_ACCOUNTING_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_RX_ACCOUNTING_H

/*
 * ITEM-617: one rule for the receive counters every physical backend uses.
 *
 * `appliance_production_network_input()` returns ACCEPTED as zero and
 * REJECTED/UNAVAILABLE as non-zero, while the wire owner returns true when it
 * consumed the frame before lwIP saw it.  The old call sites tested
 * `!wire && !input`, so an accepted frame was counted as dropped and a
 * rejected one as received.  The counters must describe the frame, not the
 * enum's zero value: a frame is received when either owner consumed it.
 */

#include <stdbool.h>

static inline bool appliance_production_network_rx_frame_received(
    bool wire_consumed,
    bool lwip_accepted)
{
    return wire_consumed || lwip_accepted;
}

#endif /* HFOC_APPLIANCE_PRODUCTION_NETWORK_RX_ACCOUNTING_H */
