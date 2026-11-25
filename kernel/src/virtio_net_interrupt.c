#include "virtio_net_interrupt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static bool virtio_net_interrupt_generation_valid(uint32_t generation)
{
    return generation >= VIRTIO_NET_INTERRUPT_GENERATION_MIN &&
           generation <= VIRTIO_NET_INTERRUPT_GENERATION_MAX;
}

static bool virtio_net_interrupt_io_valid(
    const struct virtio_net_interrupt_io *io)
{
    return io != NULL &&
           io->read_status != NULL &&
           io->context != NULL;
}

static bool virtio_net_interrupt_direction_valid(
    const struct virtio_net_interrupt_direction *direction)
{
    return direction != NULL &&
           direction->has_work != NULL &&
           direction->context != NULL;
}

static bool virtio_net_interrupt_publication_valid(
    const struct virtio_net_interrupt_publication *publication)
{
    return publication != NULL &&
           publication->publish != NULL &&
           publication->context != NULL;
}

enum virtio_net_interrupt_status virtio_net_interrupt_activate(
    struct virtio_net_interrupt *interrupt,
    const struct virtio_net_interrupt_io *io,
    const struct virtio_net_interrupt_direction *receive,
    const struct virtio_net_interrupt_direction *transmit,
    const struct virtio_net_interrupt_publication *publication,
    uint32_t generation)
{
    if (interrupt == NULL ||
        io == NULL ||
        receive == NULL ||
        transmit == NULL ||
        publication == NULL) {
        return VIRTIO_NET_INTERRUPT_NULL;
    }
    if (interrupt->published) {
        return VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED;
    }
    if (interrupt->version != 0U) {
        return VIRTIO_NET_INTERRUPT_INVALID_VERSION;
    }
    if (!virtio_net_interrupt_generation_valid(generation)) {
        return VIRTIO_NET_INTERRUPT_INVALID_GENERATION;
    }
    if (!virtio_net_interrupt_io_valid(io)) {
        return VIRTIO_NET_INTERRUPT_INVALID_IO;
    }
    if (!virtio_net_interrupt_direction_valid(receive) ||
        !virtio_net_interrupt_direction_valid(transmit)) {
        return VIRTIO_NET_INTERRUPT_INVALID_DIRECTION;
    }
    if (!virtio_net_interrupt_publication_valid(publication)) {
        return VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION;
    }

    memset(interrupt, 0, sizeof(*interrupt));
    interrupt->published = true;
    interrupt->version = VIRTIO_NET_INTERRUPT_VERSION;
    interrupt->generation = generation;
    interrupt->io = *io;
    interrupt->receive = *receive;
    interrupt->transmit = *transmit;
    interrupt->publication = *publication;
    return VIRTIO_NET_INTERRUPT_OK;
}

enum virtio_net_interrupt_status virtio_net_interrupt_handle(
    struct virtio_net_interrupt *interrupt,
    struct virtio_net_interrupt_work *work_out)
{
    uint8_t status;
    bool receive_work;
    bool transmit_work;

    if (interrupt == NULL || work_out == NULL) {
        return VIRTIO_NET_INTERRUPT_NULL;
    }
    memset(work_out, 0, sizeof(*work_out));
    if (!interrupt->published) {
        return VIRTIO_NET_INTERRUPT_UNPUBLISHED;
    }
    if (interrupt->version != VIRTIO_NET_INTERRUPT_VERSION) {
        return VIRTIO_NET_INTERRUPT_INVALID_VERSION;
    }
    if (!virtio_net_interrupt_generation_valid(interrupt->generation)) {
        return VIRTIO_NET_INTERRUPT_INVALID_GENERATION;
    }
    if (!virtio_net_interrupt_io_valid(&interrupt->io) ||
        !virtio_net_interrupt_direction_valid(&interrupt->receive) ||
        !virtio_net_interrupt_direction_valid(&interrupt->transmit) ||
        !virtio_net_interrupt_publication_valid(&interrupt->publication)) {
        return VIRTIO_NET_INTERRUPT_INVALID_IO;
    }

    if (!interrupt->io.read_status(interrupt->io.context, &status)) {
        return VIRTIO_NET_INTERRUPT_ACK_FAILED;
    }
    ++interrupt->isr_ack_count;
    if ((status & ~VIRTIO_NET_INTERRUPT_ISR_STATUS_MASK) != 0U) {
        return VIRTIO_NET_INTERRUPT_INVALID_STATUS;
    }
    if (status == 0U) {
        ++interrupt->spurious_count;
        return VIRTIO_NET_INTERRUPT_SPURIOUS;
    }

    if ((status & VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG) != 0U) {
        ++interrupt->config_count;
    }
    if ((status & VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING) == 0U) {
        return VIRTIO_NET_INTERRUPT_OK;
    }

    ++interrupt->used_ring_count;
    receive_work = interrupt->receive.has_work(interrupt->receive.context);
    transmit_work =
        interrupt->transmit.has_work(interrupt->transmit.context);
    if (!receive_work && !transmit_work) {
        ++interrupt->empty_work_count;
        return VIRTIO_NET_INTERRUPT_EMPTY_WORK;
    }

    work_out->generation = interrupt->generation;
    work_out->receive = receive_work;
    work_out->transmit = transmit_work;
    ++interrupt->published_work_count;
    interrupt->publication.publish(
        interrupt->publication.context, work_out);
    return VIRTIO_NET_INTERRUPT_OK;
}

bool virtio_net_interrupt_published(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt != NULL && interrupt->published;
}

uint32_t virtio_net_interrupt_generation(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->generation;
}

uint32_t virtio_net_interrupt_isr_ack_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->isr_ack_count;
}

uint32_t virtio_net_interrupt_used_ring_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->used_ring_count;
}

uint32_t virtio_net_interrupt_config_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->config_count;
}

uint32_t virtio_net_interrupt_empty_work_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->empty_work_count;
}

uint32_t virtio_net_interrupt_spurious_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->spurious_count;
}

uint32_t virtio_net_interrupt_published_work_count(
    const struct virtio_net_interrupt *interrupt)
{
    return interrupt == NULL ? 0U : interrupt->published_work_count;
}

const char *virtio_net_interrupt_status_string(
    enum virtio_net_interrupt_status status)
{
    switch (status) {
    case VIRTIO_NET_INTERRUPT_OK:
        return "VIRTIO_NET_INTERRUPT_OK";
    case VIRTIO_NET_INTERRUPT_NULL:
        return "VIRTIO_NET_INTERRUPT_NULL";
    case VIRTIO_NET_INTERRUPT_INVALID_VERSION:
        return "VIRTIO_NET_INTERRUPT_INVALID_VERSION";
    case VIRTIO_NET_INTERRUPT_INVALID_GENERATION:
        return "VIRTIO_NET_INTERRUPT_INVALID_GENERATION";
    case VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED:
        return "VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED";
    case VIRTIO_NET_INTERRUPT_UNPUBLISHED:
        return "VIRTIO_NET_INTERRUPT_UNPUBLISHED";
    case VIRTIO_NET_INTERRUPT_INVALID_IO:
        return "VIRTIO_NET_INTERRUPT_INVALID_IO";
    case VIRTIO_NET_INTERRUPT_INVALID_DIRECTION:
        return "VIRTIO_NET_INTERRUPT_INVALID_DIRECTION";
    case VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION:
        return "VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION";
    case VIRTIO_NET_INTERRUPT_INVALID_STATUS:
        return "VIRTIO_NET_INTERRUPT_INVALID_STATUS";
    case VIRTIO_NET_INTERRUPT_ACK_FAILED:
        return "VIRTIO_NET_INTERRUPT_ACK_FAILED";
    case VIRTIO_NET_INTERRUPT_SPURIOUS:
        return "VIRTIO_NET_INTERRUPT_SPURIOUS";
    case VIRTIO_NET_INTERRUPT_EMPTY_WORK:
        return "VIRTIO_NET_INTERRUPT_EMPTY_WORK";
    default:
        return NULL;
    }
}

