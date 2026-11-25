#ifndef HFOC_VIRTIO_NET_INTERRUPT_H
#define HFOC_VIRTIO_NET_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

#define VIRTIO_NET_INTERRUPT_VERSION 1U
#define VIRTIO_NET_INTERRUPT_GENERATION_MIN 1U
#define VIRTIO_NET_INTERRUPT_GENERATION_MAX UINT32_C(0xFFFFFFFE)

#define VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING 0x01U
#define VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG 0x02U
#define VIRTIO_NET_INTERRUPT_ISR_STATUS_MASK \
    (VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING | \
     VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG)

struct virtio_net_interrupt_work {
    uint32_t generation;
    bool receive;
    bool transmit;
};

struct virtio_net_interrupt_io {
    bool (*read_status)(void *context, uint8_t *status);
    void *context;
};

struct virtio_net_interrupt_direction {
    bool (*has_work)(void *context);
    void *context;
};

struct virtio_net_interrupt_publication {
    void (*publish)(
        void *context,
        const struct virtio_net_interrupt_work *work);
    void *context;
};

struct virtio_net_interrupt {
    bool published;
    uint32_t version;
    uint32_t generation;
    uint32_t isr_ack_count;
    uint32_t used_ring_count;
    uint32_t config_count;
    uint32_t empty_work_count;
    uint32_t spurious_count;
    uint32_t published_work_count;
    struct virtio_net_interrupt_io io;
    struct virtio_net_interrupt_direction receive;
    struct virtio_net_interrupt_direction transmit;
    struct virtio_net_interrupt_publication publication;
};

enum virtio_net_interrupt_status {
    VIRTIO_NET_INTERRUPT_OK = 0,
    VIRTIO_NET_INTERRUPT_NULL = 1,
    VIRTIO_NET_INTERRUPT_INVALID_VERSION = 2,
    VIRTIO_NET_INTERRUPT_INVALID_GENERATION = 3,
    VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED = 4,
    VIRTIO_NET_INTERRUPT_UNPUBLISHED = 5,
    VIRTIO_NET_INTERRUPT_INVALID_IO = 6,
    VIRTIO_NET_INTERRUPT_INVALID_DIRECTION = 7,
    VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION = 8,
    VIRTIO_NET_INTERRUPT_INVALID_STATUS = 9,
    VIRTIO_NET_INTERRUPT_ACK_FAILED = 10,
    VIRTIO_NET_INTERRUPT_SPURIOUS = 11,
    VIRTIO_NET_INTERRUPT_EMPTY_WORK = 12,
};

enum virtio_net_interrupt_status virtio_net_interrupt_activate(
    struct virtio_net_interrupt *interrupt,
    const struct virtio_net_interrupt_io *io,
    const struct virtio_net_interrupt_direction *receive,
    const struct virtio_net_interrupt_direction *transmit,
    const struct virtio_net_interrupt_publication *publication,
    uint32_t generation);
enum virtio_net_interrupt_status virtio_net_interrupt_handle(
    struct virtio_net_interrupt *interrupt,
    struct virtio_net_interrupt_work *work_out);
bool virtio_net_interrupt_published(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_generation(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_isr_ack_count(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_used_ring_count(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_config_count(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_empty_work_count(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_spurious_count(
    const struct virtio_net_interrupt *interrupt);
uint32_t virtio_net_interrupt_published_work_count(
    const struct virtio_net_interrupt *interrupt);
const char *virtio_net_interrupt_status_string(
    enum virtio_net_interrupt_status status);

#endif

