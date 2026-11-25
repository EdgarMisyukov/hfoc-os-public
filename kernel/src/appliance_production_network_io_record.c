/*
 * ITEM-612: the `[NET-IO]` and `[NET-FRAME]` records - one composition each.
 *
 * See the header for why these records exist.  Every field is written through
 * one bounded builder: a line that does not fit whole is refused and the
 * buffer is left empty, so the diagnostic report never carries a truncated
 * counter row that reads like a smaller number.
 */

#include "appliance_production_network_io_record.h"

#include <stdbool.h>
#include <string.h>

struct network_io_record {
    char *buffer;
    size_t size;
    size_t length;
    bool overflow;
};

static void network_io_record_text(
    struct network_io_record *record,
    const char *text)
{
    if (record->overflow || text == NULL) {
        return;
    }
    for (size_t index = 0U; text[index] != '\0'; ++index) {
        if (record->length + 1U >= record->size) {
            record->overflow = true;
            return;
        }
        record->buffer[record->length++] = text[index];
    }
    record->buffer[record->length] = '\0';
}

static void network_io_record_u64(
    struct network_io_record *record,
    uint64_t value)
{
    char digits[20];
    char text[21];
    size_t count = 0U;

    if (value == 0U) {
        network_io_record_text(record, "0");
        return;
    }
    while (value != 0U && count < sizeof(digits)) {
        digits[count++] = (char)('0' + (uint32_t)(value % 10U));
        value /= 10U;
    }
    for (size_t index = 0U; index < count; ++index) {
        text[index] = digits[count - 1U - index];
    }
    text[count] = '\0';
    network_io_record_text(record, text);
}

static void network_io_record_field(
    struct network_io_record *record,
    const char *name,
    uint32_t value)
{
    network_io_record_text(record, " ");
    network_io_record_text(record, name);
    network_io_record_text(record, "=");
    network_io_record_u64(record, value);
}

/* ITEM-643: register and descriptor fields are bit patterns; print them as
   0x-prefixed lowercase hex with leading zeroes trimmed, never as a decimal
   number a reader could mistake for a count. */
static void network_io_record_hex(
    struct network_io_record *record,
    uint32_t value)
{
    static const char digits[] = "0123456789abcdef";
    char text[11];
    bool started = false;
    size_t length = 0U;

    text[length++] = '0';
    text[length++] = 'x';
    for (int shift = 28; shift >= 0; shift -= 4) {
        const uint32_t nibble = (value >> (uint32_t)shift) & 0xFU;

        if (nibble != 0U || started || shift == 0) {
            text[length++] = digits[nibble];
            started = true;
        }
    }
    text[length] = '\0';
    network_io_record_text(record, text);
}

static void network_io_record_hex_field(
    struct network_io_record *record,
    const char *name,
    uint32_t value)
{
    network_io_record_text(record, " ");
    network_io_record_text(record, name);
    network_io_record_text(record, "=");
    network_io_record_hex(record, value);
}

static size_t network_io_record_finish(struct network_io_record *record)
{
    if (record->overflow) {
        record->buffer[0] = '\0';
        return 0U;
    }
    return record->length;
}

size_t appliance_production_network_io_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size)
{
    struct network_io_record record;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (counters == NULL) {
        return 0U;
    }
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    network_io_record_text(&record, "[NET-IO] available=");
    if (!counters->available) {
        /*
         * No backend means no counters.  Printing `tx_sub=0` here would claim
         * a transmit ring this boot never owned, which is the silence this
         * record exists to remove.
         */
        network_io_record_text(&record, "0 reason=NO_BACKEND");
        return network_io_record_finish(&record);
    }
    network_io_record_text(&record, "1");
    network_io_record_field(&record, "tx_sub", counters->tx_submitted);
    network_io_record_field(&record, "tx_recl", counters->tx_reclaimed);
    network_io_record_field(&record, "tx_lost", counters->tx_abandoned);
    network_io_record_field(&record, "tx_full", counters->tx_ring_full);
    network_io_record_field(&record, "tx_rst", counters->tx_restarts);
    network_io_record_field(&record, "tx_err", counters->tx_errors);
    network_io_record_field(&record, "rx_ok", counters->rx_frames);
    network_io_record_field(&record, "rx_drop", counters->rx_dropped);
    network_io_record_field(&record, "rx_err", counters->rx_errors);
    network_io_record_field(&record, "rx_arm", counters->rx_ready);
    return network_io_record_finish(&record);
}

size_t appliance_production_network_frame_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size)
{
    struct network_io_record record;
    const struct hfoc_lwip_proto_counters *frames;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (counters == NULL || !counters->available) {
        return 0U;
    }
    frames = &counters->classes;
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    network_io_record_text(&record, "[NET-FRAME] available=1");
    /*
     * The classes a peer's answer depends on come first, each with what
     * arrived and what left in answer.  `accepted_rx` is last: it is the
     * frames lwIP admitted, so "arrived and was accepted" is distinguishable
     * from "arrived and was refused" (`rx_drop` on [NET-IO]).
     */
    network_io_record_field(&record, "arp_req_rx", frames->arp_request_rx);
    network_io_record_field(&record, "arp_rep_rx", frames->arp_reply_rx);
    network_io_record_field(&record, "arp_req_tx", frames->arp_request_tx);
    network_io_record_field(&record, "arp_rep_tx", frames->arp_reply_tx);
    network_io_record_field(&record, "icmp_echo_rx", frames->icmp_echo_rx);
    network_io_record_field(&record, "icmp_other_rx", frames->icmp_other_rx);
    network_io_record_field(&record, "icmp_rep_tx", frames->icmp_reply_tx);
    network_io_record_field(&record, "syn_rx", frames->tcp_syn_rx);
    network_io_record_field(&record, "synack_tx", frames->tcp_syn_ack_tx);
    network_io_record_field(&record, "rst_tx", frames->tcp_rst_tx);
    network_io_record_field(&record, "ipv4_other_rx", frames->ipv4_other_rx);
    network_io_record_field(&record, "nonipv4_rx", frames->non_ipv4_rx);
    network_io_record_field(&record, "malformed_rx", frames->malformed_rx);
    network_io_record_field(&record, "limited_rx", frames->rate_limited_rx);
    network_io_record_field(&record, "accepted_rx", frames->accepted_rx);
    return network_io_record_finish(&record);
}

/*
 * ITEM-643: the live receive snapshot in the same shared-status composition
 * as [NET-IO]/[NET-FRAME].  A caller that takes this record while the SSH
 * transport is down still gets the answer from `network` or `diag save` on
 * the console.  The record is read-only; the driver performs no register
 * write and does not acknowledge IntrStatus.
 */
size_t appliance_production_network_rx_record_line(
    const struct appliance_production_network_io_counters *counters,
    char *buffer,
    size_t size)
{
    struct network_io_record record;
    const struct appliance_production_network_rx_diagnostic *diagnostic;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (counters == NULL) {
        return 0U;
    }
    diagnostic = &counters->rx_diag;
    record.buffer = buffer;
    record.size = size;
    record.length = 0U;
    record.overflow = false;
    network_io_record_text(&record, "[NET-RX] available=");
    if (!diagnostic->available) {
        network_io_record_text(&record, "0 reason=");
        network_io_record_text(
            &record, diagnostic->reason != NULL ? diagnostic->reason
                                                 : "UNKNOWN");
        return network_io_record_finish(&record);
    }
    network_io_record_text(&record, "1 reason=");
    network_io_record_text(
        &record, diagnostic->reason != NULL ? diagnostic->reason : "OK");
    network_io_record_hex_field(&record, "chip_cmd", diagnostic->chip_cmd);
    network_io_record_hex_field(&record, "intr_status",
                                diagnostic->intr_status);
    network_io_record_hex_field(&record, "intr_mask", diagnostic->intr_mask);
    network_io_record_hex_field(&record, "rx_config", diagnostic->rx_config);
    network_io_record_hex_field(&record, "rx_desc_low",
                                diagnostic->rx_desc_low);
    network_io_record_hex_field(&record, "rx_desc_high",
                                diagnostic->rx_desc_high);
    network_io_record_hex_field(&record, "misc", diagnostic->misc);
    network_io_record_hex_field(&record, "rx_index", diagnostic->rx_index);
    network_io_record_hex_field(&record, "rx_capacity",
                                diagnostic->rx_capacity);
    network_io_record_hex_field(&record, "rx_in_flight",
                                diagnostic->rx_in_flight);
    network_io_record_hex_field(&record, "rx_own", diagnostic->rx_own);
    network_io_record_hex_field(&record, "rx_head_opts1",
                                diagnostic->rx_head_opts1);
    network_io_record_hex_field(&record, "rx_head_address_low",
                                diagnostic->rx_head_address_low);
    /*
     * ITEM-645: the poll loop acknowledges IntrStatus at runtime, so the live
     * `intr_status` above is normally zero.  These two keep the history a
     * physical capture needs: an overflow was latched, and a later drained
     * pass cleared it (the receive path recovered).
     */
    network_io_record_hex_field(&record, "rx_ovf",
                                diagnostic->rx_overflow_events);
    network_io_record_hex_field(&record, "rx_ovf_ack",
                                diagnostic->rx_overflow_acks);
    return network_io_record_finish(&record);
}

size_t appliance_production_network_mac_text(
    const uint8_t *mac,
    char *buffer,
    size_t size)
{
    static const char digits[] = "0123456789ABCDEF";
    bool any = false;
    size_t length = 0U;

    if (buffer == NULL || size == 0U) {
        return 0U;
    }
    buffer[0] = '\0';
    if (mac == NULL) {
        return 0U;
    }
    for (size_t index = 0U; index < NETWORK_DEVICE_MAC_LENGTH; ++index) {
        any = any || mac[index] != 0U;
    }
    /*
     * An all-zero address is not the interface's identity: the wire has not
     * copied a device MAC yet, so `00:00:00:00:00:00` would be a fabricated
     * identity in a record the next hardware run reads as real.
     */
    if (!any) {
        return 0U;
    }
    for (size_t index = 0U; index < NETWORK_DEVICE_MAC_LENGTH; ++index) {
        if (index != 0U) {
            if (length + 1U >= size) {
                buffer[0] = '\0';
                return 0U;
            }
            buffer[length++] = ':';
        }
        if (length + 2U >= size) {
            buffer[0] = '\0';
            return 0U;
        }
        buffer[length++] = digits[(mac[index] >> 4U) & 0xFU];
        buffer[length++] = digits[mac[index] & 0xFU];
    }
    buffer[length] = '\0';
    return length;
}
