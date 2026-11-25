#include "nic_backends.h"

#include "e1000.h"
#include "nic_registry.h"
#include "rtl8168.h"
#include "virtio_pci_identity.h"

#include <stddef.h>

/*
 * ITEM-492: the one list of NIC drivers this image carries.
 *
 * Each entry is a claim: the ids the backend drives and the name the records
 * print.  A driver that drove a whole family registers one entry per part, or
 * one vendor-wide entry when its claim really is the vendor - and the registry
 * refuses two backends claiming the same ids, so this list cannot silently
 * become ambiguous.
 *
 * The Realtek entry is deliberately the same claim the RTL8168 driver already
 * made for itself: this item moves the claim into the registry, it does not
 * change which hardware that driver drives.
 *
 * ITEM-494 adds the Intel 8254x claim, and only the 82540EM id: the driver's
 * register map is proven for that part, and a family-wide or vendor-wide claim
 * would make the record say "supported" for parts nobody drove.  Adding a
 * related 8254x id is one entry here once the driver really drives it.
 */
void nic_backends_register_product(void)
{
    static const struct nic_backend backends[] = {
        {
            NIC_BACKEND_NAME_VIRTIO,
            (uint16_t)VIRTIO_PCI_VENDOR_ID,
            (uint16_t)VIRTIO_PCI_DEVICE_ID_NET_TRANSITIONAL,
            false,
        },
        {
            NIC_BACKEND_NAME_VIRTIO,
            (uint16_t)VIRTIO_PCI_VENDOR_ID,
            (uint16_t)VIRTIO_PCI_DEVICE_ID_NET_MODERN,
            false,
        },
        {
            NIC_BACKEND_NAME_RTL8168,
            (uint16_t)RTL8168_VENDOR_ID,
            (uint16_t)RTL8168_DEVICE_ID,
            false,
        },
        {
            NIC_BACKEND_NAME_E1000,
            (uint16_t)E1000_VENDOR_ID,
            (uint16_t)E1000_DEVICE_ID_82540EM,
            false,
        },
    };
    size_t index;

    nic_registry_reset();
    for (index = 0U; index < sizeof(backends) / sizeof(backends[0U]); ++index) {
        /*
         * A refusal here means the list above is ambiguous, which the host
         * proof and the contracted slice pin; the device then reads
         * UNSUPPORTED, which is the honest answer to "no driver claims it".
         */
        (void)nic_registry_register(&backends[index]);
    }
}
