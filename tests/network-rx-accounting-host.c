/*
 * ITEM-617: the receive counter must follow the frame, not the zero value of
 * the lwIP input enum.  The old call sites counted an ACCEPTED frame (status
 * zero) as dropped and a REJECTED one as received.
 */
#include "appliance_production_network_rx_accounting.h"

#include <stdio.h>

static int failures;

#define CHECK(condition, ...)                                                 \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL: ");                                        \
            fprintf(stderr, __VA_ARGS__);                                     \
            fprintf(stderr, "\n");                                            \
            ++failures;                                                       \
        }                                                                     \
    } while (0)

int main(void)
{
    CHECK(appliance_production_network_rx_frame_received(true, false),
          "a frame consumed by the wire was not counted as received");
    CHECK(appliance_production_network_rx_frame_received(true, true),
          "a frame consumed by the wire and lwIP was not counted");
    CHECK(appliance_production_network_rx_frame_received(false, true),
          "a frame accepted by lwIP was not counted as received");
    CHECK(!appliance_production_network_rx_frame_received(false, false),
          "a rejected frame was counted as received");

    if (failures != 0) {
        fprintf(stderr, "network rx accounting host: %d failure(s)\n",
                failures);
        return 1;
    }
    printf("network rx accounting host: ALL OK\n");
    return 0;
}
