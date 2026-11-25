#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
FAKE_MAKE=$TEST_DIR/fake-make
trap 'rm -rf "$TEST_DIR"' EXIT

cat >"$FAKE_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

[[ $# == 1 ]] || exit 64
target=$1
marker=$(awk -F= -v target="$target" '
    $2 == target { want = substr($1, 1, length($1) - 7) }
    want != "" && $1 == want "_marker" { print $2; exit }
' release/release-appliance-v1-acceptance.txt \
    release/https-vertical-slice-acceptance.txt)
[[ -n "$marker" ]] || exit 64
printf '%s\n' "$marker"
FAKE
chmod +x "$FAKE_MAKE"

for wrapper in \
    scripts/test-release-appliance-v1-acceptance.sh \
    scripts/test-https-vertical-slice-acceptance.sh; do
    grep -Fq 'scripts/run-acceptance-suite.sh' "$wrapper" || {
        echo "$wrapper does not invoke the acceptance runner" >&2
        exit 1
    }
done

assert_manifest_equivalent() {
    local matrix=$1
    local artifact_dir=$2
    local expected actual

    expected=$TEST_DIR/expected.txt
    actual=$TEST_DIR/actual.txt
    sed -n 's/^check_[0-9][0-9]*_target=//p' "$matrix" >"$expected"
    sed -n 's/^PASS //p' "$artifact_dir/manifest.txt" >"$actual"
    diff -u "$expected" "$actual"
}

APPLIANCE_ARTIFACT_DIR=$TEST_DIR/appliance-artifacts
HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_ACCEPTANCE_JOBS=1 \
    HFOC_APPLIANCE_V1_ACCEPTANCE_ARTIFACT_DIR="$APPLIANCE_ARTIFACT_DIR" \
    bash scripts/test-release-appliance-v1-acceptance.sh
assert_manifest_equivalent release/release-appliance-v1-acceptance.txt \
    "$APPLIANCE_ARTIFACT_DIR"

HTTPS_ARTIFACT_DIR=$TEST_DIR/https-artifacts
HFOC_MAKE_CMD="$FAKE_MAKE" HFOC_ACCEPTANCE_JOBS=2 \
    HFOC_HTTPS_ACCEPTANCE_ARTIFACT_DIR="$HTTPS_ARTIFACT_DIR" \
    bash scripts/test-https-vertical-slice-acceptance.sh
assert_manifest_equivalent release/https-vertical-slice-acceptance.txt \
    "$HTTPS_ARTIFACT_DIR"

echo 'Acceptance manifest equivalence host test: OK'
