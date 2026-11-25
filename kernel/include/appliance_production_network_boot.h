#ifndef HFOC_APPLIANCE_PRODUCTION_NETWORK_BOOT_H
#define HFOC_APPLIANCE_PRODUCTION_NETWORK_BOOT_H

#include "nic_registry.h"
#include "appliance_production_network.h"
/* ITEM-592: the PHY evidence names the driver's own cold-boot stage set. */
#include "rtl8168.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Ordinary-boot production NIC owner. Discovers exactly one production NIC -
 * the virtio-net PCI function first, then the on-board Realtek RTL8168 the
 * acceptance host carries - binds it through the reused modules, publishes the
 * lwIP netif/protocol composition, and exposes an honest readiness. No device
 * leaves readiness UNAVAILABLE; a present device whose link or lwIP path is
 * not usable leaves the matching DEGRADED state.
 */
enum appliance_production_network_status
appliance_production_network_boot_init(uint64_t hhdm_offset);
/*
 * ITEM-492: the machine's network controllers, described before any driver is
 * asked to bind one.  Every PCI network controller is reported - the ones this
 * image carries a driver for, the ones nobody wrote one for and the ones whose
 * driver refused - so a caller (the installer among them) reads the machine's
 * NICs from one interface instead of asking a vendor-specific driver whether it
 * is there.  The scan also publishes one `[NIC]` record per controller and the
 * summary; a machine with no controller is reported as such, never as an error.
 */
enum appliance_production_network_status
appliance_production_network_nic_capabilities(uint64_t hhdm_offset);
size_t appliance_production_network_nic_count(void);
enum nic_state appliance_production_network_nic_state(void);
size_t appliance_production_network_nic_summary(char *buffer, size_t size);
void appliance_production_network_boot_poll(uint32_t now_ms);
enum appliance_production_network_readiness
appliance_production_network_boot_readiness(void);
const struct appliance_production_network *
appliance_production_network_boot_network(void);
bool appliance_production_network_boot_device_present(void);
uint32_t appliance_production_network_boot_stage(void);
/*
 * ITEM-389 honest backend record: the backend that is live, the identity state
 * that was actually observed, the PCI identity behind it, the device's own
 * link signal, and a short reason when a present device is not usable.
 */
const char *appliance_production_network_boot_backend(void);
const char *appliance_production_network_boot_device_state(void);
bool appliance_production_network_boot_pci_identity(
    uint16_t *vendor_id,
    uint16_t *product_id,
    uint8_t *revision);
/*
 * ITEM-428: the live backend's PCI command register as the driver left it -
 * memory space and bus mastering enabled, and the value read back after the
 * write.  The 2026-09-16 hardware run showed a part that answered every MMIO
 * register read while its transmit ring never returned a descriptor; bus
 * mastering is the one command bit that separates those two facts, so a run
 * publishes it instead of leaving it to be inferred.  Returns false when no
 * backend was probed.
 */
bool appliance_production_network_boot_pci_command(uint32_t *command);
/*
 * ITEM-399: the chip the Realtek backend identified, rendered as the two
 * fields the `[PRODUCTION-NET]` record carries - ` xid=0x2C8
 * family=RTL8168-EXTENDED`, with its leading separator - so a hardware run can
 * state which silicon it drove and not only which PCI function answered
 * (`10EC:8168` covers a dozen parts of the 8168 family: the acceptance host's
 * 8168H and the RTL8168E this product must support among them).
 *
 * Returns the number of bytes written without the terminator, or 0 when no
 * chip was identified (no device, another backend, or a part whose XID
 * register could not be read) or when the value does not fit `capacity` - a
 * truncated identity is never published as a complete one.
 */
size_t appliance_production_network_boot_chip_record(
    char *buffer,
    size_t capacity);
bool appliance_production_network_boot_link_up(void);
const char *appliance_production_network_boot_reason(void);

/*
 * ITEM-583: the PHY evidence behind the link bit.  The owner's RTL8168 rev 07
 * reported `link=DOWN` while the cable was live; the driver had never read
 * BMCR/BMSR or restarted auto-negotiation.  `available` is false for a backend
 * without the PHYAR MII path (virtio, e1000, and the later OCP Realtek parts),
 * so a missing reading is never published as zeroed PHY state.
 */
/*
 * ITEM-589: the record is printed on every RTL8168 bind, including the ones
 * where the PHY was skipped or refused - that is the case the owner's Toshiba
 * hit, and "link=DOWN" alone could not say which rule stopped the driver.
 * `available` therefore means "this backend has a Realtek part to describe",
 * not "the MII window answered": `state`, `xid`, `access` and the two status
 * fields carry what actually happened.
 */
enum appliance_production_network_phy_state {
    APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_UNAVAILABLE = 0,
    APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_CONFIGURED = 1,
    APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_REFUSED = 2,
    APPLIANCE_PRODUCTION_NETWORK_PHY_STATE_SKIPPED = 3,
};

struct appliance_production_network_phy_info {
    struct rtl8168_phy_failure failure;
    struct rtl8168_phy_page_observation page_selection;
    bool available;
    bool status_valid;
    enum appliance_production_network_phy_state state;
    bool link_up;
    bool autoneg_complete;
    bool powered_down;
    uint16_t phy_id1;
    uint16_t phy_id2;
    uint16_t bmcr;
    uint16_t bmsr;
    /* ITEM-591: `advertise` is the value the part answered for MII register 4,
       `advertise_written` the value the driver programmed and
       `advertise_valid` whether the last read of it answered at all. */
    uint16_t advertise;
    uint16_t advertise_written;
    bool advertise_valid;
    uint16_t lpa;
    /*
     * ITEM-592: the cold start's own evidence.  A link that never comes up
     * cannot be localised from one PHY sample: the record names the confirmed
     * profile, what each stage of the start did, the raw PHYstatus and BMCR
     * the part came up with, the successive BMSR readings (and whether link
     * or auto-negotiation were ever seen), the 1000BASE-T advertisement in
     * register 9 and the partner's in register 10, each with its own read
     * verdict, and the PHY page the part settled on.
     */
    const char *profile;
    const char *stages[RTL8168_STAGE_COUNT];
    bool boot_failed;
    const char *failed_stage;
    const char *first_failure;
    bool firmware_skipped;
    bool phy_status_initial_valid;
    bool phy_status_latest_valid;
    uint8_t phy_status_initial;
    uint8_t phy_status_latest;
    bool bmcr_initial_valid;
    uint16_t bmcr_initial;
    uint16_t page;
    bool page_valid;
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
    uint32_t link_attempts;
    uint32_t configures;
    uint32_t refreshes;
    /* ITEM-589: which silicon, how its PHY is reached, and what came of it. */
    uint16_t xid;
    const char *access;
    const char *reason;
    bool configure_attempted;
    const char *config_status;
    const char *first_error;
};

/* ITEM-590: the state's name is the record's vocabulary, so its one
   implementation lives beside the record that prints it
   (kernel/src/appliance_production_network_phy_record.c). */
const char *appliance_production_network_phy_state_string(
    enum appliance_production_network_phy_state state);

void appliance_production_network_boot_phy_info(
    struct appliance_production_network_phy_info *info);

/*
 * ITEM-428: what the live backend's frame path actually did.
 *
 * The 2026-09-16 hardware run's own log ended on
 * `[PRODUCTION-DHCP] ... attempt=11/40` and left "no lease" three causes at
 * once: the machine never transmitted, it transmitted and was never answered,
 * or it was answered and ignored the answer.  These counters separate them,
 * and the boot record publishes them on the addressing line.
 *
 * `available` is false when no backend owns a frame path (no device, or a
 * device the driver refused), in which case the zeroed counters must not be
 * read as "nothing happened".  `tx_submitted`, `tx_reclaimed` and
 * `tx_abandoned` are the transmit ring's own descriptor counts - the first is
 * what the driver handed to the device, the second what the device handed back
 * by clearing the descriptor's OWN bit, the third what a transmit restart had
 * to declare lost because the device stopped completing descriptors.
 * `tx_ring_full` counts the frames the ring refused because nothing came back,
 * `tx_restarts` the bounded recoveries that followed, `tx_errors` the frames
 * the transmit path dropped for every other reason, `rx_frames` the frames the
 * receive path fed to the stack and `rx_errors` the received descriptors it
 * would not feed (a receive error, a runt, a fragment that was not a whole
 * frame).
 */
/*
 * ITEM-643: the receive path's live register/descriptor state, read without
 * touching the device.  The 2026-11-22 Toshiba outage had link up, a static
 * address and SSH listening while every ARP/ICMP/unicast probe to the port
 * went unanswered; the cumulative counters could not say whether the frame
 * reached the MAC, whether the RX ring was armed, or whether the driver
 * still owned the descriptors.  These fields are the answer for a Realtek
 * backend, sampled at the same points as the cumulative record:
 *
 *   chip_cmd, intr_status (read without acknowledge), intr_mask, rx_config,
 *   both halves of RxDescBase, misc (the RXDV gate), the software RX index,
 *   the ring capacity/in-flight count, the descriptors the device still owns,
 *   and the first descriptor's opts1/address low half.
 *
 * `available` is false for every backend that does not expose the registers;
 * `reason` names why.  The snapshot itself is read-only: no register is
 * written and IntrStatus is not acknowledged.  The two `rx_overflow_*`
 * counters are the poll loop's own history, not a second register read: a
 * runtime acknowledge makes the live `intr_status` normally zero, so the
 * counters keep the evidence that a stall happened and whether the next
 * drained pass cleared it.
 */
struct appliance_production_network_rx_diagnostic {
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
    uint32_t rx_overflow_events;
    uint32_t rx_overflow_acks;
};

struct appliance_production_network_io_counters {
    bool available;
    uint32_t tx_submitted;
    uint32_t tx_reclaimed;
    uint32_t tx_abandoned;
    uint32_t tx_ring_full;
    uint32_t tx_restarts;
    uint32_t tx_errors;
    uint32_t rx_frames;
    uint32_t rx_dropped;
    uint32_t rx_errors;
    /*
     * ITEM-435: the receive path's own states, so the record can never again
     * show an all-zero receive line that means either "the device delivered
     * nothing" or "the device delivered frames and the stack refused every one
     * of them".
     *
     * `rx_frames` counts the frames the device delivered **and the stack
     * accepted**; `rx_dropped` counts the frames the device delivered and the
     * stack refused (a reply the wire did not consume, a frame lwIP rejected);
     * `rx_errors` counts the descriptors the driver would not turn into a
     * frame at all (receive error, runt, bad fragment, impossible length).
     * `rx_ready` is the receive ring's arming state the device itself reported
     * back: true only while the receiver is enabled and the ring's descriptor
     * base register reads back the address the driver programmed, so a ring
     * the part never accepted is a stated state instead of a silent zero.
     */
    uint32_t rx_ready;
    /*
     * ITEM-451: the frame path by traffic class, in the same structure the
     * heartbeat already prints.  `rx_frames`/`rx_dropped` say how much traffic
     * moved and how much of it the stack refused; `classes` says what it was -
     * an ARP request this machine had to answer, an ICMP echo it had to
     * answer, a TCP SYN that was the panel being opened, or unrelated LAN
     * traffic that was never addressed to this machine.  Zeroes here are a
     * statement like every other counter: no frame of that class arrived.
     */
    struct hfoc_lwip_proto_counters classes;
    /* ITEM-643: the live receive register/descriptor snapshot. */
    struct appliance_production_network_rx_diagnostic rx_diag;
};

void appliance_production_network_boot_io_counters(
    struct appliance_production_network_io_counters *counters);

/*
 * ITEM-428: the wire's own view of the DHCP exchange, for the same record.
 * `available` is false when the appliance owns no wire (the interface never
 * reached the wire owner), so the record says `discover=` only for a real
 * exchange.  `discovers_sent` and `requests_sent` count the frames the
 * transmit path accepted, `send_refusals` the DHCP frames it refused (which is
 * what a full transmit ring looks like from the wire), and `offers_received`
 * and `acks_received` the replies that actually arrived.
 */
struct appliance_production_network_wire_view {
    bool available;
    uint32_t discovers_sent;
    uint32_t requests_sent;
    uint32_t offers_received;
    uint32_t acks_received;
    uint32_t send_refusals;
    /*
     * ITEM-435: what the wire itself saw arrive, as distinct from what the
     * addressing lifecycle could use.  `received` counts the DHCP/DNS packets
     * the wire parsed off the receive path, `malformed` the ones it parsed and
     * refused (a reply the exchange did not match, a truncated option).  A
     * record with `received=0` says the frames never reached the client; a
     * record with `received>0, malformed>0` says they reached it and it
     * refused them - the two are not the same failure.
     */
    uint32_t received;
    uint32_t malformed;
};

void appliance_production_network_boot_wire_view(
    struct appliance_production_network_wire_view *view);

/*
 * ITEM-486 (round 24): the receive pass's bounded service budget.  The pass
 * stops at this many microseconds (or at the frame count, whichever comes
 * first) and returns to the main loop; `..._budget_hits` counts the passes the
 * time bound cut short, so a record can say the bound was reached rather than
 * the ring being empty.
 */
uint32_t appliance_production_network_boot_service_budget_us(void);
uint32_t appliance_production_network_boot_service_budget_hits(void);

#endif
