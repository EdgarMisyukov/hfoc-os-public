#!/usr/bin/env bash
# ITEM-380 fail-closed product release gate.
#
# The gate reads the evidence a release decision needs and refuses to pass
# while any of it is missing, stale or inconsistent:
#
#   - the SPDX SBOM for the current source revision, verified by its own
#     contract (packages, licenses, the mbedTLS Apache-2.0 election and every
#     locked vendored file digest);
#   - the reproducibility evidence: two builds of the same revision produced
#     the same kernel, build-input manifest and ISO digest, recorded by
#     scripts/compare-reproducible-image-digests.sh in its comparison report
#     (reproducibility_report_version=1, source_revision == HEAD, every
#     first/second digest equal, verdict REPRODUCIBLE);
#   - the security response runbook and its verifier.
#
# It never builds anything: `make test-product-release-gate` produces the
# evidence first, and this script decides.  On success it prints
# `Product release gate: ACCEPT`; otherwise it prints the exact missing or
# inconsistent evidence and exits non-zero.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

if (( $# > 3 )); then
    echo "usage: $0 [evidence-dir] [sbom] [comparison]" >&2
    exit 2
fi

EVIDENCE_DIR=${1:-build/release}
RELEASE_ID=${HFOC_RELEASE_PROFILE_ID:-HFOC-APPLIANCE-V1}
SBOM=${2:-$EVIDENCE_DIR/sbom/$RELEASE_ID.spdx}
COMPARISON=${3:-$EVIDENCE_DIR/reproducibility/comparison-report.txt}
RUNBOOK=${HFOC_SECURITY_RUNBOOK:-docs/hfoc-security-response-runbook.md}

revision=$(git rev-parse HEAD 2>/dev/null || echo unknown)

fail() {
    echo "Product release gate: REJECT: $*" >&2
    exit 1
}

# 1. SBOM: present, current revision, and valid under its own contract.
[[ -f "$SBOM" ]] || fail "missing release SBOM: $SBOM"
grep -Fq -- "-${revision}" "$SBOM" ||
    fail "the release SBOM does not describe the current source revision"
bash "$ROOT/scripts/verify-release-sbom.sh" "$SBOM" "$revision" >/dev/null ||
    fail "the release SBOM failed its own verification"

# 2. Reproducibility evidence: the same revision produced the same artifacts.
[[ -f "$COMPARISON" ]] || fail "missing reproducibility evidence: $COMPARISON"

comparison_value() {
    local key=$1
    local value

    if [[ $(grep -c "^${key}=" "$COMPARISON") -ne 1 ]]; then
        fail "reproducibility evidence is missing ${key}"
    fi
    value=$(sed -n "s/^${key}=//p" "$COMPARISON")
    [[ -n "$value" ]] || fail "reproducibility evidence has empty ${key}"
    printf '%s' "$value"
}

require_pair() {
    local first_key=$1
    local second_key=$2
    local first
    local second

    first=$(comparison_value "$first_key")
    second=$(comparison_value "$second_key")
    [[ "$first" =~ ^[0-9a-f]{64}$ && "$second" =~ ^[0-9a-f]{64}$ ]] ||
        fail "reproducibility evidence has an invalid ${first_key}"
    [[ "$first" == "$second" ]] ||
        fail "reproducibility evidence shows ${first_key} differing between builds"
    printf '%s' "$first"
}

[[ "$(comparison_value reproducibility_report_version)" == "1" ]] ||
    fail "unsupported reproducibility evidence version"
[[ "$(comparison_value source_revision)" == "$revision" ]] ||
    fail "reproducibility evidence describes another source revision"
[[ "$(comparison_value verdict)" == "REPRODUCIBLE" ]] ||
    fail "reproducibility evidence does not record a reproducible verdict"

require_pair artifact_hfoc_kernel_elf_first artifact_hfoc_kernel_elf_second     >/dev/null
require_pair artifact_hfoc_build_input_first     artifact_hfoc_build_input_second >/dev/null
iso_digest=$(require_pair artifact_iso_first artifact_iso_second)

# 3. Security response runbook.
[[ -f "$RUNBOOK" ]] || fail "missing security response runbook: $RUNBOOK"
bash "$ROOT/scripts/verify-cve-runbook.sh" "$RUNBOOK" >/dev/null ||
    fail "the security response runbook failed its own verification"

echo "Product release gate: ACCEPT"
echo "Product release gate: release_id=${RELEASE_ID} source_revision=${revision} sbom=${SBOM} iso_sha256=${iso_digest}"
