#!/usr/bin/env bash
# ITEM-497: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The boot's input verdict, and the three boots one ring has to keep.
#
# The owner's 2026-09-30 `hw20` run came up with no keyboard and no pointer and
# came up working the next time; the durable ring holds 511 records, a boot
# publishes thousands, and the boot that failed had been overwritten before the
# machine was switched off.  This slice pins the two rules that make the next
# such boot readable and the one property that must not be traded away for it:
#
#   * one `[INPUT-PATH]` line per boot (or per state change) naming the boot,
#     the verdict and both roles, written as a flush marker, so the boot that
#     just failed is on the disk it came up on;
#   * the verdict is carried: the attach reads the earlier boots' verdicts back
#     out of the extent's tail and the loop heartbeat re-states them as
#     `[INPUT-PATH-CARRY]`, so after three boots all three verdicts still read
#     back - the acceptance the item exists for;
#   * the periodic record families are admitted at most once per interval, so
#     the ring holds a history instead of the last few seconds of it - and the
#     gate names only per-second snapshots: a refusal, the input watchdog, the
#     mouse-silence snapshots and the iteration maximum are never held back
#     (ITEM-479/480/486 observability is not weakened).

for item497_file in \
    kernel/include/appliance_boot_log.h \
    kernel/src/appliance_boot_log.c \
    kernel/include/appliance_boot_log_store.h \
    kernel/src/appliance_boot_log_store.c \
    kernel/src/xhci.c \
    tests/appliance-boot-log-host.c \
    tests/appliance-boot-log-store-host.c \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md; do
    test -f "$item497_file" || {
        echo "ITEM-497: отсутствует файл: $item497_file" >&2
        exit 1
    }
done

# 1. The verdict is one bounded line naming the boot, the verdict and both
#    roles.  The four role words are the whole classification.
for item497_literal in \
    '"boot="' \
    '" verdict="' \
    '" keyboard="' \
    '" mouse="' \
    '" arm_ms="' \
    '" first_report_ms="' \
    '" ports="' \
    '" reports="'; do
    grep -Fq "$item497_literal" kernel/src/appliance_boot_log.c || {
        echo "ITEM-497: тело вердикта не называет $item497_literal" >&2
        exit 1
    }
done
for item497_role in '"ARMED"' '"NO_REPORTS"' '"NOT_ARMED"' '"ABSENT"' \
    '"NO_DEVICE"'; do
    grep -Fq "$item497_role" kernel/src/appliance_boot_log.c || {
        echo "ITEM-497: классификация не называет $item497_role" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_BOOT_LOG_INPUT_BODY_MAX' \
    kernel/include/appliance_boot_log.h || {
    echo "ITEM-497: у тела вердикта нет границы" >&2
    exit 1
}
# A body that does not fit is refused, not truncated: the formatter returns 0.
grep -Fq 'appliance_boot_log_format_input_body' \
    kernel/include/appliance_boot_log.h kernel/src/appliance_boot_log.c || {
    echo "ITEM-497: вердикт не собирается одной функцией" >&2
    exit 1
}

# 2. The boot's own verdict is a flush marker; a carried copy is not, because a
#    copy must ride the heartbeat's flush instead of paying one write each.
grep -Fq '"[INPUT-PATH] boot=",' kernel/src/appliance_boot_log.c || {
    echo "ITEM-497: вердикт не является моментом записи" >&2
    exit 1
}
if sed -n '/appliance_boot_log_flush_markers\[\] = {/,/^};/p' \
    kernel/src/appliance_boot_log.c | grep -Fq '"[INPUT-PATH-CARRY]'; then
    echo "ITEM-497: копия вердикта снова вызывает запись" >&2
    exit 1
fi
grep -Fq '"[INPUT-PATH-CARRY] "' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: копия вердикта не помечена" >&2
    exit 1
}

# 3. Carry: the attach reads the earlier boots back, the heartbeat restates
#    them, and the scan keeps one body per boot.
grep -Fq 'appliance_boot_log_scan_input_verdicts(' \
    kernel/include/appliance_boot_log.h kernel/src/appliance_boot_log.c || {
    echo "ITEM-497: вердикты прошлых загрузок не читаются" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_load_verdicts();' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: attach не читает хвост журнала" >&2
    exit 1
}
grep -Fq '[PRODUCTION-LOOP]' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: вердикт не повторяется с ударом цикла" >&2
    exit 1
}
grep -Fq 'appliance_boot_log_store_input_digest();' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: набор вердиктов не перепубликуется" >&2
    exit 1
}
grep -Fq 'APPLIANCE_BOOT_LOG_INPUT_CARRY_MAX' kernel/include/appliance_boot_log.h \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: у переносимых вердиктов нет границы" >&2
    exit 1
}
grep -Fq 'VERDICT_SCAN_SLOTS' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-497: у окна чтения вердиктов нет границы" >&2
    exit 1
}
# The carried set is restated on its own window, not only when the unrelated
# loop heartbeat happens to be published.
grep -Fq 'appliance_boot_log_store_input_verdict_tick();' kernel/src/xhci.c || {
    echo "ITEM-497: перенос вердиктов зависит от чужого такта" >&2
    exit 1
}
grep -Fq 'void appliance_boot_log_store_input_verdict_tick(void);' \
    kernel/include/appliance_boot_log_store.h || {
    echo "ITEM-497: у такта вердиктов нет объявления" >&2
    exit 1
}

# 4. The repeat gate: the periodic snapshots are bounded, and the records that
#    carry a state change stay outside it.
grep -Fq 'appliance_boot_log_gate_admit(' kernel/src/appliance_boot_log_store.c \
    kernel/src/appliance_boot_log.c || {
    echo "ITEM-497: повторяющиеся записи не ограничены" >&2
    exit 1
}
for item497_gated in '"[INPUT-DIAG]",' '"[LOOP-DIAG]",' '"[XHCI-DIAG]",' \
    '"[BOOTLOG] state=READY",'; do
    grep -Fq "$item497_gated" kernel/src/appliance_boot_log_store.c || {
        echo "ITEM-497: семейство $item497_gated не ограничено" >&2
        exit 1
    }
done
if grep -A 12 'appliance_boot_log_store_gate_markers\[' \
    kernel/src/appliance_boot_log_store.c |
    grep -Eq 'WATCHDOG|MOUSE-SILENCE|ITER-MAX'; then
    echo "ITEM-497: гейт трогает наблюдаемость watchdog/мыши" >&2
    exit 1
fi
for item497_kept in '"[INPUT-WATCHDOG]"' '"[MOUSE-SILENCE"' '"[ITER-MAX]"'; do
    grep -Fq "$item497_kept" kernel/src/appliance_boot_log.c || {
        echo "ITEM-497: момент записи $item497_kept потерян" >&2
        exit 1
    }
done

# 5. The producer: the input path publishes the verdict, and publishing it
#    touches no endpoint (it is measurement, not a recovery).
grep -Fq 'appliance_boot_log_store_input_state(' kernel/src/xhci.c || {
    echo "ITEM-497: путь ввода не публикует вердикт" >&2
    exit 1
}
grep -Fq 'xhci_input_verdict_publish(now);' kernel/src/xhci.c || {
    echo "ITEM-497: вердикт не публикуется раз в секунду" >&2
    exit 1
}
# The boot's own copy: a boot killed before the loop's next flush still leaves
# its verdict on the disk it came up on.
grep -Fq 'xhci_input_verdict_publish_now();' kernel/src/kernel.c || {
    echo "ITEM-497: вердикт не публикуется в самой загрузке" >&2
    exit 1
}
verdict_body=$(sed -n '/static void xhci_input_verdict_publish/,/^}/p' \
    kernel/src/xhci.c)
case $verdict_body in
*endpoint_recover* | *xhci_reset* | *Stop*)
    echo "ITEM-497: публикация вердикта трогает endpoint" >&2
    exit 1
    ;;
esac

# 6. The proofs: the text, the scan and the gate on the host, and the three
#    boots that must all still read back.
grep -Fq 'test_input_verdict_text' tests/appliance-boot-log-host.c || {
    echo "ITEM-497: хост-проба вердикта не зарегистрирована" >&2
    exit 1
}
grep -Fq 'test_input_verdict_scan_keeps_every_boot' \
    tests/appliance-boot-log-host.c || {
    echo "ITEM-497: хост-проба чтения вердиктов не зарегистрирована" >&2
    exit 1
}
grep -Fq 'test_input_verdict_gate' tests/appliance-boot-log-host.c || {
    echo "ITEM-497: хост-проба гейта не зарегистрирована" >&2
    exit 1
}
grep -Fq 'test_input_verdict_survives_three_boots' \
    tests/appliance-boot-log-store-host.c || {
    echo "ITEM-497: проба трёх загрузок не зарегистрирована" >&2
    exit 1
}
grep -Fq 'three boots leave three verdicts in one log' \
    tests/appliance-boot-log-store-host.c || {
    echo "ITEM-497: проба трёх загрузок не требует трёх вердиктов" >&2
    exit 1
}

# 7. The QEMU proof: a real boot leaves its verdict on the disk it came up on,
#    and three boots of the same installed disk leave three of them.
grep -Fq 'assert_input_verdict() {' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: QEMU-проба не требует вердикта установленной загрузки" >&2
    exit 1
}
grep -Fq 'the installed boot carries no input verdict' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: QEMU-проба не называет отсутствие вердикта" >&2
    exit 1
}
grep -Fq 'assert_three_boot_verdicts() {' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: нет пробы трёх загрузок в QEMU" >&2
    exit 1
}
grep -Fq "assert_three_boot_verdicts \"\$TEST_DIR/reinstall.raw\" reinstall" \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: проба трёх загрузок не вызывается" >&2
    exit 1
}
grep -Fq "boot_installed_disk \"\$TEST_DIR/reinstall.raw\" verdict-3" \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: третья загрузка QEMU не делается" >&2
    exit 1
}
grep -Fq "the log carries \$boots input verdicts, not three" \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-497: проба не требует трёх вердиктов" >&2
    exit 1
}

# 8. The documents say what the record is and how it is read back.
grep -Fq '[INPUT-PATH]' docs/hfoc-appliance-boot-console.md || {
    echo "ITEM-497: документ журнала не описывает вердикт ввода" >&2
    exit 1
}
grep -Fq 'ITEM-497' docs/ROADMAP.md || {
    echo "ITEM-497: дорожная карта не называет пункт" >&2
    exit 1
}
