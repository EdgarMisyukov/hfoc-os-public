#include "e1000.h"

#include <string.h>

/*
 * ITEM-494: the Intel 8254x ("e1000") driver core.  See e1000.h for the
 * register map and the order this file programs it in, and rtl8168.c for the
 * shape both vendor drivers share: a caller-owned mmio vtable, descriptor
 * rings the driver reasons about as values, and a bring-up that never assumes
 * a write stuck.
 */

static bool e1000_read32(struct e1000_mmio_io *io, uint32_t offset,
                         uint32_t *value)
{
    return io != NULL && io->read32 != NULL &&
           io->read32(io->context, offset, value);
}

static bool e1000_write32(struct e1000_mmio_io *io, uint32_t offset,
                          uint32_t value)
{
    return io != NULL && io->write32 != NULL &&
           io->write32(io->context, offset, value);
}

bool e1000_pci_identity_supported(const struct pci_device_header *header)
{
    if (header == NULL || !header->present) {
        return false;
    }
    /*
     * The class and the subclass are part of the claim: a function that does
     * not present itself as an Ethernet controller is not this driver's part,
     * whatever its ids say.  A machine with a different Intel network part
     * (an e1000e, an igb) must read UNSUPPORTED rather than be driven by a
     * register map that does not fit it.
     */
    return header->vendor_id == E1000_VENDOR_ID &&
           header->device_id == E1000_DEVICE_ID_82540EM &&
           header->base_class == E1000_PCI_CLASS_NETWORK &&
           header->subclass == E1000_PCI_SUBCLASS_ETHERNET;
}

enum e1000_status e1000_memory_bar(const struct pci_device_header *header,
                                   const struct pci_bar **bar)
{
    uint32_t index;

    if (header == NULL || bar == NULL) {
        return E1000_NULL;
    }
    *bar = NULL;
    /*
     * The 8254x register file is BAR0 and is at least 64 KiB; the map used
     * here ends at RAH (0x5404), so a BAR smaller than that is not this part's
     * register file and is refused instead of being mapped and read past.
     */
    for (index = 0U; index < PCI_BAR_COUNT; ++index) {
        const struct pci_bar *candidate = &header->bars[index];

        if (!candidate->present || candidate->io_space) {
            continue;
        }
        if (candidate->length < E1000_MIN_MMIO_LENGTH) {
            continue;
        }
        *bar = candidate;
        return E1000_OK;
    }
    return E1000_MISSING_MEMORY_BAR;
}

bool e1000_mac_valid(const uint8_t mac[E1000_MAC_LENGTH])
{
    size_t index;
    bool any = false;
    bool all_ones = true;

    if (mac == NULL) {
        return false;
    }
    for (index = 0U; index < E1000_MAC_LENGTH; ++index) {
        any = any || mac[index] != 0U;
        all_ones = all_ones && mac[index] == 0xFFU;
    }
    if (!any || all_ones) {
        return false;
    }
    /* A multicast or broadcast station address is not a station address. */
    if ((mac[0] & 0x01U) != 0U) {
        return false;
    }
    return true;
}

enum e1000_status e1000_mac_read(struct e1000_mmio_io *io,
                                 uint8_t mac[E1000_MAC_LENGTH])
{
    uint32_t low = 0U;
    uint32_t high = 0U;

    if (io == NULL || mac == NULL) {
        return E1000_NULL;
    }
    memset(mac, 0, E1000_MAC_LENGTH);
    /*
     * RAL/RAH are the receive address registers the part itself filters on;
     * reading the MAC from them proves the hardware's own view of the address
     * rather than trusting a value copied from somewhere else.  RAH bits 0-15
     * are the upper two bytes; the address-valid bit and the rest are not part
     * of the address.
     */
    if (!e1000_read32(io, E1000_REG_RAL, &low) ||
        !e1000_read32(io, E1000_REG_RAH, &high)) {
        return E1000_IO_ERROR;
    }
    mac[0] = (uint8_t)(low & 0xFFU);
    mac[1] = (uint8_t)((low >> 8U) & 0xFFU);
    mac[2] = (uint8_t)((low >> 16U) & 0xFFU);
    mac[3] = (uint8_t)((low >> 24U) & 0xFFU);
    mac[4] = (uint8_t)(high & 0xFFU);
    mac[5] = (uint8_t)((high >> 8U) & 0xFFU);
    if (!e1000_mac_valid(mac)) {
        memset(mac, 0, E1000_MAC_LENGTH);
        return E1000_INVALID_MAC;
    }
    return E1000_OK;
}

bool e1000_dma_address_supported(uint64_t physical)
{
    /*
     * The legacy descriptors carry a 64-bit address, but this driver programs
     * no high-DMA enable and the product's allocator hands out addresses below
     * 4 GiB; an address the part would have to truncate is refused here rather
     * than discovered by a silent corruption later.
     */
    return physical != 0U && physical < E1000_DMA_ADDRESS_LIMIT;
}

enum e1000_status e1000_interrupts_mask(struct e1000_mmio_io *io)
{
    uint32_t pending = 0U;

    if (io == NULL) {
        return E1000_NULL;
    }
    /*
     * This driver is poll driven from the ordinary-boot event loop and IRQ
     * routing is out of scope, so the part must never assert its interrupt
     * line: mask every cause, acknowledge whatever is pending.
     */
    if (!e1000_write32(io, E1000_REG_IMC, UINT32_C(0xFFFFFFFF))) {
        return E1000_IO_ERROR;
    }
    if (!e1000_write32(io, E1000_REG_IMS, 0U)) {
        return E1000_IO_ERROR;
    }
    /* ICR is read-to-clear; reading it retires any latched cause. */
    if (!e1000_read32(io, E1000_REG_ICR, &pending)) {
        return E1000_IO_ERROR;
    }
    return E1000_OK;
}

enum e1000_status e1000_reset(struct e1000_mmio_io *io)
{
    uint32_t attempts;
    uint32_t control = 0U;

    if (io == NULL) {
        return E1000_NULL;
    }
    /* CTRL.RST, then wait for the part to clear the bit itself. */
    if (!e1000_write32(io, E1000_REG_CTRL, E1000_CTRL_RST)) {
        return E1000_IO_ERROR;
    }
    for (attempts = 0U; attempts < E1000_RESET_POLL_LIMIT; ++attempts) {
        if (!e1000_read32(io, E1000_REG_CTRL, &control)) {
            return E1000_IO_ERROR;
        }
        if ((control & E1000_CTRL_RST) == 0U) {
            return E1000_OK;
        }
    }
    return E1000_RESET_TIMEOUT;
}

static enum e1000_status e1000_zero_multicast_table(struct e1000_mmio_io *io)
{
    uint32_t index;

    for (index = 0U; index < E1000_MTA_BYTES / 4U; ++index) {
        if (!e1000_write32(io, E1000_REG_MTA + index * 4U, 0U)) {
            return E1000_IO_ERROR;
        }
    }
    return E1000_OK;
}

static enum e1000_status e1000_program_ring(
    struct e1000_mmio_io *io,
    uint32_t low_register,
    uint32_t high_register,
    uint32_t length_register,
    uint32_t head_register,
    uint32_t tail_register,
    uint64_t descriptor_physical,
    uint32_t descriptor_bytes,
    uint32_t tail)
{
    if (!e1000_dma_address_supported(descriptor_physical) ||
        (descriptor_physical % E1000_DESCRIPTOR_ALIGNMENT) != 0U) {
        return E1000_UNSUPPORTED_DMA_ADDRESS;
    }
    if (descriptor_bytes == 0U ||
        (descriptor_bytes % E1000_DESCRIPTOR_SIZE) != 0U) {
        return E1000_INVALID_LENGTH;
    }
    if (!e1000_write32(io, low_register,
                       (uint32_t)(descriptor_physical & 0xFFFFFFFFU)) ||
        !e1000_write32(io, high_register,
                       (uint32_t)(descriptor_physical >> 32U)) ||
        !e1000_write32(io, length_register, descriptor_bytes) ||
        !e1000_write32(io, head_register, 0U) ||
        !e1000_write32(io, tail_register, tail)) {
        return E1000_IO_ERROR;
    }
    return E1000_OK;
}

enum e1000_status e1000_bring_up(struct e1000_mmio_io *io,
                                 const struct e1000_bring_up *bring_up)
{
    const uint32_t rx_bytes =
        E1000_RX_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE;
    const uint32_t tx_bytes =
        E1000_TX_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE;
    uint32_t control = 0U;
    uint32_t receive_control = 0U;
    enum e1000_status status;

    if (io == NULL || bring_up == NULL) {
        return E1000_NULL;
    }
    if (bring_up->rx_buffer_capacity == 0U ||
        bring_up->rx_buffer_capacity > E1000_RX_BUFFER_CAPACITY) {
        return E1000_INVALID_LENGTH;
    }
    status = e1000_zero_multicast_table(io);
    if (status != E1000_OK) {
        return status;
    }
    /*
     * The rings are programmed before the receiver is enabled: RDT points at
     * the last descriptor the driver has handed over, so the hardware sees a
     * full ring the moment RCTL.EN goes up.
     */
    status = e1000_program_ring(
        io, E1000_REG_RDBAL, E1000_REG_RDBAH, E1000_REG_RDLEN,
        E1000_REG_RDH, E1000_REG_RDT, bring_up->rx_descriptor_physical,
        rx_bytes, E1000_RX_DESCRIPTOR_COUNT - 1U);
    if (status != E1000_OK) {
        return status;
    }
    status = e1000_program_ring(
        io, E1000_REG_TDBAL, E1000_REG_TDBAH, E1000_REG_TDLEN,
        E1000_REG_TDH, E1000_REG_TDT, bring_up->tx_descriptor_physical,
        tx_bytes, 0U);
    if (status != E1000_OK) {
        return status;
    }
    /* Set link up once; the PHY owns the link from here on. */
    if (!e1000_read32(io, E1000_REG_CTRL, &control)) {
        return E1000_IO_ERROR;
    }
    if (!e1000_write32(io, E1000_REG_CTRL, control | E1000_CTRL_SLU)) {
        return E1000_IO_ERROR;
    }
    if (!e1000_write32(io, E1000_REG_TCTL,
                       E1000_TCTL_EN | E1000_TCTL_PSP | E1000_TCTL_CT |
                           E1000_TCTL_COLD | E1000_TCTL_RTLC) ||
        !e1000_write32(io, E1000_REG_TIPG, E1000_TIPG_VALUE)) {
        return E1000_IO_ERROR;
    }
    receive_control = E1000_RCTL_EN | E1000_RCTL_BAM | E1000_RCTL_SECRC |
                      E1000_RCTL_BSIZE_2048;
    if (!e1000_write32(io, E1000_REG_RCTL, receive_control)) {
        return E1000_IO_ERROR;
    }
    /*
     * A receiver that did not take the enable bit is not a working receive
     * path, and the caller publishes the refusal with its own name instead of
     * a ready device with no frames.
     */
    {
        bool armed = false;

        status = e1000_rx_armed(io, &armed);
        if (status != E1000_OK) {
            return status;
        }
        if (!armed) {
            return E1000_ENABLE_FAILED;
        }
    }
    return E1000_OK;
}

enum e1000_status e1000_link_status(struct e1000_mmio_io *io, bool *link_up)
{
    uint32_t status = 0U;

    if (io == NULL || link_up == NULL) {
        return E1000_NULL;
    }
    *link_up = false;
    if (!e1000_read32(io, E1000_REG_STATUS, &status)) {
        return E1000_IO_ERROR;
    }
    *link_up = (status & E1000_STATUS_LU) != 0U;
    return E1000_OK;
}

enum e1000_status e1000_restart_autoneg(struct e1000_mmio_io *io)
{
    const uint32_t command =
        (E1000_MII_BMCR_SPEED1000 | E1000_MII_BMCR_FULL_DUPLEX |
         E1000_MII_BMCR_AUTOEN | E1000_MII_BMCR_ANRESTART) |
        ((uint32_t)E1000_MII_BMCR << E1000_MDIC_REG_SHIFT) |
        ((uint32_t)E1000_MDIC_PHY_ADDRESS << E1000_MDIC_PHY_SHIFT) |
        E1000_MDIC_OP_WRITE;
    uint32_t attempts;

    if (io == NULL) {
        return E1000_NULL;
    }
    /*
     * The 8254x owns the link through its PHY: CTRL.SLU alone is not enough
     * when the link partner appears after the device reset, because the link
     * bit is set by the completed negotiation and not by the driver.  Writing
     * BMCR with ANRESTART (and AUTOEN still set) starts that negotiation; the
     * part reports completion through MDIC.READY.
     */
    if (!e1000_write32(io, E1000_REG_MDIC, command)) {
        return E1000_IO_ERROR;
    }
    for (attempts = 0U; attempts < E1000_MDIC_POLL_LIMIT; ++attempts) {
        uint32_t status = 0U;

        if (!e1000_read32(io, E1000_REG_MDIC, &status)) {
            return E1000_IO_ERROR;
        }
        if ((status & E1000_MDIC_READY) != 0U) {
            return (status & E1000_MDIC_ERROR) != 0U ? E1000_IO_ERROR
                                                     : E1000_OK;
        }
    }
    return E1000_PHY_TIMEOUT;
}

enum e1000_status e1000_rx_armed(struct e1000_mmio_io *io, bool *armed)
{
    uint32_t receive_control = 0U;

    if (io == NULL || armed == NULL) {
        return E1000_NULL;
    }
    *armed = false;
    if (!e1000_read32(io, E1000_REG_RCTL, &receive_control)) {
        return E1000_IO_ERROR;
    }
    *armed = (receive_control & E1000_RCTL_EN) != 0U;
    return E1000_OK;
}

void e1000_ring_init(struct e1000_ring *ring, void *descriptors,
                     uint32_t capacity)
{
    if (ring == NULL) {
        return;
    }
    ring->descriptors = descriptors;
    ring->capacity = descriptors != NULL ? capacity : 0U;
    ring->index = 0U;
    ring->in_flight = 0U;
    ring->submitted = 0U;
    ring->reclaimed = 0U;
    ring->abandoned = 0U;
}

enum e1000_status e1000_region_calculate(struct e1000_region_layout *layout)
{
    struct e1000_region_layout draft;
    uint32_t rx_descriptor_length;
    uint32_t tx_descriptor_length;
    uint32_t rx_buffer_length;
    uint32_t tx_buffer_length;
    uint32_t offset;

    if (layout == NULL) {
        return E1000_NULL;
    }
    if ((E1000_RX_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE) %
            E1000_DESCRIPTOR_ALIGNMENT != 0U ||
        E1000_TX_BUFFER_CAPACITY < E1000_FRAME_MAXIMUM) {
        return E1000_CAPACITY;
    }
    rx_descriptor_length =
        E1000_RX_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE;
    tx_descriptor_length =
        E1000_TX_DESCRIPTOR_COUNT * E1000_DESCRIPTOR_SIZE;
    rx_buffer_length = E1000_RX_DESCRIPTOR_COUNT * E1000_RX_BUFFER_CAPACITY;
    tx_buffer_length = E1000_TX_DESCRIPTOR_COUNT * E1000_TX_BUFFER_CAPACITY;
    memset(&draft, 0, sizeof(draft));
    draft.rx_descriptor_offset = 0U;
    draft.tx_descriptor_offset =
        (rx_descriptor_length + E1000_DESCRIPTOR_ALIGNMENT - 1U) &
        ~(E1000_DESCRIPTOR_ALIGNMENT - 1U);
    offset = draft.tx_descriptor_offset + tx_descriptor_length;
    /* Keep the frame buffers page aligned so the adapter can slice them. */
    offset = (offset + 4095U) & ~4095U;
    draft.rx_buffer_offset = offset;
    offset += rx_buffer_length;
    draft.tx_buffer_offset = offset;
    offset += tx_buffer_length;
    draft.length = offset;
    *layout = draft;
    return E1000_OK;
}

uint32_t e1000_ring_next(const struct e1000_ring *ring, uint32_t index)
{
    if (ring == NULL || ring->capacity == 0U) {
        return 0U;
    }
    return (index + 1U) % ring->capacity;
}

enum e1000_status e1000_rx_post(struct e1000_ring *ring, uint64_t physical,
                                uint32_t buffer_capacity)
{
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL) {
        return E1000_NULL;
    }
    if (buffer_capacity == 0U ||
        buffer_capacity > E1000_RX_BUFFER_CAPACITY ||
        (buffer_capacity % E1000_RX_BUFFER_ALIGNMENT) != 0U) {
        return E1000_INVALID_LENGTH;
    }
    if (!e1000_dma_address_supported(physical) ||
        (physical % E1000_RX_BUFFER_ALIGNMENT) != 0U) {
        return E1000_BAD_STATE;
    }
    if (ring->capacity == 0U) {
        return E1000_BAD_STATE;
    }
    /*
     * The caller owns the region layout and hands over its base; each
     * descriptor gets one buffer, in ring order, with the status cleared so
     * the device's completion is the only thing that can set DD.
     */
    for (index = 0U; index < ring->capacity; ++index) {
        struct e1000_rx_descriptor *descriptor =
            &((struct e1000_rx_descriptor *)ring->descriptors)[index];

        descriptor->length = 0U;
        descriptor->checksum = 0U;
        descriptor->errors = 0U;
        descriptor->special = 0U;
        descriptor->address =
            physical + (uint64_t)index * buffer_capacity;
    }
    e1000_barrier();
    for (index = 0U; index < ring->capacity; ++index) {
        struct e1000_rx_descriptor *descriptor =
            &((struct e1000_rx_descriptor *)ring->descriptors)[index];

        descriptor->status = 0U;
    }
    ring->index = 0U;
    ring->in_flight = ring->capacity;
    return E1000_OK;
}

enum e1000_status e1000_rx_ready(const struct e1000_ring *ring,
                                 uint32_t index)
{
    const struct e1000_rx_descriptor *descriptor;

    if (ring == NULL || ring->descriptors == NULL) {
        return E1000_NULL;
    }
    if (ring->capacity == 0U || index >= ring->capacity) {
        return E1000_BAD_STATE;
    }
    descriptor =
        &((const struct e1000_rx_descriptor *)ring->descriptors)[index];
    if ((descriptor->status & E1000_RX_STATUS_DD) == 0U) {
        return E1000_RX_NOT_READY;
    }
    return E1000_OK;
}

enum e1000_status e1000_rx_complete(const struct e1000_ring *ring,
                                    uint32_t index, uint32_t buffer_capacity,
                                    uint16_t *length)
{
    const struct e1000_rx_descriptor *descriptor;

    if (ring == NULL || ring->descriptors == NULL || length == NULL) {
        return E1000_NULL;
    }
    *length = 0U;
    if (ring->capacity == 0U || index >= ring->capacity) {
        return E1000_BAD_STATE;
    }
    descriptor =
        &((const struct e1000_rx_descriptor *)ring->descriptors)[index];
    if ((descriptor->status & E1000_RX_STATUS_DD) == 0U) {
        return E1000_RX_NOT_READY;
    }
    if ((descriptor->status & E1000_RX_STATUS_EOP) == 0U ||
        descriptor->errors != 0U) {
        /*
         * A frame the device itself marked as bad - a fragment, a CRC error, a
         * runt - is a dropped frame with its own name, not a frame handed to
         * the network stack.
         */
        return E1000_RX_DROPPED;
    }
    if (descriptor->length == 0U ||
        descriptor->length > buffer_capacity) {
        return E1000_INVALID_LENGTH;
    }
    *length = descriptor->length;
    return E1000_OK;
}

enum e1000_status e1000_rx_repost(struct e1000_ring *ring, uint32_t index,
                                  uint64_t physical)
{
    struct e1000_rx_descriptor *descriptor;

    if (ring == NULL || ring->descriptors == NULL) {
        return E1000_NULL;
    }
    if (ring->capacity == 0U || index >= ring->capacity) {
        return E1000_BAD_STATE;
    }
    if (!e1000_dma_address_supported(physical) ||
        (physical % E1000_RX_BUFFER_ALIGNMENT) != 0U) {
        return E1000_BAD_STATE;
    }
    descriptor = &((struct e1000_rx_descriptor *)ring->descriptors)[index];
    descriptor->length = 0U;
    descriptor->checksum = 0U;
    descriptor->errors = 0U;
    descriptor->special = 0U;
    descriptor->address = physical;
    e1000_barrier();
    descriptor->status = 0U;
    return E1000_OK;
}

uint32_t e1000_tx_frame_length(size_t length)
{
    /*
     * The descriptor carries the real frame length; a frame below the Ethernet
     * minimum is padded by the caller before it is mapped, exactly as the
     * reference driver pads to ETH_ZLEN.  The part's TCTL.PSP pads too, and the
     * descriptor length is what the hardware is told to send.
     */
    if (length == 0U || length > E1000_FRAME_MAXIMUM) {
        return 0U;
    }
    if (length < E1000_FRAME_MINIMUM) {
        return E1000_FRAME_MINIMUM;
    }
    return (uint32_t)length;
}

enum e1000_status e1000_tx_program(struct e1000_ring *ring, uint64_t physical,
                                   size_t length, uint32_t frame_capacity)
{
    struct e1000_tx_descriptor *descriptor;
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL) {
        return E1000_NULL;
    }
    if (ring->capacity == 0U) {
        return E1000_BAD_STATE;
    }
    if (length < E1000_FRAME_MINIMUM || length > E1000_FRAME_MAXIMUM ||
        length > frame_capacity) {
        return E1000_INVALID_LENGTH;
    }
    if (!e1000_dma_address_supported(physical)) {
        return E1000_UNSUPPORTED_DMA_ADDRESS;
    }
    if (ring->in_flight >= ring->capacity) {
        return E1000_RING_FULL;
    }
    index = ring->index % ring->capacity;
    descriptor = &((struct e1000_tx_descriptor *)ring->descriptors)[index];
    /*
     * One descriptor carries the whole frame: end of packet, insert the
     * checksum, report status.  The command byte is written last, after every
     * field it describes, so the device never sees a half-written descriptor.
     */
    descriptor->address = physical;
    descriptor->length = (uint16_t)length;
    descriptor->cso = 0U;
    descriptor->css = 0U;
    descriptor->special = 0U;
    descriptor->status = 0U;
    e1000_barrier();
    descriptor->command = E1000_TX_CMD_EOP | E1000_TX_CMD_IFCS |
                          E1000_TX_CMD_RS;
    ring->index = e1000_ring_next(ring, ring->index);
    ++ring->in_flight;
    ++ring->submitted;
    return E1000_OK;
}

enum e1000_status e1000_tx_reclaim(struct e1000_ring *ring,
                                   uint32_t *reclaimed)
{
    uint32_t count = 0U;

    if (ring == NULL || ring->descriptors == NULL || reclaimed == NULL) {
        return E1000_NULL;
    }
    *reclaimed = 0U;
    if (ring->capacity == 0U) {
        return E1000_BAD_STATE;
    }
    while (ring->in_flight > 0U) {
        const uint32_t index =
            (ring->index + ring->capacity - ring->in_flight) %
            ring->capacity;
        const struct e1000_tx_descriptor *descriptor =
            &((const struct e1000_tx_descriptor *)ring->descriptors)[index];

        /* The device sets DD when it has read the descriptor back. */
        if ((descriptor->status & E1000_TX_STATUS_DD) == 0U) {
            break;
        }
        e1000_barrier();
        --ring->in_flight;
        ++count;
    }
    *reclaimed = count;
    ring->reclaimed += count;
    return count > 0U ? E1000_OK : E1000_RING_EMPTY;
}

enum e1000_status e1000_tx_rewind(struct e1000_ring *ring)
{
    uint32_t index;

    if (ring == NULL || ring->descriptors == NULL) {
        return E1000_NULL;
    }
    if (ring->capacity == 0U) {
        return E1000_BAD_STATE;
    }
    /*
     * The caller has reset the part, so its descriptor cursor is back at the
     * ring base and nothing it left behind is live.  Every descriptor is
     * written back as CPU-owned in the reverse of the submission order.
     */
    for (index = 0U; index < ring->capacity; ++index) {
        struct e1000_tx_descriptor *descriptor =
            &((struct e1000_tx_descriptor *)ring->descriptors)[index];

        descriptor->address = 0U;
        descriptor->length = 0U;
        descriptor->cso = 0U;
        descriptor->css = 0U;
        descriptor->special = 0U;
        e1000_barrier();
        descriptor->command = 0U;
        descriptor->status = 0U;
    }
    e1000_barrier();
    ring->abandoned += ring->in_flight;
    ring->index = 0U;
    ring->in_flight = 0U;
    return E1000_OK;
}

const char *e1000_status_string(enum e1000_status status)
{
    switch (status) {
    case E1000_OK:
        return "OK";
    case E1000_NULL:
        return "NULL";
    case E1000_IO_ERROR:
        return "IO_ERROR";
    case E1000_UNSUPPORTED_DEVICE:
        return "UNSUPPORTED_DEVICE";
    case E1000_MISSING_MEMORY_BAR:
        return "MISSING_MEMORY_BAR";
    case E1000_BAD_BAR:
        return "BAD_BAR";
    case E1000_BAD_STATE:
        return "BAD_STATE";
    case E1000_INVALID_MAC:
        return "INVALID_MAC";
    case E1000_RESET_TIMEOUT:
        return "RESET_TIMEOUT";
    case E1000_INVALID_LENGTH:
        return "INVALID_LENGTH";
    case E1000_RING_FULL:
        return "RING_FULL";
    case E1000_RING_EMPTY:
        return "RING_EMPTY";
    case E1000_RX_NOT_READY:
        return "RX_NOT_READY";
    case E1000_RX_DROPPED:
        return "RX_DROPPED";
    case E1000_UNSUPPORTED_DMA_ADDRESS:
        return "UNSUPPORTED_DMA_ADDRESS";
    case E1000_CAPACITY:
        return "CAPACITY";
    case E1000_ENABLE_FAILED:
        return "ENABLE_FAILED";
    case E1000_PHY_TIMEOUT:
        return "PHY_TIMEOUT";
    default:
        return "UNKNOWN";
    }
}
