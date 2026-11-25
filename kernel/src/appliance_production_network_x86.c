#ifdef HFOC_PRODUCTION_NETWORK

#include "appliance_production_network_boot.h"
#include "appliance_production_network_phy_record.h"
#include "appliance_production_network_rx_accounting.h"
#include "appliance_production_network_wire.h"

#include "console.h"
#include "e1000.h"
#include "link_state.h"
#include "network_backend.h"
#include "nic_backends.h"
#include "nic_discovery.h"
#include "nic_registry.h"
#include "pci.h"
#include "pci_dma.h"
#include "pmm.h"
#include "serial.h"
#include "rtl8168.h"
#include "virtio_net_feature_profile.h"
#include "virtio_net_interrupt.h"
#include "virtio_net_queue_composition.h"
#include "virtio_net_rx.h"
#include "virtio_net_tx.h"
#include "virtio_pci_capability.h"
#include "virtio_pci_device_status.h"
#include "virtio_pci_feature.h"
#include "virtio_pci_identity.h"
#include "virtio_queue_layout.h"
#include "vmm.h"

#include "arch/x86_64/timer.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define APN_PCI_CONFIG_ADDRESS 0xCF8U
#define APN_PCI_CONFIG_DATA 0xCFCU
#define APN_PCI_CONFIG_ENABLE UINT32_C(0x80000000)
#define APN_PCI_VENDOR_OFFSET 0x00U
#define APN_PCI_BAR_COUNT 6U
#define APN_PCI_MAX_DEVICE 32U
#define APN_PCI_MAX_FUNCTION 8U

#define APN_COMMON_DEVICE_FEATURE_SELECT 0x00U
#define APN_COMMON_DEVICE_FEATURE 0x04U
#define APN_COMMON_DRIVER_FEATURE_SELECT 0x08U
#define APN_COMMON_DRIVER_FEATURE 0x0CU
#define APN_COMMON_DEVICE_STATUS 0x14U
#define APN_COMMON_QUEUE_SELECT 0x16U
#define APN_COMMON_QUEUE_SIZE 0x18U
#define APN_COMMON_QUEUE_ENABLE 0x1CU
#define APN_COMMON_QUEUE_NOTIFY_OFF 0x1EU
#define APN_COMMON_QUEUE_DESC 0x20U
#define APN_COMMON_QUEUE_DRIVER 0x28U
#define APN_COMMON_QUEUE_DEVICE 0x30U

#define APN_QUEUE_SIZE 16U
#define APN_DMA_CAPACITY 16U
#define APN_RX_COUNT 8U
#define APN_TX_COUNT 8U
#define APN_RX_BUFFER_LENGTH 1536U
#define APN_TX_HEADER_STRIDE 16U
#define APN_TX_FRAME_LENGTH 1536U
#define APN_PAYLOAD_PAGES 8U
#define APN_QUEUE_REGION_LENGTH 4096U
#define APN_DMA_ALIGNMENT 16U
#define APN_MIN_FRAME_LENGTH 60U
#define APN_MAX_FRAME_LENGTH 1518U

#define APN_VIRTQ_DESC_F_NEXT 1U
#define APN_VIRTQ_DESC_F_WRITE 2U

#define APN_RX_BASE_OFFSET 0U
#define APN_TX_HEADER_BASE_OFFSET \
    (APN_RX_COUNT * APN_RX_BUFFER_LENGTH)
#define APN_TX_FRAME_BASE_OFFSET \
    (APN_TX_HEADER_BASE_OFFSET + APN_TX_COUNT * APN_TX_HEADER_STRIDE)
#define APN_PAYLOAD_LENGTH (APN_PAYLOAD_PAGES * 4096ULL)

/*
 * ITEM-389 production backend selection and Realtek RTL8168 bring-up.
 *
 * The ordinary boot probes the virtio-net function first, exactly as before,
 * so the QEMU proofs keep the path they were written against; when no virtio
 * function is present it probes the on-board Realtek NIC the acceptance host
 * carries.  Exactly one backend binds the production composition, and the
 * boot record names which one is live.
 */
#define APN_RTL_REGION_PAGES 16U
#define APN_RTL_LINK_ATTEMPTS_MAX 120U
#define APN_RTL_LINK_RETRY_MS 500U
/*
 * ITEM-583: once the bounded fast retries are spent, the link is still
 * polled, just slowly.  A cable plugged in ten minutes after boot, or a switch
 * that finishes negotiation late, must recover without a reboot.
 */
#define APN_RTL_LINK_IDLE_RETRY_MS 5000U
/*
 * ITEM-428: a transmit ring that has read full for this long is stalled, and a
 * stalled ring is reset and rebuilt - at most APN_RTL_TX_RESTART_MAX times per
 * boot, each at least APN_RTL_TX_RESTART_BACKOFF_MS after the previous one.
 */
#define APN_RTL_TX_STALL_MS 1000U
#define APN_RTL_TX_RESTART_BACKOFF_MS 2000U
#define APN_RTL_TX_RESTART_MAX 4U
/*
 * ITEM-494: the second physical vendor has the same bounded shape.  The
 * register window is the 8254x BAR0 (the map used ends at RAH, 0x5404, and
 * `e1000_memory_bar()` refuses anything smaller than the 0x6000 window), the
 * DMA region holds both legacy rings and their buffers, and a transmit ring
 * that has read full for the same second is recovered by the same bounded
 * restart.
 */
#define APN_E1000_REGISTER_LENGTH E1000_MIN_MMIO_LENGTH
#define APN_E1000_REGION_PAGES 16U
#define APN_E1000_LINK_ATTEMPTS_MAX 120U
#define APN_E1000_LINK_RETRY_MS 500U
#define APN_E1000_TX_STALL_MS 1000U
#define APN_E1000_TX_RESTART_BACKOFF_MS 2000U
#define APN_E1000_TX_RESTART_MAX 4U
/*
 * How many link attempts pass between two PHY auto-negotiation restarts.  The
 * restart is spaced well past the negotiation itself, so it cannot keep
 * re-arming the completion it is waiting for: the initial bind restarts once,
 * and a machine whose peer never appears retries every eight attempts.
 */
#define APN_E1000_AUTONEG_ATTEMPTS 8U
/*
 * ITEM-467: how many received frames one poll pass may feed the stack.
 *
 * The pass used to drain the ring to empty, so a peer that sent faster than
 * the loop could classify kept the loop inside the receive path and the
 * keyboard - serviced once at the top of the same turn - waited for every
 * queued frame.  The bound makes one turn's network work finite, so the next
 * turn reaches the input path within a fixed number of frames; the rest of
 * the ring is drained on the following turns.
 */
#define APN_RX_BUDGET_PER_PASS 16U

/*
 * ITEM-486 (round 24): the network service pass's own time budget.
 *
 * ITEM-467 bounded a pass by the number of frames it may feed the stack, which
 * fixes "how many" but not "how long": under a flood, sixteen frames each
 * spending tens of milliseconds in the stack still held the turn - and the
 * keyboard serviced at the top of the next turn - for hundreds of milliseconds
 * (`net_us` up to 750 439 in the 2026-09-26 QEMU proof).  A pass now stops at
 * `APN_SERVICE_BUDGET_US` as well, whichever bound comes first, and returns to
 * the main loop; the frames it did not take stay in the device ring and are
 * serviced on the following turns, so nothing dequeued is dropped.  The pass
 * always finishes the frame it started, so the bound is on the frames the pass
 * chooses, never on one frame's own processing.
 *
 * The deadline is opened at the start of a *receive pass*, not at the start of
 * the poll that contains it: a poll also runs the transmit path and the link
 * work before it reaches the ring, and a deadline opened before that work is
 * already spent when the first frame is offered.  In a slow (TCG) build that
 * starved the receive path completely - every pass broke before taking a
 * single frame, which the DHCP/DNS proof reported as "the receive path
 * accepted and refused no frame at all on a machine that answered DHCP".
 *
 * The time bound also may not shrink a pass below `APN_SERVICE_MIN_FRAMES`: a
 * bound expressed only in measured microseconds is not the same amount of work
 * in an emulated build as on the part, because the same frame costs tens of
 * times more microseconds there.  Without the floor the emulated path ran at
 * one frame per turn, which the DHCP/DNS proof caught a second time - its
 * boot-time DNS probe exhausted its attempt budget (`reason=DNS_PROBE_TIMEOUT
 * hold=1`), and the hold that later renewal drill is written around became
 * `hold=2`, so the marker `reason=REBIND_TIMEOUT hold=1` never appeared.  The
 * floor makes progress structural rather than a property of the host's speed;
 * on the part sixteen frames cost well under the bound, so it never applies
 * there.
 */
#define APN_SERVICE_BUDGET_US 2000U
#define APN_SERVICE_MIN_FRAMES 4U

/*
 * PCI configuration space (offset 0x04 is the command register).
 */
#define APN_PCI_COMMAND_OFFSET 0x04U
#define APN_PCI_COMMAND_MEMORY_SPACE UINT32_C(0x0002)
#define APN_PCI_COMMAND_BUS_MASTER UINT32_C(0x0004)
#define APN_RTL_REGISTER_LENGTH 4096U
#define APN_PCI_MAX_BUS 256U
#define APN_PCI_MULTIFUNCTION 0x80

enum apn_backend {
    APN_BACKEND_NONE = 0,
    APN_BACKEND_VIRTIO = 1,
    APN_BACKEND_RTL8168 = 2,
    APN_BACKEND_E1000 = 3,
};

struct apn_rtl_state {
    bool probed;
    bool supported;
    bool bring_up_done;
    bool bind_attempted;
    bool bound;
    /*
     * ITEM-399: the driver read this part's XID from TxConfig, so the boot
     * record can state which silicon it drove even when the part turns out to
     * be outside the XID table (family UNKNOWN) or the bring-up fails.
     */
    bool chip_identified;
    const char *reason;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t revision;
    uint64_t mmio_physical;
    /*
     * ITEM-428: the PCI command register after the driver enabled memory space
     * and bus mastering.  Published, because it is the one byte that decides
     * whether the part can fetch a descriptor at all, and it was invisible on
     * the 2026-09-16 hardware run.
     */
    uint32_t pci_command;
    /*
     * ITEM-393: two bases that are not interchangeable.
     *
     * `region` is the mapped MMIO register window of the PCI BAR. It is
     * reachable only through the apn_rtl register accessors, whose callers
     * stay inside APN_RTL_REGISTER_LENGTH, i.e. inside the pages apn_map_mmio()
     * actually mapped.
     *
     * `dma_ring` is the HHDM address of the ring allocation
     * pmm_alloc_contiguous_pages() returned and `region_physical` is its
     * physical counterpart: the descriptor rings, the receive buffers and the
     * transmit buffers all live inside that one allocation and may only be
     * addressed from these two. Addressing a frame buffer from the MMIO window
     * instead writes up to RTL8168_TX_BUFFER_STRIDE bytes per frame into PCI
     * address space past the single mapped register page - on the acceptance
     * host that faulted in memset on physical 0xFC000000, inside the machine's
     * PCI aperture.
     */
    uint64_t region_physical;
    volatile uint8_t *region;
    uint8_t *dma_ring;
    uint8_t mac[NETWORK_DEVICE_MAC_LENGTH];
    struct rtl8168_chip chip;
    struct rtl8168_mmio_io io;
    uint64_t delay_cycles_per_second;
    /* ITEM-583: the PHY state the MII path reads; invalid on parts whose PHY
       is not behind PHYAR (the later OCP family). */
    struct rtl8168_phy_state phy;
    /* ITEM-592: what the cold start did, stage by stage, for the record. */
    struct rtl8168_cold_boot_report boot;
    struct rtl8168_ring rx;
    struct rtl8168_ring tx;
    struct network_device_snapshot snapshot;
    struct network_backend_binding binding;
    uint32_t link_attempts;
    uint32_t next_attempt_ms;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t tx_frames;
    /*
     * ITEM-428: what the transmit path did when it stopped taking frames.  The
     * 2026-09-16 hardware run's boot log ended on a DHCP record with no way to
     * tell whether the machine never transmitted, transmitted and was never
     * answered, or was answered and ignored the answer; these counters are the
     * three answers.  `tx_ring_full` is a submission the ring refused because
     * the device had not handed a descriptor back, `tx_restarts` the bounded
     * recoveries that followed, `tx_restart_lost` the frames those recoveries
     * declared lost, and `rx_errors` the receive descriptors the device
     * delivered but the driver would not feed to the stack.
     */
    uint32_t tx_ring_full;
    uint32_t tx_restarts;
    uint32_t tx_restart_lost;
    uint32_t tx_restart_refused;
    uint32_t rx_errors;
    /*
     * ITEM-645: the receive-stop events the poll pass observed.
     * `rx_overflow` counts a new latch of RxFIFOOver/RxOverflow and
     * `rx_overflow_acks` the latches a drained pass then cleared.
     * `rx_overflow_latched` keeps one event from being counted again on every
     * pass while a busy ring keeps the bit set on purpose.
     */
    uint32_t rx_overflow;
    uint32_t rx_overflow_acks;
    bool rx_overflow_latched;
    /*
     * ITEM-435: the receiver's arming state as the part itself reported it
     * during the bring-up (`rtl8168_rx_armed()`), published so a log with an
     * all-zero receive line says whether the device ever held the ring.
     *
     * ITEM-439: and whether that question was answered at all.  A probe that
     * could not read the part is not the same state as a part that answered
     * "not armed", and neither of them is "ready": the bind used to assume the
     * receive path from the bring-up returning OK, so a receiver that never
     * held the ring could be published as READY and only show up as
     * `rx_ok=0 rx_err=0` on the wire.
     */
    bool rx_ready;
    bool rx_ready_known;
    uint32_t tx_stall_ms;
    uint32_t tx_restart_ms;
    bool tx_stall_seen;
    bool tx_stall_armed;
};

/*
 * ITEM-494: the second physical vendor's state.  It is deliberately the same
 * shape as the Realtek state above - one probe, one bring-up, one bound flag,
 * one set of frame counters, the same bounded transmit stall - because a vendor
 * is silicon and its driver, not a second architecture.  The differences are
 * the register accessors (the 8254x file is 32-bit only), the doorbell writes
 * the adapter owns (TDT/RDT are register writes the core never performs), and
 * the absence of a chip-identity field: the claim is one part, so the record
 * already names the silicon.
 */
struct apn_e1000_state {
    bool probed;
    bool supported;
    bool bring_up_done;
    bool bind_attempted;
    bool bound;
    const char *reason;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
    uint16_t vendor_id;
    uint16_t product_id;
    uint8_t revision;
    uint64_t mmio_physical;
    uint32_t pci_command;
    uint64_t region_physical;
    volatile uint8_t *region;
    uint8_t *dma_ring;
    uint8_t mac[NETWORK_DEVICE_MAC_LENGTH];
    struct e1000_mmio_io io;
    struct e1000_ring rx;
    struct e1000_ring tx;
    struct network_device_snapshot snapshot;
    struct network_backend_binding binding;
    uint32_t link_attempts;
    uint32_t next_attempt_ms;
    uint32_t rx_dropped;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t tx_frames;
    uint32_t tx_ring_full;
    uint32_t tx_restarts;
    uint32_t tx_restart_refused;
    uint32_t rx_errors;
    bool rx_ready;
    bool rx_ready_known;
    uint32_t tx_stall_ms;
    uint32_t tx_restart_ms;
    bool tx_stall_seen;
    bool tx_stall_armed;
};

struct apn_virtq_desc {
    volatile uint64_t addr;
    volatile uint32_t len;
    volatile uint16_t flags;
    volatile uint16_t next;
};

struct apn_queue_mem {
    uint64_t physical;
    volatile uint8_t *virtual_address;
    struct virtio_queue_layout_result layout;
    struct virtio_available_ring *available;
    struct virtio_used_ring *used;
    volatile struct apn_virtq_desc *descriptors;
};

/*
 * ITEM-492: the machine's network controllers, discovered before anything asks
 * a driver to bind one, and the registry of the drivers this image carries.
 *
 * The discovery is driver-agnostic on purpose: every PCI network controller is
 * described - the ones this image can drive, the ones nobody wrote a driver for
 * and the ones whose driver refused - so "the machine has a Realtek part" is a
 * result of matching, not an assumption the selection starts from.  Adding an
 * Intel, Broadcom or Marvell backend is a registration in this table plus its
 * own bring-up; nothing in the selection path mentions a vendor.
 */
static struct nic_device apn_nic_devices[NIC_DEVICES_MAX];
static size_t apn_nic_count;
static bool apn_nic_scanned;

struct apn_state {
    bool initialized;
    bool device_present;
    uint32_t stage;
    uint64_t hhdm_offset;
    enum apn_backend backend;
    uint16_t pci_vendor_id;
    uint16_t pci_product_id;
    uint8_t pci_revision;
    /*
     * ITEM-428: the PCI command register as it reads after the driver enabled
     * memory space and bus mastering - for the live backend, whichever it is -
     * and whether this driver ever read that register.  A value nobody read is
     * not a value: the boot record publishes nothing rather than a zero.
     */
    uint32_t virtio_pci_command;
    bool virtio_pci_command_valid;
    /* The virtio function completed its bind; a refused one did not. */
    bool virtio_bound;
    /* ITEM-428: why a virtio function was refused, when it was. */
    const char *virtio_reason;
    struct apn_rtl_state rtl;
    /* ITEM-494: the second physical vendor's state, the same shape. */
    struct apn_e1000_state e1000;
    volatile uint8_t *common_base;
    volatile uint8_t *notify_base;
    volatile uint8_t *isr_base;
    volatile uint8_t *device_base;
    uint32_t notify_multiplier;
    uint32_t queue_notify_offs[2];
    uint64_t rx_region_physical;
    uint64_t tx_region_physical;
    uint64_t payload_physical;
    volatile uint8_t *payload_virtual;
    struct apn_queue_mem queues[2];
    struct virtio_pci_device_status_controller device_status;
    struct virtio_net_feature_profile profile;
    struct virtio_net_queue_composition composition;
    struct virtio_net_rx rx;
    struct virtio_net_tx tx;
    struct virtio_net_interrupt interrupt;
    struct virtio_net_link_state link_state;
    struct appliance_production_network network;
    struct appliance_production_network_bind_inputs bind_inputs;
    uint64_t tx_header_physical[APN_TX_COUNT];
    volatile uint8_t *tx_header_virtual[APN_TX_COUNT];
    struct virtio_net_tx_header_descriptor tx_headers[APN_TX_COUNT];
    uint64_t tx_frame_physical[APN_TX_COUNT];
    volatile uint8_t *tx_frame_virtual[APN_TX_COUNT];
    uint32_t tx_frame_length[APN_TX_COUNT];
    bool tx_notify_pending;
    bool tx_stalled;
    /*
     * ITEM-428: the same frame-path counters the Realtek state keeps, so the
     * `[PRODUCTION-DHCP]` record carries one shape whichever backend is live
     * and the QEMU proof can require the fields on the virtio path.
     */
    uint32_t tx_frames;
    uint32_t tx_reclaimed;
    uint32_t tx_ring_full;
    uint32_t tx_dropped;
    uint32_t rx_frames;
    uint32_t rx_dropped;
    /*
     * ITEM-486 (round 24): the pass's own bounded budget.  `service_deadline_us`
     * is the microsecond instant the current pass must stop at (zero before the
     * cycle clock is calibrated, so only the frame count bounds the pass then);
     * `service_budget_hits` counts the passes the time bound cut short, which
     * is the evidence that the bound - not an idle ring - ended the pass.
     */
    uint64_t service_deadline_us;
    uint32_t service_budget_hits;
};

static struct apn_state apn;

static void apn_zero(void *destination, size_t length)
{
    memset(destination, 0, length);
}

static void apn_outl(uint16_t port, uint32_t value)
{
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}

static uint32_t apn_inl(uint16_t port)
{
    uint32_t value;

    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint32_t apn_pci_read32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset)
{
    const uint32_t address =
        APN_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    apn_outl(APN_PCI_CONFIG_ADDRESS, address);
    return apn_inl(APN_PCI_CONFIG_DATA);
}

static void apn_pci_write32(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    const uint32_t address =
        APN_PCI_CONFIG_ENABLE |
        ((uint32_t)bus << 16U) |
        ((uint32_t)device << 11U) |
        ((uint32_t)function << 8U) |
        ((uint32_t)offset & 0xFCU);

    apn_outl(APN_PCI_CONFIG_ADDRESS, address);
    apn_outl(APN_PCI_CONFIG_DATA, value);
}

static bool apn_pci_read(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t *value)
{
    (void)context;
    if (value == NULL || (offset % 4U) != 0U) {
        return false;
    }
    *value = apn_pci_read32(bus, device, function, offset);
    return true;
}

static bool apn_pci_write(
    void *context,
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t offset,
    uint32_t value)
{
    (void)context;
    if ((offset % 4U) != 0U) {
        return false;
    }
    apn_pci_write32(bus, device, function, offset, value);
    return true;
}

static void apn_pci_read_space(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint8_t *config)
{
    uint16_t offset;

    for (offset = 0U; offset < VIRTIO_PCI_CONFIG_SPACE_MAX;
         offset += 4U) {
        const uint32_t value = apn_pci_read32(
            bus, device, function, offset);

        config[offset] = (uint8_t)(value & 0xFFU);
        config[offset + 1U] = (uint8_t)((value >> 8U) & 0xFFU);
        config[offset + 2U] = (uint8_t)((value >> 16U) & 0xFFU);
        config[offset + 3U] = (uint8_t)((value >> 24U) & 0xFFU);
    }
}

/*
 * ITEM-428: enable the two PCI command bits a DMA-capable function needs.
 *
 * A driver that only reads MMIO can work with the command register exactly as
 * the firmware left it, and that is what hid this on the 2026-09-16 hardware
 * run: the RTL8168 answered every register read (`xid=0x541`, its MAC, a link
 * that read UP and a ChipCmd readback that confirmed TE|RE) while its bus
 * master enable bit was never set by this driver.  With bit 2 clear the part
 * may not fetch a single transmit descriptor or write a single received byte,
 * which looks exactly like a machine that "has no network": the transmit ring
 * fills after sixteen frames and no DHCP offer ever arrives.  Whether the
 * firmware happened to have set the bit is not something a driver may rely on,
 * so it is set here, read back, and published.
 *
 * Returns false when the function still does not have both bits after the
 * write, so the caller refuses the device with a named reason instead of
 * driving a part that cannot reach memory.
 */
static bool apn_pci_enable_bus_master(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint32_t *command_out)
{
    const uint32_t command = apn_pci_read32(
        bus, device, function, APN_PCI_COMMAND_OFFSET);
    const uint32_t enabled = command | APN_PCI_COMMAND_MEMORY_SPACE |
                             APN_PCI_COMMAND_BUS_MASTER;

    if (enabled != command) {
        apn_pci_write32(
            bus, device, function, APN_PCI_COMMAND_OFFSET, enabled);
    }
    {
        const uint32_t readback = apn_pci_read32(
            bus, device, function, APN_PCI_COMMAND_OFFSET);

        if (command_out != NULL) {
            *command_out = readback;
        }
        return (readback & APN_PCI_COMMAND_MEMORY_SPACE) != 0U &&
               (readback & APN_PCI_COMMAND_BUS_MASTER) != 0U;
    }
}

static uint32_t apn_mmio_read32(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint32_t *)(base + offset);
}

static uint16_t apn_mmio_read16(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint16_t *)(base + offset);
}

static uint8_t apn_mmio_read8(volatile uint8_t *base, uint32_t offset)
{
    return *(volatile uint8_t *)(base + offset);
}

static void apn_mmio_write32(
    volatile uint8_t *base,
    uint32_t offset,
    uint32_t value)
{
    *(volatile uint32_t *)(base + offset) = value;
}

static void apn_mmio_write16(
    volatile uint8_t *base,
    uint32_t offset,
    uint16_t value)
{
    *(volatile uint16_t *)(base + offset) = value;
}

static void apn_mmio_write8(
    volatile uint8_t *base,
    uint32_t offset,
    uint8_t value)
{
    *(volatile uint8_t *)(base + offset) = value;
}

static void apn_mmio_write64(
    volatile uint8_t *base,
    uint32_t offset,
    uint64_t value)
{
    *(volatile uint64_t *)(base + offset) = value;
}

static bool apn_map_mmio(uint64_t physical_base, uint64_t length)
{
    uint64_t page;
    const uint64_t page_count =
        (length + 4095U) / 4096U;

    if (physical_base == 0U || length == 0U ||
        physical_base % 4096U != 0U) {
        return false;
    }
    for (page = 0U; page < page_count; ++page) {
        volatile void *mapped = NULL;

        if (vmm_map_mmio_page(
                physical_base + page * 4096U,
                apn.hhdm_offset, &mapped) != VMM_OK) {
            return false;
        }
    }
    return true;
}

static uint8_t *apn_phys(uint64_t physical)
{
    return (uint8_t *)(uintptr_t)(apn.hhdm_offset + physical);
}

static bool apn_status_write(void *context, uint8_t value)
{
    (void)context;
    apn_mmio_write8(apn.common_base, APN_COMMON_DEVICE_STATUS, value);
    return true;
}

static bool apn_status_read(void *context, uint8_t *value)
{
    (void)context;
    if (value == NULL) {
        return false;
    }
    *value = apn_mmio_read8(apn.common_base, APN_COMMON_DEVICE_STATUS);
    return true;
}

static bool apn_feature_read_offered(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    if (value == NULL) {
        return false;
    }
    apn_mmio_write32(
        apn.common_base, APN_COMMON_DEVICE_FEATURE_SELECT, select);
    *value = apn_mmio_read32(apn.common_base, APN_COMMON_DEVICE_FEATURE);
    return true;
}

static bool apn_feature_write_accepted(
    void *context,
    uint32_t select,
    uint32_t value)
{
    (void)context;
    apn_mmio_write32(
        apn.common_base, APN_COMMON_DRIVER_FEATURE_SELECT, select);
    apn_mmio_write32(apn.common_base, APN_COMMON_DRIVER_FEATURE, value);
    return true;
}

static bool apn_feature_read_accepted(
    void *context,
    uint32_t select,
    uint32_t *value)
{
    (void)context;
    if (value == NULL) {
        return false;
    }
    apn_mmio_write32(
        apn.common_base, APN_COMMON_DRIVER_FEATURE_SELECT, select);
    *value = apn_mmio_read32(apn.common_base, APN_COMMON_DRIVER_FEATURE);
    return true;
}

static void apn_barrier_fence(void *context)
{
    (void)context;
    __asm__ volatile ("" : : : "memory");
}

static bool apn_zeroize(
    void *context,
    uint64_t physical_base,
    uint64_t length)
{
    (void)context;
    memset(apn_phys(physical_base), 0, (size_t)length);
    return true;
}

static bool apn_queue_reclaim(
    void *context,
    const struct virtio_used_element *element,
    const struct virtio_used_token *token)
{
    (void)context;
    (void)element;
    (void)token;
    return true;
}

static void apn_write_descriptor(
    struct apn_queue_mem *queue,
    uint32_t index,
    uint64_t address,
    uint32_t length,
    uint16_t flags,
    uint16_t next)
{
    volatile struct apn_virtq_desc *descriptor =
        &queue->descriptors[index];

    descriptor->addr = address;
    descriptor->len = length;
    descriptor->flags = flags;
    descriptor->next = next;
}

static bool apn_program_queue(uint32_t queue_index)
{
    struct apn_queue_mem *queue = &apn.queues[queue_index];
    uint16_t notify_off;

    apn_mmio_write16(
        apn.common_base, APN_COMMON_QUEUE_SELECT,
        (uint16_t)queue_index);
    if (apn_mmio_read16(
            apn.common_base, APN_COMMON_QUEUE_SELECT) !=
        (uint16_t)queue_index) {
        return false;
    }
    notify_off = apn_mmio_read16(
        apn.common_base, APN_COMMON_QUEUE_NOTIFY_OFF);
    apn_mmio_write16(
        apn.common_base, APN_COMMON_QUEUE_SIZE,
        (uint16_t)queue->layout.queue_size);
    if (apn_mmio_read16(
            apn.common_base, APN_COMMON_QUEUE_SIZE) !=
        (uint16_t)queue->layout.queue_size) {
        return false;
    }
    apn_mmio_write64(
        apn.common_base, APN_COMMON_QUEUE_DESC,
        queue->layout.region_base + queue->layout.descriptor.offset);
    apn_mmio_write64(
        apn.common_base, APN_COMMON_QUEUE_DRIVER,
        queue->layout.region_base + queue->layout.available.offset);
    apn_mmio_write64(
        apn.common_base, APN_COMMON_QUEUE_DEVICE,
        queue->layout.region_base + queue->layout.used.offset);
    apn.queue_notify_offs[queue_index] = notify_off;
    apn_mmio_write16(apn.common_base, APN_COMMON_QUEUE_ENABLE, 1U);
    return apn_mmio_read16(
               apn.common_base, APN_COMMON_QUEUE_ENABLE) == 1U;
}

static void apn_notify_queue(uint32_t queue_index, uint16_t value)
{
    volatile uint16_t *notify_address = (volatile uint16_t *)(uintptr_t)(
        apn.notify_base +
        apn.queue_notify_offs[queue_index] * apn.notify_multiplier);

    *notify_address = value;
}

/*
 * Program one transmit chain: a twelve-byte virtio-net header descriptor that
 * chains to the frame descriptor.  The frame descriptor carries the real frame
 * length and never the buffer capacity, because the device puts exactly the
 * programmed length on the wire: a fixed capacity length makes every short
 * frame leave padded (a bare ACK would leave as a 1518-byte frame) and the
 * peer reads trailing junk that the appliance never queued.  A length outside
 * the frame contract refuses the submission instead of advertising a frame the
 * device would pad or truncate.
 */
static bool apn_tx_notify(
    void *context,
    const struct virtio_net_tx_submission *submission)
{
    uint32_t slot;
    uint32_t frame_length;

    (void)context;
    if (submission == NULL ||
        submission->header_descriptor_index >= APN_QUEUE_SIZE ||
        submission->frame_descriptor_index >= APN_QUEUE_SIZE ||
        submission->slot_index >= APN_TX_COUNT) {
        return false;
    }
    slot = submission->slot_index;
    frame_length = apn.tx_frame_length[slot];
    if (frame_length < APN_MIN_FRAME_LENGTH ||
        frame_length > APN_MAX_FRAME_LENGTH) {
        return false;
    }
    apn_write_descriptor(
        &apn.queues[1], submission->header_descriptor_index,
        apn.tx_headers[slot].physical_base,
        apn.tx_headers[slot].length,
        APN_VIRTQ_DESC_F_NEXT,
        (uint16_t)submission->frame_descriptor_index);
    apn_write_descriptor(
        &apn.queues[1], submission->frame_descriptor_index,
        apn.tx_frame_physical[slot],
        frame_length,
        0U, 0U);
    apn.tx_notify_pending = true;
    return true;
}

static bool apn_link_read_config_status(
    void *context,
    uint32_t *status)
{
    (void)context;
    if (status == NULL) {
        return false;
    }
    *status = (uint32_t)apn_mmio_read16(apn.device_base, 6U);
    return true;
}

static bool apn_interrupt_read_status(void *context, uint8_t *status)
{
    (void)context;
    if (status == NULL) {
        return false;
    }
    *status = apn_mmio_read8(apn.isr_base, 0U);
    return true;
}

static bool apn_interrupt_has_rx_work(void *context)
{
    (void)context;
    return true;
}

static bool apn_interrupt_has_tx_work(void *context)
{
    (void)context;
    return true;
}

static void apn_interrupt_publish(
    void *context,
    const struct virtio_net_interrupt_work *work)
{
    (void)context;
    (void)work;
}

/* True when the registry entry is the backend with this name. */
static bool apn_nic_backend_is(const struct nic_backend *backend,
                               const char *name)
{
    return backend != NULL && backend->name != NULL && name != NULL &&
           strcmp(backend->name, name) == 0;
}

static bool apn_find_device(
    uint8_t *bus_out,
    uint8_t *device_out,
    uint8_t *function_out,
    uint16_t *vendor_out,
    uint16_t *product_out)
{
    uint8_t bus;
    uint8_t device;

    for (bus = 0U; bus < 1U; ++bus) {
        for (device = 0U; device < APN_PCI_MAX_DEVICE; ++device) {
            uint8_t function;

            for (function = 0U; function < APN_PCI_MAX_FUNCTION;
                 ++function) {
                const uint16_t vendor = (uint16_t)(
                    apn_pci_read32(
                        bus, device, function, APN_PCI_VENDOR_OFFSET) &
                    0xFFFFU);
                uint16_t product;

                if (vendor == 0xFFFFU) {
                    continue;
                }
                product = (uint16_t)(
                    apn_pci_read32(
                        bus, device, function, APN_PCI_VENDOR_OFFSET) >>
                    16U);
                /*
                 * ITEM-492: which parts this driver drives is the registry's
                 * answer, not a comparison written into the scan: a virtio
                 * function is accepted when the registry maps it to the
                 * virtio-net backend, and the ids above are only the legacy
                 * constants the rest of the driver still speaks in.
                 */
                if (apn_nic_backend_is(
                        nic_registry_match(vendor, product),
                        NIC_BACKEND_NAME_VIRTIO)) {
                    *bus_out = bus;
                    *device_out = device;
                    *function_out = function;
                    *vendor_out = vendor;
                    *product_out = product;
                    return true;
                }
            }
        }
    }
    return false;
}

static struct appliance_production_network *apn_network(void)
{
    return &apn.network;
}

/* ------------------------------------------------------------------------ *
 * ITEM-389 Realtek RTL8168 production backend.
 * ------------------------------------------------------------------------ */

static bool apn_rtl_transmit(
    void *context,
    const uint8_t *frame,
    size_t length);

/*
 * ITEM-492: the bind result of whichever physical path ran lands on the record
 * the machine's controller already has (definition below, beside the table it
 * updates).  ITEM-494 adds the address the driver read, so an AVAILABLE record
 * carries the part's own MAC and not `mac=NONE`.
 */
static void apn_nic_record_bind_result(bool bound, const char *reason,
                                       const uint8_t *mac);

static bool apn_rtl_read8(void *context, uint32_t offset, uint8_t *value)
{
    (void)context;
    if (value == NULL || apn.rtl.region == NULL ||
        offset >= APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    *value = apn.rtl.region[offset];
    return true;
}

static bool apn_rtl_read16(void *context, uint32_t offset, uint16_t *value)
{
    (void)context;
    if (value == NULL || apn.rtl.region == NULL ||
        offset + 2U > APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    *value = *(volatile uint16_t *)(apn.rtl.region + offset);
    return true;
}

static bool apn_rtl_read32(void *context, uint32_t offset, uint32_t *value)
{
    (void)context;
    if (value == NULL || apn.rtl.region == NULL ||
        offset + 4U > APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    *value = *(volatile uint32_t *)(apn.rtl.region + offset);
    return true;
}

static bool apn_rtl_write8(void *context, uint32_t offset, uint8_t value)
{
    (void)context;
    if (apn.rtl.region == NULL || offset >= APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    apn.rtl.region[offset] = value;
    return true;
}

static bool apn_rtl_write16(void *context, uint32_t offset, uint16_t value)
{
    (void)context;
    if (apn.rtl.region == NULL ||
        offset + 2U > APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    *(volatile uint16_t *)(apn.rtl.region + offset) = value;
    return true;
}

static bool apn_rtl_write32(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    if (apn.rtl.region == NULL ||
        offset + 4U > APN_RTL_REGISTER_LENGTH) {
        return false;
    }
    *(volatile uint32_t *)(apn.rtl.region + offset) = value;
    return true;
}

/* Network init precedes console clock calibration. Measure the local TSC
 * against the existing IRQ-independent millisecond wait BEFORE PHYAR use.
 * The checked wait reports a stopped reference and restores timer setup.
 * Keep the checked NIC measurement rather than replacing it with an unchecked
 * calibration from another subsystem. No interrupt policy changes. */
#define APN_RTL_DELAY_CALIBRATION_MS 100U

static uint64_t apn_rtl_delay_rate(void)
{
    uint64_t start;
    uint64_t elapsed;

    if (apn.rtl.delay_cycles_per_second != 0U) {
        return apn.rtl.delay_cycles_per_second;
    }
    start = timer_cycles();
    if (!timer_delay_ms_checked(APN_RTL_DELAY_CALIBRATION_MS)) {
        return 0U;
    }
    elapsed = timer_cycles() - start;
    if (elapsed == 0U || elapsed > UINT64_MAX / 10U) {
        return 0U;
    }
    apn.rtl.delay_cycles_per_second = elapsed * 10U;
    return apn.rtl.delay_cycles_per_second;
}

static void apn_rtl_delay_us(void *context, uint32_t microseconds)
{
    uint64_t start;
    uint64_t rate;
    uint64_t cycles;

    (void)context;
    if (microseconds == 0U) {
        return;
    }
    rate = apn_rtl_delay_rate();
    /* Preparation refuses the PHY before installing this callback if no rate
     * could be measured. This guard also prevents a division on a bad seam. */
    if (rate == 0U) {
        return;
    }
    cycles = (rate * microseconds + UINT64_C(999999)) / UINT64_C(1000000);
    start = timer_cycles();
    while (timer_cycles() - start < cycles) {
    }
}

/*
 * ITEM-583: the live link comes from BMCR/BMSR when the part's PHY is behind
 * PHYAR.  A refresh failure falls back to the PHYstatus byte, so a part this
 * driver does not have an MII path for keeps its old behaviour instead of
 * being reported as down.
 */
static bool apn_rtl_link_read(void *context, bool *link_up)
{
    (void)context;
    if (link_up == NULL) {
        return false;
    }
    if (rtl8168_phy_mii_supported(&apn.rtl.chip)) {
        if (rtl8168_phy_refresh(&apn.rtl.io, &apn.rtl.phy) == RTL8168_OK) {
            *link_up = apn.rtl.phy.link_up;
            return true;
        }
    }
    return rtl8168_link_status(&apn.rtl.io, link_up) == RTL8168_OK;
}

/*
 * One bounded scan for the on-board Realtek function.  Function zero is read
 * once per device and the remaining functions are only visited when the
 * header declares a multi-function device, so the scan costs one config read
 * per empty slot instead of eight.
 */
static bool apn_rtl_find(void)
{
    uint32_t bus;
    uint8_t device;

    for (bus = 0U; bus < APN_PCI_MAX_BUS; ++bus) {
        for (device = 0U; device < APN_PCI_MAX_DEVICE; ++device) {
            const uint32_t first = apn_pci_read32(
                (uint8_t)bus, device, 0U, APN_PCI_VENDOR_OFFSET);
            const uint16_t first_vendor = (uint16_t)(first & 0xFFFFU);
            uint8_t last_function = 0U;
            uint8_t function;

            if (first_vendor == 0xFFFFU) {
                continue;
            }
            if ((apn_pci_read32((uint8_t)bus, device, 0U, 0x0CU) &
                 ((uint32_t)APN_PCI_MULTIFUNCTION << 16U)) != 0U) {
                last_function = (uint8_t)(APN_PCI_MAX_FUNCTION - 1U);
            }
            for (function = 0U; function <= last_function; ++function) {
                const uint32_t identity = apn_pci_read32(
                    (uint8_t)bus, device, function,
                    APN_PCI_VENDOR_OFFSET);
                const uint16_t vendor = (uint16_t)(identity & 0xFFFFU);
                const uint16_t product = (uint16_t)(identity >> 16U);
                struct pci_config_io config_io;
                struct pci_device_header header;
                struct pci_location location;

                if (vendor != RTL8168_VENDOR_ID) {
                    continue;
                }
                config_io.read32 = apn_pci_read;
                config_io.write32 = apn_pci_write;
                config_io.context = &apn;
                location.bus = (uint8_t)bus;
                location.device = device;
                location.function = function;
                if (pci_device_read(&config_io, &location, &header) !=
                    PCI_OK) {
                    continue;
                }
                if (header.base_class != RTL8168_PCI_CLASS_NETWORK ||
                    header.subclass != RTL8168_PCI_SUBCLASS_ETHERNET) {
                    continue;
                }
                apn.rtl.bus = (uint8_t)bus;
                apn.rtl.device = device;
                apn.rtl.function = function;
                apn.rtl.vendor_id = vendor;
                apn.rtl.product_id = product;
                apn.rtl.revision = header.revision;
                apn.pci_vendor_id = vendor;
                apn.pci_product_id = product;
                apn.pci_revision = header.revision;
                apn.rtl.probed = true;
                if (rtl8168_pci_identity_supported(&header)) {
                    const struct pci_bar *bar = NULL;

                    if (rtl8168_memory_bar(&header, &bar) != RTL8168_OK ||
                        bar == NULL) {
                        apn.rtl.reason = "NO_MEMORY_BAR";
                        return true;
                    }
                    /*
                     * ITEM-428: memory space and bus mastering before the
                     * first register write and before the part is asked to
                     * touch a descriptor.  The command register the firmware
                     * left behind is published either way, so a hardware run
                     * states whether the appliance had to enable it.
                     */
                    if (!apn_pci_enable_bus_master(
                            (uint8_t)bus, device, function,
                            &apn.rtl.pci_command)) {
                        apn.rtl.reason = "PCI_BUS_MASTER_REFUSED";
                        return true;
                    }
                    if (!apn_map_mmio(bar->base,
                                      bar->length <
                                              APN_RTL_REGISTER_LENGTH
                                          ? bar->length
                                          : APN_RTL_REGISTER_LENGTH)) {
                        apn.rtl.reason = "MMIO_MAP_FAILED";
                        return true;
                    }
                    apn.rtl.mmio_physical = bar->base;
                    apn.rtl.region = apn_phys(bar->base);
                    return true;
                }
                apn.rtl.reason = "DEVICE_ID";
                return true;
            }
        }
    }
    return false;
}

static bool apn_rtl_prepare(void)
{
    if (!apn.rtl.probed || apn.rtl.region == NULL) {
        return false;
    }
    apn.rtl.io.context = &apn;
    apn.rtl.io.read8 = apn_rtl_read8;
    apn.rtl.io.read16 = apn_rtl_read16;
    apn.rtl.io.read32 = apn_rtl_read32;
    apn.rtl.io.write8 = apn_rtl_write8;
    apn.rtl.io.write16 = apn_rtl_write16;
    apn.rtl.io.write32 = apn_rtl_write32;
    /* No PHY command is permitted before its clock has been prepared. */
    apn.rtl.io.delay_us = NULL;

    {
        const enum rtl8168_status identify_status =
            rtl8168_chip_identify(&apn.rtl.io, &apn.rtl.chip);

        /*
         * ITEM-399: either outcome means the XID register was read, so the
         * part's silicon is known to the driver and is published; only a
         * failed register read leaves the identity unknown.
         */
        apn.rtl.chip_identified =
            identify_status == RTL8168_OK ||
            identify_status == RTL8168_UNSUPPORTED_DEVICE;
        if (identify_status != RTL8168_OK) {
            /*
             * ITEM-389: an XID outside the identified table is refused
             * instead of being driven with family values the driver never
             * proved.
             */
            apn.rtl.reason =
                identify_status == RTL8168_UNSUPPORTED_DEVICE
                    ? "CHIP_UNKNOWN"
                    : "CHIP_ID_READ_FAILED";
            return false;
        }
    }
    if (rtl8168_phy_mii_supported(&apn.rtl.chip) &&
        apn_rtl_delay_rate() == 0U) {
        apn.rtl.reason = "PHY_CLOCK_UNAVAILABLE";
        return false;
    }
    apn.rtl.io.delay_us = apn_rtl_delay_us;
    if (rtl8168_mac_read(&apn.rtl.io, &apn.rtl.chip, apn.rtl.mac) !=
        RTL8168_OK) {
        apn.rtl.reason = "MAC_UNAVAILABLE";
        return false;
    }
    {
        struct rtl8168_region_layout layout;
        struct pci_dma_buffer region_buffer;
        uint32_t pages;

        if (rtl8168_region_calculate(&layout) != RTL8168_OK) {
            apn.rtl.reason = "REGION_LAYOUT_FAILED";
            return false;
        }
        pages = (layout.length + 4095U) / 4096U;
        if (pages == 0U || pages > APN_RTL_REGION_PAGES) {
            apn.rtl.reason = "REGION_TOO_LARGE";
            return false;
        }
        apn.rtl.region_physical =
            pmm_alloc_contiguous_pages(pages);
        if (apn.rtl.region_physical == 0U) {
            apn.rtl.reason = "DMA_ALLOCATION_FAILED";
            return false;
        }
        if (pci_dma_buffer_publish(
                &region_buffer, apn.rtl.region_physical,
                (size_t)(pages * 4096U),
                PCI_DMA_DIRECTION_BIDIRECTIONAL) != PCI_DMA_OK) {
            apn.rtl.reason = "DMA_REGION_INVALID";
            return false;
        }
        if (!rtl8168_dma_address_supported(apn.rtl.region_physical) ||
            !rtl8168_dma_address_supported(
                apn.rtl.region_physical + region_buffer.length - 1U)) {
            apn.rtl.reason = "DMA_ABOVE_4G";
            return false;
        }
        {
            uint8_t *base = apn_phys(apn.rtl.region_physical);

            /*
             * ITEM-393: publish the DMA region's HHDM base once, so every
             * later frame-buffer address is derived from the allocation and
             * never from the mapped MMIO window.
             */
            apn.rtl.dma_ring = base;
            apn_zero(base, (size_t)region_buffer.length);
            rtl8168_ring_init(
                &apn.rtl.rx,
                (struct rtl8168_descriptor *)(uintptr_t)(
                    base + layout.rx_descriptor_offset),
                RTL8168_RX_DESCRIPTOR_COUNT);
            rtl8168_ring_init(
                &apn.rtl.tx,
                (struct rtl8168_descriptor *)(uintptr_t)(
                    base + layout.tx_descriptor_offset),
                RTL8168_TX_DESCRIPTOR_COUNT);
            if (rtl8168_rx_post(
                    &apn.rtl.rx,
                    apn.rtl.region_physical + layout.rx_buffer_offset,
                    RTL8168_RX_BUFFER_CAPACITY) != RTL8168_OK) {
                apn.rtl.reason = "RX_RING_POST_FAILED";
                return false;
            }
        }
    }
    apn.rtl.supported = true;
    return true;
}

static bool apn_rtl_bring_up(void)
{
    struct rtl8168_region_layout layout;
    struct rtl8168_bring_up configuration;

    if (!apn.rtl.supported) {
        return false;
    }
    if (rtl8168_region_calculate(&layout) != RTL8168_OK) {
        apn.rtl.reason = "REGION_LAYOUT_FAILED";
        return false;
    }
    memset(&configuration, 0, sizeof(configuration));
    configuration.rx_descriptor_physical =
        apn.rtl.region_physical + layout.rx_descriptor_offset;
    configuration.tx_descriptor_physical =
        apn.rtl.region_physical + layout.tx_descriptor_offset;
    configuration.rx_buffer_capacity = RTL8168_RX_BUFFER_CAPACITY;

    /*
     * ITEM-592: the cold start is one sequenced run for the confirmed profile:
     * MAC reset, EVL PHY resume/tuning/soft reset, EPHY and clock, MAC start,
     * the standard PHY stage
     * (page 0, the 10/100 and 1000BASE-T advertisements, auto-negotiation) and
     * a separate observation of completed negotiation. Each stage and the first
     * failure are kept for the record; a profile without the EPHY/clock stages
     * has them SKIPPED by name rather than silently omitted.
     */
    memset(&apn.rtl.phy, 0, sizeof(apn.rtl.phy));
    /*
     * ITEM-589: the access method is recorded whether or not it is used, so
     * the `[NET-PHY]` record can say "skipped: this part's PHY is on the OCP
     * side" instead of only "link=DOWN".  The driver keeps the configuration
     * result and the first error on the same state.
     */
    apn.rtl.phy.access = rtl8168_phy_access_method(&apn.rtl.chip);
    {
        const enum rtl8168_status boot_status = rtl8168_cold_boot(
            &apn.rtl.io, &apn.rtl.chip, &configuration, apn.rtl.mac,
            &apn.rtl.phy, &apn.rtl.boot);

        if (boot_status != RTL8168_OK) {
            apn.rtl.reason =
                boot_status == RTL8168_ENABLE_FAILED
                    ? "ENABLE_FAILED"
                    : "BRING_UP_FAILED";
            return false;
        }
    }
    apn.rtl.phy.access = rtl8168_phy_access_method(&apn.rtl.chip);
    if (rtl8168_transmit_poll(&apn.rtl.io) != RTL8168_OK) {
        apn.rtl.reason = "TRANSMIT_POLL_FAILED";
        return false;
    }
    /*
     * ITEM-435: ask the part whether it really holds the receive ring.  The
     * answer is published, not enforced: a device that refused the descriptor
     * base must still be readable as "present, receiver not armed" instead of
     * looking exactly like a device that receives nothing.  The read happens
     * after the enable, so it observes the state the part is in while the main
     * loop polls it.
     */
    {
        struct rtl8168_region_layout layout;
        bool armed = false;

        /*
         * ITEM-439: the answer describes *this* bring-up.  A restart resets the
         * part, so an earlier "armed" must not survive a probe that then fails
         * or answers "not armed" - the published `rx_arm=` and the bind both
         * read the state the last probe left.
         */
        apn.rtl.rx_ready = false;
        apn.rtl.rx_ready_known = false;
        if (rtl8168_region_calculate(&layout) == RTL8168_OK &&
            rtl8168_rx_armed(
                &apn.rtl.io,
                apn.rtl.region_physical + layout.rx_descriptor_offset,
                &armed) == RTL8168_OK) {
            apn.rtl.rx_ready = armed;
            apn.rtl.rx_ready_known = true;
        }
    }
    apn.rtl.bring_up_done = true;
    return true;
}

static void apn_rtl_publish_snapshot(void)
{
    memset(&apn.rtl.snapshot, 0, sizeof(apn.rtl.snapshot));
    apn.rtl.snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    apn.rtl.snapshot.device_id = NETWORK_BACKEND_DEVICE_ID;
    apn.rtl.snapshot.vendor_id = apn.rtl.vendor_id;
    apn.rtl.snapshot.product_id = apn.rtl.product_id;
    apn.rtl.snapshot.bus_address =
        ((uint32_t)apn.rtl.bus << 8U) |
        ((uint32_t)apn.rtl.device << 3U) |
        apn.rtl.function;
    memcpy(apn.rtl.snapshot.mac_address, apn.rtl.mac,
           NETWORK_DEVICE_MAC_LENGTH);
    apn.rtl.snapshot.capability_mask = NETWORK_BACKEND_CAPABILITY_MASK;
}

/*
 * The PHY negotiates after the reset the driver issues, so the first bind
 * attempt can legitimately find the link down.  The link record is published
 * with the state the device reports at that moment; the poll retries the bind
 * while the link is still down, and stops retrying after a bounded number of
 * attempts so a cable-less appliance settles in a published DEGRADED state
 * instead of retrying forever.
 */
static bool apn_rtl_bind(void)
{
    struct network_backend_readiness readiness;
    struct network_link_state_record link_record;
    struct appliance_production_network_backend_inputs inputs;
    struct network_backend_link_io io;
    enum network_backend_status bind_status;
    bool link_up = false;

    apn.rtl.bound = false;
    apn_rtl_publish_snapshot();
    if (!apn_rtl_link_read(NULL, &link_up)) {
        apn.rtl.reason = "LINK_READ_FAILED";
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }

    memset(&link_record, 0, sizeof(link_record));
    link_record.version = NETWORK_LINK_STATE_VERSION;
    link_record.device_id = NETWORK_BACKEND_DEVICE_ID;
    link_record.state = link_up ? NETWORK_LINK_UP : NETWORK_LINK_DOWN;
    link_record.generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;

    /*
     * ITEM-439: the receive path is ready exactly when the part said it holds
     * the ring, and the bind refuses - with a name - instead of assuming it.
     *
     * ITEM-435 opened the extended family's RXDV gate and published the
     * arming read-back; this is the half that acts on it.  The 2026-09-17
     * bisect's `rx_ok=0 rx_err=0 offer=0` came from a medium that predates the
     * gate fix, but the shape it left behind - a receiver that never held the
     * ring, published as READY because the bring-up returned OK - is the one
     * this closes: a machine whose receive path is not armed does not bind as
     * ready, and its record carries `RX_NOT_ARMED` or `RX_ARM_PROBE_FAILED`
     * instead of an all-zero receive line with no explanation.
     *
     * The rings are programmed and the transmitter is enabled by the time this
     * runs; the link record and the device's own link signal still carry the
     * link state, and the composition turns a down link into DEGRADED.
     */
    if (!apn.rtl.rx_ready_known) {
        apn.rtl.reason = "RX_ARM_PROBE_FAILED";
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }
    if (!apn.rtl.rx_ready) {
        apn.rtl.reason = "RX_NOT_ARMED";
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }
    readiness.receive_path_ready = true;
    readiness.transmit_path_ready = true;
    io.read_link_up = apn_rtl_link_read;
    io.context = &apn;

    memset(&apn.rtl.binding, 0, sizeof(apn.rtl.binding));
    bind_status = network_backend_bind(
        &apn.rtl.binding, &apn.rtl.snapshot, &link_record, &readiness,
        &io, APPLIANCE_PRODUCTION_NETWORK_GENERATION);
    if (bind_status != NETWORK_BACKEND_OK) {
        apn.rtl.reason = network_backend_status_string(bind_status);
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }

    memset(&inputs, 0, sizeof(inputs));
    inputs.binding = &apn.rtl.binding;
    inputs.ipv4_config = NULL;
    inputs.output.transmit = apn_rtl_transmit;
    inputs.output.context = &apn;
    apn.stage = 40U;
    if (appliance_production_network_bind_backend(
            &apn.network, &inputs) != APPLIANCE_PRODUCTION_NETWORK_OK &&
        !appliance_production_network_bound(&apn.network)) {
        apn.rtl.reason = "BIND_FAILED";
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }
    apn.rtl.bind_attempted = true;
    if (!link_up) {
        apn.rtl.reason = "LINK_DOWN";
        apn_nic_record_bind_result(false, apn.rtl.reason, apn.rtl.mac);
        return false;
    }
    apn.rtl.bound = true;
    apn.rtl.reason = NULL;
    apn.stage = 100U;
    apn_nic_record_bind_result(true, NULL, apn.rtl.mac);
    return true;
}

static bool apn_rtl_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    struct rtl8168_region_layout layout;
    uint32_t padded;
    uint32_t slot;
    uint8_t *buffer;
    uint64_t buffer_physical;
    uint32_t reclaimed = 0U;

    (void)context;
    if (frame == NULL || length == 0U) {
        return false;
    }
    if (apn.rtl.dma_ring == NULL) {
        /* No bound DMA region: refuse the frame instead of addressing the
         * MMIO window or the HHDM base. */
        ++apn.rtl.tx_dropped;
        return false;
    }
    padded = rtl8168_tx_frame_length(length);
    if (padded == 0U) {
        ++apn.rtl.tx_dropped;
        return false;
    }
    if (rtl8168_region_calculate(&layout) != RTL8168_OK) {
        return false;
    }
    (void)rtl8168_tx_reclaim(&apn.rtl.tx, &reclaimed);
    if (apn.rtl.tx.in_flight >= RTL8168_TX_DESCRIPTOR_COUNT) {
        /*
         * ITEM-428: the ring is full because the device has not handed a
         * descriptor back.  The frame is refused - never queued, never
         * overwritten - and the refusal is counted, so a machine that only
         * ever reports "no lease" says whether it ran out of descriptors.
         */
        ++apn.rtl.tx_ring_full;
        ++apn.rtl.tx_dropped;
        apn.rtl.tx_stall_armed = true;
        return false;
    }
    slot = apn.rtl.tx.index % RTL8168_TX_DESCRIPTOR_COUNT;
    /*
     * ITEM-393: the transmit buffer lives in the DMA ring allocation, exactly
     * like the descriptor the device reads below. Deriving it from the mapped
     * MMIO window would memset PCI address space past the register page.
     */
    buffer = apn.rtl.dma_ring + layout.tx_buffer_offset +
             (size_t)slot * RTL8168_TX_BUFFER_STRIDE;
    /*
     * The buffer is zeroed so the padding of a short frame is deterministic,
     * and the descriptor carries the padded frame length - never the buffer
     * capacity, which would put a padded 1518-byte frame on the wire for a
     * 64-byte acknowledgement.
     */
    apn_zero(buffer, RTL8168_TX_BUFFER_STRIDE);
    memcpy(buffer, frame, length);
    buffer_physical = apn.rtl.region_physical + layout.tx_buffer_offset +
                      (uint64_t)slot * RTL8168_TX_BUFFER_STRIDE;
    if (rtl8168_tx_program(&apn.rtl.tx, buffer_physical, padded,
                           RTL8168_TX_BUFFER_CAPACITY) != RTL8168_OK) {
        ++apn.rtl.tx_dropped;
        return false;
    }
    if (rtl8168_transmit_poll(&apn.rtl.io) != RTL8168_OK) {
        ++apn.rtl.tx_dropped;
        return false;
    }
    ++apn.rtl.tx_frames;
    return true;
}

/*
 * ITEM-428: the bounded transmit restart.
 *
 * A device that stops clearing its descriptors' OWN bit leaves the ring full
 * forever, and every later frame is refused - the 2026-09-16 hardware boot
 * stopped ten seconds in with exactly that shape of record.  The recovery is
 * the reference driver's `rtl8169_tx_timeout()`, in the reference driver's
 * order: stop the part, hand the transmit ring back to the CPU, re-arm the
 * receive ring, and only then bring the part up again and ring the doorbell.
 * Everything here is bounded: a restart only happens after the ring has read
 * full for APN_RTL_TX_STALL_MS, no more than APN_RTL_TX_RESTART_MAX attempts
 * happen in one boot, and two attempts are at least
 * APN_RTL_TX_RESTART_BACKOFF_MS apart.  The attempt is counted before the part
 * is touched, so a device that answers the reset with nothing is retried four
 * times and not once per turn of the main loop.  A driver that runs out of
 * restarts publishes that as its reason and stops trying, so a device that
 * cannot transmit is a stated refusal rather than an endless retry.
 */
static bool apn_rtl_tx_restart(uint32_t now_ms)
{
    struct rtl8168_region_layout layout;
    uint32_t abandoned = 0U;

    if (!apn.rtl.supported || !apn.rtl.bring_up_done) {
        return false;
    }
    if (rtl8168_region_calculate(&layout) != RTL8168_OK) {
        apn.rtl.reason = "TX_RESTART_LAYOUT_FAILED";
        return false;
    }
    if (apn.rtl.tx_restarts >= APN_RTL_TX_RESTART_MAX) {
        apn.rtl.tx_restart_refused = 1U;
        apn.rtl.reason = "TX_RING_FULL_EXHAUSTED";
        /* Out of attempts: stop arming the stall instead of re-deciding it. */
        apn.rtl.tx_stall_armed = false;
        return false;
    }
    if (apn.rtl.tx_restart_ms != 0U &&
        (uint32_t)(now_ms - apn.rtl.tx_restart_ms) <
            APN_RTL_TX_RESTART_BACKOFF_MS) {
        return false;
    }

    /*
     * The attempt is counted before the part is touched.  A restart that fails
     * is still a restart: the bound and the backoff have to hold for a device
     * that answers the reset with nothing, or such a part would be reset on
     * every turn of the main loop - each reset a bounded but tight poll of the
     * part - and the loop the item exists to protect would starve.
     */
    ++apn.rtl.tx_restarts;
    apn.rtl.tx_restart_ms = now_ms;
    apn.rtl.tx_stall_seen = false;
    apn.rtl.tx_stall_armed = false;

    /*
     * Stop the part first, and only then hand the ring back to the CPU: the
     * rewind is valid only while the device is stopped.  A restart that
     * brought the part up first would program descriptors the device still
     * owns - the very stale descriptors the ring is full of - and ask it to
     * fetch them, and it could catch one mid-rewind (address cleared, OWN not
     * yet cleared) and read a frame from physical address zero.
     */
    if (rtl8168_reset(&apn.rtl.io) != RTL8168_OK) {
        apn.rtl.reason = "TX_RESTART_RESET_FAILED";
        return false;
    }
    if (rtl8168_tx_rewind(&apn.rtl.tx, &abandoned) != RTL8168_OK) {
        apn.rtl.reason = "TX_RESTART_REWIND_FAILED";
        return false;
    }
    /*
     * The frames the device never completed are declared lost here, not after
     * the bring-up: a part that then fails to come up has still abandoned
     * them, and the count is the honest record of what the restart cost.
     */
    apn.rtl.tx_restart_lost += abandoned;
    /*
     * The reset leaves the receive ring's descriptors armed but the device's
     * cursor back at the ring base, so the ring is re-posted before the part
     * is enabled - the order the boot-time path uses - which also returns the
     * driver's own receive cursor to the base and keeps the two in step.
     */
    if (rtl8168_rx_post(
            &apn.rtl.rx,
            apn.rtl.region_physical + layout.rx_buffer_offset,
            RTL8168_RX_BUFFER_CAPACITY) != RTL8168_OK) {
        apn.rtl.reason = "TX_RESTART_RX_POST_FAILED";
        return false;
    }
    if (!apn_rtl_bring_up()) {
        /*
         * `apn_rtl_bring_up()` names the step that failed (`ENABLE_FAILED`,
         * `BRING_UP_FAILED`, ...); that is a better reason than a second name
         * for the same failure.
         */
        return false;
    }
    apn.rtl.reason = "TX_RING_FULL_RESTARTED";
    return true;
}

static void apn_rtl_poll_tx(uint32_t now_ms)
{
    uint32_t reclaimed = 0U;

    (void)rtl8168_tx_reclaim(&apn.rtl.tx, &reclaimed);
    if (reclaimed != 0U) {
        /* The device handed something back: whatever looked stuck is not. */
        apn.rtl.tx_stall_seen = false;
        apn.rtl.tx_stall_armed = false;
        return;
    }
    /*
     * The stall is armed by a submission the ring refused - a frame the
     * appliance really lost - and disarmed by the first descriptor the device
     * hands back.  A ring that is merely full between two polls, or busy under
     * load, is therefore never restarted; a ring that refused a frame and then
     * stayed full for APN_RTL_TX_STALL_MS is.
     */
    if (!apn.rtl.tx_stall_armed ||
        apn.rtl.tx.in_flight < RTL8168_TX_DESCRIPTOR_COUNT) {
        return;
    }
    if (!apn.rtl.tx_stall_seen) {
        apn.rtl.tx_stall_seen = true;
        apn.rtl.tx_stall_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - apn.rtl.tx_stall_ms) < APN_RTL_TX_STALL_MS) {
        return;
    }
    (void)apn_rtl_tx_restart(now_ms);
}

/*
 * ITEM-486 (round 24): has the current receive pass spent its time budget?
 *
 * Called at a frame boundary only, so the pass always completes the frame it
 * started and the bound is on how many further frames it takes.  With no
 * deadline set (or before the cycle clock is calibrated) the frame-count bound
 * is the only one.
 */
/*
 * ITEM-486 (round 24): open this receive pass's time budget.
 *
 * Called where the ring is about to be drained, so the deadline covers the
 * frames the pass chooses and not the transmit and link work that ran before
 * it.  `timer_cycles_to_us()` returns zero until the cycle clock is calibrated,
 * which makes the deadline 2000 us and the check false, so an uncalibrated boot
 * is bounded by the frame count alone.
 */
static void apn_service_budget_open(void)
{
    apn.service_deadline_us =
        timer_cycles_to_us(timer_cycles()) + APN_SERVICE_BUDGET_US;
}

static bool apn_service_budget_spent(void)
{
    if (apn.service_deadline_us == 0U) {
        return false;
    }
    return timer_cycles_to_us(timer_cycles()) >= apn.service_deadline_us;
}

/*
 * ITEM-645: acknowledge the events this poll pass observed.
 *
 * Linux clears IntrStatus in its working interrupt handler; a part that runs
 * out of receive descriptors sets RxOverflow and stops receiving until that
 * bit is cleared, and returning descriptors alone does not restart it.  This
 * driver cleared the status word once at bring-up and never again, so the
 * 2026-11-22 Toshiba shape - all 16 descriptors owned by the NIC, IntrStatus
 * `0xA5 -> 0xF5`, no further frame - could never recover without a reboot.
 *
 * The receive-stopped pair is cleared only once the pass has proven the ring
 * has a free descriptor, i.e. it ended on a descriptor the NIC had not
 * written.  Clearing it while the pass stopped on its own budget would re-arm
 * the receiver with nowhere to write; the bit then stays latched for the next
 * pass.  The other observed bits are harmless to clear at any point: the
 * descriptor state, not the status bit, is what the receive path consumes.
 */
static void apn_rtl_ack_events(bool descriptors_available)
{
    uint16_t observed = 0U;
    uint16_t acknowledged = 0U;

    if (rtl8168_intr_ack_events(&apn.rtl.io, descriptors_available,
                                &observed, &acknowledged) != RTL8168_OK) {
        return;
    }
    if ((observed & RTL8168_INTR_RX_STOPPED) == 0U) {
        apn.rtl.rx_overflow_latched = false;
        return;
    }
    if (!apn.rtl.rx_overflow_latched) {
        apn.rtl.rx_overflow_latched = true;
        ++apn.rtl.rx_overflow;
    }
    if ((acknowledged & RTL8168_INTR_RX_STOPPED) != 0U) {
        ++apn.rtl.rx_overflow_acks;
        apn.rtl.rx_overflow_latched = false;
    }
}

static void apn_rtl_poll_rx(uint32_t now_ms)
{
    struct rtl8168_region_layout layout;
    bool descriptors_available = false;

    if (apn.rtl.dma_ring == NULL ||
        rtl8168_region_calculate(&layout) != RTL8168_OK) {
        return;
    }
    /* ITEM-486 (round 24): this pass's budget, opened at the ring. */
    apn_service_budget_open();
    for (uint32_t budget = 0U; budget < APN_RX_BUDGET_PER_PASS; ++budget) {
        const uint32_t slot =
            apn.rtl.rx.index % RTL8168_RX_DESCRIPTOR_COUNT;
        /*
         * ITEM-393: the receive buffer the device DMA'd into lives in the DMA
         * ring allocation; the physical address the descriptor carries and the
         * address the stack reads must be the same buffer.
         */
        const uint8_t *buffer =
            apn.rtl.dma_ring + layout.rx_buffer_offset +
            (size_t)slot * RTL8168_RX_BUFFER_STRIDE;
        const uint64_t buffer_physical =
            apn.rtl.region_physical + layout.rx_buffer_offset +
            (uint64_t)slot * RTL8168_RX_BUFFER_STRIDE;
        uint32_t frame_length = 0U;
        const enum rtl8168_status status = rtl8168_rx_complete(
            &apn.rtl.rx, RTL8168_RX_BUFFER_CAPACITY, &frame_length);

        if (status == RTL8168_RX_NOT_READY) {
            /*
             * The cursor descriptor is the NIC's, so the driver has reposted
             * every frame it consumed and the ring has room.  Only this
             * outcome proves the receive-stopped pair may be cleared.
             */
            descriptors_available = true;
            break;
        }
        /*
         * ITEM-486 (round 24): a frame is waiting, so this is a real frame
         * boundary.  If the pass has spent its budget it stops here and leaves
         * the frame in the ring for the next turn; `rtl8168_rx_complete()` only
         * inspected the descriptor, so nothing has been consumed.  The check
         * starts at `APN_SERVICE_MIN_FRAMES`, which is what makes a pass that
         * finds work take a floor of frames rather than however many the host's
         * speed happens to allow.
         */
        if (budget >= APN_SERVICE_MIN_FRAMES && apn_service_budget_spent()) {
            ++apn.service_budget_hits;
            break;
        }
        if (status == RTL8168_OK) {
            const bool wire_consumed =
                appliance_production_network_wire_boot_input(
                    buffer, frame_length, now_ms);
            bool lwip_accepted = false;

            if (!wire_consumed) {
                lwip_accepted =
                    appliance_production_network_input(
                        apn_network(), buffer, frame_length, now_ms) ==
                    APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED;
            }
            if (appliance_production_network_rx_frame_received(
                    wire_consumed, lwip_accepted)) {
                ++apn.rtl.rx_frames;
            } else {
                ++apn.rtl.rx_dropped;
            }
        } else {
            ++apn.rtl.rx_dropped;
            ++apn.rtl.rx_errors;
        }
        if (rtl8168_rx_repost(&apn.rtl.rx, buffer_physical,
                              RTL8168_RX_BUFFER_CAPACITY) != RTL8168_OK) {
            break;
        }
    }
    /*
     * ITEM-645: a poll-driven receiver still has to acknowledge the events it
     * saw.  This runs after the drain so a cleared RxOverflow cannot re-arm a
     * receiver with no free descriptor.
     */
    apn_rtl_ack_events(descriptors_available);
}

static void apn_rtl_poll(uint32_t now_ms)
{
    if (!apn.rtl.bring_up_done) {
        return;
    }
    apn_rtl_poll_tx(now_ms);
    if (apn.rtl.bound) {
        apn_rtl_poll_rx(now_ms);
        return;
    }
    /*
     * ITEM-583: a PHY that reset itself (a cable event can do it) comes back
     * powered down.  Restart negotiation instead of waiting forever for a link
     * the part will never bring up.
     */
    if (rtl8168_phy_mii_supported(&apn.rtl.chip) && apn.rtl.phy.valid &&
        apn.rtl.phy.powered_down) {
        (void)rtl8168_phy_configure(&apn.rtl.io, &apn.rtl.phy);
    }
    {
        const uint32_t interval =
            apn.rtl.link_attempts >= APN_RTL_LINK_ATTEMPTS_MAX
                ? APN_RTL_LINK_IDLE_RETRY_MS
                : APN_RTL_LINK_RETRY_MS;

        if (apn.rtl.next_attempt_ms != 0U &&
            now_ms < apn.rtl.next_attempt_ms) {
            return;
        }
        apn.rtl.next_attempt_ms = now_ms + interval;
    }
    if (apn.rtl.link_attempts != UINT32_MAX) {
        ++apn.rtl.link_attempts;
    }
    if (apn.rtl.bind_attempted) {
        /*
         * The previous attempt left the composition bound to a link that was
         * not up; release it before binding again so the netif is staged
         * against the link the device reports now.
         */
        (void)appliance_production_network_stop(&apn.network);
    }
    (void)apn_rtl_bind();
}

/*
 * One bounded Realtek bring-up attempt.  Every failure is published with a
 * reason: a device that is present but cannot be driven must never look
 * either absent or ready.
 */
static bool apn_rtl_init(void)
{
    if (!apn_rtl_find()) {
        return false;
    }
    apn.device_present = true;
    apn.pci_vendor_id = apn.rtl.vendor_id;
    apn.pci_product_id = apn.rtl.product_id;
    apn.pci_revision = apn.rtl.revision;
    if (apn.rtl.reason != NULL) {
        return false;
    }
    if (!apn_rtl_prepare()) {
        return false;
    }
    if (!apn_rtl_bring_up()) {
        return false;
    }
    apn.backend = APN_BACKEND_RTL8168;
    apn.stage = 20U;
    (void)apn_rtl_bind();
    return true;
}


/* ------------------------------------------------------------------------ *
 * ITEM-494 Intel 8254x (e1000) production backend.
 *
 * The second physical vendor answers the same contract as the first: one PCI
 * function found by its own claim, one bounded bring-up, one bound composition
 * and one polled frame path.  Everything here is the adapter's half of the
 * price - mapping BAR0, one contiguous DMA region that holds both legacy rings
 * and their buffers, and the TDT/RDT doorbells the register core never writes -
 * while the silicon-specific register sequence lives in `kernel/src/e1000.c`
 * and is proven by `tests/e1000-host.c`.
 * ------------------------------------------------------------------------ */

static bool apn_e1000_read32(void *context, uint32_t offset, uint32_t *value)
{
    (void)context;
    if (value == NULL || apn.e1000.region == NULL ||
        offset + 4U > APN_E1000_REGISTER_LENGTH) {
        return false;
    }
    *value = *(volatile uint32_t *)(apn.e1000.region + offset);
    return true;
}

static bool apn_e1000_write32(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    if (apn.e1000.region == NULL ||
        offset + 4U > APN_E1000_REGISTER_LENGTH) {
        return false;
    }
    *(volatile uint32_t *)(apn.e1000.region + offset) = value;
    return true;
}

static bool apn_e1000_link_read(void *context, bool *link_up)
{
    (void)context;
    return e1000_link_status(&apn.e1000.io, link_up) == E1000_OK;
}

/*
 * One bounded scan for the claimed Intel part, the same walk the Realtek path
 * uses: function zero is read once per device, and the remaining functions are
 * visited only when the header declares a multi-function device.
 */
static bool apn_e1000_find(void)
{
    uint32_t bus;
    uint8_t device;

    for (bus = 0U; bus < APN_PCI_MAX_BUS; ++bus) {
        for (device = 0U; device < APN_PCI_MAX_DEVICE; ++device) {
            const uint32_t first = apn_pci_read32(
                (uint8_t)bus, device, 0U, APN_PCI_VENDOR_OFFSET);
            const uint16_t first_vendor = (uint16_t)(first & 0xFFFFU);
            uint8_t last_function = 0U;
            uint8_t function;

            if (first_vendor == 0xFFFFU) {
                continue;
            }
            if ((apn_pci_read32((uint8_t)bus, device, 0U, 0x0CU) &
                 ((uint32_t)APN_PCI_MULTIFUNCTION << 16U)) != 0U) {
                last_function = (uint8_t)(APN_PCI_MAX_FUNCTION - 1U);
            }
            for (function = 0U; function <= last_function; ++function) {
                const uint32_t identity = apn_pci_read32(
                    (uint8_t)bus, device, function,
                    APN_PCI_VENDOR_OFFSET);
                const uint16_t vendor = (uint16_t)(identity & 0xFFFFU);
                const uint16_t product = (uint16_t)(identity >> 16U);
                struct pci_config_io config_io;
                struct pci_device_header header;
                struct pci_location location;

                if (vendor != E1000_VENDOR_ID) {
                    continue;
                }
                config_io.read32 = apn_pci_read;
                config_io.write32 = apn_pci_write;
                config_io.context = &apn;
                location.bus = (uint8_t)bus;
                location.device = device;
                location.function = function;
                if (pci_device_read(&config_io, &location, &header) !=
                    PCI_OK) {
                    continue;
                }
                if (header.base_class != E1000_PCI_CLASS_NETWORK ||
                    header.subclass != E1000_PCI_SUBCLASS_ETHERNET) {
                    continue;
                }
                apn.e1000.bus = (uint8_t)bus;
                apn.e1000.device = device;
                apn.e1000.function = function;
                apn.e1000.vendor_id = vendor;
                apn.e1000.product_id = product;
                apn.e1000.revision = header.revision;
                apn.pci_vendor_id = vendor;
                apn.pci_product_id = product;
                apn.pci_revision = header.revision;
                apn.e1000.probed = true;
                if (e1000_pci_identity_supported(&header)) {
                    const struct pci_bar *bar = NULL;

                    if (e1000_memory_bar(&header, &bar) != E1000_OK ||
                        bar == NULL) {
                        apn.e1000.reason = "NO_MEMORY_BAR";
                        return true;
                    }
                    /*
                     * The same two command bits the Realtek path needs, read
                     * and written before the first register access, with the
                     * value published either way.
                     */
                    if (!apn_pci_enable_bus_master(
                            (uint8_t)bus, device, function,
                            &apn.e1000.pci_command)) {
                        apn.e1000.reason = "PCI_BUS_MASTER_REFUSED";
                        return true;
                    }
                    if (!apn_map_mmio(bar->base,
                                      APN_E1000_REGISTER_LENGTH)) {
                        apn.e1000.reason = "MMIO_MAP_FAILED";
                        return true;
                    }
                    apn.e1000.mmio_physical = bar->base;
                    apn.e1000.region = apn_phys(bar->base);
                    return true;
                }
                /*
                 * An Intel network function the claim does not cover: named
                 * with its ids and refused, never driven with a register map
                 * that was written for another part.
                 */
                apn.e1000.reason = "DEVICE_ID";
                return true;
            }
        }
    }
    return false;
}

static bool apn_e1000_prepare(void)
{
    if (!apn.e1000.probed || apn.e1000.region == NULL) {
        return false;
    }
    apn.e1000.io.context = &apn;
    apn.e1000.io.read32 = apn_e1000_read32;
    apn.e1000.io.write32 = apn_e1000_write32;
    if (e1000_mac_read(&apn.e1000.io, apn.e1000.mac) != E1000_OK) {
        apn.e1000.reason = "MAC_UNAVAILABLE";
        return false;
    }
    {
        struct e1000_region_layout layout;
        struct pci_dma_buffer region_buffer;
        uint32_t pages;

        if (e1000_region_calculate(&layout) != E1000_OK) {
            apn.e1000.reason = "REGION_LAYOUT_FAILED";
            return false;
        }
        pages = (layout.length + 4095U) / 4096U;
        if (pages == 0U || pages > APN_E1000_REGION_PAGES) {
            apn.e1000.reason = "REGION_TOO_LARGE";
            return false;
        }
        apn.e1000.region_physical = pmm_alloc_contiguous_pages(pages);
        if (apn.e1000.region_physical == 0U) {
            apn.e1000.reason = "DMA_ALLOCATION_FAILED";
            return false;
        }
        if (pci_dma_buffer_publish(
                &region_buffer, apn.e1000.region_physical,
                (size_t)(pages * 4096U),
                PCI_DMA_DIRECTION_BIDIRECTIONAL) != PCI_DMA_OK) {
            apn.e1000.reason = "DMA_REGION_INVALID";
            return false;
        }
        if (!e1000_dma_address_supported(apn.e1000.region_physical) ||
            !e1000_dma_address_supported(
                apn.e1000.region_physical + region_buffer.length - 1U)) {
            apn.e1000.reason = "DMA_ABOVE_4G";
            return false;
        }
        {
            uint8_t *base = apn_phys(apn.e1000.region_physical);

            /*
             * Both rings and every frame buffer live inside this one
             * allocation, reached only through `dma_ring`/`region_physical`
             * and never through the mapped MMIO window.
             */
            apn.e1000.dma_ring = base;
            apn_zero(base, (size_t)region_buffer.length);
            e1000_ring_init(
                &apn.e1000.rx,
                (struct e1000_rx_descriptor *)(uintptr_t)(
                    base + layout.rx_descriptor_offset),
                E1000_RX_DESCRIPTOR_COUNT);
            e1000_ring_init(
                &apn.e1000.tx,
                (struct e1000_tx_descriptor *)(uintptr_t)(
                    base + layout.tx_descriptor_offset),
                E1000_TX_DESCRIPTOR_COUNT);
            if (e1000_rx_post(
                    &apn.e1000.rx,
                    apn.e1000.region_physical + layout.rx_buffer_offset,
                    E1000_RX_BUFFER_CAPACITY) != E1000_OK) {
                apn.e1000.reason = "RX_RING_POST_FAILED";
                return false;
            }
        }
    }
    apn.e1000.supported = true;
    return true;
}

/*
 * The register half of a bring-up, run after the caller reset the part: mask
 * the interrupt causes (this driver is polled, like every other backend), run
 * the register sequence and publish the receiver's own arming answer.  The
 * bind refuses on `RX_NOT_ARMED`/`RX_ARM_PROBE_FAILED` instead of assuming the
 * receive path, the rule ITEM-439 gave the Realtek path.
 */
static bool apn_e1000_bring_up(void)
{
    struct e1000_region_layout layout;
    struct e1000_bring_up configuration;

    if (!apn.e1000.supported) {
        return false;
    }
    if (e1000_region_calculate(&layout) != E1000_OK) {
        apn.e1000.reason = "REGION_LAYOUT_FAILED";
        return false;
    }
    memset(&configuration, 0, sizeof(configuration));
    configuration.rx_descriptor_physical =
        apn.e1000.region_physical + layout.rx_descriptor_offset;
    configuration.tx_descriptor_physical =
        apn.e1000.region_physical + layout.tx_descriptor_offset;
    configuration.rx_buffer_capacity = E1000_RX_BUFFER_CAPACITY;

    if (e1000_interrupts_mask(&apn.e1000.io) != E1000_OK) {
        apn.e1000.reason = "INTERRUPT_MASK_FAILED";
        return false;
    }
    {
        const enum e1000_status bring_up_status =
            e1000_bring_up(&apn.e1000.io, &configuration);

        if (bring_up_status != E1000_OK) {
            apn.e1000.reason =
                bring_up_status == E1000_ENABLE_FAILED
                    ? "ENABLE_FAILED"
                    : "BRING_UP_FAILED";
            return false;
        }
    }
    {
        bool armed = false;

        apn.e1000.rx_ready = false;
        apn.e1000.rx_ready_known = false;
        if (e1000_rx_armed(&apn.e1000.io, &armed) == E1000_OK) {
            apn.e1000.rx_ready = armed;
            apn.e1000.rx_ready_known = true;
        }
    }
    apn.e1000.bring_up_done = true;
    return true;
}

static void apn_e1000_publish_snapshot(void)
{
    memset(&apn.e1000.snapshot, 0, sizeof(apn.e1000.snapshot));
    apn.e1000.snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    apn.e1000.snapshot.device_id = NETWORK_BACKEND_DEVICE_ID;
    apn.e1000.snapshot.vendor_id = apn.e1000.vendor_id;
    apn.e1000.snapshot.product_id = apn.e1000.product_id;
    apn.e1000.snapshot.bus_address =
        ((uint32_t)apn.e1000.bus << 8U) |
        ((uint32_t)apn.e1000.device << 3U) |
        apn.e1000.function;
    memcpy(apn.e1000.snapshot.mac_address, apn.e1000.mac,
           NETWORK_DEVICE_MAC_LENGTH);
    apn.e1000.snapshot.capability_mask = NETWORK_BACKEND_CAPABILITY_MASK;
}

static bool apn_e1000_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    struct e1000_region_layout layout;
    uint32_t padded;
    uint32_t slot;
    uint8_t *buffer;
    uint64_t buffer_physical;
    uint32_t reclaimed = 0U;

    (void)context;
    if (frame == NULL || length == 0U) {
        return false;
    }
    if (apn.e1000.dma_ring == NULL) {
        ++apn.e1000.tx_dropped;
        return false;
    }
    padded = e1000_tx_frame_length(length);
    if (padded == 0U) {
        ++apn.e1000.tx_dropped;
        return false;
    }
    if (e1000_region_calculate(&layout) != E1000_OK) {
        return false;
    }
    (void)e1000_tx_reclaim(&apn.e1000.tx, &reclaimed);
    if (apn.e1000.tx.in_flight >= E1000_TX_DESCRIPTOR_COUNT) {
        /*
         * The ring is full because the device has not handed a descriptor
         * back; the frame is refused, never queued or overwritten, and the
         * refusal arms the same bounded restart the Realtek path uses.
         */
        ++apn.e1000.tx_ring_full;
        ++apn.e1000.tx_dropped;
        apn.e1000.tx_stall_armed = true;
        return false;
    }
    slot = apn.e1000.tx.index % E1000_TX_DESCRIPTOR_COUNT;
    buffer = apn.e1000.dma_ring + layout.tx_buffer_offset +
             (size_t)slot * E1000_TX_BUFFER_CAPACITY;
    apn_zero(buffer, E1000_TX_BUFFER_CAPACITY);
    memcpy(buffer, frame, length);
    buffer_physical = apn.e1000.region_physical + layout.tx_buffer_offset +
                      (uint64_t)slot * E1000_TX_BUFFER_CAPACITY;
    if (e1000_tx_program(&apn.e1000.tx, buffer_physical, padded,
                         E1000_TX_BUFFER_CAPACITY) != E1000_OK) {
        ++apn.e1000.tx_dropped;
        return false;
    }
    /*
     * The core programs the descriptor; the doorbell is the adapter's.  TDT
     * takes the index of the next descriptor the device may fetch, which is
     * where `e1000_tx_program()` left the ring cursor.
     */
    if (!apn_e1000_write32(NULL, E1000_REG_TDT, apn.e1000.tx.index)) {
        ++apn.e1000.tx_dropped;
        return false;
    }
    ++apn.e1000.tx_frames;
    return true;
}

/*
 * The same bounded bind as the Realtek path: the link is read from the part,
 * the receive path must be armed, the composition binds against the device's
 * own address, and every refusal leaves its name on the device's `[NIC]`
 * record with the address the driver read.
 */
static bool apn_e1000_bind(void)
{
    struct network_backend_readiness readiness;
    struct network_link_state_record link_record;
    struct appliance_production_network_backend_inputs inputs;
    struct network_backend_link_io io;
    enum network_backend_status bind_status;
    bool link_up = false;

    apn.e1000.bound = false;
    /*
     * ITEM-494: the link is a PHY state, not a driver one.  STATUS.LU is set
     * by the completed auto-negotiation, and a machine whose link partner
     * appeared after the device was reset has no other path to it - on the
     * emulated 8254x the PHY starts un-negotiated and only a BMCR restart
     * finishes it.  The restart is bounded and spaced
     * (APN_E1000_AUTONEG_ATTEMPTS), so it never re-arms the negotiation it
     * waits for.
     */
    if (apn.e1000.link_attempts == 0U ||
        apn.e1000.link_attempts % APN_E1000_AUTONEG_ATTEMPTS == 0U) {
        (void)e1000_restart_autoneg(&apn.e1000.io);
    }
    apn_e1000_publish_snapshot();
    if (e1000_link_status(&apn.e1000.io, &link_up) != E1000_OK) {
        apn.e1000.reason = "LINK_READ_FAILED";
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }

    memset(&link_record, 0, sizeof(link_record));
    link_record.version = NETWORK_LINK_STATE_VERSION;
    link_record.device_id = NETWORK_BACKEND_DEVICE_ID;
    link_record.state = link_up ? NETWORK_LINK_UP : NETWORK_LINK_DOWN;
    link_record.generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;

    if (!apn.e1000.rx_ready_known) {
        apn.e1000.reason = "RX_ARM_PROBE_FAILED";
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }
    if (!apn.e1000.rx_ready) {
        apn.e1000.reason = "RX_NOT_ARMED";
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }
    readiness.receive_path_ready = true;
    readiness.transmit_path_ready = true;
    io.read_link_up = apn_e1000_link_read;
    io.context = &apn;

    memset(&apn.e1000.binding, 0, sizeof(apn.e1000.binding));
    bind_status = network_backend_bind(
        &apn.e1000.binding, &apn.e1000.snapshot, &link_record, &readiness,
        &io, APPLIANCE_PRODUCTION_NETWORK_GENERATION);
    if (bind_status != NETWORK_BACKEND_OK) {
        apn.e1000.reason = network_backend_status_string(bind_status);
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }

    memset(&inputs, 0, sizeof(inputs));
    inputs.binding = &apn.e1000.binding;
    inputs.ipv4_config = NULL;
    inputs.output.transmit = apn_e1000_transmit;
    inputs.output.context = &apn;
    apn.stage = 40U;
    if (appliance_production_network_bind_backend(
            &apn.network, &inputs) != APPLIANCE_PRODUCTION_NETWORK_OK &&
        !appliance_production_network_bound(&apn.network)) {
        apn.e1000.reason = "BIND_FAILED";
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }
    apn.e1000.bind_attempted = true;
    if (!link_up) {
        apn.e1000.reason = "LINK_DOWN";
        apn_nic_record_bind_result(false, apn.e1000.reason, apn.e1000.mac);
        return false;
    }
    apn.e1000.bound = true;
    apn.e1000.reason = NULL;
    apn.stage = 100U;
    apn_nic_record_bind_result(true, NULL, apn.e1000.mac);
    return true;
}

/*
 * The bounded transmit restart, in the same order as the Realtek path: stop
 * the part, hand the ring back to the CPU, re-post the receive ring, then
 * bring the part up again.  The attempt is counted before the part is touched,
 * so a part that answers the reset with nothing cannot be reset on every turn
 * of the main loop.
 */
static bool apn_e1000_tx_restart(uint32_t now_ms)
{
    struct e1000_region_layout layout;

    if (!apn.e1000.supported || !apn.e1000.bring_up_done) {
        return false;
    }
    if (e1000_region_calculate(&layout) != E1000_OK) {
        apn.e1000.reason = "TX_RESTART_LAYOUT_FAILED";
        return false;
    }
    if (apn.e1000.tx_restarts >= APN_E1000_TX_RESTART_MAX) {
        apn.e1000.tx_restart_refused = 1U;
        apn.e1000.reason = "TX_RING_FULL_EXHAUSTED";
        apn.e1000.tx_stall_armed = false;
        return false;
    }
    if (apn.e1000.tx_restart_ms != 0U &&
        (uint32_t)(now_ms - apn.e1000.tx_restart_ms) <
            APN_E1000_TX_RESTART_BACKOFF_MS) {
        return false;
    }
    ++apn.e1000.tx_restarts;
    apn.e1000.tx_restart_ms = now_ms;
    apn.e1000.tx_stall_seen = false;
    apn.e1000.tx_stall_armed = false;
    if (e1000_reset(&apn.e1000.io) != E1000_OK) {
        apn.e1000.reason = "TX_RESTART_RESET_FAILED";
        return false;
    }
    if (e1000_tx_rewind(&apn.e1000.tx) != E1000_OK) {
        apn.e1000.reason = "TX_RESTART_REWIND_FAILED";
        return false;
    }
    if (e1000_rx_post(
            &apn.e1000.rx,
            apn.e1000.region_physical + layout.rx_buffer_offset,
            E1000_RX_BUFFER_CAPACITY) != E1000_OK) {
        apn.e1000.reason = "TX_RESTART_RX_POST_FAILED";
        return false;
    }
    if (!apn_e1000_bring_up()) {
        /* The bring-up names the step that failed. */
        return false;
    }
    apn.e1000.reason = "TX_RING_FULL_RESTARTED";
    return true;
}

static void apn_e1000_poll_tx(uint32_t now_ms)
{
    uint32_t reclaimed = 0U;

    (void)e1000_tx_reclaim(&apn.e1000.tx, &reclaimed);
    if (reclaimed != 0U) {
        apn.e1000.tx_stall_seen = false;
        apn.e1000.tx_stall_armed = false;
        return;
    }
    if (!apn.e1000.tx_stall_armed ||
        apn.e1000.tx.in_flight < E1000_TX_DESCRIPTOR_COUNT) {
        return;
    }
    if (!apn.e1000.tx_stall_seen) {
        apn.e1000.tx_stall_seen = true;
        apn.e1000.tx_stall_ms = now_ms;
        return;
    }
    if ((uint32_t)(now_ms - apn.e1000.tx_stall_ms) <
        APN_E1000_TX_STALL_MS) {
        return;
    }
    (void)apn_e1000_tx_restart(now_ms);
}

static void apn_e1000_poll_rx(uint32_t now_ms)
{
    struct e1000_region_layout layout;

    if (apn.e1000.dma_ring == NULL ||
        e1000_region_calculate(&layout) != E1000_OK) {
        return;
    }
    /* ITEM-486 (round 24): this pass's budget, opened at the ring. */
    apn_service_budget_open();
    for (uint32_t budget = 0U; budget < APN_RX_BUDGET_PER_PASS; ++budget) {
        const uint32_t slot =
            apn.e1000.rx.index % E1000_RX_DESCRIPTOR_COUNT;
        const uint8_t *buffer =
            apn.e1000.dma_ring + layout.rx_buffer_offset +
            (size_t)slot * E1000_RX_BUFFER_CAPACITY;
        const uint64_t buffer_physical =
            apn.e1000.region_physical + layout.rx_buffer_offset +
            (uint64_t)slot * E1000_RX_BUFFER_CAPACITY;
        uint16_t frame_length = 0U;
        const enum e1000_status status = e1000_rx_complete(
            &apn.e1000.rx, slot, E1000_RX_BUFFER_CAPACITY, &frame_length);

        if (status == E1000_RX_NOT_READY) {
            return;
        }
        if (budget >= APN_SERVICE_MIN_FRAMES && apn_service_budget_spent()) {
            ++apn.service_budget_hits;
            break;
        }
        if (status == E1000_OK) {
            const bool wire_consumed =
                appliance_production_network_wire_boot_input(
                    buffer, frame_length, now_ms);
            bool lwip_accepted = false;

            if (!wire_consumed) {
                lwip_accepted =
                    appliance_production_network_input(
                        apn_network(), buffer, frame_length, now_ms) ==
                    APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED;
            }
            if (appliance_production_network_rx_frame_received(
                    wire_consumed, lwip_accepted)) {
                ++apn.e1000.rx_frames;
            } else {
                ++apn.e1000.rx_dropped;
            }
        } else {
            ++apn.e1000.rx_dropped;
            ++apn.e1000.rx_errors;
        }
        if (e1000_rx_repost(&apn.e1000.rx, slot, buffer_physical) !=
            E1000_OK) {
            return;
        }
        apn.e1000.rx.index = e1000_ring_next(&apn.e1000.rx, slot);
        /*
         * RDT is the last descriptor handed back to the part, so the ring's
         * tail follows the driver's cursor and not the other way round.
         */
        if (!apn_e1000_write32(NULL, E1000_REG_RDT, slot)) {
            return;
        }
    }
}

static void apn_e1000_poll(uint32_t now_ms)
{
    if (!apn.e1000.bring_up_done) {
        return;
    }
    apn_e1000_poll_tx(now_ms);
    if (apn.e1000.bound) {
        apn_e1000_poll_rx(now_ms);
        return;
    }
    if (apn.e1000.link_attempts >= APN_E1000_LINK_ATTEMPTS_MAX ||
        (apn.e1000.next_attempt_ms != 0U &&
         now_ms < apn.e1000.next_attempt_ms)) {
        return;
    }
    apn.e1000.next_attempt_ms = now_ms + APN_E1000_LINK_RETRY_MS;
    ++apn.e1000.link_attempts;
    if (apn.e1000.bind_attempted) {
        (void)appliance_production_network_stop(&apn.network);
    }
    (void)apn_e1000_bind();
}

/*
 * One bounded Intel bring-up attempt.  The part is reset first, then the
 * register half runs; every failure is published with a reason, so a part that
 * is present but cannot be driven never looks either absent or ready.
 */
static bool apn_e1000_init(void)
{
    if (!apn_e1000_find()) {
        return false;
    }
    apn.device_present = true;
    apn.pci_vendor_id = apn.e1000.vendor_id;
    apn.pci_product_id = apn.e1000.product_id;
    apn.pci_revision = apn.e1000.revision;
    if (apn.e1000.reason != NULL) {
        return false;
    }
    if (!apn_e1000_prepare()) {
        return false;
    }
    if (e1000_reset(&apn.e1000.io) != E1000_OK) {
        apn.e1000.reason = "RESET_FAILED";
        return false;
    }
    if (!apn_e1000_bring_up()) {
        return false;
    }
    apn.backend = APN_BACKEND_E1000;
    apn.stage = 20U;
    (void)apn_e1000_bind();
    return true;
}

/*
 * The number of transmit frames the device has completed but the driver has
 * not reclaimed yet.
 */
static uint32_t apn_tx_completed_count(void)
{
    const struct virtio_used_consumption *used =
        &apn.composition.transmit.used;

    return (uint32_t)(uint16_t)(
        (uint16_t)used->ring->idx - (uint16_t)used->expected_idx);
}

/*
 * Transmit recycling.
 *
 * virtio_net_tx_submit programs descriptor slot 'submitted_count' for every
 * frame, so the driver owns a contiguous in-flight window that starts at slot
 * zero; the frozen module reclaims by completion head and would leave a hole
 * below a live slot if a partial batch were reclaimed, and the next submission
 * would then overwrite a live slot record.  The driver therefore reclaims only
 * once the device has completed every submitted frame, which empties the
 * window and restores the invariant.  Without this cycle the eight slots are
 * consumed once and the appliance can never transmit again, which is what
 * stopped the serving proof: the server could answer the first ClientHello
 * segment and then had no transmit slot left for the next acknowledgement.
 *
 * Each reclaimed frame also returns its available publication and its two
 * bound DMA regions, so the counters the module keeps as
 * 'published_count == submitted_count + reclaimed_count' return to rest here,
 * and the bounded used-consume counter is reclaimed with it.
 */
static bool apn_tx_recycle(void)
{
    struct virtio_net_tx_completion completion;
    uint32_t in_flight;

    if (apn.tx_stalled) {
        return false;
    }
    in_flight = apn.tx.submitted_count;
    if (in_flight == 0U) {
        return true;
    }
    if (apn_tx_completed_count() < in_flight) {
        return false;
    }
    while (in_flight > 0U) {
        const enum virtio_net_tx_status status =
            virtio_net_tx_reclaim(&apn.tx, &completion);

        --in_flight;
        if (status == VIRTIO_NET_TX_OK) {
            if (apn.tx.reclaimed_count != 0U) {
                --apn.tx.reclaimed_count;
            }
            if (apn.composition.transmit.available.published_count != 0U) {
                --apn.composition.transmit.available.published_count;
            }
            apn.composition.transmit.used.consumed_count = 0U;
            ++apn.tx_reclaimed;
            continue;
        }
        if (status == VIRTIO_NET_TX_STALE_COMPLETION ||
            status == VIRTIO_NET_TX_INVALID_LENGTH) {
            /* The module consumed the used entry by itself and the frame
               stays in flight. */
            apn.composition.transmit.used.consumed_count = 0U;
            continue;
        }
        break;
    }
    if (apn.tx.submitted_count != 0U) {
        /* A device that leaves the batch partially completed cannot be
           served without corrupting a live slot: fail closed and keep the
           ring consistent instead of overwriting a frame in flight. */
        apn.tx_stalled = true;
        return false;
    }
    return true;
}

static bool apn_transmit(
    void *context,
    const uint8_t *frame,
    size_t length)
{
    struct apn_state *state = (struct apn_state *)context;
    struct virtio_net_tx_frame tx_frame;
    struct virtio_net_tx_submission submission;
    uint32_t slot;
    uint8_t *buffer;

    if (state == NULL || frame == NULL || length == 0U ||
        length > APN_MAX_FRAME_LENGTH) {
        ++apn.tx_dropped;
        return false;
    }
    (void)apn_tx_recycle();
    if (state->tx_stalled ||
        state->tx.submitted_count >= APN_TX_COUNT) {
        /*
         * ITEM-428: the virtio window's own "no room" case.  It is counted
         * under the same name as the Realtek ring's refusal, so the boot
         * record's `tx_full=` means one thing on either backend: a submission
         * the frame path would not take.
         */
        if (!state->tx_stalled &&
            state->tx.submitted_count >= APN_TX_COUNT) {
            ++apn.tx_ring_full;
        }
        ++apn.tx_dropped;
        return false;
    }
    slot = state->tx.submitted_count;
    buffer = (uint8_t *)state->tx_frame_virtual[slot];
    apn_zero(buffer, APN_TX_FRAME_LENGTH);
    memcpy(buffer, frame, length);
    if (length < APN_MIN_FRAME_LENGTH) {
        /* The device minimum frame is 60 bytes; padding is zero. */
    }
    tx_frame.data = buffer;
    tx_frame.physical_base = state->tx_frame_physical[slot];
    tx_frame.length = (uint32_t)(length < APN_MIN_FRAME_LENGTH
        ? APN_MIN_FRAME_LENGTH
        : length);
    state->tx_frame_length[slot] = tx_frame.length;
    if (virtio_net_tx_submit(
            &state->tx, &tx_frame, &submission) != VIRTIO_NET_TX_OK) {
        ++apn.tx_dropped;
        return false;
    }
    if (!state->tx_notify_pending) {
        /* The submission advertises a chain that was never programmed. */
        state->tx_stalled = true;
        ++apn.tx_dropped;
        return false;
    }
    apn_notify_queue(
        1U,
        (uint16_t)virtio_available_publication_visible_index(
            &state->composition.transmit.available));
    state->tx_notify_pending = false;
    ++apn.tx_frames;
    return true;
}

/*
 * Receive replenishment.
 *
 * virtio_net_rx_complete consumes the used entry and releases the posted
 * buffer, while the module keeps 'published_count == capacity' and one bound
 * DMA region per buffer.  The driver therefore has to account for the consumed
 * publication and immediately post a fresh descriptor for the released buffer:
 * it keeps the buffer's existing DMA binding (the module does not release it)
 * and takes a newly reserved descriptor, exactly the reference QEMU fixture's
 * cycle.  Without it the device consumes the eight buffers posted at
 * activation and drops every later frame, so the second and third segments of
 * the client's first flight never reach lwIP.
 */
static bool apn_repost_rx(uint32_t buffer_index)
{
    struct virtio_net_queue *queue = &apn.composition.receive;
    struct virtio_net_rx_buffer *buffer;
    struct virtio_descriptor_token descriptor;
    struct virtio_available_token available;
    uint16_t chain[1];

    /*
     * Read the controller's own capacity field.  The validating accessors
     * refuse to answer while the module invariant is unsettled, and this
     * callback runs exactly in that window: the completion has already
     * returned the consumed publication and the fresh one is not posted yet,
     * so `virtio_net_rx_capacity()` would report zero for every replenishment
     * and the device would keep dropping frames after the eight buffers
     * posted at activation.
     */
    if (buffer_index >= apn.rx.capacity) {
        return false;
    }
    buffer = &apn.rx.buffers[buffer_index];
    if (buffer->posted) {
        return false;
    }
    if (virtio_descriptor_ownership_reserve(
            &queue->descriptors, &descriptor) !=
        VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        return false;
    }
    apn_write_descriptor(
        &apn.queues[0], descriptor.index,
        buffer->physical_base, buffer->length,
        APN_VIRTQ_DESC_F_WRITE, 0U);
    if (virtio_descriptor_ownership_publish(
            &queue->descriptors, &descriptor) !=
        VIRTIO_DESCRIPTOR_OWNERSHIP_OK) {
        (void)virtio_descriptor_ownership_rollback(
            &queue->descriptors, &descriptor);
        return false;
    }
    chain[0] = (uint16_t)descriptor.index;
    if (virtio_available_publication_prepare(
            &queue->available, chain, 1U, &available) !=
        VIRTIO_AVAILABLE_PUBLICATION_OK) {
        (void)virtio_descriptor_ownership_consume(
            &queue->descriptors, &descriptor);
        return false;
    }
    if (virtio_available_publication_publish(
            &queue->available, &available) !=
        VIRTIO_AVAILABLE_PUBLICATION_OK) {
        (void)virtio_available_publication_cancel(
            &queue->available, &available);
        (void)virtio_descriptor_ownership_consume(
            &queue->descriptors, &descriptor);
        return false;
    }
    buffer->posted = true;
    buffer->descriptor = descriptor;
    buffer->generation = descriptor.generation;
    ++apn.rx.posted_count;
    apn_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &queue->available));
    return true;
}

static void apn_poll_rx(uint32_t now_ms)
{
    struct virtio_net_rx *rx = &apn.rx;

    if (!virtio_net_rx_published(rx)) {
        return;
    }
    /* ITEM-486 (round 24): this pass's budget, opened at the ring. */
    apn_service_budget_open();
    for (uint32_t budget = 0U; budget < APN_RX_BUDGET_PER_PASS; ++budget) {
        struct virtio_used_consumption *used =
            &apn.composition.receive.used;
        struct virtio_net_rx_frame rx_frame;
        const struct virtio_net_rx_buffer *buffer;
        uint32_t slot;
        uint16_t id;
        uint32_t index;
        bool found = false;

        if ((uint16_t)used->expected_idx == used->ring->idx) {
            return;
        }
        /*
         * ITEM-486 (round 24): the ring has a frame for this pass, so this is
         * a real frame boundary.  The time bound ends the pass here with the
         * ring intact for the next turn, and starts at
         * `APN_SERVICE_MIN_FRAMES` so a pass that finds work keeps a floor of
         * progress whatever the host's speed.
         */
        if (budget >= APN_SERVICE_MIN_FRAMES && apn_service_budget_spent()) {
            ++apn.service_budget_hits;
            break;
        }
        slot = used->expected_idx % used->queue_size;
        id = used->ring->ring[slot].id;
        for (index = 0U; index < virtio_net_rx_capacity(rx); ++index) {
            const struct virtio_net_rx_buffer *candidate =
                virtio_net_rx_buffer_at(rx, index);

            if (candidate != NULL && candidate->posted &&
                candidate->descriptor.index == id) {
                buffer = candidate;
                found = true;
                break;
            }
        }
        if (!found) {
            return;
        }
        if (virtio_net_rx_complete(
                rx, apn_phys(buffer->physical_base), &rx_frame) !=
            VIRTIO_NET_RX_OK) {
            return;
        }
        /*
         * The completion released one posted buffer: return the consumed
         * publication and the bounded used counter first, then feed the frame
         * and post a fresh descriptor, so the module's
         * 'published_count == capacity' invariant holds again before the next
         * completion is validated.
         */
        if (apn.composition.receive.available.published_count != 0U) {
            --apn.composition.receive.available.published_count;
        }
        apn.composition.receive.used.consumed_count = 0U;
        {
            const bool wire_consumed =
                appliance_production_network_wire_boot_input(
                    (const uint8_t *)rx_frame.data,
                    rx_frame.length, now_ms);
            bool lwip_accepted = false;

            if (!wire_consumed) {
                lwip_accepted =
                    appliance_production_network_input(
                        apn_network(), (const uint8_t *)rx_frame.data,
                        rx_frame.length, now_ms) ==
                    APPLIANCE_PRODUCTION_NETWORK_INPUT_ACCEPTED;
            }
            if (appliance_production_network_rx_frame_received(
                    wire_consumed, lwip_accepted)) {
                ++apn.rx_frames;
            } else {
                ++apn.rx_dropped;
            }
        }
        if (!apn_repost_rx(rx_frame.buffer_index)) {
            /* The buffer that could not be posted back stays out of the pool;
               keep the module invariant honest for the next completion. */
            ++apn.composition.receive.available.published_count;
            return;
        }
    }
}

static bool apn_setup_queues(uint8_t bus, uint8_t device,
                             uint8_t function,
                             const struct virtio_pci_capability_parse_result
                                 *capabilities,
                             const struct pci_device_header *header)
{
    uint32_t index;
    struct virtio_queue_layout_policy layout_policy;

    (void)bus;
    (void)device;
    (void)function;
    for (index = 0U; index < 4U; ++index) {
        const struct virtio_pci_capability_descriptor *descriptor =
            &capabilities->descriptors[index];
        const struct pci_bar *bar;
        uint64_t physical_base;

        if (descriptor->bar >= APN_PCI_BAR_COUNT) {
            return false;
        }
        bar = &header->bars[descriptor->bar];
        if (!bar->present ||
            bar->base > UINT64_MAX - descriptor->offset) {
            return false;
        }
        physical_base = bar->base + descriptor->offset;
        if (!apn_map_mmio(physical_base, 4096U)) {
            return false;
        }
        switch (descriptor->type) {
        case VIRTIO_PCI_CAPABILITY_TYPE_COMMON:
            apn.common_base = apn_phys(physical_base);
            break;
        case VIRTIO_PCI_CAPABILITY_TYPE_NOTIFY:
            apn.notify_base = apn_phys(physical_base);
            apn.notify_multiplier =
                descriptor->notify_offset_multiplier;
            break;
        case VIRTIO_PCI_CAPABILITY_TYPE_ISR:
            apn.isr_base = apn_phys(physical_base);
            break;
        case VIRTIO_PCI_CAPABILITY_TYPE_DEVICE:
            apn.device_base = apn_phys(physical_base);
            break;
        default:
            return false;
        }
    }
    if (apn.common_base == NULL || apn.notify_base == NULL ||
        apn.isr_base == NULL || apn.device_base == NULL) {
        return false;
    }

    apn.rx_region_physical =
        pmm_alloc_contiguous_pages(1U);
    apn.tx_region_physical =
        pmm_alloc_contiguous_pages(1U);
    apn.payload_physical =
        pmm_alloc_contiguous_pages(APN_PAYLOAD_PAGES);
    if (apn.rx_region_physical == 0U ||
        apn.tx_region_physical == 0U ||
        apn.payload_physical == 0U) {
        return false;
    }
    apn.queues[0].physical = apn.rx_region_physical;
    apn.queues[0].virtual_address = apn_phys(apn.rx_region_physical);
    apn.queues[1].physical = apn.tx_region_physical;
    apn.queues[1].virtual_address = apn_phys(apn.tx_region_physical);
    apn.payload_virtual = apn_phys(apn.payload_physical);
    apn_zero((void *)apn.queues[0].virtual_address,
             APN_QUEUE_REGION_LENGTH);
    apn_zero((void *)apn.queues[1].virtual_address,
             APN_QUEUE_REGION_LENGTH);
    apn_zero((void *)apn.payload_virtual, (size_t)APN_PAYLOAD_LENGTH);

    layout_policy.queue_size = APN_QUEUE_SIZE;
    layout_policy.event_index = false;
    for (index = 0U; index < 2U; ++index) {
        if (virtio_queue_layout_calculate(
                &layout_policy, apn.queues[index].physical,
                APN_QUEUE_REGION_LENGTH,
                &apn.queues[index].layout) != VIRTIO_QUEUE_LAYOUT_OK) {
            return false;
        }
        apn.queues[index].available =
            (struct virtio_available_ring *)(uintptr_t)(
                apn.queues[index].virtual_address +
                apn.queues[index].layout.available.offset);
        apn.queues[index].used =
            (struct virtio_used_ring *)(uintptr_t)(
                apn.queues[index].virtual_address +
                apn.queues[index].layout.used.offset);
        apn.queues[index].descriptors =
            (volatile struct apn_virtq_desc *)(uintptr_t)(
                apn.queues[index].virtual_address +
                apn.queues[index].layout.descriptor.offset);
    }
    if (!apn_program_queue(0U) || !apn_program_queue(1U)) {
        return false;
    }
    apn.stage = 4U;
    return true;
}

static bool apn_activate_virtio(
    uint8_t bus,
    uint8_t device,
    uint8_t function,
    uint16_t vendor,
    uint16_t product,
    const struct virtio_pci_capability_parse_result *capabilities,
    const struct pci_device_header *header)
{
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_identity identity;
    struct virtio_pci_selection selection;
    struct virtio_pci_device_status_io status_io;
    struct virtio_pci_feature_io feature_io;
    struct virtio_pci_feature_controller feature_controller;
    struct virtio_pci_feature_policy feature_policy;
    struct virtio_pci_feature_bitset pci_offered;
    struct virtio_net_feature_bitset offered;
    struct virtio_net_configuration net_configuration;
    struct virtio_net_topology net_topology;
    struct virtio_net_queue_config receive_config;
    struct virtio_net_queue_config transmit_config;
    struct virtio_net_rx_buffer_descriptor rx_buffers[APN_RX_COUNT];
    struct virtio_net_rx_config rx_config;
    struct virtio_net_tx_config tx_config;
    struct virtio_net_tx_notify tx_notify;
    struct virtio_net_interrupt_io interrupt_io;
    struct virtio_net_interrupt_direction rx_direction;
    struct virtio_net_interrupt_direction tx_direction;
    struct virtio_net_interrupt_publication interrupt_publication;
    struct network_device_snapshot device_snapshot;
    struct network_link_state_record link_record;
    struct virtio_net_link_readiness link_readiness;
    struct virtio_net_link_io link_io;
    uint32_t index;

    (void)vendor;
    apn_pci_read_space(bus, device, function, config);
    /* Capabilities already parsed from the same space by the caller. */
    (void)config;

    identity.version = VIRTIO_PCI_IDENTITY_VERSION;
    identity.vendor_id = vendor;
    identity.device_id = product;
    identity.bdf.bus = bus;
    identity.bdf.device = device;
    identity.bdf.function = function;
    identity.modern =
        product == VIRTIO_PCI_DEVICE_ID_NET_MODERN ? 1U : 0U;
    if (virtio_pci_selection_init(
            &selection, &identity, &capabilities->snapshot,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION) != VIRTIO_PCI_OK) {
        return false;
    }
    if (!apn_setup_queues(bus, device, function, capabilities, header)) {
        return false;
    }

    apn_zero(&status_io, sizeof(status_io));
    status_io.write = apn_status_write;
    status_io.read = apn_status_read;
    status_io.context = &apn;
    if (virtio_pci_device_status_init(
            &apn.device_status, &status_io,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
            VIRTIO_PCI_DEVICE_STATUS_OK ||
        virtio_pci_device_status_reset(&apn.device_status) !=
            VIRTIO_PCI_DEVICE_STATUS_OK ||
        virtio_pci_device_status_acknowledge(&apn.device_status) !=
            VIRTIO_PCI_DEVICE_STATUS_OK ||
        virtio_pci_device_status_driver(&apn.device_status) !=
            VIRTIO_PCI_DEVICE_STATUS_OK) {
        return false;
    }
    apn.stage = 5U;

    apn_zero(&feature_io, sizeof(feature_io));
    feature_io.read_offered = apn_feature_read_offered;
    feature_io.write_accepted = apn_feature_write_accepted;
    feature_io.read_accepted = apn_feature_read_accepted;
    feature_io.context = &apn;
    if (virtio_pci_feature_init(
            &feature_controller, &feature_io,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
        VIRTIO_PCI_FEATURE_OK) {
        return false;
    }
    feature_policy.required.low = 0U;
    feature_policy.required.high =
        UINT32_C(1) << (VIRTIO_PCI_FEATURE_VERSION_1 - 32U);
    feature_policy.accepted = feature_policy.required;
    (void)apn_feature_read_offered(
        NULL, VIRTIO_PCI_FEATURE_SELECT_LO, &offered.low);
    (void)apn_feature_read_offered(
        NULL, VIRTIO_PCI_FEATURE_SELECT_HI, &offered.high);
    if (virtio_pci_feature_negotiate(
            &feature_controller, &feature_policy) !=
        VIRTIO_PCI_FEATURE_OK) {
        return false;
    }
    if (virtio_pci_feature_observe(
            &feature_controller, &pci_offered) !=
        VIRTIO_PCI_FEATURE_OK) {
        return false;
    }
    apn.stage = 6U;

    apn_zero(&net_configuration, sizeof(net_configuration));
    net_configuration.version = VIRTIO_NET_CONFIGURATION_VERSION;
    net_configuration.mac_address[0] =
        apn_mmio_read8(apn.device_base, 0U);
    net_configuration.mac_address[1] =
        apn_mmio_read8(apn.device_base, 1U);
    net_configuration.mac_address[2] =
        apn_mmio_read8(apn.device_base, 2U);
    net_configuration.mac_address[3] =
        apn_mmio_read8(apn.device_base, 3U);
    net_configuration.mac_address[4] =
        apn_mmio_read8(apn.device_base, 4U);
    net_configuration.mac_address[5] =
        apn_mmio_read8(apn.device_base, 5U);
    net_configuration.status =
        (uint32_t)apn_mmio_read16(apn.device_base, 6U);
    apn_zero(&net_topology, sizeof(net_topology));
    net_topology.max_virtqueue_pairs = 1U;
    net_topology.receive_queue_count = 1U;
    net_topology.transmit_queue_count = 1U;
    net_topology.control_queue_present = false;
    if (virtio_net_feature_profile_activate(
            &apn.profile, &identity, &offered, &net_configuration,
            &net_topology, APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
        VIRTIO_NET_FEATURE_PROFILE_OK) {
        return false;
    }
    apn.stage = 7U;

    if (virtio_pci_device_status_features_ok(&apn.device_status) !=
        VIRTIO_PCI_DEVICE_STATUS_OK) {
        return false;
    }
    if (!apn_program_queue(0U) || !apn_program_queue(1U)) {
        return false;
    }
    if (virtio_pci_device_status_driver_ok(&apn.device_status) !=
            VIRTIO_PCI_DEVICE_STATUS_OK ||
        !virtio_pci_device_status_live(&apn.device_status)) {
        return false;
    }

    apn_zero(&receive_config, sizeof(receive_config));
    receive_config.direction = VIRTIO_NET_QUEUE_RECEIVE;
    receive_config.queue_index = 0U;
    receive_config.queue_size = APN_QUEUE_SIZE;
    receive_config.event_index = false;
    receive_config.region_base = apn.rx_region_physical;
    receive_config.region_length = APN_QUEUE_REGION_LENGTH;
    receive_config.available_ring = apn.queues[0].available;
    receive_config.used_ring = apn.queues[0].used;
    receive_config.barrier.fence = apn_barrier_fence;
    receive_config.barrier.context = &apn;
    receive_config.reclaim.reclaim = apn_queue_reclaim;
    receive_config.reclaim.context = &apn;
    receive_config.max_used_length = 2048U;
    receive_config.dma_window.base = apn.rx_region_physical;
    receive_config.dma_window.length = APN_QUEUE_REGION_LENGTH;
    receive_config.dma_window.alignment = APN_DMA_ALIGNMENT;
    receive_config.dma_zeroize.zeroize = apn_zeroize;
    receive_config.dma_zeroize.context = &apn;
    receive_config.dma_capacity = APN_DMA_CAPACITY;

    apn_zero(&transmit_config, sizeof(transmit_config));
    transmit_config.direction = VIRTIO_NET_QUEUE_TRANSMIT;
    transmit_config.queue_index = 1U;
    transmit_config.queue_size = APN_QUEUE_SIZE;
    transmit_config.event_index = false;
    transmit_config.region_base = apn.tx_region_physical;
    transmit_config.region_length = APN_QUEUE_REGION_LENGTH;
    transmit_config.available_ring = apn.queues[1].available;
    transmit_config.used_ring = apn.queues[1].used;
    transmit_config.barrier.fence = apn_barrier_fence;
    transmit_config.barrier.context = &apn;
    transmit_config.reclaim.reclaim = apn_queue_reclaim;
    transmit_config.reclaim.context = &apn;
    transmit_config.max_used_length = 2048U;
    transmit_config.dma_window.base = apn.tx_region_physical;
    transmit_config.dma_window.length = APN_QUEUE_REGION_LENGTH;
    transmit_config.dma_window.alignment = APN_DMA_ALIGNMENT;
    transmit_config.dma_zeroize.zeroize = apn_zeroize;
    transmit_config.dma_zeroize.context = &apn;
    transmit_config.dma_capacity = APN_DMA_CAPACITY;

    if (virtio_net_queue_composition_activate(
            &apn.composition, &apn.profile, &receive_config,
            &transmit_config, APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
        VIRTIO_NET_QUEUE_COMPOSITION_OK) {
        return false;
    }
    apn.stage = 8U;

    for (index = 0U; index < APN_RX_COUNT; ++index) {
        rx_buffers[index].physical_base =
            apn.payload_physical + APN_RX_BASE_OFFSET +
            index * APN_RX_BUFFER_LENGTH;
        rx_buffers[index].length = APN_RX_BUFFER_LENGTH;
    }
    apn_zero(&rx_config, sizeof(rx_config));
    rx_config.buffers = rx_buffers;
    rx_config.buffer_count = APN_RX_COUNT;
    rx_config.minimum_frame_capacity = 64U;
    rx_config.dma_window.base = apn.payload_physical;
    rx_config.dma_window.length = APN_PAYLOAD_LENGTH;
    rx_config.dma_window.alignment = APN_DMA_ALIGNMENT;
    rx_config.dma_zeroize.zeroize = apn_zeroize;
    rx_config.dma_zeroize.context = &apn;
    rx_config.dma_capacity = APN_DMA_CAPACITY;
    if (virtio_net_rx_activate(
            &apn.rx, &apn.composition, &rx_config) !=
        VIRTIO_NET_RX_OK) {
        return false;
    }
    for (index = 0U; index < APN_RX_COUNT; ++index) {
        const struct virtio_net_rx_buffer *buffer =
            virtio_net_rx_buffer_at(&apn.rx, index);

        if (buffer == NULL) {
            return false;
        }
        apn_write_descriptor(
            &apn.queues[0], buffer->descriptor.index,
            buffer->physical_base, buffer->length,
            APN_VIRTQ_DESC_F_WRITE, 0U);
    }
    /*
     * Activation published every receive buffer, and the queue kick is what
     * tells the device they are there; a device that only looks on
     * notification would otherwise never start receiving.
     */
    apn_notify_queue(
        0U,
        (uint16_t)virtio_available_publication_visible_index(
            &apn.composition.receive.available));
    apn.stage = 9U;

    for (index = 0U; index < APN_TX_COUNT; ++index) {
        apn.tx_header_physical[index] =
            apn.payload_physical + APN_TX_HEADER_BASE_OFFSET +
            index * APN_TX_HEADER_STRIDE;
        apn.tx_header_virtual[index] =
            apn.payload_virtual + APN_TX_HEADER_BASE_OFFSET +
            index * APN_TX_HEADER_STRIDE;
        apn.tx_headers[index].physical_base =
            apn.tx_header_physical[index];
        apn.tx_headers[index].length = 12U;
        apn.tx_headers[index].alias =
            (void *)apn.tx_header_virtual[index];
        apn.tx_frame_physical[index] =
            apn.payload_physical + APN_TX_FRAME_BASE_OFFSET +
            index * APN_TX_FRAME_LENGTH;
        apn.tx_frame_virtual[index] =
            apn.payload_virtual + APN_TX_FRAME_BASE_OFFSET +
            index * APN_TX_FRAME_LENGTH;
    }
    apn_zero(&tx_config, sizeof(tx_config));
    tx_config.headers = apn.tx_headers;
    tx_config.header_count = APN_TX_COUNT;
    tx_config.maximum_frame_length = APN_MAX_FRAME_LENGTH;
    tx_config.dma_window.base = apn.payload_physical;
    tx_config.dma_window.length = APN_PAYLOAD_LENGTH;
    tx_config.dma_window.alignment = APN_DMA_ALIGNMENT;
    tx_config.dma_zeroize.zeroize = apn_zeroize;
    tx_config.dma_zeroize.context = &apn;
    tx_config.dma_capacity = APN_DMA_CAPACITY;
    tx_notify.notify = apn_tx_notify;
    tx_notify.context = &apn;
    if (virtio_net_tx_activate(
            &apn.tx, &apn.composition, &tx_config, &tx_notify) !=
        VIRTIO_NET_TX_OK) {
        return false;
    }
    apn.stage = 10U;

    apn_zero(&interrupt_io, sizeof(interrupt_io));
    interrupt_io.read_status = apn_interrupt_read_status;
    interrupt_io.context = &apn;
    rx_direction.has_work = apn_interrupt_has_rx_work;
    rx_direction.context = &apn;
    tx_direction.has_work = apn_interrupt_has_tx_work;
    tx_direction.context = &apn;
    interrupt_publication.publish = apn_interrupt_publish;
    interrupt_publication.context = &apn;
    if (virtio_net_interrupt_activate(
            &apn.interrupt, &interrupt_io, &rx_direction,
            &tx_direction, &interrupt_publication,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
        VIRTIO_NET_INTERRUPT_OK) {
        return false;
    }
    apn.stage = 11U;

    apn_zero(&device_snapshot, sizeof(device_snapshot));
    device_snapshot.version = NETWORK_DEVICE_SNAPSHOT_VERSION;
    device_snapshot.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    device_snapshot.vendor_id = vendor;
    device_snapshot.product_id = product;
    device_snapshot.bus_address =
        ((uint32_t)bus << 8U) |
        ((uint32_t)device << 3U) |
        function;
    memcpy(device_snapshot.mac_address,
           net_configuration.mac_address,
           NETWORK_DEVICE_MAC_LENGTH);
    device_snapshot.capability_mask = VIRTIO_NET_LINK_CAPABILITY_MASK;
    apn_zero(&link_record, sizeof(link_record));
    link_record.version = NETWORK_LINK_STATE_VERSION;
    link_record.device_id = VIRTIO_NET_LINK_DEVICE_ID;
    link_record.state =
        (net_configuration.status & VIRTIO_NET_STATUS_LINK_UP) != 0U
        ? NETWORK_LINK_UP : NETWORK_LINK_DOWN;
    link_record.generation = APPLIANCE_PRODUCTION_NETWORK_GENERATION;
    link_readiness.profile_published = true;
    link_readiness.queue_composition_published = true;
    link_readiness.interrupt_published = true;
    link_io.read_config_status = apn_link_read_config_status;
    link_io.context = &apn;
    if (virtio_net_link_state_activate(
            &apn.link_state, &device_snapshot, &link_readiness,
            &link_io, &link_record,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION,
            APPLIANCE_PRODUCTION_NETWORK_GENERATION) !=
        VIRTIO_NET_LINK_OK) {
        return false;
    }
    apn.stage = 12U;
    return true;
}

/*
 * ITEM-492: register the drivers this image carries.  Registration order is
 * match order, and the registry refuses two drivers claiming one part, so an
 * ambiguous machine is refused rather than resolved by table order.
 */
static void apn_nic_register_backends(void)
{
    /* One list, shared with the installer's capability reading. */
    nic_backends_register_product();
}

static void apn_nic_publish_record(const char *text, size_t length)
{
    const uint64_t serial_flags = serial_sequence_begin();

    if (length == 0U) {
        return;
    }
    serial_write(text);
    serial_write("\n");
    serial_sequence_end(serial_flags);
    console_publish_journal(text, length);
}

/* Every discovered controller, one bounded record each, then the summary. */
static void apn_nic_publish(void)
{
    char record[NIC_RECORD_BYTES];
    size_t index;

    for (index = 0U; index < apn_nic_count; ++index) {
        const size_t length =
            nic_device_record(&apn_nic_devices[index], record,
                              sizeof(record));

        apn_nic_publish_record(record, length);
    }
    apn_nic_publish_record(
        record,
        nic_summary_record(apn_nic_devices, apn_nic_count, record,
                           sizeof(record)));
}

/*
 * ITEM-492: discover the machine's network controllers and describe them.
 *
 * The scan runs before any driver is asked to bind, so every device it reports
 * is SUPPORTED (a driver in this image claims it) or UNSUPPORTED (none does) -
 * and never AVAILABLE, because a driver that exists is not yet a device that
 * works.  The bind result updates the device it belongs to afterwards.
 *
 * `publish` separates the two callers: a capability reading prints the records
 * it just made, while the production path fills the table its bind result lands
 * on without printing a second copy of the reading the boot path already
 * published.
 */
static enum nic_discovery_status apn_nic_scan(bool publish)
{
    struct pci_config_io config_io;
    struct nic_device devices[NIC_DEVICES_MAX];
    enum nic_discovery_status status;
    size_t count = 0U;
    size_t index;

    apn_nic_register_backends();
    apn_nic_count = 0U;
    memset(apn_nic_devices, 0, sizeof(apn_nic_devices));
    config_io.read32 = apn_pci_read;
    config_io.write32 = apn_pci_write;
    config_io.context = &apn;
    status = nic_discovery_scan(&config_io, NULL, devices, NIC_DEVICES_MAX,
                                &count);
    apn_nic_scanned = true;
    if (status != NIC_DISCOVERY_OK) {
        /*
         * A bus that could not be walked is not an empty machine, and the
         * record says which of the two happened.  The install carries on:
         * nothing this product installs requires a network device.
         */
        if (publish) {
            static const char failed[] =
                "[NIC] devices=0 scan=FAILED reason=PCI_WALK_FAILED";

            apn_nic_publish_record(failed, sizeof(failed) - 1U);
        }
        return status;
    }
    for (index = 0U; index < count; ++index) {
        apn_nic_devices[index] = devices[index];
    }
    apn_nic_count = count;
    if (publish) {
        apn_nic_publish();
    }
    return status;
}

enum appliance_production_network_status
appliance_production_network_nic_capabilities(uint64_t hhdm_offset)
{
    apn.hhdm_offset = hhdm_offset;
    if (apn_nic_scan(true) != NIC_DISCOVERY_OK) {
        return APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE;
    }
    return apn_nic_count == 0U ? APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE
                               : APPLIANCE_PRODUCTION_NETWORK_OK;
}

/* How many controllers were described, and what the machine's network is. */
size_t appliance_production_network_nic_count(void)
{
    return apn_nic_count;
}

enum nic_state appliance_production_network_nic_state(void)
{
    return nic_discovery_overall_state(apn_nic_devices, apn_nic_count);
}

/* The summary record, for the caller that publishes one line about the NICs. */
size_t appliance_production_network_nic_summary(char *buffer, size_t size)
{
    return nic_summary_record(apn_nic_devices, apn_nic_count, buffer, size);
}

/*
 * ITEM-492: the bind result lands on the device it belongs to, so the records
 * distinguish "no driver" from "the driver refused" for the controller this
 * boot actually drove.
 */
static void apn_nic_record_bind_result(bool bound, const char *reason,
                                       const uint8_t *mac)
{
    size_t index;

    for (index = 0U; index < apn_nic_count; ++index) {
        struct nic_device *device = &apn_nic_devices[index];
        enum nic_state previous;
        bool mac_arrived;

        if (device->identity.vendor_id != apn.pci_vendor_id ||
            device->identity.device_id != apn.pci_product_id) {
            continue;
        }
        /*
         * The link retries reach this point up to a bounded number of times;
         * a record is printed only when the reading changed, so a cable-less
         * machine leaves one refusal behind instead of one per attempt.
         */
        previous = device->state;
        mac_arrived = mac != NULL && !device->mac_present;
        nic_device_set_state(
            device, nic_registry_match(device->identity.vendor_id,
                                       device->identity.device_id),
            bound, reason);
        nic_device_set_mac(device, mac);
        if (device->state != previous || mac_arrived) {
            apn_nic_publish();
        }
        return;
    }
}

static enum appliance_production_network_status
apn_boot_init_inner_result(uint64_t hhdm_offset)
{
    uint8_t bus = 0U;
    uint8_t device = 0U;
    uint8_t function = 0U;
    uint16_t vendor = 0U;
    uint16_t product = 0U;
    uint8_t config[VIRTIO_PCI_CONFIG_SPACE_MAX];
    struct virtio_pci_bar_range bars[VIRTIO_PCI_BAR_COUNT];
    struct virtio_pci_capability_parse_result capabilities;
    struct pci_device_header header;
    struct pci_config_io config_io;
    struct pci_location location;
    uint32_t index;

    memset(&apn, 0, sizeof(apn));
    apn.hhdm_offset = hhdm_offset;
    apn.initialized = true;
    (void)appliance_production_network_init(&apn.network);
    /*
     * ITEM-492: fill the table the bind result lands on.  The reading was
     * already published by the boot path (`kernel_publish_nic_capabilities`);
     * this is the same walk for the driver that is about to bind, and it stays
     * quiet until a bind changes what the machine's controllers are.
     */
    (void)apn_nic_scan(false);
    if (!apn_find_device(&bus, &device, &function, &vendor, &product)) {
        /*
         * ITEM-389: no virtio-net function, so probe the on-board Realtek
         * NIC the acceptance host carries.  A Realtek function that cannot
         * be driven is published as present-but-failed with its reason, not
         * as absent and not as ready.
         */
        if (apn_rtl_init()) {
            return apn.network.status;
        }
        apn.device_present = apn.device_present || apn.rtl.probed;
        if (apn.rtl.probed) {
            return APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED;
        }
        /*
         * ITEM-494: no virtio and no Realtek, so the second vendor gets its
         * turn.  The order is deliberate - the vendor proven on real hardware
         * is tried first - and a machine with an Intel part the claim does
         * not cover is still published as present-but-refused with the ids
         * the scan read.
         */
        if (apn_e1000_init()) {
            return apn.network.status;
        }
        apn.device_present = apn.device_present || apn.e1000.probed;
        if (apn.e1000.probed) {
            return APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED;
        }
        apn.device_present = false;
        return APPLIANCE_PRODUCTION_NETWORK_NO_DEVICE;
    }
    /*
     * ITEM-428: the device matched the virtio class, so it is present, and it
     * is named as the backend here.  What this driver has not done yet is read
     * its configuration space or its command register, so the two publication
     * flags below stay clear until each step really happened: a function whose
     * config read failed must not publish `pci_cmd=0x00000000` (which reads as
     * "the firmware left every bit clear"), and a function the driver refused
     * must not publish a live frame path.
     */
    apn.backend = APN_BACKEND_VIRTIO;
    apn.device_present = true;

    config_io.read32 = apn_pci_read;
    config_io.write32 = apn_pci_write;
    config_io.context = &apn;
    location.bus = bus;
    location.device = device;
    location.function = function;
    if (pci_device_read(&config_io, &location, &header) != PCI_OK) {
        return APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
    }
    apn.pci_vendor_id = header.vendor_id;
    apn.pci_product_id = header.device_id;
    apn.pci_revision = header.revision;
    /*
     * ITEM-428: the same command bits the Realtek path needs.  A virtio
     * function that may not master the bus cannot read its own descriptor
     * rings, and no proof may depend on the firmware having left the bit set.
     * The register was read and written here whether or not the bits stuck, so
     * the value is published either way - a register that refused the bits is
     * the finding, not a reason to hide it - and the refusal is named, so the
     * `[PRODUCTION-NET]` record says why the function was not driven.
     */
    {
        const bool mastered = apn_pci_enable_bus_master(
            bus, device, function, &apn.virtio_pci_command);

        apn.virtio_pci_command_valid = true;
        if (!mastered) {
            apn.virtio_reason = "PCI_BUS_MASTER_REFUSED";
            apn_nic_record_bind_result(false, apn.virtio_reason, NULL);
            return APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED;
        }
    }
    apn_pci_read_space(bus, device, function, config);
    apn_zero(bars, sizeof(bars));
    for (index = 0U; index < VIRTIO_PCI_BAR_COUNT; ++index) {
        bars[index].present = header.bars[index].present;
        bars[index].base = header.bars[index].base;
        bars[index].length = header.bars[index].length;
    }
    if (virtio_pci_capability_parse(
            config, sizeof(config), bars, &capabilities) !=
            VIRTIO_PCI_CAPABILITY_OK ||
        virtio_pci_capability_result_validate(&capabilities) !=
            VIRTIO_PCI_CAPABILITY_OK) {
        apn.virtio_reason = "CAPABILITY_INVALID";
        apn_nic_record_bind_result(false, apn.virtio_reason, NULL);
        return APPLIANCE_PRODUCTION_NETWORK_INVALID_INPUT;
    }
    if (!apn_activate_virtio(
            bus, device, function, vendor, product, &capabilities,
            &header)) {
        apn.virtio_reason = "ACTIVATE_FAILED";
        apn_nic_record_bind_result(false, apn.virtio_reason, NULL);
        return APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED;
    }

    apn.bind_inputs.link_state = &apn.link_state;
    apn.bind_inputs.profile = &apn.profile;
    apn.bind_inputs.ipv4_config = NULL;
    apn.bind_inputs.port_policy = NULL;
    apn.bind_inputs.output.transmit = apn_transmit;
    apn.bind_inputs.output.context = &apn;
    apn.stage = 13U;
    if (appliance_production_network_bind(
            &apn.network, &apn.bind_inputs) !=
        APPLIANCE_PRODUCTION_NETWORK_OK &&
        !appliance_production_network_bound(&apn.network)) {
        apn.virtio_reason = "BIND_FAILED";
        apn_nic_record_bind_result(false, apn.virtio_reason, NULL);
        return APPLIANCE_PRODUCTION_NETWORK_BIND_FAILED;
    }
    /*
     * ITEM-428: the frame path exists only now.  A virtio function that was
     * refused before this point must not publish frame counters as if the
     * driver owned it.
     */
    apn.virtio_bound = true;
    apn.stage = 100U;
    apn_nic_record_bind_result(true, NULL, NULL);
    return apn.network.status;
}

/*
 * virtio_net_rx_activate snapshots the full available ring (64 KiB) on the
 * stack. Run the production bind on a dedicated bounded stack so the ordinary
 * boot stack is never overflowed by that frozen module contract.
 */
static uint8_t apn_boot_stack[131072U] __attribute__((aligned(16)));
static enum appliance_production_network_status apn_boot_result;

static void apn_boot_init_inner(uint64_t hhdm_offset)
{
    apn_boot_result = apn_boot_init_inner_result(hhdm_offset);
}

enum appliance_production_network_status
appliance_production_network_boot_init(uint64_t hhdm_offset)
{
    uint64_t *stack_top = (uint64_t *)(
        (uintptr_t)apn_boot_stack + sizeof(apn_boot_stack));
    uint64_t old_rsp;

    apn_boot_result = APPLIANCE_PRODUCTION_NETWORK_BAD_STATE;
    __asm__ volatile (
        "mov %%rsp, %0\n\t"
        "mov %1, %%rsp\n\t"
        "call *%2\n\t"
        "mov %0, %%rsp\n\t"
        : "=&r"(old_rsp)
        : "r"(stack_top), "r"(apn_boot_init_inner), "D"(hhdm_offset)
        : "rax", "rcx", "rdx", "rsi", "r8", "r9", "r10", "r11",
          "memory", "cc");
    return apn_boot_result;
}

void appliance_production_network_boot_poll(uint32_t now_ms)
{
    if (!apn.initialized || !apn.device_present) {
        return;
    }
    /*
     * ITEM-486 (round 24): the receive pass's own time budget is opened where
     * the ring is drained (`apn_service_budget_open()`), not here: this poll
     * runs the transmit path and the link work before it reaches the ring, and
     * a deadline opened ahead of that work is already spent when the first
     * frame is offered.
     */
    if (apn.backend == APN_BACKEND_RTL8168) {
        apn_rtl_poll(now_ms);
        return;
    }
    if (apn.backend == APN_BACKEND_E1000) {
        apn_e1000_poll(now_ms);
        return;
    }
    if (apn.backend != APN_BACKEND_VIRTIO) {
        return;
    }
    (void)apn_tx_recycle();
    apn_poll_rx(now_ms);
}

enum appliance_production_network_readiness
appliance_production_network_boot_readiness(void)
{
    if (!apn.initialized) {
        return APPLIANCE_PRODUCTION_NETWORK_UNAVAILABLE;
    }
    return appliance_production_network_readiness(&apn.network);
}

const struct appliance_production_network *
appliance_production_network_boot_network(void)
{
    return apn.initialized ? &apn.network : NULL;
}

bool appliance_production_network_boot_device_present(void)
{
    return apn.initialized && apn.device_present;
}

uint32_t appliance_production_network_boot_stage(void)
{
    return apn.stage;
}

/*
 * ITEM-389: the honest boot record of the production network backend.  The
 * backend name, the PCI identity that was actually observed, the device's own
 * link signal and a short reason for a device that is present but not usable.
 */
const char *appliance_production_network_boot_backend(void)
{
    switch (apn.backend) {
    case APN_BACKEND_VIRTIO:
        return "VIRTIO";
    case APN_BACKEND_RTL8168:
        return "RTL8168";
    case APN_BACKEND_E1000:
        /* ITEM-494: the second vendor's name on the same record shape. */
        return "E1000";
    default:
        /*
         * A probed but undriven part has no live backend: the boot record
         * names it under device=UNSUPPORTED and pci=, so backend=NONE keeps
         * the field's meaning ("the live backend") exact.
         */
        return "NONE";
    }
}

const char *appliance_production_network_boot_device_state(void)
{
    if (apn.backend == APN_BACKEND_RTL8168 ||
        apn.backend == APN_BACKEND_E1000) {
        return "PRESENT";
    }
    if (!apn.device_present) {
        return "ABSENT";
    }
    if (apn.backend == APN_BACKEND_VIRTIO) {
        return "PRESENT";
    }
    /*
     * ITEM-494: a probed part no driver bound is UNSUPPORTED whichever
     * vendor's scan probed it; the ids next to this field say which one.
     */
    return apn.rtl.probed || apn.e1000.probed ? "UNSUPPORTED" : "ABSENT";
}

bool appliance_production_network_boot_pci_identity(
    uint16_t *vendor_id,
    uint16_t *product_id,
    uint8_t *revision)
{
    if (vendor_id == NULL || product_id == NULL || revision == NULL) {
        return false;
    }
    if (!apn.device_present || apn.pci_vendor_id == 0U) {
        return false;
    }
    *vendor_id = apn.pci_vendor_id;
    *product_id = apn.pci_product_id;
    *revision = apn.pci_revision;
    return true;
}

bool appliance_production_network_boot_pci_command(uint32_t *command)
{
    if (command == NULL) {
        return false;
    }
    if (apn.backend == APN_BACKEND_RTL8168) {
        *command = apn.rtl.pci_command;
        return true;
    }
    if (apn.backend == APN_BACKEND_E1000) {
        /* ITEM-494: the second vendor publishes the same read-back. */
        *command = apn.e1000.pci_command;
        return true;
    }
    if (apn.backend == APN_BACKEND_VIRTIO) {
        /*
         * ITEM-428: a register this driver never read is not published.  The
         * read-back only exists once the enable helper ran, so a function
         * whose configuration space could not be read says nothing here
         * instead of `pci_cmd=0x00000000`.
         */
        if (!apn.virtio_pci_command_valid) {
            return false;
        }
        *command = apn.virtio_pci_command;
        return true;
    }
    return false;
}

size_t appliance_production_network_boot_chip_record(
    char *buffer,
    size_t capacity)
{
    if (!apn.rtl.chip_identified) {
        return 0U;
    }
    return rtl8168_chip_render_identity(&apn.rtl.chip, buffer, capacity);
}

void appliance_production_network_boot_phy_info(
    struct appliance_production_network_phy_info *info)
{
    if (info == NULL) {
        return;
    }
    memset(info, 0, sizeof(*info));
    /* An identified probe owns diagnostic evidence even if PHY preparation
     * refused before backend activation. Reporting it performs no MMIO and
     * does not activate the failed device. */
    if (apn.backend != APN_BACKEND_RTL8168 && !apn.rtl.chip_identified) {
        return;
    }
    /*
     * ITEM-590: the answer's derivation lives beside the record that publishes
     * it (appliance_production_network_phy_record.c), so the boot journal and
     * the exported report print one composition and a host proof can drive the
     * three states through the driver's own RTL8168 model.
     */
    appliance_production_network_phy_info_from_driver(
        &apn.rtl.chip, &apn.rtl.phy, &apn.rtl.boot, apn.rtl.link_attempts,
        info);
}

bool appliance_production_network_boot_link_up(void)
{
    bool link_up = false;

    if (apn.backend == APN_BACKEND_RTL8168) {
        /*
         * ITEM-583: the same BMCR/BMSR answer the bind uses.  The PHYstatus
         * byte is the fallback for parts whose PHY is not behind PHYAR, so
         * the record and the bind cannot disagree about the link.
         */
        if (rtl8168_phy_mii_supported(&apn.rtl.chip) &&
            rtl8168_phy_refresh(&apn.rtl.io, &apn.rtl.phy) == RTL8168_OK) {
            return apn.rtl.phy.link_up;
        }
        if (rtl8168_link_status(&apn.rtl.io, &link_up) != RTL8168_OK) {
            return false;
        }
        return link_up;
    }
    if (apn.backend == APN_BACKEND_E1000) {
        /*
         * ITEM-494: the 8254x link bit (`STATUS.LU`) is the same live reading
         * the Realtek path publishes, so a link that drops after the bind is
         * named on this record too.
         */
        if (e1000_link_status(&apn.e1000.io, &link_up) != E1000_OK) {
            return false;
        }
        return link_up;
    }
    if (apn.backend == APN_BACKEND_VIRTIO) {
        return virtio_net_link_state_path_enabled(&apn.link_state);
    }
    return false;
}

const char *appliance_production_network_boot_reason(void)
{
    /*
     * ITEM-428: each backend names its own refusal.  A virtio function this
     * driver would not drive (`PCI_BUS_MASTER_REFUSED`) is a stated reason
     * like the Realtek path's, not an empty field.
     */
    if (apn.backend == APN_BACKEND_VIRTIO) {
        return apn.virtio_reason;
    }
    if (apn.backend == APN_BACKEND_E1000) {
        /* ITEM-494: the second vendor names its own refusal. */
        return apn.e1000.reason;
    }
    return apn.rtl.reason;
}

void appliance_production_network_boot_io_counters(
    struct appliance_production_network_io_counters *counters)
{
    if (counters == NULL) {
        return;
    }
    memset(counters, 0, sizeof(*counters));
    /*
     * ITEM-643: a backend that does not expose receive registers names that
     * fact instead of falling through to the record's UNKNOWN fallback.
     */
    counters->rx_diag.reason = "NO_BACKEND";
    /*
     * ITEM-451: both backends feed the same lwIP adapter, so the per-class
     * frame counters are read once, from the adapter the boot record already
     * publishes the flags of.  This runs before the backend split so a machine
     * whose driver refused the part still names what its frame path saw.
     */
    (void)hfoc_lwip_proto_counters(&apn.network.proto, &counters->classes);
    if (apn.backend == APN_BACKEND_RTL8168) {
        /*
         * ITEM-428: the ring's own descriptor counts, so a run that stops
         * transmitting names which side of the ring stopped first.
         */
        counters->available = true;
        counters->tx_submitted = apn.rtl.tx.submitted;
        counters->tx_reclaimed = apn.rtl.tx.reclaimed;
        counters->tx_abandoned = apn.rtl.tx.abandoned;
        counters->tx_ring_full = apn.rtl.tx_ring_full;
        counters->tx_restarts = apn.rtl.tx_restarts;
        counters->tx_errors = apn.rtl.tx_dropped;
        counters->rx_frames = apn.rtl.rx_frames;
        counters->rx_dropped = apn.rtl.rx_dropped;
        counters->rx_errors = apn.rtl.rx_errors;
        /*
         * ITEM-435: the device's own answer to "is the receiver armed".
         * `rx_ready` is false until the bring-up has read the receive
         * descriptor base register back, so a ring the part never accepted is
         * never published as a working receive path.
         */
        counters->rx_ready = apn.rtl.rx_ready ? 1U : 0U;
        /*
         * ITEM-643: the live receive snapshot, read without touching the
         * device.  A part that was never brought up cannot answer for its
         * ring, so the record says so instead of printing zero registers.
         */
        if (apn.rtl.bring_up_done) {
            struct rtl8168_rx_diagnostic diagnostic;
            const enum rtl8168_status status = rtl8168_rx_diagnostic_read(
                &apn.rtl.io, &apn.rtl.rx, &diagnostic);

            counters->rx_diag.available =
                status == RTL8168_OK && diagnostic.available;
            counters->rx_diag.reason = diagnostic.reason;
            counters->rx_diag.chip_cmd = diagnostic.chip_cmd;
            counters->rx_diag.intr_status = diagnostic.intr_status;
            counters->rx_diag.intr_mask = diagnostic.intr_mask;
            counters->rx_diag.rx_config = diagnostic.rx_config;
            counters->rx_diag.rx_desc_low = diagnostic.rx_desc_low;
            counters->rx_diag.rx_desc_high = diagnostic.rx_desc_high;
            counters->rx_diag.misc = diagnostic.misc;
            counters->rx_diag.rx_index = diagnostic.rx_index;
            counters->rx_diag.rx_capacity = diagnostic.rx_capacity;
            counters->rx_diag.rx_in_flight = diagnostic.rx_in_flight;
            counters->rx_diag.rx_own = diagnostic.rx_own;
            counters->rx_diag.rx_head_opts1 = diagnostic.rx_head_opts1;
            counters->rx_diag.rx_head_address_low =
                diagnostic.rx_head_address_low;
            counters->rx_diag.rx_overflow_events = apn.rtl.rx_overflow;
            counters->rx_diag.rx_overflow_acks = apn.rtl.rx_overflow_acks;
        } else {
            counters->rx_diag.available = false;
            counters->rx_diag.reason = "NOT_BROUGHT_UP";
        }
        return;
    }
    if (apn.backend == APN_BACKEND_E1000) {
        /*
         * ITEM-494: the second vendor publishes the same ring counters, so a
         * run on an Intel machine names which side of the ring stopped first
         * exactly as a Realtek run does.
         */
        counters->available = apn.e1000.bring_up_done;
        if (!apn.e1000.bring_up_done) {
            return;
        }
        counters->tx_submitted = apn.e1000.tx.submitted;
        counters->tx_reclaimed = apn.e1000.tx.reclaimed;
        counters->tx_abandoned = apn.e1000.tx.abandoned;
        counters->tx_ring_full = apn.e1000.tx_ring_full;
        counters->tx_restarts = apn.e1000.tx_restarts;
        counters->tx_errors = apn.e1000.tx_dropped;
        counters->rx_frames = apn.e1000.rx_frames;
        counters->rx_dropped = apn.e1000.rx_dropped;
        counters->rx_errors = apn.e1000.rx_errors;
        counters->rx_ready = apn.e1000.rx_ready ? 1U : 0U;
        return;
    }
    if (apn.backend == APN_BACKEND_VIRTIO) {
        /*
         * The virtio path has no descriptor ring this driver owns - the frozen
         * module does - so it publishes the frame path's own cumulative counts:
         * what `apn_transmit()` submitted, what `apn_tx_recycle()` reclaimed,
         * and what the window refused.  `available` is true only once the bind
         * completed, so a function this driver refused is not reported as a
         * frame path it never owned.
         */
        counters->available = apn.virtio_bound;
        if (!apn.virtio_bound) {
            return;
        }
        counters->tx_submitted = apn.tx_frames;
        counters->tx_reclaimed = apn.tx_reclaimed;
        counters->tx_ring_full = apn.tx_ring_full;
        counters->tx_errors = apn.tx_dropped;
        counters->rx_frames = apn.rx_frames;
        counters->rx_dropped = apn.rx_dropped;
        counters->rx_errors = apn.rx_dropped;
        counters->rx_ready = apn.virtio_bound ? 1U : 0U;
    }
}

void appliance_production_network_boot_wire_view(
    struct appliance_production_network_wire_view *view)
{
    const struct appliance_network_wire *wire;
    struct appliance_network_wire_dhcp_counters counters;

    if (view == NULL) {
        return;
    }
    memset(view, 0, sizeof(*view));
    wire = appliance_production_network_wire_boot_wire();
    if (wire == NULL ||
        !appliance_network_wire_dhcp_counters(wire, &counters)) {
        return;
    }
    view->available = true;
    view->discovers_sent = counters.discovers_sent;
    view->requests_sent = counters.requests_sent;
    view->offers_received = counters.offers_received;
    view->acks_received = counters.acks_received;
    view->send_refusals = counters.send_refusals;
    /*
     * ITEM-435: the wire's own receive view.  `received` counts the DHCP/DNS
     * packets that reached the client at all and `malformed` the ones it
     * parsed and refused; both come from the wire itself, so a record whose
     * offers and acks are zero can still say whether an answer arrived.
     */
    view->received = wire->received_count;
    view->malformed = wire->malformed_count;
}

/*
 * ITEM-486 (round 24): the network pass's bounded budget, published so the
 * loop record proves the bound is live rather than inferred from a quiet link.
 */
uint32_t appliance_production_network_boot_service_budget_us(void)
{
    return APN_SERVICE_BUDGET_US;
}

uint32_t appliance_production_network_boot_service_budget_hits(void)
{
    return apn.service_budget_hits;
}

#else
typedef int appliance_production_network_x86_translation_unit_guard;
#endif
