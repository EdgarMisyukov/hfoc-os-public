#include "virtio_net_interrupt.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef HFOC_VIRTIO_NET_INTERRUPT_HOST_TEST

static int failures;
static unsigned int alloc_attempts;
static unsigned int free_attempts;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        ++failures; \
    } else { \
        printf("PASS: %s\n", msg); \
    } \
} while (0)

void *kmalloc(size_t size)
{
    (void)size;
    ++alloc_attempts;
    return NULL;
}

bool kfree(void *ptr)
{
    (void)ptr;
    ++free_attempts;
    return false;
}

struct test_io_state {
    bool read_result;
    uint8_t status;
    unsigned int calls;
};

struct test_work_state {
    bool has_work;
    unsigned int calls;
};

struct test_publish_state {
    unsigned int calls;
    struct virtio_net_interrupt_work last;
};

struct interrupt_fixture {
    struct test_io_state io;
    struct test_work_state receive;
    struct test_work_state transmit;
    struct test_publish_state publication;
    struct virtio_net_interrupt_io io_seam;
    struct virtio_net_interrupt_direction receive_seam;
    struct virtio_net_interrupt_direction transmit_seam;
    struct virtio_net_interrupt_publication publication_seam;
    struct virtio_net_interrupt interrupt;
};

static bool test_read_status(void *context, uint8_t *status)
{
    struct test_io_state *state = (struct test_io_state *)context;

    ++state->calls;
    if (!state->read_result) {
        return false;
    }
    *status = state->status;
    return true;
}

static bool test_has_work(void *context)
{
    struct test_work_state *state = (struct test_work_state *)context;

    ++state->calls;
    return state->has_work;
}

static void test_publish(
    void *context,
    const struct virtio_net_interrupt_work *work)
{
    struct test_publish_state *state =
        (struct test_publish_state *)context;

    ++state->calls;
    if (work != NULL) {
        state->last = *work;
    }
}

static void make_seams(
    struct interrupt_fixture *fixture)
{
    memset(fixture, 0, sizeof(*fixture));
    fixture->io.read_result = true;
    fixture->io_seam.read_status = test_read_status;
    fixture->io_seam.context = &fixture->io;
    fixture->receive_seam.has_work = test_has_work;
    fixture->receive_seam.context = &fixture->receive;
    fixture->transmit_seam.has_work = test_has_work;
    fixture->transmit_seam.context = &fixture->transmit;
    fixture->publication_seam.publish = test_publish;
    fixture->publication_seam.context = &fixture->publication;
}

static void activate_fixture(
    struct interrupt_fixture *fixture,
    uint8_t status)
{
    make_seams(fixture);
    fixture->io.status = status;
    CHECK(virtio_net_interrupt_activate(
              &fixture->interrupt,
              &fixture->io_seam,
              &fixture->receive_seam,
              &fixture->transmit_seam,
              &fixture->publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_OK,
          "interrupt fixture activates");
}

static void test_status_strings(void)
{
    static const struct {
        enum virtio_net_interrupt_status status;
        const char *string;
    } cases[] = {
        {VIRTIO_NET_INTERRUPT_OK, "VIRTIO_NET_INTERRUPT_OK"},
        {VIRTIO_NET_INTERRUPT_NULL, "VIRTIO_NET_INTERRUPT_NULL"},
        {VIRTIO_NET_INTERRUPT_INVALID_VERSION,
         "VIRTIO_NET_INTERRUPT_INVALID_VERSION"},
        {VIRTIO_NET_INTERRUPT_INVALID_GENERATION,
         "VIRTIO_NET_INTERRUPT_INVALID_GENERATION"},
        {VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED,
         "VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED"},
        {VIRTIO_NET_INTERRUPT_UNPUBLISHED,
         "VIRTIO_NET_INTERRUPT_UNPUBLISHED"},
        {VIRTIO_NET_INTERRUPT_INVALID_IO,
         "VIRTIO_NET_INTERRUPT_INVALID_IO"},
        {VIRTIO_NET_INTERRUPT_INVALID_DIRECTION,
         "VIRTIO_NET_INTERRUPT_INVALID_DIRECTION"},
        {VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION,
         "VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION"},
        {VIRTIO_NET_INTERRUPT_INVALID_STATUS,
         "VIRTIO_NET_INTERRUPT_INVALID_STATUS"},
        {VIRTIO_NET_INTERRUPT_ACK_FAILED,
         "VIRTIO_NET_INTERRUPT_ACK_FAILED"},
        {VIRTIO_NET_INTERRUPT_SPURIOUS,
         "VIRTIO_NET_INTERRUPT_SPURIOUS"},
        {VIRTIO_NET_INTERRUPT_EMPTY_WORK,
         "VIRTIO_NET_INTERRUPT_EMPTY_WORK"},
    };
    size_t index;

    for (index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const char *string = virtio_net_interrupt_status_string(
            cases[index].status);

        CHECK(string != NULL && strcmp(string, cases[index].string) == 0,
              "interrupt status strings match");
    }
    CHECK(virtio_net_interrupt_status_string(
              (enum virtio_net_interrupt_status)999) == NULL,
          "interrupt unknown status string is NULL");
}

static void test_activate_lifecycle_and_invalid_seams(void)
{
    struct interrupt_fixture fixture;
    struct virtio_net_interrupt interrupt;
    struct virtio_net_interrupt_work work;

    memset(&interrupt, 0, sizeof(interrupt));
    make_seams(&fixture);
    CHECK(virtio_net_interrupt_activate(
              NULL,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_NULL,
          "interrupt activation rejects NULL controller");
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              NULL,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_NULL,
          "interrupt activation rejects NULL IO as NULL");

    memset(&interrupt, 0, sizeof(interrupt));
    fixture.io_seam.read_status = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_IO,
          "interrupt activation rejects missing read seam");
    fixture.io_seam.read_status = test_read_status;
    fixture.io_seam.context = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_IO,
          "interrupt activation rejects NULL IO context");

    fixture.io_seam.context = &fixture.io;
    fixture.receive_seam.has_work = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_DIRECTION,
          "interrupt activation rejects missing RX has-work seam");
    fixture.receive_seam.has_work = test_has_work;
    fixture.receive_seam.context = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_DIRECTION,
          "interrupt activation rejects NULL RX direction context");

    fixture.receive_seam.context = &fixture.receive;
    fixture.transmit_seam.has_work = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_DIRECTION,
          "interrupt activation rejects missing TX has-work seam");
    fixture.transmit_seam.has_work = test_has_work;
    fixture.transmit_seam.context = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_DIRECTION,
          "interrupt activation rejects NULL TX direction context");

    fixture.transmit_seam.context = &fixture.transmit;
    fixture.publication_seam.publish = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION,
          "interrupt activation rejects missing publication seam");
    fixture.publication_seam.publish = test_publish;
    fixture.publication_seam.context = NULL;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_INVALID_PUBLICATION,
          "interrupt activation rejects NULL publication context");

    memset(&interrupt, 0, sizeof(interrupt));
    fixture.publication_seam.context = &fixture.publication;
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              0U) == VIRTIO_NET_INTERRUPT_INVALID_GENERATION,
          "interrupt activation rejects invalid generation");

    memset(&interrupt, 0, sizeof(interrupt));
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              UINT32_C(0xFFFFFFFF)) ==
              VIRTIO_NET_INTERRUPT_INVALID_GENERATION,
          "interrupt activation rejects overflow generation");

    memset(&interrupt, 0, sizeof(interrupt));
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_OK &&
              virtio_net_interrupt_published(&interrupt) &&
              virtio_net_interrupt_generation(&interrupt) == 7U &&
              virtio_net_interrupt_isr_ack_count(&interrupt) == 0U &&
              virtio_net_interrupt_published_work_count(&interrupt) == 0U,
          "interrupt activation publishes valid controller");
    CHECK(virtio_net_interrupt_activate(
              &interrupt,
              &fixture.io_seam,
              &fixture.receive_seam,
              &fixture.transmit_seam,
              &fixture.publication_seam,
              7U) == VIRTIO_NET_INTERRUPT_ALREADY_PUBLISHED,
          "interrupt activation rejects double publication");

    memset(&work, 0, sizeof(work));
    CHECK(virtio_net_interrupt_handle(NULL, &work) ==
              VIRTIO_NET_INTERRUPT_NULL,
          "interrupt handle rejects NULL controller");
    CHECK(virtio_net_interrupt_handle(&interrupt, NULL) ==
              VIRTIO_NET_INTERRUPT_NULL,
          "interrupt handle rejects NULL work output");
    memset(&interrupt, 0, sizeof(interrupt));
    CHECK(virtio_net_interrupt_handle(&interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_UNPUBLISHED,
          "interrupt handle rejects unpublished controller");
}

static void test_ack_failure_and_invalid_status(void)
{
    struct interrupt_fixture fixture;
    struct virtio_net_interrupt_work work;

    activate_fixture(&fixture, 0U);
    fixture.io.read_result = false;
    memset(&work, 0xAA, sizeof(work));
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_ACK_FAILED &&
              fixture.io.calls == 1U &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 0U &&
              virtio_net_interrupt_spurious_count(&fixture.interrupt) == 0U &&
              fixture.receive.calls == 0U &&
              fixture.transmit.calls == 0U &&
              fixture.publication.calls == 0U &&
              work.generation == 0U &&
              !work.receive &&
              !work.transmit,
          "interrupt read failure acknowledges nothing");

    fixture.io.read_result = true;
    fixture.io.status = 0x04U;
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_INVALID_STATUS &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_spurious_count(&fixture.interrupt) == 0U &&
              fixture.receive.calls == 0U &&
              fixture.transmit.calls == 0U &&
              fixture.publication.calls == 0U,
          "unknown ISR status fails closed without work publication");
}

static void test_spurious_and_config_only(void)
{
    struct interrupt_fixture fixture;
    struct virtio_net_interrupt_work work;

    activate_fixture(&fixture, 0U);
    memset(&work, 0xAA, sizeof(work));
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_SPURIOUS &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_spurious_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_published_work_count(
                  &fixture.interrupt) == 0U &&
              fixture.receive.calls == 0U &&
              fixture.transmit.calls == 0U &&
              fixture.publication.calls == 0U &&
              work.generation == 0U &&
              !work.receive &&
              !work.transmit,
          "shared/spurious IRQ consumes no RX/TX work");

    fixture.io.status = VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG;
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_OK &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 2U &&
              virtio_net_interrupt_config_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_used_ring_count(&fixture.interrupt) == 0U &&
              virtio_net_interrupt_empty_work_count(&fixture.interrupt) == 0U &&
              fixture.receive.calls == 0U &&
              fixture.transmit.calls == 0U &&
              fixture.publication.calls == 0U,
          "config-change ISR is counted without RX/TX work");
}

static void test_empty_used_ring(void)
{
    struct interrupt_fixture fixture;
    struct virtio_net_interrupt_work work;

    activate_fixture(
        &fixture, VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING);
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_EMPTY_WORK &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_used_ring_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_empty_work_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_published_work_count(
                  &fixture.interrupt) == 0U &&
              fixture.receive.calls == 1U &&
              fixture.transmit.calls == 1U &&
              fixture.publication.calls == 0U &&
              work.generation == 0U &&
              !work.receive &&
              !work.transmit,
          "empty used-ring ISR publishes no nonexistent work");
}

static void test_rx_tx_and_combined_publication(void)
{
    struct interrupt_fixture fixture;
    struct virtio_net_interrupt_work work;

    activate_fixture(
        &fixture, VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING);
    fixture.receive.has_work = true;
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_OK &&
              virtio_net_interrupt_isr_ack_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_used_ring_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_empty_work_count(&fixture.interrupt) == 0U &&
              virtio_net_interrupt_published_work_count(
                  &fixture.interrupt) == 1U &&
              fixture.receive.calls == 1U &&
              fixture.transmit.calls == 1U &&
              fixture.publication.calls == 1U &&
              fixture.publication.last.generation == 7U &&
              fixture.publication.last.receive &&
              !fixture.publication.last.transmit,
          "RX-only used-ring work publishes one tagged deferred item");

    activate_fixture(
        &fixture, VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING);
    fixture.transmit.has_work = true;
    memset(&work, 0, sizeof(work));
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_OK &&
              fixture.publication.calls == 1U &&
              fixture.publication.last.generation == 7U &&
              !fixture.publication.last.receive &&
              fixture.publication.last.transmit &&
              work.generation == 7U &&
              !work.receive &&
              work.transmit,
          "TX-only used-ring work publishes one tagged deferred item");

    activate_fixture(
        &fixture,
        VIRTIO_NET_INTERRUPT_ISR_STATUS_USED_RING |
        VIRTIO_NET_INTERRUPT_ISR_STATUS_CONFIG);
    fixture.receive.has_work = true;
    fixture.transmit.has_work = true;
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_OK &&
              virtio_net_interrupt_config_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_used_ring_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_published_work_count(
                  &fixture.interrupt) == 1U &&
              fixture.publication.calls == 1U &&
              fixture.publication.last.generation == 7U &&
              fixture.publication.last.receive &&
              fixture.publication.last.transmit,
          "combined used/config ISR publishes one RX/TX deferred item");

    fixture.io.status = 0U;
    CHECK(virtio_net_interrupt_handle(&fixture.interrupt, &work) ==
              VIRTIO_NET_INTERRUPT_SPURIOUS &&
              virtio_net_interrupt_spurious_count(&fixture.interrupt) == 1U &&
              virtio_net_interrupt_published_work_count(
                  &fixture.interrupt) == 1U,
          "repeated shared IRQ after work does not publish duplicate work");
}

int main(void)
{
    test_status_strings();
    test_activate_lifecycle_and_invalid_seams();
    test_ack_failure_and_invalid_status();
    test_spurious_and_config_only();
    test_empty_used_ring();
    test_rx_tx_and_combined_publication();

    if (failures != 0) {
        fprintf(stderr,
                "Virtio net interrupt host tests: %d FAILURES\n",
                failures);
        return 1;
    }
    if (alloc_attempts != 0U || free_attempts != 0U) {
        fprintf(stderr,
                "Virtio net interrupt host tests allocated memory\n");
        return 1;
    }
    printf("Virtio net interrupt host tests: ALL OK\n");
    return 0;
}

#else
#error "HFOC_VIRTIO_NET_INTERRUPT_HOST_TEST must be defined for host fixture"
#endif
