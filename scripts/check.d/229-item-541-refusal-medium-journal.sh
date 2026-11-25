#!/usr/bin/env bash
# ITEM-541: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The L875 lesson: on a machine with no serial, the refusal page is the only
# channel, and it printed neither the medium verdict nor the journal state - an
# operator carried away an empty stick with no reason on it, and F-6 showed the
# observed-storage section could be missing entirely.  This slice pins the fix:
#   * the journal core keeps the first name that decided a non-READY state and
#     lends its bounded RAM ring tail read-only;
#   * the platform answers state/reason/counters and the ring records through
#     one seam, and xHCI answers the medium verdict with its reason - including
#     `UNAVAILABLE reason=NO_DEVICE` versus `UNCHECKED reason=NOT_PROBED`, which
#     used to read as the same `UNCHECKED`;
#   * the refusal page prints `[INSTALL-MEDIUM] state=... reason=...`,
#     `[INSTALL-LOG] state=... reason=... pending=... dropped=... refused=...`
#     and a bounded `[INSTALL-LOG-TAIL]`, says `state=NOT_PROBED` when the
#     observed report is empty instead of staying silent, keeps the way out on
#     the page and stays inside the line budget;
#   * the host proof drives every page form, the budget and the tail order.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item541_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item541_root" || exit 1

item541_files=(
    kernel/include/install_journal_core.h
    kernel/src/install_journal_core.c
    kernel/include/install_journal_platform.h
    kernel/src/install_journal_platform.c
    kernel/include/xhci.h
    kernel/src/xhci.c
    kernel/include/installer_storage.h
    kernel/src/installer_storage.c
    kernel/src/installer_storage_screen.c
    kernel/src/kernel.c
    tests/installer-storage-host.c
)
for item541_file in "${item541_files[@]}"; do
    [[ -f $item541_file ]] || {
        echo "ITEM-541: отсутствует $item541_file" >&2
        exit 1
    }
done

# 1. The journal core carries the reason and lends the ring tail read-only.
for item541_pin in \
    'const char *reason;' \
    'const char *install_journal_reason(' \
    'uint32_t install_journal_pending_count(' \
    'const char *install_journal_pending_record('; do
    grep -Fq -- "$item541_pin" kernel/include/install_journal_core.h || {
        echo "ITEM-541: ядро журнала не несёт '$item541_pin'" >&2
        exit 1
    }
done
for item541_pin in \
    'journal->reason = "NONE";' \
    'journal->reason = reason;' \
    'journal->reason = "OK";'; do
    grep -Fq -- "$item541_pin" kernel/src/install_journal_core.c || {
        echo "ITEM-541: ядро журнала не называет причину '$item541_pin'" >&2
        exit 1
    }
done

# 2. The platform seam answers the page with one status call and the records.
for item541_pin in \
    'struct install_journal_status {' \
    'void install_journal_platform_status(' \
    'const char *install_journal_platform_record('; do
    grep -Fq -- "$item541_pin" kernel/include/install_journal_platform.h || {
        echo "ITEM-541: шов журнала не несёт '$item541_pin'" >&2
        exit 1
    }
done
grep -Fq 'install_journal_reason(&platform_journal)' \
    kernel/src/install_journal_platform.c || {
    echo "ITEM-541: шов журнала не публикует причину" >&2
    exit 1
}

# 3. The medium verdict answers with the reason, and the old conflation is gone.
grep -Fq 'bool xhci_storage_medium_status(const char **state, const char **reason);' \
    kernel/include/xhci.h || {
    echo "ITEM-541: xhci.h не несёт статус носителя" >&2
    exit 1
}
for item541_pin in \
    '*reason = "NO_DEVICE";' \
    '*reason = "NOT_PROBED";' \
    'storage->medium_reason = verdict.reason;'; do
    grep -Fq -- "$item541_pin" kernel/src/xhci.c || {
        echo "ITEM-541: проба носителя не несёт '$item541_pin'" >&2
        exit 1
    }
done

# 4. The page builder names the medium, the journal and the bounded tail, and
#    an empty observed report is a statement rather than silence.
for item541_pin in \
    '#define INSTALLER_STORAGE_REFUSAL_TAIL_MAX 12U' \
    'struct installer_refusal_evidence {' \
    'size_t installer_storage_refusal_lines('; do
    grep -Fq -- "$item541_pin" kernel/include/installer_storage.h || {
        echo "ITEM-541: страница не несёт '$item541_pin'" >&2
        exit 1
    }
done
for item541_pin in \
    '"[INSTALL-MEDIUM] state="' \
    '"[INSTALL-LOG] state="' \
    '"[INSTALL-LOG-TAIL] shown="' \
    '"[STORAGE-REPORT] state=NOT_PROBED"'; do
    grep -Fq -- "$item541_pin" kernel/src/installer_storage.c || {
        echo "ITEM-541: страница не печатает '$item541_pin'" >&2
        exit 1
    }
done
grep -Fq 'installer_storage_refusal_lines(' \
    kernel/src/installer_storage_screen.c || {
    echo "ITEM-541: экран страницы не читает общий построитель" >&2
    exit 1
}

# 5. The kernel hands the page the evidence it gathered.
for item541_pin in \
    'kernel_installer_refusal_evidence(&evidence);' \
    'installer_storage_refusal_show(refusal, &observed, &evidence,'; do
    grep -Fq -- "$item541_pin" kernel/src/kernel.c || {
        echo "ITEM-541: ядро не передаёт '$item541_pin'" >&2
        exit 1
    }
done

# 6. The host proof drives every page form, the budget and the tail order.
for item541_case in \
    'test_the_refusal_page_names_medium_journal_and_tail' \
    'test_the_refusal_page_says_not_probed_instead_of_nothing' \
    'test_the_refusal_page_bounds_the_ring_tail'; do
    grep -Fq -- "$item541_case" tests/installer-storage-host.c || {
        echo "ITEM-541: свидетель страницы потерял случай $item541_case" >&2
        exit 1
    }
done

# 7. The queue keeps this item as history, the roadmap records it.  ITEM-542
#    owns the current-item pin now.
item541_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '## Item ITEM-541' "$item541_queue" || {
    echo "ITEM-541: очередь потеряла исторический раздел ITEM-541" >&2
    exit 1
}
grep -Fq '## Item ITEM-540' "$item541_queue" || {
    echo "ITEM-541: очередь потеряла исторический раздел ITEM-540" >&2
    exit 1
}
grep -Fq 'ITEM-541' docs/ROADMAP.md || {
    echo "ITEM-541: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
