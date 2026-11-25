#include "hfoc_lwip_tcp_conn.h"

#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(HFOC_LWIP_TCP_HOST_TEST) || \
    defined(HFOC_TCP_LISTENER_QEMU_TEST)

static bool hfoc_lwip_tcp_conn_generation_valid(uint32_t generation)
{
    return generation >= HFOC_LWIP_TCP_CONN_GENERATION_MIN &&
           generation <= HFOC_LWIP_TCP_CONN_GENERATION_MAX;
}

static bool hfoc_lwip_tcp_conn_live(
    const struct hfoc_lwip_tcp_connection *connection)
{
    return connection != NULL &&
           connection->published &&
           connection->version == HFOC_LWIP_TCP_CONN_VERSION &&
           hfoc_lwip_tcp_conn_generation_valid(
               connection->generation) &&
           (connection->state == HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED ||
            connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSING) &&
           connection->callbacks.event != NULL;
}

static enum hfoc_lwip_tcp_conn_status
hfoc_lwip_tcp_conn_stale(
    struct hfoc_lwip_tcp_connection *connection)
{
    ++connection->stale_count;
    ++connection->rejected_count;
    return HFOC_LWIP_TCP_CONN_STALE_CALLBACK;
}

static void hfoc_lwip_tcp_conn_add_bytes(
    uint32_t *total,
    uint32_t amount)
{
    if (amount > UINT32_MAX - *total) {
        *total = UINT32_MAX;
    } else {
        *total += amount;
    }
}

static err_t hfoc_lwip_tcp_conn_lwip_recv(
    void *arg,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err)
{
    enum hfoc_lwip_tcp_conn_status status;

    status = hfoc_lwip_tcp_conn_recv(
        (struct hfoc_lwip_tcp_connection *)arg, pcb, p, err);
    if (status == HFOC_LWIP_TCP_CONN_OK) {
        return ERR_OK;
    }
    if (p != NULL) {
        pbuf_free(p);
    }
    return ERR_ABRT;
}

static err_t hfoc_lwip_tcp_conn_lwip_sent(
    void *arg,
    struct tcp_pcb *pcb,
    u16_t len)
{
    enum hfoc_lwip_tcp_conn_status status;

    status = hfoc_lwip_tcp_conn_sent(
        (struct hfoc_lwip_tcp_connection *)arg, pcb, len);
    return status == HFOC_LWIP_TCP_CONN_OK ? ERR_OK : ERR_ABRT;
}

static err_t hfoc_lwip_tcp_conn_lwip_poll(
    void *arg,
    struct tcp_pcb *pcb)
{
    enum hfoc_lwip_tcp_conn_status status;

    status = hfoc_lwip_tcp_conn_poll(
        (struct hfoc_lwip_tcp_connection *)arg, pcb);
    return status == HFOC_LWIP_TCP_CONN_OK ? ERR_OK : ERR_ABRT;
}

static void hfoc_lwip_tcp_conn_lwip_err(
    void *arg,
    err_t err)
{
    (void)hfoc_lwip_tcp_conn_error(
        (struct hfoc_lwip_tcp_connection *)arg, err);
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_accept(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    uint32_t generation,
    const struct hfoc_lwip_tcp_conn_callbacks *callbacks)
{
    err_t event_err;

    if (connection == NULL || pcb == NULL || callbacks == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (connection->published) {
        return HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED;
    }
    if (connection->version != 0U) {
        return HFOC_LWIP_TCP_CONN_INVALID_VERSION;
    }
    if (!hfoc_lwip_tcp_conn_generation_valid(generation)) {
        return HFOC_LWIP_TCP_CONN_INVALID_GENERATION;
    }
    if (callbacks->event == NULL) {
        return HFOC_LWIP_TCP_CONN_INVALID_CALLBACK;
    }
    if (pcb->state == LISTEN) {
        return HFOC_LWIP_TCP_CONN_INVALID_PCB;
    }

    memset(connection, 0, sizeof(*connection));
    connection->version = HFOC_LWIP_TCP_CONN_VERSION;
    connection->generation = generation;
    connection->state = HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED;
    connection->pcb = pcb;
    connection->callbacks = *callbacks;
    connection->published = true;

    tcp_arg(pcb, connection);
    tcp_recv(pcb, hfoc_lwip_tcp_conn_lwip_recv);
    tcp_sent(pcb, hfoc_lwip_tcp_conn_lwip_sent);
    tcp_err(pcb, hfoc_lwip_tcp_conn_lwip_err);
    tcp_poll(
        pcb,
        hfoc_lwip_tcp_conn_lwip_poll,
        (u8_t)HFOC_LWIP_TCP_CONN_POLL_INTERVAL);

    ++connection->accept_count;
    event_err = connection->callbacks.event(
        connection->callbacks.context,
        connection,
        HFOC_LWIP_TCP_CONN_EVENT_ACCEPT,
        NULL,
        0U,
        ERR_OK);
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
        tcp_abort(connection->pcb);
        return HFOC_LWIP_TCP_CONN_ABORTED;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_recv(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    struct pbuf *p,
    err_t err)
{
    err_t event_err;
    uint32_t length;

    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (pcb == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (pcb != connection->pcb || pcb->callback_arg != connection) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }

    if (p != NULL) {
        length = p->tot_len;
        hfoc_lwip_tcp_conn_add_bytes(
            &connection->received_bytes, length);
        ++connection->receive_count;
        event_err = connection->callbacks.event(
            connection->callbacks.context,
            connection,
            HFOC_LWIP_TCP_CONN_EVENT_RECEIVE,
            p,
            (uint16_t)length,
            err);
    } else {
        connection->state = HFOC_LWIP_TCP_CONN_STATE_CLOSING;
        ++connection->close_count;
        event_err = connection->callbacks.event(
            connection->callbacks.context,
            connection,
            HFOC_LWIP_TCP_CONN_EVENT_CLOSE,
            NULL,
            0U,
            err);
    }
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
        if (connection->pcb != NULL) {
            tcp_abort(connection->pcb);
        }
        return HFOC_LWIP_TCP_CONN_ABORTED;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_sent(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb,
    uint16_t len)
{
    err_t event_err;

    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (pcb == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (pcb != connection->pcb || pcb->callback_arg != connection) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }

    hfoc_lwip_tcp_conn_add_bytes(&connection->sent_bytes, len);
    ++connection->sent_count;
    event_err = connection->callbacks.event(
        connection->callbacks.context,
        connection,
        HFOC_LWIP_TCP_CONN_EVENT_SENT,
        NULL,
        len,
        ERR_OK);
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
        if (connection->pcb != NULL) {
            tcp_abort(connection->pcb);
        }
        return HFOC_LWIP_TCP_CONN_ABORTED;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_poll(
    struct hfoc_lwip_tcp_connection *connection,
    struct tcp_pcb *pcb)
{
    err_t event_err;

    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (pcb == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (pcb != connection->pcb || pcb->callback_arg != connection) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }

    ++connection->poll_count;
    event_err = connection->callbacks.event(
        connection->callbacks.context,
        connection,
        HFOC_LWIP_TCP_CONN_EVENT_POLL,
        NULL,
        0U,
        ERR_OK);
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
        if (connection->pcb != NULL) {
            tcp_abort(connection->pcb);
        }
        return HFOC_LWIP_TCP_CONN_ABORTED;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_error(
    struct hfoc_lwip_tcp_connection *connection,
    err_t err)
{
    err_t event_err;

    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        return hfoc_lwip_tcp_conn_stale(connection);
    }

    connection->pcb = NULL;
    connection->state = HFOC_LWIP_TCP_CONN_STATE_ABORTED;
    if (err == ERR_ABRT) {
        ++connection->abort_count;
        event_err = connection->callbacks.event(
            connection->callbacks.context,
            connection,
            HFOC_LWIP_TCP_CONN_EVENT_ABORT,
            NULL,
            0U,
            err);
    } else {
        ++connection->error_count;
        event_err = connection->callbacks.event(
            connection->callbacks.context,
            connection,
            HFOC_LWIP_TCP_CONN_EVENT_ERROR,
            NULL,
            0U,
            err);
    }
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_close(
    struct hfoc_lwip_tcp_connection *connection)
{
    enum tcp_state pcb_state;
    err_t close_err;
    err_t event_err;

    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        if (connection->state == HFOC_LWIP_TCP_CONN_STATE_CLOSED ||
            connection->state == HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
            return HFOC_LWIP_TCP_CONN_ALREADY_CLOSED;
        }
        return hfoc_lwip_tcp_conn_stale(connection);
    }
    if (connection->pcb == NULL) {
        return HFOC_LWIP_TCP_CONN_ALREADY_CLOSED;
    }

    pcb_state = connection->pcb->state;
    close_err = tcp_close(connection->pcb);
    if (close_err != ERR_OK) {
        return HFOC_LWIP_TCP_CONN_CLOSE_FAILED;
    }

    ++connection->close_count;
    if (pcb_state == CLOSED) {
        connection->state = HFOC_LWIP_TCP_CONN_STATE_CLOSED;
        connection->pcb = NULL;
    } else {
        connection->state = HFOC_LWIP_TCP_CONN_STATE_CLOSING;
    }
    event_err = connection->callbacks.event(
        connection->callbacks.context,
        connection,
        HFOC_LWIP_TCP_CONN_EVENT_CLOSE,
        NULL,
        0U,
        close_err);
    if (event_err != ERR_OK) {
        ++connection->rejected_count;
        return HFOC_LWIP_TCP_CONN_ABORTED;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

enum hfoc_lwip_tcp_conn_status hfoc_lwip_tcp_conn_abort(
    struct hfoc_lwip_tcp_connection *connection)
{
    if (connection == NULL) {
        return HFOC_LWIP_TCP_CONN_NULL;
    }
    if (!connection->published) {
        return HFOC_LWIP_TCP_CONN_UNPUBLISHED;
    }
    if (!hfoc_lwip_tcp_conn_live(connection)) {
        return HFOC_LWIP_TCP_CONN_ALREADY_CLOSED;
    }
    if (connection->pcb == NULL) {
        return HFOC_LWIP_TCP_CONN_ALREADY_CLOSED;
    }

    tcp_abort(connection->pcb);
    if (connection->state != HFOC_LWIP_TCP_CONN_STATE_ABORTED) {
        connection->state = HFOC_LWIP_TCP_CONN_STATE_ABORTED;
        connection->pcb = NULL;
        ++connection->abort_count;
    }
    return HFOC_LWIP_TCP_CONN_OK;
}

bool hfoc_lwip_tcp_conn_published(
    const struct hfoc_lwip_tcp_connection *connection)
{
    return hfoc_lwip_tcp_conn_live(connection);
}

uint32_t hfoc_lwip_tcp_conn_generation(
    const struct hfoc_lwip_tcp_connection *connection)
{
    return hfoc_lwip_tcp_conn_live(connection)
        ? connection->generation
        : 0U;
}

const char *hfoc_lwip_tcp_conn_state_string(
    enum hfoc_lwip_tcp_conn_state state)
{
    switch (state) {
    case HFOC_LWIP_TCP_CONN_STATE_INACTIVE:
        return "HFOC_LWIP_TCP_CONN_STATE_INACTIVE";
    case HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED:
        return "HFOC_LWIP_TCP_CONN_STATE_ESTABLISHED";
    case HFOC_LWIP_TCP_CONN_STATE_CLOSING:
        return "HFOC_LWIP_TCP_CONN_STATE_CLOSING";
    case HFOC_LWIP_TCP_CONN_STATE_CLOSED:
        return "HFOC_LWIP_TCP_CONN_STATE_CLOSED";
    case HFOC_LWIP_TCP_CONN_STATE_ABORTED:
        return "HFOC_LWIP_TCP_CONN_STATE_ABORTED";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_conn_event_string(
    enum hfoc_lwip_tcp_conn_event event)
{
    switch (event) {
    case HFOC_LWIP_TCP_CONN_EVENT_ACCEPT:
        return "HFOC_LWIP_TCP_CONN_EVENT_ACCEPT";
    case HFOC_LWIP_TCP_CONN_EVENT_RECEIVE:
        return "HFOC_LWIP_TCP_CONN_EVENT_RECEIVE";
    case HFOC_LWIP_TCP_CONN_EVENT_SENT:
        return "HFOC_LWIP_TCP_CONN_EVENT_SENT";
    case HFOC_LWIP_TCP_CONN_EVENT_ERROR:
        return "HFOC_LWIP_TCP_CONN_EVENT_ERROR";
    case HFOC_LWIP_TCP_CONN_EVENT_POLL:
        return "HFOC_LWIP_TCP_CONN_EVENT_POLL";
    case HFOC_LWIP_TCP_CONN_EVENT_CLOSE:
        return "HFOC_LWIP_TCP_CONN_EVENT_CLOSE";
    case HFOC_LWIP_TCP_CONN_EVENT_ABORT:
        return "HFOC_LWIP_TCP_CONN_EVENT_ABORT";
    default:
        return NULL;
    }
}

const char *hfoc_lwip_tcp_conn_status_string(
    enum hfoc_lwip_tcp_conn_status status)
{
    switch (status) {
    case HFOC_LWIP_TCP_CONN_OK:
        return "HFOC_LWIP_TCP_CONN_OK";
    case HFOC_LWIP_TCP_CONN_NULL:
        return "HFOC_LWIP_TCP_CONN_NULL";
    case HFOC_LWIP_TCP_CONN_BAD_STATE:
        return "HFOC_LWIP_TCP_CONN_BAD_STATE";
    case HFOC_LWIP_TCP_CONN_INVALID_VERSION:
        return "HFOC_LWIP_TCP_CONN_INVALID_VERSION";
    case HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED:
        return "HFOC_LWIP_TCP_CONN_ALREADY_PUBLISHED";
    case HFOC_LWIP_TCP_CONN_UNPUBLISHED:
        return "HFOC_LWIP_TCP_CONN_UNPUBLISHED";
    case HFOC_LWIP_TCP_CONN_INVALID_GENERATION:
        return "HFOC_LWIP_TCP_CONN_INVALID_GENERATION";
    case HFOC_LWIP_TCP_CONN_INVALID_CALLBACK:
        return "HFOC_LWIP_TCP_CONN_INVALID_CALLBACK";
    case HFOC_LWIP_TCP_CONN_INVALID_PCB:
        return "HFOC_LWIP_TCP_CONN_INVALID_PCB";
    case HFOC_LWIP_TCP_CONN_STALE_CALLBACK:
        return "HFOC_LWIP_TCP_CONN_STALE_CALLBACK";
    case HFOC_LWIP_TCP_CONN_ABORTED:
        return "HFOC_LWIP_TCP_CONN_ABORTED";
    case HFOC_LWIP_TCP_CONN_CLOSE_FAILED:
        return "HFOC_LWIP_TCP_CONN_CLOSE_FAILED";
    case HFOC_LWIP_TCP_CONN_NOT_ESTABLISHED:
        return "HFOC_LWIP_TCP_CONN_NOT_ESTABLISHED";
    case HFOC_LWIP_TCP_CONN_ALREADY_CLOSED:
        return "HFOC_LWIP_TCP_CONN_ALREADY_CLOSED";
    default:
        return NULL;
    }
}

#else

typedef int hfoc_lwip_tcp_conn_not_compiled;

#endif
