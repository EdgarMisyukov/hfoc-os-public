#!/usr/bin/env bash
# ITEM-533: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529…532 converted the python halves of the proofs the witness covers and
# of the panic/fault, input/console and https matrix groups.  The installer
# group - disk-screen, display and production-installer - reads the same serial
# file from embedded python and matches the installer screen records, the
# refusal page and the production-install records against text a periodic line
# can split.  Their shell halves have no serial assertion, so they are listed in
# the witness's `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those four readers:
#   * each proof hands the shared renderer to its python block(s) and normalises
#     every read through the same `load_serial_view`;
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
item533_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item533_root" || exit 1

item533_proofs=(
    scripts/test-qemu-installer-disk-screen.sh
    scripts/test-qemu-installer-display.sh
    scripts/test-qemu-production-installer.sh
)
for item533_file in "${item533_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item533_file ]] || {
        echo "ITEM-533: отсутствует $item533_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (disk-screen has two python blocks, the rest one each).
item533_expectations=(
    'scripts/test-qemu-installer-disk-screen.sh 2'
    'scripts/test-qemu-installer-display.sh 1'
    'scripts/test-qemu-production-installer.sh 1'
)
for item533_expectation in "${item533_expectations[@]}"; do
    item533_proof=${item533_expectation% *}
    item533_minimum=${item533_expectation#* }
    for item533_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item533_count=$(grep -Fc -- "$item533_pin" "$item533_proof" || true)
        [[ $item533_count -ge $item533_minimum ]] || {
            echo "ITEM-533: в $item533_proof $item533_count вхождений" \
                "'$item533_pin', ожидалось не меньше $item533_minimum" >&2
            exit 1
        }
    done
done
# The witness is the authority on "no raw read"; this slice keeps the count of
# normalised reads per proof in step 1.

# 2. The assertions the readers make are unchanged.
for item533_marker in \
    'scripts/test-qemu-installer-disk-screen.sh:This medium cannot install on this machine.' \
    'scripts/test-qemu-installer-disk-screen.sh:refused reason=' \
    'scripts/test-qemu-installer-display.sh:[DISPLAY] backend=GOP status=AVAILABLE' \
    'scripts/test-qemu-production-installer.sh:[PRODUCTION-INSTALLER] proof init'; do
    item533_proof=${item533_marker%%:*}
    item533_text=${item533_marker#*:}
    grep -Fq -- "$item533_text" "$item533_proof" || {
        echo "ITEM-533: $item533_proof потерял утверждение:" \
            "$item533_text" >&2
        exit 1
    }
done

# 3. The witness lists these proofs and keeps the class control.
item533_host=tests/serial-view-normalised-host.py
for item533_control in \
    'PYTHON_HALF_PROOFS = [' \
    'scripts/test-qemu-installer-disk-screen.sh' \
    'scripts/test-qemu-installer-display.sh' \
    'scripts/test-qemu-production-installer.sh' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item533_control" "$item533_host" || {
        echo "ITEM-533: свидетель потерял контроль:" \
            "$item533_control" >&2
        exit 1
    }
done
for item533_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item533_witness" Makefile || {
        echo "ITEM-533: свидетель не зарегистрирован:" \
            "$item533_witness" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item533_doc in \
    'ITEM-533' \
    'installer group' \
    'disk-screen'; do
    grep -Fq -- "$item533_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-533:" \
            "$item533_doc" >&2
        exit 1
    }
done
