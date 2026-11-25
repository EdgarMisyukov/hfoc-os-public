#!/usr/bin/env bash
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
trap 'rm -rf "$TEST_DIR"' EXIT

CANONICAL="$ROOT/release/release-backup-restore-matrix.txt"
if [[ ! -f "$CANONICAL" ]]; then
    echo "Missing canonical release backup/restore matrix: $CANONICAL" >&2
    exit 1
fi

cp "$CANONICAL" "$TEST_DIR/canonical.txt"
bash scripts/verify-release-backup-restore-matrix.sh \
    "$TEST_DIR/canonical.txt" >/dev/null

expect_fail() {
    local matrix=$1
    local message=$2

    if bash scripts/verify-release-backup-restore-matrix.sh \
            "$matrix" >/dev/null 2>&1; then
        echo "$message" >&2
        exit 1
    fi
}

if bash scripts/verify-release-backup-restore-matrix.sh \
        "$TEST_DIR/missing.txt" >/dev/null 2>&1; then
    echo 'release backup/restore matrix accepted a missing matrix' >&2
    exit 1
fi

cp "$CANONICAL" "$TEST_DIR/missing-field.txt"
sed -i '/^target_release_id=/d' "$TEST_DIR/missing-field.txt"
expect_fail "$TEST_DIR/missing-field.txt" \
    'release backup/restore matrix accepted a missing header field'

cp "$CANONICAL" "$TEST_DIR/version.txt"
sed -i 's/^release_backup_restore_matrix_version=1/release_backup_restore_matrix_version=2/' \
    "$TEST_DIR/version.txt"
expect_fail "$TEST_DIR/version.txt" \
    'release backup/restore matrix accepted an unsupported version'

cp "$CANONICAL" "$TEST_DIR/identity.txt"
sed -i 's/^target_release_id=.*/target_release_id=OTHER/' \
    "$TEST_DIR/identity.txt"
expect_fail "$TEST_DIR/identity.txt" \
    'release backup/restore matrix accepted a release identity mismatch'

cp "$CANONICAL" "$TEST_DIR/allowed-state-schema.txt"
sed -i 's/^path_1_source_state_schema_min=1/path_1_source_state_schema_min=2/' \
    "$TEST_DIR/allowed-state-schema.txt"
sed -i 's/^path_1_source_state_schema_max=1/path_1_source_state_schema_max=2/' \
    "$TEST_DIR/allowed-state-schema.txt"
expect_fail "$TEST_DIR/allowed-state-schema.txt" \
    'release backup/restore matrix accepted an incompatible allowed state schema'

cp "$CANONICAL" "$TEST_DIR/allowed-stream-format.txt"
sed -i 's/^path_1_source_backup_stream_format_min=1/path_1_source_backup_stream_format_min=2/' \
    "$TEST_DIR/allowed-stream-format.txt"
sed -i 's/^path_1_source_backup_stream_format_max=1/path_1_source_backup_stream_format_max=2/' \
    "$TEST_DIR/allowed-stream-format.txt"
expect_fail "$TEST_DIR/allowed-stream-format.txt" \
    'release backup/restore matrix accepted an unsupported allowed stream format'

cp "$CANONICAL" "$TEST_DIR/rejected-restore.txt"
sed -i 's/^path_3_restore=no/path_3_restore=yes/' \
    "$TEST_DIR/rejected-restore.txt"
expect_fail "$TEST_DIR/rejected-restore.txt" \
    'release backup/restore matrix accepted restore on a rejected path'

cp "$CANONICAL" "$TEST_DIR/rejected-rollback.txt"
sed -i 's/^path_3_rollback=no/path_3_rollback=yes/' \
    "$TEST_DIR/rejected-rollback.txt"
expect_fail "$TEST_DIR/rejected-rollback.txt" \
    'release backup/restore matrix accepted rollback on a rejected path'

cp "$CANONICAL" "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_3_source_state_schema_min=2/path_3_source_state_schema_min=1/' \
    "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_3_source_state_schema_max=2/path_3_source_state_schema_max=1/' \
    "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_4_source_backup_stream_format_min=2/path_4_source_backup_stream_format_min=1/' \
    "$TEST_DIR/rejected-compatible.txt"
sed -i 's/^path_4_source_backup_stream_format_max=2/path_4_source_backup_stream_format_max=1/' \
    "$TEST_DIR/rejected-compatible.txt"
expect_fail "$TEST_DIR/rejected-compatible.txt" \
    'release backup/restore matrix accepted a compatible rejected path'

cp "$CANONICAL" "$TEST_DIR/bad-boolean.txt"
sed -i 's/^path_1_backup=yes/path_1_backup=maybe/' \
    "$TEST_DIR/bad-boolean.txt"
expect_fail "$TEST_DIR/bad-boolean.txt" \
    'release backup/restore matrix accepted a malformed boolean'

cp "$CANONICAL" "$TEST_DIR/missing-path-field.txt"
sed -i '/^path_2_source_state_schema_min=/d' \
    "$TEST_DIR/missing-path-field.txt"
expect_fail "$TEST_DIR/missing-path-field.txt" \
    'release backup/restore matrix accepted a missing path field'

cp "$CANONICAL" "$TEST_DIR/count-mismatch.txt"
sed -i 's/^path_count=4/path_count=3/' "$TEST_DIR/count-mismatch.txt"
expect_fail "$TEST_DIR/count-mismatch.txt" \
    'release backup/restore matrix accepted a path-count mismatch'

echo 'Release backup/restore matrix host test: OK'
