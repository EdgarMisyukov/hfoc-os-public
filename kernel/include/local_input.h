#ifndef HFOC_LOCAL_INPUT_H
#define HFOC_LOCAL_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "keyboard.h"
#include "usb_hid.h"

/* Capacity of the one-line local-input render, including the terminating NUL:
   EMPTY is not an option, the record always names the source and the other
   path's verdict. */
#define LOCAL_INPUT_RENDER_MAX 160U

/*
 * ITEM-395: one local input path for the console.
 *
 * The appliance can have two keyboards: the PS/2 controller (IRQ driven,
 * kernel/src/keyboard.c) and a USB HID boot-protocol keyboard behind an xHCI
 * controller (polled, kernel/src/xhci.c). Both decode into the same
 * struct keyboard_event, and the console consumes them through this module so
 * that the two sources cannot fight.
 *
 * Declared arbitration rule: PS/2 has priority, and a pass never mixes the two
 * devices. In one service pass the PS/2 queue is drained first; the USB queue
 * is drained only when that pass produced no PS/2 event at all. A USB event is
 * therefore deferred by at most one pass - never dropped, never interleaved
 * with a PS/2 event inside one command line - and a machine with both
 * keyboards behaves as if the PS/2 keyboard were the one in use.
 *
 * The decision is published rather than assumed: the source, the reason and
 * the readiness below are what the boot record and the console banner print,
 * and what appliance readiness reports as local_input.
 */

enum local_input_source {
    LOCAL_INPUT_SOURCE_NONE = 0,
    LOCAL_INPUT_SOURCE_PS2,
    LOCAL_INPUT_SOURCE_USB_HID
};

/* What boot observed about both paths. Every field is evidence, not a guess:
   the boot path fills it from the two drivers' own status. */
struct local_input_report {
    bool ps2_ready;
    const char *ps2_reason;
    bool usb_ready;
    const char *usb_reason;
    const char *usb_detail;
    const char *usb_speed;
    uint16_t usb_vendor_id;
    uint16_t usb_product_id;
};

/*
 * The two queues this module merges, plus the hook that services the polled
 * USB path. The production bindings are built in; the host proof replaces
 * them with fakes to exercise the priority rule without a controller.
 */
struct local_input_ops {
    void *context;
    void (*usb_service)(void *context);
    bool (*ps2_pop)(void *context, struct keyboard_event *event);
    bool (*usb_pop)(void *context, struct keyboard_event *event);
    /*
     * ITEM-448: the pointer's own queue.  A mouse event is not merged with the
     * keyboard's - it has no PS/2 counterpart to be arbitrated against - so it
     * is popped straight through, and a machine with no mouse simply never
     * produces one.  `mouse_ready` lets a caller ask before popping.
     */
    bool (*mouse_pop)(void *context, struct usb_hid_mouse_event *event);
    bool (*mouse_ready)(void *context);
};

/* Publish the boot observation. Called once, before the console paints. */
void local_input_publish(const struct local_input_report *report);

/* Replace the source bindings. NULL restores the production bindings. */
void local_input_configure(const struct local_input_ops *ops);

/* Service the USB path and merge both queues under the declared rule. Called
   from the ordinary-boot event loop. */
void local_input_service(void);

/* Pop one merged console event, false when none is pending. */
bool local_input_pop_event(struct keyboard_event *event);

/*
 * ITEM-448: pop one decoded mouse event, false when none is pending, and
 * whether a pointer is bound at all.  Both go through the same production
 * bindings as the keyboard path, so the host proof can drive them with fakes.
 */
bool local_input_pop_mouse_event(struct usb_hid_mouse_event *event);
bool local_input_mouse_ready(void);

/*
 * ITEM-480 (round 18): the pointer's turn budget and movement coalescing.
 *
 * The pointer must never make one main-loop turn unbounded: the keyboard's
 * queue is drained whole, but the mouse is served at most `budget` draw-worthy
 * batches per turn and the rest stays queued for the next turn.  A batch sums
 * consecutive movements, so a burst of reports is one drawing rather than many;
 * a button edge breaks the run and is carried alone, in order.  Reports are
 * never dropped by the budget - an unserved batch is simply not popped yet.
 *
 * `LOCAL_INPUT_MOUSE_DRAW_BUDGET` is the named N the item fixes (2..4).
 * `LOCAL_INPUT_MOUSE_BATCH_EVENTS` bounds how many reports one batch may sum,
 * so even the coalescing run is a bounded loop.
 */
#define LOCAL_INPUT_MOUSE_DRAW_BUDGET 3U
#define LOCAL_INPUT_MOUSE_BATCH_EVENTS 16U

/*
 * ITEM-482 (round 20): the keyboard's turn budget.
 *
 * The owner's run showed the pointer starving under continuous typing: the
 * keyboard was drained whole, so one turn could serve an unbounded number of
 * keystrokes while the mouse's three batches waited.  The named budget is the
 * other half of "no input source makes one turn unbounded": the keyboard serves
 * at most `LOCAL_INPUT_KEYBOARD_BUDGET` decoded events per turn, the rest stays
 * queued and is served next turn in order.  Nothing is dropped - an unserved
 * event is simply not popped yet.
 */
#define LOCAL_INPUT_KEYBOARD_BUDGET 8U

/*
 * Pop at most `budget` batches into `batches`, coalescing as it goes, and
 * return how many were written.  The unserved remainder waits in the queue (or
 * the module's one-event hold) for the next call; nothing is lost.
 */
uint32_t local_input_pop_mouse_batches(struct usb_hid_mouse_batch *batches,
                                       uint32_t budget);

bool local_input_ready(void);
enum local_input_source local_input_source(void);
const char *local_input_source_string(enum local_input_source source);

/*
 * Why local input is in the state it is in: "OK" when PS/2 supplies it,
 * "USB_HID" when the USB keyboard does, and the PS/2 status string when
 * neither path is usable (the value ITEM-392 published). Never NULL.
 */
const char *local_input_reason(void);

/*
 * One bounded line for the console's KBD command, in the style of the other
 * console records. Reports the untruncated length like the PMM renderers, so a
 * caller with a smaller buffer can tell that it truncated.
 */
size_t local_input_render(char *buffer, size_t size);

#endif
