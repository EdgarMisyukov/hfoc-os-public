#!/usr/bin/env bash
# ITEM-380 focused host proof for the SBOM, the CVE runbook and the product
# release gate. Everything runs offline and never builds an ISO: the gate is
# driven with fixture reproduction evidence.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

SBOM_GENERATOR=scripts/generate-release-sbom.sh
SBOM_VERIFIER=scripts/verify-release-sbom.sh
CVE_VERIFIER=scripts/verify-cve-runbook.sh
GATE_VERIFIER=scripts/verify-product-release-gate.sh
RUNBOOK=docs/hfoc-security-response-runbook.md

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT
mkdir -p "$TEST_DIR/release/sbom" "$TEST_DIR/release/reproducibility"

REVISION=$(git rev-parse HEAD)
SBOM=$TEST_DIR/release/sbom/HFOC-APPLIANCE-V1.spdx
COMPARISON=$TEST_DIR/release/reproducibility/comparison-report.txt

bash "$SBOM_GENERATOR" "$SBOM" >/dev/null

kernel_digest=$(printf 'kernel' | sha256sum | awk '{print $1}')
build_input_digest=$(printf 'build-input' | sha256sum | awk '{print $1}')
digest_a=$(printf 'iso-a' | sha256sum | awk '{print $1}')
digest_b=$(printf 'iso-b' | sha256sum | awk '{print $1}')
write_comparison() {
    local iso_first=$1
    local iso_second=$2
    {
        printf 'reproducibility_report_version=1\n'
        printf 'source_revision=%s\n' "$REVISION"
        printf 'artifact_hfoc_kernel_elf_first=%s\n' "$kernel_digest"
        printf 'artifact_hfoc_kernel_elf_second=%s\n' "$kernel_digest"
        printf 'artifact_hfoc_build_input_first=%s\n' "$build_input_digest"
        printf 'artifact_hfoc_build_input_second=%s\n' "$build_input_digest"
        printf 'artifact_iso_first=%s\n' "$iso_first"
        printf 'artifact_iso_second=%s\n' "$iso_second"
        printf 'verdict=REPRODUCIBLE\n'
    } >"$COMPARISON"
}
write_comparison "$digest_a" "$digest_a"

expect_failure() {
    local label=$1
    shift
    local status=0
    set +e
    "$@" >/dev/null 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

echo '== SBOM =='
bash "$SBOM_VERIFIER" "$SBOM" "$REVISION" >/dev/null
grep -Fq 'SPDXVersion: SPDX-2.3' "$SBOM"
grep -Fq 'DataLicense: CC0-1.0' "$SBOM"
grep -Fq "DocumentNamespace: https://hfoc.invalid/spdx/HFOC-APPLIANCE-V1-$REVISION" "$SBOM"
grep -Fq 'PackageName: mbedTLS' "$SBOM"
grep -Fq 'PackageName: lwIP' "$SBOM"
grep -Fq 'PackageName: Limine' "$SBOM"
grep -Eq '^PackageLicenseConcluded: Apache-2\.0$' "$SBOM"
grep -Fq 'election=Apache-2.0' "$SBOM"
grep -Fq 'Relationship: SPDXRef-DOCUMENT DESCRIBES SPDXRef-Package-mbedtls' "$SBOM"

cp "$SBOM" "$TEST_DIR/sbom-missing-package.spdx"
sed -i '/^PackageName: lwIP$/,/^$/d' "$TEST_DIR/sbom-missing-package.spdx"
expect_failure "SBOM without the lwIP package" \
    bash "$SBOM_VERIFIER" "$TEST_DIR/sbom-missing-package.spdx" "$REVISION"

cp "$SBOM" "$TEST_DIR/sbom-no-election.spdx"
sed -i '0,/^PackageLicenseConcluded: Apache-2\.0$/s//PackageLicenseConcluded: GPL-2.0-or-later/' \
    "$TEST_DIR/sbom-no-election.spdx"
expect_failure "SBOM without the mbedTLS election" \
    bash "$SBOM_VERIFIER" "$TEST_DIR/sbom-no-election.spdx" "$REVISION"

cp "$SBOM" "$TEST_DIR/sbom-tampered-file.spdx"
sed -i '0,/^FileChecksum: SHA256: /s//FileChecksum: SHA256: 0000000000000000000000000000000000000000000000000000000000000000/' \
    "$TEST_DIR/sbom-tampered-file.spdx"
expect_failure "SBOM with a tampered file digest" \
    bash "$SBOM_VERIFIER" "$TEST_DIR/sbom-tampered-file.spdx" "$REVISION"

cp "$SBOM" "$TEST_DIR/sbom-wrong-revision.spdx"
sed -i 's/^DocumentNamespace: .*/DocumentNamespace: https:\/\/hfoc.invalid\/spdx\/HFOC-APPLIANCE-V1-0000000000000000000000000000000000000000/' \
    "$TEST_DIR/sbom-wrong-revision.spdx"
expect_failure "SBOM for another source revision" \
    bash "$SBOM_VERIFIER" "$TEST_DIR/sbom-wrong-revision.spdx" "$REVISION"

echo '== CVE runbook =='
[[ -f "$RUNBOOK" ]] || {
    echo "missing security response runbook: $RUNBOOK" >&2
    exit 1
}
bash "$CVE_VERIFIER" "$RUNBOOK" >/dev/null
cp "$RUNBOOK" "$TEST_DIR/runbook-missing-section.md"
sed -i '/^## Fix and backport$/d' "$TEST_DIR/runbook-missing-section.md"
expect_failure "runbook without the fix and backport section" \
    bash "$CVE_VERIFIER" "$TEST_DIR/runbook-missing-section.md"

echo '== release gate =='
if ! gate_output=$(bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"); then
    echo 'release gate rejected complete evidence' >&2
    exit 1
fi
grep -Fq 'Product release gate: ACCEPT' <<<"$gate_output"

rm -f "$SBOM"
expect_failure "release gate without an SBOM" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"
bash "$SBOM_GENERATOR" "$SBOM" >/dev/null

rm -f "$COMPARISON"
expect_failure "release gate without reproducibility evidence" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"

write_comparison "$digest_a" "$digest_b"
expect_failure "release gate with an unreproducible ISO digest" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"

write_comparison "$digest_a" "$digest_a"
sed -i 's/^verdict=REPRODUCIBLE/verdict=UNKNOWN/' "$COMPARISON"
expect_failure "release gate without a reproducible verdict" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"

write_comparison "$digest_a" "$digest_a"
sed -i "s/^source_revision=.*/source_revision=0000000000000000000000000000000000000000/" \
    "$COMPARISON"
expect_failure "release gate with reproducibility evidence from another revision" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"

expect_failure "release gate without the CVE runbook" \
    env HFOC_SECURITY_RUNBOOK="$TEST_DIR/missing-runbook.md" \
    bash "$GATE_VERIFIER" "$TEST_DIR" "$SBOM" "$COMPARISON"

echo 'Release gate host test: OK'
