#!/usr/bin/env bash
# ITEM-532: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529 converted the five python readers of the proofs the witness already
# covered, ITEM-530 the panic/fault group and ITEM-531 the input/console group.
# The https matrix group - exhaustion, malformed-request, response and
# size-boundary - reads the same serial file from embedded python and waits for
# `[HTTPS-QEMU] listener live`, the matrix records and `[HTTPS-QEMU] failed`
# against text a periodic line can split.  Their shell halves have no serial
# assertion, so they are listed in the witness's `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those four readers:
#   * each proof hands the shared renderer to its python block and normalises
#     every read through the same `load_serial_view`;
#   * the assertions the readers make are unchanged, including their
#     `HFOC-QEMU-MARKER-TIMEOUT` records;
#   * the witness carries the list and the class control, and the runner
#     document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item532_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item532_root" || exit 1

item532_proofs=(
    scripts/test-qemu-https-exhaustion-matrix.sh
    scripts/test-qemu-https-malformed-request-matrix.sh
    scripts/test-qemu-https-response-matrix.sh
    scripts/test-qemu-https-size-boundary-matrix.sh
)
for item532_file in "${item532_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item532_file ]] || {
        echo "ITEM-532: отсутствует $item532_file" >&2
        exit 1
    }
done

# 1. Each proof carries the loader, hands in the renderer path and normalises
#    its one serial read.
for item532_proof in "${item532_proofs[@]}"; do
    for item532_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item532_count=$(grep -Fc -- "$item532_pin" "$item532_proof" || true)
        [[ $item532_count -ge 1 ]] || {
            echo "ITEM-532: в $item532_proof нет '$item532_pin'" >&2
            exit 1
        }
    done
    item532_raw=$(grep -Fc \
        'return serial_path.read_bytes().decode("ascii", errors="replace")' \
        "$item532_proof" || true)
    [[ $item532_raw -eq 0 ]] || {
        echo "ITEM-532: в $item532_proof остался сырой serial-читатель" >&2
        exit 1
    }
done

# 2. The assertions the readers make are unchanged.
for item532_marker in \
    '[HTTPS-QEMU] listener live' \
    '[HTTPS-QEMU] failed' \
    'HFOC-QEMU-MARKER-TIMEOUT'; do
    grep -Fq -- "$item532_marker" "${item532_proofs[@]}" || {
        echo "ITEM-532: утверждение исчезло:" "$item532_marker" >&2
        exit 1
    }
done

# 3. The witness lists these proofs and keeps the class control.
item532_host=tests/serial-view-normalised-host.py
for item532_control in \
    'PYTHON_HALF_PROOFS = [' \
    'scripts/test-qemu-https-exhaustion-matrix.sh' \
    'scripts/test-qemu-https-malformed-request-matrix.sh' \
    'scripts/test-qemu-https-response-matrix.sh' \
    'scripts/test-qemu-https-size-boundary-matrix.sh' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item532_control" "$item532_host" || {
        echo "ITEM-532: свидетель потерял контроль:" \
            "$item532_control" >&2
        exit 1
    }
done
for item532_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item532_witness" Makefile || {
        echo "ITEM-532: свидетель не зарегистрирован:" \
            "$item532_witness" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item532_doc in \
    'ITEM-532' \
    'https matrix' \
    'size-boundary'; do
    grep -Fq -- "$item532_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-532:" \
            "$item532_doc" >&2
        exit 1
    }
done
