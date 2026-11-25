#ifndef HFOC_EHCI_H
#define HFOC_EHCI_H

/*
 * ITEM-551: the USB 2.0 half the machine actually boots from.
 *
 * The owner's Toshiba L875 boots the diagnostic stick through the firmware and
 * then has no way to reach it from the kernel: the stick sits in the machine's
 * only USB 2 port, that port is owned by an Intel 7 Series EHCI function
 * (`00:1a.0`/`00:1d.0` in the Linux capture of the same machine:
 * `Bus 001: ehci-pci/2p 480M`), and until this item the kernel carried no EHCI
 * driver at all - only xHCI.  The xHCI controller on this machine is the one
 * failing at `ENABLE_SLOT`, so the one medium the machine can be given is also
 * the one the kernel cannot see.
 *
 * This is the minimal path that fixes that, in the shape the existing storage
 * stack already uses: a polling EHCI host controller (no interrupt is enabled
 * and none is routed - `USBINTR` stays zero, every wait is a bounded poll of
 * the completion the controller writes into memory), a control pipe for the
 * standard requests, a bulk pipe for the BOT transport, and then the *same*
 * `usb_storage` core and `usb_storage_block_device` view the xHCI path uses.
 * The medium identity guard and the reserved `HFOC/INSTALL.LOG` binding are the
 * same ones `install_medium_identify()` and
 * `install_journal_platform_bind_medium()` provide; nothing here allocates on
 * a filesystem and nothing here writes outside the file the medium's build
 * reserved.
 *
 * The registers, the queue head and the queue element transfer descriptor are
 * plain little-endian 32-bit structures (EHCI 1.0 sections 2.2, 3.1 and 3.5).
 * The pure half - the two descriptors' fields, the transfer planner and the
 * port-state decoding - lives in kernel/src/ehci_queue.c and is proven on the
 * host; this header is the seam between that half and the hardware half.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "block_device.h"

/* The PCI function this driver owns: USB, EHCI programming interface. */
#define EHCI_PCI_CLASS_SERIAL_BUS 0x0CU
#define EHCI_PCI_SUBCLASS_USB 0x03U
#define EHCI_PCI_PROGIF_EHCI 0x20U
#define EHCI_MAX_CONTROLLERS 4U
#define EHCI_MAX_PORTS 15U

/* Capability registers, relative to the memory BAR. */
#define EHCI_CAP_CAPLENGTH 0x00U
#define EHCI_CAP_HCIVERSION 0x02U
#define EHCI_CAP_CAPLENGTH_VERSION 0x00U
#define EHCI_CAPLENGTH_MASK 0x000000FFU
#define EHCI_CAP_HCSPARAMS 0x04U
#define EHCI_CAP_HCCPARAMS 0x08U
#define EHCI_CAP_HCSP_PORTROUTE 0x0CU

/*
 * Operational registers, relative to CAPLENGTH: EHCI 1.0 section 2.3 puts the
 * operational base at the BAR plus CAPLENGTH, and these are the offsets inside
 * it (USBCMD is its first dword).  The absolute offsets a reader of the
 * specification sees - USBCMD at 0x20, PORTSC at 0x64 - are these values plus
 * the 0x20 CAPLENGTH the controller declares.
 */
#define EHCI_OP_USBCMD 0x00U
#define EHCI_OP_USBSTS 0x04U
#define EHCI_OP_USBINTR 0x08U
#define EHCI_OP_FRINDEX 0x0CU
#define EHCI_OP_CTRLDSSEGMENT 0x10U
#define EHCI_OP_PERIODICLISTBASE 0x14U
#define EHCI_OP_ASYNCLISTADDR 0x18U
#define EHCI_OP_CONFIGFLAG 0x40U
#define EHCI_OP_PORTSC 0x44U
#define EHCI_OP_PORTSC_STRIDE 0x04U

#define EHCI_USBCMD_RS 0x00000001U
#define EHCI_USBCMD_HCRESET 0x00000002U
#define EHCI_USBCMD_PSE 0x00000010U
#define EHCI_USBCMD_ASE 0x00000020U
#define EHCI_USBCMD_IAAD 0x00000040U
#define EHCI_USBCMD_ITC_MASK 0x00FF0000U

#define EHCI_USBSTS_UI 0x00000001U
#define EHCI_USBSTS_UEE 0x00000002U
#define EHCI_USBSTS_PCD 0x00000004U
#define EHCI_USBSTS_HSE 0x00000010U
#define EHCI_USBSTS_IAA 0x00000020U
#define EHCI_USBSTS_HCH 0x00001000U
#define EHCI_USBSTS_PSS 0x00004000U
#define EHCI_USBSTS_ASS 0x00008000U
/* The status bits that mean the controller stopped on its own. */
#define EHCI_USBSTS_ERROR                                                                \
    (EHCI_USBSTS_UEE | EHCI_USBSTS_HSE | EHCI_USBSTS_PCD)

#define EHCI_CONFIGFLAG_CF 0x00000001U

/* HCSPARAMS: N_PORTS is bits 3:0, Port Power Control is bit 4 (Table 2-6). */
#define EHCI_HCSPARAMS_PORTS_MASK 0x0000000FU
#define EHCI_HCSPARAMS_PPC 0x00000010U
/* HCCPARAMS: 64-bit addressing is bit 0, EECP is bits 15:8 and is an offset
   into PCI configuration space (Table 2-7). */
#define EHCI_HCCPARAMS_64BIT 0x00000001U
#define EHCI_HCCPARAMS_EECP_SHIFT 8U
#define EHCI_HCCPARAMS_EECP_MASK 0x0000FF00U
/* USBSTS: the status bit that says the controller stopped executing. */
#define EHCI_USBSTS_RECL 0x00002000U

#define EHCI_PORTSC_CCS 0x00000001U
#define EHCI_PORTSC_CSC 0x00000002U
#define EHCI_PORTSC_PE 0x00000004U
#define EHCI_PORTSC_PEC 0x00000008U
#define EHCI_PORTSC_OCA 0x00000010U
#define EHCI_PORTSC_OCC 0x00000020U
#define EHCI_PORTSC_FPR 0x00000040U
#define EHCI_PORTSC_SUSP 0x00000080U
#define EHCI_PORTSC_PR 0x00000100U
#define EHCI_PORTSC_LS_MASK 0x00000C00U
#define EHCI_PORTSC_LS_LOW 0x00000400U
#define EHCI_PORTSC_PP 0x00001000U
#define EHCI_PORTSC_PO 0x00002000U
#define EHCI_PORTSC_PS_MASK 0x0C000000U
#define EHCI_PORTSC_PS_SHIFT 26U
#define EHCI_PORTSC_PS_FULL 0x00000000U
#define EHCI_PORTSC_PS_LOW 0x04000000U
#define EHCI_PORTSC_PS_HIGH 0x08000000U
/* The change bits software clears by writing them back as one. */
#define EHCI_PORTSC_CHANGE (EHCI_PORTSC_CSC | EHCI_PORTSC_PEC | EHCI_PORTSC_OCC)
/* The bits this driver ever writes: the port stays powered, the change bits are
   cleared by writing one, and only the reset/ownership bits are set on purpose. */
#define EHCI_PORTSC_WRITABLE                                                        \
    (EHCI_PORTSC_PR | EHCI_PORTSC_PO | EHCI_PORTSC_PP | EHCI_PORTSC_CHANGE |        \
     EHCI_PORTSC_SUSP | EHCI_PORTSC_FPR)

/* Queue element transfer descriptor token (EHCI 1.0 section 3.5.3). */
#define EHCI_QTD_DT 0x80000000U
#define EHCI_QTD_TOTAL_SHIFT 16U
#define EHCI_QTD_TOTAL_MASK 0x7FFF0000U
#define EHCI_QTD_IOC 0x00008000U
#define EHCI_QTD_CERR_SHIFT 10U
#define EHCI_QTD_CERR_MASK 0x00000C00U
#define EHCI_QTD_PID_SHIFT 8U
#define EHCI_QTD_PID_MASK 0x00000300U
#define EHCI_QTD_PID_OUT 0U
#define EHCI_QTD_PID_IN 1U
#define EHCI_QTD_PID_SETUP 2U
#define EHCI_QTD_STATUS_ACTIVE 0x00000080U
#define EHCI_QTD_STATUS_HALTED 0x00000040U
#define EHCI_QTD_STATUS_DATA_BUFFER 0x00000020U
#define EHCI_QTD_STATUS_BABBLE 0x00000010U
#define EHCI_QTD_STATUS_TRANSACTION 0x00000008U
#define EHCI_QTD_STATUS_MISSED_MICROFRAME 0x00000004U
#define EHCI_QTD_STATUS_SPLIT 0x00000002U
#define EHCI_QTD_STATUS_PING 0x00000001U

/* Queue head endpoint characteristics (EHCI 1.0 section 3.6). */
#define EHCI_QH_LINK_TYPE_QH 0x00000002U
#define EHCI_QH_LINK_MASK 0xFFFFFFE0U
#define EHCI_QH_TERMINATE 0x00000001U
#define EHCI_QH_NAK_RELOAD_SHIFT 28U
#define EHCI_QH_CONTROL 0x08000000U
#define EHCI_QH_MAX_PACKET_SHIFT 16U
#define EHCI_QH_MAX_PACKET_MASK 0x07FF0000U
#define EHCI_QH_HEAD_OF_LIST 0x00008000U
#define EHCI_QH_DTC 0x00004000U
#define EHCI_QH_SPEED_SHIFT 12U
#define EHCI_QH_SPEED_MASK 0x00003000U
#define EHCI_QH_SPEED_HIGH 0x2U
#define EHCI_QH_SPEED_FULL 0x0U
#define EHCI_QH_ENDPOINT_SHIFT 8U
#define EHCI_QH_ENDPOINT_MASK 0x00000F00U
#define EHCI_QH_ADDRESS_MASK 0x0000007FU
#define EHCI_QH_PARK_MASK 0x00000080U
#define EHCI_QH_MULTIPLIER_SHIFT 30U
#define EHCI_QH_HUB_SHIFT 16U
#define EHCI_QH_PORT_SHIFT 23U

/* The descriptor buffer pointers a qTD can carry (EHCI 1.0 section 3.5.3). */
#define EHCI_QTD_BUFFERS 5U
/* The pool's page size and descriptor count, needed by the layout below. */
#define EHCI_PAGE_BYTES 4096U
#define EHCI_MAX_QTD 128U
#define EHCI_MAX_QTD_PER_TRANSFER 8U

/*
 * ITEM-560: the asynchronous ring's on-wire structures, where a host proof can
 * build and check them.  They were private to the driver, so the only thing a
 * host could test was the encoder that filled a field - not the ring the
 * controller actually walks: the head's link and type, the head-of-reclamation
 * bit, the overlay that makes an empty head, the transfer head's link back to
 * the list head, the descriptors' page pointers.
 */
/*
 * ITEM-569: the two formats of the queue structures.
 *
 * EHCI 1.0 section 3 (32-bit) defines a 32-byte qTD and a 48-byte QH.  Appendix
 * B (64-bit, when HCCPARAMS.AC64 is one) keeps every 32-bit field where it was
 * and appends the high halves of the five buffer pointers: qTD-64 is 52 bytes
 * with `ext_buffer[0..4]` at 0x20..0x30, QH-64 is 68 bytes with
 * `overlay_ext_buffer[0..4]` at 0x30..0x40 (Figures B-3 and B-4).
 *
 * The structures below are always the 64-bit layout, so one pool serves both
 * formats: in 32-bit mode the controller reads only the leading fields and the
 * extended words are unused; in 64-bit mode it reads them as the upper halves
 * of the buffer addresses.  What must never happen is the old arrangement - a
 * 32-byte stride under a 52-byte structure - because then qTD n's
 * `ext_buffer[0]` is qTD n+1's `next` link and the controller combines a link
 * pointer with a buffer address: a 64-bit address it never owned, and a master
 * abort (Table 4-19: fatal) on the first transfer.
 *
 * Note what CTRLDSSEGMENT is and is not: it supplies the upper 32 bits of the
 * *schedule* structure pointers (section 3.6, Appendix B), not of the data
 * buffer addresses.  A zero segment does not make a 32-bit buffer field a
 * 64-bit one.
 */
struct ehci_qh {
    volatile uint32_t link;
    volatile uint32_t endpoint1;
    volatile uint32_t endpoint2;
    volatile uint32_t current;
    volatile uint32_t overlay_next;
    volatile uint32_t overlay_alt_next;
    volatile uint32_t overlay_token;
    volatile uint32_t overlay_buffer[EHCI_QTD_BUFFERS];
    /* QH-64, 0x30..0x40: the overlay's high buffer halves (AC64 only). */
    volatile uint32_t overlay_ext_buffer[EHCI_QTD_BUFFERS];
};

struct ehci_qtd {
    volatile uint32_t next;
    volatile uint32_t alt_next;
    volatile uint32_t token;
    volatile uint32_t buffer[EHCI_QTD_BUFFERS];
    /* qTD-64, 0x20..0x30: the high halves of buffer[0..4] (AC64 only). */
    volatile uint32_t ext_buffer[EHCI_QTD_BUFFERS];
};

/* The sizes the two formats define, and the slots this driver gives them.  A
   descriptor must not cross a 4 KiB page (section 3.5) and must be at least
   32-byte aligned; a 64-byte stride keeps every qTD inside one page and 64-byte
   aligned, and a 128-byte slot does the same for a QH-64 (68 bytes - the old
   64-byte slot could not hold it). */
#define EHCI_QTD_BYTES 32U
#define EHCI_QTD64_BYTES 52U
#define EHCI_QH_BYTES 48U
#define EHCI_QH64_BYTES 68U
#define EHCI_QTD_STRIDE 64U
#define EHCI_QH_STRIDE 128U

_Static_assert(sizeof(struct ehci_qtd) == EHCI_QTD64_BYTES,
               "the descriptor must carry the 64-bit format's ext words");
_Static_assert(sizeof(struct ehci_qh) == EHCI_QH64_BYTES,
               "the queue head must carry the 64-bit format's ext words");
_Static_assert(EHCI_QTD64_BYTES <= EHCI_QTD_STRIDE,
               "a descriptor does not fit its stride");
_Static_assert(EHCI_QH64_BYTES <= EHCI_QH_STRIDE,
               "a queue head does not fit its slot");
_Static_assert((EHCI_QTD_STRIDE % 32U) == 0U && (EHCI_QH_STRIDE % 32U) == 0U,
               "the slots are not aligned to the structures they hold");

/*
 * ITEM-560/569: the fixed DMA pool, in one place, so the host proof checks the
 * numbers the controller is actually pointed at.  Every offset is computed
 * from the structure sizes above, and the five regions are disjoint:
 *
 *   0x0000  list-head queue head          EHCI_QH_STRIDE
 *   0x0080  transfer queue head           EHCI_QH_STRIDE
 *   0x0100  transfer descriptors          EHCI_MAX_QTD * EHCI_QTD_STRIDE
 *   0x2100  (padding to a page boundary)
 *   0x3000  bounce area: setup/data/bulk  EHCI_BOUNCE_BYTES
 *   0x7000  periodic frame list           EHCI_FRAME_LIST_BYTES
 *
 * The frame list used to start at DMA+0x3000, which is also where the control
 * data buffer lives (`EHCI_DATA_LOCAL`): a control transfer's data buffer was
 * the periodic frame list.  The two are separated below, and a static
 * assertion keeps the frame list after everything the transfer path writes.
 */
#define EHCI_DMA_PAGES 8U
#define EHCI_DMA_BYTES (EHCI_DMA_PAGES * EHCI_PAGE_BYTES)
#define EHCI_LIST_HEAD_OFFSET 0U
#define EHCI_TRANSFER_QH_OFFSET EHCI_QH_STRIDE
#define EHCI_QTD_OFFSET (2U * EHCI_QH_STRIDE)
#define EHCI_QTD_AREA_BYTES (EHCI_MAX_QTD * EHCI_QTD_STRIDE)
#define EHCI_POOL_PAGE_ALIGN(value) \
    (((value) + EHCI_PAGE_BYTES - 1U) & ~(EHCI_PAGE_BYTES - 1U))
#define EHCI_BOUNCE_OFFSET \
    EHCI_POOL_PAGE_ALIGN(EHCI_QTD_OFFSET + EHCI_QTD_AREA_BYTES)
#define EHCI_FRAME_LIST_BYTES EHCI_PAGE_BYTES
#define EHCI_FRAME_LIST_OFFSET (EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES)
#define EHCI_FRAME_LIST_ENTRIES (EHCI_FRAME_LIST_BYTES / 4U)
#define EHCI_FRAME_LIST_TERMINATE 1U
#define EHCI_BOUNCE_BYTES 16384U
_Static_assert((EHCI_LIST_HEAD_OFFSET % 32U) == 0U &&
                   (EHCI_TRANSFER_QH_OFFSET % 32U) == 0U &&
                   (EHCI_QTD_OFFSET % 32U) == 0U,
               "a structure is not 32-byte aligned in the pool");
_Static_assert(EHCI_TRANSFER_QH_OFFSET >= EHCI_QH_STRIDE,
               "the two queue heads share a slot");
_Static_assert(EHCI_QTD_OFFSET >= 2U * EHCI_QH_STRIDE,
               "the descriptor array overlaps the queue heads");
_Static_assert((EHCI_BOUNCE_OFFSET % EHCI_PAGE_BYTES) == 0U,
               "the bounce area is not page aligned");
_Static_assert(EHCI_FRAME_LIST_OFFSET >= EHCI_BOUNCE_OFFSET + EHCI_BOUNCE_BYTES,
               "the frame list overlaps the bounce area");
_Static_assert((EHCI_FRAME_LIST_OFFSET % EHCI_PAGE_BYTES) == 0U,
               "the frame list is not 4 KiB aligned");
_Static_assert(EHCI_FRAME_LIST_OFFSET + EHCI_FRAME_LIST_BYTES <= EHCI_DMA_BYTES,
               "the frame list does not fit the pool");

/* The bounce area's parts, page-aligned so a descriptor's buffer pointer never
   has to be special-cased.  ITEM-569: the part offsets are named here (not in
   the driver) so the host proof can require the control data to end before the
   periodic frame list - the two shared DMA+0x3000 before this item. */
#define EHCI_SETUP_LOCAL 0U
#define EHCI_SETUP_BYTES 8U
#define EHCI_DATA_LOCAL EHCI_PAGE_BYTES
#define EHCI_DATA_BYTES EHCI_PAGE_BYTES
#define EHCI_BULK_LOCAL (2U * EHCI_PAGE_BYTES)
#define EHCI_BULK_BYTES (2U * EHCI_PAGE_BYTES)
_Static_assert(EHCI_SETUP_LOCAL + EHCI_SETUP_BYTES <= EHCI_BOUNCE_BYTES,
               "the setup packet leaves the bounce area");
_Static_assert(EHCI_DATA_LOCAL + EHCI_DATA_BYTES <= EHCI_BOUNCE_BYTES,
               "the control data leaves the bounce area");
_Static_assert(EHCI_BULK_LOCAL + EHCI_BULK_BYTES <= EHCI_BOUNCE_BYTES,
               "the bulk data leaves the bounce area");
_Static_assert(EHCI_BOUNCE_OFFSET + EHCI_DATA_LOCAL + EHCI_DATA_BYTES <=
                   EHCI_FRAME_LIST_OFFSET,
               "a control transfer's data buffer overlaps the frame list");
_Static_assert(EHCI_BOUNCE_OFFSET + EHCI_BULK_LOCAL + EHCI_BULK_BYTES <=
                   EHCI_FRAME_LIST_OFFSET,
               "a bulk transfer's data buffer overlaps the frame list");

/* One transfer is bounded so a stuck device cannot hold the boot: 1 MiB is far
   beyond any BOT command this driver issues (the block view splits longer
   requests), and a 4 KiB page pointer array is what the descriptor can carry. */
#define EHCI_TRANSFER_MAX_BYTES (4U * EHCI_PAGE_BYTES)
#define EHCI_BOUNCE_BYTES 16384U

enum ehci_status {
    EHCI_STATUS_OK = 0,
    EHCI_STATUS_UNKNOWN,
    EHCI_STATUS_CONTROLLER_ABSENT,
    EHCI_STATUS_PCI_SCAN_FAILED,
    EHCI_STATUS_NO_MEMORY_BAR,
    EHCI_STATUS_BAR_UNDECODED,
    EHCI_STATUS_MMIO_MAP_FAILED,
    EHCI_STATUS_MMIO_TOO_SMALL,
    EHCI_STATUS_DMA_UNAVAILABLE,
    EHCI_STATUS_RESET_TIMEOUT,
    EHCI_STATUS_CONFIGFLAG_TIMEOUT,
    EHCI_STATUS_PORT_RESET_TIMEOUT,
    EHCI_STATUS_PORT_NOT_ENABLED,
    EHCI_STATUS_UNSUPPORTED_SPEED,
    EHCI_STATUS_ADDRESS_FAILED,
    EHCI_STATUS_DESCRIPTOR_FAILED,
    EHCI_STATUS_CONFIG_PARSE_FAILED,
    EHCI_STATUS_SET_CONFIGURATION_FAILED,
    EHCI_STATUS_STORAGE_REFUSED,
    EHCI_STATUS_BLOCK_DEVICE_FAILED,
    EHCI_STATUS_TRANSFER_FAILED,
    EHCI_STATUS_MEDIUM_UNAVAILABLE,
    EHCI_STATUS_MEDIUM_NOT_MATCHED,
    /* ITEM-568: the controller halted itself (USBSTS.HSE, or HCH with the
       error bit), so no port reset can complete.  This is a controller verdict,
       not a port one. */
    EHCI_STATUS_HOST_SYSTEM_ERROR,
    EHCI_STATUS_NO_DEVICE
};

/* What one queued transfer ended as, out of the last descriptor's status. */
enum ehci_transfer_result {
    EHCI_TRANSFER_OK = 0,
    EHCI_TRANSFER_TIMEOUT,
    EHCI_TRANSFER_STALLED,
    EHCI_TRANSFER_DATA_BUFFER,
    EHCI_TRANSFER_BABBLE,
    EHCI_TRANSFER_TRANSACTION,
    EHCI_TRANSFER_OTHER
};

/* One transfer chunk: how many bytes each descriptor carries and how many
   packets that is, so a caller can advance the endpoint's data toggle. */
struct ehci_transfer_plan {
    uint32_t qtd_count;
    uint32_t length[EHCI_MAX_QTD_PER_TRANSFER];
    uint32_t packets[EHCI_MAX_QTD_PER_TRANSFER];
    uint32_t total_bytes;
};

const char *ehci_status_string(enum ehci_status status);
const char *ehci_transfer_result_string(enum ehci_transfer_result result);

/* The USB 2.0 hub class values the topology uses (chapter 11). */
#define EHCI_USB_CLASS_HUB 0x09U
#define EHCI_USB_DESCRIPTOR_HUB 0x29U
#define EHCI_HUB_DESCRIPTOR_PREFIX 9U
#define EHCI_HUB_MAX_PORTS 15U
#define EHCI_HUB_REQUEST_GET_STATUS 0x00U
#define EHCI_HUB_REQUEST_CLEAR_FEATURE 0x01U
#define EHCI_HUB_REQUEST_SET_FEATURE 0x03U
#define EHCI_HUB_REQUEST_TYPE_IN 0xA3U
#define EHCI_HUB_REQUEST_TYPE_DEVICE_IN 0xA0U
#define EHCI_HUB_REQUEST_TYPE_OUT 0x23U
#define EHCI_HUB_FEATURE_PORT_RESET 0x0004U
#define EHCI_HUB_FEATURE_PORT_POWER 0x0008U
#define EHCI_HUB_FEATURE_C_PORT_RESET 0x0014U
/* USB 2.0 Table 11-21 (wPortStatus): bit 0 connect, bit 1 enable, bit 9
   low-speed device attached, bit 10 high-speed device attached, bit 11 test
   mode.  ITEM-570: the speed bits were one position high - 0x0400 (high)
   read as low and 0x0800 (test mode) read as high - so a high-speed device
   behind the hub was refused as a low-speed one that needs split
   transactions. */
#define EHCI_HUB_PORT_STATUS_CONNECTED 0x0001U
#define EHCI_HUB_PORT_STATUS_ENABLED 0x0002U
#define EHCI_HUB_PORT_STATUS_LOW_SPEED 0x0200U
#define EHCI_HUB_PORT_STATUS_HIGH_SPEED 0x0400U
#define EHCI_HUB_PORT_STATUS_TEST 0x0800U
#define EHCI_HUB_PORT_CHANGE_RESET 0x0010U

/* What one downstream port of a hub carries, as far as this driver may use it:
   a high-speed device needs no transaction translator, anything slower would
   need split transactions and is refused. */
enum ehci_hub_port_verdict {
    EHCI_HUB_PORT_EMPTY = 0,
    EHCI_HUB_PORT_HIGH_SPEED,
    EHCI_HUB_PORT_FULL_SPEED,
    EHCI_HUB_PORT_LOW_SPEED,
    EHCI_HUB_PORT_NOT_ENABLED
};

/* The eight bytes of a control SETUP packet (USB 2.0 table 9-2). */
size_t ehci_control_setup(uint8_t *setup,
                          uint8_t request_type,
                          uint8_t request,
                          uint16_t value,
                          uint16_t index,
                          uint16_t length);
/* GetHubDescriptor is a class request (0xA0/0x06/0x2900), not the standard
   0x80 GET_DESCRIPTOR the driver used to send (USB 2.0 section 11.24.2.5). */
void ehci_hub_get_descriptor_fields(uint8_t *request_type,
                                    uint8_t *request,
                                    uint16_t *value);
/*
 * Classify a finished control transfer from every descriptor's token, not only
 * the last one: the host controller halts the queue head on the descriptor that
 * failed, so a stall on SETUP leaves the status descriptor active - which the
 * last-descriptor check reported as a timeout.  Returns the failure and, in
 * `failed_index`, the descriptor that failed (0 = SETUP).
 */
enum ehci_transfer_result ehci_classify_qtds(const uint32_t *tokens,
                                             uint32_t count,
                                             uint32_t *failed_index);

uint8_t ehci_hub_port_count(const uint8_t *descriptor, size_t length);
uint8_t ehci_hub_power_good_ms(const uint8_t *descriptor, size_t length);
/* The class request one hub port feature needs: bmRequestType, bRequest and
   wValue, so the host proof can pin the three values the wire carries. */
bool ehci_hub_feature_request(bool set,
                              uint16_t feature,
                              uint8_t *request_type,
                              uint8_t *request,
                              uint16_t *value);
enum ehci_hub_port_verdict ehci_hub_port_verdict(uint16_t status);
bool ehci_hub_reset_complete(uint16_t change);
const char *ehci_hub_port_verdict_string(enum ehci_hub_port_verdict verdict);

/*
 * ITEM-560: the ring a controller walks, built by pure code.
 *
 * `ehci_ring_reachable` answers whether a DMA region can be reached by this
 * controller at all: with the 64-bit capability absent, every address the
 * controller dereferences is 32 bits wide, and a region that does not fit is
 * not a transfer that fails - it is a transfer the controller cannot address.
 */
bool ehci_ring_reachable(uint64_t base, uint32_t bytes, bool supports_64bit);
/* The empty reclamation head: points at itself, is the head of the list, the
   overlay halted and terminated, and every high-bandwidth field set the way a
   high-speed queue head requires (Mult = 1). */
void ehci_ring_list_head(struct ehci_qh *qh, uint32_t physical);
/* The transfer head: linked behind the list head, overlay pointing at the
   first descriptor, alternate next terminated, its own overlay inactive. */
void ehci_ring_transfer_head(struct ehci_qh *qh,
                             uint32_t list_head_physical,
                             uint32_t first_qtd_physical,
                             uint16_t max_packet,
                             uint8_t endpoint,
                             uint8_t address);
/* One descriptor: the chain pointer, the token, and the page pointers a buffer
   that crosses pages needs. */
void ehci_ring_qtd(struct ehci_qtd *qtd,
                   uint32_t next_physical,
                   uint32_t token,
                   uint64_t buffer_physical,
                   uint32_t length,
                   bool supports_64bit);
/* The command word the first start writes: the interrupt threshold the
   controller reported, the run and asynchronous-schedule bits, and *not* the
   periodic schedule the driver never builds. */
uint32_t ehci_ring_usbcmd_start(uint32_t after_reset);

/* The pure encoders the hardware half writes, and the host proof reads. */
uint32_t ehci_qtd_token(uint32_t total_bytes,
                        bool data_toggle,
                        bool interrupt_on_complete,
                        uint8_t error_count,
                        uint8_t pid,
                        uint8_t status);
uint32_t ehci_qh_endpoint1(uint8_t nak_reload,
                           bool is_control,
                           uint16_t max_packet,
                           bool head_of_list,
                           bool toggle_from_qtd,
                           uint8_t speed,
                           uint8_t endpoint,
                           uint8_t address);
uint32_t ehci_qh_endpoint2(uint8_t multiplier, uint8_t hub, uint8_t port);
uint32_t ehci_link(uint32_t physical, bool terminate);

/* A port's own state, read from PORTSC and nothing else. */
bool ehci_port_connected(uint32_t portsc);
bool ehci_port_enabled(uint32_t portsc);
bool ehci_port_owned_by_companion(uint32_t portsc);
bool ehci_port_high_speed(uint32_t portsc);
uint8_t ehci_port_speed_code(uint32_t portsc);
/* PORTSC as this driver writes it: only the whole-port bits that are writable,
   and the change bits being cleared by writing them back as one. */
uint32_t ehci_portsc_write_value(uint32_t portsc, uint32_t set_bits);

/*
 * ITEM-563: the value that switches a root port on.  PP is writable, but the
 * `set_bits` mask of ehci_portsc_write_value() deliberately preserves only the
 * bits that belong to the port's own state, and PP was not in the set mask - so
 * `ehci_portsc_write_value(portsc, EHCI_PORTSC_PP)` wrote zero and the driver
 * never powered a port.  That is why a port whose connect was never seen could
 * still have been sitting unpowered: no caller actually switched it on.
 */
uint32_t ehci_portsc_power_value(uint32_t portsc);

/*
 * The descriptor plan of one transfer: `length` bytes from a buffer that is
 * page-aligned, split on packet boundaries (a packet may not start in one
 * descriptor and end in the next) and on the four pages a descriptor can
 * address with its five buffer pointers.  The last chunk may be shorter and is
 * not packet-aligned.  Zero length is one empty descriptor, which is what the
 * status stage of a control transfer is.
 */
bool ehci_plan_transfer(uint32_t length,
                        uint32_t max_packet,
                        struct ehci_transfer_plan *plan);
/* Bytes covered by the first `index` chunks - the offset of the next chunk's
   buffer and the packets already sent, which is what the toggle follows. */
uint32_t ehci_plan_offset(const struct ehci_transfer_plan *plan,
                          uint32_t index);
uint32_t ehci_plan_packets(const struct ehci_transfer_plan *plan,
                           uint32_t index);

/* ------------------------------------------------------------ hardware half --- */

/*
 * Bring up every EHCI function of the machine, bind the mass-storage device on
 * its only high-speed root port, prove the boot medium on it and bind the
 * reserved journal.  `hhdm_offset` is the kernel's mapping of physical memory.
 * The call is bounded and never fatal: every refusal is a named status the
 * `[EHCI]` record carries, exactly like the xHCI walk.
 */
enum ehci_status ehci_storage_init(uint64_t hhdm_offset);
enum ehci_status ehci_status(void);
const char *ehci_detail(void);

/*
 * ITEM-558: the transfer that failed, kept for the operator.
 *
 * The hardware run after ITEM-557 reported `ehci=DESCRIPTOR_FAILED/DEVICE8` and
 * nothing about the transfer: the result, the descriptor token, the bytes
 * received and the registers lived only in the serial record `[EHCI-FAIL]`,
 * and a machine with no serial capture and no bound journal could not show
 * them.  The snapshot is now rendered once, when the failure is captured, and
 * this accessor hands it to the diagnostic detail and to the report, so the
 * stage name never stands where the primary cause belongs.
 */
#define EHCI_FAILURE_TEXT_MAX 704U
/* The rendered snapshot, or NULL when no transfer has failed. */
const char *ehci_failure_text(void);

/*
 * ITEM-561: the start evidence as one bounded line, kept when the controller is
 * started and returned even when the start itself refused the controller.
 *
 * ITEM-560 promised this evidence in the report, and the run that refused at
 * MEDIUM_PROBE produced no report: the `diag save` could not reach a medium, so
 * the only channel left was the screen, and the start fields (PCI command and
 * status, the handoff, the words written and read back at stop, reset and
 * start) were never rendered anywhere the operator could photograph.  The line
 * below is that rendering: the start path publishes it as `[EHCI-START]` at the
 * moment the controller is started, the report carries it as `[EHCI-START]` and
 * the refusal record carries it as `ehci_start=`, so `diag` prints it on
 * demand without a journal.
 */
#define EHCI_START_TEXT_MAX 640U
/* The chosen controller's rendered line, or NULL when none was started. */
const char *ehci_start_evidence(void);

/* ITEM-562: every started controller's rendering, in scan order, joined with
   ` | `.  The hardware runs showed one controller in one run and the other in
   the next because each start overwrote the single snapshot. */
size_t ehci_start_evidence_all(char *buffer, size_t size);

/* ITEM-562: the per-controller verdict list the refusal record carries, e.g.
   `00:1A.0=DESCRIPTOR_FAILED/DEVICE8;00:1D.0=PORT_NOT_ENABLED/ROOT_RESET`.
   Empty until the scan has run. */
const char *ehci_controllers_text(void);

/* ITEM-562: the same lines the diagnostic report carries - one `[EHCI]` and one
   `[EHCI-START]` per controller, the walk's stage included - printed on the
   console.  This is the command that answers "what did each controller do" on
   a machine whose journal is unbound, where `bootlog` has nothing to show. */
void ehci_diagnostics_print(void);

/*
 * ITEM-560: when the Host System Error first appeared.  The transfer snapshot
 * of ITEM-559 said HSE was set when the transfer failed, but not whether the
 * controller had already halted before the transfer head was linked, at the
 * schedule start, or during the wait.  Each step of the start and the walk is
 * sampled, and the first sample that carries HSE is kept apart from everything
 * that follows (cleanup included).
 */
enum ehci_hse_stage {
    EHCI_HSE_STAGE_NONE = 0,
    EHCI_HSE_STAGE_AFTER_RESET,
    EHCI_HSE_STAGE_AFTER_CONFIGFLAG,
    EHCI_HSE_STAGE_AFTER_ASYNC_ADDRESS,
    EHCI_HSE_STAGE_AFTER_SCHEDULE,
    EHCI_HSE_STAGE_AFTER_PORT_RESET,
    EHCI_HSE_STAGE_BEFORE_LINK,
    EHCI_HSE_STAGE_IN_WAIT
};

const char *ehci_hse_stage_string(enum ehci_hse_stage stage);
/* The short result code of that transfer ("XACTERR", "STALLED", ...), or NULL. */
const char *ehci_failure_result(void);
/* ITEM-559: the name the operator's short line carries - the controller's own
   verdict ("HSE", "HALTED", "ASYNC_STOPPED", ...) when it has one, and the
   descriptor classifier's result otherwise. */
const char *ehci_failure_primary(void);
bool ehci_storage_ready(void);
struct block_device *ehci_storage_block_device(void);

/* The same read-only medium probe `storage medium` runs for xHCI, on the EHCI
   device: LBA 0, the loader's own identity, and the journal placement when the
   guard says MATCH. */
bool ehci_storage_medium_publish(void);
const char *ehci_storage_medium_result(void);
bool ehci_storage_medium_status(const char **state, const char **reason);
/* ITEM-571: the last failed command on the bound device - opcode, CSW status
   and residue, and the sense data the device answered - rendered as one bounded
   line.  False when nothing has failed or no device is bound. */
bool ehci_storage_fault_text(char *buffer, size_t size);

/*
 * ITEM-552: the same facts as one line per controller and one per bound
 * storage device, handed to the diagnostic report's sink.  Nothing here touches
 * the console or the journal: the export decides where its report goes.
 */
typedef void (*ehci_report_sink_fn)(void *context, const char *text);
void ehci_diagnostics_report(ehci_report_sink_fn sink, void *context);

#endif
