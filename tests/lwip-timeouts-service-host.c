/*
 * ITEM-605: prove the production lwIP timeout entry point.
 *
 * The production kernel calls exactly hfoc_lwip_timeouts_service() from its
 * main loop.  This test links the real lwIP timeout list and the real TCP
 * timer, schedules a due timeout and a tcp_poll deadline, and advances only
 * through that entry point.  Without the sys_check_timeouts() call the list
 * never advances and every CHECK below fails - which is the measured
 * production defect, not a copy of the algorithm.
 */
#include "hfoc_lwip_timeouts.h"
#include "hfoc_lwip.h"

#include "lwip/init.h"
#include "lwip/mem.h"
#include "lwip/memp.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/priv/tcp_priv.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

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

static uint32_t one_shot_calls;

static void one_shot_callback(void *arg)
{
    (void)arg;
    ++one_shot_calls;
}

static uint32_t poll_calls;

static err_t poll_callback(void *arg, struct tcp_pcb *pcb)
{
    (void)arg;
    (void)pcb;
    ++poll_calls;
    return ERR_OK;
}

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

static void test_due_timeout_through_production_entry(void)
{
    const uint32_t before = hfoc_lwip_timeouts_service_count();

    test_now_ms = 1000U;
    one_shot_calls = 0U;
    sys_timeout(50U, one_shot_callback, NULL);

    (void)hfoc_lwip_timeouts_service();
    CHECK(one_shot_calls == 0U,
          "a timer that is not due yet does not run");
    CHECK(hfoc_lwip_timeouts_service_count() == before + 1U,
          "the production entry point services the list once per call");

    test_now_ms = 1049U;
    (void)hfoc_lwip_timeouts_service();
    CHECK(one_shot_calls == 0U,
          "the timer still does not run one millisecond early");

    test_now_ms = 1050U;
    (void)hfoc_lwip_timeouts_service();
    CHECK(one_shot_calls == 1U,
          "the due lwIP timeout runs through the production entry point");

    test_now_ms = 2000U;
    (void)hfoc_lwip_timeouts_service();
    CHECK(one_shot_calls == 1U,
          "a one-shot timeout is not replayed on a later service");
}

static void test_tcp_timer_and_poll_through_production_entry(void)
{
    struct tcp_pcb *pcb;
    u32_t ticks_before;
    unsigned int turns = 0U;

    pcb = tcp_new();
    CHECK(pcb != NULL, "a TCP control block can be allocated");
    if (pcb == NULL) {
        return;
    }
    pcb->state = ESTABLISHED;
    pcb->poll = poll_callback;
    pcb->pollinterval = 1U;
    pcb->polltmr = 0U;
    poll_calls = 0U;
    /*
     * Registering the PCB arms lwIP's 250 ms TCP timer.  tcp_slowtmr is the
     * same handler that performs retransmission, keepalive and connection
     * cleanup, and it is also what calls tcp_poll().  A production loop
     * without sys_check_timeouts() never reaches any of them.
     */
    TCP_REG_ACTIVE(pcb);
    ticks_before = tcp_ticks;
    printf("INFO: tcp_ticks before=%u\n", (unsigned)ticks_before);

    while (poll_calls == 0U && turns < 10U) {
        test_now_ms += 250U;
        (void)hfoc_lwip_timeouts_service();
        ++turns;
    }
    printf("INFO: tcp_ticks after=%u tcp_poll calls=%u turns=%u\n",
           (unsigned)tcp_ticks, (unsigned)poll_calls, turns);
    CHECK(tcp_ticks - ticks_before == 1U,
          "the lwIP TCP slow timer runs exactly once per due turn");
    CHECK(poll_calls == 1U,
          "the tcp_poll deadline runs through the production entry point");

    tcp_pcb_remove(&tcp_active_pcbs, pcb);
    tcp_free(pcb);
}

int main(void)
{
    init_lwip_core();
    printf("INFO: production timeout entry version=%u\n",
           (unsigned)HFOC_LWIP_TIMEOUTS_VERSION);
    test_due_timeout_through_production_entry();
    test_tcp_timer_and_poll_through_production_entry();
    if (failures != 0) {
        fprintf(stderr, "FAIL: %d check(s) failed\n", failures);
        return 1;
    }
    printf("PASS: all lwIP production timeout checks\n");
    return 0;
}
