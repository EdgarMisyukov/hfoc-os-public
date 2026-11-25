#ifndef HFOC_EHCI_WALK_H
#define HFOC_EHCI_WALK_H

/*
 * ITEM-556: the EHCI device walk, with its transport substituted.
 *
 * The walk that selects the boot medium is the part that failed on the owner's
 * machine every time: not the encoders, not the queue head, but the sequence
 * controller -> rate matching hub -> the devices on it -> the boot-medium guard
 * -> the next device.  A proof that only classifies port status words cannot
 * see a walk that stops one device early, and the hardware runs paid for that.
 *
 * So the walk lives here, as production code, over the smallest transport that
 * can drive it:
 *
 *   * `control` - one control transfer to an addressed device;
 *   * `root_port_read`/`root_port_reset` - one root port's PORTSC;
 *   * `hub_port_bring_up` - power, detect and reset one downstream hub port,
 *     carrying the hub's own control-pipe size so the class requests are sent
 *     to the pipe the hub reported;
 *   * `device_begin` - the device the walk is about to hand to the storage
 *     stack: its address, control-pipe size, and where it hangs;
 *   * `attach_storage` - read its configuration, select it and bind the mass
 *     storage core (the driver's existing code);
 *   * `verify_medium` - the boot-medium guard, which also binds the reserved
 *     journal when it accepts the device.
 *   * `delay_us` - the recovery interval the walk owes a device after a reset
 *     and after SET_ADDRESS (USB 2.0 sections 7.1.7.5 and 9.2.6.3).
 *
 * The driver (kernel/src/ehci.c) passes adapters over its own hardware; a host
 * test passes a scripted model of controller -> hub -> devices and asserts what
 * the walk actually did.  The walk never touches MMIO itself.
 *
 * ITEM-557: the two recovery intervals are walk policy, not driver detail, so
 * the host proof can execute them.  A device that was just reset needs 10 ms
 * before the first SETUP to address 0, and a device that was just given an
 * address needs 2 ms before the next transfer; a walk without those waits
 * fails the very first descriptor read on real hardware - which is what the
 * machine's earlier `DEVICE8`/`DEVICE18` refusals looked like - while QEMU,
 * whose virtual devices answer immediately, never shows it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ehci.h"

/* The waits the walk owes a device it has just reset, and a device it has just
   addressed (USB 2.0: TRSTRCY is 10 ms, the SET_ADDRESS recovery interval is
   2 ms). */
#define EHCI_WALK_RESET_RECOVERY_US 10000U
#define EHCI_WALK_ADDRESS_RECOVERY_US 2000U

/*
 * ITEM-562: what a hub port bring-up actually found.  The walk may not treat an
 * empty port as a refusal - the owner's laptop has a rate matching hub with
 * ports nothing is plugged into, and a hub whose ports are empty is a normal
 * machine, not a fault - but it must treat a port that carried a device and
 * would not reset as one, by name.
 */
enum ehci_walk_port_state {
    EHCI_WALK_PORT_EMPTY = 0,
    EHCI_WALK_PORT_UP,
    EHCI_WALK_PORT_REFUSED
};

struct ehci_walk_ops {
    void *context;
    bool (*control)(void *context,
                    uint8_t address,
                    uint16_t max_packet,
                    uint8_t request_type,
                    uint8_t request,
                    uint16_t value,
                    uint16_t index,
                    uint16_t length,
                    bool data_in,
                    const void *out_data,
                    void *in_data,
                    uint32_t *moved);
    bool (*root_port_read)(void *context, uint32_t port, uint32_t *portsc);
    bool (*root_port_reset)(void *context, uint32_t port, uint32_t *portsc);
    /* ITEM-570: a downstream port's raw wPortStatus/wPortChange and the verdict
       they produced travel back with the state, so a refusal can be checked
       against the words the hub actually sent, not against a string. */
    enum ehci_walk_port_state (*hub_port_bring_up)(void *context,
                                                   uint8_t hub_address,
                                                   uint16_t hub_max_packet,
                                                   uint8_t port,
                                                   uint8_t power_good_ms,
                                                   uint16_t *status,
                                                   uint16_t *change,
                                                   const char **verdict);
    /* ITEM-582: the root port the device (or the hub it hangs behind) sits on
       travels with the device, so a configuration refusal names the port the
       owner can see instead of only the address the driver assigned. */
    bool (*device_begin)(void *context,
                         uint8_t address,
                         uint16_t max_packet,
                         uint32_t root_port,
                         uint8_t hub_address,
                         uint8_t hub_port,
                         uint16_t vendor_id,
                         uint16_t product_id);
    bool (*attach_storage)(void *context);
    bool (*verify_medium)(void *context);
    /* ITEM-582: skip a root port the caller already knows is serving a working
       device.  Optional: the installer's boot walk leaves it NULL and examines
       every port; the installed system's rescan (the operator plugged the
       diagnostic stick in after boot) sets it so the device already attached
       is never reset by the search. */
    bool (*port_skip)(void *context, uint32_t port);
    /* ITEM-582: a downstream port whose device is already bound (and must not
       be reset).  Optional: a walk that does not recognise a device skips
       nothing.  The root port is still walked, so another downstream port of
       the same hub is still searched. */
    bool (*hub_port_skip)(void *context, uint32_t root_port,
                          uint8_t hub_address, uint8_t hub_port);
    /* Busy-wait `microseconds`.  Required: the recovery intervals above are
       part of the walk's correctness, not an optional courtesy. */
    void (*delay_us)(void *context, uint32_t microseconds);
    /* ITEM-558: the transport's own text for the transfer that just failed, or
       NULL when it has none.  Optional: the walk records it beside the stage,
       so a caller can tell "the first descriptor read failed" from "it failed
       with XACTERR on descriptor 0x...". */
    const char *(*last_failure)(void *context);
};

/*
 * ITEM-562: where a port's path actually ended.  The hardware runs reported
 * `DESCRIPTOR_FAILED/NO_DEVICE` for a machine whose walk never reached a
 * descriptor: a connected root port that would not enable, a hub port that
 * would not come up, a device that would not attach and a device the boot-medium
 * guard rejected all ended their port silently, and the last one to end was
 * replaced by a single fallback string.  Each outcome below has its own name so
 * a port that failed at a reset is never reported as a descriptor failure, and a
 * machine with no device on any port is never reported as a transfer fault.
 */
enum ehci_walk_outcome {
    EHCI_WALK_OUTCOME_NONE = 0,
    /* No root port reported a connected device: nothing was plugged in. */
    EHCI_WALK_OUTCOME_NO_PORT,
    /* A connected root port was reset and did not enable (companion, low
       speed, or a reset the controller never completed). */
    EHCI_WALK_OUTCOME_PORT_RESET,
    /* A hub port carried a device and would not bring it up. */
    EHCI_WALK_OUTCOME_HUB,
    /* The device answered its port but refused a descriptor/address stage. */
    EHCI_WALK_OUTCOME_TRANSFER,
    /* The device answered its descriptors and would not attach to storage. */
    EHCI_WALK_OUTCOME_ATTACH,
    /* The device attached and the boot-medium guard refused it as the medium:
       a card reader, or another stick.  This is not a fault. */
    EHCI_WALK_OUTCOME_MEDIUM,
    /* Ports carried devices, none of them became storage: a hub with empty
       ports, a keyboard, a device that is not mass storage. */
    EHCI_WALK_OUTCOME_NO_DEVICE,
    EHCI_WALK_OUTCOME_OK
};

/* One connected port's path, in walk order. */
struct ehci_walk_port {
    uint32_t port;       /* the root port number */
    uint32_t portsc;     /* its status word, or 0 for a port behind a hub */
    uint8_t hub_address; /* the hub the device hung on, 0 for a root port */
    uint8_t hub_port;    /* the hub's port, 0 for a root port */
    /* ITEM-570: a downstream port's raw words and the speed verdict, kept
       beside the outcome so the next refusal is read from the hub's own
       status word and not from a name applied to it. */
    uint16_t hub_status;
    uint16_t hub_change;
    const char *hub_verdict;
    enum ehci_walk_outcome outcome;
    const char *stage;   /* the stage that stopped this port, or NULL */
    const char *detail;  /* the transport's text for the failing transfer */
};

/* How many port records the walk keeps for its caller.  A controller with more
   connected ports than this keeps the first records; the counts above stay
   exact, so a truncated record never claims to be complete. */
#define EHCI_WALK_PORT_RECORDS 8U

/* What the walk did, so its caller can publish it and a proof can assert it:
   how many devices reached the storage stack, how many the boot-medium guard
   was asked about, and which of them the guard accepted. */
struct ehci_walk_report {
    uint32_t ports_connected;
    uint32_t attached;
    uint32_t probes;
    /* 1-based index of the accepted attachment, or -1 when none was. */
    int selected;
    uint8_t selected_address;
    uint16_t selected_max_packet;
    uint8_t selected_hub_address;
    uint8_t selected_hub_port;
    /* ITEM-562: every connected port's path, in walk order, so a later port's
       failure cannot replace an earlier one in the caller's record. */
    uint32_t port_count;
    struct ehci_walk_port ports[EHCI_WALK_PORT_RECORDS];
    /* How many paths stopped before the guard accepted a device. */
    uint32_t failures;
    /* The first concrete refusal, in walk order: a reset, a hub port, a
       transfer or an attach.  A guard refusal is kept only when no port failed
       before it, and a machine with no connected port keeps NO_CONNECTED_PORT -
       so "a descriptor failed" is never printed for a machine that never
       reached a descriptor. */
    enum ehci_walk_outcome outcome;
    const char *failure;
    /* ITEM-558: the transport's text for the failing transfer (its result,
       descriptor token, bytes received and registers, where they exist), or
       NULL.  It is the primary cause the stage name stands for. */
    const char *failure_detail;
    /* Where the first refusal happened: 0/0 for a root port, else the hub. */
    uint32_t failure_port;
    uint8_t failure_hub_address;
    uint8_t failure_hub_port;
};

/* The EHCI status a walk outcome corresponds to, so the driver's verdict keeps
   the stage it actually reached.  ATTACH keeps the driver's own status: the
   attach path names it (`CONFIG_PARSE_FAILED`, `STORAGE_REFUSED`,
   `BLOCK_DEVICE_FAILED`) and the walk must not flatten it.  Returns
   EHCI_STATUS_UNKNOWN for ATTACH and for a successful walk. */
enum ehci_status ehci_walk_outcome_status(enum ehci_walk_outcome outcome);

/*
 * Walk `port_count` root ports.  A device on a root port is either a hub (whose
 * own configuration and downstream ports are walked, one level deep) or a
 * storage device.  A storage device is attached and then offered to the
 * boot-medium guard; a device the guard rejects does not end the walk - the
 * next port, or the next port of the same hub, still gets its turn.  The walk
 * stops only at the device the guard accepts.
 *
 * Addresses are allocated by the walk (the root device takes 1, every further
 * device takes the next value) and handed to the transport, so a hub and the
 * device behind it are distinct addresses on the same controller.
 */
bool ehci_walk_run(const struct ehci_walk_ops *ops,
                   uint32_t port_count,
                   struct ehci_walk_report *report);

#endif
