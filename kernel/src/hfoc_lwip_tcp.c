#include "hfoc_lwip_tcp.h"

/*
 * ITEM-386: the production management listener is served by this same
 * bounded listener contract.  The host-test and QEMU-proof arms stay exactly
 * as they were; the production arm adds no behaviour, it only stops the
 * translation unit from being an empty stub in the ordinary production image.
 */
#if defined(HFOC_LWIP_TCP_HOST_TEST) || \
    defined(HFOC_TCP_LISTENER_QEMU_TEST) || \
    defined(HFOC_PRODUCTION_TLS)

#include "hfoc_lwip.h"
#include "hfoc_lwip_proto.h"
#include "ipv4_config.h"

#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/priv/tcp_priv.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool hfoc_lwip_tcp_core_initialized;
/*
 * ITEM-508 increment 27: the bounded listener pool.  A NULL slot is free; a
 * published listener owns exactly one slot until its shutdown clears it.  The
 * netif, the config and the port policy stay per listener, so the pool changes
 * how many listeners may exist, never what one listener is allowed to bind.
 */
static struct hfoc_lwip_tcp_listener *
    hfoc_lwip_tcp_active[HFOC_LWIP_TCP_LISTENER_COUNT];

static bool hfoc_lwip_tcp_listener_slot_available(void)
{
    uint32_t index;

    for (index = 0U; index < HFOC_LWIP_TCP_LISTENER_COUNT; ++index) {
        if (hfoc_lwip_tcp_active[index] == NULL) {
            return true;
        }
    }
    return false;
}

static bool hfoc_lwip_tcp_listener_slot_take(
    struct hfoc_lwip_tcp_listener *listener)
{
    uint32_t index;

    for (index = 0U; index < HFOC_LWIP_TCP_LISTENER_COUNT; ++index) {
        if (hfoc_lwip_tcp_active[index] == NULL) {
            hfoc_lwip_tcp_active[index] = listener;
            return true;
        }
    }
    return false;
}

static void hfoc_lwip_tcp_listener_slot_release(
    const struct hfoc_lwip_tcp_listener *listener)
{
    uint32_t index;

    for (index = 0U; index < HFOC_LWIP_TCP_LISTENER_COUNT; ++index) {
        if (hfoc_lwip_tcp_active[index] == listener) {
            hfoc_lwip_tcp_active[index] = NULL;
            return;
        }
    }
}

static bool hfoc_lwip_tcp_generation_valid(uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_netif_live(
    const struct hfoc_lwip_netif *netif)
{
    return netif != NULL &&
           netif->published &&
           netif->live &&
           netif->version == HFOC_LWIP_NETIF_VERSION &&
           hfoc_lwip_tcp_generation_valid(netif->generation) &&
           netif->netif.mtu == HFOC_LWIP_NETIF_MTU &&
           netif->netif.flags == HFOC_LWIP_NETIF_FLAGS &&
           netif->netif.hwaddr_len == NETWORK_DEVICE_MAC_LENGTH &&
           netif->netif.name[0] == HFOC_LWIP_NETIF_NAME0 &&
           netif->netif.name[1] == HFOC_LWIP_NETIF_NAME1 &&
           netif->netif.num == HFOC_LWIP_NETIF_NUM;
}

static bool hfoc_lwip_tcp_port_policy_valid(
    const struct network_ipv4_port_policy *policy)
{
    return policy != NULL &&
           policy->version == NETWORK_IPV4_PORT_POLICY_VERSION &&
           policy->port >= NETWORK_IPV4_PORT_MIN;
}

static bool hfoc_lwip_tcp_listener_valid(
    const struct hfoc_lwip_tcp_listener *listener)
{
    return listener != NULL &&
           listener->published &&
           listener->version == HFOC_LWIP_TCP_VERSION &&
           hfoc_lwip_tcp_generation_valid(listener->generation) &&
           hfoc_lwip_tcp_netif_live(&listener->netif) &&
           network_ipv4_config_validate(&listener->config) ==
               NETWORK_IPV4_OK &&
           hfoc_lwip_tcp_port_policy_valid(&listener->port_policy) &&
           listener->backlog == HFOC_LWIP_TCP_BACKLOG &&
           listener->callback.accept != NULL;
}

static void hfoc_lwip_tcp_init_core(void)
{
    if (hfoc_lwip_tcp_core_initialized) {
        return;
    }
#if defined(HFOC_PRODUCTION_TLS)
    /*
     * ITEM-386: on ordinary boot the production NIC backend already published
     * the lwIP core, including tcp_init().  Repeating mem_init/memp_init here
     * would reset live pools, so only the TCP control-block initializer is
     * re-asserted (it is idempotent) and every other initializer is skipped.
     */
    if (hfoc_lwip_proto_core_ready()) {
        tcp_init();
        hfoc_lwip_tcp_core_initialized = true;
        return;
    }
#endif
    if (!hfoc_lwip_alloc_ready()) {
        (void)hfoc_lwip_alloc_init();
    }
    mem_init();
    memp_init();
    pbuf_init();
    netif_init();
    etharp_init();
    tcp_init();
    sys_timeouts_init();
    hfoc_lwip_tcp_core_initialized = true;
}

static err_t hfoc_lwip_tcp_lwip_accept(
    void *arg,
    struct tcp_pcb *pcb,
    err_t err)
{
    struct hfoc_lwip_tcp_listener *listener;

    listener = (struct hfoc_lwip_tcp_listener *)arg;
    if (listener == NULL ||
        !hfoc_lwip_tcp_published(listener) ||
        !hfoc_lwip_tcp_listening(listener) ||
        err != ERR_OK) {
        if (pcb != NULL) {
            tcp_abort(pcb);
        }
        return ERR_ABRT;
    }
    ++listener->accept_count;
    return listener->callback.accept(
        listener->callback.context,
        pcb,
        listener->generation);
}

enum hfoc_lwip_tcp_status hfoc_lwip_tcp_init(
    struct hfoc_lwip_tcp_listener *listener,
    const struct hfoc_lwip_netif *live_netif,
    const struct network_ipv4_config *config,
    const struct network_ipv4_port_policy *port_policy,
    const struct hfoc_lwip_tcp_callback *callback,
    uint32_t generation)
{
    if (listener == NULL ||
        live_netif == NULL ||
        config == NULL ||
        port_policy == NULL ||
        callback == NULL) {
        return HFOC_LWIP_TCP_NULL;
    }
    if (listener->published ||
        !hfoc_lwip_tcp_listener_slot_available()) {
        return HFOC_LWIP_TCP_ALREADY_PUBLISHED;
    }
    if (listener->version != 0U) {
        return HFOC_LWIP_TCP_INVALID_VERSION;
    }
    if (!hfoc_lwip_tcp_generation_valid(generation)) {
        return HFOC_LWIP_TCP_INVALID_GENERATION;
    }
    if (!hfoc_lwip_tcp_netif_live(live_netif)) {
        return HFOC_LWIP_TCP_NETIF_NOT_LIVE;
    }
    if (network_ipv4_config_validate(config) != NETWORK_IPV4_OK) {
        return HFOC_LWIP_TCP_INVALID_CONFIG;
    }
    if (!hfoc_lwip_tcp_port_policy_valid(port_policy)) {
        return HFOC_LWIP_TCP_INVALID_PORT;
    }
    if (callback->accept == NULL) {
        return HFOC_LWIP_TCP_INVALID_CALLBACK;
    }

    hfoc_lwip_tcp_init_core();
    memset(listener, 0, sizeof(*listener));
    listener->version = HFOC_LWIP_TCP_VERSION;
    listener->generation = generation;
    listener->netif = *live_netif;
    listener->config = *config;
    listener->port_policy = *port_policy;
    listener->backlog = HFOC_LWIP_TCP_BACKLOG;
    listener->callback = *callback;
    /*
     * The free slot was checked before validation and nothing here can take
     * it, so the take cannot fail; if it ever did, the listener is left
     * unpublished rather than published outside the pool.
     */
    if (!hfoc_lwip_tcp_listener_slot_take(listener)) {
        memset(listener, 0, sizeof(*listener));
        return HFOC_LWIP_TCP_ALREADY_PUBLISHED;
    }
    listener->published = true;
    return HFOC_LWIP_TCP_OK;
}

enum hfoc_lwip_tcp_status hfoc_lwip_tcp_bind(
    struct hfoc_lwip_tcp_listener *listener,
    const struct network_ipv4_port_policy *requested_policy)
{
    ip_addr_t local_address;
    struct tcp_pcb *pcb;
    err_t bind_err;

    if (listener == NULL || requested_policy == NULL) {
        return HFOC_LWIP_TCP_NULL;
    }
    if (!hfoc_lwip_tcp_listener_valid(listener)) {
        if (listener->published) {
            return HFOC_LWIP_TCP_BAD_STATE;
        }
        return HFOC_LWIP_TCP_UNPUBLISHED;
    }
    if (listener->listening) {
        return HFOC_LWIP_TCP_ALREADY_LISTENING;
    }
    if (!hfoc_lwip_tcp_port_policy_valid(requested_policy)) {
        return HFOC_LWIP_TCP_INVALID_PORT;
    }
    if (requested_policy->port != listener->port_policy.port) {
        return HFOC_LWIP_TCP_UNAUTHORIZED_PORT;
    }
    if (listener->bound || listener->pcb != NULL) {
        return HFOC_LWIP_TCP_BAD_STATE;
    }

    IP4_ADDR(
        &local_address,
        listener->config.address.octets[0],
        listener->config.address.octets[1],
        listener->config.address.octets[2],
        listener->config.address.octets[3]);
    pcb = tcp_new();
    if (pcb == NULL) {
        return HFOC_LWIP_TCP_LWIP_MEMORY;
    }
    bind_err = tcp_bind(pcb, &local_address, listener->port_policy.port);
    if (bind_err != ERR_OK) {
        tcp_abort(pcb);
        if (bind_err == ERR_USE) {
            return HFOC_LWIP_TCP_PORT_IN_USE;
        }
        return HFOC_LWIP_TCP_LWIP_BIND_FAILED;
    }

    listener->pcb = pcb;
    listener->bound = true;
    return HFOC_LWIP_TCP_OK;
}

enum hfoc_lwip_tcp_status hfoc_lwip_tcp_listen(
    struct hfoc_lwip_tcp_listener *listener)
{
    struct tcp_pcb *listen_pcb;
    err_t listen_err;

    if (listener == NULL) {
        return HFOC_LWIP_TCP_NULL;
    }
    if (!hfoc_lwip_tcp_listener_valid(listener)) {
        if (listener->published) {
            return HFOC_LWIP_TCP_BAD_STATE;
        }
        return HFOC_LWIP_TCP_UNPUBLISHED;
    }
    if (listener->listening) {
        return HFOC_LWIP_TCP_ALREADY_LISTENING;
    }
    if (!listener->bound || listener->pcb == NULL) {
        return HFOC_LWIP_TCP_NOT_BOUND;
    }

    listen_pcb = tcp_listen_with_backlog_and_err(
        listener->pcb,
        (u8_t)HFOC_LWIP_TCP_BACKLOG,
        &listen_err);
    if (listen_pcb == NULL) {
        tcp_abort(listener->pcb);
        listener->pcb = NULL;
        listener->bound = false;
        if (listen_err == ERR_MEM) {
            return HFOC_LWIP_TCP_LWIP_MEMORY;
        }
        if (listen_err == ERR_USE) {
            return HFOC_LWIP_TCP_PORT_IN_USE;
        }
        return HFOC_LWIP_TCP_LWIP_LISTEN_FAILED;
    }

    listener->pcb = listen_pcb;
    tcp_arg(listener->pcb, listener);
    tcp_accept(listener->pcb, hfoc_lwip_tcp_lwip_accept);
    listener->listening = true;
    return HFOC_LWIP_TCP_OK;
}

enum hfoc_lwip_tcp_status hfoc_lwip_tcp_shutdown(
    struct hfoc_lwip_tcp_listener *listener)
{
    if (listener == NULL) {
        return HFOC_LWIP_TCP_NULL;
    }
    if (!hfoc_lwip_tcp_listener_valid(listener)) {
        if (listener->published) {
            return HFOC_LWIP_TCP_BAD_STATE;
        }
        return HFOC_LWIP_TCP_UNPUBLISHED;
    }
    if (listener->pcb != NULL) {
        if (listener->listening) {
            if (tcp_close(listener->pcb) != ERR_OK) {
                tcp_abort(listener->pcb);
            }
        } else {
            tcp_abort(listener->pcb);
        }
    }
    hfoc_lwip_tcp_listener_slot_release(listener);
    memset(listener, 0, sizeof(*listener));
    return HFOC_LWIP_TCP_OK;
}

bool hfoc_lwip_tcp_published(
    const struct hfoc_lwip_tcp_listener *listener)
{
    return hfoc_lwip_tcp_listener_valid(listener);
}

bool hfoc_lwip_tcp_bound(
    const struct hfoc_lwip_tcp_listener *listener)
{
    return hfoc_lwip_tcp_listener_valid(listener) &&
           listener->bound &&
           listener->pcb != NULL;
}

bool hfoc_lwip_tcp_listening(
    const struct hfoc_lwip_tcp_listener *listener)
{
    return hfoc_lwip_tcp_listener_valid(listener) &&
           listener->bound &&
           listener->listening &&
           listener->pcb != NULL &&
           listener->pcb->state == LISTEN;
}

uint32_t hfoc_lwip_tcp_generation(
    const struct hfoc_lwip_tcp_listener *listener)
{
    return hfoc_lwip_tcp_listener_valid(listener)
        ? listener->generation
        : 0U;
}

const char *hfoc_lwip_tcp_status_string(
    enum hfoc_lwip_tcp_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_OK:
        return "HFOC_LWIP_TCP_OK";
    case HFOC_LWIP_TCP_NULL:
        return "HFOC_LWIP_TCP_NULL";
    case HFOC_LWIP_TCP_BAD_STATE:
        return "HFOC_LWIP_TCP_BAD_STATE";
    case HFOC_LWIP_TCP_INVALID_VERSION:
        return "HFOC_LWIP_TCP_INVALID_VERSION";
    case HFOC_LWIP_TCP_ALREADY_PUBLISHED:
        return "HFOC_LWIP_TCP_ALREADY_PUBLISHED";
    case HFOC_LWIP_TCP_UNPUBLISHED:
        return "HFOC_LWIP_TCP_UNPUBLISHED";
    case HFOC_LWIP_TCP_INVALID_GENERATION:
        return "HFOC_LWIP_TCP_INVALID_GENERATION";
    case HFOC_LWIP_TCP_NETIF_NOT_LIVE:
        return "HFOC_LWIP_TCP_NETIF_NOT_LIVE";
    case HFOC_LWIP_TCP_INVALID_CONFIG:
        return "HFOC_LWIP_TCP_INVALID_CONFIG";
    case HFOC_LWIP_TCP_INVALID_PORT:
        return "HFOC_LWIP_TCP_INVALID_PORT";
    case HFOC_LWIP_TCP_INVALID_CALLBACK:
        return "HFOC_LWIP_TCP_INVALID_CALLBACK";
    case HFOC_LWIP_TCP_INVALID_BACKLOG:
        return "HFOC_LWIP_TCP_INVALID_BACKLOG";
    case HFOC_LWIP_TCP_NOT_BOUND:
        return "HFOC_LWIP_TCP_NOT_BOUND";
    case HFOC_LWIP_TCP_ALREADY_LISTENING:
        return "HFOC_LWIP_TCP_ALREADY_LISTENING";
    case HFOC_LWIP_TCP_UNAUTHORIZED_PORT:
        return "HFOC_LWIP_TCP_UNAUTHORIZED_PORT";
    case HFOC_LWIP_TCP_PORT_IN_USE:
        return "HFOC_LWIP_TCP_PORT_IN_USE";
    case HFOC_LWIP_TCP_LWIP_MEMORY:
        return "HFOC_LWIP_TCP_LWIP_MEMORY";
    case HFOC_LWIP_TCP_LWIP_BIND_FAILED:
        return "HFOC_LWIP_TCP_LWIP_BIND_FAILED";
    case HFOC_LWIP_TCP_LWIP_LISTEN_FAILED:
        return "HFOC_LWIP_TCP_LWIP_LISTEN_FAILED";
    case HFOC_LWIP_TCP_NOT_LISTENING:
        return "HFOC_LWIP_TCP_NOT_LISTENING";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_not_compiled;

#endif
