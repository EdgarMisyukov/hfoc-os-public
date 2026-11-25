#!/usr/bin/env bash
# ITEM-452: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Every controller, every port, every device names itself.
#
# The 2026-09-19 hardware run had three xHCI controllers and published exactly
# one `[XHCI]` record - the first controller in PCI order, the one the K120
# keyboard sits on - because the machine loop left on the first bound device
# (`if (scan.selected) break;`).  The Logitech receiver `046d:c53f` on a USB3
# port of a *different* controller was never enumerated and `[MOUSE] seen=0`
# was the whole story: plugged in, and invisible.  This slice pins the driver
# that replaces that silence:
#
#   * the per-controller state is a session - its own registers, rings, event
#     ring, DMA pool and two role devices - and the machine keeps one live
#     session per bound role plus one scratch session for the walk, so the
#     keyboard's controller and the pointer's controller stay up together;
#   * the walk visits every controller the PCI scan found and every port of
#     each, and nothing it finds ends it; a role the machine already has is not
#     offered a later device, and that device is still published;
#   * the service pass is round-robin over the live sessions, each with its own
#     event ring and counters, so neither role starves the other;
#   * the controller record names the controller (`pci=bb:dd.f`), the size of
#     the machine (`controllers=`) and whether the visit kept a session
#     (`taken=`), and every connected device gets its own `[USB ]` line with
#     the port, the speed, the role, the state, the reason and - once its
#     descriptors were read - its vid/pid and interface list;
#   * the device records reach the durable boot log, because a pointer that is
#     plugged in but not bound has to be readable from the disk the machine
#     came up on;
#   * the QEMU proof boots two controllers at fixed PCI addresses, with the
#     keyboard on one and the pointer on the other, drives both (the keyboard
#     opens the menu, the monitor's click runs an entry) and then repeats the
#     boot with a non-HID device on the second controller, which must publish
#     itself with vid/pid and a reason;
#   * the host proof drives the machine shape through the scan policy: the
#     keyboard is bound on one controller and the controllers behind it are
#     still visited.

for two_controller_file in \
    kernel/src/xhci.c \
    kernel/include/xhci.h \
    kernel/src/xhci_trb.c \
    kernel/src/appliance_boot_log.c \
    tests/xhci-host.c \
    scripts/test-qemu-usb-two-controllers.sh; do
    [[ -f $two_controller_file ]] || {
        echo "ITEM-452: отсутствует $two_controller_file" >&2
        exit 1
    }
done

# 1. The session model: one pool, one event ring and one pair of role devices
#    per controller, a small fixed array of them, and the `xhci` pointer every
#    field reference goes through.  ITEM-519 increment 2 added the mass-storage
#    device as a third persistent role, so the array moved from three sessions
#    to four and the walk's scratch guarantee is one more than the roles.
for two_controller_session in \
    '#define XHCI_SESSION_SLOTS 4U' \
    'struct xhci_session {' \
    'struct xhci_machine {' \
    'static struct xhci_session xhci_sessions[XHCI_SESSION_SLOTS];' \
    'static struct xhci_session *xhci = &xhci_sessions[0];' \
    'static struct xhci_machine xhci_machine;' \
    'static struct xhci_session *xhci_session_acquire(void) {' \
    'static void xhci_session_release(struct xhci_session *session) {' \
    'static bool xhci_session_holds_role(const struct xhci_session *session) {' \
    'xhci->dma_physical = pmm_alloc_contiguous_pages(pages);' \
    'XHCI_SESSION_SLOTS > XHCI_HID_DEVICES + 1U'; do
    grep -Fq -- "$two_controller_session" kernel/src/xhci.c || {
        echo "ITEM-452: драйвер не держит сессию на контроллер:" \
            "$two_controller_session" >&2
        exit 1
    }
done

# 2. The walk: every controller the PCI scan found, in order, with no exit on a
#    bound role, and a session kept only while it holds one.
for two_controller_walk in \
    'for (size_t index = 0U; index < controller_count; ++index) {' \
    'struct xhci_session *session = xhci_session_acquire();' \
    'if (!xhci_session_holds_role(session)) {' \
    'xhci_session_release(session);' \
    'xhci_scan_begin_controller(&scan);' \
    'xhci_scan_note_controller(scan, xhci->status, xhci->detail,' \
    'xhci_machine.keyboard_session == xhci);' \
    'xhci_machine.keyboard_session = session;' \
    'xhci_machine.mouse_session = xhci;' ; do
    grep -Fq -- "$two_controller_walk" kernel/src/xhci.c || {
        echo "ITEM-452: обход не посещает каждый контроллер:" \
            "$two_controller_walk" >&2
        exit 1
    }
done
# The old early exit must be gone: a keyboard that was found may not stop the
# machine-wide walk.
if grep -Fq 'if (scan.selected) {' kernel/src/xhci.c; then
    echo "ITEM-452: обход всё ещё останавливается на выбранном контроллере" >&2
    exit 1
fi
if grep -Fq 'scan_record_suppressed' kernel/src/xhci.c; then
    echo "ITEM-452: визит всё ещё скрывает свою запись" >&2
    exit 1
fi

# 3. The service pass is round-robin over the live sessions, and the events it
#    does not own are applied rather than dropped: the control and command waits
#    share each session's event ring, and a completion consumed there would
#    leave the other role's transfer_pending set forever.
for two_controller_service in \
    'static uint32_t xhci_service_cursor;' \
    'static void xhci_service_session(void) {' \
    '(xhci_service_cursor + step) % XHCI_SESSION_SLOTS' \
    'xhci_service_cursor = (xhci_service_cursor + 1U) % XHCI_SESSION_SLOTS;' \
    'if (!session->devices[XHCI_HID_ROLE_KEYBOARD].ready &&' \
    'xhci = xhci_machine.keyboard_session;' \
    'static bool xhci_apply_event(const struct xhci_event *event);' \
    'static bool xhci_apply_event(const struct xhci_event *event) {' \
    '(void)xhci_apply_event(&event);' \
    'static void xhci_rearm_devices(void) {' ; do
    grep -Fq -- "$two_controller_service" kernel/src/xhci.c || {
        echo "ITEM-452: обслуживание не обходит живые сессии:" \
            "$two_controller_service" >&2
        exit 1
    }
done
# The accessors read the role's own session, not the session the walk happened
# to leave the pointer on.
for two_controller_accessor in \
    'static struct xhci_session *xhci_role_session(uint8_t role) {' \
    'static struct xhci_hid_device *xhci_role_device(uint8_t role) {' \
    'xhci_role_device(XHCI_HID_ROLE_KEYBOARD)' \
    'xhci_role_device(XHCI_HID_ROLE_MOUSE)' \
    'xhci_machine.verdict_status' \
    'xhci_machine.verdict_detail'; do
    grep -Fq -- "$two_controller_accessor" kernel/src/xhci.c || {
        echo "ITEM-452: доступ не читает свою роль:" \
            "$two_controller_accessor" >&2
        exit 1
    }
done

# 4. The publication: the controller names itself, and every device has its own
#    line with the port, speed, role, state, reason and identity.
for two_controller_record in \
    '"[XHCI] state="' \
    '" pci="' \
    '" controllers="' \
    '" taken="' \
    'xhci_machine.controllers_total' \
    '"[USB ] pci="' \
    '" port="' \
    '" speed="' \
    '" role="' \
    '" state="' \
    '" reason="' \
    '" vid="' \
    '" pid="' \
    '" ifaces="' \
    '"ROLE_TAKEN"' \
    'XHCI_STATUS_ROLE_TAKEN' \
    'static void xhci_publish_hex16(uint16_t value) {' \
    '"KEYBOARD_ALREADY_BOUND"' \
    '"ROLES_ALREADY_BOUND"' \
    'static void xhci_log_device_record('; do
    grep -Fq -- "$two_controller_record" kernel/src/xhci.c || {
        echo "ITEM-452: запись не называет контроллер/устройство:" \
            "$two_controller_record" >&2
        exit 1
    }
done
grep -Fq '"[USB ]"' kernel/src/appliance_boot_log.c || {
    echo "ITEM-452: записи устройств не попадают в долговечный журнал" >&2
    exit 1
}

# The attach budget may not cut the walk short: every connected device still
# names itself, and a device without an attempt left is published as REFUSED
# with the budget's reason.
if grep -Fq 'budget_exhausted' kernel/src/xhci.c; then
    echo "ITEM-452: исчерпанный бюджет попыток всё ещё обрывает обход портов" >&2
    exit 1
fi

# 5. The host proof drives all-controller walk through the scan policy.
for two_controller_host in \
    'scan.controllers_seen == scan.controllers_total' \
    'every controller was visited, including the one behind the keyboard' \
    'the controller behind the keyboard was visited too' \
    'the walk reached the machine'"'"'s last controller' \
    'the 79-byte detail and the port evidence cannot share one line' \
    'the refused device'"'"'s own line fits the console'"'"'s one-line budget' \
    'a role the machine already holds is named' \
    'XHCI_STATUS_ROLE_TAKEN'; do
    grep -Fq -- "$two_controller_host" tests/xhci-host.c || {
        echo "ITEM-452: host-проба не фиксирует обход:" \
            "$two_controller_host" >&2
        exit 1
    }
done

# 6. The QEMU proof: two controllers at fixed addresses, the keyboard on one
#    and the pointer on the other, both driven, plus the non-HID negative.
[[ -x scripts/test-qemu-usb-two-controllers.sh ]] || {
    echo "ITEM-452: QEMU-проба двух контроллеров отсутствует или не исполняема" >&2
    exit 1
}
for two_controller_qemu in \
    "-device 'qemu-xhci,id=x0,bus=pcie.0,addr=0x10'" \
    "-device 'qemu-xhci,id=x1,bus=pcie.0,addr=0x11'" \
    "-device 'usb-kbd,bus=x0.0'" \
    "-device 'usb-mouse,bus=x1.0'" \
    'usb-storage,drive=usbstick,bus=x1.0' \
    'i8042=off,usb=off' \
    'pci=([0-9A-F]{2}:[0-9A-F]{2}\.[0-9A-F])' \
    'controllers=2' \
    'taken=YES' \
    'taken=NO' \
    'role=(\w+) state=BOUND' \
    '[USB ] ' \
    'state=REFUSED' \
    'vid=0x' \
    'mouse_move' \
    'mouse_button' \
    'entry=status action=RUN' \
    '[LOCAL-MENU] run action=status' \
    'LOCAL INPUT: SOURCE=USB_HID USB=READY' \
    'proven=(\d+) ' \
    'retry=(\d+)' \
    'the two-controller ' \
    'proven path, not only the re-bind' \
    'sendkey shift' ; do
    grep -Fq -- "$two_controller_qemu" scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-452: QEMU-проба не несёт: $two_controller_qemu" >&2
        exit 1
    }
done
# The negative names what QEMU cannot reproduce instead of asserting it.
# ITEM-453 removed the "no SuperSpeed device model" half of that sentence - the
# proof now boots QEMU's `usb-uas`, whose descriptor carries the SuperSpeed
# device descriptor, on a USB3 port - so what remains named is the part that is
# still true: the port power and link-state semantics of the bring-up.
for two_controller_honest in \
    'ignores' \
    'writes to PORTSC.PED' \
    'hardware question' \
    'always reports PORTSC.PP set' \
    'never leaves RxDetect for an empty' ; do
    grep -Fq -- "$two_controller_honest" \
        scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-452: QEMU-проба не оговаривает предел модели:" \
            "$two_controller_honest" >&2
        exit 1
    }
done
grep -Fq 'usb_two_controllers: the boot panicked' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-452: QEMU-проба не отказывает на панике" >&2
    exit 1
}
grep -Fq 'test-usb-two-controllers-qemu' Makefile || {
    echo "ITEM-452: QEMU-проба не собрана в Makefile" >&2
    exit 1
}
awk '/^QEMU_PARALLEL_PHASE_1 :=/,/^$/' Makefile |
    grep -Fq 'test-usb-two-controllers-qemu' || {
    echo "ITEM-452: QEMU-проба не входит в параллельную фазу" >&2
    exit 1
}

# 7. The documentation records the item.
for two_controller_doc in \
    docs/hfoc-appliance-boot-console.md \
    docs/hfoc-known-limitations.md \
    docs/hfoc-installed-boot.md \
    docs/ARCHITECTURE.md; do
    grep -Fq 'ITEM-452' "$two_controller_doc" || {
        echo "ITEM-452: документ не описывает пункт: $two_controller_doc" >&2
        exit 1
    }
done
