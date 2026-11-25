#!/usr/bin/env bash
# ITEM-535: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529…534 converted the python halves of the proofs the witness covers and
# of the panic/fault, input/console, https matrix, installer and https-client/
# drill groups.  The install-flow and physical-storage group closes the batch:
# `production-install-flow` (four embedded readers plus the language boot),
# `production-install-flow-preserve` (three readers plus the four per-phase
# assertions), `physical-sata-acceptance`, `physical-storage` and `boot-chain`
# each read the same serial file from embedded python and match their records
# against text a periodic line can split.
#
# The last five readers are handed their log by a literal path
# (`$TEST_DIR/language.log`, `$TEST_DIR/red.log` and friends) or through
# `${1:-…}`, which the declared-serial-variable scan cannot see, so the witness
# grows a narrow `EXTRA_SERIAL_LITERALS` list for exactly those paths before the
# conversion control can cover them.  Their shell halves have no serial
# assertion, so the proofs are listed in the witness's `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those fifteen readers:
#   * each proof hands the shared renderer to every embedded python block and
#     normalises every read through the same `load_serial_view`;
#   * the assertions the readers make are unchanged;
#   * the witness carries the literal list, the proof list and the class
#     control, and the runner document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item535_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item535_root" || exit 1

item535_proofs=(
    scripts/test-qemu-production-install-flow.sh
    scripts/test-qemu-production-install-flow-preserve.sh
    scripts/test-qemu-physical-sata-acceptance.sh
    scripts/test-qemu-physical-storage.sh
    scripts/test-qemu-boot-chain.sh
)
for item535_file in "${item535_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item535_file ]] || {
        echo "ITEM-535: отсутствует $item535_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (install-flow five blocks, install-flow-preserve seven, the three
#    physical/boot proofs one each).
item535_expectations=(
    'scripts/test-qemu-production-install-flow.sh 5'
    'scripts/test-qemu-production-install-flow-preserve.sh 7'
    'scripts/test-qemu-physical-sata-acceptance.sh 1'
    'scripts/test-qemu-physical-storage.sh 1'
    'scripts/test-qemu-boot-chain.sh 1'
)
for item535_expectation in "${item535_expectations[@]}"; do
    item535_proof=${item535_expectation% *}
    item535_minimum=${item535_expectation#* }
    for item535_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item535_count=$(grep -Fc -- "$item535_pin" "$item535_proof" || true)
        [[ $item535_count -ge $item535_minimum ]] || {
            echo "ITEM-535: в $item535_proof $item535_count вхождений" \
                "'$item535_pin', ожидалось не меньше $item535_minimum" >&2
            exit 1
        }
    done
done
# The witness is the authority on "no raw read"; this slice keeps the count of
# normalised reads per proof in step 1.

# 2. The assertions the readers make are unchanged.
for item535_marker in \
    'scripts/test-qemu-production-install-flow.sh:[PRODUCTION-INSTALL] applied' \
    'scripts/test-qemu-production-install-flow.sh:role=hfoc-persistence' \
    'scripts/test-qemu-production-install-flow.sh:[BOOT-CHAIN] verdict=ATTESTED' \
    'scripts/test-qemu-production-install-flow.sh:[APPLIANCE-BOOT] runtime=READY' \
    'scripts/test-qemu-production-install-flow-preserve.sh:[PRODUCTION-INSTALL] applied' \
    'scripts/test-qemu-production-install-flow-preserve.sh:\[APPLIANCE-BOOT\] runtime=READY identity=OK config=' \
    'scripts/test-qemu-production-install-flow-preserve.sh:\[BOOT-CHAIN\] verdict=ATTESTED' \
    'scripts/test-qemu-physical-sata-acceptance.sh:[AHCI] physical evidence proof OK' \
    'scripts/test-qemu-physical-storage.sh:[PHYSICAL-STORAGE] HFOC partition discovered' \
    'scripts/test-qemu-boot-chain.sh:[BOOT-CHAIN] verdict=ATTESTED' \
    'scripts/test-qemu-boot-chain.sh:[BOOT-CHAIN] failed:'; do
    item535_proof=${item535_marker%%:*}
    item535_text=${item535_marker#*:}
    grep -Fq -- "$item535_text" "$item535_proof" || {
        echo "ITEM-535: $item535_proof потерял утверждение:" \
            "$item535_text" >&2
        exit 1
    }
done

# 3. The witness lists these proofs, names the literal paths it must recognise
#    and keeps the class control.
item535_host=tests/serial-view-normalised-host.py
for item535_control in \
    'EXTRA_SERIAL_LITERALS = {' \
    '"scripts/test-qemu-production-install-flow.sh": (' \
    '"$TEST_DIR/language.log",' \
    '"scripts/test-qemu-production-install-flow-preserve.sh": (' \
    '"$TEST_DIR/red.log",' \
    '"$TEST_DIR/green.log",' \
    '"$TEST_DIR/installed.log",' \
    '"$TEST_DIR/release.log",' \
    'EXTRA_SERIAL_LITERALS.get(proof, ())' \
    'if not names and not literals:' \
    'scripts/test-qemu-production-install-flow.sh' \
    'scripts/test-qemu-production-install-flow-preserve.sh' \
    'scripts/test-qemu-physical-sata-acceptance.sh' \
    'scripts/test-qemu-physical-storage.sh' \
    'scripts/test-qemu-boot-chain.sh' \
    'PYTHON_HALF_PROOFS = [' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item535_control" "$item535_host" || {
        echo "ITEM-535: свидетель потерял контроль:" \
            "$item535_control" >&2
        exit 1
    }
done
for item535_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item535_witness" Makefile || {
        echo "ITEM-535: свидетель не зарегистрирован:" \
            "$item535_witness" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item535_doc in \
    'ITEM-535' \
    'install-flow' \
    'physical-storage group'; do
    grep -Fq -- "$item535_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-535:" \
            "$item535_doc" >&2
        exit 1
    }
done
