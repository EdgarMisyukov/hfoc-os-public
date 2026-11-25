#ifndef HFOC_LWIP_TIME_H
#define HFOC_LWIP_TIME_H

#include <stdbool.h>
#include <stdint.h>

#define HFOC_LWIP_TIME_VERSION 1U
#define HFOC_LWIP_TIME_SLOT_COUNT 16U
#define HFOC_LWIP_TIME_DELAY_MAX (UINT32_MAX / 4U)
#define HFOC_LWIP_TIME_TOKEN_INVALID 0U
#define HFOC_LWIP_TIME_NEXT_DEADLINE_NONE UINT32_MAX

typedef void (*hfoc_lwip_time_callback)(void *arg);
typedef uint32_t (*hfoc_lwip_time_source)(void);

struct hfoc_lwip_time_snapshot {
    uint32_t version;
    uint32_t now_ms;
    uint32_t active_count;
    uint32_t invoked_count;
    uint32_t scheduled_count;
    uint32_t next_deadline_ms;
};

#ifndef HFOC_LWIP_TIME_HOST_TEST
bool hfoc_lwip_time_init_default(void);
#endif

bool hfoc_lwip_time_init(hfoc_lwip_time_source source);
void hfoc_lwip_time_shutdown(void);
bool hfoc_lwip_time_ready(void);
uint32_t hfoc_lwip_time_now(void);
uint32_t hfoc_lwip_time_arm(
    uint32_t delay_ms,
    hfoc_lwip_time_callback callback,
    void *arg,
    uint32_t generation);
bool hfoc_lwip_time_disarm(uint32_t token, uint32_t generation);
uint32_t hfoc_lwip_time_teardown_generation(uint32_t generation);
uint32_t hfoc_lwip_time_poll(void);
uint32_t hfoc_lwip_time_next_deadline_ms(void);
struct hfoc_lwip_time_snapshot hfoc_lwip_time_snapshot(void);

#endif
