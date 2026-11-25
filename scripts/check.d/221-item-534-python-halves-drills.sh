#!/usr/bin/env bash
# ITEM-534: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529…533 converted the python halves of the proofs the witness covers and
# of the panic/fault, input/console, https matrix and installer groups.  The
# remaining group starts with the https client, production-network and the four
# failure drills plus the supervisor composition: each proof reads the same
# serial file from embedded python and matches its records against text a
# periodic line can split.  Their shell halves have no serial assertion, so they
# are listed in the witness's `PYTHON_HALF_PROOFS`.
#
# This slice pins the conversion of those seven readers:
#   * each proof hands the shared renderer to its python block and normalises
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
item534_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item534_root" || exit 1

item534_proofs=(
    scripts/test-qemu-https-client.sh
    scripts/test-qemu-production-network.sh
    scripts/test-qemu-trust-failure-drill.sh
    scripts/test-qemu-update-failure-drill.sh
    scripts/test-qemu-network-partition-drill.sh
    scripts/test-qemu-integration-replay-outage-drill.sh
    scripts/test-qemu-supervisor-composition.sh
)
for item534_file in "${item534_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item534_file ]] || {
        echo "ITEM-534: отсутствует $item534_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (one python block each).
item534_expectations=(
    'scripts/test-qemu-https-client.sh 1'
    'scripts/test-qemu-production-network.sh 1'
    'scripts/test-qemu-trust-failure-drill.sh 1'
    'scripts/test-qemu-update-failure-drill.sh 1'
    'scripts/test-qemu-network-partition-drill.sh 1'
    'scripts/test-qemu-integration-replay-outage-drill.sh 1'
    'scripts/test-qemu-supervisor-composition.sh 1'
)
for item534_expectation in "${item534_expectations[@]}"; do
    item534_proof=${item534_expectation% *}
    item534_minimum=${item534_expectation#* }
    for item534_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item534_count=$(grep -Fc -- "$item534_pin" "$item534_proof" || true)
        [[ $item534_count -ge $item534_minimum ]] || {
            echo "ITEM-534: в $item534_proof $item534_count вхождений" \
                "'$item534_pin', ожидалось не меньше $item534_minimum" >&2
            exit 1
        }
    done
done
# The witness is the authority on "no raw read"; this slice keeps the count of
# normalised reads per proof in step 1.

# 2. The assertions the readers make are unchanged.
for item534_marker in \
    'scripts/test-qemu-https-client.sh:[HTTPS-QEMU] listener live' \
    'scripts/test-qemu-production-network.sh:[PRODUCTION-NET] device=PRESENT readiness=READY' \
    'scripts/test-qemu-trust-failure-drill.sh:[TRUST-FAILURE-DRILL] known-good trust state preserved' \
    'scripts/test-qemu-update-failure-drill.sh:[UPDATE-FAILURE-DRILL] known-good retained after staged fault' \
    'scripts/test-qemu-network-partition-drill.sh:[NETWORK-PARTITION-DRILL] local operation preserved' \
    'scripts/test-qemu-integration-replay-outage-drill.sh:[INTEGRATION-REPLAY-OUTAGE-DRILL] local paths unaffected' \
    'scripts/test-qemu-supervisor-composition.sh:[APP-HTTPS] contained restart lifecycle'; do
    item534_proof=${item534_marker%%:*}
    item534_text=${item534_marker#*:}
    grep -Fq -- "$item534_text" "$item534_proof" || {
        echo "ITEM-534: $item534_proof потерял утверждение:" \
            "$item534_text" >&2
        exit 1
    }
done

# 3. The witness lists these proofs and keeps the class control.
item534_host=tests/serial-view-normalised-host.py
for item534_control in \
    'PYTHON_HALF_PROOFS = [' \
    'scripts/test-qemu-https-client.sh' \
    'scripts/test-qemu-production-network.sh' \
    'scripts/test-qemu-trust-failure-drill.sh' \
    'scripts/test-qemu-update-failure-drill.sh' \
    'scripts/test-qemu-network-partition-drill.sh' \
    'scripts/test-qemu-integration-replay-outage-drill.sh' \
    'scripts/test-qemu-supervisor-composition.sh' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item534_control" "$item534_host" || {
        echo "ITEM-534: свидетель потерял контроль:" \
            "$item534_control" >&2
        exit 1
    }
done
for item534_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item534_witness" Makefile || {
        echo "ITEM-534: свидетель не зарегистрирован:" \
            "$item534_witness" >&2
        exit 1
    }
done

# 4. The runner document owns the batch.
for item534_doc in \
    'ITEM-534' \
    'https client' \
    'drill group'; do
    grep -Fq -- "$item534_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-534:" \
            "$item534_doc" >&2
        exit 1
    }
done
