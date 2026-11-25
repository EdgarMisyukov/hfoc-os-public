#ifndef HFOC_LWIPOPTS_H
#define HFOC_LWIPOPTS_H

#include "hfoc_lwip.h"

/*
 * HFOC OS lwIP configuration (item 232).
 *
 * This is the only lwIP option header used by HFOC. The profile is frozen as
 * NO_SYS IPv4/ARP/ICMP/TCP only; every disabled option is explicit so a
 * future configuration change is a reviewed change.
 */

#define HFOC_LWIP_CONFIG_VERSION 1

/* All lwIP allocations route through fixed HFOC pools (item 233). */
#define MEM_CUSTOM_ALLOCATOR 1
#define MEM_CUSTOM_MALLOC hfoc_lwip_mem_malloc
#define MEM_CUSTOM_FREE hfoc_lwip_mem_free
#define MEM_CUSTOM_CALLOC hfoc_lwip_mem_calloc
#define MEMP_MEM_MALLOC 1
#define MEM_USE_POOLS 0
#define MEMP_USE_CUSTOM_POOLS 0
#define LWIP_ALLOW_MEM_FREE_FROM_OTHER_CONTEXT 0

/* HFOC custom pbufs carry the owning bridge pointer for item 235. */
#define LWIP_SUPPORT_CUSTOM_PBUF 1
#define LWIP_PBUF_CUSTOM_DATA void *hfoc_lwip_bridge;
#define LWIP_PBUF_CUSTOM_DATA_INIT(p) \
    do { (p)->hfoc_lwip_bridge = 0; } while (0)

/* No threads or OS services. */
#define NO_SYS 1
#define LWIP_TIMERS 1
#define LWIP_TCPIP_CORE_LOCKING 0
#define SYS_LIGHTWEIGHT_PROT 0
#define LWIP_CALLBACK_API 1

/* IPv4, ARP, ICMP, and TCP only. */
#define LWIP_IPV4 1
#define LWIP_ARP 1
#define LWIP_ICMP 1
#define LWIP_TCP 1
#define LWIP_UDP 0
#define LWIP_RAW 0
#define LWIP_UDPLITE 0

/* No dynamic addressing, DNS, multicast, or alternative transport APIs. */
#define LWIP_DHCP 0
#define LWIP_AUTOIP 0
#define LWIP_ACD 0
#define LWIP_DNS 0
#define LWIP_IGMP 0
#define LWIP_ALTCP 0
#define LWIP_ALTCP_TLS 0

/* Sequential/socket APIs are not part of the raw NO_SYS port. */
#define LWIP_NETCONN 0
#define LWIP_SOCKET 0
#define LWIP_NETIF_API 0

/* No loopback or per-netif optional paths. */
#define LWIP_NETIF_LOOPBACK 0
#define LWIP_HAVE_LOOPIF 0

/* IPv6 and IPv4 fragmentation are outside the fixed vertical slice. */
#define LWIP_IPV6 0
#define LWIP_ICMP6 0
#define LWIP_IPV6_MLD 0
#define LWIP_IPV6_DHCP6 0
#define LWIP_IPV6_FRAG 0
#define LWIP_IPV6_REASS 0
#define LWIP_IPV6_AUTOCONFIG 0
#define IP_REASSEMBLY 0
#define IP_FRAG 0

/* Software checksums only; no offload or copy-time checksum paths. */
#define LWIP_CHECKSUM_CTRL_PER_NETIF 0
#define LWIP_CHECKSUM_ON_COPY 0

/* Minimal TCP option surface. */
#define TCP_LISTEN_BACKLOG 1
/*
 * ITEM-449: the forward relay is the second listener this profile has ever
 * had - the production serving listener owns port 443 and the forward owns the
 * declared port - and each relay session is two PCBs (the admitted client and
 * the target it dialled) beside the one serving session.  Without the second
 * listen PCB `tcp_listen_with_backlog_and_err` returns NULL and the item
 * cannot exist; without the two connection PCBs a relay session would starve
 * the management plane.  Both numbers are the item's whole change to the
 * frozen profile.
 */
#define MEMP_NUM_TCP_PCB 7
#define MEMP_NUM_TCP_PCB_LISTEN 3
#define LWIP_TCP_SACK_OUT 0
#define LWIP_WND_SCALE 0

/*
 * ITEM-384: the rendered management panel document, plus the hardened
 * fixed head, is handed to the transport as one TLS record. The fixture
 * writes that record with a single tcp_write(), and lwIP only accepts a
 * write when the whole length fits in the send buffer, so the default two-MSS
 * send buffer can never carry a rendered page and the connection stalls.
 * Eight MSS cover the largest recorded page with margin, and the segment
 * queue grows with it.
 */
#define TCP_SND_BUF (8 * TCP_MSS)
#define MEMP_NUM_TCP_SEG 32

/*
 * ITEM-386: the same argument applies to the receive window.  The management
 * listener terminates TLS 1.3, and the first flight a modern client sends is a
 * ClientHello far larger than the two-MSS default receive window; with the
 * default window the client fills it, the appliance waits for the rest of the
 * record and the handshake stalls with no error on either side.  Eight MSS
 * cover the whole ClientHello with margin.
 */
#define TCP_WND (8 * TCP_MSS)

/* No lwIP internal statistics/display path. */
#define LWIP_STATS 0

#endif
