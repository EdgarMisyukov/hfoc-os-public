#ifndef HFOC_E1000_H
#define HFOC_E1000_H

#include "pci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-494: the second vendor's NIC core - Intel 8254x, the "e1000" family.
 *
 * The first vendor's driver (RTL8168 10EC:8168) proved the shape the product
 * needs: a memory-mapped register file, legacy descriptor rings, a polled
 * receive and transmit path and a link bit.  The 8254x family has the same
 * shape, so this is a second implementation of the same contract and not a
 * second architecture: `network_backend.h` asks for a readiness, a device
 * snapshot and a link reading, and this driver answers with the ids, the MAC
 * and the link bit the hardware reports.
 *
 * Register offsets, descriptor layouts and the bring-up order are the ones the
 * 8254x software developer's manual documents and the Linux e1000 driver
 * (drivers/net/ethernet/intel/e1000/e1000_hw.h, e1000_main.c) programs:
 *
 *   - CTRL (0x0000), STATUS (0x0008), EECD/EERD (0x0010/0x0014),
 *     ICR/IMS/IMC (0x00C0/0x00D0/0x00D8), RCTL (0x0100), TCTL (0x0400),
 *     TIPG (0x0410), the receive ring RDBAL/RDBAH/RDLEN/RDH/RDT
 *     (0x2800..0x2818), the transmit ring TDBAL/TDBAH/TDLEN/TDH/TDT
 *     (0x3800..0x3818), RAL/RAH (0x5400/0x5404) and the multicast table
 *     MTA (0x5200..0x527F);
 *   - the legacy 16-byte transmit descriptor (u64 buffer, u16 length, u8 cso,
 *     u8 cmd, u8 status, u8 css, u16 special) and receive descriptor
 *     (u64 buffer, u16 length, u16 checksum, u8 status, u8 errors,
 *     u16 special);
 *   - TX command bits EOP/IFCS/RS and status DD, RX status DD/EOP, and
 *     RCTL.SECRC, so the device strips the Ethernet CRC and the driver does
 *     not have to;
 *   - the receive filter the product wants: broadcast accepted, this station's
 *     address accepted, error frames and runts rejected;
 *   - CTRL.RST then poll until clear, IMC = all ones and IMS = 0 (this driver
 *     is poll driven; IRQ routing is out of scope), rings programmed with
 *     their base addresses, lengths and head/tail pointers.
 *
 * This translation unit owns everything that can be reasoned about and tested
 * without the part in the socket: identity, the reset and bring-up register
 * sequences, the descriptor state machines, ring arithmetic, the frame-length
 * rules and the MAC read/validate.  The x86 adapter owns mapping the BAR,
 * allocating the DMA region and feeding lwIP.
 *
 * This is QEMU/OVMF-proven and NOT physical-hardware-proven: the acceptance
 * host carries no Intel NIC, and no record may call the Intel path hardware
 * tested until a machine with the part runs it.
 */

#define E1000_VENDOR_ID 0x8086U
/* 82540EM: the part QEMU's q35 `e1000` device emulates (8086:100E). */
#define E1000_DEVICE_ID_82540EM 0x100EU
#define E1000_PCI_CLASS_NETWORK 0x02U
#define E1000_PCI_SUBCLASS_ETHERNET 0x00U

#define E1000_MAC_LENGTH 6U
#define E1000_FRAME_MINIMUM 60U
#define E1000_FRAME_MAXIMUM 1518U

/* Register file (e1000_hw.h register offsets). */
#define E1000_REG_CTRL 0x0000U
#define E1000_REG_STATUS 0x0008U
#define E1000_REG_EECD 0x0010U
#define E1000_REG_EERD 0x0014U
#define E1000_REG_MDIC 0x0020U
#define E1000_REG_ICR 0x00C0U
#define E1000_REG_IMS 0x00D0U
#define E1000_REG_IMC 0x00D8U
#define E1000_REG_RCTL 0x0100U
#define E1000_REG_TCTL 0x0400U
#define E1000_REG_TIPG 0x0410U
#define E1000_REG_RDBAL 0x2800U
#define E1000_REG_RDBAH 0x2804U
#define E1000_REG_RDLEN 0x2808U
#define E1000_REG_RDH 0x2810U
#define E1000_REG_RDT 0x2818U
#define E1000_REG_TDBAL 0x3800U
#define E1000_REG_TDBAH 0x3804U
#define E1000_REG_TDLEN 0x3808U
#define E1000_REG_TDH 0x3810U
#define E1000_REG_TDT 0x3818U
#define E1000_REG_MTA 0x5200U
#define E1000_REG_RAL 0x5400U
#define E1000_REG_RAH 0x5404U

/*
 * CTRL.  RST is self-clearing; SLU is the "set link up" bit the reference
 * driver sets once and the PHY then owns the link.
 */
#define E1000_CTRL_RST UINT32_C(1) << 26U
#define E1000_CTRL_SLU UINT32_C(1) << 6U
/*
 * STATUS.LU: the link is up, the same reading rtl8168_link_status returns.
 * The link-up bit is bit 1 of STATUS (0x00000002) in the 8254x register map -
 * bit 2 is TXOFF - and the production proof caught this driver reading bit 2:
 * a machine whose link was up read down for a whole boot.
 */
#define E1000_STATUS_LU UINT32_C(1) << 1U

/*
 * MDIC: the management interface to the PHY.  The status bit is set by the
 * part when a command completes, and the error bit says the PHY refused the
 * register; the driver polls for the first and reports the second.
 *
 * MII_BMCR is PHY register 0; AUTOEN keeps auto-negotiation enabled (QEMU's
 * PHY model only restarts negotiation when the written value still carries
 * it) and ANRESTART asks for a new negotiation.  The link bit (STATUS.LU) only
 * comes up after that negotiation completes, which is why a machine whose peer
 * appeared after the device was reset needs the restart.
 */
#define E1000_MDIC_DATA_MASK UINT32_C(0x0000FFFF)
#define E1000_MDIC_REG_SHIFT 16U
#define E1000_MDIC_REG_MASK UINT32_C(0x001F0000)
#define E1000_MDIC_PHY_SHIFT 21U
#define E1000_MDIC_PHY_MASK UINT32_C(0x03E00000)
#define E1000_MDIC_PHY_ADDRESS 1U
#define E1000_MDIC_OP_WRITE UINT32_C(1) << 26U
#define E1000_MDIC_READY UINT32_C(1) << 28U
#define E1000_MDIC_ERROR UINT32_C(1) << 30U
#define E1000_MII_BMCR 0U
#define E1000_MII_BMCR_SPEED1000 UINT32_C(0x0040)
#define E1000_MII_BMCR_FULL_DUPLEX UINT32_C(0x0100)
#define E1000_MII_BMCR_ANRESTART UINT32_C(0x0200)
#define E1000_MII_BMCR_AUTOEN UINT32_C(0x1000)
#define E1000_MDIC_POLL_LIMIT 100000U

/*
 * RCTL.  EN enables the receiver; BAM accepts broadcast (bit 15, the bit the
 * receive filter actually consults - the production proof caught this driver
 * setting SBP, "store bad packets", and dropping every broadcast offer); UPE
 * and MPE would accept every unicast/multicast address, which this product
 * does not want; SECRC strips the Ethernet CRC; BSIZE 2048 is bits 16-17 = 0;
 * the long-packet bit stays clear because this driver rejects frames longer
 * than the maximum.
 */
#define E1000_RCTL_EN UINT32_C(1) << 1U
#define E1000_RCTL_SBP UINT32_C(1) << 2U
#define E1000_RCTL_UPE UINT32_C(1) << 3U
#define E1000_RCTL_MPE UINT32_C(1) << 4U
#define E1000_RCTL_LPE UINT32_C(1) << 5U
#define E1000_RCTL_BAM UINT32_C(1) << 15U
#define E1000_RCTL_SECRC UINT32_C(1) << 26U
#define E1000_RCTL_BSIZE_2048 0U

/*
 * TCTL.  EN enables the transmitter, PSP pads short frames, CT=0x10 and
 * COLD=0x40 are the collision thresholds and distance the reference driver
 * uses, RTLC retransmits on late collisions.
 */
#define E1000_TCTL_EN UINT32_C(1) << 1U
#define E1000_TCTL_PSP UINT32_C(1) << 3U
#define E1000_TCTL_CT (UINT32_C(0x10) << 4U)
#define E1000_TCTL_COLD (UINT32_C(0x40) << 12U)
#define E1000_TCTL_RTLC UINT32_C(1) << 24U
/* TIPG: IPGT 10, IPGR1 8, IPGR2 6 - the reference driver's values. */
#define E1000_TIPG_VALUE ((UINT32_C(10)) | (UINT32_C(8) << 10U) | \
                          (UINT32_C(6) << 20U))

/* Transmit descriptor command and status bits. */
#define E1000_TX_CMD_EOP UINT8_C(0x01)
#define E1000_TX_CMD_IFCS UINT8_C(0x02)
#define E1000_TX_CMD_RS UINT8_C(0x08)
#define E1000_TX_STATUS_DD UINT8_C(0x01)

/* Receive descriptor status and error bits. */
#define E1000_RX_STATUS_DD UINT8_C(0x01)
#define E1000_RX_STATUS_EOP UINT8_C(0x02)
#define E1000_RX_ERROR_MASK UINT8_C(0x7F)

/* The multicast table is 128 words and is cleared on bring-up. */
#define E1000_MTA_BYTES 0x80U
#define E1000_MIN_MMIO_LENGTH 0x6000U

#define E1000_RX_DESCRIPTOR_COUNT 16U
#define E1000_TX_DESCRIPTOR_COUNT 16U
#define E1000_DESCRIPTOR_SIZE 16U
#define E1000_DESCRIPTOR_ALIGNMENT 16U
#define E1000_RX_BUFFER_CAPACITY 2048U
#define E1000_TX_BUFFER_CAPACITY 1536U
#define E1000_RX_BUFFER_ALIGNMENT 16U
#define E1000_TX_BUFFER_ALIGNMENT 16U
#define E1000_RESET_POLL_LIMIT 100000U
#define E1000_DMA_ADDRESS_LIMIT UINT64_C(0x100000000)

struct e1000_tx_descriptor {
    volatile uint64_t address;
    volatile uint16_t length;
    volatile uint8_t cso;
    volatile uint8_t command;
    volatile uint8_t status;
    volatile uint8_t css;
    volatile uint16_t special;
};

struct e1000_rx_descriptor {
    volatile uint64_t address;
    volatile uint16_t length;
    volatile uint16_t checksum;
    volatile uint8_t status;
    volatile uint8_t errors;
    volatile uint16_t special;
};

struct e1000_ring {
    void *descriptors;
    uint32_t capacity;
    uint32_t index;
    uint32_t in_flight;
    uint32_t submitted;
    uint32_t reclaimed;
    uint32_t abandoned;
};

struct e1000_mmio_io {
    void *context;
    bool (*read32)(void *context, uint32_t offset, uint32_t *value);
    bool (*write32)(void *context, uint32_t offset, uint32_t value);
};

enum e1000_status {
    E1000_OK = 0,
    E1000_NULL = 1,
    E1000_IO_ERROR = 2,
    E1000_UNSUPPORTED_DEVICE = 3,
    E1000_MISSING_MEMORY_BAR = 4,
    E1000_BAD_BAR = 5,
    E1000_BAD_STATE = 6,
    E1000_INVALID_MAC = 7,
    E1000_RESET_TIMEOUT = 8,
    E1000_INVALID_LENGTH = 9,
    E1000_RING_FULL = 10,
    E1000_RING_EMPTY = 11,
    E1000_RX_NOT_READY = 12,
    E1000_RX_DROPPED = 13,
    E1000_UNSUPPORTED_DMA_ADDRESS = 14,
    E1000_CAPACITY = 15,
    E1000_ENABLE_FAILED = 16,
    /* ITEM-494: the PHY command never reported completion. */
    E1000_PHY_TIMEOUT = 17,
};

struct e1000_region_layout {
    uint32_t rx_descriptor_offset;
    uint32_t tx_descriptor_offset;
    uint32_t rx_buffer_offset;
    uint32_t tx_buffer_offset;
    uint32_t length;
};

struct e1000_bring_up {
    uint64_t rx_descriptor_physical;
    uint64_t tx_descriptor_physical;
    uint32_t rx_buffer_capacity;
};

static inline void e1000_barrier(void)
{
    __asm__ volatile ("" : : : "memory");
}

/* Identity. */
bool e1000_pci_identity_supported(const struct pci_device_header *header);
enum e1000_status e1000_memory_bar(const struct pci_device_header *header,
                                   const struct pci_bar **bar);
bool e1000_mac_valid(const uint8_t mac[E1000_MAC_LENGTH]);
enum e1000_status e1000_mac_read(struct e1000_mmio_io *io,
                                 uint8_t mac[E1000_MAC_LENGTH]);
bool e1000_dma_address_supported(uint64_t physical);

/* Registers. */
enum e1000_status e1000_interrupts_mask(struct e1000_mmio_io *io);
enum e1000_status e1000_reset(struct e1000_mmio_io *io);
enum e1000_status e1000_bring_up(struct e1000_mmio_io *io,
                                 const struct e1000_bring_up *bring_up);
enum e1000_status e1000_link_status(struct e1000_mmio_io *io, bool *link_up);
/*
 * ITEM-494: restart PHY auto-negotiation, the step that makes STATUS.LU come
 * up on a part whose link partner appeared after the reset.  The command is
 * written to MDIC and the part's completion is polled with a bound.
 */
enum e1000_status e1000_restart_autoneg(struct e1000_mmio_io *io);
/* Whether the receiver really holds the ring (RCTL.EN read back). */
enum e1000_status e1000_rx_armed(struct e1000_mmio_io *io, bool *armed);

/* Rings and descriptors. */
void e1000_ring_init(struct e1000_ring *ring, void *descriptors,
                     uint32_t capacity);
enum e1000_status e1000_region_calculate(struct e1000_region_layout *layout);
uint32_t e1000_ring_next(const struct e1000_ring *ring, uint32_t index);
enum e1000_status e1000_rx_post(struct e1000_ring *ring, uint64_t physical,
                                uint32_t buffer_capacity);
enum e1000_status e1000_rx_ready(const struct e1000_ring *ring,
                                 uint32_t index);
enum e1000_status e1000_rx_complete(const struct e1000_ring *ring,
                                    uint32_t index, uint32_t buffer_capacity,
                                    uint16_t *length);
enum e1000_status e1000_rx_repost(struct e1000_ring *ring,
                                  uint32_t index, uint64_t physical);
uint32_t e1000_tx_frame_length(size_t length);
enum e1000_status e1000_tx_program(struct e1000_ring *ring, uint64_t physical,
                                   size_t length, uint32_t frame_capacity);
enum e1000_status e1000_tx_reclaim(struct e1000_ring *ring,
                                   uint32_t *reclaimed);
enum e1000_status e1000_tx_rewind(struct e1000_ring *ring);

const char *e1000_status_string(enum e1000_status status);

#endif
