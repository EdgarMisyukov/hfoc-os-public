#!/usr/bin/env bash
# ITEM-486: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# `other_us` names its parts.
#
# The owner's 2026-09-26 journal (`/srv/hfoc-example/queue/hw9-log-all.txt`) showed
# one loop window of `other_us=3 108 092` beside `work_us=3 108 657`,
# `turn_us=40 992` and near-zero `console_us`/`xhci_us`/`input_us`/`net_us`:
# a three-second freeze lived in the unnamed remainder, and neither the journal
# nor the operator could tell which subsystem spent it.  This slice pins the
# instrumentation that names it - measurement only, no behaviour change:
#
#   * three top-level spans - the services and application state, the journal
#     records and their disk flush, and the storage/queued writes - are measured
#     outside the spans that already existed, folded into the window with them,
#     and published as `svc_us`, `journal_us`, `store_us`;
#   * `other_us` stays the residual after them, so the accounted parts still
#     cannot exceed `work_us`;
#   * the worst-turn name can now be `svc`, `journal` or `store`;
#   * the host proof drives the arithmetic and the QEMU proof reads the fields
#     and adds them to its parts-vs-work accounting.

for item486_file in \
    kernel/src/kernel.c \
    kernel/include/loop_budget.h \
    kernel/src/loop_budget.c \
    tests/loop-budget-host.c \
    scripts/test-qemu-production-storage-boot.sh \
    docs/hfoc-appliance-boot-console.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item486_file ]] || {
        echo "ITEM-486: отсутствует $item486_file" >&2
        exit 1
    }
done

# ---------------------------------------------------- the three top-level spans --
for item486_phase in \
    KERNEL_LOOP_PHASE_SERVICES \
    KERNEL_LOOP_PHASE_JOURNAL \
    KERNEL_LOOP_PHASE_STORAGE; do
    grep -Fq "$item486_phase" kernel/src/kernel.c || {
        echo "ITEM-486: kernel.c не измеряет $item486_phase" >&2
        exit 1
    }
done
for item486_budget_phase in \
    LOOP_BUDGET_SERVICES \
    LOOP_BUDGET_JOURNAL \
    LOOP_BUDGET_STORAGE; do
    grep -Fq "$item486_budget_phase" kernel/include/loop_budget.h || {
        echo "ITEM-486: loop_budget.h не называет $item486_budget_phase" >&2
        exit 1
    }
    grep -Fq "$item486_budget_phase" kernel/src/loop_budget.c || {
        echo "ITEM-486: loop_budget.c не разбирает $item486_budget_phase" >&2
        exit 1
    }
done
for item486_field in 'uint64_t svc_us;' 'uint64_t journal_us;' 'uint64_t store_us;'; do
    grep -Fq "$item486_field" kernel/include/loop_budget.h || {
        echo "ITEM-486: снимок оборота не несёт $item486_field" >&2
        exit 1
    }
done
for item486_name in '"svc"' '"journal"' '"store"'; do
    grep -Fq "return $item486_name;" kernel/src/loop_budget.c || {
        echo "ITEM-486: у фазы нет стабильного имени $item486_name" >&2
        exit 1
    }
done

# ------------------------------------------------- the record and the residual --
for item486_published in '" svc_us="' '" journal_us="' '" store_us="'; do
    grep -Fq "$item486_published" kernel/src/kernel.c || {
        echo "ITEM-486: [LOOP-PHASES] не публикует $item486_published" >&2
        exit 1
    }
done
grep -Fq 'svc_us + journal_us + store_us;' kernel/src/kernel.c || {
    echo "ITEM-486: остаток other_us не вычитает названные части" >&2
    exit 1
}
grep -Fq 'snapshot->svc_us + snapshot->journal_us +' kernel/src/loop_budget.c || {
    echo "ITEM-486: худший оборот не вычитает названные части" >&2
    exit 1
}

# --------------------------------------------------------------- the proofs --
grep -Fq 'test_remainder_names_its_parts' tests/loop-budget-host.c || {
    echo "ITEM-486: host-проба не разбирает названные части остатка" >&2
    exit 1
}
grep -Fq '"svc_us", "journal_us", "store_us",' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-486: QEMU-проба не читает названные части" >&2
    exit 1
}
grep -Fq '"svc", "journal", "store",' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-486: QEMU-проба не знает имён новых фаз" >&2
    exit 1
}
grep -Fq '"svc_us", "journal_us", "store_us", "other_us"' \
    scripts/test-qemu-production-storage-boot.sh || {
    echo "ITEM-486: QEMU-проба не сверяет части с work_us" >&2
    exit 1
}
grep -Fq 'test-loop-budget-host' Makefile || {
    echo "ITEM-486: канонический host-suite не содержит test-loop-budget-host" >&2
    exit 1
}

# ------------------------------------------------------------- the documents --
for item486_doc_word in ITEM-486 svc_us journal_us store_us; do
    grep -Fq -- "$item486_doc_word" docs/hfoc-appliance-boot-console.md || {
        echo "docs/hfoc-appliance-boot-console.md не фиксирует ITEM-486:" \
            "$item486_doc_word" >&2
        exit 1
    }
done
grep -Fq 'ITEM-486' docs/ROADMAP.md || {
    echo "docs/ROADMAP.md не фиксирует ITEM-486" >&2
    exit 1
}
grep -Fq 'ITEM-486' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-486" >&2
    exit 1
}
