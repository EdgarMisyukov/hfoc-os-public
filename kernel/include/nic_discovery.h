#ifndef HFOC_NIC_DISCOVERY_H
#define HFOC_NIC_DISCOVERY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "nic_registry.h"
#include "pci.h"

/*
 * ITEM-492: finding the machine's network controllers, whatever they are.
 *
 * The registry (`nic_registry.h`) knows which drivers this image carries; this
 * module walks the PCI bus for every network controller and describes each one
 * with the identity the hardware reports.  Nothing here knows a vendor: a
 * Realtek part and a part nobody wrote a driver for take the same path, and the
 * difference between them is the state the registry produces - not a branch
 * that only looks for one of them.
 *
 * A device is discovered independently of every other device, so a machine with
 * two NICs, one supported and one not, reports both instead of stopping at the
 * first one it can use.
 */

#define NIC_DISCOVERY_REASON_BIND_FAILED "BIND_FAILED"

enum nic_discovery_status {
    NIC_DISCOVERY_OK = 0,
    NIC_DISCOVERY_NULL = 1,
    /* The PCI walk failed: an unusable bus is not an empty machine. */
    NIC_DISCOVERY_PCI_ERROR = 2,
    /* More network controllers than the caller's table holds: refused whole,
     * never truncated, because a truncated list is a machine misdescribed. */
    NIC_DISCOVERY_CAPACITY = 3,
};

/*
 * What the caller can do with a device the registry claims.  The callback is
 * optional: without it the scan is a capability reading - "this image has a
 * driver for that part" - and every claimed device stays SUPPORTED instead of
 * pretending to be AVAILABLE.
 */
struct nic_discovery_ops {
    void *context;
    /*
     * Ask the driver behind `backend` to initialise this device.  True when the
     * device is usable; `mac_present` and `mac` carry the address the hardware
     * reported, and `reason` carries why a refusal happened.
     */
    bool (*bind)(void *context,
                 const struct nic_backend *backend,
                 const struct nic_identity *identity,
                 bool *mac_present,
                 uint8_t mac[NIC_MAC_BYTES],
                 const char **reason);
};

/*
 * Classify devices whose identity is already known: match every one against the
 * registry, and - when the caller supplied a bind callback - ask the matched
 * driver to initialise it.  Pure: no PCI access, no driver of its own, so a host
 * proof can drive every combination of supported, unsupported, refused and
 * unclaimed devices.
 */
void nic_discovery_classify(struct nic_device *devices,
                            size_t count,
                            const struct nic_discovery_ops *ops);
/*
 * Enumerate every PCI network controller and classify the result.  `capacity`
 * bounds the caller's table; a machine with more controllers than that is
 * refused with NIC_DISCOVERY_CAPACITY instead of being silently shortened.
 */
enum nic_discovery_status nic_discovery_scan(
    struct pci_config_io *io,
    const struct nic_discovery_ops *ops,
    struct nic_device *devices,
    size_t capacity,
    size_t *count);
const char *nic_discovery_status_string(enum nic_discovery_status status);
/*
 * The state a machine is in, from its device table: the summary a caller that
 * only needs one answer reads.  ABSENT covers "nothing was found", which is the
 * reading an installer continues past - and never a reason to stop installing.
 */
enum nic_state nic_discovery_overall_state(const struct nic_device *devices,
                                           size_t count);

#endif
