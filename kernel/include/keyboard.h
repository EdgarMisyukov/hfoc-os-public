#ifndef HFOC_KEYBOARD_H
#define HFOC_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

enum keyboard_event_type {
    KEYBOARD_EVENT_CHARACTER,
    KEYBOARD_EVENT_ENTER,
    KEYBOARD_EVENT_BACKSPACE,
    KEYBOARD_EVENT_DELETE,
    KEYBOARD_EVENT_LEFT,
    KEYBOARD_EVENT_RIGHT,
    KEYBOARD_EVENT_HOME,
    KEYBOARD_EVENT_END,
    KEYBOARD_EVENT_UP,
    KEYBOARD_EVENT_DOWN
};

struct keyboard_event {
    enum keyboard_event_type type;
    char character;
    /*
     * ITEM-482 (round 20): the TSC reading the USB HID completion that produced
     * this event decoded at, so the console can publish how long the operator
     * waited between the keyboard answering and the character reaching the
     * screen (`kbd_echo_us_max`).  Zero means "not measured" - the PS/2 path
     * leaves it zero because its IRQ is already the report.
     */
    uint64_t reported_cycles;
};

/*
 * ITEM-392: the PS/2 controller is one input path among several and is never a
 * precondition for the appliance, so keyboard_init() reports a classified
 * status instead of a bare success flag. A machine with no i8042 boots, the
 * boot publishes why local keyboard input is unavailable, and the framebuffer
 * console, the serial console and the network management plane stay
 * available.
 */
enum keyboard_init_status {
    KEYBOARD_INIT_OK = 0,
    /* Ports 0x60/0x64 answer all ones: no i8042 in this machine. */
    KEYBOARD_INIT_CONTROLLER_ABSENT,
    /* The controller never accepted a command, or never returned a byte it
       was asked for. */
    KEYBOARD_INIT_CONTROLLER_STUCK,
    /* The controller took the configuration write and read back different
       bits, so scan-code translation cannot be relied on. */
    KEYBOARD_INIT_CONFIG_REJECTED,
    /* The controller is usable; the keyboard itself never acknowledged a
       scan-code command (unplugged, dead, or behind a switch). */
    KEYBOARD_INIT_KEYBOARD_ABSENT,
    /* Not a reported failure: a value outside the declared set. It cannot be
       described to an operator as a reason, so it is the one keyboard status
       that stops the boot. It exists so the boot policy can be exhaustive. */
    KEYBOARD_INIT_STATUS_UNKNOWN
};

/*
 * Last observation of the initialisation path; valid after success and after
 * every failure. status_port is the last byte the kernel read from the
 * controller status port and controller_config the last configuration byte it
 * saw, so a machine with no controller is distinguishable from one whose
 * controller never answered.
 */
struct keyboard_init_diagnostic {
    enum keyboard_init_status status;
    uint8_t status_port;
    uint8_t controller_config;
};

enum keyboard_init_status keyboard_init(void);
const struct keyboard_init_diagnostic *keyboard_init_diagnostic(void);
const char *keyboard_init_status_string(enum keyboard_init_status status);
/*
 * True for every status the driver can report, including each declared
 * failure: the appliance boots without local keyboard input in all of them.
 * False only for an unclassified value, which no operator can be told a
 * reason for.
 */
bool keyboard_init_status_boots(enum keyboard_init_status status);
/*
 * ITEM-411: whether the i8042 controller itself answered at ports 0x60/0x64.
 *
 * The platform reset module needs this before it writes the controller reset
 * command 0xFE to port 0x64: on a machine with no controller (the acceptance
 * host publishes `PS2: UNAVAILABLE`, `KEYBOARD_INIT_CONTROLLER_ABSENT`) that
 * write goes nowhere and a fallback mechanism has to be used instead.  A
 * controller that answered commands qualifies even when no keyboard is plugged
 * into it (`KEYBOARD_INIT_KEYBOARD_ABSENT`) or when its configuration write did
 * not stick (`KEYBOARD_INIT_CONFIG_REJECTED`); a controller that never accepted
 * a command, never answered, or was never probed at all does not.
 */
bool keyboard_controller_present(void);
void keyboard_handle_irq(void);
bool keyboard_pop_event(struct keyboard_event *event);
uint64_t keyboard_irq_count(void);
uint8_t keyboard_last_scancode(void);
uint8_t keyboard_controller_config(void);

#endif
