#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp.h"
#include "hfoc_lwip_tcp_backlog.h"
#include "hfoc_lwip_tcp_close_policy.h"
#include "hfoc_lwip_tcp_conn.h"
#include "hfoc_lwip_tcp_deadline.h"
#include "hfoc_lwip_tcp_drain_restart.h"
#include "hfoc_lwip_tcp_pool.h"
#include "hfoc_lwip_tcp_quarantine.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_DRAIN_RESTART_HOST_TEST

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

static err_t drain_pool_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err)
{
    (void)context;
    (void)connection;
    (void)event;
    (void)p;
    (void)len;
    (void)err;
    return ERR_OK;
}

static err_t drain_accept_callback(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    (void)context;
    (void)pcb;
    (void)generation;
    return ERR_OK;
}

static struct hfoc_lwip_tcp_conn_callbacks make_callbacks(void)
{
    struct hfoc_lwip_tcp_conn_callbacks callbacks;

    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = drain_pool_event;
    callbacks.context = NULL;
    return callbacks;
}

static struct tcp_pcb *make_pcb(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb != NULL) {
        pcb->state = ESTABLISHED;
    }
    return pcb;
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

static bool start_listener(
    struct hfoc_lwip_tcp_listener *listener,
    const struct hfoc_lwip_netif *netif,
    uint32_t generation)
{
    struct network_ipv4_config config = make_config();
    struct network_ipv4_port_policy policy = make_policy(443U);
    struct hfoc_lwip_tcp_callback callback;

    memset(listener, 0, sizeof(*listener));
    callback.accept = drain_accept_callback;
    callback.context = NULL;
    return hfoc_lwip_tcp_init(
               listener, netif, &config, &policy, &callback,
               generation) == HFOC_LWIP_TCP_OK &&
           hfoc_lwip_tcp_bind(listener, &policy) ==
               HFOC_LWIP_TCP_OK &&
           hfoc_lwip_tcp_listen(listener) ==
               HFOC_LWIP_TCP_OK;
}

static bool init_pool(struct hfoc_lwip_tcp_pool *pool)
{
    memset(pool, 0, sizeof(*pool));
    return hfoc_lwip_tcp_pool_init(pool) ==
        HFOC_LWIP_TCP_POOL_OK;
}

static bool reserve_token(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_pool_token *token)
{
    memset(token, 0, sizeof(*token));
    return hfoc_lwip_tcp_pool_reserve(pool, token) ==
        HFOC_LWIP_TCP_POOL_OK;
}

static bool publish_token(
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct tcp_pcb *pcb = make_pcb();
    struct hfoc_lwip_tcp_conn_callbacks callbacks =
        make_callbacks();

    if (pcb == NULL) {
        return false;
    }
    if (hfoc_lwip_tcp_pool_publish(
            pool, token, pcb, &callbacks) !=
        HFOC_LWIP_TCP_POOL_OK) {
        tcp_abort(pcb);
        return false;
    }
    return true;
}

static bool init_deadline(
    struct hfoc_lwip_tcp_deadline_scheduler *deadline)
{
    memset(deadline, 0, sizeof(*deadline));
    return hfoc_lwip_tcp_deadline_init(deadline) ==
        HFOC_LWIP_TCP_DEADLINE_OK;
}

static bool attach_deadline(
    struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_deadline_policy policy;

    memset(&policy, 0, sizeof(policy));
    policy.version = HFOC_LWIP_TCP_DEADLINE_VERSION;
    policy.accept_timeout_ms = 100000U;
    policy.progress_timeout_ms = 100000U;
    policy.idle_timeout_ms = 100000U;
    policy.close_timeout_ms = 100000U;
    return hfoc_lwip_tcp_deadline_attach(
               deadline, token, &policy, 1000U) ==
        HFOC_LWIP_TCP_DEADLINE_OK;
}

static bool init_backlog(struct hfoc_lwip_tcp_backlog *backlog)
{
    memset(backlog, 0, sizeof(*backlog));
    return hfoc_lwip_tcp_backlog_init(backlog) ==
        HFOC_LWIP_TCP_BACKLOG_OK;
}

static bool init_quarantine(
    struct hfoc_lwip_tcp_quarantine *gate)
{
    memset(gate, 0, sizeof(*gate));
    return hfoc_lwip_tcp_quarantine_init(gate) ==
        HFOC_LWIP_TCP_QUARANTINE_OK;
}

static bool attach_quarantine(
    struct hfoc_lwip_tcp_quarantine *gate,
    struct hfoc_lwip_tcp_pool *pool,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    struct hfoc_lwip_tcp_connection *connection =
        &pool->slots[token->index].connection;

    return hfoc_lwip_tcp_quarantine_attach(
        gate, token, connection, connection->pcb) ==
        HFOC_LWIP_TCP_QUARANTINE_OK;
}

static bool init_close_policy(
    struct hfoc_lwip_tcp_close_policy *policy)
{
    memset(policy, 0, sizeof(*policy));
    return hfoc_lwip_tcp_close_policy_init(policy) ==
        HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

static bool attach_close_policy(
    struct hfoc_lwip_tcp_close_policy *policy,
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_quarantine *gate,
    const struct hfoc_lwip_tcp_pool_token *token)
{
    return hfoc_lwip_tcp_close_policy_attach(
        policy, pool, gate, token) ==
        HFOC_LWIP_TCP_CLOSE_POLICY_OK;
}

static bool init_drain(
    struct hfoc_lwip_tcp_drain_restart *drain,
    uint32_t drain_timeout_ms)
{
    struct hfoc_lwip_tcp_drain_restart_config config;

    memset(drain, 0, sizeof(*drain));
    memset(&config, 0, sizeof(config));
    config.version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    config.drain_timeout_ms = drain_timeout_ms;
    return hfoc_lwip_tcp_drain_restart_init(
               drain, &config) ==
        HFOC_LWIP_TCP_DRAIN_RESTART_OK;
}

static bool init_boundaries(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    struct hfoc_lwip_tcp_backlog *backlog,
    struct hfoc_lwip_tcp_quarantine *gate,
    struct hfoc_lwip_tcp_close_policy *close_policy)
{
    return init_pool(pool) &&
           init_deadline(deadline) &&
           init_backlog(backlog) &&
           init_quarantine(gate) &&
           init_close_policy(close_policy);
}

static bool publish_attached_slot(
    struct hfoc_lwip_tcp_pool *pool,
    struct hfoc_lwip_tcp_deadline_scheduler *deadline,
    struct hfoc_lwip_tcp_quarantine *gate,
    struct hfoc_lwip_tcp_close_policy *close_policy,
    struct hfoc_lwip_tcp_pool_token *token)
{
    if (!reserve_token(pool, token) ||
        !publish_token(pool, token)) {
        return false;
    }
    return attach_deadline(deadline, token) &&
           attach_quarantine(gate, pool, token) &&
           attach_close_policy(
               close_policy, pool, gate, token);
}

static void test_strings_and_init_validation(void)
{
    struct hfoc_lwip_tcp_drain_restart drain;
    struct hfoc_lwip_tcp_drain_restart_config config;
    struct hfoc_lwip_tcp_drain_restart_snapshot snapshot;

    memset(&drain, 0, sizeof(drain));
    CHECK(hfoc_lwip_tcp_drain_restart_phase_string(
              HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE) != NULL &&
              hfoc_lwip_tcp_drain_restart_phase_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING) != NULL &&
              hfoc_lwip_tcp_drain_restart_phase_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING) != NULL &&
              hfoc_lwip_tcp_drain_restart_phase_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED) != NULL &&
              hfoc_lwip_tcp_drain_restart_phase_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED) != NULL &&
              hfoc_lwip_tcp_drain_restart_phase_string(
                  (enum hfoc_lwip_tcp_drain_restart_phase)99U) == NULL,
          "drain phase strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_drain_restart_status_string(
              HFOC_LWIP_TCP_DRAIN_RESTART_OK) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_NULL) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_VERSION) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_ALREADY_PUBLISHED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_UNPUBLISHED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_LISTENER) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_BACKLOG_NOT_EMPTY) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_NOT_DRAINED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_STOP_FAILED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_REMOVE_FAILED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_RESTART_FAILED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  HFOC_LWIP_TCP_DRAIN_RESTART_GENERATION_EXHAUSTED) != NULL &&
              hfoc_lwip_tcp_drain_restart_status_string(
                  (enum hfoc_lwip_tcp_drain_restart_status)99U) == NULL,
          "drain status strings cover known and unknown values");

    memset(&config, 0, sizeof(config));
    config.version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    config.drain_timeout_ms = 1000U;
    CHECK(hfoc_lwip_tcp_drain_restart_init(NULL, &config) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_NULL,
          "drain init rejects null drain");
    CHECK(hfoc_lwip_tcp_drain_restart_init(&drain, NULL) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_NULL,
          "drain init rejects null config");
    config.version = 0U;
    CHECK(hfoc_lwip_tcp_drain_restart_init(&drain, &config) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG,
          "drain init rejects invalid config version");
    config.version = HFOC_LWIP_TCP_DRAIN_RESTART_VERSION;
    config.drain_timeout_ms = 0U;
    CHECK(hfoc_lwip_tcp_drain_restart_init(&drain, &config) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG,
          "drain init rejects zero timeout");
    config.drain_timeout_ms =
        HFOC_LWIP_TCP_DRAIN_RESTART_DRAIN_TIMEOUT_MAX + 1U;
    CHECK(hfoc_lwip_tcp_drain_restart_init(&drain, &config) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CONFIG,
          "drain init rejects oversized timeout");

    config.drain_timeout_ms = 1000U;
    CHECK(init_drain(&drain, 1000U),
          "drain init publishes valid idle state");
    CHECK(hfoc_lwip_tcp_drain_restart_init(
              &drain, &config) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_BAD_STATE,
          "drain init rejects double init");
    snapshot = hfoc_lwip_tcp_drain_restart_snapshot(&drain);
    CHECK(snapshot.version ==
              HFOC_LWIP_TCP_DRAIN_RESTART_VERSION &&
              snapshot.phase ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_IDLE &&
              snapshot.pool_free_count == 0U &&
              snapshot.pool_published_count == 0U &&
              snapshot.stopped_count == 0U,
          "idle drain snapshot is exact");
}

static void test_start_validation(void)
{
    struct hfoc_lwip_tcp_drain_restart drain;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_netif wrong_netif = make_live_netif();
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_deadline_scheduler deadline;
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_close_policy close_policy;

    CHECK(init_boundaries(
              &pool, &deadline, &backlog, &gate,
              &close_policy) &&
              init_drain(&drain, 1000U),
          "start validation fixture initializes boundaries");

    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, NULL, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_NULL,
          "drain start rejects null listener");
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_LISTENER_NOT_LIVE,
          "drain start rejects unpublished listener");

    CHECK(start_listener(&listener, &netif, 1U),
          "start validation fixture publishes listening listener");
    wrong_netif.generation = 2U;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &wrong_netif, &pool, &deadline,
              &backlog, &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_NETIF,
          "drain start rejects mismatched netif generation");

    pool.published = false;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_POOL,
          "drain start rejects unpublished pool");
    pool.published = true;

    deadline.published = false;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_DEADLINE,
          "drain start rejects unpublished deadline scheduler");
    deadline.published = true;

    backlog.published = false;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_BACKLOG,
          "drain start rejects unpublished backlog");
    backlog.published = true;

    close_policy.published = false;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_CLOSE_POLICY,
          "drain start rejects unpublished close policy");
    close_policy.published = true;

    gate.published = false;
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_QUARANTINE,
          "drain start rejects unpublished quarantine");
    gate.published = true;

    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              drain.phase ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING &&
              drain.admission_generation == 1U,
          "drain start publishes admitting phase with exact generation");

    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_INVALID_PHASE,
          "drain start rejects non-idle phase");
    CHECK(hfoc_lwip_tcp_shutdown(&listener) ==
              HFOC_LWIP_TCP_OK,
          "start validation fixture closes listener after validation");
}

static void test_bounded_drain_and_restart(void)
{
    struct hfoc_lwip_tcp_drain_restart drain;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_deadline_scheduler deadline;
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_close_policy close_policy;
    struct hfoc_lwip_tcp_pool_token tokens[4];
    struct hfoc_lwip_tcp_drain_restart_snapshot snapshot;
    uint32_t index;

    CHECK(init_boundaries(
              &pool, &deadline, &backlog, &gate,
              &close_policy) &&
              start_listener(&listener, &netif, 1U) &&
              init_drain(&drain, 10000U),
          "drain restart fixture initializes boundaries and listener");
    for (index = 0U; index < 4U; ++index) {
        CHECK(publish_attached_slot(
                  &pool, &deadline, &gate, &close_policy,
                  &tokens[index]),
              "drain restart fixture publishes attached pool slot");
    }

    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK,
          "drain restart starts with four live connections");
    snapshot = hfoc_lwip_tcp_drain_restart_snapshot(&drain);
    CHECK(snapshot.phase ==
              HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_ADMITTING &&
              snapshot.pool_free_count == 0U &&
              snapshot.pool_published_count == 4U &&
              snapshot.deadline_active_count == 4U &&
              snapshot.close_policy_active_count == 4U &&
              snapshot.quarantine_active_count == 4U,
          "drain snapshot captures four live attached slots");

    CHECK(hfoc_lwip_tcp_drain_restart_stop_admission(
              &drain, 1000U) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              !hfoc_lwip_tcp_published(&listener) &&
              drain.phase ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINING &&
              drain.stopped_count == 1U,
          "stop admission closes listener and arms bounded drain");
    CHECK(hfoc_lwip_tcp_drain_restart_step(
              &drain, 2000U) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_pool_published_count(&pool) == 4U &&
              drain.aborted_count == 0U,
          "bounded drain leaves live connections before deadline");
    CHECK(hfoc_lwip_tcp_pool_connection(&pool, 0U)->state ==
              HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_pool_connection(&pool, 3U)->state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED,
          "bounded drain leaves first and last connections established");
    CHECK(hfoc_lwip_tcp_drain_restart_finish_drain(&drain) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_POOL_NOT_EMPTY,
          "finish drain rejects non-empty pool");

    CHECK(hfoc_lwip_tcp_drain_restart_step(
              &drain, 12000U) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_pool_free_count(&pool) == 4U &&
              hfoc_lwip_tcp_pool_published_count(&pool) == 0U &&
              hfoc_lwip_tcp_deadline_active_count(&deadline) == 0U &&
              hfoc_lwip_tcp_close_policy_active_count(
                  &close_policy) == 0U &&
              hfoc_lwip_tcp_quarantine_active_count(&gate) == 0U &&
              hfoc_lwip_tcp_backlog_pending_count(&backlog) == 0U &&
              drain.aborted_count == 4U,
          "drain deadline aborts and releases all live connections");

    CHECK(hfoc_lwip_tcp_drain_restart_finish_drain(&drain) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              !hfoc_lwip_netif_live(&netif) &&
              drain.phase ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_DRAINED &&
              drain.drained_count == 1U,
          "finish drain removes netif after empty boundaries");

    CHECK(hfoc_lwip_tcp_drain_restart_restart(&drain) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_listening(&listener) &&
              hfoc_lwip_tcp_generation(&listener) == 2U &&
              hfoc_lwip_netif_live(&netif) &&
              hfoc_lwip_tcp_pool_free_count(&pool) == 4U &&
              hfoc_lwip_tcp_pool_reserved_count(&pool) == 0U &&
              hfoc_lwip_tcp_pool_published_count(&pool) == 0U &&
              drain.phase ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_PHASE_RESTARTED &&
              drain.restarted_count == 1U,
          "restart publishes listener and netif with exact pool counts");

    CHECK(hfoc_lwip_tcp_drain_restart_stop_admission(
              &drain, 20000U) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_drain_restart_step(
                  &drain, 30000U) ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_drain_restart_finish_drain(&drain) ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_OK,
          "restarted listener supports a second bounded drain cycle");
}

static void test_backlog_drain(void)
{
    struct hfoc_lwip_tcp_drain_restart drain;
    struct hfoc_lwip_tcp_listener listener;
    struct hfoc_lwip_netif netif = make_live_netif();
    struct hfoc_lwip_tcp_pool pool;
    struct hfoc_lwip_tcp_deadline_scheduler deadline;
    struct hfoc_lwip_tcp_backlog backlog;
    struct hfoc_lwip_tcp_quarantine gate;
    struct hfoc_lwip_tcp_close_policy close_policy;
    struct hfoc_lwip_tcp_backlog_token backlog_token;
    struct tcp_pcb *first = make_pcb();
    struct tcp_pcb *second = make_pcb();

    CHECK(first != NULL && second != NULL,
          "backlog drain fixture allocates pending PCBs");
    CHECK(init_boundaries(
              &pool, &deadline, &backlog, &gate,
              &close_policy) &&
              start_listener(&listener, &netif, 1U) &&
              init_drain(&drain, 10000U),
          "backlog drain fixture initializes boundaries and listener");
    CHECK(hfoc_lwip_tcp_drain_restart_start(
              &drain, &listener, &netif, &pool, &deadline, &backlog,
              &close_policy, &gate) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK,
          "backlog drain starts with empty pool");

    CHECK(hfoc_lwip_tcp_backlog_admit(
              &backlog, first, &backlog_token) ==
              HFOC_LWIP_TCP_BACKLOG_OK &&
              hfoc_lwip_tcp_backlog_admit(
                  &backlog, second, &backlog_token) ==
                  HFOC_LWIP_TCP_BACKLOG_OK &&
              hfoc_lwip_tcp_backlog_pending_count(&backlog) == 2U,
          "backlog drain fixture admits two pending PCBs");
    CHECK(hfoc_lwip_tcp_drain_restart_stop_admission(
              &drain, 1000U) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_drain_restart_step(
                  &drain, 2000U) ==
                  HFOC_LWIP_TCP_DRAIN_RESTART_OK &&
              hfoc_lwip_tcp_backlog_pending_count(&backlog) == 0U &&
              drain.backlog_aborted_count == 2U &&
              hfoc_lwip_tcp_pool_free_count(&pool) == 4U,
          "drain aborts pending backlog PCBs exactly once");
    CHECK(hfoc_lwip_tcp_drain_restart_finish_drain(&drain) ==
              HFOC_LWIP_TCP_DRAIN_RESTART_OK,
          "backlog drain finishes cleanly");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "drain restart fixture starts fixed lwIP allocator");

    test_strings_and_init_validation();
    test_start_validation();
    test_bounded_drain_and_restart();
    test_backlog_drain();

    if (failures != 0) {
        fprintf(stderr, "%d listener drain/restart fixture failure(s)\n",
                failures);
        return 1;
    }
    printf("lwIP listener drain and restart host fixture: OK\n");
    return 0;
}

#else
int main(void)
{
    return 0;
}
#endif
