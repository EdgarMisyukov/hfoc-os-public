#include "appliance_loop_heartbeat.h"

#include <stddef.h>

static bool appliance_loop_heartbeat_elapsed(
    uint32_t now_ms,
    uint32_t last_ms)
{
    /*
     * The signed difference is the rollover-safe comparison the rest of the
     * kernel uses for its millisecond clock: it also means a clock that went
     * backwards (or stopped) cannot arm a storm of heartbeats.
     */
    return (int32_t)(now_ms - last_ms) >=
           (int32_t)APPLIANCE_LOOP_HEARTBEAT_INTERVAL_MS;
}

bool appliance_loop_heartbeat_due(
    struct appliance_loop_heartbeat *heartbeat,
    uint32_t now_ms)
{
    if (heartbeat == NULL) {
        return false;
    }
    if (!heartbeat->started) {
        heartbeat->started = true;
        heartbeat->last_ms = now_ms;
        ++heartbeat->interval_index;
        return true;
    }
    if (!appliance_loop_heartbeat_elapsed(now_ms, heartbeat->last_ms)) {
        return false;
    }
    heartbeat->last_ms = now_ms;
    ++heartbeat->interval_index;
    return true;
}
