#!/usr/bin/env bash
# ITEM-372 signed boot chain and boot attestation proof.
#
# Boot 1: the signed production ISO.  The ordinary production kernel verifies
# the loader-loaded kernel and configuration against the signed boot-chain
# manifest with real ECDSA P-256 verification and publishes ATTESTED with
# readable measurements.
# Boot 2: the macro-gated boot-chain proof ISO.  The proof verifies the
# shared component vector, tampers the kernel component and the signature,
# and binds the refused attestation into the production boot coordinator,
# which must refuse NORMAL mode with APPLIANCE_PRODUCTION_BOOT_UNATTESTED.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_BOOT_CHAIN_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_BOOT_CHAIN_TEST_TIMEOUT:-240}s" \
        env HFOC_BOOT_CHAIN_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
PRODUCTION_IMAGE=${PRODUCTION_IMAGE:-build/hfoc-os-$(<VERSION).iso}
PROOF_IMAGE=${PROOF_IMAGE:-build/boot-chain/hfoc-os-$(<VERSION)-boot-chain.iso}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Boot-chain test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Boot-chain test must set QEMU_SMP=1" >&2
    exit 1
}
test -f "$PRODUCTION_IMAGE" || {
    echo "Boot-chain test requires image: $PRODUCTION_IMAGE" >&2
    exit 1
}
test -f "$PROOF_IMAGE" || {
    echo "Boot-chain test requires image: $PROOF_IMAGE" >&2
    exit 1
}
echo "QEMU boot-chain baseline: machine=q35 smp=1 cpu=default"

TEST_DIR=$(mktemp -d)
FAILURE_REASON=$TEST_DIR/failure-reason.txt
QEMU_PID=
FAILURE_MESSAGE=
CURRENT_LOG=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        mkdir -p build/test-artifacts
        artifact_dir="build/test-artifacts/boot-chain-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        printf '%s\n' "${FAILURE_MESSAGE:-boot-chain QEMU proof failed}" \
            >"$FAILURE_REASON"
        [[ -n "$CURRENT_LOG" && -f "$CURRENT_LOG" ]] &&
            cp -a "$CURRENT_LOG" "$artifact_dir/"
        cp -a "$FAILURE_REASON" "$artifact_dir/"
        echo "Boot-chain artifacts: $artifact_dir" >&2
    fi
    if [[ -n "$QEMU_PID" ]] && kill -0 "$QEMU_PID" 2>/dev/null; then
        kill -TERM "$QEMU_PID" 2>/dev/null
        for _ in $(seq 1 20); do
            kill -0 "$QEMU_PID" 2>/dev/null || break
            sleep 0.2
        done
        kill -KILL "$QEMU_PID" 2>/dev/null
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT INT TERM

wait_for_marker() {
    local log=$1
    local marker=$2
    local deadline=$3

    python3 - "$log" "$marker" "$deadline" \
    "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

log, marker, deadline = sys.argv[1], sys.argv[2], float(sys.argv[3])
failure = ("[BOOT-CHAIN] failed:", "Kernel panic", "PANIC")
end = time.time() + deadline
while time.time() < end:
    try:
        with open(log, "rb") as handle:
            data = SERIAL_VIEW.normalise(
                handle.read().decode("utf-8", "replace"))
    except FileNotFoundError:
        data = ""
    if marker in data:
        sys.exit(0)
    for text in failure:
        if text in data:
            print("guest failure: %s" % text, file=sys.stderr)
            sys.exit(1)
    time.sleep(0.2)
print("marker not observed: %s" % marker, file=sys.stderr)
sys.exit(1)
PY
}

boot_image() {
    local image=$1
    local log=$2
    local marker=$3
    local deadline=$4

    : >"$log"
    CURRENT_LOG=$log
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m 512M -smp "$QEMU_SMP" \
        -display none -no-reboot -cdrom "$image" \
        -serial "file:$log" >/dev/null 2>&1 &
    QEMU_PID=$!
    if ! wait_for_marker "$log" "$marker" "$deadline"; then
        FAILURE_MESSAGE="marker not observed in $image: $marker"
        return 1
    fi
    kill -TERM "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
    QEMU_PID=
}

boot_image "$PRODUCTION_IMAGE" "$TEST_DIR/production.log" \
    "[BOOT-CHAIN] verdict=ATTESTED reason=NONE release=1 signer=1 components=2" \
    90 || exit 1
echo "signed production ISO boot: attested"

boot_image "$PROOF_IMAGE" "$TEST_DIR/proof.log" \
    "[BOOT-CHAIN] proof OK" 90 || exit 1
for marker in \
    '[BOOT-CHAIN] measured boot register proof OK' \
    '[BOOT-CHAIN] tamper refusal proof OK' \
    '[BOOT-CHAIN] asymmetric refusal proof OK' \
    '[BOOT-CHAIN] production boot refusal proof OK'; do
    if [[ $(grep -Fc -- "$marker" "$TEST_DIR/proof.log") -ne 1 ]]; then
        FAILURE_MESSAGE="proof marker missing or repeated: $marker"
        exit 1
    fi
done
grep -Fq -- '[BOOT-CHAIN] status=APPLIANCE_PRODUCTION_BOOT_UNATTESTED mode=FAIL_CLOSED' \
    "$TEST_DIR/proof.log" || {
    FAILURE_MESSAGE="refused attestation did not refuse production boot"
    exit 1
}

echo "QEMU production boot chain test: OK"
