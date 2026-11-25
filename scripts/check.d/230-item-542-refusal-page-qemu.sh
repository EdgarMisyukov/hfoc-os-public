#!/usr/bin/env bash
# ITEM-542: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-541 put the medium verdict, the journal state and the bounded ring tail
# on the refusal page, but only the host half proved the text; the machine was
# never asked.  This slice pins the QEMU half:
#   * one shape-based probe reads every refusal page: a machine record is
#     selected by the fields only a record carries (`pci=`, `scope=`,
#     `loader_id=`) and a page line by its own shape - the bare medium verdict
#     and the journal line with its `pending=`/`dropped=`/`refused=` counters;
#   * the optical refusal proves the "no USB medium at all" shape
#     (`UNAVAILABLE reason=NO_DEVICE` beside `UNAVAILABLE reason=READ_ONLY`);
#   * the USB refusal proves the shape F-7 left unreadable: the operator medium
#     is recognised (`state=MATCH reason=OK`, loader and device ids equal) while
#     its reserved journal frame is damaged (`UNAVAILABLE reason=HEADER`);
#   * the tail is bounded on the machine (`shown=N`, N <= 12), exactly N records
#     stand between the header and the way out, and the way out is still there;
#   * the two-controller proof selects its machine records by the same shape, so
#     a page line can never shadow the record it asserts.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item542_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item542_root" || exit 1

for item542_file in \
    scripts/test-qemu-installer-disk-screen.sh \
    scripts/test-qemu-usb-two-controllers.sh \
    Makefile \
    docs/hfoc-installer-storage.md; do
    [[ -f $item542_file ]] || {
        echo "ITEM-542: отсутствует $item542_file" >&2
        exit 1
    }
done

# 1. One probe reads both refusal shapes, and the page's lines are told from the
#    machine's records by shape rather than by position.
item542_proof=scripts/test-qemu-installer-disk-screen.sh
for item542_pin in \
    'refusal_page_probe() {' \
    'run_usb_refusal_case() {' \
    'unbound_medium() {' \
    'extra = sys.argv[10:-1]' \
    'page_medium = f"[INSTALL-MEDIUM] state={medium_state} reason={medium_reason}"' \
    'page_journal = f"[INSTALL-LOG] state={journal_state} reason={journal_reason}"' \
    'if line.startswith(page_journal + " pending=")' \
    'if line.startswith("[INSTALL-MEDIUM] pci=") and' \
    '" scope=WHOLE " in line]' \
    'r" loader_id=([0-9A-F]+) disk_id=([0-9A-F]+)"' \
    'tail_indices = set(range(header_index + 1, header_index + 1 + shown))' \
    'outside = [line for index, line in enumerate(lines)' \
    'r"\[INSTALL-LOG-TAIL\] shown=(\d+)"' \
    'if shown > 12:' \
    'if press_index != header_index + 1 + shown:'; do
    grep -Fq -- "$item542_pin" "$item542_proof" || {
        echo "ITEM-542: проба страницы не несёт '$item542_pin'" >&2
        exit 1
    }
done

# 2. The two refusal cases and their expectations: no USB medium at all, and a
#    recognised medium whose reserved journal frame is damaged.
for item542_pin in \
    'UNAVAILABLE NO_DEVICE UNAVAILABLE READ_ONLY 0' \
    'MATCH OK UNAVAILABLE HEADER 1' \
    'run_refusal_case refusal NO_DISK' \
    'run_usb_refusal_case unbound NO_DISK'; do
    grep -Fq -- "$item542_pin" "$item542_proof" || {
        echo "ITEM-542: случай отказа не называет '$item542_pin'" >&2
        exit 1
    }
done
# The USB fixture boots the medium itself through a USB stick and damages the
# reserved frame by its documented layout, validated by the header CRC.
for item542_pin in \
    "qemu-xhci,id=item542-xhci" \
    "usb-storage,drive=item542-medium" \
    'MAGIC = b"HFOC-ILOG"' \
    'zlib.crc32(header[:CRC_BYTES])' \
    'data[valid[0]] = 0x00'; do
    grep -Fq -- "$item542_pin" "$item542_proof" || {
        echo "ITEM-542: стенд носителя не несёт '$item542_pin'" >&2
        exit 1
    }
done

# 3. The in-CI two-controller proof reads its records by the same shape, so the
#    page's bare verdict can never stand in for the record it asserts.
item542_two=scripts/test-qemu-usb-two-controllers.sh
for item542_pin in \
    'readonly_logs = [line for line in journal_lines if " pending=" not in line]' \
    'page_logs = [line for line in journal_lines if " pending=" in line]' \
    'if page_logs:' \
    'line.startswith("[INSTALL-MEDIUM] ") and " pci=" in line'; do
    grep -Fq -- "$item542_pin" "$item542_two" || {
        echo "ITEM-542: двухконтроллерная проба не различает '$item542_pin'" >&2
        exit 1
    }
done

# 4. The standalone proof stays a registered target and the document records it.
grep -Fq 'test-installer-disk-screen-qemu' Makefile || {
    echo "ITEM-542: цель QEMU-пробы экрана пропала" >&2
    exit 1
}
for item542_doc in ITEM-542 'state=MATCH reason=OK' 'reason=HEADER' \
    'reason=NO_DEVICE' 'NOT TESTED'; do
    grep -Fq -- "$item542_doc" docs/hfoc-installer-storage.md || {
        echo "docs/hfoc-installer-storage.md не фиксирует ITEM-542:" \
            "$item542_doc" >&2
        exit 1
    }
done

# 5. The queue keeps this item as history, the roadmap records it.  ITEM-543
#    owns the current-item pin now.
item542_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '## Item ITEM-542' "$item542_queue" || {
    echo "ITEM-542: очередь потеряла исторический раздел ITEM-542" >&2
    exit 1
}
grep -Fq '## Item ITEM-541' "$item542_queue" || {
    echo "ITEM-542: очередь потеряла исторический раздел ITEM-541" >&2
    exit 1
}
grep -Fq 'ITEM-542' docs/ROADMAP.md || {
    echo "ITEM-542: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
