#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${HFOC_HTTPS_ACCEPTANCE_MATRIX:-$ROOT/release/https-vertical-slice-acceptance.txt}
MAKE_CMD=${HFOC_MAKE_CMD:-make}
ARTIFACT_DIR=${HFOC_HTTPS_ACCEPTANCE_ARTIFACT_DIR:-}

[[ -f "$MATRIX" ]] || {
    echo "Missing HTTPS vertical-slice acceptance matrix: $MATRIX" >&2
    exit 1
}

bash "$ROOT/scripts/verify-https-vertical-slice-acceptance.sh" "$MATRIX"

check_count=$(sed -n 's/^check_count=//p' "$MATRIX")
if [[ ! "$check_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "HTTPS vertical-slice acceptance matrix has invalid check count" >&2
    exit 1
fi

targets=()
markers=()
for ((check_index = 1; check_index <= check_count; ++check_index)); do
    target=$(sed -n "s/^check_${check_index}_target=//p" "$MATRIX")
    marker=$(sed -n "s/^check_${check_index}_marker=//p" "$MATRIX")
    if [[ -z "$target" || ! "$target" =~ ^test-[a-z0-9-]+$ ]]; then
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid target" >&2
        exit 1
    fi
    if [[ -z "$marker" ]]; then
        echo "HTTPS vertical-slice acceptance matrix check ${check_index} has invalid marker" >&2
        exit 1
    fi
    targets+=("$target")
    markers+=("$marker")
done

if [[ -z "$ARTIFACT_DIR" ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    ARTIFACT_DIR="$ROOT/build/test-artifacts/https-vertical-slice-acceptance-$timestamp"
fi
mkdir -p "$ARTIFACT_DIR"
manifest="$ARTIFACT_DIR/manifest.txt"

if HFOC_MAKE_CMD="$MAKE_CMD" bash "$ROOT/scripts/run-acceptance-suite.sh" \
    --artifact-dir "$ARTIFACT_DIR" \
    --serial "${targets[0]}" "${markers[0]}" \
    --serial "${targets[1]}" "${markers[1]}" \
    --serial "${targets[2]}" "${markers[2]}" \
    --serial "${targets[3]}" "${markers[3]}" \
    --serial "${targets[4]}" "${markers[4]}" \
    --serial "${targets[5]}" "${markers[5]}" \
    --serial "${targets[6]}" "${markers[6]}" \
    --serial "${targets[7]}" "${markers[7]}" \
    --serial "${targets[8]}" "${markers[8]}"; then
    :
else
    status=$?
    echo "HTTPS vertical-slice acceptance gate: REJECT" >&2
    exit "$status"
fi

printf 'HTTPS VERTICAL-SLICE ACCEPTANCE OK\n' >>"$manifest"
echo "HTTPS vertical-slice acceptance gate artifacts: $ARTIFACT_DIR"
echo "HTTPS vertical-slice acceptance gate: ACCEPT"
