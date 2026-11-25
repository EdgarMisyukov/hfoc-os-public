#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

MATRIX=${HFOC_RELEASE_DISASTER_RECOVERY_MATRIX:-$ROOT/release/release-disaster-recovery-rehearsal.txt}
MAKE_CMD=${HFOC_MAKE_CMD:-make}
ARTIFACT_DIR=${HFOC_RELEASE_DISASTER_RECOVERY_ARTIFACT_DIR:-}

[[ -f "$MATRIX" ]] || {
    echo "Missing appliance v1 disaster-recovery rehearsal: $MATRIX" >&2
    exit 1
}

bash "$ROOT/scripts/verify-release-disaster-recovery-rehearsal.sh" "$MATRIX"

check_count=$(sed -n 's/^check_count=//p' "$MATRIX")
if [[ ! "$check_count" =~ ^[1-9][0-9]*$ ]]; then
    echo "Appliance v1 disaster-recovery rehearsal has invalid check count" >&2
    exit 1
fi

targets=()
markers=()
for ((check_index = 1; check_index <= check_count; ++check_index)); do
    target=$(sed -n "s/^check_${check_index}_target=//p" "$MATRIX")
    marker=$(sed -n "s/^check_${check_index}_marker=//p" "$MATRIX")
    if [[ -z "$target" || ! "$target" =~ ^test-[a-z0-9-]+$ ]]; then
        echo "Appliance v1 disaster-recovery rehearsal check ${check_index} has invalid target" >&2
        exit 1
    fi
    if [[ -z "$marker" ]]; then
        echo "Appliance v1 disaster-recovery rehearsal check ${check_index} has invalid marker" >&2
        exit 1
    fi
    targets+=("$target")
    markers+=("$marker")
done

if [[ -z "$ARTIFACT_DIR" ]]; then
    timestamp=$(date -u +%Y%m%dT%H%M%SZ)-$$
    ARTIFACT_DIR="$ROOT/build/test-artifacts/release-disaster-recovery-rehearsal-$timestamp"
fi
mkdir -p "$ARTIFACT_DIR"
manifest="$ARTIFACT_DIR/manifest.txt"
: >"$manifest"

for ((check_index = 0; check_index < check_count; ++check_index)); do
    target=${targets[$check_index]}
    marker=${markers[$check_index]}
    log="$ARTIFACT_DIR/$target.log"
    set +e
    "$MAKE_CMD" "$target" >"$log" 2>&1
    status=$?
    set -e
    if (( status != 0 )); then
        printf 'FAIL %s\n' "$target" >>"$manifest"
        printf 'FAILURE %s\n' "$status" >>"$manifest"
        cat "$log" >&2
        echo "Appliance v1 disaster-recovery rehearsal failed at ${target} with status ${status}" >&2
        exit "$status"
    fi
    if ! grep -Fq "$marker" "$log"; then
        printf 'FAIL %s\n' "$target" >>"$manifest"
        printf 'FAILURE missing-marker\n' >>"$manifest"
        cat "$log" >&2
        echo "Appliance v1 disaster-recovery rehearsal target ${target} lacked its exact marker" >&2
        exit 1
    fi
    printf 'PASS %s\n' "$target" >>"$manifest"
    echo "Appliance v1 disaster-recovery rehearsal: PASS $target"
done

printf 'DISASTER RECOVERY REHEARSAL OK\n' >>"$manifest"
echo "Appliance v1 disaster-recovery rehearsal artifacts: $ARTIFACT_DIR"
echo "Appliance v1 disaster-recovery rehearsal: OK"
