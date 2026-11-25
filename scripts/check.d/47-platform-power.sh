#!/usr/bin/env bash
# ITEM-418: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Reboot and power off, from the machine itself.
#
# `platform_reset` (ITEM-411) answered "how does this machine reset itself" and
# the installer used it; nothing answered "how does it power off", and nothing
# in the product offered either action to an operator.  A power off has exactly
# one honest mechanism on a PC - ACPI S5 - and it takes two declarations from
# the firmware: the PM1 control block out of the FADT, and the `\_S5` sleep type
# out of the DSDT.  This slice pins that the module reads both, refuses rather
# than guesses when either is missing, reuses the reset chain for a reboot, and
# publishes what it is about to do before it does it - a machine that is still
# running after an S5 write says so instead of leaving the operator guessing.
for power_source in \
    kernel/include/platform_power.h \
    kernel/src/platform_power.c \
    tests/platform-power-host.c \
    scripts/test-qemu-platform-power.sh; do
    [[ -f "$power_source" ]] || {
        echo "ITEM-418: нет файла $power_source" >&2
        exit 1
    }
done
for power_vocabulary in \
    'PLATFORM_POWER_ACTION_REBOOT' \
    'PLATFORM_POWER_ACTION_SHUTDOWN' \
    'PLATFORM_POWER_MECHANISM_RESET_CHAIN' \
    'PLATFORM_POWER_MECHANISM_ACPI_S5' \
    'PLATFORM_POWER_SLP_EN'; do
    grep -Fq -- "$power_vocabulary" kernel/include/platform_power.h || {
        echo "модуль питания не описывает: $power_vocabulary" >&2
        exit 1
    }
done
# The firmware's own declarations, read where the memory map is.
if ! grep -Fq 'acpi_fadt_power' kernel/include/acpi.h ||
    ! grep -Fq 'ACPI_FADT_PM1A_CNT_OFFSET' kernel/include/acpi.h; then
    echo "ACPI не публикует PM1-блоки для выключения" >&2
    exit 1
fi
# The sleep type object, including the four-element package older firmware
# emits, and a refusal that names the ingredient that was missing rather than a
# bare verdict.
if ! grep -Fq 'PLATFORM_POWER_AML_MAX_ELEMENTS' kernel/src/platform_power.c ||
    ! grep -Fq 'dsdt_bytes=' kernel/src/platform_power.c ||
    ! grep -Fq 's5_literals=' kernel/src/platform_power.c; then
    echo "разбор \\_S5 или отказ не называет, чего не хватило" >&2
    exit 1
fi
# The machine reports its own attempts, and an ignored write is a finding.
grep -Fq '"STILL_RUNNING"' kernel/src/platform_power.c || {
    echo "модуль не публикует, что платформа проигнорировала выключение" >&2
    exit 1
}
# Reboot is the proven reset chain, not a second implementation of it.
grep -Fq 'platform_reset_execute(&reset)' kernel/src/platform_power.c || {
    echo "перезагрузка не переиспользует цепочку сброса" >&2
    exit 1
}
# An operator reaches both actions from the console - in whatever case the
# command is typed (ITEM-512) - and the request reaches the disk before the
# machine goes down.
if ! grep -Fq 'console_command_is(input_line, "reboot")' kernel/src/console.c ||
    ! grep -Fq 'console_command_is(input_line, "shutdown")' kernel/src/console.c; then
    echo "консоль не предлагает перезагрузку и выключение" >&2
    exit 1
fi
grep -Fq '"[PLATFORM-POWER]"' kernel/src/appliance_boot_log.c || {
    echo "запрос питания не выталкивается в журнал до ухода машины" >&2
    exit 1
}
# The host proof reads the sleep type out of AML, including the four-element
# package older firmware emits, and refuses what it does not understand.
for power_proof in \
    'the four-element \\_S5 package reads as its first two types' \
    'a DSDT without \\_S5 leaves the sleep type undeclared' \
    'a shutdown without ACPI is refused, not guessed' \
    'a reboot is a plan on a platform that declares no ACPI at all'; do
    grep -Fq -- "$power_proof" tests/platform-power-host.c || {
        echo "хостовое доказательство не проверяет: $power_proof" >&2
        exit 1
    }
done
# And the QEMU proof drives the console and requires the machine to leave.
if ! grep -Fq 'PLATFORM-POWER] plan action=SHUTDOWN mechanism=ACPI_S5' \
        scripts/test-qemu-platform-power.sh ||
    ! grep -Fq 'PLATFORM-RESET] attempt=1/' \
        scripts/test-qemu-platform-power.sh; then
    echo "QEMU-доказательство не требует записей питания" >&2
    exit 1
fi
if ! grep -Fq 'run_scenario shutdown' scripts/test-qemu-platform-power.sh ||
    ! grep -Fq 'run_scenario reboot' scripts/test-qemu-platform-power.sh; then
    echo "QEMU-доказательство проверяет не оба действия" >&2
    exit 1
fi
grep -Fq 'test-platform-power-qemu' Makefile || {
    echo "QEMU-доказательство питания не подключено к набору" >&2
    exit 1
}

# The console vocabulary this item extends is the vocabulary the console-driven
# proofs await.  `reboot` and `shutdown` were inserted into the COMMANDS line,
# and the three proofs kept waiting for the old text: the command answered in
# milliseconds, the marker could not match, and the phase read a healthy machine
# as a marker timeout (measured on 2026-09-16: 58-83 s per attempt and two
# spent retries).  The kernel's line and every `help_marker` move together, and
# this check refuses a marker that is no longer a substring of it - an appended
# command (BOOTLOG) stays compatible, an inserted or renamed one does not.  The
# same comparison is then run against the line with this item's own words
# removed, so the check proves it would have caught the regression it exists
# for instead of only asserting today's agreement.
python3 - <<'PY' || {
import pathlib
import re
import sys

console = pathlib.Path("kernel/src/console.c").read_text(encoding="utf-8")
match = re.search(r'console_write\("(COMMANDS: [^"]*?)\\n"\)', console)
if match is None:
    print("ITEM-418: console.c не публикует строку COMMANDS", file=sys.stderr)
    raise SystemExit(1)
canonical = match.group(1)
markers = []
failures = []
for proof in ("scripts/test-qemu-lapic-timer-oneshot.sh",
              "scripts/test-qemu-pic-disable.sh",
              "scripts/test-qemu-console-history.sh"):
    text = pathlib.Path(proof).read_text(encoding="utf-8")
    proof_markers = ["".join(re.findall(r'"([^"]*)"', block))
                     for block in re.findall(r"help_marker = \((.*?)\)\n", text,
                                             re.DOTALL)]
    if not proof_markers:
        failures.append("%s: нет help_marker" % proof)
    for marker in proof_markers:
        markers.append(marker)
        if not marker.startswith("COMMANDS: ") or marker not in canonical:
            failures.append("%s: маркер не совпадает с языком консоли" % proof)
if failures:
    print("ITEM-418: " + "; ".join(failures), file=sys.stderr)
    raise SystemExit(1)
without_item_words = canonical.replace("REBOOT SHUTDOWN ", "", 1)
if without_item_words == canonical:
    print("ITEM-418: строка COMMANDS не называет команды этого item",
          file=sys.stderr)
    raise SystemExit(1)
if not any(marker not in without_item_words for marker in markers):
    print("ITEM-418: проверка не заметила бы вставленную команду",
          file=sys.stderr)
    raise SystemExit(1)
print("platform power: console vocabulary matches every proof marker")
PY
    echo "ITEM-418: маркеры консольных доказательств разошлись с языком консоли" >&2
    exit 1
}
