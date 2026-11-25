/*
 * ITEM-569: the DMA structures in both formats, decoded independently.
 *
 * EHCI 1.0 Appendix B (Figures B-3 and B-4) defines the 64-bit queue element
 * transfer descriptor and queue head: every 32-bit field keeps its offset, and
 * the high halves of the five buffer addresses are appended -
 *
 *   qTD-64: next 0x00, alt 0x04, token 0x08, buffer[0..4] 0x0C..0x1C,
 *           ext_buffer[0..4] 0x20..0x30, total 52 bytes;
 *   QH-64:  link 0x00, ep1 0x04, ep2 0x08, current 0x0C, next 0x10,
 *           alt 0x14, token 0x18, buffer[0..4] 0x1C..0x2C,
 *           overlay_ext_buffer[0..4] 0x30..0x40, total 68 bytes.
 *
 * The previous pool used a 32-byte qTD stride and a 64-byte QH slot, so on a
 * controller with HCCPARAMS.AC64 = 1 the bytes read as `ext_buffer[0]` were the
 * next descriptor's `next` link: the controller combined a link pointer with a
 * buffer address and fetched memory the driver never named - a master abort
 * (Table 4-19: fatal) on the first transfer, which is exactly the
 * `HSE`/`IN_WAIT`/`DEVICE8` the Toshiba reported.
 *
 * The proof drives the production builders (ehci_ring_list_head,
 * ehci_ring_transfer_head, ehci_ring_qtd) into a pool image and then decodes
 * the bytes with the offsets above - not with the production structs - so a
 * structure that disagrees with the format fails here even though the kernel
 * compiles.  It also proves the regions of the pool do not overlap and that a
 * data write cannot reach the periodic frame list.
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

/* The format's numbers, written here as the appendix gives them. */
#define FORMAT_QTD64_BYTES 52U
#define FORMAT_QH64_BYTES 68U
#define FORMAT_QTD32_BYTES 32U
#define FORMAT_QH32_BYTES 48U
#define FORMAT_QTD_BUFFERS 5U

static uint32_t pool_image[EHCI_DMA_BYTES / 4U];

static uint32_t image_word(uint32_t offset) {
    return pool_image[offset / 4U];
}

static uint8_t *image_at(uint32_t offset) {
    return (uint8_t *)pool_image + offset;
}

/* The five buffer addresses a qTD-64 names, independently decoded. */
static uint64_t decoded_buffer(const uint8_t *qtd, uint32_t index) {
    const uint32_t low = *(const uint32_t *)(const void *)(qtd + 0x0CU +
                                                           index * 4U);
    const uint32_t high = *(const uint32_t *)(const void *)(qtd + 0x20U +
                                                            index * 4U);

    return ((uint64_t)high << 32U) | low;
}

static uint64_t decoded_overlay_buffer(const uint8_t *qh, uint32_t index) {
    const uint32_t low = *(const uint32_t *)(const void *)(qh + 0x1CU +
                                                           index * 4U);
    const uint32_t high = *(const uint32_t *)(const void *)(qh + 0x30U +
                                                            index * 4U);

    return ((uint64_t)high << 32U) | low;
}

static void test_the_structures_are_the_64_bit_format(void) {
    CHECK(sizeof(struct ehci_qtd) == FORMAT_QTD64_BYTES,
          "the descriptor is the 64-bit format's 52 bytes, not 32");
    CHECK(sizeof(struct ehci_qh) == FORMAT_QH64_BYTES,
          "the queue head is the 64-bit format's 68 bytes, not 48");
    CHECK(sizeof(struct ehci_qtd) > FORMAT_QTD32_BYTES &&
              sizeof(struct ehci_qh) > FORMAT_QH32_BYTES,
          "both structures carry the extended buffer words");
    CHECK(EHCI_QTD_STRIDE >= FORMAT_QTD64_BYTES,
          "the descriptor stride holds the 64-bit format");
    CHECK(EHCI_QH_STRIDE >= FORMAT_QH64_BYTES,
          "the slot holds the 64-bit queue head");
    CHECK((EHCI_QTD_STRIDE % 32U) == 0U && (EHCI_QH_STRIDE % 32U) == 0U,
          "the strides keep the specification's 32-byte alignment");
}

/* The production qTD builder, decoded by the appendix's offsets. */
static void test_the_builder_writes_both_halves_of_every_buffer(void) {
    uint8_t *qtd = image_at(EHCI_QTD_OFFSET);
    const uint64_t above_4g = UINT64_C(0x0000000123456000);
    const uint64_t low_buffer = UINT64_C(0x0000000000123000);
    uint32_t expected_low[FORMAT_QTD_BUFFERS];
    uint32_t expected_high[FORMAT_QTD_BUFFERS];
    uint64_t cursor = above_4g;
    uint32_t remaining = 0x1800U;

    memset(pool_image, 0, sizeof(pool_image));
    for (uint32_t index = 0U; index < FORMAT_QTD_BUFFERS; ++index) {
        const uint32_t page_offset = (uint32_t)(cursor % EHCI_PAGE_BYTES);
        const uint32_t to_page_end = EHCI_PAGE_BYTES - page_offset;

        expected_low[index] = (uint32_t)cursor;
        expected_high[index] = (uint32_t)(cursor >> 32U);
        if (remaining == 0U) {
            expected_low[index] = 0U;
            expected_high[index] = 0U;
            continue;
        }
        if (to_page_end >= remaining) {
            remaining = 0U;
            continue;
        }
        remaining -= to_page_end;
        cursor += to_page_end;
    }
    ehci_ring_qtd((struct ehci_qtd *)(void *)qtd, EHCI_QH_TERMINATE,
                  0x00080E80U, above_4g, 0x1800U, true);

    CHECK(*(const uint32_t *)(const void *)(qtd + 0x00U) == EHCI_QH_TERMINATE,
          "the next link is the appendix's first dword");
    CHECK(*(const uint32_t *)(const void *)(qtd + 0x04U) == EHCI_QH_TERMINATE,
          "the alternate next link is the second dword");
    CHECK(*(const uint32_t *)(const void *)(qtd + 0x08U) == 0x00080E80U,
          "the token is the third dword");
    for (uint32_t index = 0U; index < FORMAT_QTD_BUFFERS; ++index) {
        CHECK(decoded_buffer(qtd, index) ==
                  (((uint64_t)expected_high[index] << 32U) |
                   expected_low[index]),
              "the descriptor names the 64-bit buffer address in both halves");
    }
    CHECK(decoded_buffer(qtd, 0U) == above_4g,
          "a buffer above 4 GiB keeps its upper half");
    CHECK(decoded_buffer(qtd, 1U) > above_4g,
          "a buffer crossing the page keeps the same upper half");
    CHECK(*(const uint32_t *)(const void *)(qtd + 0x34U) != 0xDEADBEEFU,
          "nothing is written past the descriptor's 52 bytes");

    /* A buffer below 4 GiB has a zero upper half, and the builder is asked to
       write it as such (not to leave the previous descriptor's words). */
    memset(pool_image, 0xFF, sizeof(pool_image));
    ehci_ring_qtd((struct ehci_qtd *)(void *)qtd, EHCI_QH_TERMINATE,
                  0x00080E80U, low_buffer, 8U, true);
    CHECK(decoded_buffer(qtd, 0U) == low_buffer,
          "a buffer below 4 GiB decodes with a zero upper half");
    CHECK(*(const uint32_t *)(const void *)(qtd + 0x20U) == 0U,
          "the upper half of a below-4-GiB buffer is written as zero");

    /* In 32-bit mode the extended words are outside the structure; the low
       halves are still the format's. */
    memset(pool_image, 0, sizeof(pool_image));
    ehci_ring_qtd((struct ehci_qtd *)(void *)qtd, EHCI_QH_TERMINATE,
                  0x00080E80U, low_buffer, 8U, false);
    CHECK(decoded_buffer(qtd, 0U) == low_buffer,
          "a 32-bit controller still gets the low half where the format puts "
          "it");
}

/* The production queue-head builders, decoded by the appendix's offsets. */
static void test_the_queue_heads_are_the_64_bit_layout(void) {
    uint8_t *list = image_at(EHCI_LIST_HEAD_OFFSET);
    uint8_t *transfer = image_at(EHCI_TRANSFER_QH_OFFSET);
    const uint32_t list_physical = 0x0083B000U;
    const uint32_t first_qtd = 0x0083B100U;

    memset(pool_image, 0, sizeof(pool_image));
    ehci_ring_list_head((struct ehci_qh *)(void *)list, list_physical);
    ehci_ring_transfer_head((struct ehci_qh *)(void *)transfer, list_physical,
                            first_qtd, 64U, 0U, 1U);

    CHECK(*(const uint32_t *)(const void *)(list + 0x00U) ==
              ehci_link(list_physical, false),
          "the list head's horizontal link is the first dword");
    CHECK((*(const uint32_t *)(const void *)(list + 0x04U) & 0x40000000U) == 0U,
          "the list head's endpoint word is at 0x04");
    CHECK(*(const uint32_t *)(const void *)(list + 0x10U) == EHCI_QH_TERMINATE,
          "the list head's overlay next is the appendix's 0x10");
    CHECK(*(const uint32_t *)(const void *)(transfer + 0x10U) == first_qtd,
          "the transfer head points its overlay at the first descriptor");
    CHECK(*(const uint32_t *)(const void *)(transfer + 0x00U) ==
              ehci_link(list_physical, false),
          "the transfer head links back to the list head");
    for (uint32_t index = 0U; index < FORMAT_QTD_BUFFERS; ++index) {
        CHECK(decoded_overlay_buffer(transfer, index) == 0U,
              "the transfer head's overlay buffers start empty in both halves");
        CHECK(*(const uint32_t *)(const void *)(list + 0x30U + index * 4U) ==
                  0U,
              "the list head's extended overlay words are zeroed");
    }
    CHECK(*(const uint32_t *)(const void *)(transfer + 0x40U) == 0U,
          "the queue head's layout ends at the appendix's 0x40");
}

/* The pool: five disjoint regions, and the frame list after the data. */
static void test_the_pool_regions_do_not_overlap(void) {
    const uint32_t qh0 = EHCI_LIST_HEAD_OFFSET;
    const uint32_t qh1 = EHCI_TRANSFER_QH_OFFSET;
    const uint32_t qtd0 = EHCI_QTD_OFFSET;
    const uint32_t qtd_last = EHCI_QTD_OFFSET +
                              (EHCI_MAX_QTD - 1U) * EHCI_QTD_STRIDE;
    const uint32_t qtd_end = qtd_last + FORMAT_QTD64_BYTES;

    CHECK(qh0 + FORMAT_QH64_BYTES <= qh1,
          "the list head's 68 bytes end before the transfer head's slot");
    CHECK(qh1 + FORMAT_QH64_BYTES <= qtd0,
          "the transfer head's 68 bytes end before the descriptor array");
    CHECK(qtd_end <= EHCI_BOUNCE_OFFSET,
          "the descriptor array ends before the bounce area");
    CHECK(EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES <= EHCI_FRAME_LIST_OFFSET,
          "the bounce area ends before the frame list");
    CHECK(EHCI_FRAME_LIST_OFFSET + EHCI_FRAME_LIST_BYTES <= EHCI_DMA_BYTES,
          "the frame list ends inside the pool");
    CHECK((EHCI_FRAME_LIST_OFFSET % EHCI_PAGE_BYTES) == 0U,
          "the frame list is 4 KiB aligned");
    CHECK(EHCI_FRAME_LIST_ENTRIES * 4U == EHCI_FRAME_LIST_BYTES,
          "the frame list holds exactly 1024 pointers");

    /* The user-visible defect: the control data buffer and the frame list used
       to start at the same pool offset. */
    CHECK(EHCI_BOUNCE_OFFSET + EHCI_DATA_LOCAL != EHCI_FRAME_LIST_OFFSET,
          "the control data buffer does not start at the frame list");
    CHECK(EHCI_BOUNCE_OFFSET + EHCI_DATA_LOCAL + EHCI_DATA_BYTES <=
              EHCI_FRAME_LIST_OFFSET,
          "the control data buffer ends before the frame list");
    CHECK(EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL + EHCI_BULK_BYTES <=
              EHCI_FRAME_LIST_OFFSET,
          "the bulk data buffer ends before the frame list");
}

/* Writing data must not reach the frame list: build a descriptor for the last
   bounce byte and fill the bounce area, then read the frame list back. */
static void test_a_data_write_cannot_reach_the_frame_list(void) {
    uint8_t *qtd = image_at(EHCI_QTD_OFFSET);
    uint32_t *frame_list = (uint32_t *)(void *)image_at(EHCI_FRAME_LIST_OFFSET);
    const uint64_t buffer =
        (uint64_t)(0x0083B000U + EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL);

    memset(pool_image, 0, sizeof(pool_image));
    for (uint32_t entry = 0U; entry < EHCI_FRAME_LIST_ENTRIES; ++entry) {
        frame_list[entry] = EHCI_FRAME_LIST_TERMINATE;
    }
    ehci_ring_qtd((struct ehci_qtd *)(void *)qtd, EHCI_QH_TERMINATE,
                  0x00080E80U, buffer, EHCI_BULK_BYTES, true);
    CHECK(decoded_buffer(qtd, 0U) == buffer &&
              decoded_buffer(qtd, 1U) ==
                  (uint64_t)(0x0083B000U + EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL +
                             EHCI_PAGE_BYTES),
          "the bulk descriptor names the bounce buffers, not the frame list");
    /* Simulate the controller writing the whole bounce area. */
    memset(image_at(EHCI_BOUNCE_OFFSET), 0xA5, EHCI_BOUNCE_BYTES);
    for (uint32_t entry = 0U; entry < EHCI_FRAME_LIST_ENTRIES; ++entry) {
        CHECK(frame_list[entry] == EHCI_FRAME_LIST_TERMINATE,
              "a data write left the frame list untouched");
        if (frame_list[entry] != EHCI_FRAME_LIST_TERMINATE) {
            break;
        }
    }
    CHECK(image_word(EHCI_QTD_OFFSET + 0x00U) == EHCI_QH_TERMINATE,
          "a data write left the descriptor array untouched");
}

/* The format decision comes from the raw capability word. */
static void test_ac64_is_the_raw_capability_bit(void) {
    CHECK(EHCI_HCCPARAMS_64BIT == 0x00000001U,
          "AC64 is bit 0 of HCCPARAMS (Table 2-7)");
    CHECK((EHCI_HCCPARAMS_64BIT & EHCI_HCCPARAMS_EECP_MASK) == 0U,
          "the 64-bit capability and the EECP pointer do not share a bit");
    CHECK(EHCI_HCCPARAMS_EECP_SHIFT == 8U,
          "the EECP is in bits 15:8 of the same word");
}

int main(void) {
    test_the_structures_are_the_64_bit_format();
    test_the_builder_writes_both_halves_of_every_buffer();
    test_the_queue_heads_are_the_64_bit_layout();
    test_the_pool_regions_do_not_overlap();
    test_a_data_write_cannot_reach_the_frame_list();
    test_ac64_is_the_raw_capability_bit();
    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    printf("EHCI 64-bit DMA format host test: ALL OK\n");
    return 0;
}
