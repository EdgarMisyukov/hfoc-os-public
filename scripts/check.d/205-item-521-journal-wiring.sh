#!/usr/bin/env bash
# ITEM-521 (increment 5): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521w_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521w_root" || exit 1

# ITEM-521 increment 5: the wiring of the placement-independent journal to the
# kernel's one funnel.  The owner's requirement is that the install run and
# every refusal end up on the medium the machine booted from; the placement
# itself is the answer to the ITEM-520 fork - (c) the \HFOC\INSTALL.LOG the
# medium's build reserves in the FAT12 partition - so this slice holds the half
# that is the same for every answer and pins the one answer that was chosen:
#   * the journal subscribes to the store's *admitted* records - past the dump
#     guard, the repeated-record dedup and the periodic gate - not to the
#     console funnel's repaints, so a bounded journal holds the machine
#     announcement and the observed storage instead of heartbeats (F-A of
#     item-521-wiring-audit-2026-10-12.md);
#   * the status of the journal goes to the serial stream and to the durable
#     store directly, never back through the funnel that feeds the journal
#     (F7 of item-521-wiring-plan.md);
#   * the placement seam `install_journal_platform_bind_medium()` is filled
#     with that answer: it binds the FAT12 writer on the proven medium or
#     refuses by name, and it names none of the other answers;
#   * the flush hooks requirement 2 asks for are in place: before the refusal
#     page is painted and on the panic path, after the durable log's own flush;
#   * the bind attempt hangs off the boot-medium identity guard's MATCH - the
#     only verdict that proves the device is the medium the machine booted
#     from - and a refusal there never fails the install (requirement 3).

# 1. The module, its header, its contact points and its witness exist.
for item521w_file in \
    kernel/include/install_journal_platform.h \
    kernel/src/install_journal_platform.c \
    kernel/include/install_journal_core.h \
    kernel/include/appliance_boot_log_store.h \
    kernel/src/appliance_boot_log_store.c \
    kernel/src/kernel.c \
    kernel/src/panic.c \
    kernel/src/xhci.c \
    tests/appliance-boot-log-store-host.c \
    docs/ROADMAP.md; do
    [[ -f $item521w_file ]] || {
        echo "ITEM-521: отсутствует $item521w_file" >&2
        exit 1
    }
done

# 2. The placement seam is the one point ITEM-520 fills in, and it is filled
#    with the owner's answer (c): the reserved FAT12 file.  Increment 7 hands
#    it the proven medium (block view and verdict, slice 207); with the
#    comments stripped, its body must bind the FAT12 writer, refuse by its own
#    name and hand the bound journal to the core - and it must no longer claim
#    the fork is open.
grep -Fq 'bool install_journal_platform_bind_medium(' \
    kernel/include/install_journal_platform.h || {
    echo "ITEM-521: заголовок не объявляет шов размещения" >&2
    exit 1
}
if grep -Fq 'ITEM-520 fork: the placement is not chosen yet' \
    kernel/src/install_journal_platform.c; then
    echo "ITEM-521: шов размещения всё ещё называет развилку открытой" >&2
    exit 1
fi
grep -Fq '#include "install_fat12_journal.h"' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-521: шов размещения не называет выбранное размещение" >&2
    exit 1
}
item521w_seam=$(awk '
    /^bool install_journal_platform_bind_medium\(/ {
        signature = 1
        if ($0 ~ /\) \{/) {
            inside = 1
        }
        next
    }
    signature && !inside {
        if ($0 ~ /\) \{/) {
            inside = 1
        }
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
' kernel/src/install_journal_platform.c)
[[ -n $item521w_seam ]] || {
    echo "ITEM-521: тело шва размещения не найдено" >&2
    exit 1
}
item521w_seam_code=$(printf '%s\n' "$item521w_seam" | grep -v '^[[:space:]]*$')
[[ $item521w_seam_code == *'placement_fat12_try('* ]] || {
    echo "ITEM-521: шов размещения не связывает выбранное размещение: $item521w_seam_code" >&2
    exit 1
}
[[ $item521w_seam_code == *'install_journal_platform_refuse('* ]] || {
    echo "ITEM-521: шов размещения не отказывает своей причиной: $item521w_seam_code" >&2
    exit 1
}
[[ $item521w_seam_code == *'return true;'* ]] || {
    echo "ITEM-521: шов размещения не сообщает об успешной привязке: $item521w_seam_code" >&2
    exit 1
}
grep -Fq 'fat12_journal_bind(' kernel/src/install_journal_platform.c || {
    echo "ITEM-521: платформа журнала не связывает писатель FAT12" >&2
    exit 1
}
grep -Fq 'install_journal_bind(&platform_journal, &platform_fat12_backend)' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-521: платформа журнала не отдаёт связанный журнал ядру" >&2
    exit 1
}

# 3. The platform owns exactly one placement and no direct device write: with
#    the comments stripped it never names one of the other ITEM-520 answers and
#    never writes a block device itself, and the status text lives in the core
#    rather than in the platform.  The comments are allowed to *name* the other
#    answers; only the code may not choose one of them.
item521w_platform_code=$(awk '
    {
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
' kernel/src/install_journal_platform.c)
[[ -n $item521w_platform_code ]] || {
    echo "ITEM-521: код платформы журнала не прочитан" >&2
    exit 1
}
for item521w_device in \
    block_device_write \
    extent_journal \
    xhci_storage \
    install_medium_identify; do
    if printf '%s\n' "$item521w_platform_code" | grep -Fq "$item521w_device"; then
        echo "ITEM-521: платформа журнала называет невыбранное размещение или чужое устройство: $item521w_device" >&2
        exit 1
    fi
done
if printf '%s\n' "$item521w_platform_code" | grep -Fq '[INSTALL-LOG]'; then
    echo "ITEM-521: статус журнала печатается платформой, а не ядром журнала" >&2
    exit 1
fi

# 4. The status path never re-enters the funnel: the notify side reaches the
#    serial stream and the durable store directly (F7).
printf '%s\n' "$item521w_platform_code" | grep -Fq 'serial_write(text);' || {
    echo "ITEM-521: статус журнала не уходит в serial" >&2
    exit 1
}
printf '%s\n' "$item521w_platform_code" |
    grep -Fq 'appliance_boot_log_store_observe(text, strlen(text));' || {
    echo "ITEM-521: статус журнала не уходит в долговременный журнал" >&2
    exit 1
}
for item521w_funnel in \
    console_publish_journal \
    console_journal_record \
    production_install_flow_publish; do
    if printf '%s\n' "$item521w_platform_code" | grep -Fq "$item521w_funnel"; then
        echo "ITEM-521: статус журнала вернулся в воронку: $item521w_funnel" >&2
        exit 1
    fi
done

# 5. The store hands its admitted records to the second consumer, and it does
#    so after the dump guard, the dedup and the periodic gate - that order is
#    what makes one admission policy for the durable log and the journal.
grep -Fq 'void (*admitted)(const char *record, size_t length);' \
    kernel/include/appliance_boot_log_store.h || {
    echo "ITEM-521: у стока нет второго потребителя" >&2
    exit 1
}
[[ $(grep -c 'store.sink.admitted != NULL' \
    kernel/src/appliance_boot_log_store.c) -eq 2 ]] || {
    echo "ITEM-521: второй потребитель зовётся не на обеих ветвях записи" >&2
    exit 1
}
item521w_gate=$(grep -n 'if (!appliance_boot_log_store_gate_admit(' \
    kernel/src/appliance_boot_log_store.c | head -n 1 | cut -d: -f1)
item521w_dedup=$(grep -n 'memcmp(store.dedup, record, length) == 0' \
    kernel/src/appliance_boot_log_store.c | head -n 1 | cut -d: -f1)
item521w_admitted=$(grep -n 'store.sink.admitted != NULL' \
    kernel/src/appliance_boot_log_store.c | head -n 1 | cut -d: -f1)
[[ -n $item521w_gate && -n $item521w_dedup && -n $item521w_admitted ]] || {
    echo "ITEM-521: не найдены дедупликация, гейт допуска или второй потребитель" >&2
    exit 1
}
((item521w_dedup < item521w_admitted && item521w_gate < item521w_admitted)) || {
    echo "ITEM-521: второй потребитель обходит политику допуска стока" >&2
    exit 1
}

# 6. The wiring points exist in the kernel and in the order requirement 2
#    needs: the journal is initialised before the store can admit a record, the
#    flush precedes the refusal page, and the panic flush follows the durable
#    log's own.
grep -Fq 'install_journal_platform_observe' kernel/src/kernel.c || {
    echo "ITEM-521: kernel.c не подписывает журнал на допущенные записи" >&2
    exit 1
}
grep -Fq 'install_journal_platform_init();' kernel/src/kernel.c || {
    echo "ITEM-521: kernel.c не инициализирует журнал" >&2
    exit 1
}
grep -Fq '(void)install_journal_platform_flush();' kernel/src/kernel.c || {
    echo "ITEM-521: kernel.c не сбрасывает журнал перед страницей отказа" >&2
    exit 1
}
item521w_init=$(grep -n 'install_journal_platform_init();' kernel/src/kernel.c |
    head -n 1 | cut -d: -f1)
item521w_store=$(grep -n 'appliance_boot_log_store_init(&sink)' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
item521w_flush=$(grep -n '(void)install_journal_platform_flush();' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
item521w_refusal=$(grep -n 'installer_storage_refusal_show(' \
    kernel/src/kernel.c | head -n 1 | cut -d: -f1)
[[ -n $item521w_init && -n $item521w_store && -n $item521w_flush &&
    -n $item521w_refusal ]] || {
    echo "ITEM-521: не найдены точки инициализации, привязки или отказа" >&2
    exit 1
}
((item521w_init < item521w_store)) || {
    echo "ITEM-521: журнал инициализируется после первого допуска стока" >&2
    exit 1
}
((item521w_flush < item521w_refusal)) || {
    echo "ITEM-521: журнал сбрасывается после отрисовки страницы отказа" >&2
    exit 1
}
item521w_log_panic=$(grep -n 'appliance_boot_log_store_panic_flush();' \
    kernel/src/panic.c | head -n 1 | cut -d: -f1)
item521w_journal_panic=$(grep -n 'install_journal_platform_panic_flush();' \
    kernel/src/panic.c | head -n 1 | cut -d: -f1)
[[ -n $item521w_log_panic && -n $item521w_journal_panic ]] || {
    echo "ITEM-521: панический сброс журнала не подключён" >&2
    exit 1
}
((item521w_log_panic < item521w_journal_panic)) || {
    echo "ITEM-521: панический сброс журнала идёт раньше долговременного" >&2
    exit 1
}

# 7. The bind attempt hangs off the medium identity guard's MATCH and nowhere
#    else: a device that is merely attached is never a placement, and the seam
#    is handed the proven medium's block view (slice 207 pins the verdict too).
#    ITEM-521 increment 8: the attempt is an `if`, and its false answer is the
#    named unbound loss of slice 208 - the seam itself still names nothing.
grep -Fq 'if (!install_journal_platform_bind_medium(&storage->block.device,' \
    kernel/src/xhci.c || {
    echo "ITEM-521: xhci.c не пробует привязать носитель" >&2
    exit 1
}
item521w_match=$(grep -n 'if (verdict.result == INSTALL_MEDIUM_MATCH) {' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
item521w_bind=$(grep -n 'if (!install_journal_platform_bind_medium(&storage->block.device,' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
item521w_name=$(grep -n '(void)install_journal_platform_medium_unbound();' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
item521w_return=$(grep -n 'return verdict.result == INSTALL_MEDIUM_MATCH;' \
    kernel/src/xhci.c | head -n 1 | cut -d: -f1)
[[ -n $item521w_match && -n $item521w_bind && -n $item521w_name &&
   -n $item521w_return ]] || {
    echo "ITEM-521: не найдены вердикт носителя, точка привязки или имя потери" >&2
    exit 1
}
((item521w_match < item521w_bind && item521w_bind < item521w_name &&
  item521w_name < item521w_return)) || {
    echo "ITEM-521: привязка не привязана к вердикту MATCH" >&2
    exit 1
}

# 8. The host witness drives the second consumer's admission policy and is
#    registered in the store suite's main.
for item521w_proof_pin in \
    'test_the_second_consumer_sees_only_admitted_records' \
    'ITEM-521: an unchanged repeat is not handed to the journal' \
    'ITEM-521: the same periodic record inside its interval is held ' \
    'ITEM-521: the second consumer receives the record text itself'; do
    grep -Fq "$item521w_proof_pin" tests/appliance-boot-log-store-host.c || {
        echo "ITEM-521: host-проба не проверяет: $item521w_proof_pin" >&2
        exit 1
    }
done
grep -Fq '    test_the_second_consumer_sees_only_admitted_records();' \
    tests/appliance-boot-log-store-host.c || {
    echo "ITEM-521: host-проба второго потребителя не зарегистрирована" >&2
    exit 1
}

# 9. The queue records this item (ITEM-526 moved the `## Current item` heading on
#    to its own item, so this pin reads ITEM-521's own historical section), the
#    roadmap records the increment, and the wiring chooses no placement in prose
#    either.
grep -A 3 '^## Item ITEM-521$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-521' || {
    echo "ITEM-521: раздел очереди не называет ITEM-521" >&2
    exit 1
}
grep -Fq 'инкремент 5' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-521: очередь не называет текущий инкремент" >&2
    exit 1
}
grep -Fq 'ITEM-521' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
