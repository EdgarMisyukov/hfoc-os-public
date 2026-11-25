#!/usr/bin/env bash
# ITEM-518: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item518_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item518_root" || exit 1

# ITEM-518: the installer refusal page and the console print what the storage
# probe observed, and `bootlog` returns.  The pins below are the contract:
#   * the one bounded model (`appliance_storage_report.{h,c}`) and its reason
#     tokens;
#   * the probe and the install flow fill it, the boot publishes it to the
#     console, the refusal page prints it and the `storage` command renders it
#     without a state volume;
#   * the boot-log dump captures its end once, refuses to observe its own
#     output, bounds the window, reports progress and says when it is done;
#   * the host proofs and the QEMU refusal proof exist and are registered.

# 1. The model and the page exist where the contract says they do.
for item518_file in \
    kernel/include/appliance_storage_report.h \
    kernel/src/appliance_storage_report.c \
    tests/installer-storage-host.c \
    tests/console-host.c \
    tests/appliance-boot-log-store-host.c \
    scripts/test-qemu-installer-disk-screen.sh \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-installer-storage.md \
    docs/hfoc-appliance-boot-console.md; do
    [[ -f $item518_file ]] || {
        echo "ITEM-518: отсутствует $item518_file" >&2
        exit 1
    }
done

# 2. The model names every finding the page has to distinguish, and the two
#    table-level annotations stay usable instead of rejecting the disk.
for item518_reason in \
    APPLIANCE_STORAGE_REPORT_OK \
    APPLIANCE_STORAGE_REPORT_NO_CONTROLLER \
    APPLIANCE_STORAGE_REPORT_BRINGUP_FAILED \
    APPLIANCE_STORAGE_REPORT_EMPTY_IDENTITY \
    APPLIANCE_STORAGE_REPORT_ZERO_GEOMETRY \
    APPLIANCE_STORAGE_REPORT_UNSUPPORTED_BLOCK_SIZE \
    APPLIANCE_STORAGE_REPORT_INVALID_DEVICE \
    APPLIANCE_STORAGE_REPORT_IO_ERROR \
    APPLIANCE_STORAGE_REPORT_INVALID_PARTITION_TABLE \
    APPLIANCE_STORAGE_REPORT_NOT_PROBED; do
    grep -Fq "$item518_reason" kernel/include/appliance_storage_report.h || {
        echo "ITEM-518: модель не называет причину: $item518_reason" >&2
        exit 1
    }
done
for item518_api in \
    'void appliance_storage_report_init(' \
    'enum appliance_storage_report_reason' \
    'bool appliance_storage_report_reason_usable(' \
    'size_t appliance_storage_report_lines(' \
    'size_t appliance_storage_report_record('; do
    grep -Fq "$item518_api" kernel/include/appliance_storage_report.h || {
        echo "ITEM-518: модель не объявляет: $item518_api" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_STORAGE_REPORT_LINES_MAX 24U' \
    kernel/include/appliance_storage_report.h || {
    echo "ITEM-518: у страницы нет бюджетного числа строк" >&2
    exit 1
}
grep -Fq 'reason == APPLIANCE_STORAGE_REPORT_IO_ERROR ||' \
    kernel/src/appliance_storage_report.c || {
    echo "ITEM-518: нечитаемая таблица считается отказом, а не пометкой" >&2
    exit 1
}
grep -Fq 'STORAGE TRANSLATION MISSING' kernel/src/appliance_storage_report.c || {
    echo "ITEM-518: модель не называет отсутствующий перевод" >&2
    exit 1
}

# 3. The labels come from the one catalog; the values stay machine text.
for item518_key in \
    '"storage.report.title"' \
    '"storage.report.controllers"' \
    '"storage.report.disks"' \
    '"storage.report.disk"' \
    '"storage.report.not_probed"'; do
    grep -Fq "$item518_key" kernel/src/appliance_language.c || {
        echo "ITEM-518: каталог не отвечает метку: $item518_key" >&2
        exit 1
    }
done

# 4. The probe fills the model, the flow folds the discovery verdict in, the
#    boot arms the console and the refusal page prints the same model.
grep -Fq 'void appliance_production_storage_report(struct appliance_storage_report *out)' \
    kernel/src/appliance_production_storage.c || {
    echo "ITEM-518: проб не заполняет наблюдённое" >&2
    exit 1
}
grep -Fq 'appliance_production_storage_report(&production_install_flow.storage_report);' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-518: поток установки не забирает наблюдённое у проба" >&2
    exit 1
}
grep -Fq 'production_install_flow_observed_reason(' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-518: вердикт discovery не сворачивается в строки" >&2
    exit 1
}
grep -Fq 'console_publish_storage_report(&production_install_flow.storage_report);' \
    kernel/src/production_install_flow.c || {
    echo "ITEM-518: поток установки не вооружает консольную команду" >&2
    exit 1
}
grep -Fq 'console_publish_storage_report(&storage_report);' kernel/src/kernel.c || {
    echo "ITEM-518: загрузка не публикует наблюдённое консоли" >&2
    exit 1
}
grep -Fq 'appliance_storage_report_lines(observed, language,' \
    kernel/src/installer_storage.c || {
    echo "ITEM-518: страница отказа не печатает наблюдённое" >&2
    exit 1
}
grep -Fq 'production_install_flow_storage_report(&observed);' kernel/src/kernel.c || {
    echo "ITEM-518: страница отказа не получает наблюдённое" >&2
    exit 1
}

# 5. The console command exists and works without a state volume.
grep -Fq 'console_command_is(input_line, "storage")' kernel/src/console.c || {
    echo "ITEM-518: консоль не знает команду storage" >&2
    exit 1
}
grep -Fq 'static void cmd_storage(void) {' kernel/src/console.c || {
    echo "ITEM-518: команда storage не написана" >&2
    exit 1
}
grep -Fq 'console_storage_report_published' kernel/src/console.c || {
    echo "ITEM-518: команда storage не различает публикацию и её отсутствие" >&2
    exit 1
}
grep -Fq 'void console_publish_storage_report(const struct appliance_storage_report *report)' \
    kernel/src/console.c || {
    echo "ITEM-518: публикация отчёта консоли не объявлена" >&2
    exit 1
}

# 6. `bootlog` is finite: the end is captured once, the dump is not observed,
#    the window is the staging ring, and the reading says when it is done.  The
#    old unbounded loop must be gone - it is the defect this item fixes.
grep -Fq 'if (store.dumping) {' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: dump наблюдает собственный вывод" >&2
    exit 1
}
grep -Fq 'store.dumping = true;' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: dump не помечает себя читающим" >&2
    exit 1
}
grep -Fq 'end = store.ring.next_sequence;' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: конец чтения не фиксируется" >&2
    exit 1
}
grep -Fq 'for (sequence = first; sequence < end; ++sequence) {' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: цикл dump не ограничен зафиксированным концом" >&2
    exit 1
}
if grep -Fq 'sequence < store.ring.next_sequence' \
    kernel/src/appliance_boot_log_store.c; then
    echo "ITEM-518: старый неограниченный цикл dump вернулся" >&2
    exit 1
fi
grep -Fq '#define APPLIANCE_BOOT_LOG_DUMP_CHUNK 16U' \
    kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: у dump нет порционного шага" >&2
    exit 1
}
grep -Fq 'BOOTLOG: progress=' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: dump не печатает прогресс" >&2
    exit 1
}
grep -Fq '"BOOTLOG: shown="' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: dump не называет, сколько показал" >&2
    exit 1
}
grep -Fq 'window=' kernel/src/appliance_boot_log_store.c || {
    echo "ITEM-518: dump не называет окно" >&2
    exit 1
}

# 7. The proofs exist: the report's forms, the command without a state volume,
#    the finite dump, the QEMU refusal with the observed section.
grep -Fq 'test_the_observed_report_names_an_empty_machine' \
    tests/installer-storage-host.c || {
    echo "ITEM-518: нет host-пробы пустой машины" >&2
    exit 1
}
grep -Fq 'test_the_observed_report_names_each_refusal_form' \
    tests/installer-storage-host.c || {
    echo "ITEM-518: нет host-пробы форм отказа" >&2
    exit 1
}
grep -Fq 'test_storage_command' tests/console-host.c || {
    echo "ITEM-518: нет host-пробы команды storage" >&2
    exit 1
}
grep -Fq 'test_the_console_dump_is_bounded_and_does_not_feed_itself' \
    tests/appliance-boot-log-store-host.c || {
    echo "ITEM-518: нет host-пробы конечного dump" >&2
    exit 1
}
grep -Fq 'kernel/src/appliance_storage_report.c' Makefile || {
    echo "ITEM-518: host-пробы не линкуют модель" >&2
    exit 1
}
grep -Fq '"Observed storage",' scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-518: QEMU-проба отказа не требует наблюдённого" >&2
    exit 1
}
grep -Fq 'observed_reasons = ("reason=NO_CONTROLLER", "reason=BRINGUP_FAILED")' \
    scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-518: QEMU-проба не требует именованной причины" >&2
    exit 1
}
grep -Fq '"[STORAGE-REPORT] probed=1",' \
    scripts/test-qemu-installer-disk-screen.sh || {
    echo "ITEM-518: QEMU-проба не требует записи наблюдённого" >&2
    exit 1
}
# The storage-boot proof asserts the finite reading: the dump must say it is
# done, and no progress line may leave the window it names.
grep -Fq 'BOOTLOG: shown=\d+ of=\d+ done' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-518: QEMU-проба не требует завершения bootlog" >&2
    exit 1
}
grep -Fq 'the boot log dump ran past its window' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-518: QEMU-проба не ловит уход dump за окно" >&2
    exit 1
}
grep -Fq 'the boot log dump never said it was done' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-518: QEMU-проба не требует строки завершения" >&2
    exit 1
}

# 8. The item is recorded where the next window reads it.  ITEM-519 moved the
#    queue's `## Current item` heading on to its own item, so this pin reads
#    ITEM-518's own historical section; the heading shape itself is held by
#    scripts/check.d/52-queue-current-item.sh.
grep -A 3 '^## Item ITEM-518$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-518' || {
    echo "ITEM-518: раздел очереди не называет ITEM-518" >&2
    exit 1
}
for item518_doc in \
    docs/ROADMAP.md \
    docs/ARCHITECTURE.md \
    docs/hfoc-installer-storage.md \
    docs/hfoc-appliance-boot-console.md; do
    grep -Fq 'ITEM-518' "$item518_doc" || {
        echo "ITEM-518: $item518_doc не называет пункт" >&2
        exit 1
    }
done
