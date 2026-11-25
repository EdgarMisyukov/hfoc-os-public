#include "hfoc_lwip_time.h"

#include <string.h>

#ifndef HFOC_LWIP_TIME_HOST_TEST
#include "arch/x86_64/timer.h"
#include "lwip/sys.h"
#endif

#define HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT 4U
#define HFOC_LWIP_TIME_TOKEN_INDEX_MASK 0xFU
#define HFOC_LWIP_TIME_TOKEN_SERIAL_MASK 0x0FFFFFFFU
#define HFOC_LWIP_TIME_HALF_RANGE UINT32_C(0x7FFFFFFF)

struct hfoc_lwip_time_slot {
    bool active;
    uint32_t generation;
    uint32_t due_ms;
    uint32_t serial;
    hfoc_lwip_time_callback callback;
    void *arg;
};

static struct hfoc_lwip_time_slot hfoc_lwip_time_slots[
    HFOC_LWIP_TIME_SLOT_COUNT];
static hfoc_lwip_time_source hfoc_lwip_time_source_fn;
static bool hfoc_lwip_time_published;
static uint32_t hfoc_lwip_time_invoked_count;
static uint32_t hfoc_lwip_time_scheduled_count;
static uint32_t hfoc_lwip_time_next_serial = 1U;

static bool hfoc_lwip_time_generation_valid(uint32_t generation)
{
    return generation != 0U;
}

static bool hfoc_lwip_time_token_valid(uint32_t token)
{
    return token != HFOC_LWIP_TIME_TOKEN_INVALID &&
           (token & HFOC_LWIP_TIME_TOKEN_INDEX_MASK) <
               HFOC_LWIP_TIME_SLOT_COUNT &&
           (token >> HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT) != 0U;
}

static unsigned int hfoc_lwip_time_token_index(uint32_t token)
{
    return (unsigned int)(token & HFOC_LWIP_TIME_TOKEN_INDEX_MASK);
}

static uint32_t hfoc_lwip_time_token_serial(uint32_t token)
{
    return token >> HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT;
}

static uint32_t hfoc_lwip_time_next_token(unsigned int index)
{
    const uint32_t serial = hfoc_lwip_time_next_serial;

    hfoc_lwip_time_next_serial =
        (serial == HFOC_LWIP_TIME_TOKEN_SERIAL_MASK) ?
            1U : serial + 1U;
    return (serial << HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT) |
           (uint32_t)index;
}

static bool hfoc_lwip_time_reached(uint32_t now_ms, uint32_t due_ms)
{
    return (uint32_t)(now_ms - due_ms) <= HFOC_LWIP_TIME_HALF_RANGE;
}

static bool hfoc_lwip_time_before(uint32_t left, uint32_t right)
{
    return (uint32_t)(left - right) > HFOC_LWIP_TIME_HALF_RANGE;
}

bool hfoc_lwip_time_init(hfoc_lwip_time_source source)
{
    if (hfoc_lwip_time_published || source == NULL) {
        return false;
    }

    memset(hfoc_lwip_time_slots, 0, sizeof(hfoc_lwip_time_slots));
    hfoc_lwip_time_source_fn = source;
    hfoc_lwip_time_invoked_count = 0U;
    hfoc_lwip_time_scheduled_count = 0U;
    hfoc_lwip_time_next_serial = 1U;
    hfoc_lwip_time_published = true;
    return true;
}

void hfoc_lwip_time_shutdown(void)
{
    memset(hfoc_lwip_time_slots, 0, sizeof(hfoc_lwip_time_slots));
    hfoc_lwip_time_source_fn = NULL;
    hfoc_lwip_time_published = false;
    hfoc_lwip_time_invoked_count = 0U;
    hfoc_lwip_time_scheduled_count = 0U;
    hfoc_lwip_time_next_serial = 1U;
}

bool hfoc_lwip_time_ready(void)
{
    return hfoc_lwip_time_published;
}

uint32_t hfoc_lwip_time_now(void)
{
    if (!hfoc_lwip_time_published ||
        hfoc_lwip_time_source_fn == NULL) {
        return 0U;
    }
    return hfoc_lwip_time_source_fn();
}

uint32_t hfoc_lwip_time_arm(
    uint32_t delay_ms,
    hfoc_lwip_time_callback callback,
    void *arg,
    uint32_t generation)
{
    unsigned int index;
    const uint32_t now_ms = hfoc_lwip_time_now();
    const uint32_t due_ms = (uint32_t)(now_ms + delay_ms);

    if (!hfoc_lwip_time_published ||
        delay_ms > HFOC_LWIP_TIME_DELAY_MAX ||
        callback == NULL ||
        !hfoc_lwip_time_generation_valid(generation)) {
        return HFOC_LWIP_TIME_TOKEN_INVALID;
    }

    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        struct hfoc_lwip_time_slot *slot =
            &hfoc_lwip_time_slots[index];

        if (slot->active) {
            continue;
        }
        slot->active = true;
        slot->generation = generation;
        slot->due_ms = due_ms;
        slot->callback = callback;
        slot->arg = arg;
        slot->serial =
            hfoc_lwip_time_next_token(index) >>
            HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT;
        ++hfoc_lwip_time_scheduled_count;
        return (slot->serial << HFOC_LWIP_TIME_TOKEN_SERIAL_SHIFT) |
               (uint32_t)index;
    }
    return HFOC_LWIP_TIME_TOKEN_INVALID;
}

bool hfoc_lwip_time_disarm(uint32_t token, uint32_t generation)
{
    struct hfoc_lwip_time_slot *slot;
    unsigned int index;

    if (!hfoc_lwip_time_published ||
        !hfoc_lwip_time_token_valid(token) ||
        !hfoc_lwip_time_generation_valid(generation)) {
        return false;
    }

    index = hfoc_lwip_time_token_index(token);
    slot = &hfoc_lwip_time_slots[index];
    if (!slot->active ||
        slot->serial != hfoc_lwip_time_token_serial(token) ||
        slot->generation != generation) {
        return false;
    }

    slot->active = false;
    slot->generation = 0U;
    slot->due_ms = 0U;
    slot->serial = 0U;
    slot->callback = NULL;
    slot->arg = NULL;
    return true;
}

uint32_t hfoc_lwip_time_teardown_generation(uint32_t generation)
{
    uint32_t cancelled = 0U;
    unsigned int index;

    if (!hfoc_lwip_time_published ||
        !hfoc_lwip_time_generation_valid(generation)) {
        return 0U;
    }

    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        struct hfoc_lwip_time_slot *slot =
            &hfoc_lwip_time_slots[index];

        if (!slot->active || slot->generation != generation) {
            continue;
        }
        slot->active = false;
        slot->generation = 0U;
        slot->due_ms = 0U;
        slot->serial = 0U;
        slot->callback = NULL;
        slot->arg = NULL;
        ++cancelled;
    }
    return cancelled;
}

uint32_t hfoc_lwip_time_poll(void)
{
    uint32_t invoked = 0U;
    uint32_t now_ms;

    if (!hfoc_lwip_time_published) {
        return 0U;
    }

    now_ms = hfoc_lwip_time_now();
    for (;;) {
        int best = -1;
        unsigned int index;

        for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
            const struct hfoc_lwip_time_slot *slot =
                &hfoc_lwip_time_slots[index];

            if (!slot->active ||
                !hfoc_lwip_time_reached(now_ms, slot->due_ms)) {
                continue;
            }
            if (best < 0 ||
                hfoc_lwip_time_before(
                    slot->due_ms,
                    hfoc_lwip_time_slots[best].due_ms)) {
                best = (int)index;
            }
        }
        if (best < 0) {
            break;
        }
        {
            struct hfoc_lwip_time_slot *slot =
                &hfoc_lwip_time_slots[best];
            hfoc_lwip_time_callback callback = slot->callback;
            void *arg = slot->arg;

            slot->active = false;
            slot->generation = 0U;
            slot->due_ms = 0U;
            slot->serial = 0U;
            slot->callback = NULL;
            slot->arg = NULL;
            ++invoked;
            ++hfoc_lwip_time_invoked_count;
            callback(arg);
        }
    }
    return invoked;
}

uint32_t hfoc_lwip_time_next_deadline_ms(void)
{
    int best = -1;
    unsigned int index;

    if (!hfoc_lwip_time_published) {
        return HFOC_LWIP_TIME_NEXT_DEADLINE_NONE;
    }
    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        const struct hfoc_lwip_time_slot *slot =
            &hfoc_lwip_time_slots[index];

        if (!slot->active) {
            continue;
        }
        if (best < 0 ||
            hfoc_lwip_time_before(
                slot->due_ms,
                hfoc_lwip_time_slots[best].due_ms)) {
            best = (int)index;
        }
    }
    return best < 0 ?
        HFOC_LWIP_TIME_NEXT_DEADLINE_NONE :
        hfoc_lwip_time_slots[best].due_ms;
}

struct hfoc_lwip_time_snapshot hfoc_lwip_time_snapshot(void)
{
    struct hfoc_lwip_time_snapshot snapshot;
    uint32_t active_count = 0U;
    unsigned int index;

    memset(&snapshot, 0, sizeof(snapshot));
    if (!hfoc_lwip_time_published) {
        snapshot.next_deadline_ms =
            HFOC_LWIP_TIME_NEXT_DEADLINE_NONE;
        return snapshot;
    }

    for (index = 0U; index < HFOC_LWIP_TIME_SLOT_COUNT; ++index) {
        if (hfoc_lwip_time_slots[index].active) {
            ++active_count;
        }
    }
    snapshot.version = HFOC_LWIP_TIME_VERSION;
    snapshot.now_ms = hfoc_lwip_time_now();
    snapshot.active_count = active_count;
    snapshot.invoked_count = hfoc_lwip_time_invoked_count;
    snapshot.scheduled_count = hfoc_lwip_time_scheduled_count;
    snapshot.next_deadline_ms = hfoc_lwip_time_next_deadline_ms();
    return snapshot;
}

#ifndef HFOC_LWIP_TIME_HOST_TEST
static uint32_t hfoc_lwip_time_kernel_source(void)
{
    return (uint32_t)(timer_ticks() * 10U);
}

bool hfoc_lwip_time_init_default(void)
{
    return hfoc_lwip_time_init(hfoc_lwip_time_kernel_source);
}

u32_t sys_now(void)
{
    return hfoc_lwip_time_now();
}
#endif
