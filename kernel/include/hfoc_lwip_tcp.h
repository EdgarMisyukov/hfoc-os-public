#ifndef HFOC_LWIP_TCP_H
#define HFOC_LWIP_TCP_H

#include "hfoc_lwip_netif.h"
#include "network_ipv4_profile.h"

#include "lwip/err.h"
#include "lwip/tcp.h"

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TCP_VERSION 1U
#define HFOC_LWIP_TCP_BACKLOG 4U
/*
 * ITEM-508 increment 27: the bounded listener pool.
 *
 * Until this increment the module kept exactly one active listener in a
 * file-scope pointer, so the production serving listener on 443 held the whole
 * socket wrapper for the boot and the ACME HTTP-01 listener the item requires
 * on port 80 could not exist beside it.  The pool is the smallest honest
 * bound: slot one is the management listener, slot two is the challenge
 * listener, and a third concurrent listener is still refused with
 * `HFOC_LWIP_TCP_ALREADY_PUBLISHED` - the refusal moved from "a second
 * listener" to "one listener too many", it was not deleted.  Each slot keeps
 * its own netif, config and port policy, so every listener still binds exactly
 * the one port its policy authorizes.
 */
#define HFOC_LWIP_TCP_LISTENER_COUNT 3U
#define HFOC_LWIP_TCP_GENERATION_MIN 1U
#define HFOC_LWIP_TCP_GENERATION_MAX UINT32_C(0xFFFFFFFE)

struct hfoc_lwip_tcp_callback {
    err_t (*accept)(
        void *context,
        struct tcp_pcb *pcb,
        uint32_t generation);
    void *context;
};

struct hfoc_lwip_tcp_listener {
    bool published;
    bool bound;
    bool listening;
    uint32_t version;
    uint32_t generation;
    struct hfoc_lwip_netif netif;
    struct network_ipv4_config config;
    struct network_ipv4_port_policy port_policy;
    uint32_t backlog;
    struct hfoc_lwip_tcp_callback callback;
    struct tcp_pcb *pcb;
    uint32_t accept_count;
    uint32_t reject_count;
};

enum hfoc_lwip_tcp_status {
    HFOC_LWIP_TCP_OK = 0,
    HFOC_LWIP_TCP_NULL = 1,
    HFOC_LWIP_TCP_BAD_STATE = 2,
    HFOC_LWIP_TCP_INVALID_VERSION = 3,
    HFOC_LWIP_TCP_ALREADY_PUBLISHED = 4,
    HFOC_LWIP_TCP_UNPUBLISHED = 5,
    HFOC_LWIP_TCP_INVALID_GENERATION = 6,
    HFOC_LWIP_TCP_NETIF_NOT_LIVE = 7,
    HFOC_LWIP_TCP_INVALID_CONFIG = 8,
    HFOC_LWIP_TCP_INVALID_PORT = 9,
    HFOC_LWIP_TCP_INVALID_CALLBACK = 10,
    HFOC_LWIP_TCP_INVALID_BACKLOG = 11,
    HFOC_LWIP_TCP_NOT_BOUND = 12,
    HFOC_LWIP_TCP_ALREADY_LISTENING = 13,
    HFOC_LWIP_TCP_UNAUTHORIZED_PORT = 14,
    HFOC_LWIP_TCP_PORT_IN_USE = 15,
    HFOC_LWIP_TCP_LWIP_MEMORY = 16,
    HFOC_LWIP_TCP_LWIP_BIND_FAILED = 17,
    HFOC_LWIP_TCP_LWIP_LISTEN_FAILED = 18,
    HFOC_LWIP_TCP_NOT_LISTENING = 19,
};

enum hfoc_lwip_tcp_status hfoc_lwip_tcp_init(
    struct hfoc_lwip_tcp_listener *listener,
    const struct hfoc_lwip_netif *live_netif,
    const struct network_ipv4_config *config,
    const struct network_ipv4_port_policy *port_policy,
    const struct hfoc_lwip_tcp_callback *callback,
    uint32_t generation);
enum hfoc_lwip_tcp_status hfoc_lwip_tcp_bind(
    struct hfoc_lwip_tcp_listener *listener,
    const struct network_ipv4_port_policy *requested_policy);
enum hfoc_lwip_tcp_status hfoc_lwip_tcp_listen(
    struct hfoc_lwip_tcp_listener *listener);
enum hfoc_lwip_tcp_status hfoc_lwip_tcp_shutdown(
    struct hfoc_lwip_tcp_listener *listener);
bool hfoc_lwip_tcp_published(
    const struct hfoc_lwip_tcp_listener *listener);
bool hfoc_lwip_tcp_bound(
    const struct hfoc_lwip_tcp_listener *listener);
bool hfoc_lwip_tcp_listening(
    const struct hfoc_lwip_tcp_listener *listener);
uint32_t hfoc_lwip_tcp_generation(
    const struct hfoc_lwip_tcp_listener *listener);
const char *hfoc_lwip_tcp_status_string(
    enum hfoc_lwip_tcp_status status);

#endif
