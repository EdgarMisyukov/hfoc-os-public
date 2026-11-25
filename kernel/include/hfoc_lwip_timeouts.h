#ifndef HFOC_LWIP_TIMEOUTS_H
#define HFOC_LWIP_TIMEOUTS_H

#include <stdint.h>

/*
 * ITEM-605: the one production entry point that services lwIP's NO_SYS timeout
 * list.
 *
 * The HFOC adapter in hfoc_lwip_time.c owns its own callback slots; it does
 * not advance the lwIP list that sys_timeouts_init() creates.  On a NO_SYS
 * build lwIP requires the main loop to call sys_check_timeouts(), otherwise
 * the TCP retransmission timer, tcp_poll(), ARP ageing and the other cyclic
 * handlers never run.  This module is that call, kept in one small unit so the
 * host proof exercises the same function the production loop calls.
 *
 * The timeout list is bounded by MEMP_NUM_SYS_TIMEOUT, and this entry point
 * runs it at most once per loop turn; sys_check_timeouts() itself processes
 * only the timers already due at entry.
 */
#define HFOC_LWIP_TIMEOUTS_VERSION 1U

/*
 * Run the due lwIP timeouts once and return the number of service calls made
 * since boot (saturating at UINT32_MAX).  Calling it before lwIP is
 * initialised is safe: the timer list is empty and the call is a no-op.
 */
uint32_t hfoc_lwip_timeouts_service(void);

/* The same counter, without servicing anything. */
uint32_t hfoc_lwip_timeouts_service_count(void);

#endif
