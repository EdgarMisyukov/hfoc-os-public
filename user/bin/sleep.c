/*
 * ITEM-633: bounded native sleep, used by the job-control proofs and by the
 * owner shell.  One argument in seconds; the scheduler tick is 100 Hz.
 */
#include "hfoc.h"

int main(void)
{
    int ok = 0;
    long seconds;
    uint32_t ticks;

    if (hfoc_argc < 2) {
        hfoc_eprintf("sleep: usage: sleep SECONDS\n");
        return 2;
    }
    seconds = hfoc_parse_int(hfoc_argv[1], &ok);
    if (!ok || seconds < 0 || seconds > 1000) {
        hfoc_eprintf("sleep: bad interval\n");
        return 2;
    }
    ticks = (uint32_t)(seconds * 100);
    if (ticks == 0U) {
        ticks = 1U;
    }
    return hfoc_sleep(ticks) == 0 ? 0 : 1;
}
