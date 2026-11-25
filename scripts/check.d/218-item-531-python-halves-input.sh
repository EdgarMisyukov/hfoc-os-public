#!/usr/bin/env bash
# ITEM-531: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529 converted the five python readers of the proofs the witness already
# covered and ITEM-530 the panic/fault group.  The input/console group -
# keyboard-line-editing, local-control, network-foundation, nic-discovery and
# physical-sata-warm-reset - reads the same serial file from embedded python and
# matches typed-command echoes, local-control records, network-foundation
# records, installer NIC records and the AHCI warm-reset records against text a
# periodic line can split.  Their shell halves have no serial assertion, so they
# are listed in the witness's `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those six readers:
#   * each proof hands the shared renderer to its python block(s) and normalises
#     every read through the same `load_serial_view`;
#   * the byte bookkeeping stays raw - keyboard-line-editing keeps the raw suffix
#     slice (`suffix_text_raw`) and its QEMU `-d int` trace reader
#     (`trace_suffix_text`) is deliberately not normalised, it is host output;
#   * the assertions the readers make are unchanged;
#   * the witness carries the list and the class control, and the runner
#     document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item531_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item531_root" || exit 1

item531_proofs=(
    scripts/test-qemu-keyboard-line-editing.sh
    scripts/test-qemu-local-control.sh
    scripts/test-qemu-network-foundation.sh
    scripts/test-qemu-nic-discovery.sh
    scripts/test-qemu-physical-sata-warm-reset.sh
)
for item531_file in "${item531_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item531_file ]] || {
        echo "ITEM-531: отсутствует $item531_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (keyboard-line-editing has two python blocks, the rest one each).
item531_expectations=(
    'scripts/test-qemu-keyboard-line-editing.sh 2'
    'scripts/test-qemu-local-control.sh 1'
    'scripts/test-qemu-network-foundation.sh 1'
    'scripts/test-qemu-nic-discovery.sh 1'
    'scripts/test-qemu-physical-sata-warm-reset.sh 1'
)
for item531_expectation in "${item531_expectations[@]}"; do
    item531_proof=${item531_expectation% *}
    item531_minimum=${item531_expectation#* }
    for item531_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item531_count=$(grep -Fc -- "$item531_pin" "$item531_proof" || true)
        [[ $item531_count -ge $item531_minimum ]] || {
            echo "ITEM-531: в $item531_proof $item531_count вхождений" \
                "'$item531_pin', ожидалось не меньше $item531_minimum" >&2
            exit 1
        }
    done
done

# 2. The byte bookkeeping stays raw: the raw suffix slice exists by name and the
#    host-side QEMU trace reader is not normalised.
for item531_raw in \
    'def suffix_text_raw():' \
    'return SERIAL_VIEW.normalise(suffix_text_raw())' \
    'def trace_suffix_text():' \
    'return data[trace_offset:].decode("ascii", errors="replace")'; do
    grep -Fq -- "$item531_raw" \
        scripts/test-qemu-keyboard-line-editing.sh || {
        echo "ITEM-531: сырой учёт keyboard-пробы исчез:" \
            "$item531_raw" >&2
        exit 1
    }
done
if grep -Fq 'return SERIAL_VIEW.normalise(data[trace_offset:]' \
        scripts/test-qemu-keyboard-line-editing.sh; then
    echo "ITEM-531: host-журнал QEMU -d int нормализуется" >&2
    exit 1
fi

# 3. The assertions the readers make are unchanged.
for item531_marker in \
    'scripts/test-qemu-keyboard-line-editing.sh:UNKNOWN COMMAND: abc. TYPE HELP.' \
    'scripts/test-qemu-keyboard-line-editing.sh:HFOC-QEMU-MARKER-TIMEOUT' \
    'scripts/test-qemu-local-control.sh:[LOCAL-CONTROL] failed' \
    'scripts/test-qemu-local-control.sh:HFOC-QEMU-MARKER-TIMEOUT' \
    'scripts/test-qemu-network-foundation.sh:[NETWORK-FOUNDATION] proof init' \
    'scripts/test-qemu-network-foundation.sh:[NETWORK-FOUNDATION] failed' \
    'scripts/test-qemu-nic-discovery.sh:[INSTALLER-NIC]' \
    'scripts/test-qemu-nic-discovery.sh:install=CONTINUES' \
    'scripts/test-qemu-physical-sata-warm-reset.sh:[AHCI] physical boot 2 marker written' \
    'scripts/test-qemu-physical-sata-warm-reset.sh:[AHCI] failed:'; do
    item531_proof=${item531_marker%%:*}
    item531_text=${item531_marker#*:}
    grep -Fq -- "$item531_text" "$item531_proof" || {
        echo "ITEM-531: $item531_proof потерял утверждение:" \
            "$item531_text" >&2
        exit 1
    }
done

# 4. The witness lists these proofs and keeps the class control.
item531_host=tests/serial-view-normalised-host.py
for item531_control in \
    'PYTHON_HALF_PROOFS = [' \
    'scripts/test-qemu-keyboard-line-editing.sh' \
    'scripts/test-qemu-local-control.sh' \
    'scripts/test-qemu-network-foundation.sh' \
    'scripts/test-qemu-nic-discovery.sh' \
    'scripts/test-qemu-physical-sata-warm-reset.sh' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item531_control" "$item531_host" || {
        echo "ITEM-531: свидетель потерял контроль:" \
            "$item531_control" >&2
        exit 1
    }
done
for item531_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item531_witness" Makefile || {
        echo "ITEM-531: свидетель не зарегистрирован:" \
            "$item531_witness" >&2
        exit 1
    }
done

# 5. The runner document owns the batch.
for item531_doc in \
    'ITEM-531' \
    'input/console' \
    'suffix_text_raw'; do
    grep -Fq -- "$item531_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-531:" \
            "$item531_doc" >&2
        exit 1
    }
done
