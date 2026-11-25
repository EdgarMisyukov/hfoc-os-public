#!/usr/bin/env bash
# ITEM-538: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-525…537 converted the python halves the witness covers.  The storage and
# configuration group is the next batch: thirteen proofs whose python halves
# still carried ITEM-526's private copy of the periodic rule
# (`periodic_diagnostics = re.compile(r"\[LAPIC-TIMER\] irq …")`) while their
# shell halves already read through the shared renderer (ITEM-528).
#
# This slice pins the conversion:
#   * every proof hands the shared renderer to its python block
#     (`"$ROOT/scripts/hfoc-serial-view.py"`) and reads its serial text only
#     through the one `load_serial_view` and `SERIAL_VIEW.normalise(...)`;
#   * the private copy of the rule is gone from all thirteen;
#   * the assertions, messages and `HFOC-QEMU-MARKER-TIMEOUT` records are
#     unchanged;
#   * the witness carries the strict predicate, the thirteen new proof entries
#     and the class control, and the runner document records the batch.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item538_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item538_root" || exit 1

item538_proofs=(
    scripts/test-qemu-accounting-boundary.sh
    scripts/test-qemu-backup-restore.sh
    scripts/test-qemu-block-cache.sh
    scripts/test-qemu-block-device.sh
    scripts/test-qemu-boot-policy-regression.sh
    scripts/test-qemu-config-journal.sh
    scripts/test-qemu-data-region.sh
    scripts/test-qemu-persistent-config.sh
    scripts/test-qemu-persistent-config-reboot-acceptance.sh
    scripts/test-qemu-persistent-network-config.sh
    scripts/test-qemu-power-cycle-continuity-drill.sh
    scripts/test-qemu-storage-interruption.sh
    scripts/test-qemu-update-lifecycle.sh
)
for item538_file in "${item538_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item538_file ]] || {
        echo "ITEM-538: отсутствует $item538_file" >&2
        exit 1
    }
done

# 1. Every converted reader hands the shared renderer over, loads it through the
#    one loader and normalises its serial text.
for item538_proof in "${item538_proofs[@]}"; do
    for item538_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        grep -Fq -- "$item538_pin" "$item538_proof" || {
            echo "ITEM-538: $item538_proof не несёт '$item538_pin'" >&2
            exit 1
        }
    done
    if grep -Fq -- 'periodic_diagnostics' "$item538_proof"; then
        echo "ITEM-538: $item538_proof сохранил частную копию правила" >&2
        exit 1
    fi
done

# 2. The assertions the readers make are unchanged: one record per proof that
#    only its python half reads.
item538_markers=(
    'scripts/test-qemu-accounting-boundary.sh:[ACCOUNTING-BOUNDARY] audit correlation exact'
    'scripts/test-qemu-backup-restore.sh:[BACKUP-RESTORE] active state changed'
    'scripts/test-qemu-block-cache.sh:[BLOCK-CACHE] cache hit preserved'
    'scripts/test-qemu-block-device.sh:[BLOCK-DEVICE] alignment rejected'
    'scripts/test-qemu-boot-policy-regression.sh:[BOOT-POLICY] failed'
    'scripts/test-qemu-config-journal.sh:[CONFIG-JOURNAL] atomic retry committed'
    'scripts/test-qemu-data-region.sh:[DATA-REGION] failed'
    'scripts/test-qemu-persistent-config.sh:[PERSISTENT-CONFIG] default hostname loaded'
    'scripts/test-qemu-persistent-config-reboot-acceptance.sh:[REBOOT-CONFIG] phase1 committed'
    'scripts/test-qemu-persistent-network-config.sh:[NETWORK-PERSISTENT] default DHCP config loaded'
    'scripts/test-qemu-power-cycle-continuity-drill.sh:[POWER-CYCLE-DRILL] clean reboot'
    'scripts/test-qemu-storage-interruption.sh:[STORAGE-INTERRUPTION] backup stream valid'
    'scripts/test-qemu-update-lifecycle.sh:[UPDATE-LIFECYCLE] audit continuity verified'
)
for item538_marker in "${item538_markers[@]}"; do
    item538_proof=${item538_marker%%:*}
    item538_text=${item538_marker#*:}
    grep -Fq -- "$item538_text" "$item538_proof" || {
        echo "ITEM-538: $item538_proof потерял утверждение:" \
            "$item538_text" >&2
        exit 1
    }
done

# 3. The witness carries the strict predicate, the thirteen new proofs and the
#    class control.
item538_host=tests/serial-view-normalised-host.py
for item538_proof in "${item538_proofs[@]}"; do
    grep -Fq -- "\"$item538_proof\"," "$item538_host" || {
        echo "ITEM-538: свидетель не перечисляет $item538_proof" >&2
        exit 1
    }
done
for item538_control in \
    'normalised = ("serial_view" in invocation or LOADER in source)' \
    'PYTHON_HALF_PROOFS = [' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item538_control" "$item538_host" || {
        echo "ITEM-538: свидетель потерял контроль: $item538_control" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item538_doc in \
    'ITEM-538' \
    'storage and configuration group'; do
    grep -Fq -- "$item538_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-538:" \
            "$item538_doc" >&2
        exit 1
    }
done

# 5. The queue is on this item and the roadmap records it.
item538_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
# This item is history now that a later item owns the current section; the
# document has to keep recording it.
grep -Fq 'ITEM-538' "$item538_queue" || {
    echo "ITEM-538: очередь не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-538' docs/ROADMAP.md || {
    echo "ITEM-538: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
