#include "e1000.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_E1000_HOST_TEST

/*
 * ITEM-494 host proof of the Intel 8254x driver core: identity and claim,
 * the register file the bring-up writes, the MAC read, the descriptor state
 * machines and the ring arithmetic.  The part is a fixture: an array that
 * answers reads, records writes and can be told to fail, never a fake PASS.
 */

#define TEST_MMIO_LENGTH 0x6000U
#define TEST_DESCRIPTOR_COUNT 16U
#define TEST_RX_BUFFER_CAPACITY 2048U
#define TEST_TX_BUFFER_CAPACITY 1536U
#define TEST_RX_PHYSICAL UINT64_C(0x300000)
#define TEST_TX_PHYSICAL UINT64_C(0x400000)

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

struct mmio_fixture {
    uint8_t registers[TEST_MMIO_LENGTH];
    bool fail_read;
    bool fail_write;
    bool reset_never_clears;
    bool rx_enable_never_sticks;
    uint32_t mac_low;
    uint32_t mac_high;

    /* Observations. */
    uint32_t reset_writes;
    uint32_t control_value;
    uint32_t control_writes;
    uint32_t rctl_value;
    uint32_t rctl_writes;
    uint32_t tctl_value;
    uint32_t tipg_value;
    uint32_t imc_value;
    uint32_t ims_value;
    uint32_t icr_reads;
    uint32_t mta_zero_writes;
    uint32_t rdba_low;
    uint32_t rdba_high;
    uint32_t rdlen;
    uint32_t rdh;
    uint32_t rdt;
    uint32_t tdba_low;
    uint32_t tdba_high;
    uint32_t tdlen;
    uint32_t tdh;
    uint32_t tdt;
    uint32_t mdic_writes;
    uint32_t mdic_command;
    bool mdic_never_ready;
    bool mdic_error;
};

static void store32(uint8_t *base, uint32_t offset, uint32_t value)
{
    base[offset] = (uint8_t)(value & 0xFFU);
    base[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
    base[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
    base[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
}

static uint32_t load32(const uint8_t *base, uint32_t offset)
{
    return (uint32_t)base[offset] |
           ((uint32_t)base[offset + 1U] << 8U) |
           ((uint32_t)base[offset + 2U] << 16U) |
           ((uint32_t)base[offset + 3U] << 24U);
}

static bool fixture_read32(void *context, uint32_t offset, uint32_t *value)
{
    struct mmio_fixture *fixture = context;

    if (fixture->fail_read || value == NULL ||
        offset + 4U > TEST_MMIO_LENGTH) {
        return false;
    }
    if (offset == E1000_REG_ICR) {
        ++fixture->icr_reads;
    }
    *value = load32(fixture->registers, offset);
    return true;
}

static bool fixture_write32(void *context, uint32_t offset, uint32_t value)
{
    struct mmio_fixture *fixture = context;

    if (fixture->fail_write || offset + 4U > TEST_MMIO_LENGTH) {
        return false;
    }
    switch (offset) {
    case E1000_REG_CTRL:
        ++fixture->control_writes;
        fixture->control_value = value;
        if ((value & E1000_CTRL_RST) != 0U) {
            ++fixture->reset_writes;
            if (!fixture->reset_never_clears) {
                value &= ~E1000_CTRL_RST;
            }
        }
        break;
    case E1000_REG_RCTL:
        ++fixture->rctl_writes;
        fixture->rctl_value = value;
        if ((value & E1000_RCTL_EN) != 0U &&
            fixture->rx_enable_never_sticks) {
            value &= ~E1000_RCTL_EN;
        }
        break;
    case E1000_REG_TCTL:
        fixture->tctl_value = value;
        break;
    case E1000_REG_TIPG:
        fixture->tipg_value = value;
        break;
    case E1000_REG_IMC:
        fixture->imc_value = value;
        break;
    case E1000_REG_IMS:
        fixture->ims_value = value;
        break;
    case E1000_REG_RDBAL:
        fixture->rdba_low = value;
        break;
    case E1000_REG_RDBAH:
        fixture->rdba_high = value;
        break;
    case E1000_REG_RDLEN:
        fixture->rdlen = value;
        break;
    case E1000_REG_RDH:
        fixture->rdh = value;
        break;
    case E1000_REG_RDT:
        fixture->rdt = value;
        break;
    case E1000_REG_TDBAL:
        fixture->tdba_low = value;
        break;
    case E1000_REG_TDBAH:
        fixture->tdba_high = value;
        break;
    case E1000_REG_TDLEN:
        fixture->tdlen = value;
        break;
    case E1000_REG_TDH:
        fixture->tdh = value;
        break;
    case E1000_REG_TDT:
        fixture->tdt = value;
        break;
    case E1000_REG_MDIC:
        /*
         * ITEM-494: the PHY command interface.  A real part sets READY (and
         * ERROR on failure) itself, which is what the driver polls for.
         */
        ++fixture->mdic_writes;
        fixture->mdic_command = value;
        value &= ~(E1000_MDIC_READY | E1000_MDIC_ERROR);
        if (!fixture->mdic_never_ready) {
            value |= E1000_MDIC_READY;
        }
        if (fixture->mdic_error) {
            value |= E1000_MDIC_ERROR;
        }
        break;
    default:
        if (offset >= E1000_REG_MTA &&
            offset < E1000_REG_MTA + E1000_MTA_BYTES && value == 0U) {
            ++fixture->mta_zero_writes;
        }
        break;
    }
    store32(fixture->registers, offset, value);
    return true;
}

static void fixture_mmio(struct mmio_fixture *fixture, struct e1000_mmio_io *io)
{
    memset(io, 0, sizeof(*io));
    io->context = fixture;
    io->read32 = fixture_read32;
    io->write32 = fixture_write32;
}

static void fixture_mac(struct mmio_fixture *fixture, const uint8_t mac[6])
{
    fixture->mac_low = (uint32_t)mac[0] | ((uint32_t)mac[1] << 8U) |
                       ((uint32_t)mac[2] << 16U) | ((uint32_t)mac[3] << 24U);
    fixture->mac_high = (uint32_t)mac[4] | ((uint32_t)mac[5] << 8U);
    store32(fixture->registers, E1000_REG_RAL, fixture->mac_low);
    store32(fixture->registers, E1000_REG_RAH,
            fixture->mac_high | (UINT32_C(1) << 31U));
}

static struct pci_device_header test_header(uint16_t vendor, uint16_t device,
                                            uint8_t base_class,
                                            uint8_t subclass)
{
    struct pci_device_header header;

    memset(&header, 0, sizeof(header));
    header.present = true;
    header.vendor_id = vendor;
    header.device_id = device;
    header.base_class = base_class;
    header.subclass = subclass;
    header.bars[0].present = true;
    header.bars[0].io_space = false;
    header.bars[0].base = UINT64_C(0xFEB00000);
    header.bars[0].length = 0x20000U;
    return header;
}

static void test_identity(void)
{
    const struct pci_device_header supported =
        test_header(E1000_VENDOR_ID, E1000_DEVICE_ID_82540EM,
                    E1000_PCI_CLASS_NETWORK, E1000_PCI_SUBCLASS_ETHERNET);
    const struct pci_device_header intel_other =
        test_header(E1000_VENDOR_ID, 0x10D3U, E1000_PCI_CLASS_NETWORK,
                    E1000_PCI_SUBCLASS_ETHERNET);
    const struct pci_device_header wrong_class =
        test_header(E1000_VENDOR_ID, E1000_DEVICE_ID_82540EM, 0x01U, 0x06U);
    struct pci_device_header absent = supported;

    CHECK(e1000_pci_identity_supported(&supported),
          "82540EM in the network class is this driver's part");
    CHECK(!e1000_pci_identity_supported(&intel_other),
          "a different Intel part is not claimed by the 8254x driver");
    CHECK(!e1000_pci_identity_supported(&wrong_class),
          "the class is part of the claim, not an extra");
    absent.present = false;
    CHECK(!e1000_pci_identity_supported(&absent),
          "an absent function is not a supported part");
    CHECK(!e1000_pci_identity_supported(NULL), "a null header is refused");
}

static void test_memory_bar(void)
{
    struct pci_device_header header =
        test_header(E1000_VENDOR_ID, E1000_DEVICE_ID_82540EM,
                    E1000_PCI_CLASS_NETWORK, E1000_PCI_SUBCLASS_ETHERNET);
    const struct pci_bar *bar = NULL;

    header.bars[0].length = 0x100U;
    CHECK(e1000_memory_bar(&header, &bar) == E1000_MISSING_MEMORY_BAR &&
              bar == NULL,
          "a window shorter than the register file is not the register file");
    header.bars[0].length = E1000_MIN_MMIO_LENGTH;
    CHECK(e1000_memory_bar(&header, &bar) == E1000_OK && bar != NULL &&
              bar->base == UINT64_C(0xFEB00000),
          "the first memory BAR that holds the register file is selected");
    header.bars[0].io_space = true;
    bar = NULL;
    CHECK(e1000_memory_bar(&header, &bar) == E1000_MISSING_MEMORY_BAR,
          "an I/O window is not a memory register file");
    CHECK(e1000_memory_bar(NULL, &bar) == E1000_NULL,
          "a null header is refused");
}

static void test_mac(void)
{
    static const uint8_t good[6] = {0x52U, 0x54U, 0x00U, 0x12U, 0x34U, 0x56U};
    static const uint8_t zero[6] = {0U, 0U, 0U, 0U, 0U, 0U};
    static const uint8_t broadcast[6] =
        {0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU};
    static const uint8_t multicast[6] =
        {0x01U, 0x00U, 0x5EU, 0x00U, 0x00U, 0x01U};
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;
    uint8_t mac[E1000_MAC_LENGTH];

    CHECK(e1000_mac_valid(good), "a unicast station address is valid");
    CHECK(!e1000_mac_valid(zero), "an all-zero address is not an address");
    CHECK(!e1000_mac_valid(broadcast), "the broadcast address is not a station");
    CHECK(!e1000_mac_valid(multicast), "a multicast address is not a station");

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    fixture_mac(&fixture, good);
    CHECK(e1000_mac_read(&io, mac) == E1000_OK &&
              memcmp(mac, good, sizeof(good)) == 0,
          "the MAC is read from the part's own receive address registers");

    fixture_mac(&fixture, zero);
    CHECK(e1000_mac_read(&io, mac) == E1000_INVALID_MAC,
          "a part that reports no address is refused, not published");
    CHECK(memcmp(mac, zero, sizeof(zero)) == 0,
          "a refused MAC read leaves no half-address behind");
    fixture.fail_read = true;
    CHECK(e1000_mac_read(&io, mac) == E1000_IO_ERROR,
          "a register read that failed is an I/O error");
    CHECK(e1000_mac_read(NULL, mac) == E1000_NULL, "a null io is refused");
}

static void test_reset_and_interrupts(void)
{
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    CHECK(e1000_reset(&io) == E1000_OK && fixture.reset_writes == 1U,
          "the reset writes CTRL.RST and waits for the bit to clear");
    fixture.reset_never_clears = true;
    CHECK(e1000_reset(&io) == E1000_RESET_TIMEOUT,
          "a part that never clears RST is a timeout, not a pass");
    fixture.fail_write = true;
    CHECK(e1000_reset(&io) == E1000_IO_ERROR,
          "a register write that failed stops the reset");
    fixture.fail_write = false;
    CHECK(e1000_reset(NULL) == E1000_NULL, "a null io is refused");

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    CHECK(e1000_interrupts_mask(&io) == E1000_OK &&
              fixture.imc_value == UINT32_C(0xFFFFFFFF) &&
              fixture.ims_value == 0U && fixture.icr_reads == 1U,
          "every interrupt is masked and the pending cause is read clear");
}

static void test_bring_up(void)
{
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;
    struct e1000_bring_up bring_up;

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    memset(&bring_up, 0, sizeof(bring_up));
    bring_up.rx_descriptor_physical = TEST_RX_PHYSICAL;
    bring_up.tx_descriptor_physical = TEST_TX_PHYSICAL;
    bring_up.rx_buffer_capacity = TEST_RX_BUFFER_CAPACITY;

    CHECK(e1000_bring_up(&io, &bring_up) == E1000_OK,
          "the bring-up programs the part");
    CHECK((fixture.rctl_value & E1000_RCTL_EN) != 0U &&
              (fixture.rctl_value & E1000_RCTL_BAM) != 0U &&
              (fixture.rctl_value & E1000_RCTL_SECRC) != 0U &&
              (fixture.rctl_value & E1000_RCTL_UPE) == 0U &&
              (fixture.rctl_value & E1000_RCTL_MPE) == 0U,
          "the receiver accepts broadcast and this station, and strips the CRC");
    CHECK((fixture.tctl_value & E1000_TCTL_EN) != 0U &&
              (fixture.tctl_value & E1000_TCTL_PSP) != 0U &&
              (fixture.tctl_value & E1000_TCTL_RTLC) != 0U,
          "the transmitter is enabled and configured");
    CHECK(fixture.tipg_value == E1000_TIPG_VALUE,
          "the inter-packet gap is the reference driver's value");
    CHECK(fixture.rdba_low == (uint32_t)(TEST_RX_PHYSICAL & 0xFFFFFFFFU) &&
              fixture.rdba_high == (uint32_t)(TEST_RX_PHYSICAL >> 32U) &&
              fixture.rdlen == TEST_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE &&
              fixture.rdh == 0U &&
              fixture.rdt == TEST_DESCRIPTOR_COUNT - 1U,
          "the receive ring is programmed with its base, length and tail");
    CHECK(fixture.tdba_low == (uint32_t)(TEST_TX_PHYSICAL & 0xFFFFFFFFU) &&
              fixture.tdba_high == (uint32_t)(TEST_TX_PHYSICAL >> 32U) &&
              fixture.tdlen == TEST_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE &&
              fixture.tdh == 0U && fixture.tdt == 0U,
          "the transmit ring is programmed with its base, length and tail");
    CHECK((fixture.control_value & E1000_CTRL_SLU) != 0U,
          "the link is set up once at bring-up");
    CHECK(fixture.mta_zero_writes == E1000_MTA_BYTES / 4U,
          "the multicast table is cleared, so no stale filter survives");

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    fixture.rx_enable_never_sticks = true;
    CHECK(e1000_bring_up(&io, &bring_up) == E1000_ENABLE_FAILED,
          "a receiver that did not take the enable bit is a named refusal");
    bring_up.rx_buffer_capacity = E1000_RX_BUFFER_CAPACITY + 16U;
    CHECK(e1000_bring_up(&io, &bring_up) == E1000_INVALID_LENGTH,
          "a buffer larger than the part's maximum is refused");
    bring_up.rx_buffer_capacity = 0U;
    CHECK(e1000_bring_up(&io, &bring_up) == E1000_INVALID_LENGTH,
          "a zero buffer capacity is refused");
    CHECK(e1000_bring_up(NULL, &bring_up) == E1000_NULL,
          "a null io is refused");
    CHECK(e1000_bring_up(&io, NULL) == E1000_NULL,
          "a null bring-up is refused");
}

static void test_link_and_arming(void)
{
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;
    bool value = false;

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    CHECK(e1000_link_status(&io, &value) == E1000_OK && !value,
          "a part with LU clear reads link down");
    store32(fixture.registers, E1000_REG_STATUS, E1000_STATUS_LU);
    CHECK(e1000_link_status(&io, &value) == E1000_OK && value,
          "a part with LU set reads link up");
    fixture.fail_read = true;
    CHECK(e1000_link_status(&io, &value) == E1000_IO_ERROR,
          "a status read that failed is an I/O error");

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    CHECK(e1000_rx_armed(&io, &value) == E1000_OK && !value,
          "a receiver that was never enabled is not armed");
    store32(fixture.registers, E1000_REG_RCTL, E1000_RCTL_EN);
    CHECK(e1000_rx_armed(&io, &value) == E1000_OK && value,
          "the enable bit read back is the arming proof");
}

static void test_autoneg_restart(void)
{
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;
    const uint32_t expected =
        (E1000_MII_BMCR_SPEED1000 | E1000_MII_BMCR_FULL_DUPLEX |
         E1000_MII_BMCR_AUTOEN | E1000_MII_BMCR_ANRESTART) |
        ((uint32_t)E1000_MII_BMCR << E1000_MDIC_REG_SHIFT) |
        ((uint32_t)E1000_MDIC_PHY_ADDRESS << E1000_MDIC_PHY_SHIFT) |
        E1000_MDIC_OP_WRITE;

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    CHECK(e1000_restart_autoneg(NULL) == E1000_NULL,
          "a null register file is refused");
    CHECK(e1000_restart_autoneg(&io) == E1000_OK,
          "a completed PHY command is accepted");
    CHECK(fixture.mdic_writes == 1U && fixture.mdic_command == expected,
          "the restart names PHY 1, BMCR, a write, AUTOEN and ANRESTART");
    CHECK((fixture.mdic_command & E1000_MDIC_DATA_MASK) ==
              (E1000_MII_BMCR_SPEED1000 | E1000_MII_BMCR_FULL_DUPLEX |
               E1000_MII_BMCR_AUTOEN | E1000_MII_BMCR_ANRESTART),
          "the written value keeps auto-negotiation enabled");

    memset(&fixture, 0, sizeof(fixture));
    fixture.mdic_never_ready = true;
    fixture_mmio(&fixture, &io);
    CHECK(e1000_restart_autoneg(&io) == E1000_PHY_TIMEOUT,
          "a PHY that never completes is a bounded timeout");

    memset(&fixture, 0, sizeof(fixture));
    fixture.mdic_error = true;
    fixture_mmio(&fixture, &io);
    CHECK(e1000_restart_autoneg(&io) == E1000_IO_ERROR,
          "a PHY that refused the register is an I/O error");

    memset(&fixture, 0, sizeof(fixture));
    fixture_mmio(&fixture, &io);
    fixture.fail_write = true;
    CHECK(e1000_restart_autoneg(&io) == E1000_IO_ERROR,
          "a register write that failed is an I/O error");
}

static void test_region_and_rings(void)
{
    struct e1000_region_layout layout;
    struct e1000_ring ring;

    CHECK(e1000_region_calculate(NULL) == E1000_NULL,
          "a null layout is refused");
    CHECK(e1000_region_calculate(&layout) == E1000_OK,
          "the region layout is calculated");
    CHECK(layout.rx_descriptor_offset == 0U &&
              layout.tx_descriptor_offset >=
                  layout.rx_descriptor_offset +
                      TEST_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE &&
              layout.rx_buffer_offset >= layout.tx_descriptor_offset +
                      TEST_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE &&
              layout.tx_buffer_offset >=
                  layout.rx_buffer_offset +
                      TEST_DESCRIPTOR_COUNT * E1000_RX_BUFFER_CAPACITY &&
              layout.length >= layout.tx_buffer_offset +
                      TEST_DESCRIPTOR_COUNT * E1000_TX_BUFFER_CAPACITY,
          "the layout orders the descriptors and the buffers without overlap");
    CHECK((layout.rx_buffer_offset % 4096U) == 0U,
          "the frame buffers stay page aligned");
    CHECK((layout.tx_descriptor_offset % E1000_DESCRIPTOR_ALIGNMENT) == 0U,
          "the transmit descriptors are aligned as the part requires");

    e1000_ring_init(&ring, NULL, TEST_DESCRIPTOR_COUNT);
    CHECK(ring.descriptors == NULL && ring.capacity == 0U,
          "a ring without descriptors is empty, not a ring of null entries");
    e1000_ring_init(&ring, &layout, TEST_DESCRIPTOR_COUNT);
    CHECK(ring.capacity == TEST_DESCRIPTOR_COUNT && ring.index == 0U &&
              ring.in_flight == 0U && ring.submitted == 0U &&
              ring.reclaimed == 0U && ring.abandoned == 0U,
          "a ring starts empty and owns its counters");
    CHECK(e1000_ring_next(&ring, TEST_DESCRIPTOR_COUNT - 1U) == 0U,
          "the ring wraps at its capacity");
    CHECK(e1000_ring_next(&ring, 3U) == 4U, "the ring advances one entry");
    {
        struct e1000_ring empty;

        e1000_ring_init(&empty, &layout, 0U);
        CHECK(e1000_ring_next(&empty, 0U) == 0U,
              "a zero-capacity ring has no next entry");
    }
}

static void test_receive_path(void)
{
    struct mmio_fixture fixture;
    struct e1000_mmio_io io;
    struct e1000_rx_descriptor descriptors[TEST_DESCRIPTOR_COUNT];
    struct e1000_ring ring;
    uint16_t length = 0U;

    memset(&fixture, 0, sizeof(fixture));
    memset(descriptors, 0, sizeof(descriptors));
    e1000_ring_init(&ring, descriptors, TEST_DESCRIPTOR_COUNT);
    CHECK(e1000_rx_post(&ring, TEST_RX_PHYSICAL, TEST_RX_BUFFER_CAPACITY) ==
              E1000_OK &&
              descriptors[0].address == TEST_RX_PHYSICAL &&
              descriptors[1].address ==
                  TEST_RX_PHYSICAL + TEST_RX_BUFFER_CAPACITY &&
              descriptors[0].status == 0U && ring.index == 0U &&
              ring.in_flight == TEST_DESCRIPTOR_COUNT,
          "the whole ring is posted with consecutive buffers");
    CHECK(e1000_rx_post(&ring, TEST_RX_PHYSICAL, 7U) == E1000_INVALID_LENGTH,
          "a buffer size off the alignment is refused");
    CHECK(e1000_rx_post(&ring, TEST_RX_PHYSICAL + 1U, 2048U) == E1000_BAD_STATE,
          "an unaligned buffer address is refused");

    CHECK(e1000_rx_ready(&ring, 0U) == E1000_RX_NOT_READY,
          "a descriptor the device has not written is not ready");
    descriptors[0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
    descriptors[0].length = 64U;
    CHECK(e1000_rx_ready(&ring, 0U) == E1000_OK,
          "DD is the device's ownership hand-back");
    CHECK(e1000_rx_complete(&ring, 0U, TEST_RX_BUFFER_CAPACITY, &length) ==
              E1000_OK && length == 64U,
          "a completed frame reports its own length");
    descriptors[0].status = E1000_RX_STATUS_DD;
    CHECK(e1000_rx_complete(&ring, 0U, TEST_RX_BUFFER_CAPACITY, &length) ==
              E1000_RX_DROPPED && length == 0U,
          "DD without EOP is a fragment and is dropped, not published");
    descriptors[0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
    descriptors[0].errors = 0x01U;
    CHECK(e1000_rx_complete(&ring, 0U, TEST_RX_BUFFER_CAPACITY, &length) ==
              E1000_RX_DROPPED,
          "a descriptor the part marked with an error is dropped");
    descriptors[0].errors = 0U;
    descriptors[0].length = (uint16_t)(TEST_RX_BUFFER_CAPACITY + 16U);
    CHECK(e1000_rx_complete(&ring, 0U, TEST_RX_BUFFER_CAPACITY, &length) ==
              E1000_INVALID_LENGTH,
          "a length beyond the posted buffer is refused");
    CHECK(e1000_rx_complete(&ring, TEST_DESCRIPTOR_COUNT,
                            TEST_RX_BUFFER_CAPACITY, &length) ==
              E1000_BAD_STATE,
          "an index outside the ring is refused");

    descriptors[0].status = E1000_RX_STATUS_DD | E1000_RX_STATUS_EOP;
    CHECK(e1000_rx_repost(&ring, 0U, TEST_RX_PHYSICAL + 4096U) == E1000_OK &&
              descriptors[0].status == 0U &&
              descriptors[0].address == TEST_RX_PHYSICAL + 4096U,
          "a reposted descriptor is handed back to the device clean");
    (void)fixture;
    (void)io;
}

static void test_transmit_path(void)
{
    struct e1000_tx_descriptor descriptors[TEST_DESCRIPTOR_COUNT];
    struct e1000_ring ring;
    uint32_t reclaimed = 0U;

    memset(descriptors, 0, sizeof(descriptors));
    e1000_ring_init(&ring, descriptors, TEST_DESCRIPTOR_COUNT);

    CHECK(e1000_tx_frame_length(0U) == 0U &&
              e1000_tx_frame_length(E1000_FRAME_MAXIMUM + 1U) == 0U,
          "a zero or oversized frame has no length");
    CHECK(e1000_tx_frame_length(40U) == E1000_FRAME_MINIMUM,
          "a short frame is padded to the Ethernet minimum");
    CHECK(e1000_tx_frame_length(100U) == 100U,
          "a real frame keeps its own length");

    CHECK(e1000_tx_program(&ring, TEST_TX_PHYSICAL, 100U,
                           TEST_TX_BUFFER_CAPACITY) == E1000_OK &&
              descriptors[0].address == TEST_TX_PHYSICAL &&
              descriptors[0].length == 100U &&
              descriptors[0].command ==
                  (E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS |
                   E1000_TX_CMD_RS) &&
              ring.index == 1U && ring.in_flight == 1U &&
              ring.submitted == 1U,
          "one descriptor carries one whole frame");
    CHECK(e1000_tx_program(&ring, TEST_TX_PHYSICAL, 40U,
                           TEST_TX_BUFFER_CAPACITY) == E1000_INVALID_LENGTH,
          "a frame below the minimum is refused by the descriptor path");
    CHECK(e1000_tx_program(&ring, 0U, 100U, TEST_TX_BUFFER_CAPACITY) ==
              E1000_UNSUPPORTED_DMA_ADDRESS,
          "a device address of zero is refused");

    CHECK(e1000_tx_reclaim(&ring, &reclaimed) == E1000_RING_EMPTY &&
              reclaimed == 0U,
          "a descriptor the device has not completed is not reclaimed");
    descriptors[0].status = E1000_TX_STATUS_DD;
    CHECK(e1000_tx_reclaim(&ring, &reclaimed) == E1000_OK &&
              reclaimed == 1U && ring.in_flight == 0U && ring.reclaimed == 1U,
          "DD is the completion that frees the descriptor");

    {
        uint32_t index;

        for (index = 0U; index < TEST_DESCRIPTOR_COUNT; ++index) {
            (void)e1000_tx_program(&ring, TEST_TX_PHYSICAL + index * 2048U,
                                   100U, TEST_TX_BUFFER_CAPACITY);
        }
        CHECK(e1000_tx_program(&ring, TEST_TX_PHYSICAL, 100U,
                               TEST_TX_BUFFER_CAPACITY) == E1000_RING_FULL,
              "a full ring refuses a frame instead of overwriting one");
        CHECK(e1000_tx_rewind(&ring) == E1000_OK && ring.index == 0U &&
                  ring.in_flight == 0U &&
                  ring.abandoned == TEST_DESCRIPTOR_COUNT &&
                  descriptors[0].address == 0U &&
                  descriptors[0].command == 0U,
              "a rewind hands every descriptor back and counts the lost ones");
    }
}

int main(void)
{
    test_identity();
    test_memory_bar();
    test_mac();
    test_reset_and_interrupts();
    test_bring_up();
    test_link_and_arming();
    test_autoneg_restart();
    test_region_and_rings();
    test_receive_path();
    test_transmit_path();

    if (failures != 0) {
        fprintf(stderr, "e1000 host tests: %d FAILED\n", failures);
        return 1;
    }
    printf("e1000 host tests: ALL OK\n");
    return 0;
}

#endif /* HFOC_E1000_HOST_TEST */
