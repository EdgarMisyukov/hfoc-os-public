#ifndef HFOC_NIC_REGISTRY_H
#define HFOC_NIC_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ITEM-492: the network device a machine has, as a value.
 *
 * Before this item the production path asked one question - "is there a Realtek
 * part?" - and answered it inside the Realtek driver's own file.  That made the
 * acceptance host's NIC the only network device the product could describe, and
 * it made every other NIC indistinguishable from a machine with no NIC at all.
 *
 * This module is the other half: a device is described by the identity the
 * hardware reports (vendor, device, bus/device/function), by the driver that
 * claims it, and by what happened when that driver was asked to bind.  A machine
 * with no NIC, a machine with a NIC nobody wrote a driver for, and a machine
 * whose driver failed are three different readings, and none of them is a panic
 * or a fake pass.
 *
 * The registry is a value machine: backends are registered by name and by the
 * ids they claim, matching is a pure lookup, and every record is bounded.  That
 * is what lets adding an Intel, Broadcom or Marvell backend be a registration
 * and not a rewrite - and it is what a host proof can drive case by case.
 *
 * The states are the ones the product needs and no more:
 *
 *   AVAILABLE    discovered, and its driver initialised it: the machine can
 *                use it;
 *   SUPPORTED    discovered, and a registered driver claims it, but nothing has
 *                asked that driver to initialise it yet - the capability
 *                reading an installer publishes before it touches anything;
 *   UNSUPPORTED  discovered, and no registered driver claims it;
 *   ERROR        discovered, and the driver that claims it refused or failed;
 *   ABSENT       no device was found at all.  This is a reading, not a failure:
 *                an install that does not need the network continues, and the
 *                record says why the network is not there.
 */

/* Bounded: a backend table and a device table a machine can actually fill. */
#define NIC_REGISTRY_BACKENDS_MAX 8U
#define NIC_DEVICES_MAX 8U
#define NIC_MAC_BYTES 6U
#define NIC_RECORD_BYTES 320U
/* One PCI address: `00:1f.6` plus the null terminator. */
#define NIC_BDF_BYTES 8U
/* A vendor id that claims every device the vendor ships. */
#define NIC_VENDOR_ANY UINT16_C(0xFFFF)

enum nic_state {
    /*
     * Discovered and a registered backend claims it, but nothing has asked that
     * backend to initialise the device yet.  This is the installer's capability
     * reading: "this image has a driver for that part".  It is deliberately not
     * AVAILABLE - a driver that exists is not a device that works.
     */
    NIC_STATE_SUPPORTED = 0,
    /* Discovered and its driver initialised it: the machine can use it. */
    NIC_STATE_AVAILABLE = 1,
    /* Discovered, and no registered driver claims it. */
    NIC_STATE_UNSUPPORTED = 2,
    /* Discovered, and the driver that claims it refused or failed. */
    NIC_STATE_ERROR = 3,
    /* No device was found at all. */
    NIC_STATE_ABSENT = 4,
};

struct nic_identity {
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t bus;
    uint8_t device;
    uint8_t function;
};

/*
 * One driver backend, as the registry knows it: what it is called, and which
 * PCI ids it claims.  `match_any_device` is the vendor-wide claim a backend
 * makes when it drives a family rather than one part number; it is explicit so
 * a backend cannot claim a whole vendor by leaving a field at zero.
 */
struct nic_backend {
    const char *name;
    uint16_t vendor_id;
    uint16_t device_id;
    bool match_any_device;
};

/*
 * One discovered device, as the product describes it.  `driver` is the backend
 * that claimed it (NULL when none did) and `mac_present` is separate from the
 * bytes: a driver that could not read the MAC has not read it, and the record
 * says so instead of printing six zero bytes as if they were an address.
 */
struct nic_device {
    struct nic_identity identity;
    bool mac_present;
    uint8_t mac[NIC_MAC_BYTES];
    const char *driver;
    enum nic_state state;
    const char *reason;
};

/* Start an empty registry.  Registration order is match order. */
void nic_registry_reset(void);
/*
 * Register one backend.  False - and the registry unchanged - when the table is
 * full, when the name or the claim is unusable, or when the same claim is
 * already registered: two backends claiming one id is an ambiguity, and an
 * ambiguous machine must be refused rather than resolved by table order.
 */
bool nic_registry_register(const struct nic_backend *backend);
/* The backend that claims this id, or NULL when none does. */
const struct nic_backend *nic_registry_match(uint16_t vendor_id,
                                             uint16_t device_id);
size_t nic_registry_count(void);
const struct nic_backend *nic_registry_at(size_t index);
/* Fill a device's identity and its starting state from one discovery result. */
void nic_device_set_identity(struct nic_device *device,
                             const struct nic_identity *identity);
/*
 * Record what the registry knows: a backend claims this device (SUPPORTED) or
 * none does (UNSUPPORTED).  No driver has been asked to initialise anything, so
 * this can never produce AVAILABLE.
 */
void nic_device_set_capability(struct nic_device *device,
                               const struct nic_backend *backend);
/*
 * Record the outcome of asking a backend to bind.  A backend that returned
 * false leaves the device in ERROR with the reason the caller names, and a
 * device no backend claimed is UNSUPPORTED - never AVAILABLE.
 */
void nic_device_set_state(struct nic_device *device,
                          const struct nic_backend *backend,
                          bool bound,
                          const char *reason);
/*
 * ITEM-494: the station address a bound driver actually read, so a record that
 * says AVAILABLE carries the address the part reports instead of `mac=NONE`.  A
 * NULL `mac` clears the flag, and the record keeps saying NONE rather than
 * printing six zero bytes as if they were an address.
 */
void nic_device_set_mac(struct nic_device *device,
                        const uint8_t mac[NIC_MAC_BYTES]);
const char *nic_state_name(enum nic_state state);
const char *nic_state_reason(enum nic_state state);
/* True only for the state that means the machine can use this device. */
bool nic_state_is_available(enum nic_state state);
/*
 * The unified description, one bounded record:
 *
 *   [NIC] bdf=00:1f.6 vendor=10EC device=8168 driver=RTL8168 mac=... state=AVAILABLE reason=OK
 *
 * Assembled whole and refused when it does not fit the caller's buffer: half a
 * description is worse than none.
 */
size_t nic_device_record(const struct nic_device *device,
                         char *buffer,
                         size_t size);
/*
 * The whole machine as one bounded record:
 *
 *   [NIC] devices=2 available=1 unsupported=1 error=0 absent=0
 *
 * A machine with no device at all reads `devices=0 ... absent=1`, which is what
 * tells a reader the difference between "nothing was found" and "nothing was
 * looked for".
 */
size_t nic_summary_record(const struct nic_device *devices,
                          size_t count,
                          char *buffer,
                          size_t size);

#endif
