/*
 * ITEM-551: the pure half of the EHCI driver.
 *
 * The hardware half (kernel/src/ehci.c) owns one controller and cannot be
 * unit-tested on a host, so every bit position and every scheduling rule it
 * depends on lives here, where `make test-ehci-host` drives it directly: the
 * queue element transfer descriptor's token, the queue head's two
 * characteristics dwords, the link values, the port-state decoders and the
 * transfer planner that splits a buffer into descriptors without ever letting a
 * USB packet start in one descriptor and end in the next.
 *
 * The layouts are EHCI 1.0 sections 3.5 (qTD), 3.6 (QH) and 2.3.9 (PORTSC);
 * the comments name the field each constant addresses so a reader can check the
 * document instead of trusting a number.  Nothing in this file touches MMIO,
 * allocates memory or depends on a controller.
 */

#include "ehci.h"

#include <stddef.h>

const char *ehci_status_string(enum ehci_status status) {
    switch (status) {
    case EHCI_STATUS_OK:
        return "OK";
    case EHCI_STATUS_UNKNOWN:
        return "UNKNOWN";
    case EHCI_STATUS_CONTROLLER_ABSENT:
        return "CONTROLLER_ABSENT";
    case EHCI_STATUS_PCI_SCAN_FAILED:
        return "PCI_SCAN_FAILED";
    case EHCI_STATUS_NO_MEMORY_BAR:
        return "NO_MEMORY_BAR";
    case EHCI_STATUS_BAR_UNDECODED:
        return "BAR_UNDECODED";
    case EHCI_STATUS_MMIO_MAP_FAILED:
        return "MMIO_MAP_FAILED";
    case EHCI_STATUS_MMIO_TOO_SMALL:
        return "MMIO_TOO_SMALL";
    case EHCI_STATUS_DMA_UNAVAILABLE:
        return "DMA_UNAVAILABLE";
    case EHCI_STATUS_RESET_TIMEOUT:
        return "RESET_TIMEOUT";
    case EHCI_STATUS_CONFIGFLAG_TIMEOUT:
        return "CONFIGFLAG_TIMEOUT";
    case EHCI_STATUS_PORT_RESET_TIMEOUT:
        return "PORT_RESET_TIMEOUT";
    case EHCI_STATUS_PORT_NOT_ENABLED:
        return "PORT_NOT_ENABLED";
    case EHCI_STATUS_UNSUPPORTED_SPEED:
        return "UNSUPPORTED_SPEED";
    case EHCI_STATUS_ADDRESS_FAILED:
        return "ADDRESS_FAILED";
    case EHCI_STATUS_DESCRIPTOR_FAILED:
        return "DESCRIPTOR_FAILED";
    case EHCI_STATUS_CONFIG_PARSE_FAILED:
        return "CONFIG_PARSE_FAILED";
    case EHCI_STATUS_SET_CONFIGURATION_FAILED:
        return "SET_CONFIGURATION_FAILED";
    case EHCI_STATUS_STORAGE_REFUSED:
        return "STORAGE_REFUSED";
    case EHCI_STATUS_BLOCK_DEVICE_FAILED:
        return "BLOCK_DEVICE_FAILED";
    case EHCI_STATUS_TRANSFER_FAILED:
        return "TRANSFER_FAILED";
    case EHCI_STATUS_MEDIUM_UNAVAILABLE:
        return "MEDIUM_UNAVAILABLE";
    case EHCI_STATUS_MEDIUM_NOT_MATCHED:
        return "MEDIUM_NOT_MATCHED";
    case EHCI_STATUS_HOST_SYSTEM_ERROR:
        return "HOST_SYSTEM_ERROR";
    case EHCI_STATUS_NO_DEVICE:
        return "NO_DEVICE";
    }
    return "UNKNOWN";
}

const char *ehci_transfer_result_string(enum ehci_transfer_result result) {
    switch (result) {
    case EHCI_TRANSFER_OK:
        return "OK";
    case EHCI_TRANSFER_TIMEOUT:
        return "TIMEOUT";
    case EHCI_TRANSFER_STALLED:
        return "STALLED";
    case EHCI_TRANSFER_DATA_BUFFER:
        return "DATABUFERR";
    case EHCI_TRANSFER_BABBLE:
        return "BABBLE";
    case EHCI_TRANSFER_TRANSACTION:
        return "XACTERR";
    case EHCI_TRANSFER_OTHER:
        return "OTHER";
    }
    return "UNKNOWN";
}

size_t ehci_control_setup(uint8_t *setup,
                          uint8_t request_type,
                          uint8_t request,
                          uint16_t value,
                          uint16_t index,
                          uint16_t length) {
    if (setup == NULL) {
        return 0U;
    }
    setup[0] = request_type;
    setup[1] = request;
    setup[2] = (uint8_t)(value & 0xFFU);
    setup[3] = (uint8_t)(value >> 8U);
    setup[4] = (uint8_t)(index & 0xFFU);
    setup[5] = (uint8_t)(index >> 8U);
    setup[6] = (uint8_t)(length & 0xFFU);
    setup[7] = (uint8_t)(length >> 8U);
    return 8U;
}

void ehci_hub_get_descriptor_fields(uint8_t *request_type,
                                    uint8_t *request,
                                    uint16_t *value) {
    if (request_type != NULL) {
        *request_type = EHCI_HUB_REQUEST_TYPE_DEVICE_IN;
    }
    if (request != NULL) {
        *request = 0x06U;
    }
    if (value != NULL) {
        *value = (uint16_t)((uint16_t)EHCI_USB_DESCRIPTOR_HUB << 8U);
    }
}

/* The EHCI status byte of a retired descriptor: Active (bit 7) means it never
   ran; Halted (bit 6) is a STALL; the rest name the failure class. */
enum ehci_transfer_result ehci_classify_qtds(const uint32_t *tokens,
                                             uint32_t count,
                                             uint32_t *failed_index) {
    if (tokens == NULL || count == 0U) {
        return EHCI_TRANSFER_OTHER;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        const uint32_t status = tokens[index] & 0xFFU;

        if ((status & EHCI_QTD_STATUS_ACTIVE) != 0U) {
            continue;
        }
        /* ITEM-555: a real controller sets HALTED together with the reason it
           halted (0x48 XACTERR, 0x50 BABBLE, 0x60 DATABUFERR), so the specific
           cause must be tested first; the old order reported all three as a
           STALL - and a STALL in the record does not prove the bus answered
           STALL. */
        if ((status & EHCI_QTD_STATUS_DATA_BUFFER) != 0U) {
            if (failed_index != NULL) {
                *failed_index = index;
            }
            return EHCI_TRANSFER_DATA_BUFFER;
        }
        if ((status & EHCI_QTD_STATUS_BABBLE) != 0U) {
            if (failed_index != NULL) {
                *failed_index = index;
            }
            return EHCI_TRANSFER_BABBLE;
        }
        if ((status & EHCI_QTD_STATUS_TRANSACTION) != 0U) {
            if (failed_index != NULL) {
                *failed_index = index;
            }
            return EHCI_TRANSFER_TRANSACTION;
        }
        if ((status & EHCI_QTD_STATUS_HALTED) != 0U) {
            if (failed_index != NULL) {
                *failed_index = index;
            }
            return EHCI_TRANSFER_STALLED;
        }
    }
    {
        uint32_t retired = 0U;

        for (uint32_t index = 0U; index < count; ++index) {
            if ((tokens[index] & EHCI_QTD_STATUS_ACTIVE) == 0U) {
                ++retired;
            }
        }
        if (retired == count) {
            if (failed_index != NULL) {
                *failed_index = count;
            }
            return EHCI_TRANSFER_OK;
        }
    }
    if (failed_index != NULL) {
        *failed_index = count;
    }
    return EHCI_TRANSFER_TIMEOUT;
}

/* --------------------------------------------------- asynchronous ring --- */

bool ehci_ring_reachable(uint64_t base, uint32_t bytes, bool supports_64bit) {
    if (base == 0U || bytes == 0U) {
        return false;
    }
    /* 32 bits is the widest address a link pointer or a descriptor buffer can
       name when the controller has no 64-bit addressing capability
       (HCCPARAMS.AC64 = 0); with the capability the segment register carries
       the high bits and the driver's own pool check keeps them zero. */
    return supports_64bit ||
           (base != 0U && base + bytes <= UINT64_C(0x100000000));
}

void ehci_ring_list_head(struct ehci_qh *qh, uint32_t physical) {
    if (qh == NULL) {
        return;
    }
    for (size_t index = 0U; index < sizeof(*qh); ++index) {
        ((volatile uint8_t *)qh)[index] = 0U;
    }
    qh->link = ehci_link(physical, false);
    /* The head of the reclamation list: H = 1, high speed, and Mult = 1 as
       EHCI 1.0 section 3.6.2 requires on every high-speed queue head.  The
       transfer head already had Mult = 1 (ITEM-554); the list head did not,
       and the machine's schedule is started from exactly this head. */
    qh->endpoint1 = ehci_qh_endpoint1(8U, false, 0U, true, false,
                                      EHCI_QH_SPEED_HIGH, 0U, 0U);
    qh->endpoint2 = ehci_qh_endpoint2(1U, 0U, 0U);
    qh->overlay_next = EHCI_QH_TERMINATE;
    qh->overlay_alt_next = EHCI_QH_TERMINATE;
    /* Halted says "no descriptors are attached"; Active stays clear. */
    qh->overlay_token = EHCI_QTD_STATUS_HALTED;
}

void ehci_ring_transfer_head(struct ehci_qh *qh,
                             uint32_t list_head_physical,
                             uint32_t first_qtd_physical,
                             uint16_t max_packet,
                             uint8_t endpoint,
                             uint8_t address) {
    if (qh == NULL) {
        return;
    }
    for (size_t index = 0U; index < sizeof(*qh); ++index) {
        ((volatile uint8_t *)qh)[index] = 0U;
    }
    /* C = 0 on a high-speed control pipe, DTC = 1 so the toggle comes from the
       descriptor, Mult = 1 (EHCI 1.0 section 3.6.2). */
    qh->endpoint1 = ehci_qh_endpoint1(8U, false, max_packet, false, true,
                                      EHCI_QH_SPEED_HIGH, endpoint, address);
    qh->endpoint2 = ehci_qh_endpoint2(1U, 0U, 0U);
    qh->link = ehci_link(list_head_physical, false);
    qh->overlay_next = first_qtd_physical;
    qh->overlay_alt_next = EHCI_QH_TERMINATE;
    qh->overlay_token = 0U;
}

static void ehci_ring_buffer_pointers(struct ehci_qtd *qtd,
                                      uint64_t physical,
                                      uint32_t length,
                                      bool supports_64bit) {
    uint64_t cursor = physical;
    uint32_t remaining = length;

    for (uint32_t index = 0U; index < EHCI_QTD_BUFFERS; ++index) {
        qtd->buffer[index] = (uint32_t)cursor;
        if (supports_64bit) {
            /* Appendix B, qTD-64: the upper half of each buffer address.  The
               segment register carries the schedule pointers' upper half, not
               the buffers'. */
            qtd->ext_buffer[index] = (uint32_t)(cursor >> 32U);
        }
        if (remaining == 0U) {
            return;
        }
        {
            const uint32_t offset = (uint32_t)(cursor % EHCI_PAGE_BYTES);
            const uint32_t to_page_end = EHCI_PAGE_BYTES - offset;

            if (to_page_end >= remaining) {
                return;
            }
            remaining -= to_page_end;
            cursor += to_page_end;
        }
    }
}

void ehci_ring_qtd(struct ehci_qtd *qtd,
                   uint32_t next_physical,
                   uint32_t token,
                   uint64_t buffer_physical,
                   uint32_t length,
                   bool supports_64bit) {
    if (qtd == NULL) {
        return;
    }
    for (size_t index = 0U; index < sizeof(*qtd); ++index) {
        ((volatile uint8_t *)qtd)[index] = 0U;
    }
    qtd->next = next_physical;
    qtd->alt_next = EHCI_QH_TERMINATE;
    qtd->token = token;
    ehci_ring_buffer_pointers(qtd, buffer_physical, length, supports_64bit);
}

uint32_t ehci_ring_usbcmd_start(uint32_t after_reset) {
    /*
     * The first start is a known value, not a read-modify-write of whatever
     * the controller reports: the interrupt threshold it came back with is
     * kept, everything else - the periodic schedule enable in particular - is
     * written as clear, and only run plus the asynchronous schedule are set.
     * The hardware snapshot of ITEM-559 had PSE = 1 and PSS = 1 with a
     * PERIODICLISTBASE the driver never programmed: a schedule the driver did
     * not build must not be running.
     */
    return (after_reset & EHCI_USBCMD_ITC_MASK) | EHCI_USBCMD_RS |
           EHCI_USBCMD_ASE;
}

/* ------------------------------------------------- USB 2.0 hub protocol --- */

/* The hub class descriptor (USB 2.0 11.23.2.1): bLength, bDescriptorType 0x29,
   bNbrPorts at byte 2 and bPwrOn2PwrGood at byte 5 (in 2 ms units). */
uint8_t ehci_hub_port_count(const uint8_t *descriptor, size_t length) {
    if (descriptor == NULL || length < EHCI_HUB_DESCRIPTOR_PREFIX ||
        descriptor[1] != EHCI_USB_DESCRIPTOR_HUB || descriptor[0] < 7U) {
        return 0U;
    }
    return descriptor[2];
}

uint8_t ehci_hub_power_good_ms(const uint8_t *descriptor, size_t length) {
    if (descriptor == NULL || length < EHCI_HUB_DESCRIPTOR_PREFIX ||
        descriptor[1] != EHCI_USB_DESCRIPTOR_HUB || descriptor[0] < 7U) {
        return 0U;
    }
    return (uint8_t)((uint16_t)descriptor[5] * 2U);
}

bool ehci_hub_feature_request(bool set,
                              uint16_t feature,
                              uint8_t *request_type,
                              uint8_t *request,
                              uint16_t *value) {
    if (request_type == NULL || request == NULL || value == NULL) {
        return false;
    }
    *request_type = EHCI_HUB_REQUEST_TYPE_OUT;
    *request = set ? EHCI_HUB_REQUEST_SET_FEATURE
                   : EHCI_HUB_REQUEST_CLEAR_FEATURE;
    *value = feature;
    return true;
}

/* The port status bits (USB 2.0 11.24.2.7).  A high-speed device behind a
   high-speed hub needs no transaction translator; the slower ones would need
   split transactions, which this driver does not implement and refuses by
   name. */
enum ehci_hub_port_verdict ehci_hub_port_verdict(uint16_t status) {
    if ((status & EHCI_HUB_PORT_STATUS_CONNECTED) == 0U) {
        return EHCI_HUB_PORT_EMPTY;
    }
    if ((status & EHCI_HUB_PORT_STATUS_ENABLED) == 0U) {
        return EHCI_HUB_PORT_NOT_ENABLED;
    }
    if ((status & EHCI_HUB_PORT_STATUS_LOW_SPEED) != 0U) {
        return EHCI_HUB_PORT_LOW_SPEED;
    }
    if ((status & EHCI_HUB_PORT_STATUS_HIGH_SPEED) != 0U) {
        return EHCI_HUB_PORT_HIGH_SPEED;
    }
    return EHCI_HUB_PORT_FULL_SPEED;
}

const char *ehci_hub_port_verdict_string(enum ehci_hub_port_verdict verdict) {
    switch (verdict) {
    case EHCI_HUB_PORT_EMPTY:
        return "EMPTY";
    case EHCI_HUB_PORT_HIGH_SPEED:
        return "HIGH_SPEED";
    case EHCI_HUB_PORT_FULL_SPEED:
        return "FULL_SPEED";
    case EHCI_HUB_PORT_LOW_SPEED:
        return "LOW_SPEED";
    case EHCI_HUB_PORT_NOT_ENABLED:
        return "NOT_ENABLED";
    }
    return "UNKNOWN";
}

bool ehci_hub_reset_complete(uint16_t change) {
    return (change & EHCI_HUB_PORT_CHANGE_RESET) != 0U;
}

/*
 * The transfer descriptor token (EHCI 1.0 section 3.5.3): the data toggle the
 * first packet of this descriptor uses in bit 31, the byte count in bits
 * 30:16, interrupt-on-complete in bit 15, the error counter in bits 11:10, the
 * PID in bits 9:8 and the status byte in bits 7:0 (bit 0 is PERR for a
 * ping, bit 7 makes the descriptor active).
 */
uint32_t ehci_qtd_token(uint32_t total_bytes,
                        bool data_toggle,
                        bool interrupt_on_complete,
                        uint8_t error_count,
                        uint8_t pid,
                        uint8_t status) {
    uint32_t token = 0U;

    token |= (total_bytes << EHCI_QTD_TOTAL_SHIFT) & EHCI_QTD_TOTAL_MASK;
    if (data_toggle) {
        token |= EHCI_QTD_DT;
    }
    if (interrupt_on_complete) {
        token |= EHCI_QTD_IOC;
    }
    token |= ((uint32_t)error_count << EHCI_QTD_CERR_SHIFT) & EHCI_QTD_CERR_MASK;
    token |= ((uint32_t)pid << EHCI_QTD_PID_SHIFT) & EHCI_QTD_PID_MASK;
    token |= (uint32_t)status & 0xFFU;
    return token;
}

/*
 * The queue head's endpoint characteristics (EHCI 1.0 section 3.6, dword 1):
 * NAK count reload in bits 31:28 (how many times a NAK is retried inside one
 * frame before the head moves on), the Control Endpoint Flag in bit 27 (a
 * control pipe, not a bulk/interrupt one), the maximum packet length in bits
 * 26:16, Head of Reclamation List in bit 15 (this head is the head of the
 * asynchronous list), the Data Toggle Control bit 14 (1 = the toggle comes
 * from the descriptor, which is the only mode in which software can hand the
 * controller a known first packet), the endpoint speed in bits 13:12 (2 = high
 * speed), the endpoint number in bits 11:8 and the device address in bits 6:0.
 */
uint32_t ehci_qh_endpoint1(uint8_t nak_reload,
                           bool is_control,
                           uint16_t max_packet,
                           bool head_of_list,
                           bool toggle_from_qtd,
                           uint8_t speed,
                           uint8_t endpoint,
                           uint8_t address) {
    uint32_t value = 0U;

    value |= ((uint32_t)nak_reload & 0x0FU) << EHCI_QH_NAK_RELOAD_SHIFT;
    if (is_control) {
        value |= EHCI_QH_CONTROL;
    }
    value |= ((uint32_t)max_packet << EHCI_QH_MAX_PACKET_SHIFT) &
             EHCI_QH_MAX_PACKET_MASK;
    if (head_of_list) {
        value |= EHCI_QH_HEAD_OF_LIST;
    }
    if (toggle_from_qtd) {
        value |= EHCI_QH_DTC;
    }
    value |= ((uint32_t)speed & 0x03U) << EHCI_QH_SPEED_SHIFT;
    value |= ((uint32_t)endpoint & 0x0FU) << EHCI_QH_ENDPOINT_SHIFT;
    value |= (uint32_t)address & EHCI_QH_ADDRESS_MASK;
    return value;
}

/*
 * The queue head's second characteristics dword (EHCI 1.0 section 3.6, dword
 * 2): the high-bandwidth multiplier in bits 31:30, the port number in bits
 * 29:23 and the hub address in bits 22:16.  This driver binds devices on the
 * controller's own root ports, so the hub address is zero and the port number
 * matters only for full/low-speed devices behind a transaction translator -
 * none of which this path binds; it is encoded anyway so a record can prove
 * what was written.
 */
uint32_t ehci_qh_endpoint2(uint8_t multiplier, uint8_t hub, uint8_t port) {
    return (((uint32_t)multiplier & 0x03U) << EHCI_QH_MULTIPLIER_SHIFT) |
           (((uint32_t)hub & 0x7FU) << EHCI_QH_HUB_SHIFT) |
           (((uint32_t)port & 0x7FU) << EHCI_QH_PORT_SHIFT);
}

/* A queue head link: the head's own address with the QH type (01 in bits 2:1)
   and, when asked for, the terminate bit that ends the list. */
uint32_t ehci_link(uint32_t physical, bool terminate) {
    if (terminate) {
        return EHCI_QH_TERMINATE;
    }
    return (physical & EHCI_QH_LINK_MASK) | EHCI_QH_LINK_TYPE_QH;
}

bool ehci_port_connected(uint32_t portsc) {
    return (portsc & EHCI_PORTSC_CCS) != 0U;
}

bool ehci_port_enabled(uint32_t portsc) {
    return (portsc & EHCI_PORTSC_PE) != 0U;
}

bool ehci_port_owned_by_companion(uint32_t portsc) {
    return (portsc & EHCI_PORTSC_PO) != 0U;
}

uint8_t ehci_port_speed_code(uint32_t portsc) {
    return (uint8_t)((portsc & EHCI_PORTSC_PS_MASK) >> EHCI_PORTSC_PS_SHIFT);
}

bool ehci_port_high_speed(uint32_t portsc) {
    return (portsc & EHCI_PORTSC_PS_MASK) == EHCI_PORTSC_PS_HIGH;
}

/*
 * PORTSC as this driver writes it (EHCI 1.0 section 2.3.9).  Only these fields
 * are writable: the port's power state (a port must stay powered), the four
 * change bits (written as one, which clears them), Port Reset, Port Owner and
 * the suspend bits.  Everything else - the connect status, the enable status,
 * the speed, the line status - is read-only and a write that carried it back
 * would be a write of whatever the controller last said, which is exactly the
 * class of bug the xHCI PORTSC work (ITEM-406) already cost this project.  The
 * caller passes the bits it wants set; a bit it does not name is written as
 * zero, which is what clears Port Reset.
 */
uint32_t ehci_portsc_write_value(uint32_t portsc, uint32_t set_bits) {
    uint32_t value = portsc & (EHCI_PORTSC_PP | EHCI_PORTSC_CHANGE);

    value |= set_bits & (EHCI_PORTSC_PR | EHCI_PORTSC_PO | EHCI_PORTSC_SUSP |
                         EHCI_PORTSC_FPR);
    return value & EHCI_PORTSC_WRITABLE;
}

uint32_t ehci_portsc_power_value(uint32_t portsc) {
    /*
     * Setting PP is the one write that separates "the port is empty" from "the
     * port was never powered": the connect bit of an unpowered port reads zero
     * exactly like an empty one.  The change bits are written back, as the
     * other port writes do, so this write clears no event the reader wanted.
     */
    return ((portsc & EHCI_PORTSC_CHANGE) | EHCI_PORTSC_PP) &
           EHCI_PORTSC_WRITABLE;
}

/*
 * The descriptor plan of one transfer.
 *
 * Two rules decide the split, and both come from the descriptor's own shape:
 *
 *   * a transfer descriptor addresses at most five pages of a physically
 *     contiguous buffer, and its first buffer pointer may be unaligned, so the
 *     chunk this driver plans is bounded by four whole pages;
 *   * a USB packet may not start in one descriptor and finish in the next, so
 *     every chunk but the last is a whole number of packets.  A short packet
 *     therefore always ends a chunk and is always visible to the controller as
 *     the end of the transfer, which is what the BOT transport relies on for
 *     its CSW (13 bytes) and for a short data-in.
 *
 * The buffer the hardware half uses is page-aligned (its own bounce page), so
 * the plan does not need to know the offset; `length` zero is one empty
 * descriptor, the status stage of a control transfer.
 */
bool ehci_plan_transfer(uint32_t length,
                        uint32_t max_packet,
                        struct ehci_transfer_plan *plan) {
    uint32_t remaining;
    uint32_t index = 0U;

    if (plan == NULL || max_packet == 0U) {
        return false;
    }
    plan->qtd_count = 0U;
    plan->total_bytes = length;
    if (length == 0U) {
        plan->qtd_count = 1U;
        plan->length[0] = 0U;
        plan->packets[0] = 0U;
        return true;
    }
    remaining = length;
    while (remaining != 0U) {
        uint32_t chunk;

        if (index == EHCI_MAX_QTD_PER_TRANSFER) {
            /* More than the transfer bound: refuse rather than silently drop
               the tail, so the caller splits the request instead. */
            plan->qtd_count = 0U;
            return false;
        }
        chunk = remaining;
        if (chunk > EHCI_TRANSFER_MAX_BYTES) {
            chunk = EHCI_TRANSFER_MAX_BYTES;
            /* Whole packets only when another chunk follows. */
            chunk -= chunk % max_packet;
            if (chunk == 0U) {
                plan->qtd_count = 0U;
                return false;
            }
        }
        plan->length[index] = chunk;
        plan->packets[index] = (chunk + max_packet - 1U) / max_packet;
        remaining -= chunk;
        ++index;
    }
    plan->qtd_count = index;
    return true;
}

uint32_t ehci_plan_offset(const struct ehci_transfer_plan *plan,
                          uint32_t index) {
    uint32_t offset = 0U;

    if (plan == NULL) {
        return 0U;
    }
    for (uint32_t step = 0U; step < index && step < plan->qtd_count; ++step) {
        offset += plan->length[step];
    }
    return offset;
}

uint32_t ehci_plan_packets(const struct ehci_transfer_plan *plan,
                           uint32_t index) {
    uint32_t packets = 0U;

    if (plan == NULL) {
        return 0U;
    }
    for (uint32_t step = 0U; step < index && step < plan->qtd_count; ++step) {
        packets += plan->packets[step];
    }
    return packets;
}
