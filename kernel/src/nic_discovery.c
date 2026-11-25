#include "nic_discovery.h"

#include <stddef.h>
#include <string.h>

/*
 * ITEM-492: the discovery half of the network description.
 *
 * Two steps, deliberately separable:
 *
 *   `nic_discovery_classify()` takes identities and answers what this image can
 *   do with them.  It is a value machine - the registry, a device table and an
 *   optional driver callback - which is what makes "unsupported", "refused" and
 *   "several devices" provable on a host;
 *
 *   `nic_discovery_scan()` asks the PCI bus for every network controller and
 *   hands the identities to the classifier.  It knows the *class* of device it
 *   is looking for and nothing about which vendor makes one.
 */

void nic_discovery_classify(struct nic_device *devices,
                            size_t count,
                            const struct nic_discovery_ops *ops)
{
    size_t index;

    if (devices == NULL) {
        return;
    }
    for (index = 0U; index < count; ++index) {
        struct nic_device *device = &devices[index];
        const struct nic_backend *backend =
            nic_registry_match(device->identity.vendor_id,
                               device->identity.device_id);

        /*
         * The registry decides first: a device nobody claims is UNSUPPORTED,
         * and stays UNSUPPORTED - a driver that does not exist cannot be asked
         * to initialise anything, and reporting it as an error would blame the
         * hardware for a missing driver.
         */
        nic_device_set_capability(device, backend);
        if (backend == NULL || ops == NULL || ops->bind == NULL) {
            continue;
        }
        {
            bool mac_present = false;
            uint8_t mac[NIC_MAC_BYTES];
            const char *reason = NIC_DISCOVERY_REASON_BIND_FAILED;
            const bool bound = ops->bind(ops->context, backend,
                                         &device->identity, &mac_present,
                                         mac, &reason);

            nic_device_set_state(device, backend, bound, reason);
            if (bound && mac_present) {
                memcpy(device->mac, mac, NIC_MAC_BYTES);
                device->mac_present = true;
            }
        }
    }
}

enum nic_discovery_status nic_discovery_scan(
    struct pci_config_io *io,
    const struct nic_discovery_ops *ops,
    struct nic_device *devices,
    size_t capacity,
    size_t *count)
{
    struct pci_device_header headers[NIC_DEVICES_MAX];
    enum pci_status status;
    size_t found = 0U;
    size_t index;

    if (io == NULL || devices == NULL || count == NULL) {
        return NIC_DISCOVERY_NULL;
    }
    if (capacity == 0U || capacity > NIC_DEVICES_MAX) {
        return NIC_DISCOVERY_CAPACITY;
    }
    *count = 0U;
    /*
     * Every subclass of the network class, and every programmed interface: the
     * machine's NIC is not required to be the Ethernet controller somebody
     * expected, and a device this image cannot drive still has to be reported.
     */
    status = pci_scan_class(io, PCI_CLASS_NETWORK, PCI_FILTER_ANY,
                            PCI_FILTER_ANY, headers, capacity, &found);
    if (status == PCI_CAPACITY) {
        return NIC_DISCOVERY_CAPACITY;
    }
    if (status != PCI_OK) {
        return NIC_DISCOVERY_PCI_ERROR;
    }
    for (index = 0U; index < found; ++index) {
        struct nic_identity identity;

        identity.vendor_id = headers[index].vendor_id;
        identity.device_id = headers[index].device_id;
        identity.bus = headers[index].location.bus;
        identity.device = headers[index].location.device;
        identity.function = headers[index].location.function;
        nic_device_set_identity(&devices[index], &identity);
    }
    *count = found;
    nic_discovery_classify(devices, found, ops);
    return NIC_DISCOVERY_OK;
}

const char *nic_discovery_status_string(enum nic_discovery_status status)
{
    switch (status) {
    case NIC_DISCOVERY_OK:
        return "OK";
    case NIC_DISCOVERY_NULL:
        return "NULL";
    case NIC_DISCOVERY_PCI_ERROR:
        return "PCI_ERROR";
    case NIC_DISCOVERY_CAPACITY:
    default:
        return "CAPACITY";
    }
}

enum nic_state nic_discovery_overall_state(const struct nic_device *devices,
                                           size_t count)
{
    size_t index;

    if (count == 0U || devices == NULL) {
        return NIC_STATE_ABSENT;
    }
    for (index = 0U; index < count; ++index) {
        if (devices[index].state == NIC_STATE_AVAILABLE) {
            return NIC_STATE_AVAILABLE;
        }
    }
    for (index = 0U; index < count; ++index) {
        if (devices[index].state == NIC_STATE_SUPPORTED) {
            return NIC_STATE_SUPPORTED;
        }
    }
    for (index = 0U; index < count; ++index) {
        if (devices[index].state == NIC_STATE_ERROR) {
            return NIC_STATE_ERROR;
        }
    }
    return NIC_STATE_UNSUPPORTED;
}
