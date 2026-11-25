#!/usr/bin/env bash
# Host regression harness for the HTTPS vertical-slice acceptance gate.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

TEST_DIR=$(mktemp -d)
export FAKE_STATE="$TEST_DIR/state"
FAKE_MAKE=$TEST_DIR/fake-make
FAKE_MATRIX=$TEST_DIR/matrix.txt
mkdir -p "$FAKE_STATE"

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

cat >"$FAKE_MAKE" <<'FAKE'
#!/usr/bin/env bash
set -Eeuo pipefail

target=${1:-}
marker=
printf 'start %s\n' "$target" >>"$FAKE_STATE/events"

case "$target" in
    test-fake-client)
        marker='fake HTTPS client OK'
        ;;
    test-fake-response)
        marker='fake HTTPS response OK'
        ;;
    test-fake-malformed)
        marker='fake HTTPS malformed OK'
        ;;
    test-fake-size)
        marker='fake HTTPS size OK'
        ;;
    test-fake-exhaustion)
        marker='fake HTTPS exhaustion OK'
        ;;
    test-fake-entropy)
        marker='fake HTTPS entropy OK'
        ;;
    test-fake-certificate)
        marker='fake HTTPS certificate OK'
        ;;
    test-fake-secret)
        marker='fake HTTPS secret ALL OK'
        ;;
    test-fake-restart)
        marker='fake HTTPS restart OK'
        ;;
    test-fake-fail)
        printf 'fake failure\n' >&2
        exit 23
        ;;
    test-fake-no-marker)
        printf 'fake %s completed without marker\n' "$target"
        printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
        exit 0
        ;;
    *)
        printf 'unknown target: %s\n' "$target" >&2
        exit 64
        ;;
esac

printf '%s\n' "$marker"
printf 'end %s\n' "$target" >>"$FAKE_STATE/events"
FAKE
chmod +x "$FAKE_MAKE"

write_matrix() {
    cat >"$FAKE_MATRIX"
}

write_full_matrix() {
    write_matrix <<'EOF'
https_vertical_slice_acceptance_version=1
target_slice_id=HFOC-HTTPS-VERTICAL-SLICE
check_count=9
check_1_id=HTTPS-CLIENT-291
check_1_category=CLIENT
check_1_name=https-client
check_1_target=test-fake-client
check_1_marker=fake HTTPS client OK
check_2_id=HTTPS-RESPONSE-292
check_2_category=RESPONSE
check_2_name=https-response
check_2_target=test-fake-response
check_2_marker=fake HTTPS response OK
check_3_id=HTTPS-MALFORMED-293
check_3_category=MALFORMED
check_3_name=https-malformed
check_3_target=test-fake-malformed
check_3_marker=fake HTTPS malformed OK
check_4_id=HTTPS-SIZE-294
check_4_category=SIZE
check_4_name=https-size
check_4_target=test-fake-size
check_4_marker=fake HTTPS size OK
check_5_id=HTTPS-EXHAUSTION-295
check_5_category=EXHAUSTION
check_5_name=https-exhaustion
check_5_target=test-fake-exhaustion
check_5_marker=fake HTTPS exhaustion OK
check_6_id=HTTPS-ENTROPY-296
check_6_category=ENTROPY
check_6_name=https-entropy
check_6_target=test-fake-entropy
check_6_marker=fake HTTPS entropy OK
check_7_id=HTTPS-CERTIFICATE-297
check_7_category=CERTIFICATE
check_7_name=https-certificate
check_7_target=test-fake-certificate
check_7_marker=fake HTTPS certificate OK
check_8_id=HTTPS-SECRET-298
check_8_category=SECRET
check_8_name=https-secret
check_8_target=test-fake-secret
check_8_marker=fake HTTPS secret ALL OK
check_9_id=HTTPS-RESTART-299
check_9_category=RESTART
check_9_name=https-restart
check_9_target=test-fake-restart
check_9_marker=fake HTTPS restart OK
EOF
}

run_verifier() {
    bash scripts/verify-https-vertical-slice-acceptance.sh "$FAKE_MATRIX"
}

run_runner() {
    HFOC_HTTPS_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
    HFOC_MAKE_CMD="$FAKE_MAKE" \
    HFOC_HTTPS_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
        bash scripts/test-https-vertical-slice-acceptance.sh
}

expect_failure() {
    local label=$1
    local status=0
    shift

    set +e
    "$@" >/dev/null 2>&1
    status=$?
    set -e
    if (( status == 0 )); then
        echo "$label unexpectedly succeeded" >&2
        exit 1
    fi
}

write_full_matrix
if ! verifier_output=$(run_verifier); then
    echo 'canonical HTTPS vertical-slice acceptance verification failed' >&2
    exit 1
fi
grep -Fq 'HTTPS vertical-slice acceptance gate verification: OK' \
    <<<"$verifier_output"

expect_failure "missing HTTPS vertical-slice acceptance matrix" \
    bash scripts/verify-https-vertical-slice-acceptance.sh \
    "$TEST_DIR/missing.txt"

write_full_matrix
sed -i 's/^https_vertical_slice_acceptance_version=1/\
https_vertical_slice_acceptance_version=2/' "$FAKE_MATRIX"
expect_failure "unsupported HTTPS vertical-slice acceptance version" \
    run_verifier

write_full_matrix
sed -i 's/^check_2_category=RESPONSE/\
check_2_category=CLIENT/' "$FAKE_MATRIX"
expect_failure "missing HTTPS vertical-slice acceptance category" \
    run_verifier

write_full_matrix
sed -i -e 's/^check_1_category=CLIENT/\
check_1_category=RESPONSE/' -e 's/^check_2_category=RESPONSE/\
check_2_category=CLIENT/' "$FAKE_MATRIX"
expect_failure "reordered HTTPS vertical-slice acceptance category" \
    run_verifier

write_full_matrix
sed -i 's/^check_2_id=HTTPS-RESPONSE-292/\
check_2_id=HTTPS-CLIENT-291/' "$FAKE_MATRIX"
expect_failure "duplicate HTTPS vertical-slice acceptance check ID" \
    run_verifier

write_full_matrix
sed -i 's/^check_1_target=test-fake-client/\
check_1_target=not-a-test/' "$FAKE_MATRIX"
expect_failure "malformed HTTPS vertical-slice acceptance target" \
    run_verifier

write_full_matrix
cat >>"$FAKE_MATRIX" <<'EOF'
check_10_id=HTTPS-UNKNOWN-300
check_10_category=CLIENT
check_10_name=https-unknown
check_10_target=test-fake-client
check_10_marker=fake HTTPS client OK
EOF
expect_failure "undeclared HTTPS vertical-slice acceptance check" \
    run_verifier

write_full_matrix
sed -i 's/^check_1_marker=fake HTTPS client OK/\
check_1_marker=fake HTTPS client OK!/' "$FAKE_MATRIX"
expect_failure "malformed HTTPS vertical-slice acceptance marker" \
    run_verifier

: >"$FAKE_STATE/events"
write_full_matrix
if ! runner_output=$(run_runner); then
    echo 'HTTPS vertical-slice acceptance success case failed' >&2
    exit 1
fi
grep -Fq 'HTTPS vertical-slice acceptance gate: ACCEPT' <<<"$runner_output"
for target in test-fake-client test-fake-response test-fake-malformed \
              test-fake-size test-fake-exhaustion test-fake-entropy \
              test-fake-certificate test-fake-secret test-fake-restart; do
    grep -Fxq "start $target" "$FAKE_STATE/events"
    grep -Fxq "end $target" "$FAKE_STATE/events"
done
[[ "$(grep -c '^start ' "$FAKE_STATE/events")" == 9 ]] || {
    echo 'HTTPS vertical-slice acceptance did not invoke exactly nine stages' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
}
grep -Fq 'PASS test-fake-client' \
    "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'PASS test-fake-restart' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'HTTPS VERTICAL-SLICE ACCEPTANCE OK' \
    "$TEST_DIR/artifacts/manifest.txt"

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-response/\
check_2_target=test-fake-fail/' "$FAKE_MATRIX"
set +e
HFOC_HTTPS_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_HTTPS_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-https-vertical-slice-acceptance.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'HTTPS vertical-slice acceptance failure case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-client' "$FAKE_STATE/events"
grep -Fxq 'start test-fake-fail' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-malformed' "$FAKE_STATE/events"; then
    echo 'HTTPS vertical-slice acceptance started a queued stage after failure' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
[[ -f "$TEST_DIR/artifacts/manifest.txt" ]] || {
    echo 'HTTPS vertical-slice acceptance did not preserve failure manifest' >&2
    exit 1
}
grep -Fq 'FAIL test-fake-fail' "$TEST_DIR/artifacts/manifest.txt"
[[ -f "$TEST_DIR/artifacts/test-fake-fail.log" ]] || {
    echo 'HTTPS vertical-slice acceptance did not preserve failure log' >&2
    exit 1
}

rm -rf "$TEST_DIR/artifacts"
: >"$FAKE_STATE/events"
write_full_matrix
sed -i 's/^check_2_target=test-fake-response/\
check_2_target=test-fake-no-marker/' "$FAKE_MATRIX"
set +e
HFOC_HTTPS_ACCEPTANCE_MATRIX="$FAKE_MATRIX" \
HFOC_MAKE_CMD="$FAKE_MAKE" \
HFOC_HTTPS_ACCEPTANCE_ARTIFACT_DIR="$TEST_DIR/artifacts" \
    bash scripts/test-https-vertical-slice-acceptance.sh >/dev/null 2>&1
runner_status=$?
set -e
if (( runner_status == 0 )); then
    echo 'HTTPS vertical-slice acceptance missing-marker case unexpectedly succeeded' >&2
    exit 1
fi
grep -Fxq 'start test-fake-no-marker' "$FAKE_STATE/events"
if grep -Fxq 'start test-fake-malformed' "$FAKE_STATE/events"; then
    echo 'HTTPS vertical-slice acceptance continued after missing marker' >&2
    cat "$FAKE_STATE/events" >&2
    exit 1
fi
grep -Fq 'FAIL test-fake-no-marker' "$TEST_DIR/artifacts/manifest.txt"
grep -Fq 'FAILURE missing-marker' "$TEST_DIR/artifacts/manifest.txt"

echo 'HTTPS vertical-slice acceptance gate host test: OK'
