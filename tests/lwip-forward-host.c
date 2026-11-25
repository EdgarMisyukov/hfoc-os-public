/*
 * ITEM-644: the relay's failed-backend error path, through the real lwIP.
 *
 * The production relay is compiled unchanged against the vendored lwIP stack.
 * A client is admitted, the relay dials a target whose SYN is blackholed, and
 * lwIP exhausts TCP_SYNMAXRTX and removes the target PCB with ERR_ABRT.  The
 * relay must treat that as a stack-originated failure (not as its own abort):
 * the accepted client has to be failed too, the verdict/counter must be
 * published, and the one relay slot must accept the next client instead of
 * rejecting it as BUSY.
 *
 * The second fixture covers the same invariant for a leg that disappeared:
 * a lone client older than HFOC_LWIP_FORWARD_IDLE_MS must be failed by the
 * service turn, not kept forever because the idle check requires both legs.
 */
#include "hfoc_lwip_forward.h"
#include "hfoc_lwip_timeouts.h"

#include "lwip/init.h"
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
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_FORWARD_HOST_TEST

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

static u32_t test_now_ms;

u32_t sys_now(void)
{
    return test_now_ms;
}

/*
 * The host netif has no hardware.  Every IP packet, including the relay's
 * backend SYN and its retransmissions, is counted and dropped: that is the
 * blackhole the production trial exposed.
 */
static uint32_t ip_outputs;

static err_t test_ip_output(
    struct netif *netif,
    struct pbuf *p,
    const ip4_addr_t *address)
{
    (void)netif;
    (void)address;
    ++ip_outputs;
    /*
     * A real driver copies the pbuf and leaves ownership with the TCP layer
     * until the segment is acknowledged; freeing it here would dangle the
     * unacked segment and hide the retransmission path under test.
     */
    (void)p;
    return ERR_OK;
}

static err_t test_netif_init(struct netif *netif)
{
    netif->output = test_ip_output;
    netif->linkoutput = NULL;
    return ERR_OK;
}

/*
 * The relay reads only these three predicates.  The real wrapper has its own
 * host proof (`test-lwip-netif-host`); keeping this fixture independent makes
 * the relay state machine the only thing under test here.
 */
bool hfoc_lwip_netif_published(const struct hfoc_lwip_netif *netif)
{
    return netif != NULL && netif->published;
}

bool hfoc_lwip_netif_live(const struct hfoc_lwip_netif *netif)
{
    return netif != NULL && netif->live;
}

bool hfoc_lwip_netif_staged(const struct hfoc_lwip_netif *netif)
{
    return netif != NULL && netif->staged;
}

bool hfoc_lwip_netif_addressed(const struct hfoc_lwip_netif *netif)
{
    return netif != NULL && netif->live &&
           netif->config.address.octets[0] != 0U;
}

static struct hfoc_lwip_netif netif;

static void init_lwip_core(void)
{
    if (!hfoc_lwip_alloc_ready()) {
        (void)hfoc_lwip_alloc_init();
    }
    mem_init();
    memp_init();
    pbuf_init();
    netif_init();
    sys_timeouts_init();
    tcp_init();
}

static void init_netif(void)
{
    ip4_addr_t address;
    ip4_addr_t mask;
    ip4_addr_t gateway;

    memset(&netif, 0, sizeof(netif));
    netif.published = true;
    netif.live = true;
    netif.version = HFOC_LWIP_NETIF_VERSION;
    netif.generation = 1U;
    netif.config.address.octets[0] = 10U;
    netif.config.address.octets[1] = 0U;
    netif.config.address.octets[2] = 2U;
    netif.config.address.octets[3] = 15U;
    netif.config.prefix_length = 24U;
    netif.config.gateway.octets[0] = 10U;
    netif.config.gateway.octets[1] = 0U;
    netif.config.gateway.octets[2] = 2U;
    netif.config.gateway.octets[3] = 2U;
    netif.netif.mtu = HFOC_LWIP_NETIF_MTU;
    netif.netif.flags = HFOC_LWIP_NETIF_FLAGS;
    netif.netif.hwaddr_len = NETWORK_DEVICE_MAC_LENGTH;
    netif.netif.hwaddr[0] = 0x52U;
    netif.netif.hwaddr[1] = 0x54U;
    netif.netif.hwaddr[2] = 0x00U;
    netif.netif.hwaddr[3] = 0x12U;
    netif.netif.hwaddr[4] = 0x34U;
    netif.netif.hwaddr[5] = 0x56U;
    netif.netif.name[0] = HFOC_LWIP_NETIF_NAME0;
    netif.netif.name[1] = HFOC_LWIP_NETIF_NAME1;
    netif.netif.num = HFOC_LWIP_NETIF_NUM;
    IP4_ADDR(&address, 10U, 0U, 2U, 15U);
    IP4_ADDR(&mask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&gateway, 10U, 0U, 2U, 2U);
    IP4_ADDR(&netif.netif.ip_addr, 10U, 0U, 2U, 15U);
    IP4_ADDR(&netif.netif.netmask, 255U, 255U, 255U, 0U);
    IP4_ADDR(&netif.netif.gw, 10U, 0U, 2U, 2U);
    if (netif_add(&netif.netif, &address, &mask, &gateway,
                  NULL, test_netif_init, ip_input) == NULL) {
        fprintf(stderr, "FAIL: host netif could not be added\n");
        ++failures;
        return;
    }
    netif_set_up(&netif.netif);
    netif_set_link_up(&netif.netif);
    netif_set_default(&netif.netif);
}

static struct forward_config make_config(uint16_t listen_port)
{
    struct forward_config config;

    memset(&config, 0, sizeof(config));
    config.version = FORWARD_CONFIG_VERSION;
    config.declared = true;
    config.enabled = true;
    config.target[0] = 10U;
    config.target[1] = 0U;
    config.target[2] = 2U;
    config.target[3] = 2U;
    config.target_port = 18443U;
    config.listen_port = listen_port;
    config.client_count = 1U;
    config.clients[0][0] = 10U;
    config.clients[0][1] = 0U;
    config.clients[0][2] = 2U;
    config.clients[0][3] = 2U;
    return config;
}

static struct tcp_pcb *make_client(uint16_t local_port)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb == NULL) {
        return NULL;
    }
    pcb->state = ESTABLISHED;
    IP4_ADDR(&pcb->local_ip, 10U, 0U, 2U, 15U);
    IP4_ADDR(&pcb->remote_ip, 10U, 0U, 2U, 2U);
    pcb->local_port = local_port;
    pcb->remote_port = 40000U;
    pcb->mss = 1460U;
    TCP_REG_ACTIVE(pcb);
    return pcb;
}

static err_t admit_client(
    struct hfoc_lwip_forward *forward,
    struct tcp_pcb *client)
{
    struct tcp_pcb_listen *listener =
        (struct tcp_pcb_listen *)forward->listen_pcb;

    return listener->accept(listener->callback_arg, client, ERR_OK);
}

static bool start_forward(
    struct hfoc_lwip_forward *forward,
    uint16_t listen_port)
{
    struct forward_config config = make_config(listen_port);

    memset(forward, 0, sizeof(*forward));
    if (hfoc_lwip_forward_init(forward, &netif, &config, 1U) !=
        HFOC_LWIP_FORWARD_OK) {
        return false;
    }
    return hfoc_lwip_forward_listen(forward) == HFOC_LWIP_FORWARD_OK;
}

static void test_target_syn_exhaustion_frees_the_slot(void)
{
    struct hfoc_lwip_forward forward;
    struct tcp_pcb *first;
    struct tcp_pcb *second;
    unsigned int turns = 0U;

    CHECK(start_forward(&forward, 19443U),
          "relay publishes its listener for the retry fixture");
    CHECK(hfoc_lwip_forward_listening(&forward),
          "the retry fixture listener is live");

    first = make_client(40001U);
    CHECK(first != NULL, "the first client PCB can be allocated");
    if (first == NULL) {
        (void)hfoc_lwip_forward_shutdown(&forward);
        return;
    }
    ip_outputs = 0U;
    CHECK(admit_client(&forward, first) == ERR_OK,
          "the declared client is admitted");
    CHECK(forward.client_pcb == first && forward.target_pcb != NULL,
          "the admitted client opens exactly one backend leg");
    CHECK(ip_outputs >= 1U,
          "the backend SYN leaves through the netif and is blackholed");

    /*
     * Drive only lwIP's timers.  The relay's service turn is deliberately not
     * called here: the regression is the stack-originated ERR_ABRT itself,
     * and the production idle check cannot help once the target leg is gone.
     */
    while (forward.target_pcb != NULL && turns < 2000U) {
        test_now_ms += 250U;
        (void)hfoc_lwip_timeouts_service();
        ++turns;
    }
    printf("INFO: target SYN retries exhausted after %u turns (now=%u ms), "
           "backend outputs=%u service_calls=%u tcp_ticks=%u\n",
           turns, (unsigned)test_now_ms, (unsigned)ip_outputs,
           (unsigned)hfoc_lwip_timeouts_service_count(),
           (unsigned)tcp_ticks);

    CHECK(forward.target_pcb == NULL,
          "lwIP exhausted the backend SYN retries and removed the target");
    CHECK(forward.client_pcb == NULL,
          "a stack-originated backend abort also fails the accepted client");
    CHECK(!hfoc_lwip_forward_session_active(&forward),
          "no half-session is left holding the one relay slot");
    CHECK(forward.aborted >= 1U,
          "the failed session is counted as aborted");
    CHECK(strcmp(hfoc_lwip_forward_verdict(&forward), "OPEN") != 0,
          "the failure publishes a verdict instead of leaving OPEN");
    CHECK(forward.event == HFOC_LWIP_FORWARD_EVENT_SESSION_FAILED,
          "the failure publishes the session-failed event");

    second = make_client(40002U);
    CHECK(second != NULL, "the second client PCB can be allocated");
    if (second != NULL) {
        ip_outputs = 0U;
        CHECK(admit_client(&forward, second) == ERR_OK,
              "the next client is admitted after the failed session");
        CHECK(forward.client_pcb == second && forward.target_pcb != NULL,
              "the next client opens a fresh backend leg");
        CHECK(forward.refused_busy == 0U,
              "the failed slot was not left busy");
        CHECK(ip_outputs >= 1U,
              "the fresh backend leg really dials the target");
    }
    (void)hfoc_lwip_forward_shutdown(&forward);
}

static void test_lone_client_hits_the_idle_deadline(void)
{
    struct hfoc_lwip_forward forward;
    struct tcp_pcb *client;

    CHECK(start_forward(&forward, 19444U),
          "relay publishes its listener for the lone-client fixture");
    client = make_client(40003U);
    CHECK(client != NULL, "the lone client PCB can be allocated");
    if (client == NULL) {
        (void)hfoc_lwip_forward_shutdown(&forward);
        return;
    }
    forward.client_pcb = client;
    forward.target_pcb = NULL;
    forward.last_progress_ms = test_now_ms;
    forward.now_ms = test_now_ms;
    (void)hfoc_lwip_forward_service(
        &forward, test_now_ms + HFOC_LWIP_FORWARD_IDLE_MS + 1U);

    CHECK(forward.client_pcb == NULL,
          "a lone client older than the idle deadline is failed");
    CHECK(!hfoc_lwip_forward_session_active(&forward),
          "the lone client no longer holds the relay slot");
    CHECK(forward.aborted >= 1U,
          "the idle failure is counted");
    CHECK(strcmp(hfoc_lwip_forward_verdict(&forward), "OPEN") != 0,
          "the idle failure publishes a verdict");
    (void)hfoc_lwip_forward_shutdown(&forward);
}

int main(void)
{
    init_lwip_core();
    init_netif();
    if (failures != 0) {
        fprintf(stderr, "lwIP forward fixture setup failed\n");
        return 1;
    }
    test_now_ms = 1000U;
    test_target_syn_exhaustion_frees_the_slot();
    test_lone_client_hits_the_idle_deadline();
    if (failures != 0) {
        fprintf(stderr, "lwIP forward fixture failures: %d\n", failures);
        return 1;
    }
    printf("lwIP forward relay failure fixture: OK\n");
    return 0;
}

#endif
