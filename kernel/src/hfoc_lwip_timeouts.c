#include "hfoc_lwip_timeouts.h"

#if defined(HFOC_PRODUCTION_LWIP) || defined(HFOC_LWIP_TIMEOUTS_HOST_TEST)

#include "lwip/timeouts.h"

static uint32_t hfoc_lwip_timeouts_calls;

uint32_t hfoc_lwip_timeouts_service(void)
{
    /*
     * ITEM-605: the production loop's one bounded turn of lwIP's NO_SYS
     * timeout list.  sys_check_timeouts() processes only the timers already
     * due at entry and the list is bounded by MEMP_NUM_SYS_TIMEOUT.
     */
    sys_check_timeouts();
    if (hfoc_lwip_timeouts_calls != UINT32_MAX) {
        ++hfoc_lwip_timeouts_calls;
    }
    return hfoc_lwip_timeouts_calls;
}

uint32_t hfoc_lwip_timeouts_service_count(void)
{
    return hfoc_lwip_timeouts_calls;
}

#else

uint32_t hfoc_lwip_timeouts_service(void)
{
    return 0U;
}

uint32_t hfoc_lwip_timeouts_service_count(void)
{
    return 0U;
}

#endif
