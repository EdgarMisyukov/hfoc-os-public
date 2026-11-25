#!/usr/bin/env bash
# Static regression for the post-acceptance QEMU duplicate policy.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

fail() {
    echo "QEMU duplicate policy regression: $*" >&2
    exit 1
}

extract_block_targets() {
    local start=$1
    local file=$2
    awk -v start="$start" '
        $0 ~ start { in_block = 1; next }
        in_block && /^[[:space:]]*$/ { exit }
        in_block {
            gsub(/[[:space:]\\]/, "")
            if ($0 != "") print
        }
    ' "$file" | sort -u
}

extract_release_targets() {
    sed -n 's/^check_[0-9][0-9]*_target=//p' \
        release/release-*.txt \
        release/https-vertical-slice-acceptance.txt | sort -u
}

grep -Fq 'test-qemu-parallel-after-acceptance:' Makefile ||
    fail "Makefile не содержит after-acceptance QEMU target"
grep -Fq 'QEMU_ACCEPTANCE_PROVEN_TARGETS :=' Makefile ||
    fail "Makefile не содержит QEMU_ACCEPTANCE_PROVEN_TARGETS"
grep -Fq 'QEMU_PARALLEL_RERUN_TARGETS :=' Makefile ||
    fail "Makefile не содержит QEMU_PARALLEL_RERUN_TARGETS"
grep -Fq '$(MAKE) test-qemu-parallel-after-acceptance' Makefile ||
    fail "test recipe не вызывает after-acceptance QEMU target"
grep -Fq \
    '$(filter-out $(QEMU_ACCEPTANCE_PROVEN_TARGETS),$(QEMU_PARALLEL_PHASE_1))' \
    Makefile ||
    fail "QEMU_PARALLEL_RERUN_TARGETS не фильтрует acceptance-proven targets"

if ! awk '
    /^test-qemu-parallel-after-acceptance:/ { in_recipe = 1; next }
    in_recipe && /^[^[:space:]#]/ { exit }
    in_recipe && /QEMU_PARALLEL_RERUN_TARGETS/ { found = 1 }
    END { exit found ? 0 : 1 }
' Makefile; then
    fail "after-acceptance target не использует QEMU_PARALLEL_RERUN_TARGETS"
fi

if ! awk '
    /^test-qemu-parallel:/ { in_recipe = 1; next }
    in_recipe && /^[^[:space:]#]/ { exit }
    in_recipe && /QEMU_PARALLEL_PHASE_1/ { found = 1 }
    END { exit found ? 0 : 1 }
' Makefile; then
    fail "full test-qemu-parallel target не использует QEMU_PARALLEL_PHASE_1"
fi

expected_duplicates=(
    test-accounting-boundary-qemu
    test-backup-restore-qemu
    test-boot-policy-regression
    test-entropy-fail-closed-matrix-qemu
    test-https-client-qemu
    test-https-exhaustion-matrix-qemu
    test-https-malformed-request-matrix-qemu
    test-https-response-matrix-qemu
    test-https-restart-soak-qemu
    test-https-size-boundary-matrix-qemu
    test-supportability-qemu
)

proven_targets=$(extract_block_targets '^QEMU_ACCEPTANCE_PROVEN_TARGETS :=' Makefile)
if ! diff -u \
    <(printf '%s\n' "${expected_duplicates[@]}") \
    <(printf '%s\n' "$proven_targets") >/dev/null; then
    fail "QEMU_ACCEPTANCE_PROVEN_TARGETS не равен аудированному списку"
fi

release_targets=$(extract_release_targets)
phase_targets=$(extract_block_targets '^QEMU_PARALLEL_PHASE_1 :=' Makefile)
intersection=$(comm -12 \
    <(printf '%s\n' "$phase_targets") \
    <(printf '%s\n' "$release_targets"))

if ! diff -u \
    <(printf '%s\n' "${expected_duplicates[@]}") \
    <(printf '%s\n' "$intersection") >/dev/null; then
    fail "release/QEMU intersection не равен аудированному списку"
fi

echo "QEMU duplicate policy regression: OK"
