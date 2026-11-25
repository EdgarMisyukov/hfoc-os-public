#ifndef HFOC_LWIP_PPP_OPTS_H
#define HFOC_LWIP_PPP_OPTS_H

/*
 * HFOC lwIP port boundary for the pinned upstream core include.
 *
 * The vendored lwIP core unconditionally includes netif/ppp/ppp_opts.h.
 * HFOC disables PPP and PPPoE; no vendored lwIP file is edited.
 */

#define PPP_SUPPORT 0
#define PPPOE_SUPPORT 0
#define PPPOL2TP_SUPPORT 0
#define PPPOS_SUPPORT 0
#define LWIP_PPP_API 0

#endif
