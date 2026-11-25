#!/usr/bin/env bash
# ITEM-459: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The key that produced nothing names itself, and every printable usage has a
# character.
#
# The 2026-09-19 hardware run pressed the key left of Enter on an ISO keyboard
# and got `*` (or nothing) where `|` was expected.  The USB HID character
# tables stopped at usage 0x38 while the array itself had 0x65 entries, so
# every usage above the US letter rows - the whole numeric keypad and 0x64
# `Keyboard Non-US \ and |`, the key in question - was zero and the decoder
# skipped it in silence: a dead key with no record anywhere.
#
# What is pinned here:
#
#   * the tables reach the printable usages of HID 1.11 Appendix B.1.2 up to
#     0x67 (Keypad =), with 0x64 `\` unshifted and `|` shifted, the numeric
#     keypad, and the US 0x31 `\` / `|` unchanged;
#   * the declared range moves with them (`USB_HID_USAGE_MAX` 0x67,
#     `USB_HID_USAGE_TABLE_SIZE` 0x68), so nothing above 0x38 is dead by
#     declaration either;
#   * a usage that still produces no character is kept in the decoder state and
#     handed to the caller (`usb_hid_take_unmapped`), with the keys the decoder
#     handles by name and the protocol's reserved usages excluded - Caps Lock
#     and the arrows are answers, not findings;
#   * the driver publishes it once per distinct usage as
#     `[KBD ] unmapped usage=0x.. modifier=0x..`, so a held key is one record
#     and the operator's next dead key names its own code;
#   * the host proof walks every printable US-ASCII character and requires it
#     to be reachable from some usage, states the usages the item is about, and
#     drives the finding (taken once, carrying its modifiers, silent for a
#     named key);
#   * the QEMU proof types a line of every sign HMP can name through the same
#     USB keyboard with a per-character echo, requires the completed repaint,
#     and requires exactly one `[KBD ] unmapped` record for one press of a key
#     the machine has no name for.
for usage_table_file in \
    kernel/include/usb_hid.h \
    kernel/src/usb_hid.c \
    kernel/src/xhci.c \
    tests/usb-hid-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $usage_table_file ]] || {
        echo "ITEM-459: отсутствует $usage_table_file" >&2
        exit 1
    }
done

# The tables reach the key the owner pressed and the keypad above the letters.
grep -Fq "[0x63]='.',[0x64]='\\\\',[0x67]='='" kernel/src/usb_hid.c || {
    echo "ITEM-459: 0x64 не даёт обратную косую черту" >&2
    exit 1
}
grep -Fq "[0x63]='.',[0x64]='|',[0x67]='+'" kernel/src/usb_hid.c || {
    echo "ITEM-459: 0x64 не даёт вертикальную черту с Shift" >&2
    exit 1
}
for usage_table_keypad in \
    "[0x54]='/',[0x55]='*',[0x56]='-',[0x57]='+',[0x58]='\\n'," \
    "[0x59]='1',[0x5a]='2',[0x5b]='3',[0x5c]='4',[0x5d]='5'," \
    "[0x5e]='6',[0x5f]='7',[0x60]='8',[0x61]='9',[0x62]='0',"; do
    grep -Fq -- "$usage_table_keypad" kernel/src/usb_hid.c || {
        echo "ITEM-459: цифровой блок не заполнен:" "$usage_table_keypad" >&2
        exit 1
    }
done
grep -Fq '#define USB_HID_USAGE_MAX 0x67U' kernel/include/usb_hid.h || {
    echo "ITEM-459: объявленный диапазон таблиц не поднят" >&2
    exit 1
}
grep -Fq '#define USB_HID_USAGE_TABLE_SIZE 0x68U' kernel/include/usb_hid.h || {
    echo "ITEM-459: размер таблиц не покрывает 0x67" >&2
    exit 1
}
if grep -Fq '#define USB_HID_USAGE_MAX 0x38U' kernel/include/usb_hid.h; then
    echo "ITEM-459: диапазон всё ещё обрывается на 0x38" >&2
    exit 1
fi

# A usage without a character is a finding the caller can publish.
grep -Fq 'uint8_t unmapped_usage;' kernel/include/usb_hid.h || {
    echo "ITEM-459: декодер не хранит неназванный usage" >&2
    exit 1
}
grep -Fq 'bool usb_hid_take_unmapped(struct usb_hid_state *state, uint8_t *usage_out,' \
    kernel/include/usb_hid.h || {
    echo "ITEM-459: нет доступа к находке" >&2
    exit 1
}
grep -Fq 'static bool usb_hid_usage_is_named(uint8_t usage) {' \
    kernel/src/usb_hid.c || {
    echo "ITEM-459: именованные клавиши не исключены из находок" >&2
    exit 1
}
grep -Fq 'static bool usb_hid_usage_is_reserved(uint8_t usage) {' \
    kernel/src/usb_hid.c || {
    echo "ITEM-459: зарезервированные usage не исключены из находок" >&2
    exit 1
}
grep -Fq 'state->unmapped_usage = usage;' kernel/src/usb_hid.c || {
    echo "ITEM-459: неназванный usage не доходит до вызывающего" >&2
    exit 1
}
grep -Fq 'bool usb_hid_take_unmapped(struct usb_hid_state *state, uint8_t *usage_out,' \
    kernel/src/usb_hid.c || {
    echo "ITEM-459: находка не отдаётся и не очищается" >&2
    exit 1
}

# The driver publishes it, once per distinct usage.
grep -Fq '"[KBD ] unmapped usage="' kernel/src/xhci.c || {
    echo "ITEM-459: драйвер не публикует неназванный usage" >&2
    exit 1
}
grep -Fq 'uint64_t unmapped_seen[4];' kernel/src/xhci.c || {
    echo "ITEM-459: у драйвера нет карты уже названных usage" >&2
    exit 1
}
grep -Fq 'xhci_note_unmapped_usage(device);' kernel/src/xhci.c || {
    echo "ITEM-459: разбор отчёта не сообщает о находке" >&2
    exit 1
}

# The host proof: the round trip, the pinned usages and the finding's rules.
for usage_table_proof in \
    '"the non-US backslash key produces a backslash"' \
    '"and a pipe with Shift, which is the key the owner pressed"' \
    '"an out-of-table usage still produces no character"' \
    '{0x64U, false,' \
    '{0x64U, true,' \
    '{0x56U, false,' \
    '{0x67U, false,' \
    '{0x64U, true, '"'"'|'"'"'},' \
    '{0x56U, false, '"'"'-'"'"'},' \
    '{0x67U, false, '"'"'='"'"'},' \
    '"every printable US-ASCII character is reachable"' \
    '"an unmapped usage is a finding"' \
    '"the finding carries the modifiers that were down"' \
    '"Caps Lock produces no character on purpose and is no finding"'; do
    grep -Fq -- "$usage_table_proof" tests/usb-hid-host.c || {
        echo "ITEM-459: host-проба не требует:" "$usage_table_proof" >&2
        exit 1
    }
done
grep -Fq 'test_printable_ascii_is_reachable();' tests/usb-hid-host.c || {
    echo "ITEM-459: round-trip по печатному ASCII не запускается" >&2
    exit 1
}
grep -Fq 'test_unmapped_usage_is_reported();' tests/usb-hid-host.c || {
    echo "ITEM-459: проба находки не запускается" >&2
    exit 1
}

# The QEMU proof types the signs and requires the finding.
for usage_table_qemu in \
    '("\\", "backslash"), ("|", "shift-backslash"),' \
    '("~", "shift-grave_accent"), ("`", "grave_accent"),' \
    '("<", "shift-comma"), (">", "shift-dot"),' \
    'type_line_exact(signs_line, signs_offset, signs_keys)' \
    'the punctuation line was never repainted whole' \
    'sendkey f13 1' \
    'the unmapped-usage record is missing or not exactly once'; do
    grep -Fq -- "$usage_table_qemu" \
        scripts/test-qemu-production-storage-boot.sh || {
        echo "ITEM-459: QEMU-проба не требует:" "$usage_table_qemu" >&2
        exit 1
    }
done

# The item is recorded where a maintainer looks.
for usage_table_doc in \
    'ITEM-459' \
    'unmapped usage'; do
    grep -Fq -- "$usage_table_doc" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-459:" \
            "$usage_table_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-459' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-459" >&2
    exit 1
}
grep -Fq 'ITEM-459' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-459" >&2
    exit 1
}
