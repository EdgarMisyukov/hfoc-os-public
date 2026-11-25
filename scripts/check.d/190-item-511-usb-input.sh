#!/usr/bin/env bash
# ITEM-511: contracted slice of the HFOC OS check suite. Sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# ITEM-511: the two-controller proof stops rolling its own console input and
# types through the shared echo-confirmed helper (ITEM-427), and a failing phase
# keeps its transcript under build/test-artifacts instead of deleting it with
# the temporary directory.  The proof's assertions are unchanged; the slice pins
# both the transport and the preserved evidence.
for item511_file in \
    scripts/test-qemu-usb-two-controllers.sh \
    scripts/hfoc_console_command.py \
    docs/ci-self-hosted-runner.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item511_file ]] || {
        echo "ITEM-511: отсутствует $item511_file" >&2
        exit 1
    }
done

# 1. The proof types through the shared helper: the line is echo-confirmed and
#    retyped when it was never echoed, Enter is repeated while the console has
#    not answered, and a lost budget writes the canonical timeout record.
grep -Fq 'sys.path.insert(0, "scripts")' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: проба не подключает общий помощник" >&2
    exit 1
}
grep -Fq 'import hfoc_console_command as console_command' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: проба не импортирует помощник" >&2
    exit 1
}
grep -Fq 'console_command.type_line(send_key, serial_bytes, value, deadline,' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: строка не подтверждается эхом" >&2
    exit 1
}
grep -Fq 'witness=str.lower' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: верхний регистр эха не принят во свидетеля" >&2
    exit 1
}
grep -Fq 'console_command.submit(' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: Enter не повторяется до ответа" >&2
    exit 1
}
grep -Fq 'except console_command.LostKeystrokes as lost:' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: потерянный бюджет не назван" >&2
    exit 1
}
grep -Fq 'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=200s' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: каноническая запись таймаута потеряна" >&2
    exit 1
}
for item511_command in \
    'type_command("menu", menu_probe, "the menu to open")' \
    'type_command("kbd", kbd_probe, "the keyboard'"'"'s own counters after the click")'; do
    grep -Fq -- "$item511_command" scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-511: команда не идёт через помощник: $item511_command" >&2
        exit 1
    }
done
if grep -Fq 'def type_text(' scripts/test-qemu-usb-two-controllers.sh ||
    grep -Fq 'type_text("' scripts/test-qemu-usb-two-controllers.sh; then
    echo "ITEM-511: сырой type_text остался в пробе" >&2
    exit 1
fi

# 2. A failing phase keeps its transcript: every log is copied to the directory
#    the CI diagnostics step collects, and its tail is printed into the phase
#    log, so the next failure is read instead of re-derived.
grep -Fq 'save_failure_artifacts()' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: нет сохранения журналов на отказе" >&2
    exit 1
}
grep -Fq 'artifact_dir="build/test-artifacts/usb-two-controllers-$timestamp"' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: журналы не попадают в артефакты CI" >&2
    exit 1
}
grep -Fq 'cp -a "$log" "$artifact_dir/"' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: журнал не копируется" >&2
    exit 1
}
grep -Fq 'tail -n 30 "$log" >&2' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: хвост журнала не печатается" >&2
    exit 1
}
grep -Fq 'if (( status != 0 )); then' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: сохранение не привязано к отказу" >&2
    exit 1
}
grep -Fq 'trap cleanup EXIT' scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-511: trap не ведёт к сохранению" >&2
    exit 1
}

# 3. The assertions the proof already carried are still spelled out: the menu
#    opens, the click runs Status, the menu closes and the keyboard answers.
for item511_assertion in \
    'wait_for(lambda text: "[LOCAL-MENU] open" in text, "the menu to open")' \
    '"[LOCAL-MENU] run action=status"' \
    '"[LOCAL-MENU] closed"' \
    '"LOCAL INPUT: SOURCE=USB_HID USB=READY" in text' \
    'KEYS=[1-9][0-9]*'; do
    grep -Fq -- "$item511_assertion" scripts/test-qemu-usb-two-controllers.sh || {
        echo "ITEM-511: утверждение потеряно: $item511_assertion" >&2
        exit 1
    }
done

# 4. The rule and the item are recorded where the next window reads them.
grep -Fq 'ITEM-511' docs/ci-self-hosted-runner.md || {
    echo "ITEM-511: правило не записано в документ раннера" >&2
    exit 1
}
grep -Fq 'ITEM-511' docs/ROADMAP.md || {
    echo "ITEM-511: ROADMAP не называет пункт" >&2
    exit 1
}
# ITEM-509 increment 27 moved the queue's `## Current item` heading on to its own
# item, so this pin names the ITEM-511 section the queue keeps instead of the
# heading, which belongs to whichever item is running.
grep -Fq 'ITEM-511' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-511: очередь не называет ITEM-511" >&2
    exit 1
}
