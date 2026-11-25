#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

u32_t sys_now(void)
{
    return 1000U;
}

static err_t test_accept_callback(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    (void)context;
    (void)pcb;
    (void)generation;
    return ERR_OK;
}

static struct hfoc_lwip_netif make_live_netif(void)
{
    struct hfoc_lwip_netif netif;

    memset(&netif, 0, sizeof(netif));
    netif.published = true;
    netif.live = true;
    netif.version = HFOC_LWIP_NETIF_VERSION;
    netif.generation = 1U;
    netif.netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif.netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif.netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    netif.netif.hwaddr[0] = 0x52U;
    netif.netif.hwaddr[1] = 0x54U;
    netif.netif.hwaddr[2] = 0x00U;
    netif.netif.hwaddr[3] = 0x12U;
    netif.netif.hwaddr[4] = 0x34U;
    netif.netif.hwaddr[5] = 0x56U;
    netif.netif.name[0] = 'e';
    netif.netif.name[1] = 't';
    netif.netif.num = 0U;
    IP4_ADDR(&netif.netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif.netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif.netif.gw, 10U, 0U, 2U, 2U);
    return netif;
}

static struct network_ipv4_config make_config(void)
{
    struct network_ipv4_config config;

    memset(&config, 0, sizeof(config));
    config.schema_version = NETWORK_IPV4_CONFIG_VERSION;
    config.address.octets[0] = 10U;
    config.address.octets[1] = 0U;
    config.address.octets[2] = 2U;
    config.address.octets[3] = 15U;
    config.prefix_length = 24U;
    config.gateway.octets[0] = 10U;
    config.gateway.octets[1] = 0U;
    config.gateway.octets[2] = 2U;
    config.gateway.octets[3] = 2U;
    config.dns.octets[0] = 10U;
    config.dns.octets[1] = 0U;
    config.dns.octets[2] = 2U;
    config.dns.octets[3] = 3U;
    return config;
}

static struct network_ipv4_port_policy make_policy(uint16_t port)
{
    struct network_ipv4_port_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = NETWORK_IPV4_PORT_POLICY_VERSION;
    policy.port = port;
    return policy;
}

static void test_init_validation(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct network_ipv4_config config = make_config();
    struct network_ipv4_port_policy policy = make_policy(443U);
    struct hfoc_lwip_tcp_callback callback;
    struct hfoc_lwip_tcp_listener listener;

    memset(&listener, 0, sizeof(listener));
    callback.accept = test_accept_callback;
    callback.context = NULL;

    CHECK(hfoc_lwip_tcp_init(
              NULL, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_NULL,
          "TCP init rejects null listener");
    CHECK(hfoc_lwip_tcp_init(
              &listener, NULL, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_NULL,
          "TCP init rejects null netif");
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, NULL, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_NULL,
          "TCP init rejects null config");
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, NULL, &callback, 1U) ==
              HFOC_LWIP_TCP_NULL,
          "TCP init rejects null port policy");
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, NULL, 1U) ==
              HFOC_LWIP_TCP_NULL,
          "TCP init rejects null callback");

    callback.accept = NULL;
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_INVALID_CALLBACK,
          "TCP init rejects null accept callback");
    callback.accept = test_accept_callback;

    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 0U) ==
              HFOC_LWIP_TCP_INVALID_GENERATION,
          "TCP init rejects zero generation");

    netif.live = false;
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_NETIF_NOT_LIVE,
          "TCP init rejects non-live netif");
    netif.live = true;

    config.schema_version = 0U;
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_INVALID_CONFIG,
          "TCP init rejects invalid config");
    config = make_config();

    policy.version = 0U;
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_INVALID_PORT,
          "TCP init rejects invalid port policy");
}

static void test_deny_default_and_authorized_listen(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct network_ipv4_config config = make_config();
    struct network_ipv4_port_policy policy = make_policy(443U);
    struct network_ipv4_port_policy unauthorized = make_policy(80U);
    struct hfoc_lwip_tcp_callback callback;
    struct hfoc_lwip_tcp_listener listener;
    struct tcp_pcb_listen *lpcb;

    memset(&listener, 0, sizeof(listener));
    callback.accept = test_accept_callback;
    callback.context = NULL;

    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy, &callback, 1U) ==
              HFOC_LWIP_TCP_OK,
          "TCP init publishes configured listener");
    CHECK(hfoc_lwip_tcp_published(&listener) &&
              hfoc_lwip_tcp_generation(&listener) == 1U &&
              !hfoc_lwip_tcp_bound(&listener) &&
              !hfoc_lwip_tcp_listening(&listener) &&
              listener.pcb == NULL &&
              listener.backlog == HFOC_LWIP_TCP_BACKLOG,
          "published listener is not bound or listening by default");

    CHECK(hfoc_lwip_tcp_listen(&listener) ==
              HFOC_LWIP_TCP_NOT_BOUND,
          "listen before bind is denied by default");
    CHECK(hfoc_lwip_tcp_bind(
              &listener, &unauthorized) ==
              HFOC_LWIP_TCP_UNAUTHORIZED_PORT &&
              !hfoc_lwip_tcp_bound(&listener) &&
              listener.pcb == NULL,
          "unauthorized port bind is rejected before lwIP allocation");

    CHECK(hfoc_lwip_tcp_bind(
              &listener, &policy) == HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_bound(&listener) &&
              !hfoc_lwip_tcp_listening(&listener) &&
              listener.pcb != NULL &&
              listener.pcb->local_port == 443U,
          "authorized port bind publishes bound lwIP PCB");

    CHECK(hfoc_lwip_tcp_listen(&listener) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_listening(&listener) &&
              hfoc_lwip_tcp_bound(&listener) &&
              listener.pcb != NULL &&
              listener.pcb->state == LISTEN,
          "authorized listener enters lwIP LISTEN state");

    lpcb = (struct tcp_pcb_listen *)listener.pcb;
    CHECK(lpcb->local_port == 443U &&
              lpcb->backlog == HFOC_LWIP_TCP_BACKLOG &&
              lpcb->callback_arg == &listener,
          "lwIP LISTEN pcb has configured port, fixed backlog, and context");

    CHECK(hfoc_lwip_tcp_shutdown(&listener) ==
              HFOC_LWIP_TCP_OK &&
              !hfoc_lwip_tcp_published(&listener) &&
              !hfoc_lwip_tcp_bound(&listener) &&
              !hfoc_lwip_tcp_listening(&listener) &&
              listener.pcb == NULL,
          "shutdown closes listener and clears published state");
}

/*
 * ITEM-508 increment 27 / ITEM-601: the listener pool is bounded and
 * per-listener.  Three listeners - the management listener, the ACME HTTP-01
 * challenge listener and the SSH endpoint - may exist together, each still
 * binds only the port its own policy authorizes, a fourth concurrent listener
 * is refused by name, and shutting one listener frees its slot for the next.
 */
static void test_listener_pool_bound(void)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct network_ipv4_config config = make_config();
    struct network_ipv4_port_policy management_policy = make_policy(443U);
    struct network_ipv4_port_policy challenge_policy = make_policy(80U);
    struct network_ipv4_port_policy ssh_policy = make_policy(22U);
    struct hfoc_lwip_tcp_callback callback;
    struct hfoc_lwip_tcp_listener first;
    struct hfoc_lwip_tcp_listener second;
    struct hfoc_lwip_tcp_listener third;
    struct hfoc_lwip_tcp_listener fourth;
    struct hfoc_lwip_tcp_listener fifth;

    memset(&first, 0, sizeof(first));
    memset(&second, 0, sizeof(second));
    memset(&third, 0, sizeof(third));
    memset(&fourth, 0, sizeof(fourth));
    memset(&fifth, 0, sizeof(fifth));
    callback.accept = test_accept_callback;
    callback.context = NULL;

    CHECK(hfoc_lwip_tcp_init(
              &first, &netif, &config, &management_policy, &callback, 1U) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_published(&first),
          "the management listener takes the first pool slot");
    CHECK(hfoc_lwip_tcp_init(
              &second, &netif, &config, &challenge_policy, &callback, 2U) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_published(&second),
          "the challenge listener takes the second pool slot");
    CHECK(hfoc_lwip_tcp_init(
              &third, &netif, &config, &ssh_policy, &callback, 3U) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_published(&third),
          "the SSH listener takes the third pool slot");
    CHECK(hfoc_lwip_tcp_init(
              &fourth, &netif, &config, &management_policy, &callback, 4U) ==
              HFOC_LWIP_TCP_ALREADY_PUBLISHED &&
              !hfoc_lwip_tcp_published(&fourth),
          "a fourth concurrent listener is refused while three slots are held");

    /*
     * The pool changes how many listeners exist, never what one listener may
     * bind: each listener still binds exactly its own policy port.
     */
    CHECK(hfoc_lwip_tcp_bind(&first, &management_policy) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_bound(&first) &&
              first.pcb != NULL &&
              first.pcb->local_port == 443U,
          "the management listener binds only the port its policy authorizes");
    CHECK(hfoc_lwip_tcp_bind(&second, &challenge_policy) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_bound(&second) &&
              second.pcb != NULL &&
              second.pcb->local_port == 80U,
          "the challenge listener binds only the port its policy authorizes");
    CHECK(hfoc_lwip_tcp_bind(&third, &ssh_policy) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_bound(&third) &&
              third.pcb != NULL &&
              third.pcb->local_port == 22U,
          "the SSH listener binds only the port its policy authorizes");
    CHECK(hfoc_lwip_tcp_bind(&second, &management_policy) ==
              HFOC_LWIP_TCP_UNAUTHORIZED_PORT,
          "a listener cannot borrow the other listener's port policy");

    CHECK(hfoc_lwip_tcp_shutdown(&first) ==
              HFOC_LWIP_TCP_OK &&
              !hfoc_lwip_tcp_published(&first),
          "shutting the first listener frees its pool slot");
    CHECK(hfoc_lwip_tcp_init(
              &fifth, &netif, &config, &management_policy, &callback, 5U) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_published(&fifth),
          "the freed slot serves the next listener");
    CHECK(hfoc_lwip_tcp_shutdown(&second) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_shutdown(&third) ==
              HFOC_LWIP_TCP_OK &&
              hfoc_lwip_tcp_shutdown(&fifth) ==
              HFOC_LWIP_TCP_OK,
          "the remaining listeners shut down and release their slots");
}

static void test_status_strings(void)
{
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_OK) != NULL,
          "TCP status string covers OK");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_NULL) != NULL,
          "TCP status string covers NULL");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_BAD_STATE) != NULL,
          "TCP status string covers bad state");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_VERSION) != NULL,
          "TCP status string covers invalid version");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_ALREADY_PUBLISHED) != NULL,
          "TCP status string covers already published");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_UNPUBLISHED) != NULL,
          "TCP status string covers unpublished");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_GENERATION) != NULL,
          "TCP status string covers invalid generation");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_NETIF_NOT_LIVE) != NULL,
          "TCP status string covers non-live netif");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_CONFIG) != NULL,
          "TCP status string covers invalid config");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_PORT) != NULL,
          "TCP status string covers invalid port");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_CALLBACK) != NULL,
          "TCP status string covers invalid callback");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_INVALID_BACKLOG) != NULL,
          "TCP status string covers invalid backlog");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_NOT_BOUND) != NULL,
          "TCP status string covers not bound");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_ALREADY_LISTENING) != NULL,
          "TCP status string covers already listening");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_UNAUTHORIZED_PORT) != NULL,
          "TCP status string covers unauthorized port");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_PORT_IN_USE) != NULL,
          "TCP status string covers port in use");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_LWIP_MEMORY) != NULL,
          "TCP status string covers lwIP memory");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_LWIP_BIND_FAILED) != NULL,
          "TCP status string covers lwIP bind failure");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_LWIP_LISTEN_FAILED) != NULL,
          "TCP status string covers lwIP listen failure");
    CHECK(hfoc_lwip_tcp_status_string(
              HFOC_LWIP_TCP_NOT_LISTENING) != NULL,
          "TCP status string covers not listening");
    CHECK(hfoc_lwip_tcp_status_string(
              (enum hfoc_lwip_tcp_status)99U) == NULL,
          "TCP status string rejects unknown value");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "host fixture starts fixed lwIP allocator");

    test_init_validation();
    test_deny_default_and_authorized_listen();
    test_listener_pool_bound();
    test_status_strings();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP listener fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP listener fixture: OK\n");
    return 0;
}

#endif
