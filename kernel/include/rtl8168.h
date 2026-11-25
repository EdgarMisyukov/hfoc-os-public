#ifndef HFOC_RTL8168_H
#define HFOC_RTL8168_H

#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-389 production driver core for the on-board Realtek
 * RTL8111/8168/8411 PCIe Gigabit Ethernet controller (PCI 10EC:8168), the
 * acceptance host's NIC.
 *
 * Every constant below is derived from the register map the Linux r8169
 * driver programs for this chip family, which is the datasheet-level map of
 * the part (drivers/net/ethernet/realtek/r8169_main.c and r8169.h, v5.10 and
 * current master agree on all of them):
 *
 *   - register offsets/widths and the descriptor layout: enum rtl_registers,
 *     struct TxDesc/struct RxDesc, enum rtl_desc_bit;
 *   - the descriptor ownership/first/last/ring-end bits: enum rtl_desc_bit
 *     (DescOwn 1<<31, RingEnd 1<<30, FirstFrag 1<<29, LastFrag 1<<28);
 *   - the receive status decoding: rtl_rx() - DescOwn gate, RxRES error gate,
 *     pkt_size = status & GENMASK(13, 0), the 4-byte FCS the descriptor length
 *     includes (pkt_size -= ETH_FCS_LEN), and the fragmented-frame test
 *     (status & (FirstFrag | LastFrag)) != (FirstFrag | LastFrag);
 *   - the transmit programming: rtl8169_tx_map() - opts1 = flags | len with
 *     RingEnd on the last ring entry and DescOwn set last, plus FirstFrag and
 *     LastFrag for the single-segment frame the driver sends;
 *   - the chip identity probe: rtl8169_get_mac_version() reading
 *     xid = (TxConfig >> 20) & 0xfcf, and rtl_is_8168evl_up() for the
 *     extended-MAC (ERI 0xe0/0xe4) family;
 *   - the reset: rtl_hw_reset() - ChipCmd = CmdReset, then wait for the bit to
 *     clear;
 *   - the bring-up order: rtl_hw_start()/rtl_reset_work() - unlock Cfg9346,
 *     CPlusCmd, RxMaxSize, descriptor start registers, lock Cfg9346, PCI
 *     commit, ChipCmd = TxEnb | RxEnb, RxConfig, TxConfig, receive filter.
 *
 * This translation unit owns the parts that can be reasoned about and tested
 * without the part in the socket: identity classification, MAC read/validate,
 * the reset and bring-up register sequences, the descriptor state machines,
 * ring arithmetic and the frame-length rules.  The x86 adapter owns mapping
 * the BARs, allocating the DMA region and feeding lwIP.
 */

#define RTL8168_VENDOR_ID 0x10ECU
#define RTL8168_DEVICE_ID 0x8168U
#define RTL8168_PCI_CLASS_NETWORK 0x02U
#define RTL8168_PCI_SUBCLASS_ETHERNET 0x00U

#define RTL8168_MAC_LENGTH 6U
#define RTL8168_FCS_LENGTH 4U
#define RTL8168_FRAME_MINIMUM 60U
#define RTL8168_FRAME_MAXIMUM 1518U

/* Register file (enum rtl_registers in r8169_main.c). */
#define RTL8168_REG_MAC0 0x00U
#define RTL8168_REG_MAC4 0x04U
#define RTL8168_REG_MAR0 0x08U
#define RTL8168_REG_TX_DESC_LOW 0x20U
#define RTL8168_REG_TX_DESC_HIGH 0x24U
#define RTL8168_REG_CHIP_CMD 0x37U
#define RTL8168_REG_TX_POLL 0x38U
#define RTL8168_REG_INTR_MASK 0x3CU
#define RTL8168_REG_INTR_STATUS 0x3EU
#define RTL8168_REG_TX_CONFIG 0x40U
#define RTL8168_REG_RX_CONFIG 0x44U
#define RTL8168_REG_CFG9346 0x50U
#define RTL8168_REG_CONFIG3 0x54U
#define RTL8168_REG_PHY_STATUS 0x6CU
#define RTL8168_REG_ERI_DATA 0x70U
#define RTL8168_REG_ERI_ADDRESS 0x74U
/*
 * ITEM-592: the registers the 8168evl cold start programs before the MAC is
 * enabled, and their bits.  The offsets are the reference driver's own
 * register map; the values below are hardware facts about this generation.
 */
#define RTL8168_REG_MCU 0xD3U
#define RTL8168_MCU_NOW_IS_OOB (1U << 7U)
#define RTL8168_REG_DLLPR 0xD0U
#define RTL8168_DLLPR_PFM_EN (1U << 6U)
#define RTL8168_REG_EEE_LED 0x1BU
#define RTL8168_EEE_LED_MASK 0x07U
#define RTL8168_REG_CONFIG2 0x53U
#define RTL8168_REG_CONFIG5 0x56U
#define RTL8168_CONFIG5_SPI_EN (1U << 3U)
#define RTL8168_REG_MAX_TX_PACKET_SIZE 0xECU
/* The 8168evl-and-later start programs this with the early transmit size. */
#define RTL8168_MAX_TX_PACKET_SIZE_EARLY 0x27U
#define RTL8168_REG_INTR_MITIGATE 0xE2U
/* MISC bit 22: the 8168evl start turns the PWM block on. */
#define RTL8168_MISC_PWM_EN (1U << 22U)
#define RTL8168_REG_RX_MAX_SIZE 0xDAU
#define RTL8168_REG_CPLUS_CMD 0xE0U
#define RTL8168_REG_RX_DESC_LOW 0xE4U
#define RTL8168_REG_RX_DESC_HIGH 0xE8U
/*
 * ITEM-435: the MISC register and its receive data-valid gate.
 *
 * The 8168G-and-later parts gate the receive data path behind bit 19 of MISC
 * (0xF0), and a part whose gate is closed answers every register, completes
 * transmit descriptors and never writes a receive descriptor - no frame, no
 * receive error, no interrupt.  Mainline r8169 opens the gate for this family
 * in `rtl_disable_rxdvgate()` (`RTL_W32(tp->xi->misc, RTL_R32(tp->xi->misc) &
 * ~RXDV_GATED_EN)`, `RXDV_GATED_EN = BIT(19)`), which `rtl_hw_start_8168h_1()`
 * calls for this part's MAC version (VER_46) before the receiver is enabled.
 */
#define RTL8168_REG_MISC 0xF0U
#define RTL8168_MISC_RXDV_GATED_EN UINT32_C(1) << 19U
#define RTL8168_MIN_MMIO_LENGTH 0x100U

/*
 * ChipCmd (0x37).  The RTL8111B/8168B register datasheet (Table 3) documents
 * exactly three bits: bit 4 RST, bit 3 RE (receiver enable), bit 2 TE
 * (transmitter enable); bits 7-5 and 1-0 are reserved.  The reference driver
 * carries two further bit definitions (StopReq 0x80, RxBufEmpty 0x01) that it
 * never uses on this part, so this driver does not claim them either.
 */
#define RTL8168_CHIP_CMD_RESET 0x10U
#define RTL8168_CHIP_CMD_RX_ENABLE 0x08U
#define RTL8168_CHIP_CMD_TX_ENABLE 0x04U
/* The receive buffer addresses and sizes are 8-byte granular (Table 57/58). */
#define RTL8168_BUFFER_ALIGNMENT 8U

/* TxPoll. */
#define RTL8168_TX_POLL_NPQ 0x40U

/* Cfg9346 (Cfg9346Bits). */
#define RTL8168_CFG9346_LOCK 0x00U
#define RTL8168_CFG9346_UNLOCK 0xC0U

/* RxConfig accept bits (rx_mode_bits). */
#define RTL8168_RX_ACCEPT_ERROR 0x20U
#define RTL8168_RX_ACCEPT_RUNT 0x10U
#define RTL8168_RX_ACCEPT_BROADCAST 0x08U
#define RTL8168_RX_ACCEPT_MULTICAST 0x04U
#define RTL8168_RX_ACCEPT_MY_PHYSICAL 0x02U
#define RTL8168_RX_ACCEPT_ALL_PHYSICAL 0x01U
#define RTL8168_RX_ACCEPT_OK_MASK 0x0FU
#define RTL8168_RX_ACCEPT_ERROR_MASK 0x30U
#define RTL8168_RX_DMA_BURST (7U << 8U)
#define RTL8168_RX_EARLY_OFF (1U << 11U)
#define RTL8168_RX_MULTI_EN (1U << 14U)
#define RTL8168_RX128_INT_EN (1U << 15U)

/* TxConfig: DMA burst 7 ("unlimited") and the shortest inter-frame gap. */
#define RTL8168_TX_DMA_BURST (7U << 8U)
#define RTL8168_TX_INTER_FRAME_GAP (0x03U << 24U)
#define RTL8168_TX_CONFIG_AUTO_FIFO (1U << 7U)

/*
 * Config3 (0x54), bit 1 Rdy_to_L23: cleared while the config registers are
 * unlocked, matching rtl_pcie_state_l2l3_disable(), so the part does not enter
 * the L2/L3 ready state underneath a poll-driven driver.
 */
#define RTL8168_CONFIG3_READY_TO_L23 (1U << 1U)

/*
 * Receive buffer size the command descriptor may carry.  The datasheet is
 * explicit: "The Rx buffer size should not exceed 2^13-1 (8KB-1) and should be
 * a multiple of 8, i.e., the maximum value of this field is 0x1FF8, and bit
 * 2-0 and bit 13 should always be 0."
 */
#define RTL8168_RX_BUFFER_SIZE_MAX 0x1FF8U

/*
 * InterruptStatus bits (Linux v6.12 `enum rtl_register_content`; the
 * RTL8111B/8168B register datasheet Table 4 names the same events).  The
 * driver is poll driven: IntrMask stays 0 and no event is ever delivered as
 * an interrupt, but the status word is still live and is written back as
 * write-one-to-clear.
 *
 * ITEM-645: `RxOverflow` is the vendor driver's `RxDescUnavail`.  A part that
 * runs out of receive descriptors sets it and stops receiving; returning the
 * descriptors does not restart the receiver - only clearing the bit does.
 * `RxFIFOOver` is the FIFO-overrun event.  Linux handles both in
 * `rtl8169_interrupt()` and, for the same stopped shape, clears `RxOverflow`
 * in `rtl8169_poll()` once the ring has room (the 2026-11 upstream fix).
 * `RTL8168_INTR_RX_STOPPED` is the pair this driver keeps latched until its
 * ring is proven to have a free descriptor.
 */
#define RTL8168_INTR_SYSTEM_ERROR 0x8000U
#define RTL8168_INTR_PCS_TIMEOUT 0x4000U
#define RTL8168_INTR_SOFTWARE 0x0100U
#define RTL8168_INTR_TX_DESCRIPTOR_UNAVAILABLE 0x0080U
#define RTL8168_INTR_RX_FIFO_OVERFLOW 0x0040U
#define RTL8168_INTR_LINK_CHANGE 0x0020U
#define RTL8168_INTR_RX_OVERFLOW 0x0010U
#define RTL8168_INTR_TX_ERROR 0x0008U
#define RTL8168_INTR_TX_OK 0x0004U
#define RTL8168_INTR_RX_ERROR 0x0002U
#define RTL8168_INTR_RX_OK 0x0001U
#define RTL8168_INTR_RX_STOPPED \
    (RTL8168_INTR_RX_FIFO_OVERFLOW | RTL8168_INTR_RX_OVERFLOW)

/* InterruptMitigate/interrupt bits: this driver is poll driven. */
#define RTL8168_INTR_MASK_NONE 0x0000U
#define RTL8168_INTR_STATUS_CLEAR 0xFFFFU

/* PHYstatus. */
#define RTL8168_PHY_STATUS_LINK 0x02U
#define RTL8168_PHY_STATUS_FULL_DUPLEX 0x01U

/*
 * ITEM-583: the standard MII management registers behind PHYAR (0x60).  The
 * RTL8168E/8168F family exposes clause-22 MII through this one register:
 * bit 31 is the busy/complete flag, bits 20..16 select the PHY register and
 * bits 15..0 carry the data.  The reference r8169 driver uses exactly this
 * handshake for RTL_GIGA_MAC_VER_40 and earlier, which includes the owner's
 * rev 07 part.
 */
#define RTL8168_REG_PHY_ACCESS 0x60U
#define RTL8168_PHY_ACCESS_BUSY UINT32_C(0x80000000)
#define RTL8168_PHY_ACCESS_WRITE UINT32_C(0x80000000)
#define RTL8168_PHY_ACCESS_REG_SHIFT 16U
#define RTL8168_PHY_ACCESS_REG_MASK 0x1FU
#define RTL8168_PHY_ACCESS_DATA_MASK 0xFFFFU
/*
 * ITEM-591: the handshake's timing, taken from the reference driver's own
 * numbers rather than from a loop count that measured nothing.
 *
 * Linux v6.12 `drivers/net/ethernet/realtek/r8169_main.c`:
 *
 *   rtl_loop_wait_low(tp, &rtl_phyar_cond, 25, 20);   // write
 *   rtl_loop_wait_high(tp, &rtl_phyar_cond, 25, 20);  // read
 *   udelay(20);
 *
 * `rtl_loop_wait(tp, cond, usecs = 25, n = 20, high)` checks the completion
 * condition and sleeps 25 us between checks, so a part that never answers is
 * given up after 20 polls (500 us) instead of being read in a tight loop.
 * Both the read and the write then wait a further 20 us: the comment there
 * says the hardware spec requires that gap "after write complete indication,
 * but before sending next command", and the read path does the same after the
 * read completion indication.  The Toshiba run of 49ac1ffc configured a PHY
 * that answered (phyid=0x001C:0xC915) while the link stayed down and the
 * partner advertisement read 0x0000; the driver issued its commands
 * back-to-back, so the gap the part requires was never honoured.
 */
#define RTL8168_PHY_ACCESS_POLL_US 25U
#define RTL8168_PHY_ACCESS_POLLS 20U
#define RTL8168_PHY_ACCESS_TIMEOUT_US \
    (RTL8168_PHY_ACCESS_POLL_US * RTL8168_PHY_ACCESS_POLLS)
#define RTL8168_PHY_ACCESS_SETTLE_US 20U
/*
 * The reset-bit wait keeps its own bound: each poll of it is a whole PHY read
 * (and therefore already carries the handshake's gaps), so a count of reads is
 * both bounded and about the 50 ms the old loop took.  It is deliberately not
 * the handshake's 500 us budget, which is for one command's completion.
 */
#define RTL8168_PHY_RESET_POLLS 1000U
/* Linux v6.12 phy_poll_reset: 50 ms, 600 ms, then 1 ms settle. */
#define RTL8168_PHY_RESET_POLL_US 50000U
#define RTL8168_PHY_RESET_WAIT_POLLS 12U
#define RTL8168_PHY_RESET_SETTLE_US 1000U
/*
 * ITEM-592: EPHYAR (0x80) - the 8168evl's EPHY register window over the
 * integrated PHY's analog/clock block.  Same handshake shape as PHYAR, with
 * the reference driver's own numbers: `rtl_ephy_write()` waits
 * `rtl_loop_wait_low(&rtl_ephyar_cond, 10, 100)` - 10 us between polls, at
 * most 100 of them (1 ms) - and then pauses 10 us before the next command;
 * the read path does the same.
 */
#define RTL8168_REG_EPHY_ACCESS 0x80U
#define RTL8168_EPHY_ACCESS_FLAG UINT32_C(0x80000000)
#define RTL8168_EPHY_ACCESS_WRITE UINT32_C(0x80000000)
#define RTL8168_EPHY_ACCESS_REG_SHIFT 16U
#define RTL8168_EPHY_ACCESS_REG_MASK 0x1FU
#define RTL8168_EPHY_ACCESS_DATA_MASK 0xFFFFU
#define RTL8168_EPHY_ACCESS_POLL_US 10U
#define RTL8168_EPHY_ACCESS_POLLS 100U
#define RTL8168_EPHY_ACCESS_TIMEOUT_US \
    (RTL8168_EPHY_ACCESS_POLL_US * RTL8168_EPHY_ACCESS_POLLS)
#define RTL8168_EPHY_ACCESS_SETTLE_US 10U

#define RTL8168_PHY_REG_BMCR 0x00U
#define RTL8168_PHY_REG_BMSR 0x01U
#define RTL8168_PHY_REG_PHYID1 0x02U
#define RTL8168_PHY_REG_PHYID2 0x03U
#define RTL8168_PHY_REG_ADVERTISE 0x04U
#define RTL8168_PHY_REG_LPA 0x05U
/*
 * ITEM-592: registers 9 and 10 are where 1000BASE-T is advertised and read.
 *
 * Register 4 carries only the 10/100 (and the obsolete 100BASE-T4) bits; the
 * 1000BASE-T half/full duplex advertisement is register 9's bits 8 and 9, and
 * the link partner's is register 10's bits 10 and 11.  The distinction matters
 * for the Toshiba: the driver wrote 0x03E1 into register 4, whose 0x0200 bit is
 * 100BASE-T4 - not gigabit - and never wrote register 9 at all, so the PHY
 * advertised 10/100 only while the record claimed "10/100/1000".  The part
 * masked the T4 bit off, which is why the read-back said 0x01E1.
 */
#define RTL8168_PHY_REG_CTRL1000 0x09U
#define RTL8168_PHY_REG_STAT1000 0x0AU
#define RTL8168_PHY_CTRL1000_ADVERTISE_1000_HALF 0x0100U
#define RTL8168_PHY_CTRL1000_ADVERTISE_1000_FULL 0x0200U
#define RTL8168_PHY_STAT1000_LP_1000_HALF 0x0400U
#define RTL8168_PHY_STAT1000_LP_1000_FULL 0x0800U
/*
 * ITEM-592: the vendor page register.  Registers 0..15 mean the standard
 * clause-22 set only while the PHY's page is 0; firmware can leave the part on
 * another page, so the cold start selects page 0 before it reads anything it
 * means to interpret, and the record names the page it settled on.
 */
#define RTL8168_PHY_REG_PAGE 0x1FU
#define RTL8168_PHY_PAGE_STANDARD 0x0000U

#define RTL8168_PHY_BMCR_RESET 0x8000U
#define RTL8168_PHY_BMCR_LOOPBACK 0x4000U
#define RTL8168_PHY_BMCR_SPEED_100 0x2000U
#define RTL8168_PHY_BMCR_ANEG_ENABLE 0x1000U
#define RTL8168_PHY_BMCR_POWER_DOWN 0x0800U
#define RTL8168_PHY_BMCR_ISOLATE 0x0400U
#define RTL8168_PHY_BMCR_RESTART_ANEG 0x0200U
#define RTL8168_PHY_BMCR_DUPLEX 0x0100U
#define RTL8168_PHY_BMCR_SPEED_1000 0x0040U

#define RTL8168_PHY_BMSR_LINK 0x0004U
#define RTL8168_PHY_BMSR_ANEG_COMPLETE 0x0020U

/*
 * ITEM-592: the advertisement, corrected.
 *
 * Register 4 carries the standard 10/100 set only: 0x01E1 = CSMA/T4-capability
 * bit 0, 10BASE-T half/full (5/6), 100BASE-TX half/full (7/8).  The old value
 * 0x03E1 added bit 9, which is 100BASE-T4 - a mode this part does not have, so
 * the PHY masked it off and the read-back disagreed with what the record
 * claimed.  1000BASE-T is advertised separately in register 9, and only on a
 * profile whose PHY really is gigabit.
 */
#define RTL8168_PHY_ADVERTISE_DEFAULT 0x01E1U
#define RTL8168_PHY_ADVERTISE_1000 \
    (RTL8168_PHY_CTRL1000_ADVERTISE_1000_HALF | \
     RTL8168_PHY_CTRL1000_ADVERTISE_1000_FULL)

/* Extended register access (ERIAR/ERIDR). */
#define RTL8168_ERIAR_FLAG UINT32_C(0x80000000)
#define RTL8168_ERIAR_WRITE UINT32_C(0x80000000)
#define RTL8168_ERIAR_EXGMAC UINT32_C(0x00000000)
#define RTL8168_ERIAR_MASK_1111 UINT32_C(0x0000F000)
/*
 * ITEM-592: the extended-register address field is 11 bits (0x7FF), so the
 * cold start's 0x1D0/0x1B0/0x1CC words are addressable - a byte-wide address
 * type would have written 0xD0 where 0x1D0 was meant and collided with a real
 * register.
 */
#define RTL8168_ERIAR_ADDRESS_MASK UINT32_C(0x000007FF)
#define RTL8168_ERI_MAC_LOW 0xE0U
#define RTL8168_ERI_MAC_HIGH 0xE4U

/* Descriptor bits (enum rtl_desc_bit) - shared by receive and transmit. */
#define RTL8168_DESC_OWN UINT32_C(0x80000000)
#define RTL8168_DESC_RING_END UINT32_C(0x40000000)
#define RTL8168_DESC_FIRST_FRAGMENT UINT32_C(0x20000000)
#define RTL8168_DESC_LAST_FRAGMENT UINT32_C(0x10000000)
#define RTL8168_DESC_LENGTH_MASK UINT32_C(0x00003FFF)

/* Receive status (RxStatusDesc; RxRES gates every other receive error). */
#define RTL8168_RX_STATUS_RECEIVE_ERROR UINT32_C(1) << 21U
#define RTL8168_RX_STATUS_WATCHDOG UINT32_C(1) << 22U
#define RTL8168_RX_STATUS_RUNT UINT32_C(1) << 20U
#define RTL8168_RX_STATUS_CRC UINT32_C(1) << 19U

/* Receive descriptor capacity programmed into the descriptor itself. */
#define RTL8168_RX_DESCRIPTOR_COUNT 16U
#define RTL8168_TX_DESCRIPTOR_COUNT 16U
#define RTL8168_RX_BUFFER_CAPACITY 2048U
#define RTL8168_TX_BUFFER_CAPACITY 1536U
#define RTL8168_DESCRIPTOR_SIZE 16U
#define RTL8168_DESCRIPTOR_ALIGNMENT 256U
#define RTL8168_RX_BUFFER_STRIDE 2048U
#define RTL8168_TX_BUFFER_STRIDE 1536U

#define RTL8168_RESET_POLL_LIMIT 100000U
#define RTL8168_ERI_POLL_LIMIT 10000U
#define RTL8168_DMA_ADDRESS_LIMIT UINT64_C(0x100000000)

struct rtl8168_descriptor {
    volatile uint32_t opts1;
    volatile uint32_t opts2;
    volatile uint64_t address;
};

struct rtl8168_ring {
    struct rtl8168_descriptor *descriptors;
    uint32_t capacity;
    uint32_t index;
    uint32_t in_flight;
    /*
     * ITEM-428: the ring's own liveness, so a machine that stops transmitting
     * is never three causes at once.  `submitted` counts the descriptors handed
     * to the device, `reclaimed` the ones the device handed back (its OWN bit
     * cleared), and `abandoned` the frames a transmit restart declared lost
     * because the device stopped completing them.  The three are read by the
     * boot record and by the host proof; the driver never infers a cause from
     * them, it only reports what the descriptors said.
     */
    uint32_t submitted;
    uint32_t reclaimed;
    uint32_t abandoned;
};

/*
 * ITEM-643: a read-only snapshot of the receive path for the live-outage
 * report.  The register reads are ordinary MMIO reads; no register is written
 * and IntrStatus is not acknowledged, so sampling the snapshot cannot clear a
 * pending condition or change the driver's poll loop.  The descriptor scan is
 * bounded by the ring capacity the driver owns.
 */
struct rtl8168_rx_diagnostic {
    bool available;
    const char *reason;
    uint32_t chip_cmd;
    uint32_t intr_status;
    uint32_t intr_mask;
    uint32_t rx_config;
    uint32_t rx_desc_low;
    uint32_t rx_desc_high;
    uint32_t misc;
    uint32_t rx_index;
    uint32_t rx_capacity;
    uint32_t rx_in_flight;
    uint32_t rx_own;
    uint32_t rx_head_opts1;
    uint32_t rx_head_address_low;
};

/* ITEM-594/595: evidence from the PHYAR completion and payload operations. */
enum rtl8168_phy_failure_reason {
    RTL8168_PHY_FAILURE_NONE = 0,
    RTL8168_PHY_FAILURE_NO_DELAY,
    RTL8168_PHY_FAILURE_MMIO_WRITE,
    RTL8168_PHY_FAILURE_MMIO_READ,
    RTL8168_PHY_FAILURE_TIMEOUT,
    RTL8168_PHY_FAILURE_PAGE_MISMATCH,
    RTL8168_PHY_FAILURE_RESET_TIMEOUT,
    RTL8168_PHY_FAILURE_BAD_REGISTER,
};

enum rtl8168_phy_phase {
    RTL8168_PHY_PHASE_DIRECT = 0,
    RTL8168_PHY_PHASE_PAGE,
    RTL8168_PHY_PHASE_SELECTOR,
    RTL8168_PHY_PHASE_READ,
    RTL8168_PHY_PHASE_WRITE,
    RTL8168_PHY_PHASE_RESTORE,
    RTL8168_PHY_PHASE_RESET_WAIT,
};

struct rtl8168_phy_transaction {
    enum rtl8168_phy_failure_reason reason;
    bool write;
    uint8_t reg;
    uint32_t command;
    /* Last successful completion-poll sample, retained on payload failure. */
    uint32_t last;
    uint32_t polls;
    bool last_valid;
    uint16_t expected;
    bool expected_valid;
    /* Low16 of the separate payload read performed after completion. */
    uint16_t observed;
    bool observed_valid;
    /* Full separate payload sample; never synthesized from the poll/data. */
    uint32_t payload;
    bool payload_valid;
};

struct rtl8168_phy_failure {
    bool valid;
    uint8_t step; /* 1..15 vendor action; zero for standard PHY operations. */
    enum rtl8168_phy_phase phase;
    uint16_t page;
    uint16_t selector;
    bool selector_valid;
    struct rtl8168_phy_transaction transaction;
    /* Completed page write preceding a failed verification read, frozen
       before cleanup. Completion alone does not prove the bank changed. */
    struct rtl8168_phy_transaction page_write;
    bool page_write_valid;
    bool restore_attempted;
    bool restore_ok;
    uint16_t restore_page;
    bool restore_page_valid;
};

/* ITEM-597: a completed page write is not bank acceptance. Keep the first
 * EVL non-echo observation separately from the immutable first IO failure. */
struct rtl8168_phy_page_observation {
    uint32_t zero_reads;
    uint16_t requested;
    struct rtl8168_phy_transaction write;
    struct rtl8168_phy_transaction read;
};

const char *rtl8168_phy_failure_reason_string(enum rtl8168_phy_failure_reason);
const char *rtl8168_phy_phase_string(enum rtl8168_phy_phase);

struct rtl8168_mmio_io {
    void *context;
    bool (*read8)(void *context, uint32_t offset, uint8_t *value);
    bool (*read16)(void *context, uint32_t offset, uint16_t *value);
    bool (*read32)(void *context, uint32_t offset, uint32_t *value);
    bool (*write8)(void *context, uint32_t offset, uint8_t value);
    bool (*write16)(void *context, uint32_t offset, uint16_t value);
    bool (*write32)(void *context, uint32_t offset, uint32_t value);
    /*
     * ITEM-591: a microsecond pause, supplied by the platform that owns the
     * clock.  The PHYAR handshake cannot be honoured without one - the part
     * requires a gap after every completion indication - so the PHY access
     * refuses a bus that has no way to wait instead of issuing commands
     * back-to-back.  The kernel adapter measures the pause with the machine's
     * calibrated cycle counter; a host proof supplies a model with a virtual
     * clock, which is what makes an early command observable.
     */
    void (*delay_us)(void *context, uint32_t microseconds);
    /* Reinitialized by every PHY read/write; persistent state copies first
       failure before another operation, including cleanup, can replace it. */
    struct rtl8168_phy_transaction phy_last;
    struct rtl8168_phy_transaction phy_page_write;
    bool phy_page_write_valid;
};

enum rtl8168_chip_family {
    RTL8168_CHIP_UNKNOWN = 0,
    /* 8168B..8168DP: the MAC lives in the IDR registers only. */
    RTL8168_CHIP_CLASSIC = 1,
    /* 8168E and later (rtl_is_8168evl_up): the MAC is mirrored in the
       extended registers 0xe0/0xe4 and the extended RxConfig/TxConfig bits
       apply. */
    RTL8168_CHIP_EXTENDED = 2,
};

/*
 * ITEM-592: the confirmed chip profile.
 *
 * The family (classic/extended) is too coarse to decide a cold-boot sequence:
 * within the extended family, the 8168E, the 8168evl, the 8168G, the 8168H and
 * the 8168EP differ in the registers a start may touch and in the order those
 * touches may happen.  The reference driver draws the same line by MAC version
 * (RTL_GIGA_MAC_VER_32..53); the values below name the same generations, and
 * the XID table decides which one a part is.  The owner's Toshiba answers XID
 * 0x2C9, whose row is the 8168evl (RTL_GIGA_MAC_VER_34, "RTL8168evl/8111evl").
 */
enum rtl8168_profile {
    RTL8168_PROFILE_UNKNOWN = 0,
    RTL8168_PROFILE_CLASSIC = 1,
    /* 8168E/8168F/8411: the PHY is behind PHYAR, the start uses the E tables. */
    RTL8168_PROFILE_8168E = 2,
    /* 8168evl (RTL_GIGA_MAC_VER_34): the E tables plus the EPHY/clock start. */
    RTL8168_PROFILE_8168EVL = 3,
    RTL8168_PROFILE_8168G = 4,
    RTL8168_PROFILE_8168H = 5,
    RTL8168_PROFILE_8168EP = 6,
};

struct rtl8168_chip {
    uint16_t xid;
    enum rtl8168_chip_family family;
    enum rtl8168_profile profile;
};

const char *rtl8168_profile_string(enum rtl8168_profile profile);
/*
 * The generation bounds of the settings the cold start writes.  Each is a
 * question about the confirmed profile, not about the family, because the
 * reference driver answers them by MAC version:
 *
 *   * `rxdv_gate`: the receive data-valid gate is opened by the G/H/EP starts
 *     only (RTL_GIGA_MAC_VER_40 and later); the 8168evl start never touches
 *     MISC's bit 19;
 *   * `rx_early_off`: RX_EARLY_OFF is a G/H/EP RxConfig bit, not an E one;
 *   * `rx_multi_en`: RX_MULTI_EN belongs to the E generation and later;
 *   * `tx_auto_fifo`: TXCFG_AUTO_FIFO is the 8168evl-and-later ("evl_up")
 *     setting, not the E one;
 *   * `evl_cold_boot`: the EPHY and clock stages are the 8168evl's own.
 */
bool rtl8168_profile_rxdv_gate(const struct rtl8168_chip *chip);
bool rtl8168_profile_rx_early_off(const struct rtl8168_chip *chip);
bool rtl8168_profile_rx_multi_en(const struct rtl8168_chip *chip);
bool rtl8168_profile_tx_auto_fifo(const struct rtl8168_chip *chip);
bool rtl8168_profile_evl_cold_boot(const struct rtl8168_chip *chip);
/* Whether the part's PHY answers clause-22 registers 4/5/9/10 behind PHYAR. */
bool rtl8168_profile_phy_gigabit_mii(const struct rtl8168_chip *chip);

enum rtl8168_status {
    RTL8168_OK = 0,
    RTL8168_NULL = 1,
    RTL8168_IO_ERROR = 2,
    RTL8168_UNSUPPORTED_DEVICE = 3,
    RTL8168_MISSING_MEMORY_BAR = 4,
    RTL8168_BAD_BAR = 5,
    RTL8168_BAD_STATE = 6,
    RTL8168_INVALID_MAC = 7,
    RTL8168_RESET_TIMEOUT = 8,
    RTL8168_INVALID_LENGTH = 9,
    RTL8168_RING_FULL = 10,
    RTL8168_RING_EMPTY = 11,
    RTL8168_RX_NOT_READY = 12,
    RTL8168_RX_DROPPED = 13,
    RTL8168_UNSUPPORTED_DMA_ADDRESS = 14,
    RTL8168_CAPACITY = 15,
    RTL8168_ENABLE_FAILED = 16,
};

struct rtl8168_region_layout {
    uint32_t rx_descriptor_offset;
    uint32_t tx_descriptor_offset;
    uint32_t rx_buffer_offset;
    uint32_t tx_buffer_offset;
    uint32_t length;
};

struct rtl8168_bring_up {
    uint64_t rx_descriptor_physical;
    uint64_t tx_descriptor_physical;
    uint32_t rx_buffer_capacity;
};

static inline void rtl8168_barrier(void)
{
    __asm__ volatile ("" : : : "memory");
}

/* Identity */
bool rtl8168_pci_identity_supported(
    const struct pci_device_header *header);
enum rtl8168_status rtl8168_memory_bar(
    const struct pci_device_header *header,
    const struct pci_bar **bar);
enum rtl8168_status rtl8168_chip_identify(
    struct rtl8168_mmio_io *io,
    struct rtl8168_chip *chip);
const char *rtl8168_chip_name(const struct rtl8168_chip *chip);
/*
 * ITEM-399: the chip identity as the boot record carries it.
 *
 * `rtl8168_chip_family_string()` names the family the XID table mapped the
 * part to - `RTL8168-EXTENDED` (8168E and later, the acceptance host's 8168H
 * and the RTL8168E the product must support), `RTL8168-CLASSIC`, or `UNKNOWN`
 * for an XID outside the table (the caller then has the raw XID and says the
 * driver did not identify the family, instead of guessing).
 *
 * `rtl8168_chip_render_identity()` renders the two fields the
 * `[PRODUCTION-NET]` record publishes - ` xid=0x2C8 family=RTL8168-EXTENDED` -
 * with its leading separator, into a caller-owned buffer, and returns the
 * number of bytes written without the terminator (0 when the value does not
 * fit, so a truncated identity can never be published as a complete one).
 */
const char *rtl8168_chip_family_string(
    enum rtl8168_chip_family family);
size_t rtl8168_chip_render_identity(
    const struct rtl8168_chip *chip,
    char *buffer,
    size_t capacity);
bool rtl8168_mac_valid(const uint8_t mac[RTL8168_MAC_LENGTH]);
enum rtl8168_status rtl8168_mac_read(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    uint8_t mac[RTL8168_MAC_LENGTH]);
bool rtl8168_dma_address_supported(uint64_t physical);

/* Bring-up */
enum rtl8168_status rtl8168_interrupts_mask(struct rtl8168_mmio_io *io);
enum rtl8168_status rtl8168_reset(struct rtl8168_mmio_io *io);
enum rtl8168_status rtl8168_bring_up(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    const struct rtl8168_bring_up *configuration,
    const uint8_t mac[RTL8168_MAC_LENGTH]);
enum rtl8168_status rtl8168_link_status(
    struct rtl8168_mmio_io *io,
    bool *link_up);

/*
 * ITEM-589: how this part's PHY is reached.
 *
 * The E/F-era 8168 parts expose clause-22 MII behind PHYAR (0x60); the later
 * 8168G/H/EP parts use the OCP doorbell, which this driver does not claim.
 * The owner's Toshiba L875 reports XID 0x2C9 - one bit above the 0x2C8 the
 * exact-XID list named - while the production family table's mask row
 * `{ 0x7C8, 0x2C8 }` matches it, so the MII window is the right one for that
 * part and skipping it left the link down forever.  The access method is
 * derived from the same table the family comes from, so a mask the table
 * accepts cannot be skipped by a second, hand-written list.
 */
enum rtl8168_phy_access {
    RTL8168_PHY_ACCESS_NONE = 0,
    RTL8168_PHY_ACCESS_MII_PHYAR = 1,
    RTL8168_PHY_ACCESS_OCP = 2,
};

/*
 * ITEM-591: advertisement read-back.
 *
 * `advertise` is always the value MII register 4 answered, never the constant
 * the driver wrote: the Toshiba run printed `adv=0x03E1` while the part was
 * free to answer something else, so the record could not say whether the PHY
 * had accepted the advertisement.  `advertise_written` keeps the value the
 * driver last programmed and `advertise_valid` says whether the last read of
 * the register answered at all.  A read that fails keeps the first PHY error
 * and leaves the configuration's own verdict (CONFIGURED/REFUSED), the link
 * state and the auto-negotiation result as the separate answers they are.
 */
/*
 * ITEM-583: the MII management view of the PHY.  `rtl8168_phy_configure()`
 * clears a firmware power-down/isolate state, advertises the standard
 * 10/100/1000 set and restarts auto-negotiation; `rtl8168_phy_refresh()`
 * re-reads BMCR/BMSR/LPA so a link that comes up after boot (and the
 * negotiated speed/duplex) is published instead of being inferred from the
 * single PHYstatus byte.  Both are bounded register operations and never
 * spin forever on a part that does not answer.
 */
struct rtl8168_phy_state {
    bool valid;
    bool status_valid;
    bool aneg_start_issued;
    bool aneg_start_attempted;
    bool link_up;
    bool autoneg_complete;
    bool powered_down;
    uint16_t phy_id1;
    uint16_t phy_id2;
    uint16_t bmcr;
    uint16_t bmsr;
    /* ITEM-591: the read-back value of MII register 4, the value the driver
       wrote into it, and whether the last read of it actually answered.  A
       record that printed only the constant could not tell a PHY that took
       the advertisement from one that ignored it. */
    uint16_t advertise;
    uint16_t advertise_written;
    bool advertise_valid;
    uint16_t lpa;
    uint32_t configures;
    uint32_t refreshes;
    /*
     * ITEM-589: what the last configuration attempt did and the first PHY
     * error this state ever saw.  The evidence a hardware run needs is not
     * "the link is down" but "the configuration was skipped/refused and this
     * is the first reason it gave".
     */
    enum rtl8168_phy_access access;
    bool configure_attempted;
    enum rtl8168_status config_status;
    enum rtl8168_status first_error;
    /*
     * ITEM-592: the PHY-side evidence a link that never comes up needs.
     *
     * The Toshiba report of 40b35ec3 showed a PHY that answered every read and
     * never saw a link partner: `lpa=0x0000`.  One sample cannot say whether
     * auto-negotiation never started, never completed, or completed and lost
     * the link, so the state keeps the initial PHYstatus and BMCR, the first
     * distinct BMSR samples (with how many reads and whether link/AN were ever
     * seen), and the 1000BASE-T registers 9 and 10 with their own read
     * verdicts - register 4 alone can never name a gigabit problem.
     */
    uint8_t phy_status_initial;
    uint8_t phy_status_latest;
    bool phy_status_initial_valid;
    bool phy_status_latest_valid;
    uint16_t bmcr_initial;
    bool bmcr_initial_valid;
#define RTL8168_PHY_BMSR_SAMPLES 4U
    uint16_t bmsr_sequence[RTL8168_PHY_BMSR_SAMPLES];
    uint8_t bmsr_sequence_count;
    uint32_t bmsr_reads;
    bool link_seen;
    bool aneg_complete_seen;
    uint16_t advertise_1000;
    uint16_t advertise_1000_written;
    bool advertise_1000_valid;
    uint16_t lpa_1000;
    bool lpa_1000_valid;
    uint16_t page;
    bool page_valid;
    /* ITEM-592: the confirmed profile advertises 1000BASE-T (register 9). */
    bool gigabit;
    struct rtl8168_phy_page_observation page_selection;
    struct rtl8168_phy_failure failure;
};

/*
 * ITEM-592: the cold-boot stages, in the order the reference driver's start
 * performs them for this generation.
 *
 *   reset  - ChipCmd = CmdReset, bounded wait for the bit to clear;
 *   phy_resume / phy_tune / phy_reset - EVL power resume, vendor register
 *            settings and an initiated BMCR reset (distinct from ChipCmd);
 *   ephy   - the 8168evl EPHY table over EPHYAR (analog/clock of the
 *            integrated PHY), before the MAC is enabled;
 *   clock  - the 8168evl clock/FIFO/MCU/EEE programming (ERI 0xc0/0xb8/0xc8/
 *            0xe8/0x1d0/0x1dc/0x1b0/0xcc/0xd0/0xd4, MCU, DLLPR, MISC PWM,
 *            Config5), before the MAC is enabled;
 *   mac    - the ordinary MAC start: config unlock, CPLUSCMD, station address,
 *            descriptor registers, lock, commit, ChipCmd enable, RxConfig,
 *            TxConfig, multicast filter;
 *   phy    - page 0, the 10/100 advertisement in register 4 and the gigabit
 *            one in register 9, BMCR auto-negotiation, read-back of 4/9/10/5;
 *   aneg_start - only the BMCR command result; BMSR completion is a separate
 *            live observation, never inferred from a successful command.
 *
 * A stage that does not apply to the confirmed profile is SKIPPED by name; a
 * stage that fails stops the boot and the failed stage is the one recorded.
 */
enum rtl8168_cold_boot_stage {
    RTL8168_STAGE_RESET = 0,
    RTL8168_STAGE_PHY_RESUME = 1,
    RTL8168_STAGE_PHY_TUNE = 2,
    RTL8168_STAGE_PHY_RESET = 3,
    RTL8168_STAGE_EPHY = 4,
    RTL8168_STAGE_CLOCK = 5,
    RTL8168_STAGE_MAC = 6,
    RTL8168_STAGE_PHY = 7,
    RTL8168_STAGE_ANEG = 8,
    RTL8168_STAGE_COUNT = 9,
};

enum rtl8168_stage_result {
    RTL8168_STAGE_OK = 0,
    RTL8168_STAGE_FAILED = 1,
    RTL8168_STAGE_SKIPPED = 2,
};

struct rtl8168_cold_boot_report {
    enum rtl8168_profile profile;
    const char *profile_name;
    enum rtl8168_stage_result stages[RTL8168_STAGE_COUNT];
    enum rtl8168_cold_boot_stage failed_stage;
    enum rtl8168_status first_failure;
    bool failed;
    /*
     * ITEM-592: firmware.  The reference driver applies `rtl8168e-3.fw` to
     * this part's PHY MCU before the PHY configuration, and its vendor tuning
     * tuning on top of that. HFOC carries no vendor blob, so the report names
     * the dependency it did not satisfy instead of pretending it did.
     */
    bool firmware_skipped;
};

enum rtl8168_status rtl8168_phy_read(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t *value);
enum rtl8168_status rtl8168_phy_write(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t value);
enum rtl8168_status rtl8168_phy_configure(
    struct rtl8168_mmio_io *io,
    struct rtl8168_phy_state *state);
enum rtl8168_status rtl8168_phy_refresh(
    struct rtl8168_mmio_io *io,
    struct rtl8168_phy_state *state);
/*
 * ITEM-592: the EPHY window (0x80) - the integrated PHY's analog/clock block,
 * reached with the same two-bit handshake as PHYAR but the reference driver's
 * EPHY timing.
 */
enum rtl8168_status rtl8168_ephy_read(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t *value);
enum rtl8168_status rtl8168_ephy_write(
    struct rtl8168_mmio_io *io,
    uint8_t register_index,
    uint16_t value);
/* The PHY page register: page 0 is the standard clause-22 set. */
enum rtl8168_status rtl8168_phy_select_page(
    struct rtl8168_mmio_io *io,
    uint16_t page);
/*
 * ITEM-592: the whole cold start for a confirmed profile, in the reference
 * driver's order, with the stage results the record publishes.  `phy` is
 * filled by the PHY stages (and left zeroed when the profile has no PHY
 * window this driver drives); `report` is filled on every path. A NULL `phy`
 * explicitly requests MAC-only startup with PHY stages SKIPPED; production
 * always supplies it. No skipped PHY stage establishes complete PHY init.
 */
enum rtl8168_status rtl8168_cold_boot(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_chip *chip,
    const struct rtl8168_bring_up *configuration,
    const uint8_t mac[RTL8168_MAC_LENGTH],
    struct rtl8168_phy_state *phy,
    struct rtl8168_cold_boot_report *report);
const char *rtl8168_stage_string(enum rtl8168_cold_boot_stage stage);
const char *rtl8168_stage_result_string(enum rtl8168_stage_result result);
/*
 * The 8168E/8168F family (XID 0x2C0/0x2C1/0x2C8) reaches the PHY through
 * PHYAR.  The later 8168G/H/EP parts use the OCP doorbell instead, so this
 * driver does not claim those registers on their behalf; the caller keeps the
 * PHYstatus link bit as before for them.
 */
bool rtl8168_phy_mii_supported(const struct rtl8168_chip *chip);
/*
 * ITEM-589: the access method the chip's family table row names - `NONE` for a
 * chip that was never identified, `MII_PHYAR` for the E/F-era parts and `OCP`
 * for the later ones - and its stable name for the `[NET-PHY]` record.
 */
enum rtl8168_phy_access rtl8168_phy_access_method(
    const struct rtl8168_chip *chip);
const char *rtl8168_phy_access_string(enum rtl8168_phy_access access);
/*
 * ITEM-435: reads the receive descriptor base registers and the receiver
 * enable bit back from the part and reports whether the receiver really holds
 * the ring the driver programmed.  Non-fatal by design: the caller publishes
 * the answer rather than failing a transmit-capable device.
 */
enum rtl8168_status rtl8168_rx_armed(
    struct rtl8168_mmio_io *io,
    uint64_t rx_descriptor_physical,
    bool *armed);

/*
 * ITEM-643: fill `out` from the mapped register window and the driver's RX
 * ring.  Returns RTL8168_OK when every register read succeeded, RTL8168_NULL
 * for a missing argument, or the first MMIO read error.  On error `out` is
 * still safe to print: available=false and reason names the failure.
 */
enum rtl8168_status rtl8168_rx_diagnostic_read(
    struct rtl8168_mmio_io *io,
    const struct rtl8168_ring *ring,
    struct rtl8168_rx_diagnostic *out);

/*
 * ITEM-645: the runtime acknowledge the poll loop was missing.
 *
 * Read IntrStatus once, then write back exactly the observed bits that must be
 * cleared (write-one-to-clear).  When `descriptors_available` is false the
 * receive-stopped pair (`RTL8168_INTR_RX_STOPPED`) is left latched: the driver
 * has not yet proven the ring has room, and clearing the stop condition then
 * would re-arm a receiver with nowhere to write.  The caller passes true after
 * a pass that ended on a descriptor the device had not written, i.e. with the
 * cursor descriptor free.
 *
 * `observed` (if not NULL) receives the status word the read returned and
 * `acknowledged` the mask the write cleared; both are zeroed first, and an
 * `acknowledged` of zero means no write was needed or the write failed.  A
 * read of all ones is a dead bus, not sixteen events: it is reported as
 * RTL8168_IO_ERROR and never written back.
 */
enum rtl8168_status rtl8168_intr_ack_events(
    struct rtl8168_mmio_io *io,
    bool descriptors_available,
    uint16_t *observed,
    uint16_t *acknowledged);

enum rtl8168_status rtl8168_transmit_poll(struct rtl8168_mmio_io *io);

/* Ring arithmetic and descriptor state machines */
void rtl8168_ring_init(
    struct rtl8168_ring *ring,
    struct rtl8168_descriptor *descriptors,
    uint32_t capacity);
enum rtl8168_status rtl8168_region_calculate(
    struct rtl8168_region_layout *layout);
uint32_t rtl8168_ring_next(const struct rtl8168_ring *ring, uint32_t index);
enum rtl8168_status rtl8168_rx_post(
    struct rtl8168_ring *ring,
    uint64_t buffer_physical,
    uint32_t buffer_capacity);
enum rtl8168_status rtl8168_rx_complete(
    const struct rtl8168_ring *ring,
    uint32_t buffer_capacity,
    uint32_t *frame_length);
enum rtl8168_status rtl8168_rx_repost(
    struct rtl8168_ring *ring,
    uint64_t buffer_physical,
    uint32_t buffer_capacity);
enum rtl8168_status rtl8168_tx_program(
    struct rtl8168_ring *ring,
    uint64_t frame_physical,
    uint32_t frame_length,
    uint32_t frame_capacity);
enum rtl8168_status rtl8168_tx_reclaim(
    struct rtl8168_ring *ring,
    uint32_t *reclaimed);
/*
 * ITEM-428: hand the whole transmit ring back to the CPU.
 *
 * A device that stops clearing its descriptors' OWN bit leaves the transmit
 * ring full forever, and every frame after that is refused.  The recovery is
 * the reference driver's: the caller stops the part first with
 * rtl8168_reset(), calls this to make the memory agree with that stop, re-arms
 * the receive ring, and only then brings the part up again with
 * rtl8168_bring_up() - every descriptor is written as CPU-owned, the
 * programming cursor and the in-flight count return to zero, and the number of
 * frames the driver had handed over but never saw completed is counted as
 * abandoned and returned to the caller.
 *
 * It is only valid while the device is stopped: a rewind of a live part would
 * ask it to fetch the stale descriptors the ring is full of, and it could
 * catch one mid-write (address cleared, OWN still set).
 */
enum rtl8168_status rtl8168_tx_rewind(
    struct rtl8168_ring *ring,
    uint32_t *abandoned);
uint32_t rtl8168_tx_frame_length(size_t length);
const char *rtl8168_status_string(enum rtl8168_status status);

#endif
