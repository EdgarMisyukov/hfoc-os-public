#ifndef HFOC_LWIP_PROTO_H
#define HFOC_LWIP_PROTO_H

#include "hfoc_lwip_netif.h"
#include "icmp_reachability.h"

#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "lwip/pbuf.h"
#include "lwip/prot/ethernet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HFOC_LWIP_PROTO_VERSION 1U
#define HFOC_LWIP_PROTO_FRAME_MAX 1518U
#define HFOC_LWIP_PROTO_ICMP_BUDGET 4U
#define HFOC_LWIP_PROTO_ICMP_WINDOW_MS 1000U

enum hfoc_lwip_proto_result {
    HFOC_LWIP_PROTO_ACCEPTED = 0,
    HFOC_LWIP_PROTO_RATE_LIMITED = 1,
    HFOC_LWIP_PROTO_REJECTED = 2,
};

enum hfoc_lwip_proto_status {
    HFOC_LWIP_PROTO_OK = 0,
    HFOC_LWIP_PROTO_NULL = 1,
    HFOC_LWIP_PROTO_BAD_STATE = 2,
    HFOC_LWIP_PROTO_INVALID_VERSION = 3,
    HFOC_LWIP_PROTO_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_PROTO_UNPUBLISHED = 5,
    HFOC_LWIP_PROTO_INVALID_GENERATION = 6,
    HFOC_LWIP_PROTO_NETIF_NOT_LIVE = 7,
    HFOC_LWIP_PROTO_INVALID_OUTPUT = 8,
    HFOC_LWIP_PROTO_INVALID_FRAME = 9,
    HFOC_LWIP_PROTO_TOO_LONG = 10,
    HFOC_LWIP_PROTO_LINKOUTPUT_FAILED = 11,
};

struct hfoc_lwip_proto_output {
    err_t (*transmit)(void *context, struct pbuf *frame);
    void *context;
};

struct hfoc_lwip_proto {
    bool published;
    uint32_t version;
    uint32_t generation;
    struct hfoc_lwip_netif netif;
    struct hfoc_lwip_proto_output output;
    uint32_t rx_count;
    uint32_t arp_reply_count;
    uint32_t icmp_reply_count;
    uint32_t malformed_count;
    uint32_t rate_limited_count;
    /*
     * ITEM-451: the receive path by traffic class.
     *
     * The 2026-09-19 hardware run could say only `rx_ok=5 rx_drop=407`, which
     * is the same record whether a laptop's ARP request never arrived, arrived
     * and was answered, or arrived and was refused - and the owner's panel and
     * ping failed behind that one number.  These counters name each class the
     * frame path already distinguishes: what arrived (ARP requests and
     * replies, ICMP echo requests and other ICMP, TCP SYNs, other IPv4 and
     * every non-ARP/IPv4 ethertype) and what left in answer (ARP requests and
     * replies, ICMP echo replies, TCP SYN+ACKs and RSTs).  A frame that is not
     * ARP or IPv4 is counted as its own class and no longer as `malformed`,
     * so unrelated LAN traffic can never be read as a broken appliance.
     */
    uint32_t arp_request_rx;
    uint32_t arp_reply_rx;
    uint32_t arp_request_tx;
    uint32_t icmp_echo_rx;
    uint32_t icmp_other_rx;
    uint32_t tcp_syn_rx;
    uint32_t tcp_syn_ack_tx;
    uint32_t tcp_rst_tx;
    uint32_t ipv4_other_rx;
    uint32_t non_ipv4_rx;
    uint32_t icmp_window_start;
    uint32_t icmp_budget;
    struct icmp_reachability_engine icmp;
};

/*
 * ITEM-451: one copy of the frame path's per-class counters.
 *
 * `malformed_rx` and `rate_limited_rx` keep their own names because they are
 * the two refusals with a reason: a frame that could not be parsed at all, and
 * an ICMP echo the reachability window refused.  `accepted_rx` is the frames
 * handed to lwIP, so a record can always be read as
 * `accepted_rx + malformed_rx + rate_limited_rx + non_ipv4_rx + refused`.
 */
struct hfoc_lwip_proto_counters {
    uint32_t arp_request_rx;
    uint32_t arp_reply_rx;
    uint32_t arp_request_tx;
    uint32_t arp_reply_tx;
    uint32_t icmp_echo_rx;
    uint32_t icmp_other_rx;
    uint32_t icmp_reply_tx;
    uint32_t tcp_syn_rx;
    uint32_t tcp_syn_ack_tx;
    uint32_t tcp_rst_tx;
    uint32_t ipv4_other_rx;
    uint32_t non_ipv4_rx;
    uint32_t malformed_rx;
    uint32_t rate_limited_rx;
    uint32_t accepted_rx;
};

/*
 * ITEM-386: the lwIP core (mem/memp/pbuf/netif/etharp/timeouts and TCP) is
 * initialized exactly once, by whichever production component publishes the
 * stack first.  The production NIC backend already owns that initialization,
 * so the management TCP listener must not repeat it: re-running mem_init and
 * memp_init would reset live pools.  This predicate tells a later component
 * that the one initialization already happened.
 */
bool hfoc_lwip_proto_core_ready(void);

enum hfoc_lwip_proto_status hfoc_lwip_proto_init(
    struct hfoc_lwip_proto *proto,
    const struct hfoc_lwip_netif *live_netif,
    const struct hfoc_lwip_proto_output *output,
    uint32_t generation);
enum hfoc_lwip_proto_status hfoc_lwip_proto_input(
    struct hfoc_lwip_proto *proto,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms,
    enum hfoc_lwip_proto_result *result);
enum hfoc_lwip_proto_status hfoc_lwip_proto_lookup(
    const struct hfoc_lwip_proto *proto,
    const ip4_addr_t *address,
    struct eth_addr *mac);
enum hfoc_lwip_proto_status hfoc_lwip_proto_shutdown(
    struct hfoc_lwip_proto *proto);
bool hfoc_lwip_proto_published(
    const struct hfoc_lwip_proto *proto);
/*
 * ITEM-451: read the frame path's per-class counters.  The adapter owns the
 * numbers, so the publication path asks for one copy instead of reaching into
 * the struct; a NULL argument is refused and returns false.
 */
bool hfoc_lwip_proto_counters(
    const struct hfoc_lwip_proto *proto,
    struct hfoc_lwip_proto_counters *counters);
const char *hfoc_lwip_proto_result_string(
    enum hfoc_lwip_proto_result result);
const char *hfoc_lwip_proto_status_string(
    enum hfoc_lwip_proto_status status);

#endif
