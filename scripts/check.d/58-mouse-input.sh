#!/usr/bin/env bash
# ITEM-448: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The mouse types next to the keyboard.
#
# Until this item the USB path bound exactly one HID device per controller and
# skipped everything that was not a keyboard, so a mouse - or the keyboard half
# of a wireless receiver - was either ignored or mistaken for the keyboard.  The
# owner's contract is that both work at the same time: the second device does
# not displace the first, both are published as devices of their own, the mouse
# draws a pointer and a click runs the menu entry under it, and the keyboard
# keeps typing.
#
# This slice starts with the half that can be proven without a controller: the
# boot mouse report decoder and its counters.  The driver half (two HID device
# slots on one controller), the pointer and the hit test are added to this slice
# as they land, so the contract grows with the item instead of being asserted
# before it exists.
for mouse_input_file in \
    kernel/include/usb_hid.h \
    kernel/src/usb_hid.c \
    tests/usb-hid-host.c; do
    [[ -f $mouse_input_file ]] || {
        echo "ITEM-448: отсутствует $mouse_input_file" >&2
        exit 1
    }
done

# The boot mouse report and its counters exist, and the byte count is exact.
for mouse_input_symbol in \
    USB_HID_BOOT_MOUSE_REPORT_LENGTH \
    USB_HID_BOOT_MOUSE_TRANSFER_LENGTH \
    USB_HID_MOUSE_BUTTON_LEFT \
    USB_HID_MOUSE_BUTTON_RIGHT \
    USB_HID_MOUSE_BUTTON_MIDDLE \
    usb_hid_mouse_state_reset \
    usb_hid_boot_mouse_report \
    usb_hid_mouse_state_reports \
    usb_hid_mouse_state_moves \
    usb_hid_mouse_state_presses \
    usb_hid_mouse_state_releases \
    usb_hid_mouse_state_refused; do
    grep -Fq "$mouse_input_symbol" \
        kernel/include/usb_hid.h \
        kernel/src/usb_hid.c || {
        echo "ITEM-448: HID-модуль не публикует $mouse_input_symbol" >&2
        exit 1
    }
done
grep -Fq '#define USB_HID_BOOT_MOUSE_REPORT_LENGTH 3U' \
    kernel/include/usb_hid.h || {
    echo "ITEM-448: boot-отчёт мыши не объявлен трёхбайтовым" >&2
    exit 1
}
# A report shorter than the three boot fields is refused, never padded: the one
# thing a pointer must not do is move somewhere the operator did not.
grep -Fq 'received_bytes < USB_HID_BOOT_MOUSE_REPORT_LENGTH' \
    kernel/src/usb_hid.c || {
    echo "ITEM-448: короткий отчёт мыши не отвергается" >&2
    exit 1
}
# A mouse that declares no boot protocol is read through its own report
# descriptor, and the configuration walk is asked for the mouse role separately
# from the keyboard role - one device, two interfaces.
for mouse_input_symbol in \
    USB_HID_PROTOCOL_MOUSE \
    USB_HID_STATUS_NO_HID_MOUSE \
    USB_HID_ROLE_MOUSE \
    usb_hid_parse_config_role \
    usb_hid_parse_mouse_report_descriptor \
    usb_hid_mouse_layout_decode; do
    grep -Fq "$mouse_input_symbol" \
        kernel/include/usb_hid.h \
        kernel/src/usb_hid.c || {
        echo "ITEM-448: HID-модуль не публикует $mouse_input_symbol" >&2
        exit 1
    }
done
# The decoder holds no console, no framebuffer and no controller: where the
# pointer is stays with the caller.
if grep -nE '\b(console_|framebuffer_|xhci_|serial_)\w*' \
        kernel/src/usb_hid.c; then
    echo "ITEM-448: декодер мыши трогает консоль, экран или контроллер" >&2
    exit 1
fi

# The host proof drives the real decoder and states the edges and refusals.
for mouse_input_claim in \
    'a boot mouse report decodes its signed deltas' \
    'a movement is counted as one report and one move' \
    'a left button going down is a press edge' \
    'a held button is not a second press' \
    'a button coming up is a release edge' \
    'the middle and right buttons are edges of their own' \
    'a boot mouse report with a wheel byte decodes the wheel' \
    'a short mouse report is refused, never padded' \
    'a null mouse report is refused' \
    'a null event destination is refused' \
    'the mouse counters are published individually' \
    'a reset forgets the old edges and keeps the state usable' \
    'the boot mouse is selected, and only, for the mouse role' \
    'the same boot mouse is still no keyboard candidate' \
    'the mouse role selects the boot mouse interface of a combined device' \
    'the keyboard role selects the keyboard interface of the same device' \
    'a configuration with no mouse names the mouse verdict' \
    'a mouse report descriptor names its pointer layout' \
    'a report-protocol mouse report decodes through its layout' \
    'the wheel byte after the axes is the wheel, not a delta' \
    'a report shorter than the layout is refused, not padded' \
    "a combined receiver's mouse collection is found under its report ID" \
    'the wheel byte after the axes is the wheel, not a delta' \
    'a report of another report ID is refused' \
    'a keyboard report descriptor is not a pointer' \
    'a consumer-control descriptor is not a pointer' \
    'a pointer with 12-bit axes is refused, not read as bytes' \
    'a null pointer descriptor or destination is refused'; do
    grep -Fq "$mouse_input_claim" tests/usb-hid-host.c || {
        echo "ITEM-448: проба не утверждает: $mouse_input_claim" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------------
# The driver half: two HID devices on one controller.
#
# Until this item the controller state *was* the keyboard state - one slot, one
# interrupt ring, one report buffer, one queue - so the second device either
# displaced the first or was skipped.  The contract is that both are bound at
# once, each with its own binding, rings, report buffer, counters and decoded
# queue, and that the walk offers each port to the role that is still missing.
for mouse_driver_file in \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/include/pointer.h \
    kernel/src/pointer.c \
    kernel/src/console.c \
    kernel/src/framebuffer.c \
    kernel/src/kernel.c \
    kernel/src/local_input.c \
    tests/pointer-host.c; do
    [[ -f $mouse_driver_file ]] || {
        echo "ITEM-448: отсутствует $mouse_driver_file" >&2
        exit 1
    }
done

for mouse_driver_symbol in \
    '#define XHCI_HID_ROLE_KEYBOARD 0U' \
    '#define XHCI_HID_ROLE_MOUSE 1U' \
    '#define XHCI_HID_DEVICES 2U' \
    'struct xhci_hid_device {' \
    'struct xhci_ring ep0_rings[XHCI_HID_DEVICES];' \
    'struct xhci_ring interrupt_rings[XHCI_HID_DEVICES];' \
    'struct xhci_hid_device devices[XHCI_HID_DEVICES];' \
    'static struct xhci_hid_device *xhci_device(uint8_t role) {' \
    'static bool xhci_role_ready(uint8_t role) {' \
    'for (uint8_t role = 0U; role < XHCI_HID_DEVICES; ++role) {' \
    'if (role == XHCI_HID_ROLE_KEYBOARD && keyboard_proven) {' \
    'if (!keyboard && xhci_machine.mouse_session != NULL) {' \
    'xhci_machine.mouse_session = xhci;' \
    'static bool xhci_same_controller(' \
    'static bool xhci_rebind_device(struct xhci_hid_device *device,' \
    'xhci_service_session();' \
    'static void xhci_decode_mouse_report(struct xhci_hid_device *device,' \
    'usb_hid_mouse_layout_decode(' \
    'usb_hid_boot_mouse_report(&device->mouse, report,' \
    '"[MOUSE] state=UNAVAILABLE reason=NO_REPORT detail=P"' \
    '" hid=BOOT_MOUSE"' \
    '" hid=REPORT_MOUSE"'; do
    grep -Fq -- "$mouse_driver_symbol" \
        kernel/src/xhci.c || {
        echo "ITEM-448: драйвер не держит две HID-роли: $mouse_driver_symbol" >&2
        exit 1
    }
done

# The pointer is a published device of its own: the same binding and traffic
# facts the [KBD ] record carries, so "no pointer" is a finding with a reason.
for mouse_driver_accessor in \
    'bool xhci_mouse_pop_event(struct usb_hid_mouse_event *event);' \
    'bool xhci_mouse_attached(void);' \
    'uint8_t xhci_mouse_interface(void);' \
    'uint8_t xhci_mouse_protocol(void);' \
    'uint8_t xhci_mouse_endpoint_interval(void);' \
    'uint16_t xhci_mouse_endpoint_max_packet(void);' \
    'uint32_t xhci_mouse_report_count(void);' \
    'uint32_t xhci_mouse_transfer_count(void);' \
    'uint32_t xhci_mouse_error_count(void);' \
    'uint32_t xhci_mouse_refused_count(void);' \
    'uint32_t xhci_mouse_move_count(void);' \
    'const char *xhci_mouse_status_string(void);' \
    'bool xhci_mouse_seen(void);' \
    'uint8_t xhci_mouse_seen_port(void);'; do
    grep -Fq -- "$mouse_driver_accessor" kernel/include/xhci.h || {
        echo "ITEM-448: доступ к мыши не объявлен: $mouse_driver_accessor" >&2
        exit 1
    }
done

# The record: a bound pointer publishes its binding and its counters, a silent
# one is a NO_REPORT finding rather than a silence, and a refused or released
# device names its vid/pid and its interface list.
for mouse_driver_record in \
    '"[MOUSE] state="' \
    '"[MOUSE] state=READY finding=NO_REPORT reports="' \
    '" mouse_reports="' \
    '" mouse_moves="' \
    '" mouse_presses="' \
    '" mouse_silent="' \
    'xhci_mouse_status_string()' \
    'xhci_mouse_interface_summary()' \
    'xhci_mouse_refused_vendor_id()' \
    'log_production_mouse_silence();' \
    'console_pointer_position(&pointer_x, &pointer_y);'; do
    grep -Fq -- "$mouse_driver_record" kernel/src/kernel.c || {
        echo "ITEM-448: запись [MOUSE] не полна: $mouse_driver_record" >&2
        exit 1
    }
done

# The pointer's queue is not merged with the keyboard's: a mouse has no PS/2
# counterpart to arbitrate against, so it is popped straight through.
for mouse_driver_input in \
    'bool (*mouse_pop)(void *context, struct usb_hid_mouse_event *event);' \
    'bool (*mouse_ready)(void *context);' \
    'bool local_input_pop_mouse_event(struct usb_hid_mouse_event *event);' \
    'bool local_input_mouse_ready(void);' \
    'local_input_production_mouse_pop' \
    'xhci_mouse_pop_event(event)'; do
    grep -Fq -- "$mouse_driver_input" \
        kernel/include/local_input.h kernel/src/local_input.c || {
        echo "ITEM-448: очередь указателя не подключена: $mouse_driver_input" >&2
        exit 1
    }
done

# ---------------------------------------------------------------------------
# The pointer and the hit test.
#
# Where the pointer is and what a click hits is the console's business; the
# arithmetic lives in pointer.c so the host proof can drive every edge without
# a screen, and the console aims at the rectangles its own last paint recorded,
# so a click can only run a line that is on the screen.
for mouse_pointer_symbol in \
    'struct pointer_state {' \
    'struct pointer_target {' \
    'void pointer_state_reset(struct pointer_state *state, int32_t x, int32_t y);' \
    'bool pointer_move(struct pointer_state *state, int32_t dx, int32_t dy,' \
    'bool pointer_button(struct pointer_state *state, bool down);' \
    'int32_t pointer_hit_test(const struct pointer_target *targets, size_t count,'; do
    grep -Fq -- "$mouse_pointer_symbol" kernel/include/pointer.h || {
        echo "ITEM-448: модуль указателя не объявляет: $mouse_pointer_symbol" >&2
        exit 1
    }
done
for mouse_pointer_claim in \
    'a reset puts the pointer at its start and counts nothing' \
    'a movement lands at the previous position plus the report deltas' \
    'the next report moves relative to where the pointer now is' \
    'a report with no movement is not counted as one' \
    'a movement past the left edge stops at zero and is counted' \
    'a movement past the bottom right stops at the last pixel' \
    'a clamped movement is still a movement the pointer made' \
    'a null pointer state is refused' \
    'a zero width screen is refused and counted' \
    'a held button is not a second press' \
    'a button coming up is one release edge' \
    'a null button state is refused' \
    'the top left corner of a rectangle is inside it' \
    'the last pixel of a rectangle is inside it' \
    'one pixel right of a rectangle is outside it' \
    'the second entry is selected by its own rectangle' \
    'a rectangle with no width is never hit' \
    'a click above every entry hits nothing' \
    'a null target list hits nothing' \
    'each menu entry owns its own line and the seam belongs to one'; do
    grep -Fq -- "$mouse_pointer_claim" tests/pointer-host.c || {
        echo "ITEM-448: проба указателя не утверждает: $mouse_pointer_claim" >&2
        exit 1
    }
done
grep -Fq 'test-pointer-host' Makefile || {
    echo "ITEM-448: проба указателя не собрана в Makefile" >&2
    exit 1
}
# shellcheck disable=SC2016  # the Makefile's own $(MAKE) is the pin
grep -Fq '$(MAKE) test-pointer-host' Makefile || {
    echo "ITEM-448: проба указателя не входит в host suite" >&2
    exit 1
}
grep -Fq 'test-pointer-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-448: манифест host suite не содержит test-pointer-host" >&2
    exit 1
}

# The console: the arrow follows the report, a left click runs the entry under
# it through the keyboard's own path, a miss is published and changes nothing,
# and the menu's own order decides what an index means.
for mouse_console_symbol in \
    'void console_process_mouse_event(const struct usb_hid_mouse_event *event)' \
    'bool console_pointer_position(int32_t *x, int32_t *y)' \
    'static void pointer_click(void)' \
    '"[POINTER] click entry="' \
    '"[POINTER] click entry=NONE action=NONE"' \
    '"[POINTER] menu targets="' \
    'pointer_hit_test(pointer_targets, pointer_target_count,' \
    'local_menu_select(&menu_state, (size_t)hit)' \
    'local_menu_run(local_menu_selected(&menu_state));' \
    'console_test_menu_target_count' \
    'console_test_menu_target'; do
    grep -Fq -- "$mouse_console_symbol" kernel/src/console.c \
        kernel/include/console.h || {
        echo "ITEM-448: консоль не ведёт указатель: $mouse_console_symbol" >&2
        exit 1
    }
done
for mouse_console_claim in \
    'the paint recorded one rectangle per menu entry' \
    'the Network entry has a rectangle a click can land in' \
    "a movement report moves the arrow to the entry's own line" \
    'the click names the entry under the pointer and that it ran' \
    "the click ran the entry through the keyboard's own path" \
    'the arrow was painted where the report put it' \
    'a click outside every entry is a published miss' \
    'a miss runs nothing'; do
    grep -Fq -- "$mouse_console_claim" tests/console-host.c || {
        echo "ITEM-448: проба консоли не утверждает: $mouse_console_claim" >&2
        exit 1
    }
done

# The screen keeps the pixels under the arrow, so hiding it puts back exactly
# what it covered, and a scroll forgets a block that no longer describes the
# screen instead of restoring it at the wrong place.
for mouse_cursor_symbol in \
    'bool framebuffer_cursor_show(uint64_t x, uint64_t y);' \
    'void framebuffer_cursor_hide(void);' \
    'bool framebuffer_cursor_visible(void);' \
    '#define FRAMEBUFFER_CURSOR_WIDTH 11U' \
    '#define FRAMEBUFFER_CURSOR_HEIGHT 16U'; do
    grep -Fq -- "$mouse_cursor_symbol" kernel/include/framebuffer.h || {
        echo "ITEM-448: экран не объявляет указатель: $mouse_cursor_symbol" >&2
        exit 1
    }
done
for mouse_cursor_impl in \
    'static uint32_t pixel_read(uint64_t x, uint64_t y) {' \
    'cursor_saved[row * FRAMEBUFFER_CURSOR_WIDTH + column] =' \
    'cursor_visible = false;'; do
    grep -Fq -- "$mouse_cursor_impl" kernel/src/framebuffer.c || {
        echo "ITEM-448: экран не хранит пиксели под указателем: $mouse_cursor_impl" >&2
        exit 1
    }
done

# The QEMU proof: usb-kbd and usb-mouse on one controller, both READY, the
# pointer moved by the monitor, a click that runs the topmost entry, and the
# keyboard still answering afterwards.
[[ -x scripts/test-qemu-usb-mouse-menu.sh ]] || {
    echo "ITEM-448: QEMU-проба мыши отсутствует или не исполняема" >&2
    exit 1
}
for mouse_qemu_marker in \
    "-device 'qemu-xhci,id=xhci'" \
    "-device 'usb-kbd,bus=xhci.0'" \
    "-device 'usb-mouse,bus=xhci.0'" \
    'i8042=off,usb=off' \
    '[MOUSE] state=READY reason=OK' \
    'hid=BOOT_MOUSE' \
    'entry=status action=RUN' \
    '\[POINTER\] click ' \
    '[LOCAL-MENU] run action=status' \
    'LOCAL INPUT: SOURCE=USB_HID USB=READY' \
    'mouse_move' \
    'mouse_button'; do
    grep -Fq -- "$mouse_qemu_marker" scripts/test-qemu-usb-mouse-menu.sh || {
        echo "ITEM-448: QEMU-проба не несёт: $mouse_qemu_marker" >&2
        exit 1
    }
done
grep -Fq 'usb_mouse_menu: the boot panicked' scripts/test-qemu-usb-mouse-menu.sh || {
    echo "ITEM-448: QEMU-проба не отказывает на панике" >&2
    exit 1
}
grep -Fq 'usb_mouse_menu: the console still reports disabled local input' \
    scripts/test-qemu-usb-mouse-menu.sh || {
    echo "ITEM-448: QEMU-проба не отказывает на выключенном вводе" >&2
    exit 1
}
grep -Fq 'test-usb-mouse-menu-qemu' Makefile || {
    echo "ITEM-448: QEMU-проба мыши не собрана в Makefile" >&2
    exit 1
}
awk '/^QEMU_PARALLEL_PHASE_1 :=/,/^$/' Makefile |
    grep -Fq 'test-usb-mouse-menu-qemu' || {
    echo "ITEM-448: QEMU-проба мыши не входит в параллельную фазу" >&2
    exit 1
}

# ---------------------------------------------------------------- ITEM-621 -----
# The decoded wheel reaches the retained history, not only the batch.  The
# production single-report entry must count a wheel-only report, the batch
# consumer must hand the notch to the pager, and the QEMU proof must inject a
# real USB wheel event rather than typing the keyboard `scroll` command.
for wheel_symbol in \
    'event->wheel != 0' \
    'pager_wheel(event->wheel)' \
    'pager_wheel(' \
    'CONSOLE_WHEEL_ROWS' \
    'pager_bottom_top'; do
    grep -Fq -- "$wheel_symbol" kernel/src/console.c || {
        echo "ITEM-621: консоль не несёт wheel-путь:" "$wheel_symbol" >&2
        exit 1
    }
done
grep -Fq 'test_mouse_wheel_scrollback' tests/console-host.c || {
    echo "ITEM-621: host-проба колеса отсутствует" >&2
    exit 1
}
for wheel_qemu_marker in \
    '-device qemu-xhci,id=xhci -device usb-mouse,bus=xhci.0' \
    'input-send-event' \
    '"wheel-up"' \
    '"wheel-down"' \
    'mouse_wheels=' \
    'Wheel up changed the painted page'; do
    grep -Fq -- "$wheel_qemu_marker" scripts/test-qemu-console-history.sh || {
        echo "ITEM-621: QEMU-проба не несёт:" "$wheel_qemu_marker" >&2
        exit 1
    }
done
grep -Fq 'test-qemu-console-history' Makefile || {
    echo "ITEM-621: QEMU-проба колеса не собрана в Makefile" >&2
    exit 1
}
