#!/usr/bin/env bash
# ITEM-521 (increment 6): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521r_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521r_root" || exit 1

# ITEM-521 increment 6: a medium that cannot carry the journal is not a failed
# run.  The owner's requirement 3 names the case - booting from a CD, an image
# mounted read-only, a partition that is not reachable - and asks for the run
# to continue with a record about it in the ordinary records, while the probes
# that boot the ISO as a CD stay green.
#
# The placement itself (ITEM-520: a FAT12 file, a raw extent or a preallocated
# file) is still the owner's open fork and this increment chooses nothing: it
# only refuses the one medium that none of the three answers could ever write.
# The loader already names the medium it loaded the kernel from
# (`install_medium_boot_identity()`), an optical boot is read-only by nature,
# and the refusal happens before any placement is tried - so no device is
# touched and no byte is written:
#   * the core gains `install_journal_refuse()`: a named refusal for the
#     pre-placement decision, which never stops a journal that is already
#     bound and names itself once on the notify side;
#   * the platform exposes it, and the kernel calls it for an optical boot
#     after the durable log is bound, so the record lands in the serial stream
#     and in the durable log rather than being left for the medium to discover;
#   * the host witness proves the named refusal, the counted later records and
#     the untouched placement;
#   * the QEMU proof that boots the ISO as a CD requires exactly one such
#     record and keeps every other assertion it had.

# 1. The files the increment touches exist.
for item521r_file in \
    kernel/include/install_journal_core.h \
    kernel/src/install_journal_core.c \
    kernel/include/install_journal_platform.h \
    kernel/src/install_journal_platform.c \
    kernel/include/install_medium.h \
    kernel/src/kernel.c \
    tests/install-journal-core-host.c \
    scripts/test-qemu-usb-two-controllers.sh \
    docs/ROADMAP.md; do
    [[ -f $item521r_file ]] || {
        echo "ITEM-521: отсутствует $item521r_file" >&2
        exit 1
    }
done

# 2. The core gains the pre-placement refusal and it is exactly that: it only
#    acts while nothing is bound, and it names the reason through the same
#    one-time notify path a backend refusal uses.
grep -Fq 'bool install_journal_refuse(struct install_journal *journal,' \
    kernel/include/install_journal_core.h || {
    echo "ITEM-521: ядро журнала не объявляет отказ размещения" >&2
    exit 1
}
grep -Fq 'bool install_journal_refuse(struct install_journal *journal, const char *reason)' \
    kernel/src/install_journal_core.c || {
    echo "ITEM-521: отказ размещения не определён" >&2
    exit 1
}
item521r_refuse=$(awk '
    /^bool install_journal_refuse\(struct install_journal \*journal, const char \*reason\)$/ {
        inside = 1
        next
    }
    inside && /^\}/ { exit }
    inside {
        line = $0
        if (comment) {
            if (line ~ /\*\//) {
                sub(/^.*\*\//, "", line)
                comment = 0
            } else {
                next
            }
        }
        while (line ~ /\/\*/) {
            if (line ~ /\*\//) {
                sub(/\/\*[^*]*\*\//, "", line)
            } else {
                sub(/\/\*.*$/, "", line)
                comment = 1
                break
            }
        }
        print line
    }
' kernel/src/install_journal_core.c)
[[ -n $item521r_refuse ]] || {
    echo "ITEM-521: тело отказа размещения не найдено" >&2
    exit 1
}
printf '%s\n' "$item521r_refuse" |
    grep -Fq 'if (journal->state != INSTALL_JOURNAL_UNBOUND) {' || {
    echo "ITEM-521: отказ размещения не ограничен решением до привязки" >&2
    exit 1
}
printf '%s\n' "$item521r_refuse" |
    grep -Fq 'go_unavailable(journal, reason);' || {
    echo "ITEM-521: отказ размещения не называет причину" >&2
    exit 1
}

# 3. The platform hands the refusal to the core and never touches a device.
grep -Fq 'bool install_journal_platform_refuse(const char *reason);' \
    kernel/include/install_journal_platform.h || {
    echo "ITEM-521: заголовок платформы не объявляет отказ размещения" >&2
    exit 1
}
grep -Fq 'bool install_journal_platform_refuse(const char *reason) {' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-521: платформа не определяет отказ размещения" >&2
    exit 1
}
grep -Fq 'return install_journal_refuse(&platform_journal, reason);' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-521: платформа не передаёт отказ ядру журнала" >&2
    exit 1
}

# 4. The kernel refuses an optical boot, and it does so after the durable log
#    is bound: requirement 3 asks for the record in the ordinary records, and
#    an earlier call would reach the serial stream only.
grep -Fq 'media_type == INSTALL_MEDIUM_MEDIA_OPTICAL' kernel/src/kernel.c || {
    echo "ITEM-521: ядро не распознаёт оптическую загрузку" >&2
    exit 1
}
grep -Fq 'install_journal_platform_refuse("READ_ONLY")' kernel/src/kernel.c || {
    echo "ITEM-521: ядро не отказывает журналу на оптическом носителе" >&2
    exit 1
}
item521r_store=$(grep -n 'appliance_boot_log_store_init(&sink)' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
item521r_readonly=$(grep -n 'install_journal_platform_refuse("READ_ONLY")' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
[[ -n $item521r_store && -n $item521r_readonly ]] || {
    echo "ITEM-521: не найдены привязка долговременного журнала или отказ" >&2
    exit 1
}
((item521r_store < item521r_readonly)) || {
    echo "ITEM-521: отказ идёт раньше, чем долговременный журнал готов" >&2
    exit 1
}

# 5. The host witness drives the refusal: named once, later records counted,
#    the placement untouched, and a bound journal left alone.
for item521r_proof_pin in \
    'test_placement_refusal_before_write_is_named' \
    'a placement that cannot carry the journal binds nothing' \
    '"the read-only medium is named once on the notify side"' \
    '"no slot reaches a placement that refused"' \
    '"a running journal is not stopped by a late refusal"'; do
    grep -Fq "$item521r_proof_pin" tests/install-journal-core-host.c || {
        echo "ITEM-521: host-проба не проверяет: $item521r_proof_pin" >&2
        exit 1
    }
done
grep -Fq '    test_placement_refusal_before_write_is_named();' \
    tests/install-journal-core-host.c || {
    echo "ITEM-521: host-проба отказа не зарегистрирована" >&2
    exit 1
}

# 6. The QEMU proof that boots the ISO as a CD requires exactly one such
#    record; without it the behaviour would only be pinned on the host.
grep -Fq 'the optical boot did not name the unwritable journal exactly once' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-521: CD-проба не требует записи об оптическом носителе" >&2
    exit 1
}
grep -Fq '"[INSTALL-LOG] state=UNAVAILABLE reason=READ_ONLY"]' \
    scripts/test-qemu-usb-two-controllers.sh || {
    echo "ITEM-521: CD-проба не пинит саму строку отказа" >&2
    exit 1
}

# 7. The queue records this item (ITEM-526 moved the `## Current item` heading on
#    to its own item, so this pin reads ITEM-521's own historical section), and
#    the roadmap records the increment.
grep -A 3 '^## Item ITEM-521$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-521' || {
    echo "ITEM-521: раздел очереди не называет ITEM-521" >&2
    exit 1
}
grep -Fq 'инкремент 6' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-521: очередь не называет текущий инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-521' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
