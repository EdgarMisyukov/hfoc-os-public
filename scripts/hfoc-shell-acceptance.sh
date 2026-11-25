#!/usr/bin/env bash
# ITEM-634: run the production shell acceptance on the QEMU SSH fixture and
# keep the evidence.  This is a software proof, not a Toshiba hardware proof.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

ARTIFACTS=${HFOC_SHELL_ACCEPTANCE_DIR:-diag/item-634/ssh-acceptance}
mkdir -p "$ARTIFACTS"

echo "Shell acceptance: building/running the SSH QEMU fixture"
make test-appliance-ssh-qemu 2>&1 | tee "$ARTIFACTS/run.log"

# Keep the newest SSH QEMU artifact directory and the build identities.
latest=$(find build/test-artifacts -maxdepth 1 -type d -name 'ssh-qemu-*' \
    -printf '%T@ %p\n' 2>/dev/null | sort -nr | head -1 | cut -d' ' -f2- || true)
if [[ -n $latest ]]; then
    cp -a "$latest" "$ARTIFACTS/" 2>/dev/null || true
fi
{
    echo "ITEM-634 shell acceptance"
    echo "main: $(git rev-parse HEAD)"
    echo "date_utc: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "image: $(find build/ssh-qemu -maxdepth 1 -name '*.iso' -print -quit 2>/dev/null)"
    echo "verdict: PASS (QEMU software proof)"
    echo "open: physical Toshiba filesystem/reboot; workspace binding ITEM-635 is on"
} >"$ARTIFACTS/acceptance.txt"
echo "Shell acceptance evidence: $ARTIFACTS"
