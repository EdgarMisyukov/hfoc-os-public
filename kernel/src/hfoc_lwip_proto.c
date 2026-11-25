#include "hfoc_lwip_proto.h"

#include "lwip/etharp.h"
#include "lwip/init.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "netif/ethernet.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_PROTO_HOST_TEST) || \
    defined(HFOC_ARP_ICMP_QEMU_TEST) || \
    defined(HFOC_PRODUCTION_LWIP)

static bool hfoc_lwip_proto_core_initialized;

static uint16_t hfoc_lwip_proto_read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static uint16_t hfoc_lwip_proto_checksum(
    const uint8_t *data,
    size_t length)
{
    uint32_t sum = 0U;
    size_t offset;

    for (offset = 0U; offset + 1U < length; offset += 2U) {
        sum += ((uint32_t)data[offset] << 8U) | data[offset + 1U];
    }
    if ((length & 1U) != 0U) {
        sum += (uint32_t)data[length - 1U] << 8U;
    }
    while ((sum >> 16U) != 0U) {
        sum = (sum & 0xFFFFU) + (sum >> 16U);
    }
    return (uint16_t)~sum;
}

static bool hfoc_lwip_proto_generation_valid(uint32_t generation)
{
    return generation >= HFOC_LWIP_NETIF_GENERATION_MIN &&
           generation <= HFOC_LWIP_NETIF_GENERATION_MAX;
}

static bool hfoc_lwip_proto_netif_live(
    const struct hfoc_lwip_netif *netif)
{
    return netif != NULL &&
           netif->published &&
           netif->live &&
           netif->version == HFOC_LWIP_NETIF_VERSION &&
           hfoc_lwip_proto_generation_valid(netif->generation) &&
           netif->netif.mtu == HFOC_LWIP_NETIF_MTU &&
           netif->netif.flags == HFOC_LWIP_NETIF_FLAGS &&
           netif->netif.hwaddr_len == NETWORK_DEVICE_MAC_LENGTH &&
           netif->netif.name[0] == HFOC_LWIP_NETIF_NAME0 &&
           netif->netif.name[1] == HFOC_LWIP_NETIF_NAME1 &&
           netif->netif.num == HFOC_LWIP_NETIF_NUM;
}

static void hfoc_lwip_proto_init_core(void)
{
    if (hfoc_lwip_proto_core_initialized) {
        return;
    }
    if (!hfoc_lwip_alloc_ready()) {
        (void)hfoc_lwip_alloc_init();
    }
    mem_init();
    memp_init();
    pbuf_init();
    netif_init();
    sys_timeouts_init();
    etharp_init();
    /*
     * ITEM-386: the management TCP listener is the second owner of this
     * stack, so the one core initialization also brings up the TCP control
     * blocks.  Without it a production listener cannot exist at all.
     */
    tcp_init();
    hfoc_lwip_proto_core_initialized = true;
}

bool hfoc_lwip_proto_core_ready(void)
{
    return hfoc_lwip_proto_core_initialized;
}

static bool hfoc_lwip_proto_valid(
    const struct hfoc_lwip_proto *proto)
{
    return proto != NULL &&
           proto->published &&
           proto->version == HFOC_LWIP_PROTO_VERSION &&
           proto->generation != 0U &&
           hfoc_lwip_proto_netif_live(&proto->netif) &&
           proto->output.transmit != NULL;
}

static void hfoc_lwip_proto_count_frame(
    struct hfoc_lwip_proto *proto,
    const struct pbuf *frame)
{
    const uint8_t *header;
    uint16_t ether_type;

    if (frame == NULL || frame->len < 14U) {
        return;
    }
    header = (const uint8_t *)frame->payload;
    ether_type = hfoc_lwip_proto_read_be16(header + 12U);
    if (ether_type == 0x0806U) {
        if (frame->len >= 14U + 28U) {
            if (hfoc_lwip_proto_read_be16(header + 20U) == 1U) {
                ++proto->arp_request_tx;
            } else if (hfoc_lwip_proto_read_be16(header + 20U) == 2U) {
                ++proto->arp_reply_count;
            }
        }
        return;
    }
    if (ether_type != 0x0800U || frame->len < 14U + 20U) {
        return;
    }
    {
        const uint8_t *ip = header + 14U;
        const size_t header_length = (size_t)(ip[0U] & 0x0FU) * 4U;

        if (header_length < 20U || frame->len < 14U + header_length) {
            return;
        }
        if (ip[9U] == 1U && frame->len >= 14U + header_length + 8U) {
            const uint8_t *icmp = ip + header_length;

            if (icmp[0U] == 0U) {
                ++proto->icmp_reply_count;
            }
            return;
        }
        if (ip[9U] == 6U && frame->len >= 14U + header_length + 20U) {
            const uint8_t flags = ip[header_length + 13U];

            if ((flags & 0x02U) != 0U && (flags & 0x10U) != 0U) {
                ++proto->tcp_syn_ack_tx;
            } else if ((flags & 0x04U) != 0U) {
                ++proto->tcp_rst_tx;
            }
        }
    }
}

static err_t hfoc_lwip_proto_linkoutput(
    struct netif *netif,
    struct pbuf *frame)
{
    struct hfoc_lwip_proto *proto;

    if (netif == NULL || frame == NULL || netif->state == NULL) {
        return ERR_VAL;
    }
    proto = (struct hfoc_lwip_proto *)netif->state;
    if (!hfoc_lwip_proto_valid(proto)) {
        return ERR_VAL;
    }
    hfoc_lwip_proto_count_frame(proto, frame);
    return proto->output.transmit(proto->output.context, frame);
}

static enum hfoc_lwip_proto_status
hfoc_lwip_proto_validate_frame(
    struct hfoc_lwip_proto *proto,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms,
    enum hfoc_lwip_proto_result *result)
{
    uint16_t ether_type;
    uint8_t icmp_message[ICMP_REACHABILITY_MESSAGE_MAX];
    uint8_t icmp_reply[ICMP_REACHABILITY_MESSAGE_MAX];
    size_t icmp_length;
    enum icmp_reachability_status icmp_status;

    *result = HFOC_LWIP_PROTO_ACCEPTED;
    if (frame_length < 14U) {
        ++proto->malformed_count;
        *result = HFOC_LWIP_PROTO_REJECTED;
        return HFOC_LWIP_PROTO_OK;
    }
    if (frame_length > HFOC_LWIP_PROTO_FRAME_MAX) {
        ++proto->malformed_count;
        *result = HFOC_LWIP_PROTO_REJECTED;
        return HFOC_LWIP_PROTO_TOO_LONG;
    }

    ether_type = hfoc_lwip_proto_read_be16(frame + 12U);
    if (ether_type == 0x0806U) {
        uint16_t opcode;

        if (frame_length < 14U + 28U) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        opcode = hfoc_lwip_proto_read_be16(frame + 20U);
        if (opcode != 1U && opcode != 2U) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        /*
         * ITEM-451: a request and a reply are different events - the first is
         * a peer asking this machine to name itself, the second is a peer
         * answering a question this machine asked - and the record has to be
         * able to tell them apart.
         */
        if (opcode == 1U) {
            ++proto->arp_request_rx;
        } else {
            ++proto->arp_reply_rx;
        }
        return HFOC_LWIP_PROTO_OK;
    }
    if (ether_type == 0x0800U) {
        const uint8_t *ip = frame + 14U;
        uint16_t total_length;
        size_t header_length;

        if (frame_length < 14U + 20U || (ip[0U] >> 4U) != 4U ||
            (ip[0U] & 0x0FU) < 5U) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        header_length = (size_t)(ip[0U] & 0x0FU) * 4U;
        total_length = hfoc_lwip_proto_read_be16(ip + 2U);
        if (header_length < 20U ||
            frame_length < 14U + header_length ||
            total_length < header_length ||
            frame_length < 14U + total_length ||
            hfoc_lwip_proto_checksum(ip, header_length) != 0U) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        if (ip[9U] == 6U) {
            /*
             * ITEM-451: a TCP SYN is the panel being opened from another
             * machine.  What leaves in answer says which way it went: a
             * SYN+ACK is an accepted connection, an RST is this machine
             * refusing one, and neither says the SYN never arrived.
             */
            if (total_length >= header_length + 20U &&
                frame_length >= 14U + header_length + 20U) {
                const uint8_t tcp_flags = ip[header_length + 13U];

                if ((tcp_flags & 0x02U) != 0U &&
                    (tcp_flags & 0x10U) == 0U) {
                    ++proto->tcp_syn_rx;
                }
            }
            return HFOC_LWIP_PROTO_OK;
        }
        if (ip[9U] != 1U) {
            ++proto->ipv4_other_rx;
            return HFOC_LWIP_PROTO_OK;
        }
        if (total_length < header_length + ICMP_REACHABILITY_HEADER_SIZE ||
            frame_length < 14U + header_length +
                               ICMP_REACHABILITY_HEADER_SIZE) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        if (ip[header_length] != 8U) {
            /*
             * ITEM-451: ICMP that is not an echo request (an echo reply to
             * this machine's own probe, a destination-unreachable) is a named
             * class, not a malformed frame, and is still not fed to lwIP:
             * the reachability engine owns every ICMP answer this machine
             * gives.
             */
            ++proto->icmp_other_rx;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        icmp_length = (size_t)total_length - header_length;
        if (icmp_length > sizeof(icmp_message) ||
            icmp_length < ICMP_REACHABILITY_HEADER_SIZE) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        memcpy(icmp_message, ip + header_length, icmp_length);
        icmp_status = icmp_reachability_handle_request(
            &proto->icmp,
            icmp_message,
            icmp_length,
            true,
            now_ms,
            icmp_reply,
            sizeof(icmp_message),
            &icmp_length);
        if (icmp_status == ICMP_REACHABILITY_RATE_LIMITED) {
            ++proto->rate_limited_count;
            *result = HFOC_LWIP_PROTO_RATE_LIMITED;
            return HFOC_LWIP_PROTO_OK;
        }
        if (icmp_status != ICMP_REACHABILITY_OK) {
            ++proto->malformed_count;
            *result = HFOC_LWIP_PROTO_REJECTED;
            return HFOC_LWIP_PROTO_OK;
        }
        ++proto->icmp_echo_rx;
        return HFOC_LWIP_PROTO_OK;
    }

    /*
     * ITEM-451: every other ethertype - IPv6 router advertisements, LLDP,
     * PTP, anything a busy LAN carries - is unrelated traffic this appliance
     * does not serve.  Counting it as malformed made the 2026-09-19 hardware
     * record's `rx_drop=407` read like a broken receiver when it was a busy
     * segment; the class counter says which it was.
     */
    ++proto->non_ipv4_rx;
    *result = HFOC_LWIP_PROTO_REJECTED;
    return HFOC_LWIP_PROTO_OK;
}

enum hfoc_lwip_proto_status hfoc_lwip_proto_init(
    struct hfoc_lwip_proto *proto,
    const struct hfoc_lwip_netif *live_netif,
    const struct hfoc_lwip_proto_output *output,
    uint32_t generation)
{
    struct icmp_reachability_policy policy;
    struct netif *netif;

    if (proto == NULL || live_netif == NULL || output == NULL) {
        return HFOC_LWIP_PROTO_NULL;
    }
    if (proto->published) {
        return HFOC_LWIP_PROTO_ALREADY_PUBLISHED;
    }
    if (output->transmit == NULL) {
        return HFOC_LWIP_PROTO_INVALID_OUTPUT;
    }
    if (!hfoc_lwip_proto_generation_valid(generation)) {
        return HFOC_LWIP_PROTO_INVALID_GENERATION;
    }
    if (!hfoc_lwip_proto_netif_live(live_netif)) {
        return HFOC_LWIP_PROTO_NETIF_NOT_LIVE;
    }

    hfoc_lwip_proto_init_core();
    memset(proto, 0, sizeof(*proto));
    proto->version = HFOC_LWIP_PROTO_VERSION;
    proto->generation = generation;
    proto->netif = *live_netif;
    proto->output = *output;
    proto->published = true;
    proto->icmp_budget = HFOC_LWIP_PROTO_ICMP_BUDGET;
    proto->icmp_window_start = 0U;

    policy.version = ICMP_REACHABILITY_VERSION;
    policy.rate_budget = HFOC_LWIP_PROTO_ICMP_BUDGET;
    policy.rate_window_ticks = HFOC_LWIP_PROTO_ICMP_WINDOW_MS;
    (void)icmp_reachability_engine_init(&proto->icmp, &policy);

    netif = &proto->netif.netif;
    netif->state = proto;
    netif->input = ethernet_input;
    netif->output = etharp_output;
    netif->linkoutput = hfoc_lwip_proto_linkoutput;
    netif_set_default(netif);
    netif_set_up(netif);
    netif_set_link_up(netif);
    return HFOC_LWIP_PROTO_OK;
}

enum hfoc_lwip_proto_status hfoc_lwip_proto_input(
    struct hfoc_lwip_proto *proto,
    const uint8_t *frame,
    size_t frame_length,
    uint32_t now_ms,
    enum hfoc_lwip_proto_result *result)
{
    enum hfoc_lwip_proto_status status;
    struct pbuf *pbuf;

    if (proto == NULL || frame == NULL || result == NULL) {
        return HFOC_LWIP_PROTO_NULL;
    }
    if (!hfoc_lwip_proto_valid(proto)) {
        return HFOC_LWIP_PROTO_UNPUBLISHED;
    }

    *result = HFOC_LWIP_PROTO_ACCEPTED;
    status = hfoc_lwip_proto_validate_frame(
        proto, frame, frame_length, now_ms, result);
    if (status != HFOC_LWIP_PROTO_OK ||
        *result != HFOC_LWIP_PROTO_ACCEPTED) {
        return status;
    }

    pbuf = pbuf_alloc(PBUF_RAW, (u16_t)frame_length, PBUF_RAM);
    if (pbuf == NULL) {
        *result = HFOC_LWIP_PROTO_REJECTED;
        return HFOC_LWIP_PROTO_BAD_STATE;
    }
    if (frame_length > (size_t)pbuf->len ||
        pbuf->payload == NULL) {
        pbuf_free(pbuf);
        *result = HFOC_LWIP_PROTO_REJECTED;
        return HFOC_LWIP_PROTO_BAD_STATE;
    }
    memcpy(pbuf->payload, frame, frame_length);
    ++proto->rx_count;
    (void)ethernet_input(pbuf, &proto->netif.netif);
    *result = HFOC_LWIP_PROTO_ACCEPTED;
    return HFOC_LWIP_PROTO_OK;
}

enum hfoc_lwip_proto_status hfoc_lwip_proto_lookup(
    const struct hfoc_lwip_proto *proto,
    const ip4_addr_t *address,
    struct eth_addr *mac)
{
    struct eth_addr *found = NULL;
    const ip4_addr_t *resolved_ip = NULL;
    ssize_t index;

    if (proto == NULL || address == NULL || mac == NULL) {
        return HFOC_LWIP_PROTO_NULL;
    }
    if (!hfoc_lwip_proto_valid(proto)) {
        return HFOC_LWIP_PROTO_UNPUBLISHED;
    }
    index = etharp_find_addr(
        (struct netif *)&proto->netif.netif, address, &found,
        &resolved_ip);
    if (index < 0 || found == NULL) {
        return HFOC_LWIP_PROTO_BAD_STATE;
    }
    *mac = *found;
    return HFOC_LWIP_PROTO_OK;
}

enum hfoc_lwip_proto_status hfoc_lwip_proto_shutdown(
    struct hfoc_lwip_proto *proto)
{
    if (proto == NULL) {
        return HFOC_LWIP_PROTO_NULL;
    }
    if (!proto->published ||
        proto->version != HFOC_LWIP_PROTO_VERSION) {
        return HFOC_LWIP_PROTO_UNPUBLISHED;
    }
    etharp_cleanup_netif(&proto->netif.netif);
    if (netif_default == &proto->netif.netif) {
        netif_default = NULL;
    }
    memset(proto, 0, sizeof(*proto));
    return HFOC_LWIP_PROTO_OK;
}

bool hfoc_lwip_proto_published(
    const struct hfoc_lwip_proto *proto)
{
    return hfoc_lwip_proto_valid(proto);
}

/*
 * ITEM-451: one copy of the frame path's per-class counters, for the records
 * that have to name why a frame did or did not get an answer.  The adapter is
 * copied whether or not it is currently published, because a record that
 * cannot show the numbers is the defect this exists to remove.
 */
bool hfoc_lwip_proto_counters(
    const struct hfoc_lwip_proto *proto,
    struct hfoc_lwip_proto_counters *counters)
{
    if (proto == NULL || counters == NULL) {
        return false;
    }
    memset(counters, 0, sizeof(*counters));
    counters->arp_request_rx = proto->arp_request_rx;
    counters->arp_reply_rx = proto->arp_reply_rx;
    counters->arp_request_tx = proto->arp_request_tx;
    counters->arp_reply_tx = proto->arp_reply_count;
    counters->icmp_echo_rx = proto->icmp_echo_rx;
    counters->icmp_other_rx = proto->icmp_other_rx;
    counters->icmp_reply_tx = proto->icmp_reply_count;
    counters->tcp_syn_rx = proto->tcp_syn_rx;
    counters->tcp_syn_ack_tx = proto->tcp_syn_ack_tx;
    counters->tcp_rst_tx = proto->tcp_rst_tx;
    counters->ipv4_other_rx = proto->ipv4_other_rx;
    counters->non_ipv4_rx = proto->non_ipv4_rx;
    counters->malformed_rx = proto->malformed_count;
    counters->rate_limited_rx = proto->rate_limited_count;
    counters->accepted_rx = proto->rx_count;
    return true;
}

const char *hfoc_lwip_proto_result_string(
    enum hfoc_lwip_proto_result result)
{
    switch (result) {
    case HFOC_LWIP_PROTO_ACCEPTED:
        return "HFOC_LWIP_PROTO_ACCEPTED";
    case HFOC_LWIP_PROTO_RATE_LIMITED:
        return "HFOC_LWIP_PROTO_RATE_LIMITED";
    case HFOC_LWIP_PROTO_REJECTED:
        return "HFOC_LWIP_PROTO_REJECTED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_proto_status_string(
    enum hfoc_lwip_proto_status status)
{
    switch (status) {
    case HFOC_LWIP_PROTO_OK:
        return "HFOC_LWIP_PROTO_OK";
    case HFOC_LWIP_PROTO_NULL:
        return "HFOC_LWIP_PROTO_NULL";
    case HFOC_LWIP_PROTO_BAD_STATE:
        return "HFOC_LWIP_PROTO_BAD_STATE";
    case HFOC_LWIP_PROTO_INVALID_VERSION:
        return "HFOC_LWIP_PROTO_INVALID_VERSION";
    case HFOC_LWIP_PROTO_ALREADY_PUBLISHED:
        return "HFOC_LWIP_PROTO_ALREADY_PUBLISHED";
    case HFOC_LWIP_PROTO_UNPUBLISHED:
        return "HFOC_LWIP_PROTO_UNPUBLISHED";
    case HFOC_LWIP_PROTO_INVALID_GENERATION:
        return "HFOC_LWIP_PROTO_INVALID_GENERATION";
    case HFOC_LWIP_PROTO_NETIF_NOT_LIVE:
        return "HFOC_LWIP_PROTO_NETIF_NOT_LIVE";
    case HFOC_LWIP_PROTO_INVALID_OUTPUT:
        return "HFOC_LWIP_PROTO_INVALID_OUTPUT";
    case HFOC_LWIP_PROTO_INVALID_FRAME:
        return "HFOC_LWIP_PROTO_INVALID_FRAME";
    case HFOC_LWIP_PROTO_TOO_LONG:
        return "HFOC_LWIP_PROTO_TOO_LONG";
    case HFOC_LWIP_PROTO_LINKOUTPUT_FAILED:
        return "HFOC_LWIP_PROTO_LINKOUTPUT_FAILED";
    default:
        return NULL;
    }
}

#endif
