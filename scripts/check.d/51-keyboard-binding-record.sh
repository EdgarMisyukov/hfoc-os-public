#!/usr/bin/env bash
# ITEM-436: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The input record names the interface it bound and the traffic it carried.
#
# The 2026-09-17 hardware run published a bound keyboard and nothing else:
#
#   [XHCI] state=READY ... hid=BOOT_KEYBOARD port=10 speed=FULL
#       vid=0x046D pid=0xC53F max_packet=12
#   [KBD ] state=READY reason=USB_HID ... source=USB_HID
#       ps2=KEYBOARD_ABSENT usb=OK speed=FULL vid=0x046D pid=0xC53F
#
# The device is a Logitech wireless receiver that answers with more than one
# HID interface, and the record did not say which one the driver selected, on
# which endpoint, or whether any report ever arrived. The driver decoded a
# completed transfer only when it carried exactly eight bytes and dropped every
# other length without counting it, so a keyboard that was bound and answered
# with short reports read exactly like a keyboard that answered nothing - the
# two states the next hardware run has to tell apart.
#
# Four claims are pinned here, because the next hardware boot reads them:
#
#   * a completed interrupt IN transfer is classified against the 8-byte boot
#     report before the decoder sees it (whole / short / impossible), and the
#     classification is a pure function the host proof owns;
#   * the driver counts the transfers that did not carry a whole report and
#     remembers the byte count of the last completion, and it keeps the
#     descriptor values of the interface it bound;
#   * the boot record publishes the binding (`iface=`, `protocol=`, `ep=`,
#     `interval=`, `max_packet=`) and the heartbeat publishes the traffic
#     (`kbd_reports=`, `kbd_xfer=`, `kbd_err=`, `kbd_ref=`, `kbd_last=`), which
#     is what makes the next run readable without a tcpdump or a second boot;
#   * both QEMU proofs require the fields, so a record that loses them fails
#     the phase instead of being read past.
for keyboard_binding_file in \
    kernel/include/usb_hid.h \
    kernel/src/usb_hid.c \
    kernel/include/xhci.h \
    kernel/src/xhci.c \
    kernel/src/console.c \
    tests/usb-hid-host.c; do
    [[ -f $keyboard_binding_file ]] || {
        echo "ITEM-436: отсутствует $keyboard_binding_file" >&2
        exit 1
    }
done

# The classification is one function with three outcomes, declared where the
# decoder's other contract lives and implemented without an underflowing
# subtraction: a residual past the 8-byte buffer is named, not wrapped.
for keyboard_binding_contract in \
    'enum usb_hid_report_length {' \
    'USB_HID_REPORT_LENGTH_EXACT = 0' \
    'USB_HID_REPORT_LENGTH_SHORT' \
    'USB_HID_REPORT_LENGTH_IMPOSSIBLE' \
    'usb_hid_boot_report_classify('; do
    grep -Fq -- "$keyboard_binding_contract" kernel/include/usb_hid.h || {
        echo "ITEM-436: заголовок не объявляет классификацию:" \
            "$keyboard_binding_contract" >&2
        exit 1
    }
done
for keyboard_binding_impl in \
    'if (transfer_length > USB_HID_BOOT_REPORT_LENGTH) {' \
    '*received_bytes = 0U;' \
    '*received_bytes = USB_HID_BOOT_REPORT_LENGTH - transfer_length;' \
    'return transfer_length == 0U ? USB_HID_REPORT_LENGTH_EXACT'; do
    grep -Fq -- "$keyboard_binding_impl" kernel/src/usb_hid.c || {
        echo "ITEM-436: классификация не выполняет свой контракт:" \
            "$keyboard_binding_impl" >&2
        exit 1
    }
done
grep -Fq 'test_report_length_classification();' tests/usb-hid-host.c || {
    echo "ITEM-436: host-проба не вызывает классификацию" >&2
    exit 1
}
grep -Fq 'test-usb-hid-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-436: манифест host suite не содержит test-usb-hid-host" >&2
    exit 1
}

# The driver counts what the decoder refused and keeps the binding it selected;
# a counter nobody increments is not evidence.
#
# ITEM-438: the classification is now one branch of a two-mode decode (a
# report-protocol keyboard is classified against its own layout instead), so
# the pin names the call and its arguments rather than one formatting of the
# line - the contract is that a completed boot transfer is classified, not how
# the call is wrapped.
#
# ITEM-448: the driver holds the keyboard and the pointer as two devices of
# their own, so the counters and the binding below live on the role's device -
# the contract is unchanged (the completed transfer is classified, the refused
# report and the last byte count are kept, the selected interface and endpoint
# are published), and the pin follows the state that now owns them.
for keyboard_binding_driver in \
    'usb_hid_boot_report_classify(transfer_length,' \
    'usb_hid_layout_build_boot_report(' \
    'device->last_report_bytes = received;' \
    '++device->refused_report_count;' \
    'device->interface_protocol = config.interface_protocol;' \
    'device->endpoint_address = config.endpoint_address;' \
    'device->refused_report_count = 0U;' \
    'device->last_report_bytes = 0U;'; do
    grep -Fq -- "$keyboard_binding_driver" kernel/src/xhci.c || {
        echo "ITEM-436: драйвер не ведёт привязку или счётчики:" \
            "$keyboard_binding_driver" >&2
        exit 1
    }
done
for keyboard_binding_accessor in \
    'uint8_t xhci_keyboard_interface(void);' \
    'uint8_t xhci_keyboard_protocol(void);' \
    'uint8_t xhci_keyboard_endpoint_address(void);' \
    'uint8_t xhci_keyboard_endpoint_interval(void);' \
    'uint16_t xhci_keyboard_endpoint_max_packet(void);' \
    'uint32_t xhci_keyboard_refused_count(void);' \
    'uint32_t xhci_keyboard_last_report_bytes(void);'; do
    grep -Fq -- "$keyboard_binding_accessor" kernel/include/xhci.h || {
        echo "ITEM-436: доступ к привязке не объявлен:" \
            "$keyboard_binding_accessor" >&2
        exit 1
    }
done

# The records carry the fields. The boot record names the binding, and the
# heartbeat names the traffic; both are read off the appliance's own disk by
# the hardware run.
for keyboard_binding_record in \
    '" iface="' \
    '" protocol="' \
    '" ep=0x"' \
    '" interval="' \
    '" max_packet="' \
    'xhci_keyboard_interface()' \
    'xhci_keyboard_protocol()' \
    'xhci_keyboard_endpoint_address()' \
    'xhci_keyboard_endpoint_interval()' \
    'xhci_keyboard_endpoint_max_packet()'; do
    grep -Fq -- "$keyboard_binding_record" kernel/src/kernel.c || {
        echo "ITEM-436: запись не несёт поле привязки:" \
            "$keyboard_binding_record" >&2
        exit 1
    }
done
for keyboard_binding_heartbeat in \
    '" kbd_reports="' \
    '" kbd_xfer="' \
    '" kbd_err="' \
    '" kbd_ref="' \
    '" kbd_last="'; do
    grep -Fq -- "$keyboard_binding_heartbeat" kernel/src/kernel.c || {
        echo "ITEM-436: heartbeat не несёт поле трафика ввода:" \
            "$keyboard_binding_heartbeat" >&2
        exit 1
    }
done
for keyboard_binding_console in \
    '" REFUSED="' \
    '" LAST="' \
    'xhci_keyboard_refused_count()' \
    'xhci_keyboard_last_report_bytes()'; do
    grep -Fq -- "$keyboard_binding_console" kernel/src/console.c || {
        echo "ITEM-436: команда kbd не публикует счётчики:" \
            "$keyboard_binding_console" >&2
        exit 1
    }
done

# Both QEMU proofs require the fields: the keyboard phase names the binding and
# the live counters, the DHCP/DNS phase names the heartbeat's input fields.
# ITEM-438 extended the binding with the DCI and the descriptor's own
# bInterval, so the pattern names them too (the record's `interval=` is the
# exponent of that number for a low- or full-speed endpoint).
for keyboard_binding_qemu in \
    'dci=[0-9]+ interval=[0-9]+ binterval=[0-9]+ max_packet=8' \
    'ERRORS=0 REFUSED=0 LAST=8'; do
    grep -Fq -- "$keyboard_binding_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-436: QEMU-проба клавиатуры не требует: $keyboard_binding_qemu" >&2
        exit 1
    }
done
for keyboard_binding_heartbeat_qemu in \
    'kbd_reports=[0-9]+.*kbd_xfer=[0-9]+' \
    'kbd_err=[0-9]+.*kbd_ref=[0-9]+.*kbd_last=[0-9]+'; do
    grep -Fq -- "$keyboard_binding_heartbeat_qemu" \
        scripts/test-qemu-production-dhcp-dns.sh || {
        echo "ITEM-436: QEMU-проба heartbeat не требует:" \
            "$keyboard_binding_heartbeat_qemu" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks: the console document owns the
# local input path, and the queue owns the item itself.
for keyboard_binding_doc in \
    'ITEM-436' \
    'iface='; do
    grep -Fq -- "$keyboard_binding_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-436:" \
            "$keyboard_binding_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-436' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-436" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# ITEM-438: the keyboard is polled when it asked to be, and its silence is a
# finding.
#
# The 2026-09-17 hardware run read `[KBD ] state=READY ... iface=0 protocol=1
# ep=0x81 interval=8 max_packet=12 freq=...`: a bound keyboard on the endpoint
# the driver selected, with every counter at zero and no line anywhere saying
# the device had never answered. Two defects were behind the silence:
#
#   * the Interval field was programmed as `bInterval * 8`, a microframe count,
#     where xHCI 1.2 section 6.2.3.6 has an exponent (2^Interval x 125 us, the
#     field in 3..10 for a low- or full-speed interrupt endpoint): the
#     receiver's 1 ms endpoint was polled every 32 ms, and QEMU's model - which
#     only exponentiates whatever it is handed - could not show it;
#   * `kbd_reports` counted whole reports, so a device that answers every poll
#     with an idle report and a device that answers nothing at all were the
#     same three characters, and a bound keyboard that never answered was
#     published exactly like one nobody had touched.
#
# The claims below are what the next hardware run reads, so they are pinned:
# the exponent conversion and its range, the descriptor's own bInterval next to
# it, the keys the decoded reports produced, and the one-shot NO_REPORT finding
# whose window is the console's own idle window.
for keyboard_polling_file in \
    kernel/include/xhci_trb.h \
    kernel/src/xhci_trb.c \
    kernel/src/kernel.c \
    kernel/src/console.c \
    tests/xhci-host.c \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $keyboard_polling_file ]] || {
        echo "ITEM-438: отсутствует $keyboard_polling_file" >&2
        exit 1
    }
done

# The Interval field: one function, an exponent, and the range the
# specification gives it. The host proof pins the table these names feed.
for keyboard_polling_interval in \
    '#define XHCI_INTERVAL_FS_MIN 3U' \
    '#define XHCI_INTERVAL_FS_MAX 10U'; do
    grep -Fq -- "$keyboard_polling_interval" kernel/include/xhci_trb.h || {
        echo "ITEM-438: заголовок не объявляет диапазон Interval:" \
            "$keyboard_polling_interval" >&2
        exit 1
    }
done
for keyboard_polling_conversion in \
    'uint32_t microframes = b_interval;' \
    'uint8_t exponent = XHCI_INTERVAL_FS_MIN;' \
    'while (microframes > 1U && exponent < XHCI_INTERVAL_FS_MAX) {' \
    'microframes >>= 1U;' \
    '++exponent;' \
    '6.2.3.6'; do
    grep -Fq -- "$keyboard_polling_conversion" kernel/src/xhci_trb.c || {
        echo "ITEM-438: интервал не переводится в экспоненту:" \
            "$keyboard_polling_conversion" >&2
        exit 1
    }
done
# The old rule must be gone: a microframe count in this field is the defect.
if grep -Fq 'return (uint8_t)(frames * 8U);' kernel/src/xhci_trb.c; then
    echo "ITEM-438: интервал всё ещё программируется счётчиком микрофреймов" >&2
    exit 1
fi

# The host proof owns the table: the acceptance host's receiver (1 ms), the
# K120 (10 ms), the 128 ms ceiling and the round-down of a period that is no
# power of two.
for keyboard_polling_host in \
    'xhci_endpoint_interval(XHCI_SPEED_FULL, 1U) == 3U' \
    'xhci_endpoint_interval(XHCI_SPEED_FULL, 10U) == 6U' \
    'xhci_endpoint_interval(XHCI_SPEED_LOW, 255U) == 10U' \
    'xhci_endpoint_interval(XHCI_SPEED_FULL, 9U) == 6U' \
    'test_no_report_window();'; do
    grep -Fq -- "$keyboard_polling_host" tests/xhci-host.c || {
        echo "ITEM-438: host-проба не фиксирует таблицу интервалов:" \
            "$keyboard_polling_host" >&2
        exit 1
    }
done

# The driver keeps both numbers and counts the keys, and the window decision is
# a pure function of the binding, the counter and the clock.  ITEM-448 moved
# both numbers onto the keyboard's own device state; the contract is the same.
for keyboard_polling_driver in \
    'device->endpoint_binterval = config.endpoint_interval;' \
    'device->key_count += (uint32_t)count;' \
    'device->key_count = 0U;' \
    'bool usb_hid_no_report_due(' \
    'return bound && decoded_reports == 0U && elapsed_seconds >= window_seconds;'; do
    grep -Fq -- "$keyboard_polling_driver" kernel/src/xhci.c \
        kernel/src/usb_hid.c kernel/include/usb_hid.h || {
        echo "ITEM-438: драйвер не ведёт опрос или окно молчания:" \
            "$keyboard_polling_driver" >&2
        exit 1
    }
done
for keyboard_polling_accessor in \
    'uint8_t xhci_keyboard_dci(void);' \
    'uint8_t xhci_keyboard_endpoint_binterval(void);' \
    'uint32_t xhci_keyboard_key_count(void);'; do
    grep -Fq -- "$keyboard_polling_accessor" kernel/include/xhci.h || {
        echo "ITEM-438: доступ к опросу не объявлен:" \
            "$keyboard_polling_accessor" >&2
        exit 1
    }
done

# The records: the binding names the DCI and both interval numbers, the
# heartbeat names the keys and the silence, and the finding is one line with
# the binding and the counters on it.
for keyboard_polling_record in \
    '" dci="' \
    '" binterval="' \
    '" kbd_keys="' \
    '" kbd_silent="' \
    'xhci_keyboard_dci()' \
    'xhci_keyboard_endpoint_binterval()' \
    'xhci_keyboard_key_count()' \
    'usb_hid_no_report_due(' \
    'USB_HID_NO_REPORT_WINDOW_SECONDS' \
    'finding=NO_REPORT'; do
    grep -Fq -- "$keyboard_polling_record" kernel/src/kernel.c || {
        echo "ITEM-438: запись не несёт поле опроса:" \
            "$keyboard_polling_record" >&2
        exit 1
    }
done
grep -Fq '" KEYS="' kernel/src/console.c || {
    echo "ITEM-438: команда kbd не публикует счётчик клавиш" >&2
    exit 1
}

# Both QEMU proofs require the new fields: the keyboard phase needs the binding
# and KEYS>0 from the typed command, the DHCP/DNS phase needs the heartbeat's
# two new fields.
grep -Fq 'KEYS=[1-9][0-9]*' scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-438: QEMU-проба клавиатуры не требует KEYS>0" >&2
    exit 1
}
grep -Fq 'kbd_keys=[0-9]+.*kbd_silent=[0-9]+' \
    scripts/test-qemu-production-dhcp-dns.sh || {
    echo "ITEM-438: QEMU-проба heartbeat не требует поля опроса" >&2
    exit 1
}

# The item is recorded where a maintainer looks.
for keyboard_polling_doc in \
    'ITEM-438' \
    'binterval=' \
    'NO_REPORT'; do
    grep -Fq -- "$keyboard_polling_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-438:" \
            "$keyboard_polling_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-438' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-438" >&2
    exit 1
}

# ---------------------------------------------------------------------------
# ITEM-438, second half: the driver accepts what the hardware run refused.
#
# The first half (above) made the silence readable.  This half is the part that
# makes a keyboard type: the interface shapes the run refused are accepted, the
# report descriptor decides what an interface descriptor cannot, a SuperSpeed
# port is addressable, and the walk no longer stops at the first device that
# parses as a keyboard.
#
# The claims are pinned where they are implemented and proven:
for keyboard_accept_file in \
    kernel/include/usb_hid.h \
    kernel/src/usb_hid.c \
    kernel/src/xhci.c \
    tests/usb-hid-host.c \
    tests/xhci-host.c; do
    [[ -f $keyboard_accept_file ]] || {
        echo "ITEM-438: отсутствует $keyboard_accept_file" >&2
        exit 1
    }
done

# 1. The interface walk scores candidates instead of demanding the boot
#    keyboard protocol, and says which ones need their report descriptor read.
#    ITEM-448 made the walk role-aware: the same scoring asks for the role's
#    own boot protocol, so a boot mouse is no candidate for the keyboard and
#    the other way round.
for keyboard_accept_config in \
    'USB_HID_INTERFACE_SCORE_BOOT_KEYBOARD' \
    'USB_HID_INTERFACE_SCORE_REPORT_PROTOCOL' \
    'USB_HID_INTERFACE_SCORE_HID' \
    'static uint8_t usb_hid_interface_score(' \
    'if (interface_class != USB_HID_CLASS) {' \
    'interface_subclass == USB_HID_SUBCLASS_BOOT' \
    'if (interface_protocol == boot_protocol) {' \
    'return USB_HID_INTERFACE_SCORE_REPORT_PROTOCOL;' \
    'config.report_descriptor_needed =' \
    'candidate_needs_report_descriptor;'; do
    grep -Fq -- "$keyboard_accept_config" kernel/src/usb_hid.c || {
        echo "ITEM-438: разбор конфигурации не выбирает кандидата:" \
            "$keyboard_accept_config" >&2
        exit 1
    }
done
grep -Fq 'bool report_descriptor_needed;' kernel/include/usb_hid.h || {
    echo "ITEM-438: кандидат не объявляет нужду в report descriptor" >&2
    exit 1
}

# 2. The report-descriptor walk: the keyboard application collection, the
#    modifier field, the key array and the report ID, plus the conversion of a
#    report into the boot report the tested decoder already covers.
for keyboard_accept_report in \
    'bool usb_hid_parse_report_descriptor(' \
    'bool usb_hid_layout_build_boot_report(' \
    '#define USB_HID_USAGE_PAGE_GENERIC_DESKTOP 0x01U' \
    '#define USB_HID_USAGE_GENERIC_KEYBOARD 0x06U' \
    '#define USB_HID_USAGE_PAGE_KEYBOARD 0x07U' \
    '#define USB_HID_USAGE_KEYBOARD_MODIFIERS_MIN 0xE0U' \
    'USB_HID_MAIN_COLLECTION' \
    'keyboard_depth = depth;' \
    'report_count >= USB_HID_BOOT_KEY_COUNT' \
    'found.key_offset = (uint8_t)start;' \
    'boot_report[2U + index] = report[layout->key_offset + index];'; do
    grep -Fq -- "$keyboard_accept_report" kernel/src/usb_hid.c || {
        echo "ITEM-438: разбор report descriptor не полон:" \
            "$keyboard_accept_report" >&2
        exit 1
    }
done
for keyboard_accept_layout in \
    '#define USB_HID_REPORT_KEYS_MAX 6U' \
    '#define USB_HID_REPORT_LENGTH_MAX 64U' \
    '#define USB_HID_REPORT_OFFSET_NONE 0xFFU' \
    '#define USB_HID_REPORT_DESCRIPTOR_MAX 256U' \
    'struct usb_hid_report_layout {' \
    'uint8_t modifier_offset;' \
    'uint8_t key_offset;' \
    'uint8_t key_count;'; do
    grep -Fq -- "$keyboard_accept_layout" kernel/include/usb_hid.h || {
        echo "ITEM-438: разметка отчёта не объявлена:" \
            "$keyboard_accept_layout" >&2
        exit 1
    }
done

# 3. The driver reads the descriptor, keeps the layout, sends SET_PROTOCOL(boot)
#    only to an interface that promised it, asks for a once-a-second idle report
#    and submits one whole report of the bound layout.  ITEM-448: the layout,
#    the report length and the mode belong to the keyboard's own device state
#    (the mouse carries its own), so the pins name that state.
for keyboard_accept_driver in \
    'static bool xhci_parse_report_layout(' \
    'USB_DESCRIPTOR_HID_REPORT << 8' \
    'if (config.report_descriptor_needed) {' \
    'config.interface_subclass == USB_HID_SUBCLASS_BOOT &&' \
    'USB_HID_BOOT_PROTOCOL_VALUE' \
    'USB_HID_REQUEST_SET_IDLE,' \
    'USB_HID_IDLE_PERIOD' \
    'device->report_length = device->report_mode' \
    '? config.endpoint_max_packet' \
    'whole_report = usb_hid_layout_build_boot_report(' \
    'device->ready && device->report_mode' ; do
    grep -Fq -- "$keyboard_accept_driver" kernel/src/xhci.c || {
        echo "ITEM-438: драйвер не принимает report-протокол:" \
            "$keyboard_accept_driver" >&2
        exit 1
    }
done
grep -Fq '#define USB_HID_IDLE_PERIOD 250U' kernel/include/usb_hid.h || {
    echo "ITEM-438: окно простоя не объявлено" >&2
    exit 1
}
grep -Fq 'USB_HID_BOOT_PROTOCOL_VALUE 0x00U' kernel/include/usb_hid.h || {
    echo "ITEM-438: значение boot-протокола не объявлено" >&2
    exit 1
}

# 4. SuperSpeed is addressable: the 512-byte control pipe and the interval the
#    field itself carries.
grep -Fq 'ITEM-438: USB 3.0 section 9.6.1 gives a SuperSpeed device a' \
    kernel/src/xhci_trb.c || {
    echo "ITEM-438: SuperSpeed не поддержан в таблице скоростей" >&2
    exit 1
}
for keyboard_accept_speed in \
    'xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER) == 512U' \
    'xhci_speed_ep0_max_packet(XHCI_SPEED_SUPER_PLUS) == 512U' \
    'xhci_endpoint_interval(XHCI_SPEED_SUPER, 10U) == 10U' \
    'xhci_endpoint_interval(XHCI_SPEED_SUPER, 255U) == 16U'; do
    grep -Fq -- "$keyboard_accept_speed" tests/xhci-host.c || {
        echo "ITEM-438: host-проба не фиксирует SuperSpeed:" \
            "$keyboard_accept_speed" >&2
        exit 1
    }
done

# 5. The walk continues past a silent candidate: a bounded proof window, the
#    release, the finding that is published instead of a READY record, and the
#    re-bind of the first silent candidate when nothing better answered.
#    ITEM-452: the counters and the remembered candidate live in the machine
#    state (the walk works in one session at a time), and the retry opens the
#    candidate's controller only when the keyboard role is still unbound; the
#    device that was released was already named by its own [USB ] line, so the
#    visit no longer suppresses the controller's record.
for keyboard_accept_walk in \
    '#define XHCI_CANDIDATE_PROVE_MS 1200U' \
    'static bool xhci_candidate_prove(struct xhci_hid_device *device)' \
    'static void xhci_release_candidate(struct xhci_hid_device *device)' \
    'static void xhci_log_candidate_rejection(' \
    '"[KBD ] state=UNAVAILABLE reason=NO_REPORT detail=P"' \
    'if (xhci_candidate_prove(device)) {' \
    'xhci_machine.silent_candidate.valid = true;' \
    'xhci_release_candidate(device);' \
    'xhci_machine.silent_retry = true;' \
    'if (xhci_machine.keyboard_session == NULL &&' \
    'xhci_machine.silent_candidate.valid) {' \
    'static bool xhci_rebind_device(struct xhci_hid_device *device,' \
    'xhci_port_claim(port, NULL);' \
    'xhci_attach_device(device, port, speed)) {'; do
    grep -Fq -- "$keyboard_accept_walk" kernel/src/xhci.c || {
        echo "ITEM-438: обход не продолжается после молчащего кандидата:" \
            "$keyboard_accept_walk" >&2
        exit 1
    }
done

# 6. The records name the mode, the layout, the idle verdict and the walk.
for keyboard_accept_record in \
    '" mode="' \
    '" report_id="' \
    '" keys_at="' \
    '" keys="' \
    '" idle="' \
    '" candidates="' \
    '" silent="' \
    '" proven="' \
    '" retry="' \
    'xhci_keyboard_report_mode()' \
    'xhci_keyboard_idle_refused()' \
    'xhci_keyboard_candidates_proven()' \
    'xhci_keyboard_silent_retry()'; do
    grep -Fq -- "$keyboard_accept_record" kernel/src/kernel.c || {
        echo "ITEM-438: запись не несёт режим/разметку/обход:" \
            "$keyboard_accept_record" >&2
        exit 1
    }
done
# ITEM-452: the controller record names the role, and the device's own
# [USB ] line carries the port, the speed and the identity beside it.
for keyboard_accept_hid in \
    '" hid=REPORT_KEYBOARD"' \
    '" hid=BOOT_KEYBOARD"' \
    'keyboard->report_mode ?' \
    '"[USB ] pci="' \
    '" port="' \
    '" speed="' \
    '" role="' \
    'identity->vendor_id'; do
    grep -Fq -- "$keyboard_accept_hid" kernel/src/xhci.c || {
        echo "ITEM-438: запись [XHCI] не называет режим клавиатуры:" \
            "$keyboard_accept_hid" >&2
        exit 1
    }
done

# 7. The host proofs drive the new walks, including the negative shapes.
for keyboard_accept_proof in \
    'test_parse_report_protocol_candidates();' \
    'test_report_descriptor_walk();' \
    'test_report_protocol_decode();' \
    'test_parse_receiver_is_a_report_descriptor_candidate();' \
    'usb_hid_parse_report_descriptor(mouse_report_descriptor,' \
    'usb_hid_layout_build_boot_report(&layout, other_id,'; do
    grep -Fq -- "$keyboard_accept_proof" tests/usb-hid-host.c || {
        echo "ITEM-438: host-проба HID не покрывает новый разбор:" \
            "$keyboard_accept_proof" >&2
        exit 1
    }
done
grep -Fq 'sizeof(keyboard_report_descriptor) - 8U' tests/usb-hid-host.c || {
    echo "ITEM-438: host-проба не проверяет обрезанный report descriptor" >&2
    exit 1
}

# 8. The documentation records the item and the fields a hardware run reads.
for keyboard_accept_doc in \
    'ITEM-438' \
    'REPORT_KEYBOARD' \
    'report_id=' \
    'keys_at=' \
    'candidates=' \
    'idle='; do
    grep -Fq -- "$keyboard_accept_doc" \
        docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-438:" \
            "$keyboard_accept_doc" >&2
        exit 1
    }
done

