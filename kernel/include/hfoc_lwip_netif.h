#ifndef HFOC_LWIP_NETIF_H
#define HFOC_LWIP_NETIF_H

#include "lwip/netif.h"
#include "network_backend.h"
#include "network_device.h"
#include "network_ipv4_profile.h"
#include "virtio_net_link_state.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_NETIF_VERSION 1U
#define HFOC_LWIP_NETIF_GENERATION_MIN 1U
#define HFOC_LWIP_NETIF_GENERATION_MAX UINT32_C(0xFFFFFFFE)
#define HFOC_LWIP_NETIF_MTU 1500U
#define HFOC_LWIP_NETIF_NAME0 'e'
#define HFOC_LWIP_NETIF_NAME1 't'
#define HFOC_LWIP_NETIF_NUM 0U
#define HFOC_LWIP_NETIF_FLAGS \
    (NETIF_FLAG_UP | NETIF_FLAG_BROADCAST | NETIF_FLAG_LINK_UP | \
     NETIF_FLAG_ETHARP | NETIF_FLAG_ETHERNET)

struct hfoc_lwip_netif {
    bool published;
    bool live;
    bool staged;
    uint32_t version;
    uint32_t generation;
    struct network_ipv4_config config;
    struct network_device_snapshot device_snapshot;
    struct network_link_selection link;
    struct netif netif;
};

enum hfoc_lwip_netif_status {
    HFOC_LWIP_NETIF_OK = 0,
    HFOC_LWIP_NETIF_NULL = 1,
    HFOC_LWIP_NETIF_BAD_STATE = 2,
    HFOC_LWIP_NETIF_INVALID_VERSION = 3,
    HFOC_LWIP_NETIF_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_NETIF_UNPUBLISHED = 5,
    HFOC_LWIP_NETIF_INVALID_GENERATION = 6,
    HFOC_LWIP_NETIF_PROFILE_UNPUBLISHED = 7,
    HFOC_LWIP_NETIF_INVALID_PROFILE = 8,
    HFOC_LWIP_NETIF_INVALID_CONFIG = 9,
    HFOC_LWIP_NETIF_LINK_UNPUBLISHED = 10,
    HFOC_LWIP_NETIF_DEVICE_MISMATCH = 11,
    HFOC_LWIP_NETIF_NOT_STAGED = 12,
    HFOC_LWIP_NETIF_LINK_NOT_READY = 13,
    HFOC_LWIP_NETIF_STAGE_MISMATCH = 14,
    HFOC_LWIP_NETIF_ALREADY_LIVE = 15,
    HFOC_LWIP_NETIF_NOT_LIVE = 16,
    HFOC_LWIP_NETIF_LIVE = 17,
};

enum hfoc_lwip_netif_status hfoc_lwip_netif_init(
    struct hfoc_lwip_netif *netif,
    uint32_t generation);
enum hfoc_lwip_netif_status hfoc_lwip_netif_stage(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_profile *profile,
    const struct virtio_net_link_state *link_state,
    uint32_t observed_generation);
enum hfoc_lwip_netif_status hfoc_lwip_netif_activate(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_profile *profile,
    const struct virtio_net_link_state *link_state,
    uint32_t observed_generation);
/*
 * ITEM-389 backend-neutral staging and activation.  A backend that publishes
 * no virtio feature profile (the on-board Realtek NIC) stages the netif
 * against its published network_backend_binding and the IPv4 configuration
 * directly.  Both pairs share one internal path, so the address, hwaddr and
 * link rules are identical whichever backend is live.
 */
enum hfoc_lwip_netif_status hfoc_lwip_netif_stage_backend(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_backend_binding *binding,
    uint32_t observed_generation);
enum hfoc_lwip_netif_status hfoc_lwip_netif_activate_backend(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config,
    const struct network_backend_binding *binding,
    uint32_t observed_generation);
/*
 * Rebind the published IPv4 configuration of a live netif. ITEM-362B uses
 * this to apply a validated DHCP lease without recreating the netif.
 */
enum hfoc_lwip_netif_status hfoc_lwip_netif_set_config(
    struct hfoc_lwip_netif *netif,
    const struct network_ipv4_config *config);
/*
 * ITEM-416: keep the interface up and take its address away, the state a DHCP
 * client acquires from and the state a lost lease returns to.  A configuration
 * without an address is refused by `set_config`; this is the operation that
 * means "the machine holds no address", and it is what a released lease leaves
 * behind instead of an address the machine was never given.
 */
enum hfoc_lwip_netif_status hfoc_lwip_netif_clear_address(
    struct hfoc_lwip_netif *netif);

enum hfoc_lwip_netif_status hfoc_lwip_netif_remove(
    struct hfoc_lwip_netif *netif);
enum hfoc_lwip_netif_status hfoc_lwip_netif_shutdown(
    struct hfoc_lwip_netif *netif);
bool hfoc_lwip_netif_published(
    const struct hfoc_lwip_netif *netif);
bool hfoc_lwip_netif_live(
    const struct hfoc_lwip_netif *netif);
bool hfoc_lwip_netif_staged(
    const struct hfoc_lwip_netif *netif);
/*
 * ITEM-416: live and unaddressed, which is the state a DHCP acquisition starts
 * from.  It is not a failure and not a configured interface: a caller that
 * needs an address to bind one has to wait for it, and this is the predicate
 * that says so.
 */
bool hfoc_lwip_netif_addressed(
    const struct hfoc_lwip_netif *netif);
uint32_t hfoc_lwip_netif_generation(
    const struct hfoc_lwip_netif *netif);
const struct network_ipv4_config *hfoc_lwip_netif_config(
    const struct hfoc_lwip_netif *netif);
const struct netif *hfoc_lwip_netif_lwip_netif(
    const struct hfoc_lwip_netif *netif);
const char *hfoc_lwip_netif_status_string(
    enum hfoc_lwip_netif_status status);

#endif
