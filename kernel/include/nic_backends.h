#ifndef HFOC_NIC_BACKENDS_H
#define HFOC_NIC_BACKENDS_H

/*
 * ITEM-492: the NIC drivers this image carries, in one place.
 *
 * Registering a backend is the whole cost of teaching this kernel a new NIC:
 * a claim on the ids it drives, a name, and its own bring-up.  Nothing in the
 * discovery, the records or the installer names a vendor - they read whatever
 * the registry matched - so the list below is the only place a vendor appears.
 *
 * The names are the record values (`driver=`), so they are part of the contract
 * and live here rather than in a caller's string literal.
 */

#define NIC_BACKEND_NAME_VIRTIO "VIRTIO-NET"
#define NIC_BACKEND_NAME_RTL8168 "RTL8168"
/* ITEM-494: the second vendor, Intel 8254x ("e1000"). */
#define NIC_BACKEND_NAME_E1000 "E1000"

/*
 * Put every backend this image carries into the registry: the virtio-net
 * function (both the transitional and the modern id), the Realtek RTL8168
 * family and the Intel 8254x part.  Idempotent: the registry is reset first,
 * and a caller may register before it matches without caring who called it
 * before.
 */
void nic_backends_register_product(void);

#endif
