#include "event_ring.h"

#include <stddef.h>
#include <string.h>

enum event_ring_status event_ring_init(struct event_ring *ring) {
    if (ring == NULL) {
        return EVENT_RING_NULL;
    }
    if (ring->published) {
        return EVENT_RING_BAD_STATE;
    }
    memset(ring, 0, sizeof(*ring));
    ring->published = true;
    return EVENT_RING_OK;
}

enum event_ring_status event_ring_destroy(struct event_ring *ring) {
    if (ring == NULL) {
        return EVENT_RING_NULL;
    }
    if (!ring->published) {
        return EVENT_RING_BAD_STATE;
    }
    memset(ring, 0, sizeof(*ring));
    return EVENT_RING_OK;
}

enum event_ring_status event_ring_push(
    struct event_ring *ring,
    const struct kernel_event *event)
{
    struct kernel_event *slot;

    if (ring == NULL || event == NULL) {
        return EVENT_RING_NULL;
    }
    if (!ring->published) {
        return EVENT_RING_BAD_STATE;
    }
    if (ring->count == KERNEL_EVENT_RING_CAPACITY) {
        return EVENT_RING_FULL;
    }

    slot = &ring->events[ring->write_cursor % KERNEL_EVENT_RING_CAPACITY];
    slot->sequence = ring->next_sequence;
    slot->type = event->type;
    slot->flags = event->flags;
    slot->data[0] = event->data[0];
    slot->data[1] = event->data[1];
    slot->data[2] = event->data[2];
    ++ring->next_sequence;
    ++ring->write_cursor;
    ++ring->count;
    return EVENT_RING_OK;
}

enum event_ring_status event_ring_pop(
    struct event_ring *ring,
    struct kernel_event *event_out)
{
    const struct kernel_event *slot;

    if (ring == NULL || event_out == NULL) {
        return EVENT_RING_NULL;
    }
    if (!ring->published) {
        return EVENT_RING_BAD_STATE;
    }
    if (ring->count == 0U) {
        return EVENT_RING_EMPTY;
    }

    slot = &ring->events[ring->read_cursor % KERNEL_EVENT_RING_CAPACITY];
    *event_out = *slot;
    ++ring->read_cursor;
    --ring->count;
    return EVENT_RING_OK;
}

enum event_ring_status event_ring_snapshot(
    const struct event_ring *ring,
    struct kernel_event *events_out,
    uint32_t capacity,
    uint32_t *count_out)
{
    const struct kernel_event *slot;
    uint32_t index;

    if (ring == NULL || count_out == NULL) {
        return EVENT_RING_NULL;
    }
    if ((events_out == NULL) != (capacity == 0U)) {
        return EVENT_RING_NULL;
    }
    if (!ring->published) {
        return EVENT_RING_BAD_STATE;
    }
    if (events_out == NULL) {
        *count_out = ring->count;
        return EVENT_RING_OK;
    }
    if (capacity < ring->count) {
        *count_out = ring->count;
        return EVENT_RING_OVERFLOW;
    }

    for (index = 0U; index < ring->count; ++index) {
        slot = &ring->events[(ring->read_cursor + index) %
                             KERNEL_EVENT_RING_CAPACITY];
        events_out[index] = *slot;
    }
    *count_out = ring->count;
    return EVENT_RING_OK;
}

uint32_t event_ring_count(const struct event_ring *ring) {
    return ring != NULL ? ring->count : 0U;
}

uint32_t event_ring_capacity(const struct event_ring *ring) {
    return ring != NULL && ring->published
        ? KERNEL_EVENT_RING_CAPACITY
        : 0U;
}

uint64_t event_ring_write_cursor(const struct event_ring *ring) {
    return ring != NULL ? ring->write_cursor : 0U;
}

uint64_t event_ring_read_cursor(const struct event_ring *ring) {
    return ring != NULL ? ring->read_cursor : 0U;
}

bool event_ring_is_published(const struct event_ring *ring) {
    return ring != NULL && ring->published;
}

const char *event_ring_status_string(enum event_ring_status status) {
    switch (status) {
    case EVENT_RING_OK:
        return "EVENT_RING_OK";
    case EVENT_RING_NULL:
        return "EVENT_RING_NULL";
    case EVENT_RING_BAD_STATE:
        return "EVENT_RING_BAD_STATE";
    case EVENT_RING_FULL:
        return "EVENT_RING_FULL";
    case EVENT_RING_EMPTY:
        return "EVENT_RING_EMPTY";
    case EVENT_RING_OVERFLOW:
        return "EVENT_RING_OVERFLOW";
    default:
        return "EVENT_RING_UNKNOWN";
    }
}
