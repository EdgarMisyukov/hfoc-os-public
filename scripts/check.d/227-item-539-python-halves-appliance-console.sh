#!/usr/bin/env bash
# ITEM-539: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-525…538 converted the python halves the witness covers.  The appliance,
# console and session group is the last safe batch: twelve proofs whose python
# halves still carried ITEM-526's private copy of the periodic rule
# (`periodic_diagnostics = re.compile(r"\[LAPIC-TIMER\] irq …")`) while their
# shell halves already read through the shared renderer (ITEM-528).
# process-scheduler and scheduler-stress are deliberately not in this batch:
# their assertions search the periodic lines the shared rule removes.
#
# This slice pins the conversion:
#   * every proof hands the shared renderer to its python block
#     (`"$ROOT/scripts/hfoc-serial-view.py"`) and reads its serial text only
#     through the one `load_serial_view` and `SERIAL_VIEW.normalise(...)`;
#   * the private copy of the rule is gone from all thirteen;
#   * the assertions, messages and `HFOC-QEMU-MARKER-TIMEOUT` records are
#     unchanged;
#   * the witness carries the strict predicate, the twelve new proof entries
#     and the class control, and the runner document records the batch.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item539_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item539_root" || exit 1

item539_proofs=(
    scripts/test-qemu-appliance-boot-console.sh
    scripts/test-qemu-appliance-console.sh
    scripts/test-qemu-install-notice.sh
    scripts/test-qemu-integrated-appliance-boot-acceptance.sh
    scripts/test-qemu-local-control-outage-drill.sh
    scripts/test-qemu-network-config.sh
    scripts/test-qemu-service-cascade-drill.sh
    scripts/test-qemu-supportability.sh
    scripts/test-qemu-syscall.sh
    scripts/test-qemu-trust-policy.sh
    scripts/test-qemu-user-init.sh
    scripts/test-qemu-user-iretq.sh
)
for item539_file in "${item539_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item539_file ]] || {
        echo "ITEM-539: отсутствует $item539_file" >&2
        exit 1
    }
done

# 1. Every converted reader hands the shared renderer over, loads it through the
#    one loader and normalises its serial text.
for item539_proof in "${item539_proofs[@]}"; do
    for item539_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        grep -Fq -- "$item539_pin" "$item539_proof" || {
            echo "ITEM-539: $item539_proof не несёт '$item539_pin'" >&2
            exit 1
        }
    done
    if grep -Fq -- 'periodic_diagnostics' "$item539_proof"; then
        echo "ITEM-539: $item539_proof сохранил частную копию правила" >&2
        exit 1
    fi
done

# 2. The assertions the readers make are unchanged: one record per proof that
#    only its python half reads.
item539_markers=(
    'scripts/test-qemu-appliance-boot-console.sh:[APPLIANCE-BOOT] RENDER FAILED'
    'scripts/test-qemu-appliance-console.sh:[APPLIANCE] RENDER FAILED'
    'scripts/test-qemu-install-notice.sh:[PRODUCTION-INSTALL] notice screen=NONE'
    'scripts/test-qemu-integrated-appliance-boot-acceptance.sh:[ACPI] Probing RSDP via Limine...'
    'scripts/test-qemu-local-control-outage-drill.sh:[LOCAL-CONTROL-OUTAGE-DRILL] catalogue local-control fault points exact'
    'scripts/test-qemu-network-config.sh:[DHCP-CLIENT] bound lease published'
    'scripts/test-qemu-service-cascade-drill.sh:[SERVICE-CASCADE-DRILL] base restart contained'
    'scripts/test-qemu-supportability.sh:[SUPPORTABILITY] clean reboot'
    'scripts/test-qemu-syscall.sh:[SYSCALL] failed'
    'scripts/test-qemu-trust-policy.sh:[TRUST-POLICY] audit continuity'
    'scripts/test-qemu-user-init.sh:[AUDIT-OBS] consumer start'
    'scripts/test-qemu-user-iretq.sh:[USER] failed'
)
for item539_marker in "${item539_markers[@]}"; do
    item539_proof=${item539_marker%%:*}
    item539_text=${item539_marker#*:}
    grep -Fq -- "$item539_text" "$item539_proof" || {
        echo "ITEM-539: $item539_proof потерял утверждение:" \
            "$item539_text" >&2
        exit 1
    }
done

# 3. The witness carries the strict predicate, the thirteen new proofs and the
#    class control.
item539_host=tests/serial-view-normalised-host.py
for item539_proof in "${item539_proofs[@]}"; do
    grep -Fq -- "\"$item539_proof\"," "$item539_host" || {
        echo "ITEM-539: свидетель не перечисляет $item539_proof" >&2
        exit 1
    }
done
for item539_control in \
    'normalised = ("serial_view" in invocation or LOADER in source)' \
    'PYTHON_HALF_PROOFS = [' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item539_control" "$item539_host" || {
        echo "ITEM-539: свидетель потерял контроль: $item539_control" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item539_doc in \
    'ITEM-539' \
    'appliance, console and session group'; do
    grep -Fq -- "$item539_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-539:" \
            "$item539_doc" >&2
        exit 1
    }
done

# 5. The queue keeps recording this item and the roadmap records it.
item539_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
# This item is history now that a later item owns the current section; the
# document has to keep recording it.
grep -Fq 'ITEM-539' "$item539_queue" || {
    echo "ITEM-539: очередь не называет пункт" >&2
    exit 1
}
grep -Fq 'ITEM-539' docs/ROADMAP.md || {
    echo "ITEM-539: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
