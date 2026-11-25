#ifdef HFOC_ARP_ICMP_QEMU_TEST

#include "hfoc_lwip_proto.h"
#include "serial.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define ARP_ICMP_FRAME_MAX HFOC_LWIP_PROTO_FRAME_MAX

static void arp_icmp_qemu_fail(const char *message)
    __attribute__((noreturn));

static void arp_icmp_qemu_fail(const char *message)
{
    {
        const uint64_t serial_flags = serial_sequence_begin();
        serial_write("[ARP-ICMP] failed: ");
        serial_write(message);
        serial_write("\n");
        serial_sequence_end(serial_flags);
    }
    for (;;) {
        __asm__ volatile ("hlt" : : : "memory");
    }
}

static void arp_icmp_qemu_require(bool condition, const char *message)
{
    if (!condition) {
        arp_icmp_qemu_fail(message);
    }
}

static uint16_t arp_icmp_checksum(
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

static void arp_icmp_write_be16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)(value >> 8U);
    bytes[1] = (uint8_t)(value & 0xFFU);
}

static void arp_icmp_build_arp_request(
    uint8_t *frame,
    size_t *length)
{
    static const uint8_t peer_mac[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    size_t position = 0U;

    memset(frame, 0, ARP_ICMP_FRAME_MAX);
    memset(frame + position, 0xFF, 6U);
    position += 6U;
    memcpy(frame + position, peer_mac, 6U);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x06U;
    arp_icmp_write_be16(frame + position, 1U);
    position += 2U;
    arp_icmp_write_be16(frame + position, 0x0800U);
    position += 2U;
    frame[position++] = 6U;
    frame[position++] = 4U;
    arp_icmp_write_be16(frame + position, 1U);
    position += 2U;
    memcpy(frame + position, peer_mac, 6U);
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
    while (position < 60U) {
        frame[position++] = 0U;
    }
    *length = position;
}

static void arp_icmp_build_echo(
    uint8_t *frame,
    size_t *length,
    uint32_t now_ms)
{
    static const uint8_t peer_mac[6] =
        {0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x02U};
    static const uint8_t local_mac[6] =
        {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    static const uint8_t payload[] = "HFOC-PING";
    size_t position = 0U;
    size_t ip_offset;
    size_t icmp_offset;

    memset(frame, 0, ARP_ICMP_FRAME_MAX);
    memcpy(frame + position, local_mac, 6U);
    position += 6U;
    memcpy(frame + position, peer_mac, 6U);
    position += 6U;
    frame[position++] = 0x08U;
    frame[position++] = 0x00U;
    ip_offset = position;
    frame[position++] = 0x45U;
    frame[position++] = 0U;
    icmp_offset = position + 18U;
    arp_icmp_write_be16(
        frame + position,
        (uint16_t)(20U + 8U + sizeof(payload) - 1U));
    position += 2U;
    arp_icmp_write_be16(
        frame + position, (uint16_t)(0x1200U + (now_ms & 0xFFU)));
    position += 2U;
    arp_icmp_write_be16(frame + position, 0x4000U);
    position += 2U;
    frame[position++] = 64U;
    frame[position++] = 1U;
    arp_icmp_write_be16(frame + position, 0U);
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
    frame[position++] = 8U;
    frame[position++] = 0U;
    arp_icmp_write_be16(frame + position, 0U);
    position += 2U;
    arp_icmp_write_be16(frame + position, 0xABCDU);
    position += 2U;
    arp_icmp_write_be16(
        frame + position, (uint16_t)(1U + (now_ms & 0x7FFFU)));
    position += 2U;
    memcpy(frame + position, payload, sizeof(payload) - 1U);
    position += sizeof(payload) - 1U;
    arp_icmp_write_be16(
        frame + icmp_offset + 2U,
        arp_icmp_checksum(frame + icmp_offset,
                          position - icmp_offset));
    arp_icmp_write_be16(
        frame + ip_offset + 10U,
        arp_icmp_checksum(frame + ip_offset, 20U));
    while (position < 60U) {
        frame[position++] = 0U;
    }
    *length = position;
}

static err_t arp_icmp_transmit(void *context, struct pbuf *frame)
{
    (void)context;
    (void)frame;
    return ERR_OK;
}

static struct hfoc_lwip_netif arp_icmp_live_netif(void)
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

void arp_icmp_qemu_test_init(void)
{
    struct hfoc_lwip_netif netif = arp_icmp_live_netif();
    struct hfoc_lwip_proto_output output;
    struct hfoc_lwip_proto proto;
    uint8_t frame[ARP_ICMP_FRAME_MAX];
    size_t length;
    enum hfoc_lwip_proto_result result;
    ip4_addr_t peer_ip;
    struct eth_addr mac;
    uint32_t index;

    memset(&proto, 0, sizeof(proto));
    output.transmit = arp_icmp_transmit;
    output.context = NULL;
    serial_write("[ARP-ICMP] proof init\n");
    arp_icmp_qemu_require(
        hfoc_lwip_proto_init(
            &proto, &netif, &output, 1U) == HFOC_LWIP_PROTO_OK,
        "proto init");

    arp_icmp_build_arp_request(frame, &length);
    arp_icmp_qemu_require(
        hfoc_lwip_proto_input(
            &proto, frame, length, 1000U, &result) ==
            HFOC_LWIP_PROTO_OK &&
        result == HFOC_LWIP_PROTO_ACCEPTED &&
        proto.arp_reply_count == 1U,
        "ARP reply");
    IP4_ADDR(&peer_ip, 10U, 0U, 2U, 2U);
    arp_icmp_qemu_require(
        hfoc_lwip_proto_lookup(
            &proto, &peer_ip, &mac) == HFOC_LWIP_PROTO_OK,
        "ARP cache");
    serial_write("[ARP-ICMP] arp ok\n");

    arp_icmp_build_echo(frame, &length, 2000U);
    arp_icmp_qemu_require(
        hfoc_lwip_proto_input(
            &proto, frame, length, 2000U, &result) ==
            HFOC_LWIP_PROTO_OK &&
        result == HFOC_LWIP_PROTO_ACCEPTED &&
        proto.icmp_reply_count == 1U,
        "ICMP reply");
    serial_write("[ARP-ICMP] icmp ok\n");

    arp_icmp_qemu_require(
        hfoc_lwip_proto_input(
            &proto, frame, 4U, 3000U, &result) ==
            HFOC_LWIP_PROTO_OK &&
        result == HFOC_LWIP_PROTO_REJECTED &&
        proto.malformed_count == 1U,
        "malformed rejection");

    for (index = 0U; index < HFOC_LWIP_PROTO_ICMP_BUDGET; ++index) {
        arp_icmp_build_echo(frame, &length, 4000U + index);
        arp_icmp_qemu_require(
            hfoc_lwip_proto_input(
                &proto, frame, length, 4000U + index, &result) ==
                HFOC_LWIP_PROTO_OK &&
            result == HFOC_LWIP_PROTO_ACCEPTED,
            "ICMP budget");
    }
    arp_icmp_build_echo(frame, &length, 4500U);
    arp_icmp_qemu_require(
        hfoc_lwip_proto_input(
            &proto, frame, length, 4500U, &result) ==
            HFOC_LWIP_PROTO_OK &&
        result == HFOC_LWIP_PROTO_RATE_LIMITED &&
        proto.rate_limited_count == 1U,
        "ICMP rate limit");
    serial_write("[ARP-ICMP] rate limit ok\n");

    arp_icmp_qemu_require(
        hfoc_lwip_proto_shutdown(
            &proto) == HFOC_LWIP_PROTO_OK,
        "proto shutdown");
    serial_write("[ARP-ICMP] proof OK\n");
}


#else
/*
 * ITEM-432: this whole proof is compiled out of an ordinary kernel, and an
 * empty translation unit is an ISO C violation under -Wpedantic.  One
 * internal declaration keeps the unit non-empty without adding a symbol.
 */
typedef int hfoc_arp_icmp_qemu_translation_unit_not_empty;
#endif
