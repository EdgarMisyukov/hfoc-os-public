#ifndef HFOC_EVENT_RING_H
#define HFOC_EVENT_RING_H

#include <stdbool.h>
#include <stdint.h>

#define KERNEL_EVENT_RING_CAPACITY 64U

enum event_ring_status {
    EVENT_RING_OK = 0,
    EVENT_RING_NULL = 1,
    EVENT_RING_BAD_STATE = 2,
    EVENT_RING_FULL = 3,
    EVENT_RING_EMPTY = 4,
    EVENT_RING_OVERFLOW = 5,
};

struct kernel_event {
    uint64_t sequence;
    uint32_t type;
    uint32_t flags;
    uint64_t data[3];
};

struct event_ring {
    bool published;
    uint64_t next_sequence;
    uint64_t write_cursor;
    uint64_t read_cursor;
    uint32_t count;
    struct kernel_event events[KERNEL_EVENT_RING_CAPACITY];
};

enum event_ring_status event_ring_init(struct event_ring *ring);
enum event_ring_status event_ring_destroy(struct event_ring *ring);
enum event_ring_status event_ring_push(
    struct event_ring *ring,
    const struct kernel_event *event);
enum event_ring_status event_ring_pop(
    struct event_ring *ring,
    struct kernel_event *event_out);
enum event_ring_status event_ring_snapshot(
    const struct event_ring *ring,
    struct kernel_event *events_out,
    uint32_t capacity,
    uint32_t *count_out);
uint32_t event_ring_count(const struct event_ring *ring);
uint32_t event_ring_capacity(const struct event_ring *ring);
uint64_t event_ring_write_cursor(const struct event_ring *ring);
uint64_t event_ring_read_cursor(const struct event_ring *ring);
bool event_ring_is_published(const struct event_ring *ring);
const char *event_ring_status_string(enum event_ring_status status);
struct event_ring *kernel_event_ring_get(void);

#endif
