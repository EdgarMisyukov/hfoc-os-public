#!/usr/bin/env bash
# ITEM-529: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-525 fixed one boot's shell half and its embedded python; ITEM-526 and
# ITEM-527 converted the shell halves of the remaining QEMU proofs; ITEM-528
# narrowed the shared rule to the periodic IRQ lines.  The python halves of the
# proofs the witness covers were still reading the raw serial file and matching
# records in it, so a periodic line landing inside a record could make one half
# see a record the other half missed - the same defect, in the other direction.
#
# This slice pins the conversion of the five remaining readers
# (`test-qemu-irqs.sh`, `test-qemu-usb-two-controllers.sh`):
#   * each one loads the shared renderer through the same byte-identical
#     `load_serial_view` and normalises every read;
#   * the reader count per proof is kept, so a conversion cannot silently drop a
#     check;
#   * the assertions the readers make are unchanged (the markers are still
#     required and the failure records still say what they said);
#   * the witness carries the class control for every proof it covers, and the
#     runner document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item529_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item529_root" || exit 1

for item529_file in \
    scripts/test-qemu-irqs.sh \
    scripts/test-qemu-usb-two-controllers.sh \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item529_file ]] || {
        echo "ITEM-529: отсутствует $item529_file" >&2
        exit 1
    }
done

# 1. Every converted reader loads the shared renderer through the same loader
#    and reads through it.  The loader is pinned line by line, so a copy cannot
#    drift; the witness pins the same text as one block.
item529_loader_lines=(
    'def load_serial_view(path):'
    '    import importlib.util'
    '    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)'
    '    module = importlib.util.module_from_spec(spec)'
    '    spec.loader.exec_module(module)'
    '    return module'
)
item529_expectations=(
    'scripts/test-qemu-irqs.sh 1'
    'scripts/test-qemu-usb-two-controllers.sh 4'
)
for item529_expectation in "${item529_expectations[@]}"; do
    item529_proof=${item529_expectation% *}
    item529_minimum=${item529_expectation#* }
    item529_loaders=$(grep -Fc 'def load_serial_view(path):' \
        "$item529_proof" || true)
    [[ $item529_loaders -ge $item529_minimum ]] || {
        echo "ITEM-529: в $item529_proof $item529_loaders загрузчиков," \
            "ожидалось не меньше $item529_minimum" >&2
        exit 1
    }
    item529_reads=$(grep -Fc 'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        "$item529_proof" || true)
    [[ $item529_reads -ge $item529_minimum ]] || {
        echo "ITEM-529: в $item529_proof $item529_reads чтений через вид," \
            "ожидалось не меньше $item529_minimum" >&2
        exit 1
    }
    item529_renders=$(grep -Fc '"$ROOT/scripts/hfoc-serial-view.py"' \
        "$item529_proof" || true)
    [[ $item529_renders -ge $item529_minimum ]] || {
        echo "ITEM-529: в $item529_proof $item529_renders передач пути" \
            "рендерера, ожидалось не меньше $item529_minimum" >&2
        exit 1
    }
    for item529_line in "${item529_loader_lines[@]}"; do
        grep -Fq -- "$item529_line" "$item529_proof" || {
            echo "ITEM-529: $item529_proof потерял строку загрузчика:" \
                "$item529_line" >&2
            exit 1
        }
    done
done

# 2. The readers still read raw text nowhere: a python block handed a serial
#    variable must either be handed a rendered view, load the shared renderer,
#    or carry the periodic-line rule ITEM-526 pinned - the same criterion the
#    witness applies.
python3 - "${item529_expectations[@]}" <<'PY' || exit 1
import pathlib
import re
import sys

# Same extraction rule as the witness (tests/serial-view-normalised-host.py):
# the invocation is the `python3 - ...` statement, the source is the heredoc.
for expectation in sys.argv[1:]:
    proof, minimum = expectation.split()
    lines = pathlib.Path(proof).read_text().splitlines()
    found = 0
    index = 0
    while index < len(lines):
        if "python3 -" not in lines[index]:
            index += 1
            continue
        marker = index
        while marker < len(lines) and "<<'" not in lines[marker]:
            marker += 1
        if marker >= len(lines):
            break
        name = re.search(r"<<'([A-Z_]+)'", lines[marker]).group(1)
        end = marker + 1
        while end < len(lines) and lines[end].strip() != name:
            end += 1
        invocation = "\n".join(lines[index:marker + 1])
        source = "\n".join(lines[marker + 1:end])
        if re.search(r"\$(?:[A-Za-z_]*SERIAL[A-Za-z_]*|[a-z_]*serial[a-z_]*)",
                     invocation):
            normalised = ("serial_view" in invocation or
                          "load_serial_view" in source or
                          re.search(r"IOAPIC|LAPIC-TIMER", source) is not None)
            if not normalised:
                raise SystemExit(
                    f"ITEM-529: {proof}:{index + 1} читает сырой serial")
            if "load_serial_view" in source:
                found += 1
        index = end + 1
    if found < int(minimum):
        raise SystemExit(
            f"ITEM-529: в {proof} {found} конвертированных читалок, "
            f"ожидалось не меньше {minimum}")
PY

# 3. The assertions the readers make are unchanged.
for item529_marker in \
    'scripts/test-qemu-irqs.sh:completed boot and hfoc> prompt' \
    'scripts/test-qemu-irqs.sh:HFOC-QEMU-MARKER-TIMEOUT' \
    'scripts/test-qemu-usb-two-controllers.sh:the mass-storage phase panicked' \
    'scripts/test-qemu-usb-two-controllers.sh:the SuperSpeed phase panicked' \
    'scripts/test-qemu-usb-two-controllers.sh:the boot-medium phase panicked'; do
    item529_proof=${item529_marker%%:*}
    item529_text=${item529_marker#*:}
    grep -Fq -- "$item529_text" "$item529_proof" || {
        echo "ITEM-529: $item529_proof потерял утверждение:" \
            "$item529_text" >&2
        exit 1
    }
done

# 4. The witness carries the class control and is registered in the canonical
#    host suite.
item529_host=tests/serial-view-normalised-host.py
for item529_control in \
    'def python_blocks(text):' \
    'LOADER = ' \
    'python halves read the normalised stream' \
    'hfoc_serial_view'; do
    grep -Fq -- "$item529_control" "$item529_host" || {
        echo "ITEM-529: свидетель потерял контроль:" \
            "$item529_control" >&2
        exit 1
    }
done
for item529_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item529_witness" Makefile || {
        echo "ITEM-529: свидетель не зарегистрирован:" \
            "$item529_witness" >&2
        exit 1
    }
done

# 5. The runner document owns the conversion.
for item529_doc in \
    'ITEM-529' \
    'python halve' \
    'load_serial_view'; do
    grep -Fq -- "$item529_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-529:" \
            "$item529_doc" >&2
        exit 1
    }
done
