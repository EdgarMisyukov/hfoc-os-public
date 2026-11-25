#!/usr/bin/env bash
# ITEM-530: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529 converted the five python readers of the proofs the witness already
# covered.  The panic/fault group - doublefault, nmi, pagefault and panic - reads
# the same serial file from embedded python, and their readers match structured
# frames (`vector=... error=... rip=...`), panic records and the one boot
# signature against text a periodic line can split.  Their shell halves have no
# serial assertion at all, so they are listed in the witness's
# `PYTHON_HALF_PROOFS` rather than in `PROOFS`.
#
# This slice pins the conversion of those eight readers:
#   * each proof hands the shared renderer to both of its python blocks and
#     normalises every read through the same `load_serial_view`;
#   * the offset bookkeeping stays on the raw bytes (the normalised text is only
#     what the markers are searched in), so a suffix or a byte count cannot
#     drift;
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
item530_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item530_root" || exit 1

item530_proofs=(
    scripts/test-qemu-doublefault.sh
    scripts/test-qemu-nmi.sh
    scripts/test-qemu-pagefault.sh
    scripts/test-qemu-panic.sh
)
for item530_file in "${item530_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item530_file ]] || {
        echo "ITEM-530: отсутствует $item530_file" >&2
        exit 1
    }
done

# 1. Both python blocks of every proof carry the one loader and normalise their
#    reads through it; the renderer path is handed in as the last argument.
for item530_proof in "${item530_proofs[@]}"; do
    item530_loaders=$(grep -Fc 'def load_serial_view(path):' \
        "$item530_proof" || true)
    [[ $item530_loaders -ge 2 ]] || {
        echo "ITEM-530: в $item530_proof $item530_loaders загрузчиков," \
            "ожидалось 2" >&2
        exit 1
    }
    item530_reads=$(grep -Fc 'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        "$item530_proof" || true)
    [[ $item530_reads -ge 2 ]] || {
        echo "ITEM-530: в $item530_proof $item530_reads чтений через вид," \
            "ожидалось 2" >&2
        exit 1
    }
    item530_normalised=$(grep -Fc 'SERIAL_VIEW.normalise(' \
        "$item530_proof" || true)
    [[ $item530_normalised -ge 2 ]] || {
        echo "ITEM-530: в $item530_proof $item530_normalised нормализаций," \
            "ожидалось не меньше 2" >&2
        exit 1
    }
    item530_renders=$(grep -Fc '"$ROOT/scripts/hfoc-serial-view.py"' \
        "$item530_proof" || true)
    [[ $item530_renders -ge 2 ]] || {
        echo "ITEM-530: в $item530_proof $item530_renders передач пути" \
            "рендерера, ожидалось 2" >&2
        exit 1
    }
done

# 2. The offset bookkeeping stays on the raw bytes: the byte length written for
#    the next phase and the suffix written to the artifact are not normalised.
for item530_raw in \
    'offset_path.write_text(str(len(serial_bytes())), encoding="ascii")' \
    'suffix_path.write_bytes(suffix)'; do
    grep -Fq -- "$item530_raw" "${item530_proofs[@]}" || {
        echo "ITEM-530: сырой учёт байтов исчез:" "$item530_raw" >&2
        exit 1
    }
done

# 3. The assertions the readers make are unchanged.
for item530_marker in \
    'scripts/test-qemu-doublefault.sh:TRIGGERING DOUBLE FAULT VIA #PF GATE NOT-PRESENT...' \
    'scripts/test-qemu-doublefault.sh:double-fault:' \
    'scripts/test-qemu-nmi.sh:nmi:' \
    'scripts/test-qemu-pagefault.sh:page-fault:' \
    'scripts/test-qemu-panic.sh:TRIGGERING DIVIDE ERROR...' \
    'scripts/test-qemu-panic.sh:Divide error' \
    'scripts/test-qemu-panic.sh:HFOC OS 1.0.0 INITIALISED'; do
    item530_proof=${item530_marker%%:*}
    item530_text=${item530_marker#*:}
    grep -Fq -- "$item530_text" "$item530_proof" || {
        echo "ITEM-530: $item530_proof потерял утверждение:" \
            "$item530_text" >&2
        exit 1
    }
done
for item530_timeout in "${item530_proofs[@]}"; do
    grep -Fq 'HFOC-QEMU-MARKER-TIMEOUT' "$item530_timeout" || {
        echo "ITEM-530: $item530_timeout потерял запись тайм-аута" >&2
        exit 1
    }
done

# 4. The witness lists these proofs and keeps the class control.
item530_host=tests/serial-view-normalised-host.py
for item530_control in \
    'PYTHON_HALF_PROOFS = [' \
    'scripts/test-qemu-doublefault.sh' \
    'scripts/test-qemu-nmi.sh' \
    'scripts/test-qemu-pagefault.sh' \
    'scripts/test-qemu-panic.sh' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item530_control" "$item530_host" || {
        echo "ITEM-530: свидетель потерял контроль:" \
            "$item530_control" >&2
        exit 1
    }
done
for item530_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item530_witness" Makefile || {
        echo "ITEM-530: свидетель не зарегистрирован:" \
            "$item530_witness" >&2
        exit 1
    }
done

# 5. The runner document owns the batch.
for item530_doc in \
    'ITEM-530' \
    'PYTHON_HALF_PROOFS' \
    'panic/fault'; do
    grep -Fq -- "$item530_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-530:" \
            "$item530_doc" >&2
        exit 1
    }
done
