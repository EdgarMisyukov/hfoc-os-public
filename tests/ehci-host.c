/*
 * ITEM-551 host proof: the EHCI encoders and the transfer planner.
 *
 * The hardware half (kernel/src/ehci.c) owns a controller and cannot be
 * unit-tested on a host, so every bit position and scheduling rule it depends
 * on lives in kernel/src/ehci_queue.c, which this proof drives directly.  The
 * QEMU proof (`make test-ehci-storage-qemu`) shows a real emulated EHCI
 * controller accepting the result and a real mass-storage device answering on
 * it; this proof shows *why* the encoding is the one the controller accepts,
 * including the two defects only a controller can otherwise report:
 *
 *   - a queue head whose Data Toggle Control bit is clear ignores the
 *     descriptor's data toggle and takes the toggle from the head; the BOT
 *     transport is built on the host and the device agreeing about the first
 *     packet of every CBW, so this driver must set the bit that makes the
 *     descriptor authoritative;
 *   - a transfer descriptor whose byte count makes a packet start in one
 *     descriptor and end in the next is a controller error the device sees as
 *     a corrupted transfer; the planner keeps every packet inside one
 *     descriptor, and a short packet always ends one.
 *
 * It also pins the port-status write rule: only the fields EHCI 1.0 section
 * 2.3.9 lets software write, and a change bit written back as one - the same
 * class of mistake the xHCI PORTSC work (ITEM-406) already cost this project.
 */
#include "ehci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

static void test_qtd_token(void) {
    const uint32_t token =
        ehci_qtd_token(512U, true, true, 3U, EHCI_QTD_PID_IN,
                       EHCI_QTD_STATUS_ACTIVE);

    CHECK((token & EHCI_QTD_TOTAL_MASK) >> EHCI_QTD_TOTAL_SHIFT == 512U,
          "a transfer descriptor carries its byte count in bits 30:16");
    CHECK((token & EHCI_QTD_DT) != 0U,
          "the data toggle bit is bit 31 and selects the first packet");
    CHECK((token & EHCI_QTD_IOC) != 0U,
          "interrupt-on-complete is bit 15");
    CHECK((token & EHCI_QTD_PID_MASK) >> EHCI_QTD_PID_SHIFT ==
              EHCI_QTD_PID_IN,
          "the PID code is bits 9:8");
    CHECK((token & EHCI_QTD_CERR_MASK) >> EHCI_QTD_CERR_SHIFT == 3U,
          "the error counter is bits 11:10");
    CHECK((token & EHCI_QTD_STATUS_ACTIVE) != 0U,
          "the Active bit is bit 7 and starts the descriptor");
    CHECK((ehci_qtd_token(0U, false, false, 0U, EHCI_QTD_PID_OUT, 0U)) == 0U,
          "a cleared descriptor encodes to zero");

    /* The three PID codes are the only values the descriptor may carry. */
    CHECK(ehci_qtd_token(31U, false, false, 0U, EHCI_QTD_PID_SETUP, 0U) ==
              ((31U << 16) | (EHCI_QTD_PID_SETUP << 8)),
          "a setup packet is PID 2 with its eight bytes counted");
}

static void test_qh_characteristics(void) {
    const uint32_t endpoint1 = ehci_qh_endpoint1(
        8U, true, 64U, false, true, EHCI_QH_SPEED_HIGH, 0U, 3U);

    CHECK((endpoint1 & 0xF0000000U) == 0x80000000U,
          "the NAK reload count is bits 31:28");
    CHECK((endpoint1 & EHCI_QH_CONTROL) != 0U,
          "the encoder can set the Control Endpoint Flag for a split control pipe");
    /* EHCI 1.0 tables 3-19/3-20: a high-speed control pipe carries C=0, and a
       high-speed queue head must not carry a zero high-bandwidth multiplier.
       The first hardware run got this wrong (C=1, Mult=0) and the controller
       refused the first control transfer. */
    {
        const uint32_t hs_control = ehci_qh_endpoint1(
            8U, false, 64U, false, true, EHCI_QH_SPEED_HIGH, 0U, 1U);

        CHECK((hs_control & EHCI_QH_CONTROL) == 0U,
              "the high-speed control queue head carries C=0");
        CHECK(hs_control == 0x80406001U,
              "the actual HS control QH is the word the controller accepts");
        CHECK(ehci_qh_endpoint2(1U, 0U, 0U) == 0x40000000U,
              "the high-speed head carries Mult=1, never zero");
    }
    CHECK((endpoint1 & EHCI_QH_MAX_PACKET_MASK) >> EHCI_QH_MAX_PACKET_SHIFT ==
              64U,
          "the maximum packet length is bits 26:16");
    CHECK((endpoint1 & EHCI_QH_DTC) != 0U,
          "the Data Toggle Control bit makes the descriptor authoritative");
    CHECK((endpoint1 & EHCI_QH_SPEED_MASK) >> EHCI_QH_SPEED_SHIFT ==
              EHCI_QH_SPEED_HIGH,
          "the endpoint speed is bits 13:12 and high speed is 2");
    CHECK((endpoint1 & EHCI_QH_ENDPOINT_MASK) >> EHCI_QH_ENDPOINT_SHIFT == 0U,
          "the endpoint number is bits 11:8");
    CHECK((endpoint1 & EHCI_QH_ADDRESS_MASK) == 3U,
          "the device address is bits 6:0");
    CHECK((endpoint1 & EHCI_QH_HEAD_OF_LIST) == 0U,
          "a transfer head is not the head of the reclamation list");
    CHECK((ehci_qh_endpoint1(0U, false, 0U, true, false,
                             EHCI_QH_SPEED_HIGH, 0U, 0U) &
           EHCI_QH_HEAD_OF_LIST) != 0U,
          "the list head carries the Head of Reclamation List flag");

    CHECK(ehci_qh_endpoint2(0U, 0U, 1U) ==
              ((uint32_t)1U << EHCI_QH_PORT_SHIFT),
          "the hub port number is bits 29:23");
    CHECK(ehci_qh_endpoint2(1U, 2U, 0U) ==
              (((uint32_t)1U << EHCI_QH_MULTIPLIER_SHIFT) |
               ((uint32_t)2U << EHCI_QH_HUB_SHIFT)),
          "the multiplier is bits 31:30 and the hub address bits 22:16");
}


/*
 * The owner's machine does not put its USB 2.0 ports on the EHCI controller:
 * an Integrated Rate Matching Hub sits between them (datasheet 326776-003
 * section 5.19), and the machine's own Linux capture shows it on `Bus 001` with
 * the card reader and the camera behind it.  The first hardware run of the
 * export failed with `NO_MEDIUM` because this driver could only bind a device
 * on the root port; these two proofs pin the protocol and the topology rule the
 * fix is built on.
 */

/* ITEM-554: the setup packet, the hub's class request and the failure
   classification the driver now runs - the parts of the real path a host can
   drive without a controller. */

static void test_control_seams(void) {
    uint8_t setup[8];
    uint8_t request_type = 0U;
    uint8_t request = 0U;
    uint16_t value = 0U;
    uint32_t tokens[3];
    uint32_t failed = 0U;

    CHECK(ehci_control_setup(setup, 0x80U, 0x06U, 0x0100U, 0U, 18U) == 8U &&
              setup[0] == 0x80U && setup[1] == 0x06U && setup[2] == 0x00U &&
              setup[3] == 0x01U && setup[6] == 18U,
          "GET_DESCRIPTOR(device, 18) is the eight bytes on the wire");

    ehci_hub_get_descriptor_fields(&request_type, &request, &value);
    CHECK(request_type == 0xA0U && request == 0x06U && value == 0x2900U,
          "GetHubDescriptor is a class request 0xA0/0x06/0x2900");

    /* ITEM-555: HALTED set together with the reason must be classified by the
       reason, not by the bit. */
    tokens[0] = 0U;
    tokens[1] = EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_TRANSACTION;
    tokens[2] = EHCI_QTD_STATUS_ACTIVE;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) ==
              EHCI_TRANSFER_TRANSACTION && failed == 1U,
          "0x48 (HALTED|XACTERR) is a transaction error, not a stall");
    tokens[1] = EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_BABBLE;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_BABBLE,
          "0x50 (HALTED|BABBLE) is a babble, not a stall");
    tokens[1] = EHCI_QTD_STATUS_HALTED | EHCI_QTD_STATUS_DATA_BUFFER;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) ==
              EHCI_TRANSFER_DATA_BUFFER,
          "0x60 (HALTED|DATABUFERR) is a data buffer error, not a stall");
    tokens[1] = EHCI_QTD_STATUS_HALTED;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_STALLED,
          "0x40 alone is the bus's own STALL");
    /* A stall on the DATA stage: the status descriptor never runs. */
    tokens[0] = 0U;
    tokens[1] = EHCI_QTD_STATUS_HALTED;
    tokens[2] = EHCI_QTD_STATUS_ACTIVE;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_STALLED &&
              failed == 1U,
          "a stall on DATA is a stall, not a timeout");
    /* The early-SETUP case the old last-descriptor check called a timeout. */
    tokens[0] = EHCI_QTD_STATUS_HALTED;
    tokens[1] = EHCI_QTD_STATUS_ACTIVE;
    tokens[2] = EHCI_QTD_STATUS_ACTIVE;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_STALLED &&
              failed == 0U,
          "a stall on SETUP is found on SETUP, not on the last descriptor");
    /* Nothing retired at all: the real timeout. */
    tokens[0] = EHCI_QTD_STATUS_ACTIVE;
    tokens[1] = EHCI_QTD_STATUS_ACTIVE;
    tokens[2] = EHCI_QTD_STATUS_ACTIVE;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_TIMEOUT,
          "every descriptor still Active is the timeout");
    /* A completed transfer is a success, not a timeout. */
    tokens[0] = 0U;
    tokens[1] = 0U;
    tokens[2] = 0U;
    CHECK(ehci_classify_qtds(tokens, 3U, &failed) == EHCI_TRANSFER_OK,
          "a fully retired transfer classifies as OK");
}

static void test_hub_protocol(void) {
    uint8_t descriptor[EHCI_HUB_DESCRIPTOR_PREFIX] = {9U, EHCI_USB_DESCRIPTOR_HUB,
                                                      4U, 0U, 0U, 50U,
                                                      0U, 0U, 0U};
    uint8_t request_type = 0U;
    uint8_t request = 0U;
    uint16_t value = 0U;

    CHECK(ehci_hub_port_count(descriptor, sizeof(descriptor)) == 4U,
          "the hub descriptor names its four downstream ports");
    CHECK(ehci_hub_power_good_ms(descriptor, sizeof(descriptor)) == 100U,
          "bPwrOn2PwrGood is in 2 ms units, so 50 is 100 ms");
    descriptor[1] = 0x22U;
    CHECK(ehci_hub_port_count(descriptor, sizeof(descriptor)) == 0U,
          "a descriptor that is not a hub descriptor carries no ports");
    descriptor[1] = EHCI_USB_DESCRIPTOR_HUB;
    CHECK(ehci_hub_port_count(descriptor, 4U) == 0U,
          "a short hub descriptor is refused");

    CHECK(ehci_hub_feature_request(true, EHCI_HUB_FEATURE_PORT_POWER,
                                   &request_type, &request, &value) &&
              request_type == 0x23U && request == 0x03U &&
              value == EHCI_HUB_FEATURE_PORT_POWER,
          "powering a port is a class SET_FEATURE with feature 8");
    CHECK(ehci_hub_feature_request(false, EHCI_HUB_FEATURE_C_PORT_RESET,
                                   &request_type, &request, &value) &&
              request == 0x01U && value == 0x0014U,
          "clearing the reset change is CLEAR_FEATURE(20)");
    CHECK(ehci_hub_reset_complete(0x0010U) && !ehci_hub_reset_complete(0x0001U),
          "the hub reports a finished reset in the port change word");
}

static void test_hub_topology(void) {
    /* USB 2.0 Table 11-21: bit 9 = low-speed device attached, bit 10 =
       high-speed device attached, bit 11 = test mode.  ITEM-570: the driver had
       the two speed bits one position high, so a high-speed device (0x0400) was
       classified low-speed and refused as one that needs split transactions.
       The words below are raw wPortStatus values, independent of the driver's
       constants: connected + enabled + port power (0x0100) plus the speed. */
    CHECK(ehci_hub_port_verdict(0x0503U) == EHCI_HUB_PORT_HIGH_SPEED,
          "0x0503 is bit 10: a high-speed device behind the hub");
    CHECK(ehci_hub_port_verdict(0x0303U) == EHCI_HUB_PORT_LOW_SPEED,
          "0x0303 is bit 9: a low-speed device behind the hub");
    CHECK(ehci_hub_port_verdict(0x0103U) == EHCI_HUB_PORT_FULL_SPEED,
          "0x0103 carries no speed bit: a full-speed device");
    CHECK(ehci_hub_port_verdict(0x0800U) != EHCI_HUB_PORT_HIGH_SPEED,
          "0x0800 is bit 11, the test mode, not high speed");
    CHECK(ehci_hub_port_verdict(0x0903U) == EHCI_HUB_PORT_FULL_SPEED,
          "a test-mode port with no speed bit is not high speed");
    CHECK(ehci_hub_port_verdict(0x0100U) == EHCI_HUB_PORT_EMPTY,
          "a port with nothing plugged in is empty");
    CHECK(strcmp(ehci_hub_port_verdict_string(EHCI_HUB_PORT_HIGH_SPEED),
                 "HIGH_SPEED") == 0,
          "the speed the hub reports is named as it is");
    CHECK(ehci_hub_port_verdict(0x0101U) == EHCI_HUB_PORT_NOT_ENABLED,
          "a connected but not yet enabled port is not a device yet");
}

static void test_links(void) {
    const uint32_t head = 0x0000000000100000U;

    CHECK(ehci_link(head, false) ==
              ((head & EHCI_QH_LINK_MASK) | EHCI_QH_LINK_TYPE_QH),
          "a queue head link carries the QH type in bits 2:1");
    CHECK(ehci_link(head, true) == EHCI_QH_TERMINATE,
          "a terminating link is the terminate bit alone");
    CHECK((ehci_link(head | 0x1FU, false) & 0x1FU) == 0x02U,
          "the low five bits of a head address never leak into its link");
}

static void test_port_decoding(void) {
    const uint32_t connected = EHCI_PORTSC_CCS | EHCI_PORTSC_PP;
    const uint32_t enabled = connected | EHCI_PORTSC_PE |
                             EHCI_PORTSC_PS_HIGH;
    const uint32_t companion = connected | EHCI_PORTSC_PO;

    CHECK(ehci_port_connected(connected), "CCS is bit 0");
    CHECK(!ehci_port_enabled(connected), "PED is bit 2 and starts clear");
    CHECK(ehci_port_enabled(enabled), "an enabled port reports PED");
    CHECK(ehci_port_high_speed(enabled), "PS 2 is high speed");
    CHECK(!ehci_port_high_speed(connected),
          "an unmeasured port does not claim high speed");
    CHECK(ehci_port_owned_by_companion(companion),
          "a companion-owned port is worth refusing by name");
    CHECK(ehci_port_speed_code(enabled) == 2U, "PS is bits 27:26");
    CHECK(ehci_port_speed_code(0x04000000U) == 1U, "PS value 1 is low speed");

    /* The write value must not carry a single read-only bit back. */
    {
        const uint32_t read = 0xFE003FFFU;
        const uint32_t write = ehci_portsc_write_value(read, EHCI_PORTSC_PR);

        CHECK((write & EHCI_PORTSC_PR) != 0U, "the reset request is written");
        CHECK((write & EHCI_PORTSC_PP) != 0U, "the port stays powered");
        CHECK((write & (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC |
                        EHCI_PORTSC_OCC)) ==
                  (read & (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC |
                           EHCI_PORTSC_OCC)),
              "the change bits are written back as one, which clears them");
        CHECK((write & (EHCI_PORTSC_CCS | EHCI_PORTSC_PE |
                        EHCI_PORTSC_LS_MASK | EHCI_PORTSC_PS_MASK)) == 0U,
              "a read-only status bit is never written back");
        CHECK((write & ~EHCI_PORTSC_WRITABLE) == 0U,
              "no bit outside the writable set is ever written");
        CHECK((ehci_portsc_write_value(read, 0U) & EHCI_PORTSC_PR) == 0U,
              "clearing the reset request writes the bit as zero");
    }
}

static void test_transfer_plan(void) {
    struct ehci_transfer_plan plan;

    CHECK(ehci_plan_transfer(0U, 512U, &plan) && plan.qtd_count == 1U &&
              plan.length[0] == 0U && plan.packets[0] == 0U,
          "zero bytes is one empty descriptor - the status stage");
    CHECK(ehci_plan_transfer(31U, 64U, &plan) && plan.qtd_count == 1U &&
              plan.length[0] == 31U && plan.packets[0] == 1U,
          "a CBW is one descriptor and one packet");
    CHECK(ehci_plan_transfer(512U, 512U, &plan) && plan.qtd_count == 1U &&
              plan.packets[0] == 1U,
          "one sector is one packet");
    CHECK(ehci_plan_transfer(EHCI_TRANSFER_MAX_BYTES, 512U, &plan) &&
              plan.qtd_count == 1U &&
              plan.length[0] == EHCI_TRANSFER_MAX_BYTES &&
              plan.packets[0] == EHCI_TRANSFER_MAX_BYTES / 512U,
          "a full four-page chunk stays inside one descriptor");
    CHECK(ehci_plan_transfer(EHCI_TRANSFER_MAX_BYTES + 512U, 512U, &plan) &&
              plan.qtd_count == 2U &&
              plan.length[0] == EHCI_TRANSFER_MAX_BYTES &&
              plan.length[1] == 512U,
          "a transfer longer than four pages is split on a packet boundary");
    CHECK(ehci_plan_transfer(13U, 512U, &plan) && plan.qtd_count == 1U &&
              plan.packets[0] == 1U,
          "the CSW is one short packet in one descriptor");
    CHECK(!ehci_plan_transfer(16U, 0U, &plan),
          "a zero maximum packet is refused rather than dividing by it");
    {
        const uint32_t too_many = (EHCI_MAX_QTD_PER_TRANSFER + 1U) *
                                  EHCI_TRANSFER_MAX_BYTES;

        CHECK(!ehci_plan_transfer(too_many, 512U, &plan),
              "a transfer past the descriptor bound is refused, not truncated");
    }

    /* Every packet of every chunk stays inside its own descriptor. */
    if (ehci_plan_transfer(EHCI_TRANSFER_MAX_BYTES * 3U + 700U, 64U, &plan)) {
        uint32_t total = 0U;
        bool whole = true;

        for (uint32_t index = 0U; index < plan.qtd_count; ++index) {
            total += plan.length[index];
            if (index + 1U < plan.qtd_count &&
                (plan.length[index] % 64U) != 0U) {
                whole = false;
            }
        }
        CHECK(whole, "a chunk that is not the last is a whole number of packets");
        CHECK(total == EHCI_TRANSFER_MAX_BYTES * 3U + 700U,
              "the plan covers every byte of the transfer");
        CHECK(ehci_plan_offset(&plan, plan.qtd_count) == total,
              "the chunk offsets advance by exactly what the chunks carry");
        CHECK(ehci_plan_packets(&plan, 1U) == plan.packets[0],
              "the packet cursor advances by the packets already sent");
    }
}

static void test_names(void) {
    CHECK(strcmp(ehci_status_string(EHCI_STATUS_OK), "OK") == 0,
          "the controller's success has a name");
    CHECK(strcmp(ehci_status_string(EHCI_STATUS_UNSUPPORTED_SPEED),
                 "UNSUPPORTED_SPEED") == 0,
          "a companion-owned device is refused by name");
    CHECK(strcmp(ehci_status_string(EHCI_STATUS_MEDIUM_NOT_MATCHED),
                 "MEDIUM_NOT_MATCHED") == 0,
          "a device that is not the boot medium is named");
    CHECK(strcmp(ehci_transfer_result_string(EHCI_TRANSFER_STALLED),
                 "STALLED") == 0,
          "a halted endpoint is a stall, not a timeout");
    CHECK(strcmp(ehci_transfer_result_string(EHCI_TRANSFER_TIMEOUT),
                 "TIMEOUT") == 0,
          "a silent device is a timeout");
}

int main(void) {
    test_qtd_token();
    test_qh_characteristics();
    test_links();
    test_control_seams();
    test_hub_protocol();
    test_hub_topology();
    test_port_decoding();
    test_transfer_plan();
    test_names();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("EHCI queue, port and transfer planner host test: ALL OK");
    return 0;
}
