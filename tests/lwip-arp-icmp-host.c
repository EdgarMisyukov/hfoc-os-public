#include "hfoc_lwip_proto.h"
#include "hfoc_lwip.h"
#include "hfoc_lwip_time.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_PROTO_HOST_TEST

#define TEST_OUTPUT_MAX 16U
#define TEST_FRAME_MAX HFOC_LWIP_PROTO_FRAME_MAX

static int failures;
static uint32_t test_now_ms;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct test_output_state {
    uint32_t count;
    uint32_t lengths[TEST_OUTPUT_MAX];
    uint8_t frames[TEST_OUTPUT_MAX][TEST_FRAME_MAX];
};

static struct test_output_state output_state;

static uint32_t test_time_source(void)
{
    return test_now_ms;
}

u32_t sys_now(void)
{
    return test_now_ms;
}

static uint16_t read_be16(const uint8_t *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8U) | bytes[1]);
}

static void write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)(value & 0xFFU);
}

static uint16_t checksum(const uint8_t *data, size_t length)
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

static void copy_mac(uint8_t destination[6], const uint8_t source[6])
{
    memcpy(destination, source, 6U);
}

static bool mac_equal(const uint8_t left[6], const uint8_t right[6])
{
    return memcmp(left, right, 6U) == 0;
}

static void pad_frame(uint8_t *frame, size_t *length, size_t minimum)
{
    while (*length < minimum) {
        frame[(*length)++] = 0U;
    }
}

static void build_arp_request(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    size_t position = 0U;

    memset(frame, 0, TEST_FRAME_MAX);
    memset(frame + position, 0xFF, 6U);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x06U;

    write_be16(frame + position, 1U);
    position += 2U;
    write_be16(frame + position, 0x0800U);
    position += 2U;
    frame[position++] = 6U;
    frame[position++] = 4U;
    write_be16(frame + position, 1U);
    position += 2U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 2U;
    memset(frame + position, 0, 6U);
    position += 6U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 15U;

    (void)local_mac;
    pad_frame(frame, &position, 60U);
    *length = position;
}

static void build_icmp_message(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6],
    uint32_t now_ms,
    uint8_t type)
{
    static const uint8_t payload[] = "HFOC-PING";
    size_t position = 0U;
    size_t ip_offset;
    size_t icmp_offset;
    size_t total;

    memset(frame, 0, TEST_FRAME_MAX);
    copy_mac(frame + position, local_mac);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x00U;

    ip_offset = position;
    frame[position++] = 0x45U;
    frame[position++] = 0U;
    icmp_offset = position + 18U;
    total = 20U + 8U + sizeof(payload) - 1U;
    write_be16(frame + position, (uint16_t)total);
    position += 2U;
    write_be16(frame + position, (uint16_t)(0x1200U + (now_ms & 0xFFU)));
    position += 2U;
    write_be16(frame + position, 0x4000U);
    position += 2U;
    frame[position++] = 64U;
    frame[position++] = 1U;
    write_be16(frame + position, 0U);
    position += 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 15U;

    position = icmp_offset;
    frame[position++] = type;
    frame[position++] = 0U;
    write_be16(frame + position, 0U);
    position += 2U;
    write_be16(frame + position, 0xABCDU);
    position += 2U;
    write_be16(frame + position, (uint16_t)(1U + (now_ms & 0x7FFFU)));
    position += 2U;
    memcpy(frame + position, payload, sizeof(payload) - 1U);
    position += sizeof(payload) - 1U;
    write_be16(frame + icmp_offset + 2U,
               checksum(frame + icmp_offset, position - icmp_offset));
    write_be16(frame + ip_offset + 10U,
               checksum(frame + ip_offset, 20U));

    pad_frame(frame, &position, 60U);
    *length = position;
}

static void build_icmp_echo(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6],
    uint32_t now_ms)
{
    build_icmp_message(frame, length, local_mac, peer_mac, now_ms, 8U);
}

/*
 * ITEM-451: one IPv4/TCP segment from the peer to this machine.  The proof
 * only needs the fields the frame path classifies - protocol, ports and flags -
 * so the checksum is the only part that has to be exact.
 */
static void build_tcp_segment(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6],
    uint16_t destination_port,
    uint8_t flags,
    uint32_t sequence)
{
    static const uint8_t source_ip[4] = {10U, 0U, 2U, 2U};
    static const uint8_t target_ip[4] = {10U, 0U, 2U, 15U};
    uint8_t pseudo[12U + 20U];
    size_t position = 0U;
    size_t ip_offset;
    size_t tcp_offset;
    uint16_t total = 20U + 20U;

    memset(frame, 0, TEST_FRAME_MAX);
    copy_mac(frame + position, local_mac);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x00U;

    ip_offset = position;
    frame[position++] = 0x45U;
    frame[position++] = 0U;
    write_be16(frame + position, total);
    position += 2U;
    write_be16(frame + position, 0x3300U);
    position += 2U;
    write_be16(frame + position, 0x4000U);
    position += 2U;
    frame[position++] = 64U;
    frame[position++] = 6U;
    write_be16(frame + position, 0U);
    position += 2U;
    memcpy(frame + position, source_ip, 4U);
    position += 4U;
    memcpy(frame + position, target_ip, 4U);
    position += 4U;
    write_be16(frame + ip_offset + 10U, checksum(frame + ip_offset, 20U));

    tcp_offset = position;
    write_be16(frame + position, 40000U);
    position += 2U;
    write_be16(frame + position, destination_port);
    position += 2U;
    frame[position++] = (uint8_t)(sequence >> 24U);
    frame[position++] = (uint8_t)(sequence >> 16U);
    frame[position++] = (uint8_t)(sequence >> 8U);
    frame[position++] = (uint8_t)sequence;
    position += 4U; /* acknowledgement */
    frame[position++] = 0x50U;
    frame[position++] = flags;
    write_be16(frame + position, 0x2000U);
    position += 2U;
    write_be16(frame + position, 0U);
    position += 2U;
    write_be16(frame + position, 0U);
    position += 2U;

    memset(pseudo, 0, sizeof(pseudo));
    memcpy(pseudo, source_ip, 4U);
    memcpy(pseudo + 4U, target_ip, 4U);
    pseudo[9U] = 6U;
    write_be16(pseudo + 10U, 20U);
    memcpy(pseudo + 12U, frame + tcp_offset, 20U);
    write_be16(frame + tcp_offset + 16U, checksum(pseudo, sizeof(pseudo)));

    pad_frame(frame, &position, 60U);
    *length = position;
}

/* ITEM-451: an unrelated ethertype a busy LAN carries, e.g. IPv6. */
static void build_non_ipv4_frame(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    size_t position = 0U;

    memset(frame, 0, TEST_FRAME_MAX);
    copy_mac(frame + position, local_mac);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x86U;
    frame[position++] = 0xDDU;
    frame[position++] = 0x60U;
    pad_frame(frame, &position, 60U);
    *length = position;
}

/* ITEM-451: IPv4 traffic that is neither ICMP nor TCP. */
static void build_udp_frame(
    uint8_t *frame,
    size_t *length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    size_t position = 0U;
    size_t ip_offset;
    uint16_t total = 20U + 8U;

    memset(frame, 0, TEST_FRAME_MAX);
    copy_mac(frame + position, local_mac);
    position += 6U;
    copy_mac(frame + position, peer_mac);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x00U;

    ip_offset = position;
    frame[position++] = 0x45U;
    frame[position++] = 0U;
    write_be16(frame + position, total);
    position += 2U;
    write_be16(frame + position, 0x4400U);
    position += 2U;
    write_be16(frame + position, 0x4000U);
    position += 2U;
    frame[position++] = 64U;
    frame[position++] = 17U;
    write_be16(frame + position, 0U);
    position += 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 2U;
    frame[position++] = 10U;
    frame[position++] = 0U;
    frame[position++] = 2U;
    frame[position++] = 15U;
    write_be16(frame + ip_offset + 10U, checksum(frame + ip_offset, 20U));

    write_be16(frame + position, 40001U);
    position += 2U;
    write_be16(frame + position, 9999U);
    position += 2U;
    write_be16(frame + position, 8U);
    position += 2U;
    write_be16(frame + position, 0U); /* checksum: UDP is not served here */
    position += 2U;

    pad_frame(frame, &position, 60U);
    *length = position;
}

static bool arp_reply_valid(
    const uint8_t *frame,
    size_t length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    return length >= 42U &&
           mac_equal(frame, peer_mac) &&
           mac_equal(frame + 6U, local_mac) &&
           frame[12U] == 0x08U && frame[13U] == 0x06U &&
           read_be16(frame + 14U) == 1U &&
           read_be16(frame + 16U) == 0x0800U &&
           frame[18U] == 6U && frame[19U] == 4U &&
           read_be16(frame + 20U) == 2U &&
           mac_equal(frame + 22U, local_mac) &&
           frame[28U] == 10U && frame[29U] == 0U &&
           frame[30U] == 2U && frame[31U] == 15U &&
           mac_equal(frame + 32U, peer_mac) &&
           frame[38U] == 10U && frame[39U] == 0U &&
           frame[40U] == 2U && frame[41U] == 2U;
}

static bool icmp_reply_valid(
    const uint8_t *frame,
    size_t length,
    const uint8_t local_mac[6],
    const uint8_t peer_mac[6])
{
    size_t ip_offset = 14U;
    size_t icmp_offset;

    if (length < 51U ||
        !mac_equal(frame, peer_mac) ||
        !mac_equal(frame + 6U, local_mac) ||
        frame[12U] != 0x08U || frame[13U] != 0x00U) {
        return false;
    }
    if (frame[ip_offset] != 0x45U ||
        frame[ip_offset + 8U] == 0U ||
        frame[ip_offset + 9U] != 1U ||
        frame[ip_offset + 12U] != 10U ||
        frame[ip_offset + 13U] != 0U ||
        frame[ip_offset + 14U] != 2U ||
        frame[ip_offset + 15U] != 15U ||
        frame[ip_offset + 16U] != 10U ||
        frame[ip_offset + 17U] != 0U ||
        frame[ip_offset + 18U] != 2U ||
        frame[ip_offset + 19U] != 2U ||
        checksum(frame + ip_offset, 20U) != 0U) {
        return false;
    }
    icmp_offset = ip_offset + 20U;
    return frame[icmp_offset] == 0U &&
           frame[icmp_offset + 1U] == 0U &&
           checksum(frame + icmp_offset, 8U + 9U) == 0U &&
           frame[icmp_offset + 4U] == 0xABU &&
           frame[icmp_offset + 5U] == 0xCDU &&
           memcmp(frame + icmp_offset + 8U, "HFOC-PING", 9U) == 0;
}

static err_t test_output_transmit(void *context, struct pbuf *frame)
{
    struct test_output_state *state =
        (struct test_output_state *)context;
    struct pbuf *current;
    uint32_t length = 0U;
    uint32_t index;

    if (state->count >= TEST_OUTPUT_MAX) {
        return ERR_MEM;
    }
    current = frame;
    while (current != NULL) {
        if (length + current->len > TEST_FRAME_MAX) {
            return ERR_VAL;
        }
        for (index = 0U; index < current->len; ++index) {
            state->frames[state->count][length + index] =
                ((const uint8_t *)current->payload)[index];
        }
        length += current->len;
        current = current->next;
    }
    state->lengths[state->count] = length;
    ++state->count;
    return ERR_OK;
}

static struct hfoc_lwip_netif make_live_netif(void)
{
    struct hfoc_lwip_netif netif;

    memset(&netif, 0, sizeof(netif));
    netif.published = true;
    netif.live = true;
    netif.version = HFOC_LWIP_NETIF_VERSION;
    netif.generation = 1U;
    netif.netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif.netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif.netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    netif.netif.hwaddr[0] = 0x52U;
    netif.netif.hwaddr[1] = 0x54U;
    netif.netif.hwaddr[2] = 0x00U;
    netif.netif.hwaddr[3] = 0x12U;
    netif.netif.hwaddr[4] = 0x34U;
    netif.netif.hwaddr[5] = 0x56U;
    netif.netif.name[0] = 'e';
    netif.netif.name[1] = 't';
    netif.netif.num = 0U;
    IP4_ADDR(&netif.netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif.netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif.netif.gw, 10U, 0U, 2U, 2U);
    return netif;
}

static void reset_lwip(void)
{
    memset(&output_state, 0, sizeof(output_state));
    test_now_ms = 1000U;
    CHECK(hfoc_lwip_alloc_init(), "host fixture starts lwIP allocator");
    CHECK(hfoc_lwip_time_init(test_time_source),
          "host fixture starts lwIP time adapter");
}

static void test_init_validation(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_proto_output output;
    struct hfoc_lwip_proto proto;

    memset(&proto, 0, sizeof(proto));
    output.transmit = test_output_transmit;
    output.context = &output_state;
    CHECK(hfoc_lwip_proto_init(
              NULL, &netif, &output, 1U) == HFOC_LWIP_PROTO_NULL,
          "proto init rejects null proto");
    CHECK(hfoc_lwip_proto_init(
              &proto, NULL, &output, 1U) == HFOC_LWIP_PROTO_NULL,
          "proto init rejects null netif");
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, NULL, 1U) == HFOC_LWIP_PROTO_NULL,
          "proto init rejects null output");
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, &output, 0U) ==
              HFOC_LWIP_PROTO_INVALID_GENERATION,
          "proto init rejects zero generation");
    netif.live = false;
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, &output, 1U) ==
              HFOC_LWIP_PROTO_NETIF_NOT_LIVE,
          "proto init rejects non-live netif");
}

static void test_arp_and_icmp(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_proto_output output;
    struct hfoc_lwip_proto proto;
    uint8_t frame[TEST_FRAME_MAX];
    size_t length;
    enum hfoc_lwip_proto_result result;
    ip4_addr_t peer_ip;
    struct eth_addr resolved;
    uint8_t peer_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    uint8_t local_mac[6] = {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};

    memset(&proto, 0, sizeof(proto));
    output.transmit = test_output_transmit;
    output.context = &output_state;
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, &output, 1U) == HFOC_LWIP_PROTO_OK &&
              hfoc_lwip_proto_published(&proto),
          "proto init publishes adapter");

    build_arp_request(frame, &length, local_mac, peer_mac);
    test_now_ms = 2000U;
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED &&
              output_state.count == 1U,
          "ARP request is accepted and emits one reply");
    CHECK(arp_reply_valid(
              output_state.frames[0], output_state.lengths[0],
              local_mac, peer_mac),
          "ARP reply has exact addresses and opcode");

    IP4_ADDR(&peer_ip, 10U, 0U, 2U, 2U);
    CHECK(hfoc_lwip_proto_lookup(
              &proto, &peer_ip, &resolved) == HFOC_LWIP_PROTO_OK &&
              memcmp(resolved.addr, peer_mac, 6U) == 0,
          "ARP cache resolves peer MAC");

    build_icmp_echo(frame, &length, local_mac, peer_mac, test_now_ms);
    test_now_ms = 3000U;
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED &&
              output_state.count == 2U,
          "ICMP echo is accepted and emits one reply");
    CHECK(icmp_reply_valid(
              output_state.frames[1], output_state.lengths[1],
              local_mac, peer_mac),
          "ICMP reply swaps addresses and preserves echo fields");

    CHECK(hfoc_lwip_proto_shutdown(
              &proto) == HFOC_LWIP_PROTO_OK &&
              !hfoc_lwip_proto_published(&proto),
          "proto shutdown clears published state");
}

static void test_rejections_and_rate_limit(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_proto_output output;
    struct hfoc_lwip_proto proto;
    uint8_t frame[TEST_FRAME_MAX];
    uint8_t peer_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    uint8_t local_mac[6] = {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    size_t length;
    enum hfoc_lwip_proto_result result;
    uint32_t index;

    memset(&proto, 0, sizeof(proto));
    output.transmit = test_output_transmit;
    output.context = &output_state;
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, &output, 1U) == HFOC_LWIP_PROTO_OK,
          "rate fixture publishes adapter");

    CHECK(hfoc_lwip_proto_input(
              &proto, frame, 4U, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_REJECTED &&
              proto.malformed_count == 1U,
          "short frame is rejected before lwIP");

    build_arp_request(frame, &length, local_mac, peer_mac);
    frame[20U] = 9U;
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_REJECTED &&
              proto.malformed_count == 2U,
          "malformed ARP opcode is rejected");

    build_icmp_echo(frame, &length, local_mac, peer_mac, test_now_ms);
    frame[24U] ^= 0x01U;
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_REJECTED &&
              proto.malformed_count == 3U,
          "invalid IPv4 checksum is rejected");

    for (index = 0U; index < HFOC_LWIP_PROTO_ICMP_BUDGET; ++index) {
        build_icmp_echo(frame, &length, local_mac, peer_mac,
                        test_now_ms + index);
        CHECK(hfoc_lwip_proto_input(
                  &proto, frame, length, test_now_ms + index,
                  &result) == HFOC_LWIP_PROTO_OK &&
                  result == HFOC_LWIP_PROTO_ACCEPTED,
              "valid ICMP echo is accepted within budget");
    }
    build_icmp_echo(frame, &length, local_mac, peer_mac, test_now_ms + 10U);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms + 10U,
              &result) == HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_RATE_LIMITED &&
              proto.rate_limited_count == 1U,
          "ICMP echo beyond budget is rate limited");

    CHECK(hfoc_lwip_proto_shutdown(
              &proto) == HFOC_LWIP_PROTO_OK,
          "rate fixture shutdown succeeds");
}

/*
 * ITEM-451: the frame path has to name the traffic class it saw.
 *
 * The 2026-09-19 hardware run published `rx_ok=5 rx_drop=407` while the
 * owner's laptop could neither ping the machine nor open its panel - one
 * number for a peer's ARP request, an unrelated IPv6 frame and a refused ICMP
 * echo alike.  This proof feeds one frame of every class the path
 * distinguishes and requires the counters to name each of them, and it
 * requires an unrelated ethertype to stop being counted as malformed.
 */
static void test_named_classes(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_proto_output output;
    struct hfoc_lwip_proto proto;
    struct hfoc_lwip_proto_counters counters;
    uint8_t frame[TEST_FRAME_MAX];
    uint8_t peer_mac[6] = {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    uint8_t local_mac[6] = {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    size_t length;
    enum hfoc_lwip_proto_result result;

    memset(&proto, 0, sizeof(proto));
    output.transmit = test_output_transmit;
    output.context = &output_state;
    CHECK(hfoc_lwip_proto_init(
              &proto, &netif, &output, 1U) == HFOC_LWIP_PROTO_OK,
          "class fixture publishes adapter");
    CHECK(!hfoc_lwip_proto_counters(NULL, &counters) &&
              !hfoc_lwip_proto_counters(&proto, NULL),
          "class counters refuse null arguments");

    build_arp_request(frame, &length, local_mac, peer_mac);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED,
          "class fixture answers the ARP request");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.arp_request_rx == 1U &&
              counters.arp_reply_tx == 1U &&
              counters.arp_reply_rx == 0U &&
              counters.arp_request_tx == 0U &&
              counters.malformed_rx == 0U,
          "ARP request and its answer are named as such");

    build_arp_request(frame, &length, local_mac, peer_mac);
    write_be16(frame + 20U, 2U);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED,
          "class fixture accepts an ARP reply");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.arp_reply_rx == 1U &&
              counters.arp_request_rx == 1U,
          "an ARP reply is not counted as a request");

    build_icmp_echo(frame, &length, local_mac, peer_mac, test_now_ms);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED,
          "class fixture answers the ICMP echo");
    build_icmp_message(frame, &length, local_mac, peer_mac, test_now_ms, 0U);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_REJECTED,
          "class fixture refuses an echo reply as a request");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.icmp_echo_rx == 1U &&
              counters.icmp_reply_tx == 1U &&
              counters.icmp_other_rx == 1U &&
              counters.malformed_rx == 0U,
          "ICMP echo and other ICMP are named as such");

    build_non_ipv4_frame(frame, &length, local_mac, peer_mac);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_REJECTED,
          "class fixture refuses an unrelated ethertype");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.non_ipv4_rx == 1U &&
              counters.malformed_rx == 0U,
          "unrelated LAN traffic is a class, not a malformed frame");

    build_tcp_segment(
        frame, &length, local_mac, peer_mac, 443U, 0x02U, 0x10203040U);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED,
          "class fixture feeds a panel SYN to lwIP");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.tcp_syn_rx == 1U &&
              counters.tcp_rst_tx == 1U &&
              counters.tcp_syn_ack_tx == 0U,
          "a refused panel SYN is named as a SYN and an RST");

    build_udp_frame(frame, &length, local_mac, peer_mac);
    CHECK(hfoc_lwip_proto_input(
              &proto, frame, length, test_now_ms, &result) ==
              HFOC_LWIP_PROTO_OK &&
              result == HFOC_LWIP_PROTO_ACCEPTED,
          "class fixture feeds other IPv4 traffic to lwIP");
    CHECK(hfoc_lwip_proto_counters(&proto, &counters) &&
              counters.ipv4_other_rx == 1U &&
              counters.tcp_syn_rx == 1U &&
              counters.accepted_rx == 5U &&
              counters.malformed_rx == 0U &&
              counters.rate_limited_rx == 0U,
          "every class and the accepted total are published together");

    CHECK(hfoc_lwip_proto_shutdown(
              &proto) == HFOC_LWIP_PROTO_OK,
          "class fixture shutdown succeeds");
}

int main(void)
{
    reset_lwip();
    test_init_validation();
    test_arp_and_icmp();
    test_rejections_and_rate_limit();
    test_named_classes();

    if (failures != 0) {
        fprintf(stderr, "lwIP ARP/ICMP integration host tests: %d FAIL\n",
                failures);
        return 1;
    }
    printf("lwIP ARP/ICMP integration host tests: ALL OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
