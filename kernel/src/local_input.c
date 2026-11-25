#include "local_input.h"

#include "xhci.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* One pass never queues more than this many events; the console drains the
   queue every loop iteration, and a bounded queue means a stalled console
   cannot grow memory. */
#define LOCAL_INPUT_QUEUE 32U

static struct local_input_report report;
static bool published;
static struct keyboard_event queue[LOCAL_INPUT_QUEUE];
static uint32_t queue_head;
static uint32_t queue_tail;
static enum local_input_source source;
static const char *reason = "UNKNOWN";
/*
 * ITEM-480 (round 18): one decoded mouse report held back from the driver, so
 * a batch can look at the next report without consuming it.  A button edge
 * that ends a movement run is left here for the next batch; the movement already
 * summed is returned now, and the order is unchanged.
 */
static struct usb_hid_mouse_event mouse_hold;
static bool mouse_hold_valid;

static void local_input_production_service(void *context) {
    (void)context;
    xhci_keyboard_service();
}

static bool local_input_production_ps2_pop(
    void *context,
    struct keyboard_event *event) {
    (void)context;
    return keyboard_pop_event(event);
}

static bool local_input_production_usb_pop(
    void *context,
    struct keyboard_event *event) {
    (void)context;
    return xhci_keyboard_pop_event(event);
}

/* ITEM-448: the pointer's queue, straight from the driver. */
static bool local_input_production_mouse_pop(
    void *context,
    struct usb_hid_mouse_event *event) {
    (void)context;
    return xhci_mouse_pop_event(event);
}

static bool local_input_production_mouse_ready(void *context) {
    (void)context;
    return xhci_mouse_attached();
}

static const struct local_input_ops local_input_production_ops = {
    NULL,
    local_input_production_service,
    local_input_production_ps2_pop,
    local_input_production_usb_pop,
    local_input_production_mouse_pop,
    local_input_production_mouse_ready,
};

static const struct local_input_ops *ops = &local_input_production_ops;

static bool local_input_push(const struct keyboard_event *event) {
    const uint32_t next = (queue_head + 1U) % LOCAL_INPUT_QUEUE;

    if (next == queue_tail) {
        return false;
    }
    queue[queue_head] = *event;
    queue_head = next;
    return true;
}

/*
 * The declared rule, in one place: PS/2 first, and the USB queue only when the
 * PS/2 queue produced nothing in this pass. Deferring a USB event by one pass
 * keeps a single keystroke on either device intact without dropping anything.
 */
static void local_input_merge(void) {
    struct keyboard_event event;
    bool ps2_delivered = false;

    if (ops->ps2_pop != NULL) {
        while (ops->ps2_pop(ops->context, &event)) {
            ps2_delivered = true;
            (void)local_input_push(&event);
        }
    }
    if (!ps2_delivered && ops->usb_pop != NULL) {
        while (ops->usb_pop(ops->context, &event)) {
            (void)local_input_push(&event);
        }
    }
}

void local_input_publish(const struct local_input_report *value) {
    if (value == NULL) {
        return;
    }
    report = *value;
    published = true;
    queue_head = 0U;
    queue_tail = 0U;
    mouse_hold_valid = false;

    if (report.ps2_ready) {
        source = LOCAL_INPUT_SOURCE_PS2;
        reason = "OK";
    } else if (report.usb_ready) {
        source = LOCAL_INPUT_SOURCE_USB_HID;
        reason = "USB_HID";
    } else {
        source = LOCAL_INPUT_SOURCE_NONE;
        reason = report.ps2_reason != NULL ? report.ps2_reason : "UNKNOWN";
    }
}

void local_input_configure(const struct local_input_ops *value) {
    ops = value != NULL ? value : &local_input_production_ops;
    /* ITEM-480: a new binding must not inherit a held report from the old one. */
    mouse_hold_valid = false;
}

void local_input_service(void) {
    if (ops->usb_service != NULL) {
        ops->usb_service(ops->context);
    }
    local_input_merge();
}

bool local_input_pop_event(struct keyboard_event *event) {
    if (event == NULL || queue_tail == queue_head) {
        return false;
    }
    *event = queue[queue_tail];
    queue_tail = (queue_tail + 1U) % LOCAL_INPUT_QUEUE;
    return true;
}

bool local_input_pop_mouse_event(struct usb_hid_mouse_event *event) {
    if (event == NULL || ops->mouse_pop == NULL) {
        return false;
    }
    return ops->mouse_pop(ops->context, event);
}

/*
 * ITEM-480 (round 18): the pointer's batch path.
 *
 * `local_input_mouse_peek()` reads the next report - from the one-event hold,
 * or from the driver when the hold is empty - without consuming it;
 * `local_input_mouse_drop()` consumes what peek handed back.  That is what lets
 * a movement run stop in front of a button edge and leave the button queued.
 */
static bool local_input_mouse_peek(struct usb_hid_mouse_event *event) {
    if (!mouse_hold_valid) {
        if (ops->mouse_pop == NULL ||
            !ops->mouse_pop(ops->context, &mouse_hold)) {
            return false;
        }
        mouse_hold_valid = true;
    }
    *event = mouse_hold;
    return true;
}

static void local_input_mouse_drop(void) {
    mouse_hold_valid = false;
}

static bool local_input_mouse_has_button(
    const struct usb_hid_mouse_event *event) {
    return event->pressed != 0U || event->released != 0U;
}

bool local_input_pop_mouse_batch(struct usb_hid_mouse_batch *batch) {
    struct usb_hid_mouse_event event;
    uint32_t spent = 0U;
    int32_t dx = 0;
    int32_t dy = 0;
    int32_t wheel = 0;

    if (batch == NULL) {
        return false;
    }
    /*
     * A report that carries neither movement nor a button edge is not a
     * movement and not an edge: consume it without spending the budget or
     * forcing a drawing, then look at what follows.
     */
    for (spent = 0U; spent < LOCAL_INPUT_MOUSE_BATCH_EVENTS; ++spent) {
        if (!local_input_mouse_peek(&event)) {
            break;
        }
        if (local_input_mouse_has_button(&event) || event.dx != 0 ||
            event.dy != 0 || event.wheel != 0) {
            break;
        }
        local_input_mouse_drop();
    }
    if (!local_input_mouse_peek(&event)) {
        return false;
    }
    /*
     * A button edge is its own batch (it may still carry the movement decoded
     * in the same report) and ends the run, so the next batch starts a fresh
     * movement run behind it.
     */
    /*
     * ITEM-579: a button edge and a wheel movement are each their own batch -
     * neither is a movement run - and both carry whatever movement the same
     * report decoded.
     */
    if (local_input_mouse_has_button(&event) || event.wheel != 0) {
        local_input_mouse_drop();
        batch->dx = event.dx;
        batch->dy = event.dy;
        batch->wheel = event.wheel;
        batch->pressed = event.pressed;
        batch->released = event.released;
        batch->events = 1U;
        return true;
    }
    /*
     * A movement run: sum consecutive movements, up to the batch bound.  The
     * loop ends in front of a button edge or the queue's end, and what ended it
     * is left for the next batch.
     */
    for (spent = 0U; spent < LOCAL_INPUT_MOUSE_BATCH_EVENTS; ++spent) {
        if (!local_input_mouse_peek(&event) ||
            local_input_mouse_has_button(&event) || event.wheel != 0 ||
            (event.dx == 0 && event.dy == 0)) {
            break;
        }
        dx += event.dx;
        dy += event.dy;
        local_input_mouse_drop();
    }
    if (spent == 0U) {
        return false;
    }
    batch->dx = dx;
    batch->dy = dy;
    batch->wheel = wheel;
    batch->pressed = 0U;
    batch->released = 0U;
    batch->events = (uint16_t)spent;
    return true;
}

uint32_t local_input_pop_mouse_batches(struct usb_hid_mouse_batch *batches,
                                       uint32_t budget) {
    uint32_t count = 0U;

    if (batches == NULL) {
        return 0U;
    }
    /* The bound is the caller's named budget: the loop cannot run away. */
    while (count < budget && local_input_pop_mouse_batch(&batches[count])) {
        ++count;
    }
    return count;
}

bool local_input_mouse_ready(void) {
    return ops->mouse_ready != NULL && ops->mouse_ready(ops->context);
}

bool local_input_ready(void) {
    return published && source != LOCAL_INPUT_SOURCE_NONE;
}

enum local_input_source local_input_source(void) {
    return published ? source : LOCAL_INPUT_SOURCE_NONE;
}

const char *local_input_source_string(enum local_input_source value) {
    switch (value) {
    case LOCAL_INPUT_SOURCE_PS2:
        return "PS2";
    case LOCAL_INPUT_SOURCE_USB_HID:
        return "USB_HID";
    case LOCAL_INPUT_SOURCE_NONE:
    default:
        return "NONE";
    }
}

const char *local_input_reason(void) {
    return reason;
}

struct local_input_text {
    char *buffer;
    size_t size;
    size_t length;
};

static void local_input_append(struct local_input_text *text,
                               const char *value) {
    if (value == NULL) {
        return;
    }
    while (*value != '\0') {
        if (text->buffer != NULL && text->length + 1U < text->size) {
            text->buffer[text->length] = *value;
        }
        ++text->length;
        ++value;
    }
    if (text->buffer != NULL && text->size != 0U) {
        const size_t end = text->length < text->size ? text->length
                                                     : text->size - 1U;
        text->buffer[end] = '\0';
    }
}

static void local_input_append_hex(struct local_input_text *text,
                                   uint64_t value) {
    static const char digits[] = "0123456789ABCDEF";
    char buffer[17];
    size_t length = 0U;

    local_input_append(text, "0x");
    if (value == 0U) {
        local_input_append(text, "0");
        return;
    }
    while (value != 0U && length < sizeof(buffer)) {
        buffer[length++] = digits[value & 0xFU];
        value >>= 4U;
    }
    while (length != 0U) {
        const char single[2] = { buffer[--length], '\0' };

        local_input_append(text, single);
    }
}

size_t local_input_render(char *buffer, size_t size) {
    struct local_input_text text;

    text.buffer = buffer;
    text.size = size;
    text.length = 0U;
    if (buffer != NULL && size != 0U) {
        buffer[0] = '\0';
    }

    local_input_append(&text, "LOCAL INPUT: SOURCE=");
    local_input_append(
        &text, local_input_source_string(local_input_source()));
    if (local_input_source() == LOCAL_INPUT_SOURCE_PS2) {
        local_input_append(&text, " PS2=READY");
    } else if (local_input_source() == LOCAL_INPUT_SOURCE_USB_HID) {
        local_input_append(&text, " USB=READY SPEED=");
        local_input_append(&text, report.usb_speed != NULL
                                      ? report.usb_speed
                                      : "UNKNOWN");
        local_input_append(&text, " VID=");
        local_input_append_hex(&text, report.usb_vendor_id);
        local_input_append(&text, " PID=");
        local_input_append_hex(&text, report.usb_product_id);
        local_input_append(&text, " PS2=");
        local_input_append(&text, report.ps2_reason != NULL
                                      ? report.ps2_reason
                                      : "UNKNOWN");
    } else {
        local_input_append(&text, " PS2=");
        local_input_append(&text, report.ps2_reason != NULL
                                      ? report.ps2_reason
                                      : "UNKNOWN");
        local_input_append(&text, " USB=");
        local_input_append(&text, report.usb_reason != NULL
                                      ? report.usb_reason
                                      : "UNKNOWN");
        local_input_append(&text, " USB_DETAIL=");
        local_input_append(&text, report.usb_detail != NULL
                                      ? report.usb_detail
                                      : "UNKNOWN");
    }
    local_input_append(&text, "\n");
    return text.length;
}
