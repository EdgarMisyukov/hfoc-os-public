#include "hfoc_lwip.h"
#include "hfoc_lwip_tcp.h"
#include "hfoc_lwip_tcp_conn.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_LWIP_TCP_CONN_HOST_TEST

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

struct event_record {
    uint32_t accept_count;
    uint32_t receive_count;
    uint32_t sent_count;
    uint32_t error_count;
    uint32_t poll_count;
    uint32_t close_count;
    uint32_t abort_count;
    uint32_t last_len;
    err_t last_err;
    struct pbuf *last_p;
};

struct listener_seam_context {
    struct hfoc_lwip_tcp_connection *connection;
    const struct hfoc_lwip_tcp_conn_callbacks *callbacks;
};

static err_t record_event(
    void *context,
    struct hfoc_lwip_tcp_connection *connection,
    enum hfoc_lwip_tcp_conn_event event,
    struct pbuf *p,
    uint16_t len,
    err_t err)
{
    struct event_record *record = (struct event_record *)context;

    (void)connection;
    record->last_len = len;
    record->last_err = err;
    record->last_p = p;
    switch (event) {
    case HFOC_LWIP_TCP_CONN_EVENT_ACCEPT:
        ++record->accept_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_RECEIVE:
        ++record->receive_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_SENT:
        ++record->sent_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ERROR:
        ++record->error_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_POLL:
        ++record->poll_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_CLOSE:
        ++record->close_count;
        break;
    case HFOC_LWIP_TCP_CONN_EVENT_ABORT:
        ++record->abort_count;
        break;
    default:
        break;
    }
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

static err_t test_accept_callback(
    void *context,
    struct tcp_pcb *pcb,
    uint32_t generation)
{
    const struct listener_seam_context *seam =
        (const struct listener_seam_context *)context;

    (void)generation;
    return hfoc_lwip_tcp_conn_accept(
        seam->connection, pcb, 1U, seam->callbacks) ==
        HFOC_LWIP_TCP_CONN_OK
        ? ERR_OK
        : ERR_ABRT;
}

static struct hfoc_lwip_tcp_listener make_listener(
    const struct listener_seam_context *seam)
{
    struct hfoc_lwip_netif netif = make_live_netif();
    struct network_ipv4_config config = make_config();
    struct network_ipv4_port_policy policy = make_policy(443U);
    struct hfoc_lwip_tcp_callback listener_callback;
    struct hfoc_lwip_tcp_listener listener;

    memset(&listener, 0, sizeof(listener));
    memset(&listener_callback, 0, sizeof(listener_callback));
    listener_callback.accept = test_accept_callback;
    listener_callback.context = (void *)seam;
    CHECK(hfoc_lwip_tcp_init(
              &listener, &netif, &config, &policy,
              &listener_callback, 1U) == HFOC_LWIP_TCP_OK,
          "connection fixture initializes raw TCP listener");
    CHECK(hfoc_lwip_tcp_bind(&listener, &policy) == HFOC_LWIP_TCP_OK,
          "connection fixture binds raw TCP listener");
    CHECK(hfoc_lwip_tcp_listen(&listener) == HFOC_LWIP_TCP_OK,
          "connection fixture starts raw TCP listener");
    return listener;
}

static struct tcp_pcb *make_pcb(void)
{
    struct tcp_pcb *pcb = tcp_new();

    if (pcb != NULL) {
        pcb->state = ESTABLISHED;
    }
    return pcb;
}

static void test_accept_validation(void)
{
    struct event_record record;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    struct hfoc_lwip_tcp_connection connection;
    struct tcp_pcb *pcb;
    struct tcp_pcb *listen_pcb;

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = record_event;
    callbacks.context = &record;

    CHECK(hfoc_lwip_tcp_conn_accept(
              NULL, NULL, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_NULL,
          "connection accept rejects null connection");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, NULL, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_NULL,
          "connection accept rejects null PCB");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, NULL, 1U, NULL) ==
              HFOC_LWIP_TCP_CONN_NULL,
          "connection accept rejects null callbacks");

    pcb = make_pcb();
    CHECK(pcb != NULL, "connection fixture allocates validation PCB");
    callbacks.event = NULL;
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_INVALID_CALLBACK,
          "connection accept rejects null event callback");
    callbacks.event = record_event;

    connection.version = 2U;
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_INVALID_VERSION,
          "connection accept rejects invalid version");
    memset(&connection, 0, sizeof(connection));

    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 0U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_INVALID_GENERATION,
          "connection accept rejects zero generation");

    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_OK,
          "connection accept publishes a valid connection");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED,
          "connection accept rejects an already-published connection");
    CHECK(hfoc_lwip_tcp_conn_abort(&connection) ==
              HFOC_LWIP_TCP_CONN_OK,
          "connection validation fixture aborts published connection");
    memset(&connection, 0, sizeof(connection));

    listen_pcb = make_pcb();
    CHECK(listen_pcb != NULL,
          "connection fixture allocates invalid PCB");
    listen_pcb->state = LISTEN;
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, listen_pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_INVALID_PCB,
          "connection accept rejects a LISTEN PCB");
    listen_pcb->state = CLOSED;
    tcp_abort(listen_pcb);
}

static void test_raw_callback_normalization(void)
{
    struct event_record record;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    struct hfoc_lwip_tcp_connection connection;
    struct tcp_pcb *pcb;
    struct pbuf *p;
    err_t err;
    void *saved_arg;

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = record_event;
    callbacks.context = &record;

    pcb = make_pcb();
    CHECK(pcb != NULL, "raw callback fixture allocates connection PCB");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_OK,
          "raw callback fixture accepts connection");
    CHECK(hfoc_lwip_tcp_conn_published(&connection) &&
              connection.state ==
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED &&
              hfoc_lwip_tcp_conn_generation(&connection) == 1U &&
              connection.accept_count == 1U &&
              record.accept_count == 1U,
          "accepted connection publishes established state");
    CHECK(pcb->callback_arg == &connection &&
              pcb->recv != NULL &&
              pcb->sent != NULL &&
              pcb->errf != NULL &&
              pcb->poll != NULL,
          "accepted PCB installs raw lifecycle callbacks");

    p = pbuf_alloc(PBUF_RAW, 8U, PBUF_RAM);
    CHECK(p != NULL, "raw callback fixture allocates receive pbuf");
    err = pcb->recv(pcb->callback_arg, pcb, p, ERR_OK);
    CHECK(err == ERR_OK &&
              connection.receive_count == 1U &&
              connection.received_bytes == 8U &&
              record.receive_count == 1U,
          "raw recv normalizes receive event and bytes");
    pbuf_free(p);

    err = pcb->sent(pcb->callback_arg, pcb, 5U);
    CHECK(err == ERR_OK &&
              connection.sent_count == 1U &&
              connection.sent_bytes == 5U &&
              record.sent_count == 1U,
          "raw sent normalizes sent event and bytes");

    err = pcb->poll(pcb->callback_arg, pcb);
    CHECK(err == ERR_OK &&
              connection.poll_count == 1U &&
              record.poll_count == 1U,
          "raw poll normalizes poll event");

    saved_arg = pcb->callback_arg;
    pcb->callback_arg = NULL;
    err = pcb->recv(saved_arg, pcb, NULL, ERR_OK);
    CHECK(err != ERR_OK &&
              connection.stale_count == 1U &&
              connection.rejected_count == 1U &&
              record.receive_count == 1U,
          "mismatched raw callback argument is rejected as stale");
    pcb->callback_arg = saved_arg;

    CHECK(hfoc_lwip_tcp_conn_abort(&connection) ==
              HFOC_LWIP_TCP_CONN_OK &&
              connection.state == HFOC_LWIP_TCP_CONN_STATE_ABORTED &&
              connection.abort_count == 1U &&
              connection.pcb == NULL &&
              record.abort_count == 1U &&
              record.error_count == 0U,
          "abort normalizes lwIP ERR_ABRT as abort event");
    CHECK(hfoc_lwip_tcp_conn_recv(
              &connection, saved_arg, NULL, ERR_OK) ==
              HFOC_LWIP_TCP_CONN_STALE_CALLBACK &&
              connection.stale_count == 2U,
          "post-abort callback is rejected as stale");
}

static void test_error_and_remote_close(void)
{
    struct event_record record;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    struct hfoc_lwip_tcp_connection connection;
    struct tcp_pcb *error_pcb;
    struct tcp_pcb *close_pcb;

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = record_event;
    callbacks.context = &record;

    error_pcb = make_pcb();
    CHECK(error_pcb != NULL, "error fixture allocates connection PCB");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, error_pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_OK,
          "error fixture accepts connection");
    CHECK(hfoc_lwip_tcp_conn_error(
              &connection, ERR_RST) == HFOC_LWIP_TCP_CONN_OK &&
              connection.state == HFOC_LWIP_TCP_CONN_STATE_ABORTED &&
              connection.error_count == 1U &&
              connection.pcb == NULL &&
              record.error_count == 1U,
          "remote error normalizes error event and aborts state");
    CHECK(hfoc_lwip_tcp_conn_error(
              &connection, ERR_RST) ==
              HFOC_LWIP_TCP_CONN_STALE_CALLBACK &&
              connection.stale_count == 1U,
          "post-error callback is rejected as stale");
    tcp_abort(error_pcb);

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    close_pcb = make_pcb();
    CHECK(close_pcb != NULL, "close fixture allocates connection PCB");
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, close_pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_OK,
          "close fixture accepts connection");
    CHECK(hfoc_lwip_tcp_conn_recv(
              &connection, close_pcb, NULL, ERR_OK) ==
              HFOC_LWIP_TCP_CONN_OK &&
              connection.state ==
                  HFOC_LWIP_TCP_CONN_STATE_CLOSING &&
              connection.close_count == 1U &&
              record.close_count == 1U,
          "null recv normalizes remote close event");
    CHECK(hfoc_lwip_tcp_conn_close(&connection) ==
              HFOC_LWIP_TCP_CONN_OK &&
              connection.state ==
                  HFOC_LWIP_TCP_CONN_STATE_CLOSING &&
              connection.close_count == 2U &&
              connection.pcb != NULL,
          "close after remote close emits close event and closes state");
    CHECK(hfoc_lwip_tcp_conn_recv(
              &connection, close_pcb, NULL, ERR_OK) ==
              HFOC_LWIP_TCP_CONN_STALE_CALLBACK &&
              connection.stale_count == 1U,
          "post-close callback is rejected as stale");
    CHECK(hfoc_lwip_tcp_conn_abort(&connection) ==
              HFOC_LWIP_TCP_CONN_OK &&
              connection.state ==
                  HFOC_LWIP_TCP_CONN_STATE_ABORTED,
          "close-state connection aborts cleanly");
}

static void test_local_close(void)
{
    struct event_record record;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    struct hfoc_lwip_tcp_connection connection;
    struct tcp_pcb *pcb;

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.event = record_event;
    callbacks.context = &record;

    pcb = make_pcb();
    CHECK(pcb != NULL, "local close fixture allocates connection PCB");
    pcb->state = CLOSED;
    CHECK(hfoc_lwip_tcp_conn_accept(
              &connection, pcb, 1U, &callbacks) ==
              HFOC_LWIP_TCP_CONN_OK,
          "local close fixture accepts connection");
    CHECK(hfoc_lwip_tcp_conn_close(&connection) ==
              HFOC_LWIP_TCP_CONN_OK &&
              connection.state ==
                  HFOC_LWIP_TCP_CONN_STATE_CLOSED &&
              connection.close_count == 1U &&
              connection.pcb == NULL &&
              record.close_count == 1U,
          "local close emits close event and clears PCB owner");
    CHECK(hfoc_lwip_tcp_conn_close(&connection) ==
              HFOC_LWIP_TCP_CONN_ALREADY_CLOSED,
          "second local close is rejected");
}

static void test_listener_seam(void)
{
    struct event_record record;
    struct hfoc_lwip_tcp_conn_callbacks callbacks;
    struct hfoc_lwip_tcp_connection connection;
    struct listener_seam_context seam;
    struct hfoc_lwip_tcp_listener listener;
    struct tcp_pcb *pcb;

    memset(&record, 0, sizeof(record));
    memset(&connection, 0, sizeof(connection));
    memset(&callbacks, 0, sizeof(callbacks));
    memset(&seam, 0, sizeof(seam));
    callbacks.event = record_event;
    callbacks.context = &record;
    seam.connection = &connection;
    seam.callbacks = &callbacks;
    listener = make_listener(&seam);

    pcb = make_pcb();
    CHECK(pcb != NULL, "listener seam fixture allocates connection PCB");
    CHECK(listener.callback.accept(
              listener.callback.context, pcb,
              listener.generation) == ERR_OK &&
              hfoc_lwip_tcp_conn_published(&connection) &&
              record.accept_count == 1U,
          "listener accept seam publishes connection and records accept");
    CHECK(hfoc_lwip_tcp_conn_abort(&connection) ==
              HFOC_LWIP_TCP_CONN_OK &&
              record.abort_count == 1U,
          "listener seam connection aborts and records abort");
    CHECK(hfoc_lwip_tcp_shutdown(&listener) ==
              HFOC_LWIP_TCP_OK,
          "listener seam shutdown succeeds");
}

static void test_strings(void)
{
    CHECK(hfoc_lwip_tcp_conn_state_string(
              HFOC_LWIP_TCP_CONN_STATE_INACTIVE) != NULL &&
              hfoc_lwip_tcp_conn_state_string(
                  HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED) != NULL &&
              hfoc_lwip_tcp_conn_state_string(
                  HFOC_LWIP_TCP_CONN_STATE_CLOSING) != NULL &&
              hfoc_lwip_tcp_conn_state_string(
                  HFOC_LWIP_TCP_CONN_STATE_CLOSED) != NULL &&
              hfoc_lwip_tcp_conn_state_string(
                  HFOC_LWIP_TCP_CONN_STATE_ABORTED) != NULL &&
              hfoc_lwip_tcp_conn_state_string(
                  (enum hfoc_lwip_tcp_conn_state)99U) == NULL,
          "connection state strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_conn_event_string(
              HFOC_LWIP_TCP_CONN_EVENT_ACCEPT) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_RECEIVE) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_SENT) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_ERROR) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_POLL) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_CLOSE) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  HFOC_LWIP_TCP_CONN_EVENT_ABORT) != NULL &&
              hfoc_lwip_tcp_conn_event_string(
                  (enum hfoc_lwip_tcp_conn_event)99U) == NULL,
          "connection event strings cover known and unknown values");
    CHECK(hfoc_lwip_tcp_conn_status_string(
              HFOC_LWIP_TCP_CONN_OK) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_NULL) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_BAD_STATE) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_INVALID_VERSION) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_UNPUBLISHED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_INVALID_GENERATION) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_INVALID_CALLBACK) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_INVALID_PCB) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_STALE_CALLBACK) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_ABORTED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_CLOSE_FAILED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_NOT_ESTABLISHED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  HFOC_LWIP_TCP_CONN_ALREADY_CLOSED) != NULL &&
              hfoc_lwip_tcp_conn_status_string(
                  (enum hfoc_lwip_tcp_conn_status)99U) == NULL,
          "connection status strings cover known and unknown values");
}

int main(void)
{
    CHECK(hfoc_lwip_alloc_init(),
          "connection fixture starts fixed lwIP allocator");

    test_accept_validation();
    test_raw_callback_normalization();
    test_error_and_remote_close();
    test_local_close();
    test_listener_seam();
    test_strings();

    if (failures != 0) {
        fprintf(stderr, "lwIP TCP connection fixture failures: %d\n",
                failures);
        return 1;
    }
    printf("lwIP TCP connection lifecycle fixture: OK\n");
    return 0;
}

#endif
