/*
 * ITEM-612 host proof for the `[NET-IO]`/`[NET-FRAME]` composition.
 *
 * The 2026-11-15 Toshiba export proved link-up and a lease while the peer's
 * ARP/ICMP/TCP never reached the listener, and the snapshot could not say what
 * the frame path saw because these counters were only in the heartbeat.  This
 * proof drives the composition itself: exact fields, an unavailable backend
 * that is named instead of zeroed, a frame path that is absent instead of
 * claimed, the bounded refusal of a line that does not fit, the maximum
 * counter values, and the active MAC identity (including the zero address
 * that is not an identity).
 */

#include "appliance_production_network_io_record.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_NETWORK_IO_RECORD_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static struct appliance_production_network_io_counters make_counters(void)
{
    struct appliance_production_network_io_counters counters;

    memset(&counters, 0, sizeof(counters));
    counters.available = true;
    counters.tx_submitted = 11U;
    counters.tx_reclaimed = 10U;
    counters.tx_abandoned = 1U;
    counters.tx_ring_full = 2U;
    counters.tx_restarts = 3U;
    counters.tx_errors = 4U;
    counters.rx_frames = 21U;
    counters.rx_dropped = 5U;
    counters.rx_errors = 6U;
    counters.rx_ready = 1U;
    counters.classes.arp_request_rx = 31U;
    counters.classes.arp_reply_rx = 32U;
    counters.classes.arp_request_tx = 33U;
    counters.classes.arp_reply_tx = 34U;
    counters.classes.icmp_echo_rx = 41U;
    counters.classes.icmp_other_rx = 42U;
    counters.classes.icmp_reply_tx = 43U;
    counters.classes.tcp_syn_rx = 51U;
    counters.classes.tcp_syn_ack_tx = 52U;
    counters.classes.tcp_rst_tx = 53U;
    counters.classes.ipv4_other_rx = 61U;
    counters.classes.non_ipv4_rx = 62U;
    counters.classes.malformed_rx = 63U;
    counters.classes.rate_limited_rx = 64U;
    counters.classes.accepted_rx = 71U;
    counters.rx_diag.available = true;
    counters.rx_diag.reason = "OK";
    counters.rx_diag.chip_cmd = 0x1CU;
    counters.rx_diag.intr_status = 0x1234U;
    counters.rx_diag.intr_mask = 0U;
    counters.rx_diag.rx_config = 0xCFU;
    counters.rx_diag.rx_desc_low = 0x12345000U;
    counters.rx_diag.rx_desc_high = 0x1U;
    counters.rx_diag.misc = 0x80000000U;
    counters.rx_diag.rx_index = 7U;
    counters.rx_diag.rx_capacity = 64U;
    counters.rx_diag.rx_in_flight = 3U;
    counters.rx_diag.rx_own = 61U;
    counters.rx_diag.rx_head_opts1 = 0x80000100U;
    counters.rx_diag.rx_head_address_low = 0x2000U;
    counters.rx_diag.rx_overflow_events = 2U;
    counters.rx_diag.rx_overflow_acks = 1U;
    return counters;
}

static void test_io_available(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_MAX];
    static const char expected[] =
        "[NET-IO] available=1 tx_sub=11 tx_recl=10 tx_lost=1 tx_full=2 "
        "tx_rst=3 tx_err=4 rx_ok=21 rx_drop=5 rx_err=6 rx_arm=1";

    CHECK(appliance_production_network_io_record_line(
              &counters, line, sizeof(line)) == strlen(expected),
          "[NET-IO] composes the published ring counters");
    CHECK(strcmp(line, expected) == 0,
          "[NET-IO] fields, order and values are the contract");
}

static void test_io_unavailable(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_MAX];

    counters.available = false;
    CHECK(appliance_production_network_io_record_line(
              &counters, line, sizeof(line)) ==
              strlen("[NET-IO] available=0 reason=NO_BACKEND"),
          "[NET-IO] names an absent backend");
    CHECK(strcmp(line, "[NET-IO] available=0 reason=NO_BACKEND") == 0,
          "[NET-IO] absent backend is exact");
    CHECK(strstr(line, "tx_sub") == NULL && strstr(line, "rx_ok") == NULL,
          "[NET-IO] never prints counters for a backend it did not own");
}

static void test_frame_available(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX];
    static const char expected[] =
        "[NET-FRAME] available=1 arp_req_rx=31 arp_rep_rx=32 arp_req_tx=33 "
        "arp_rep_tx=34 icmp_echo_rx=41 icmp_other_rx=42 icmp_rep_tx=43 "
        "syn_rx=51 synack_tx=52 rst_tx=53 ipv4_other_rx=61 nonipv4_rx=62 "
        "malformed_rx=63 limited_rx=64 accepted_rx=71";

    CHECK(appliance_production_network_frame_record_line(
              &counters, line, sizeof(line)) == strlen(expected),
          "[NET-FRAME] composes every traffic class");
    CHECK(strcmp(line, expected) == 0,
          "[NET-FRAME] ARP/ICMP/TCP/admission fields are exact");
    CHECK(strstr(line, "arp_req_rx=") != NULL &&
              strstr(line, "icmp_echo_rx=") != NULL &&
              strstr(line, "syn_rx=") != NULL &&
              strstr(line, "accepted_rx=") != NULL,
          "[NET-FRAME] names arrival, answer and admission separately");
}

static void test_frame_unavailable(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX];

    counters.available = false;
    line[0] = 'x';
    CHECK(appliance_production_network_frame_record_line(
              &counters, line, sizeof(line)) == 0U,
          "[NET-FRAME] refuses to claim a frame path that does not exist");
    CHECK(line[0] == '\0',
          "[NET-FRAME] leaves no partial line behind");
}

static void test_bounded_refusal(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX];

    line[0] = 'x';
    CHECK(appliance_production_network_io_record_line(
              &counters, line, 8U) == 0U && line[0] == '\0',
          "[NET-IO] refuses a cut record instead of truncating it");
    line[0] = 'x';
    CHECK(appliance_production_network_frame_record_line(
              &counters, line, 8U) == 0U && line[0] == '\0',
          "[NET-FRAME] refuses a cut record instead of truncating it");
    CHECK(appliance_production_network_io_record_line(
              &counters, NULL, sizeof(line)) == 0U,
          "[NET-IO] rejects a missing buffer");
    CHECK(appliance_production_network_io_record_line(
              NULL, line, sizeof(line)) == 0U,
          "[NET-IO] rejects missing counters");
}

static void test_maximum_counters(void)
{
    struct appliance_production_network_io_counters counters;
    char io[APPLIANCE_PRODUCTION_NETWORK_IO_RECORD_MAX];
    char frames[APPLIANCE_PRODUCTION_NETWORK_FRAME_RECORD_MAX];
    size_t length;

    memset(&counters, 0, sizeof(counters));
    counters.available = true;
    counters.tx_submitted = UINT32_MAX;
    counters.tx_reclaimed = UINT32_MAX;
    counters.tx_abandoned = UINT32_MAX;
    counters.tx_ring_full = UINT32_MAX;
    counters.tx_restarts = UINT32_MAX;
    counters.tx_errors = UINT32_MAX;
    counters.rx_frames = UINT32_MAX;
    counters.rx_dropped = UINT32_MAX;
    counters.rx_errors = UINT32_MAX;
    counters.rx_ready = UINT32_MAX;
    counters.classes.arp_request_rx = UINT32_MAX;
    counters.classes.arp_reply_rx = UINT32_MAX;
    counters.classes.arp_request_tx = UINT32_MAX;
    counters.classes.arp_reply_tx = UINT32_MAX;
    counters.classes.icmp_echo_rx = UINT32_MAX;
    counters.classes.icmp_other_rx = UINT32_MAX;
    counters.classes.icmp_reply_tx = UINT32_MAX;
    counters.classes.tcp_syn_rx = UINT32_MAX;
    counters.classes.tcp_syn_ack_tx = UINT32_MAX;
    counters.classes.tcp_rst_tx = UINT32_MAX;
    counters.classes.ipv4_other_rx = UINT32_MAX;
    counters.classes.non_ipv4_rx = UINT32_MAX;
    counters.classes.malformed_rx = UINT32_MAX;
    counters.classes.rate_limited_rx = UINT32_MAX;
    counters.classes.accepted_rx = UINT32_MAX;
    length = appliance_production_network_io_record_line(
        &counters, io, sizeof(io));
    CHECK(length > 0U && length < sizeof(io),
          "[NET-IO] ten-digit counters fit the declared bound");
    length = appliance_production_network_frame_record_line(
        &counters, frames, sizeof(frames));
    CHECK(length > 0U && length < sizeof(frames),
          "[NET-FRAME] ten-digit classes fit the declared bound");
    CHECK(strstr(frames, "4294967295") != NULL,
          "[NET-FRAME] publishes the maximum counter as a number");
}

static void test_rx_available(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_RX_RECORD_MAX];
    static const char expected[] =
        "[NET-RX] available=1 reason=OK chip_cmd=0x1c intr_status=0x1234 "
        "intr_mask=0x0 rx_config=0xcf rx_desc_low=0x12345000 "
        "rx_desc_high=0x1 misc=0x80000000 rx_index=0x7 rx_capacity=0x40 "
        "rx_in_flight=0x3 rx_own=0x3d rx_head_opts1=0x80000100 "
        "rx_head_address_low=0x2000 rx_ovf=0x2 rx_ovf_ack=0x1";

    CHECK(appliance_production_network_rx_record_line(
              &counters, line, sizeof(line)) == strlen(expected),
          "[NET-RX] composes the live receive snapshot");
    CHECK(strcmp(line, expected) == 0,
          "[NET-RX] register, descriptor and ring fields are exact");
    CHECK(strstr(line, "0x80000000") != NULL &&
              strstr(line, "rx_own=0x3d") != NULL,
          "[NET-RX] publishes MISC and the owned descriptor count");
    CHECK(strstr(line, "rx_ovf=0x2") != NULL &&
              strstr(line, "rx_ovf_ack=0x1") != NULL,
          "[NET-RX] keeps the overflow and recovery history");
}

static void test_rx_unavailable(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_RX_RECORD_MAX];

    counters.rx_diag.available = false;
    counters.rx_diag.reason = "NOT_BROUGHT_UP";
    CHECK(appliance_production_network_rx_record_line(
              &counters, line, sizeof(line)) ==
              strlen("[NET-RX] available=0 reason=NOT_BROUGHT_UP"),
          "[NET-RX] names an absent receive path");
    CHECK(strcmp(line, "[NET-RX] available=0 reason=NOT_BROUGHT_UP") == 0,
          "[NET-RX] absent path is exact");
    CHECK(strstr(line, "chip_cmd") == NULL,
          "[NET-RX] never prints registers for a path it did not read");
}

static void test_rx_bounded_refusal(void)
{
    struct appliance_production_network_io_counters counters = make_counters();
    char line[APPLIANCE_PRODUCTION_NETWORK_RX_RECORD_MAX];

    CHECK(appliance_production_network_rx_record_line(
              &counters, line, 24U) == 0U && line[0] == '\0',
          "[NET-RX] refuses a cut record instead of truncating it");
    CHECK(appliance_production_network_rx_record_line(
              NULL, line, sizeof(line)) == 0U && line[0] == '\0',
          "[NET-RX] rejects missing counters");
}

static void test_mac_identity(void)
{
    static const uint8_t mac[NETWORK_DEVICE_MAC_LENGTH] = {
        0x02U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U};
    static const uint8_t zero[NETWORK_DEVICE_MAC_LENGTH] = {0U};
    char text[APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX];

    CHECK(appliance_production_network_mac_text(
              mac, text, sizeof(text)) == strlen("02:00:00:00:00:01"),
          "the active MAC is uppercase colon-separated pairs");
    CHECK(strcmp(text, "02:00:00:00:00:01") == 0,
          "the MAC text is exact");
    CHECK(appliance_production_network_mac_text(
              zero, text, sizeof(text)) == 0U && text[0] == '\0',
          "an all-zero address is not published as an identity");
    CHECK(appliance_production_network_mac_text(
              NULL, text, sizeof(text)) == 0U && text[0] == '\0',
          "a missing MAC is not published");
    CHECK(appliance_production_network_mac_text(
              mac, text, APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX - 1U) == 0U &&
              text[0] == '\0',
          "a MAC buffer one byte short is refused, not cut");
    CHECK(appliance_production_network_mac_text(
              mac, text, APPLIANCE_PRODUCTION_NETWORK_MAC_TEXT_MAX) ==
              strlen("02:00:00:00:00:01"),
          "the declared MAC bound is exactly enough");
}

int main(void)
{
    test_io_available();
    test_io_unavailable();
    test_frame_available();
    test_frame_unavailable();
    test_bounded_refusal();
    test_maximum_counters();
    test_rx_available();
    test_rx_unavailable();
    test_rx_bounded_refusal();
    test_mac_identity();
    if (failures != 0) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("network io record host: ALL OK\n");
    return 0;
}

#endif
