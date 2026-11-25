#!/usr/bin/env bash
# ITEM-536: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529…535 converted the python halves of the proofs the witness covers and
# of the panic/fault, input/console, https matrix, installer, https-client/drill
# and install-flow/physical-storage groups.  The heap, pmm and vmm group closes
# the class: `test-qemu-heap.sh` (phase one and the validation block),
# `test-qemu-pmm.sh` (high-memory boot, UEFI boot, phase one and validation),
# `test-qemu-vmm.sh` (phase one and validation) and `test-qemu-vmm-as.sh` each
# read the same serial file from embedded python and match their records
# against text a periodic line can split.
#
# The pmm high-memory and UEFI blocks are handed `$HIGHMEM_SERIAL_LOG` and
# `$UEFI_SERIAL_LOG`, which the declared-serial-variable scan cannot see, so the
# witness grows the narrow `EXTRA_SERIAL_NAMES` entry for exactly those two
# names before the conversion control can cover them.  Two blocks already
# carried a private copy of the periodic rule (`normalise_suffix`); they now
# delegate to the shared renderer, and the byte offsets (`len(serial_bytes())`,
# `data[offset:]`) stay raw as ITEM-527 requires.  The shell halves of the four
# proofs have no serial assertion, so they are listed in `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those ten readers:
#   * each proof hands the shared renderer to every embedded python block and
#     normalises every read through the same `load_serial_view`;
#   * the assertions the readers make are unchanged;
#   * the witness carries the extra names, the proof list and the class
#     control, and the runner document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item536_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item536_root" || exit 1

item536_proofs=(
    scripts/test-qemu-heap.sh
    scripts/test-qemu-pmm.sh
    scripts/test-qemu-vmm.sh
    scripts/test-qemu-vmm-as.sh
)
for item536_file in "${item536_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item536_file ]] || {
        echo "ITEM-536: отсутствует $item536_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (heap and vmm two blocks each, pmm four, vmm-as one).
item536_expectations=(
    'scripts/test-qemu-heap.sh 2'
    'scripts/test-qemu-pmm.sh 4'
    'scripts/test-qemu-vmm.sh 2'
    'scripts/test-qemu-vmm-as.sh 1'
)
for item536_expectation in "${item536_expectations[@]}"; do
    item536_proof=${item536_expectation% *}
    item536_minimum=${item536_expectation#* }
    for item536_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item536_count=$(grep -Fc -- "$item536_pin" "$item536_proof" || true)
        [[ $item536_count -ge $item536_minimum ]] || {
            echo "ITEM-536: в $item536_proof $item536_count вхождений" \
                "'$item536_pin', ожидалось не меньше $item536_minimum" >&2
            exit 1
        }
    done
done

# 2. The private copies of the rule delegate to the shared renderer, and the
#    byte offsets stay raw.
for item536_rule in \
    'scripts/test-qemu-heap.sh:def normalise_suffix(text):' \
    'scripts/test-qemu-heap.sh:return SERIAL_VIEW.normalise(text)' \
    'scripts/test-qemu-pmm.sh:def normalise_suffix(text):' \
    'scripts/test-qemu-pmm.sh:return SERIAL_VIEW.normalise(text)'; do
    item536_proof=${item536_rule%%:*}
    item536_text=${item536_rule#*:}
    grep -Fq -- "$item536_text" "$item536_proof" || {
        echo "ITEM-536: $item536_proof потерял общее правило:" \
            "$item536_text" >&2
        exit 1
    }
done
for item536_offset in \
    'scripts/test-qemu-heap.sh:str(len(serial_bytes()))' \
    'scripts/test-qemu-pmm.sh:str(len(serial_bytes()))' \
    'scripts/test-qemu-vmm.sh:str(len(serial_bytes()))'; do
    item536_proof=${item536_offset%%:*}
    item536_text=${item536_offset#*:}
    grep -Fq -- "$item536_text" "$item536_proof" || {
        echo "ITEM-536: $item536_proof потерял сырое смещение:" \
            "$item536_text" >&2
        exit 1
    }
done

# 3. The assertions the readers make are unchanged.
for item536_marker in \
    'scripts/test-qemu-heap.sh:[HEAP] ready=' \
    'scripts/test-qemu-heap.sh:HFOC-QEMU-MARKER-TIMEOUT marker="HEAP diagnostic"' \
    'scripts/test-qemu-pmm.sh:[PMM ] status=' \
    'scripts/test-qemu-pmm.sh:[PMM ] usable=' \
    'scripts/test-qemu-pmm.sh:HFOC-QEMU-MARKER-TIMEOUT marker="high-memory boot markers"' \
    'scripts/test-qemu-pmm.sh:HFOC-QEMU-MARKER-TIMEOUT marker="UEFI boot markers"' \
    'scripts/test-qemu-vmm.sh:[VMM] MMIO page mapped successfully' \
    'scripts/test-qemu-vmm.sh:[VMM] Mapping LAPIC page phys=0x00000000FEE00000' \
    'scripts/test-qemu-vmm-as.sh:[VMM-AS] address space map/activate/restore: OK'; do
    item536_proof=${item536_marker%%:*}
    item536_text=${item536_marker#*:}
    grep -Fq -- "$item536_text" "$item536_proof" || {
        echo "ITEM-536: $item536_proof потерял утверждение:" \
            "$item536_text" >&2
        exit 1
    }
done

# 4. The witness lists these proofs, names the serial variables it must
#    recognise and keeps the class control.
item536_host=tests/serial-view-normalised-host.py
for item536_control in \
    'EXTRA_SERIAL_NAMES = {' \
    '"scripts/test-qemu-pmm.sh": {' \
    '"HIGHMEM_SERIAL_LOG", "UEFI_SERIAL_LOG",' \
    'scripts/test-qemu-heap.sh' \
    'scripts/test-qemu-pmm.sh' \
    'scripts/test-qemu-vmm.sh' \
    'scripts/test-qemu-vmm-as.sh' \
    'PYTHON_HALF_PROOFS = [' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item536_control" "$item536_host" || {
        echo "ITEM-536: свидетель потерял контроль:" \
            "$item536_control" >&2
        exit 1
    }
done
for item536_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item536_witness" Makefile || {
        echo "ITEM-536: свидетель не зарегистрирован:" \
            "$item536_witness" >&2
        exit 1
    }
done

# 5. The runner document owns the batch.
for item536_doc in \
    'ITEM-536' \
    'heap' \
    'pmm and vmm group'; do
    grep -Fq -- "$item536_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-536:" \
            "$item536_doc" >&2
        exit 1
    }
done
