/*
 * ITEM-560 host proof: the asynchronous ring and the first start word, built by
 * the production code and checked field by field.
 *
 * The hardware run of ITEM-559 named a Host System Error but could not say
 * whether the controller had ever been a bus master, whether the ring it walked
 * was well formed, or whether a schedule the driver does not build was running.
 * The checks below are the ones that question asks for, on the code the machine
 * runs:
 *
 *   * the pool the controller must reach is inside the address width the
 *     controller has (32 bits without HCCPARAMS.AC64);
 *   * the list head is a high-speed queue head with the head-of-reclamation
 *     bit, Mult = 1, a self link of type QH, a terminated and halted overlay;
 *   * the transfer head links back to the list head, names the first
 *     descriptor, has C = 0, DTC = 1 and Mult = 1;
 *   * the descriptors chain to each other and terminate, and a buffer that
 *     crosses a page boundary gets one pointer per page;
 *   * the structures fit their slots and never cross a page;
 *   * the first start word keeps the interrupt threshold and clears the
 *     periodic schedule enable - the state that was found running on the
 *     laptop's controller although this driver never builds a periodic
 *     schedule.
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

static uint32_t field(uint32_t value, uint32_t shift, uint32_t mask) {
    return (value >> shift) & mask;
}

static void test_reachability(void) {
    CHECK(ehci_ring_reachable(0x00100000ULL, 0x6000U, false),
          "a pool below 4 GiB is reachable without the 64-bit capability");
    CHECK(!ehci_ring_reachable(UINT64_C(0x100000000), 0x1000U, false),
          "a pool at 4 GiB is not addressable by a 32-bit controller");
    CHECK(!ehci_ring_reachable(UINT64_C(0x0FFFFFF80), 0x1000U, false),
          "a pool that ends above 4 GiB is not addressable either");
    CHECK(ehci_ring_reachable(UINT64_C(0x100000000), 0x1000U, true),
          "the 64-bit capability lets the controller reach above 4 GiB");
    CHECK(!ehci_ring_reachable(0U, 0x1000U, true),
          "a zero base is not a pool");
    CHECK(!ehci_ring_reachable(0x100000ULL, 0U, true),
          "an empty pool is not reachable");
}

static void test_list_head(void) {
    struct ehci_qh qh;
    const uint32_t physical = 0x00100000U;

    memset(&qh, 0xAA, sizeof(qh));
    ehci_ring_list_head(&qh, physical);

    CHECK(qh.link == ((physical & EHCI_QH_LINK_MASK) | EHCI_QH_LINK_TYPE_QH),
          "the list head links to itself as a queue head");
    CHECK((qh.link & EHCI_QH_TERMINATE) == 0U,
          "the self link is not a terminator");
    CHECK(field(qh.endpoint1, 15U, 1U) == 1U,
          "the list head is the head of the reclamation list");
    CHECK(field(qh.endpoint1, EHCI_QH_SPEED_SHIFT, 0x3U) ==
              EHCI_QH_SPEED_HIGH,
          "the list head is a high-speed queue head");
    CHECK(field(qh.endpoint2, EHCI_QH_MULTIPLIER_SHIFT, 0x3U) == 1U,
          "the list head carries Mult = 1 as EHCI 1.0 section 3.6.2 requires");
    CHECK(qh.overlay_next == EHCI_QH_TERMINATE &&
              qh.overlay_alt_next == EHCI_QH_TERMINATE,
          "an empty head terminates both overlay pointers");
    CHECK((qh.overlay_token & EHCI_QTD_STATUS_HALTED) != 0U &&
              (qh.overlay_token & EHCI_QTD_STATUS_ACTIVE) == 0U,
          "an empty head's overlay is halted and not active");
    CHECK(qh.current == 0U, "the current pointer starts clear");
    for (uint32_t index = 0U; index < EHCI_QTD_BUFFERS; ++index) {
        CHECK(qh.overlay_buffer[index] == 0U,
              "the overlay buffers are not confused with addresses");
    }
}

static void test_transfer_head(void) {
    struct ehci_qh qh;
    const uint32_t list_head = 0x00100000U;
    const uint32_t first_qtd = 0x00101000U;

    memset(&qh, 0xAA, sizeof(qh));
    ehci_ring_transfer_head(&qh, list_head, first_qtd, 64U, 0U, 0U);

    CHECK(qh.link == ((list_head & EHCI_QH_LINK_MASK) | EHCI_QH_LINK_TYPE_QH),
          "the transfer head links back to the list head as a queue head");
    CHECK(qh.overlay_next == first_qtd,
          "the transfer head names the first descriptor");
    CHECK(qh.overlay_alt_next == EHCI_QH_TERMINATE,
          "the alternate pointer is a terminator");
    CHECK((qh.overlay_token & EHCI_QTD_STATUS_ACTIVE) == 0U,
          "the transfer head's own overlay is not active");
    CHECK(field(qh.endpoint1, EHCI_QH_SPEED_SHIFT, 0x3U) ==
              EHCI_QH_SPEED_HIGH,
          "the transfer head is high speed");
    CHECK(field(qh.endpoint1, EHCI_QH_MAX_PACKET_SHIFT, 0x7FFU) == 64U,
          "the transfer head carries the endpoint's maximum packet");
    CHECK(field(qh.endpoint1, 27U, 1U) == 0U,
          "a high-speed control pipe carries C = 0");
    CHECK(field(qh.endpoint1, 14U, 1U) == 1U,
          "the toggle comes from the descriptors (DTC = 1)");
    CHECK(field(qh.endpoint2, EHCI_QH_MULTIPLIER_SHIFT, 0x3U) == 1U,
          "the transfer head carries Mult = 1");
    CHECK((qh.endpoint1 & EHCI_QH_ADDRESS_MASK) == 0U &&
              (qh.endpoint1 & EHCI_QH_ENDPOINT_MASK) == 0U,
          "address and endpoint are carried in their fields");
}

static void test_descriptor_chain_and_pages(void) {
    struct ehci_qtd first;
    struct ehci_qtd second;
    const uint32_t token = 0x80008C80U;

    memset(&first, 0xAA, sizeof(first));
    ehci_ring_qtd(&first, 0x00101020U, token, 0x00102000U, 8U, false);
    CHECK(first.next == 0x00101020U, "the descriptor names its successor");
    CHECK(first.alt_next == EHCI_QH_TERMINATE,
          "the alternate pointer is a terminator");
    CHECK(first.token == token, "the token is carried unchanged");
    CHECK(first.buffer[0] == 0x00102000U,
          "a buffer inside one page needs one pointer");
    CHECK(first.buffer[1] == 0U && first.buffer[2] == 0U &&
              first.buffer[3] == 0U && first.buffer[4] == 0U,
          "unused page pointers stay zero");

    /* A buffer that starts 0x800 into its page and runs 0x900 bytes uses two
       pages: 0x800 bytes in the first, 0x100 in the second. */
    ehci_ring_qtd(&second, EHCI_QH_TERMINATE, token, 0x00102800U,
                  0x900U, false);
    CHECK(second.next == EHCI_QH_TERMINATE,
          "the last descriptor terminates the chain");
    CHECK(second.buffer[0] == 0x00102800U &&
              second.buffer[1] == 0x00103000U,
          "a buffer crossing a page gets the next page's pointer");
    CHECK(second.buffer[2] == 0U,
          "the pointer after the last used page stays zero");
}

static void test_slots_and_alignment(void) {
    /* ITEM-569: the structures are the 64-bit format; the slots are the stride
       the driver walks them at, and they are not sizeof(struct). */
    CHECK(sizeof(struct ehci_qtd) == EHCI_QTD64_BYTES,
          "a transfer descriptor is the 64-bit format's 52 bytes");
    CHECK(sizeof(struct ehci_qh) == EHCI_QH64_BYTES,
          "a queue head is the 64-bit format's 68 bytes");
    CHECK(sizeof(struct ehci_qtd) <= EHCI_QTD_STRIDE &&
              sizeof(struct ehci_qh) <= EHCI_QH_STRIDE,
          "each structure fits its slot");
    CHECK(EHCI_DMA_BYTES % EHCI_PAGE_BYTES == 0U,
          "the pool is a whole number of pages");
    CHECK((EHCI_LIST_HEAD_OFFSET % 32U) == 0U &&
              (EHCI_TRANSFER_QH_OFFSET % 32U) == 0U &&
              (EHCI_QTD_OFFSET % 32U) == 0U,
          "every structure is 32-byte aligned inside the pool");
    CHECK(EHCI_TRANSFER_QH_OFFSET >= EHCI_QH_STRIDE,
          "the two queue heads do not share a slot");
    CHECK(EHCI_QTD_OFFSET >= 2U * EHCI_QH_STRIDE,
          "the descriptor array does not overlap the queue heads");
    CHECK(((EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES) <= EHCI_DMA_BYTES),
          "the bounce area ends inside the pool");
    CHECK(EHCI_FRAME_LIST_OFFSET >= EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES &&
              EHCI_FRAME_LIST_OFFSET + EHCI_FRAME_LIST_BYTES <= EHCI_DMA_BYTES,
          "the periodic frame list is outside the bounce area and inside the "
          "pool");
}

static void test_first_start_word(void) {
    /* The value the laptop's controller reported: ITC = 8, PSE, ASE, IAA. */
    const uint32_t reported = 0x00080070U;
    const uint32_t start = ehci_ring_usbcmd_start(reported);

    CHECK((start & EHCI_USBCMD_RS) != 0U, "the start word runs the controller");
    CHECK((start & EHCI_USBCMD_ASE) != 0U,
          "the start word enables the asynchronous schedule");
    CHECK((start & EHCI_USBCMD_PSE) == 0U,
          "the start word clears the periodic schedule this driver never builds");
    CHECK((start & EHCI_USBCMD_IAAD) == 0U,
          "a stale async-advance doorbell is not carried into the start");
    CHECK((start & EHCI_USBCMD_ITC_MASK) ==
              (reported & EHCI_USBCMD_ITC_MASK),
          "the interrupt threshold the controller reported is kept");
    CHECK(start == (EHCI_USBCMD_RS | EHCI_USBCMD_ASE | 0x00080000U),
          "the start word is exactly run, async and the threshold");
}

int main(void) {
    test_reachability();
    test_list_head();
    test_transfer_head();
    test_descriptor_chain_and_pages();
    test_slots_and_alignment();
    test_first_start_word();

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI ring host test: ALL OK\n");
    return 0;
}
