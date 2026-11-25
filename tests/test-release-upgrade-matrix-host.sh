#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

CANONICAL="$ROOT/release/release-upgrade-matrix.txt"
if [[ ! -f "$CANONICAL" ]]; then
    echo "Missing canonical release upgrade matrix: $CANONICAL" >&2
    exit 1
fi

cp "$CANONICAL" "$TEST_DIR/canonical.txt"
bash scripts/verify-release-upgrade-matrix.sh \
    "$TEST_DIR/canonical.txt" >/dev/null

expect_fail() {
    local matrix=$1
    local message=$2

    if bash scripts/verify-release-upgrade-matrix.sh \
            "$matrix" >/dev/null 2>&1; then
        echo "$message" >&2
        exit 1
    fi
}

if bash scripts/verify-release-upgrade-matrix.sh \
        "$TEST_DIR/missing.txt" >/dev/null 2>&1; then
    echo 'release upgrade matrix accepted a missing matrix' >&2
    exit 1
fi

cp "$CANONICAL" "$TEST_DIR/missing-field.txt"
sed -i '/^target_release_id=/d' "$TEST_DIR/missing-field.txt"
expect_fail "$TEST_DIR/missing-field.txt" \
    'release upgrade matrix accepted a missing header field'

cp "$CANONICAL" "$TEST_DIR/version.txt"
sed -i 's/^release_upgrade_matrix_version=1/release_upgrade_matrix_version=2/' \
    "$TEST_DIR/version.txt"
expect_fail "$TEST_DIR/version.txt" \
    'release upgrade matrix accepted an unsupported version'

cp "$CANONICAL" "$TEST_DIR/identity.txt"
sed -i 's/^target_release_id=.*/target_release_id=OTHER/' \
    "$TEST_DIR/identity.txt"
expect_fail "$TEST_DIR/identity.txt" \
    'release upgrade matrix accepted a release identity mismatch'

cp "$CANONICAL" "$TEST_DIR/allowed-schema.txt"
sed -i 's/^path_1_source_schema_min=1/path_1_source_schema_min=2/' \
    "$TEST_DIR/allowed-schema.txt"
expect_fail "$TEST_DIR/allowed-schema.txt" \
    'release upgrade matrix accepted an incompatible allowed path'

cp "$CANONICAL" "$TEST_DIR/allowed-target.txt"
sed -i 's/^path_1_target_image_id=2/path_1_target_image_id=99/' \
    "$TEST_DIR/allowed-target.txt"
expect_fail "$TEST_DIR/allowed-target.txt" \
    'release upgrade matrix accepted an unknown allowed target'

cp "$CANONICAL" "$TEST_DIR/rejected-rollback.txt"
sed -i 's/^path_3_rollback=no/path_3_rollback=yes/' \
    "$TEST_DIR/rejected-rollback.txt"
expect_fail "$TEST_DIR/rejected-rollback.txt" \
    'release upgrade matrix accepted rollback on a rejected path'

cp "$CANONICAL" "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_3_source_schema_min=2/path_3_source_schema_min=1/' \
    "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_3_source_schema_max=2/path_3_source_schema_max=1/' \
    "$TEST_DIR/rejected-compatible.txt"
expect_fail "$TEST_DIR/rejected-compatible.txt" \
    'release upgrade matrix accepted a compatible rejected path'

cp "$CANONICAL" "$TEST_DIR/bad-boolean.txt"
sed -i 's/^path_1_forward=yes/path_1_forward=maybe/' \
    "$TEST_DIR/bad-boolean.txt"
expect_fail "$TEST_DIR/bad-boolean.txt" \
    'release upgrade matrix accepted a malformed boolean'

cp "$CANONICAL" "$TEST_DIR/missing-path-field.txt"
sed -i '/^path_2_source_schema_min=/d' \
    "$TEST_DIR/missing-path-field.txt"
expect_fail "$TEST_DIR/missing-path-field.txt" \
    'release upgrade matrix accepted a missing path field'

cp "$CANONICAL" "$TEST_DIR/count-mismatch.txt"
sed -i 's/^path_count=4/path_count=3/' "$TEST_DIR/count-mismatch.txt"
expect_fail "$TEST_DIR/count-mismatch.txt" \
    'release upgrade matrix accepted a path-count mismatch'

echo 'Release upgrade matrix host test: OK'
