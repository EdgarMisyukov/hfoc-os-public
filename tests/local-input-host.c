/*
 * ITEM-395 local input arbitration host proof.
 *
 * Two keyboards can feed one console: the PS/2 controller (IRQ driven) and a
 * USB HID boot-protocol keyboard behind an xHCI controller (polled). The
 * declared rule is that PS/2 has priority and that one pass never mixes the two
 * devices, so a keystroke on either keyboard lands once and a command line
 * typed on one device is never interleaved with the other.
 *
 * The QEMU proof can show a command arriving through the USB keyboard on a
 * machine without PS/2; it cannot show what happens when both paths exist, and
 * it cannot show that a deferred USB event is not simply lost. This proof
 * drives kernel/src/local_input.c with fake queues and asserts
 *
 *   - the published source is PS2 when the PS/2 path is ready, USB_HID when
 *     only the USB path is, and NONE when neither is, with the reason the boot
 *     record publishes for each case;
 *   - a pass delivers PS/2 first and defers the USB queue by exactly one pass,
 *     never dropping it and never interleaving the two devices;
 *   - a USB-only machine delivers USB events in the same pass, since there is
 *     nothing to arbitrate;
 *   - the merged queue is bounded, so a stalled console cannot grow memory;
 *   - the console's one-line render names the live source, the other path's
 *     verdict and the USB report count, truncates instead of overflowing, and
 *     reports the untruncated length.
 */
#ifdef HFOC_LOCAL_INPUT_HOST_TEST

#include "local_input.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++failures; } \
} while (0)

/* The production bindings reference these; the proof replaces the bindings, so
   the definitions only have to exist for the linker. */
void xhci_keyboard_service(void) {
}

bool xhci_keyboard_pop_event(struct keyboard_event *event) {
    (void)event;
    return false;
}

bool keyboard_pop_event(struct keyboard_event *event) {
    (void)event;
    return false;
}

/* ITEM-448: the pointer's production bindings, stubbed for the same reason. */
bool xhci_mouse_pop_event(struct usb_hid_mouse_event *event) {
    (void)event;
    return false;
}

bool xhci_mouse_attached(void) {
    return false;
}

/* A fake queue pair the proof can fill and drain. */
#define FAKE_QUEUE 64U

struct fake_queue {
    struct keyboard_event events[FAKE_QUEUE];
    size_t head;
    size_t tail;
    unsigned int pops;
};

static struct fake_queue fake_ps2;
static struct fake_queue fake_usb;
static unsigned int fake_service_calls;

static void fake_push(struct fake_queue *queue,
                      enum keyboard_event_type type,
                      char character) {
    if (queue->head >= FAKE_QUEUE) {
        return;
    }
    queue->events[queue->head].type = type;
    queue->events[queue->head].character = character;
    ++queue->head;
}

static bool fake_pop(struct fake_queue *queue, struct keyboard_event *event) {
    if (queue->tail >= queue->head) {
        return false;
    }
    *event = queue->events[queue->tail++];
    ++queue->pops;
    return true;
}

static void fake_usb_service(void *context) {
    (void)context;
    ++fake_service_calls;
}

static bool fake_ps2_pop(void *context, struct keyboard_event *event) {
    (void)context;
    return fake_pop(&fake_ps2, event);
}

static bool fake_usb_pop(void *context, struct keyboard_event *event) {
    (void)context;
    return fake_pop(&fake_usb, event);
}

/* ITEM-448: a fake pointer queue, filled by the proof and drained through the
   module's own passthrough.  ITEM-480 grows it: the budget proof queues a whole
   movement run plus button edges. */
#define FAKE_MOUSE_QUEUE 32U

static struct usb_hid_mouse_event fake_mouse[FAKE_MOUSE_QUEUE];
static size_t fake_mouse_head;
static size_t fake_mouse_tail;
static bool fake_mouse_ready;

static bool fake_mouse_pop(void *context, struct usb_hid_mouse_event *event) {
    (void)context;
    if (fake_mouse_tail >= fake_mouse_head) {
        return false;
    }
    *event = fake_mouse[fake_mouse_tail++];
    return true;
}

/* ITEM-480: queue one decoded report for the batch proof. */
static void fake_mouse_push(int16_t dx, int16_t dy, uint8_t pressed,
                            uint8_t released) {
    if (fake_mouse_head >= FAKE_MOUSE_QUEUE) {
        return;
    }
    fake_mouse[fake_mouse_head].dx = dx;
    fake_mouse[fake_mouse_head].dy = dy;
    fake_mouse[fake_mouse_head].pressed = pressed;
    fake_mouse[fake_mouse_head].released = released;
    ++fake_mouse_head;
}

static bool fake_mouse_ready_hook(void *context) {
    (void)context;
    return fake_mouse_ready;
}

static const struct local_input_ops fake_ops = {
    NULL,
    fake_usb_service,
    fake_ps2_pop,
    fake_usb_pop,
    fake_mouse_pop,
    fake_mouse_ready_hook
};

static void fake_reset(void) {
    memset(&fake_ps2, 0, sizeof(fake_ps2));
    memset(&fake_usb, 0, sizeof(fake_usb));
    memset(fake_mouse, 0, sizeof(fake_mouse));
    fake_mouse_head = 0U;
    fake_mouse_tail = 0U;
    fake_mouse_ready = false;
    fake_service_calls = 0U;
    local_input_configure(&fake_ops);
}

static struct local_input_report base_report(void) {
    struct local_input_report report;

    memset(&report, 0, sizeof(report));
    report.ps2_ready = false;
    report.ps2_reason = "CONTROLLER_ABSENT";
    report.usb_ready = false;
    report.usb_reason = "NO_DEVICE";
    report.usb_detail = "NO_CONNECTED_DEVICE";
    report.usb_speed = "UNKNOWN";
    return report;
}

static void test_source_policy(void) {
    struct local_input_report report = base_report();

    report.ps2_ready = true;
    report.usb_ready = false;
    local_input_publish(&report);
    CHECK(local_input_source() == LOCAL_INPUT_SOURCE_PS2,
          "a ready PS/2 controller is the source");
    CHECK(local_input_ready(), "PS/2 alone makes local input available");
    CHECK(strcmp(local_input_reason(), "OK") == 0,
          "PS/2 readiness publishes reason OK");
    CHECK(strcmp(local_input_source_string(local_input_source()), "PS2") == 0,
          "the PS/2 source renders as PS2");

    report.ps2_ready = false;
    report.usb_ready = true;
    report.usb_reason = "OK";
    report.usb_detail = "OK";
    report.usb_speed = "HIGH";
    local_input_publish(&report);
    CHECK(local_input_source() == LOCAL_INPUT_SOURCE_USB_HID,
          "a USB HID keyboard is the source when PS/2 is not");
    CHECK(local_input_ready(), "the USB path alone makes local input available");
    CHECK(strcmp(local_input_reason(), "USB_HID") == 0,
          "the USB source publishes reason USB_HID");
    CHECK(strcmp(local_input_source_string(local_input_source()),
                 "USB_HID") == 0,
          "the USB source renders as USB_HID");

    report.usb_ready = false;
    local_input_publish(&report);
    CHECK(local_input_source() == LOCAL_INPUT_SOURCE_NONE,
          "neither path publishes source NONE");
    CHECK(!local_input_ready(), "no path means local input is unavailable");
    CHECK(strcmp(local_input_reason(), "CONTROLLER_ABSENT") == 0,
          "the reason is the PS/2 status the ITEM-392 record published");
    CHECK(strcmp(local_input_source_string(local_input_source()), "NONE") == 0,
          "the absent source renders as NONE");

    /* Publishing nothing must not invent readiness. */
    local_input_publish(NULL);
    CHECK(!local_input_ready(),
          "a rejected publish leaves the previous state alone");
}

static void test_ps2_priority_and_deferral(void) {
    struct local_input_report report = base_report();
    struct keyboard_event event;
    size_t ps2 = 0U;
    size_t usb = 0U;

    fake_reset();
    report.ps2_ready = true;
    report.usb_ready = true;
    local_input_publish(&report);
    fake_push(&fake_ps2, KEYBOARD_EVENT_CHARACTER, 'p');
    fake_push(&fake_ps2, KEYBOARD_EVENT_CHARACTER, 's');
    fake_push(&fake_usb, KEYBOARD_EVENT_CHARACTER, 'u');

    local_input_service();
    CHECK(fake_service_calls == 1U, "the pass services the USB path");
    CHECK(fake_usb.pops == 0U,
          "a pass with PS/2 input does not drain the USB queue");
    while (local_input_pop_event(&event)) {
        CHECK(event.type == KEYBOARD_EVENT_CHARACTER,
              "merged events keep their type");
        if (event.character == 'p' || event.character == 's') {
            ++ps2;
        }
        if (event.character == 'u') {
            ++usb;
        }
    }
    CHECK(ps2 == 2U, "both PS/2 events are delivered");
    CHECK(usb == 0U, "the USB event is deferred, not merged into this pass");

    local_input_service();
    CHECK(fake_usb.pops == 1U, "the next pass drains the USB queue");
    usb = 0U;
    while (local_input_pop_event(&event)) {
        if (event.character == 'u') {
            ++usb;
        }
    }
    CHECK(usb == 1U, "the deferred USB event is delivered, not dropped");
}

static void test_usb_only_machine(void) {
    struct local_input_report report = base_report();
    struct keyboard_event event;
    size_t usb = 0U;

    fake_reset();
    report.usb_ready = true;
    local_input_publish(&report);
    fake_push(&fake_usb, KEYBOARD_EVENT_ENTER, '\0');
    fake_push(&fake_usb, KEYBOARD_EVENT_BACKSPACE, '\0');

    local_input_service();
    CHECK(fake_usb.pops == 2U,
          "a machine without PS/2 delivers USB events in the same pass");
    while (local_input_pop_event(&event)) {
        ++usb;
    }
    CHECK(usb == 2U, "both USB events are merged for the console");
}

static void test_queue_is_bounded(void) {
    struct local_input_report report = base_report();
    struct keyboard_event event;
    size_t delivered = 0U;

    fake_reset();
    report.ps2_ready = true;
    local_input_publish(&report);
    for (size_t index = 0U; index < FAKE_QUEUE; ++index) {
        fake_push(&fake_ps2, KEYBOARD_EVENT_CHARACTER, 'a');
    }

    local_input_service();
    while (local_input_pop_event(&event)) {
        ++delivered;
    }
    CHECK(delivered < FAKE_QUEUE,
          "the merged queue drops the overflow instead of growing");
    CHECK(delivered != 0U, "the merged queue still delivers what fits");
    /* The module is reusable after the overflow: the next pass drains what the
       bound allowed. */
    local_input_service();
    while (local_input_pop_event(&event)) {
        ++delivered;
    }
    CHECK(delivered != 0U, "later passes keep delivering events");
}

static void test_render(void) {
    struct local_input_report report = base_report();
    char buffer[LOCAL_INPUT_RENDER_MAX];
    size_t length;

    fake_reset();
    report.ps2_ready = false;
    report.usb_ready = true;
    report.usb_reason = "OK";
    report.usb_detail = "OK";
    report.usb_speed = "HIGH";
    report.usb_vendor_id = 0x0627U;
    report.usb_product_id = 0x0001U;
    local_input_publish(&report);

    length = local_input_render(buffer, sizeof(buffer));
    CHECK(length == strlen(buffer),
          "the render reports the length it wrote");
    CHECK(strstr(buffer, "SOURCE=USB_HID") != NULL,
          "the render names the live source");
    CHECK(strstr(buffer, "SPEED=HIGH") != NULL, "the render names the speed");
    CHECK(strstr(buffer, "VID=0x627") != NULL, "the render names the vendor");
    CHECK(strstr(buffer, "PID=0x1") != NULL, "the render names the product");
    CHECK(strstr(buffer, "PS2=CONTROLLER_ABSENT") != NULL,
          "the render keeps the other path's verdict");

    report.usb_ready = false;
    report.usb_reason = "NO_DEVICE";
    report.usb_detail = "NO_CONNECTED_DEVICE";
    local_input_publish(&report);
    length = local_input_render(buffer, sizeof(buffer));
    CHECK(strstr(buffer, "SOURCE=NONE") != NULL,
          "an unavailable machine renders SOURCE=NONE");
    CHECK(strstr(buffer, "USB=NO_DEVICE") != NULL,
          "the render names why the USB path is unavailable");
    CHECK(strstr(buffer, "USB_DETAIL=NO_CONNECTED_DEVICE") != NULL,
          "the render carries the USB detail");

    /* A short buffer truncates and says so. */
    {
        char small[16];
        const size_t full = local_input_render(buffer, sizeof(buffer));

        memset(small, 0x7F, sizeof(small));
        length = local_input_render(small, sizeof(small));
        CHECK(length == full,
              "a short buffer still reports the untruncated length");
        CHECK(small[sizeof(small) - 1U] == '\0',
              "a short buffer is NUL terminated");
        CHECK(strncmp(small, buffer, sizeof(small) - 1U) == 0,
              "a short buffer holds the start of the record");
    }

    length = local_input_render(NULL, 0U);
    CHECK(length != 0U, "the render can measure without a buffer");
    CHECK(local_input_render(NULL, sizeof(buffer)) == length,
          "the measured length does not depend on the buffer");
}

/*
 * ITEM-448: the pointer's queue is not merged with the keyboard's - there is
 * no PS/2 mouse to arbitrate against - so it is popped straight through, a
 * machine without a pointer reports not-ready, and an empty queue is false
 * rather than a fabricated event.
 */
static void test_mouse_passthrough(void) {
    struct usb_hid_mouse_event event;

    fake_reset();
    CHECK(!local_input_mouse_ready(),
          "a machine with no mouse reports no pointer");
    CHECK(!local_input_pop_mouse_event(&event),
          "an empty mouse queue pops nothing");
    CHECK(!local_input_pop_mouse_event(NULL),
          "a null mouse destination pops nothing");

    fake_mouse_ready = true;
    CHECK(local_input_mouse_ready(),
          "a bound mouse reports a pointer");
    fake_mouse_head = 2U;
    fake_mouse[0].dx = 5;
    fake_mouse[0].dy = -4;
    fake_mouse[0].buttons = 0U;
    fake_mouse[1].dx = 0;
    fake_mouse[1].dy = 0;
    fake_mouse[1].pressed = USB_HID_MOUSE_BUTTON_LEFT;
    CHECK(local_input_pop_mouse_event(&event) && event.dx == 5 &&
              event.dy == -4,
          "a movement is popped as the driver decoded it");
    CHECK(local_input_pop_mouse_event(&event) &&
              event.pressed == USB_HID_MOUSE_BUTTON_LEFT,
          "a button edge is popped as the driver decoded it");
    CHECK(!local_input_pop_mouse_event(&event),
          "the mouse queue empties like any other");

    /* A machine with a mouse but no bound keyboard still has a pointer. */
    fake_reset();
    fake_mouse_ready = true;
    CHECK(!local_input_ready() && local_input_mouse_ready(),
          "a pointer without a keyboard is still a pointer");
}

/*
 * ITEM-480 (round 18): the pointer's turn budget and movement coalescing.
 *
 * A run of consecutive movements is summed into one draw-worthy batch; a button
 * edge ends the run and is carried by a batch of its own, in order; the budget
 * serves only a bounded number of batches per call, and the unserved remainder
 * waits in the queue and is served by the next call - nothing is lost.
 */
static void test_mouse_turn_budget_and_coalescing(void) {
    struct usb_hid_mouse_batch batches[LOCAL_INPUT_MOUSE_DRAW_BUDGET];
    uint32_t served;

    fake_reset();
    fake_mouse_ready = true;
    /* move move move -> one summed batch (dx = 6, three reports). */
    fake_mouse_push(1, 0, 0U, 0U);
    fake_mouse_push(2, 0, 0U, 0U);
    fake_mouse_push(3, 0, 0U, 0U);
    /* LEFT_PRESS breaks the run. */
    fake_mouse_push(0, 0, USB_HID_MOUSE_BUTTON_LEFT, 0U);
    /* move -> its own batch behind the press. */
    fake_mouse_push(4, -2, 0U, 0U);
    /* LEFT_RELEASE breaks that run. */
    fake_mouse_push(0, 0, 0U, USB_HID_MOUSE_BUTTON_LEFT);
    /* move move move move -> one summed batch (dx = 4, dy = 4). */
    fake_mouse_push(1, 1, 0U, 0U);
    fake_mouse_push(1, 1, 0U, 0U);
    fake_mouse_push(1, 1, 0U, 0U);
    fake_mouse_push(1, 1, 0U, 0U);

    served = local_input_pop_mouse_batches(
        batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET);
    CHECK(served == LOCAL_INPUT_MOUSE_DRAW_BUDGET,
          "the budget bounds how many batches one turn may draw");
    CHECK(batches[0].dx == 6 && batches[0].dy == 0 &&
              batches[0].events == 3U && batches[0].pressed == 0U &&
              batches[0].released == 0U,
          "a run of movements is summed into one batch");
    CHECK(batches[1].pressed == USB_HID_MOUSE_BUTTON_LEFT &&
              batches[1].events == 1U,
          "a press breaks the run and is its own batch");
    CHECK(batches[2].dx == 4 && batches[2].dy == -2 &&
              batches[2].events == 1U,
          "the movement after the press is a fresh run");

    /* The remainder is still queued: the release and the four-move run. */
    served = local_input_pop_mouse_batches(
        batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET);
    CHECK(served == 2U, "the unserved remainder is served by the next call");
    CHECK(batches[0].released == USB_HID_MOUSE_BUTTON_LEFT &&
              batches[0].events == 1U,
          "no release is lost to the budget");
    CHECK(batches[1].dx == 4 && batches[1].dy == 4 &&
              batches[1].events == 4U,
          "the rest of the movement run is not dropped");
    CHECK(local_input_pop_mouse_batches(batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET) ==
              0U,
          "the queue empties after the remainder is served");
    CHECK(fake_mouse_tail == fake_mouse_head,
          "every queued report was served exactly once");
}

/* A report with no movement and no button edge is not a movement: it is
   consumed without spending the budget or producing a batch. */
static void test_mouse_zero_move_is_not_a_movement(void) {
    struct usb_hid_mouse_batch batches[LOCAL_INPUT_MOUSE_DRAW_BUDGET];
    uint32_t served;

    fake_reset();
    fake_mouse_ready = true;
    fake_mouse_push(0, 0, 0U, 0U);
    fake_mouse_push(0, 0, 0U, 0U);
    fake_mouse_push(7, 0, 0U, 0U);

    served = local_input_pop_mouse_batches(
        batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET);
    CHECK(served == 1U, "the empty reports do not become batches");
    CHECK(batches[0].dx == 7 && batches[0].events == 1U,
          "the first real movement is the batch");
    CHECK(local_input_pop_mouse_batches(batches, LOCAL_INPUT_MOUSE_DRAW_BUDGET) ==
              0U,
          "the empty reports were consumed, not left to fabricate a batch");
}

/* The keyboard is drained whole; the pointer's budget never delays it, and a
   mouse burst is one drawing while the keyboard's events are already out. */
static void test_keyboard_does_not_wait_for_the_mouse(void) {
    struct keyboard_event event;
    struct usb_hid_mouse_batch batches[LOCAL_INPUT_MOUSE_DRAW_BUDGET];
    size_t index;
    size_t delivered = 0U;

    fake_reset();
    fake_mouse_ready = true;
    for (index = 0U; index < 4U; ++index) {
        fake_push(&fake_usb, KEYBOARD_EVENT_CHARACTER, (char)('a' + index));
    }
    for (index = 0U; index < 6U; ++index) {
        fake_mouse_push(1, 0, 0U, 0U);
    }
    local_input_service();
    while (local_input_pop_event(&event)) {
        ++delivered;
    }
    CHECK(delivered == 4U,
          "the keyboard queue is drained whole before the pointer is served");
    CHECK(local_input_pop_mouse_batches(batches, 1U) == 1U,
          "one mouse batch is served after the keyboard's events");
    CHECK(batches[0].events == 6U,
          "the whole mouse burst arrives as one drawing, not six");
}

int main(void) {
    test_source_policy();
    test_ps2_priority_and_deferral();
    test_usb_only_machine();
    test_queue_is_bounded();
    test_render();
    test_mouse_passthrough();
    test_mouse_turn_budget_and_coalescing();
    test_mouse_zero_move_is_not_a_movement();
    test_keyboard_does_not_wait_for_the_mouse();

    local_input_configure(NULL);

    if (failures != 0) {
        printf("\n%d FAILURES\n", failures);
        return 1;
    }
    puts("Local input arbitration host test: ALL OK");
    return 0;
}

#else
#error "tests/local-input-host.c requires -DHFOC_LOCAL_INPUT_HOST_TEST"
#endif
