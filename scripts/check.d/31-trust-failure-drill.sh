#!/usr/bin/env bash
# ITEM-387: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Trust failure drill contracts.
grep -Fq 'test-trust-failure-drill-host' Makefile || {
    echo "Makefile не содержит test-trust-failure-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-trust-failure-drill-host' Makefile || {
    echo "test: target не содержит test-trust-failure-drill-host" >&2
    exit 1
}
grep -Fq 'test-trust-failure-drill-qemu' Makefile || {
    echo "Makefile не содержит test-trust-failure-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'trust_failure_drill_qemu_test_init'; then
    echo "Production kernel содержит trust-failure-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[TRUST-FAILURE-DRILL]'; then
    echo "Production kernel содержит trust-failure-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-trust-failure-drill.sh' Makefile || {
    echo "Makefile не вызывает trust-failure-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU trust failure baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-trust-failure-drill.sh || {
    echo "test-qemu-trust-failure-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU trust failure drill test: OK' \
    scripts/test-qemu-trust-failure-drill.sh || {
    echo "test-qemu-trust-failure-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-trust-failure-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит trust-failure-drill script" >&2
    exit 1
fi
for trust_failure_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'trust failure drill|trust-failure drill' \
            "$trust_failure_drill_doc"; then
        echo "$trust_failure_drill_doc не документирует trust failure drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **186. Trust failure drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 186 завершённым" >&2
    exit 1
}

# Update failure drill contracts.
grep -Fq 'test-update-failure-drill-host' Makefile || {
    echo "Makefile не содержит test-update-failure-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-update-failure-drill-host' Makefile || {
    echo "test: target не содержит test-update-failure-drill-host" >&2
    exit 1
}
grep -Fq 'test-update-failure-drill-qemu' Makefile || {
    echo "Makefile не содержит test-update-failure-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'update_failure_drill_qemu_test_init'; then
    echo "Production kernel содержит update-failure-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[UPDATE-FAILURE-DRILL]'; then
    echo "Production kernel содержит update-failure-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-update-failure-drill.sh' Makefile || {
    echo "Makefile не вызывает update-failure-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU update failure baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-update-failure-drill.sh || {
    echo "test-qemu-update-failure-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU update failure drill test: OK' \
    scripts/test-qemu-update-failure-drill.sh || {
    echo "test-qemu-update-failure-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-update-failure-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит update-failure-drill script" >&2
    exit 1
fi
for update_failure_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'update failure drill|update-failure drill' \
            "$update_failure_drill_doc"; then
        echo "$update_failure_drill_doc не документирует update failure drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **187. Update failure drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 187 завершённым" >&2
    exit 1
}

# Integration replay/outage drill contracts.
grep -Fq 'test-integration-replay-outage-drill-host' Makefile || {
    echo "Makefile не содержит test-integration-replay-outage-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-integration-replay-outage-drill-host' Makefile || {
    echo "test: target не содержит test-integration-replay-outage-drill-host" >&2
    exit 1
}
grep -Fq 'test-integration-replay-outage-drill-qemu' Makefile || {
    echo "Makefile не содержит test-integration-replay-outage-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'integration_replay_outage_drill_qemu_test_init'; then
    echo "Production kernel содержит integration-replay-outage-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[INTEGRATION-REPLAY-OUTAGE-DRILL]'; then
    echo "Production kernel содержит integration-replay-outage-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-integration-replay-outage-drill.sh' Makefile || {
    echo "Makefile не вызывает integration-replay-outage-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU integration replay baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-integration-replay-outage-drill.sh || {
    echo "test-qemu-integration-replay-outage-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU integration replay drill test: OK' \
    scripts/test-qemu-integration-replay-outage-drill.sh || {
    echo "test-qemu-integration-replay-outage-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-integration-replay-outage-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит integration-replay-outage-drill script" >&2
    exit 1
fi
for integration_replay_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'integration replay/outage drill|integration-replay-outage drill' \
            "$integration_replay_drill_doc"; then
        echo "$integration_replay_drill_doc не документирует integration replay/outage drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **188. Integration replay/outage drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 188 завершённым" >&2
    exit 1
}

# Power-cycle continuity drill contracts.
grep -Fq 'test-power-cycle-continuity-drill-host' Makefile || {
    echo "Makefile не содержит test-power-cycle-continuity-drill-host" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-power-cycle-continuity-drill-host' Makefile || {
    echo "test: target не содержит test-power-cycle-continuity-drill-host" >&2
    exit 1
}
grep -Fq 'test-power-cycle-continuity-drill-qemu' Makefile || {
    echo "Makefile не содержит test-power-cycle-continuity-drill-qemu" >&2
    exit 1
}
if nm -g build/hfoc-kernel.elf |
        grep 'power_cycle_continuity_drill_qemu_test_init'; then
    echo "Production kernel содержит power-cycle-continuity-drill QEMU symbol" >&2
    exit 1
fi
if bash scripts/hfoc-file-marker.sh build/hfoc-kernel.elf \
        '[POWER-CYCLE-DRILL]'; then
    echo "Production kernel содержит power-cycle-continuity-drill proof marker" >&2
    exit 1
fi
grep -Fq 'scripts/test-qemu-power-cycle-continuity-drill.sh' Makefile || {
    echo "Makefile не вызывает power-cycle-continuity-drill QEMU script" >&2
    exit 1
}
grep -Fq 'QEMU power-cycle baseline: machine=q35 smp=1 cpu=default' \
    scripts/test-qemu-power-cycle-continuity-drill.sh || {
    echo "test-qemu-power-cycle-continuity-drill.sh не фиксирует portable baseline" >&2
    exit 1
}
grep -Fq 'QEMU power-cycle continuity drill test: OK' \
    scripts/test-qemu-power-cycle-continuity-drill.sh || {
    echo "test-qemu-power-cycle-continuity-drill.sh не содержит success marker" >&2
    exit 1
}
if grep -Fq 'qemu-power-cycle-continuity-drill' .github/workflows/ci.yml; then
    :
else
    echo "CI pgrep guard не содержит power-cycle-continuity-drill script" >&2
    exit 1
fi
for power_cycle_drill_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'power-cycle continuity drill|power-cycle-continuity drill' \
            "$power_cycle_drill_doc"; then
        echo "$power_cycle_drill_doc не документирует power-cycle continuity drill" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **189. Power-cycle continuity drill' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 189 завершённым" >&2
    exit 1
}

# Bounded QEMU suite contracts.
for qemu_suite_contract in \
    'test-serial:' \
    'test-qemu-parallel:' \
    'test-qemu-parallel-after-acceptance:' \
    'qemu-test-images:' \
    'HFOC_IMAGE_JOBS' \
    'QEMU_ACCEPTANCE_PROVEN_TARGETS' \
    'QEMU_PARALLEL_RERUN_TARGETS' \
    'scripts/run-qemu-suite.sh' \
    'tests/test-qemu-suite-host.sh'; do
    grep -Fq "$qemu_suite_contract" Makefile || {
        echo "Makefile не содержит QEMU suite contract: $qemu_suite_contract" >&2
        exit 1
    }
done
# shellcheck disable=SC2016
grep -Fq '$(MAKE) -j$(HFOC_IMAGE_JOBS) iso $(QEMU_TEST_IMAGES)' Makefile || {
    echo "qemu-test-images не использует bounded HFOC_IMAGE_JOBS recipe" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(MAKE) test-qemu-parallel-after-acceptance' Makefile || {
    echo "test recipe не вызывает post-acceptance QEMU phase" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq '$(filter-out $(QEMU_ACCEPTANCE_PROVEN_TARGETS),$(QEMU_PARALLEL_PHASE_1))' \
    Makefile || {
    echo "QEMU rerun phase не фильтрует acceptance-proven targets" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq \
    'bash scripts/run-qemu-suite.sh --serial $(QEMU_SERIAL_TARGETS) --parallel $(QEMU_PARALLEL_RERUN_TARGETS)' \
    Makefile || {
    echo "after-acceptance QEMU target не использует rerun targets" >&2
    exit 1
}
# shellcheck disable=SC2016
grep -Fq \
    'bash scripts/run-qemu-suite.sh --serial $(QEMU_SERIAL_TARGETS) --parallel $(QEMU_PARALLEL_PHASE_1)' \
    Makefile || {
    echo "full test-qemu-parallel target не использует QEMU_PARALLEL_PHASE_1" >&2
    exit 1
}
for ci_parallel_contract in \
    'HFOC_HOST_JOBS: "6"' \
    'HFOC_QEMU_JOBS: "4"' \
    'HFOC_ACCEPTANCE_JOBS: "2"' \
    'HFOC_IMAGE_JOBS: "4"' \
    'CCACHE_DIR: "/var/cache/ccache-hfoc"' \
    'CCACHE_PATH: "/usr/bin:/bin"'; do
    grep -Fq "$ci_parallel_contract" .github/workflows/ci.yml || {
        echo "CI env не содержит runner parallel contract: $ci_parallel_contract" >&2
        exit 1
    }
done
# The two verification phases and their host-wide serialization are the
# contract that keeps QEMU marker tests away from a second heavy phase.
# shellcheck disable=SC2016
for ci_phase_contract in \
    'phase: [static-host, acceptance-qemu]' \
    "if: matrix.phase == 'static-host'" \
    "if: matrix.phase == 'acceptance-qemu' && (github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch')"; do
    grep -Fq "$ci_phase_contract" .github/workflows/ci.yml || {
        echo "CI не содержит phase contract: $ci_phase_contract" >&2
        exit 1
    }
done
# ITEM-370: one job-level group per phase, shared by every event, so at most
# one static-host and one acceptance-qemu phase are live host-wide. An
# event-scoped group would let a second run's phase in, and a phase without the
# overlap record could not be proven non-overlapping from the log.
verify_ci_host_serialization() {
    local ci_file=$1
    local group_count
    local group_line
    local ci_expected_group
    local fragment

    group_count=$(grep -c "^[[:space:]]*group: hfoc-verify-" "$ci_file" || true)
    if [[ "$group_count" -ne 1 ]]; then
        echo "Ожидалась ровно одна строка group: hfoc-verify- в $ci_file, найдено: $group_count" >&2
        return 1
    fi
    group_line=$(grep "^[[:space:]]*group: hfoc-verify-" "$ci_file" |
        sed 's/^[[:space:]]*//')
    # shellcheck disable=SC2016
    ci_expected_group='group: hfoc-verify-${{ matrix.phase }}-${{ github.event_name == '"'"'push'"'"' && '"'"'main'"'"' || '"'"'pr'"'"' }}'
    if [[ "$group_line" != "$ci_expected_group" ]]; then
        echo "CI group не сериализует фазы host-wide в $ci_file: $group_line" >&2
        return 1
    fi
    if ! grep -Fq 'cancel-in-progress: false' "$ci_file"; then
        echo "CI group не выстроен в очередь (cancel-in-progress: false) в $ci_file" >&2
        return 1
    fi
    # shellcheck disable=SC2016
    for fragment in \
        'HFOC_PHASE_RECORD phase=%s event=%s runner=%s heavy=%s start_utc=%s start_epoch=%s' \
        'HFOC_PHASE_START_EPOCH=$start_epoch' \
        'HFOC_PHASE_RECORD phase=%s event=%s runner=%s end_utc=%s end_epoch=%s elapsed=%ss'; do
        if ! grep -Fq -- "$fragment" "$ci_file"; then
            echo "CI не публикует overlap record в $ci_file: $fragment" >&2
            return 1
        fi
    done
}
verify_ci_host_serialization .github/workflows/ci.yml || exit 1
ci_host_fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-ci-host.XXXXXX")
trap 'rm -rf "$ci_host_fixture_dir"' EXIT
cat >"$ci_host_fixture_dir/event-scoped.yml" <<'EOF'
    concurrency:
      group: hfoc-verify-${{ matrix.phase }}-${{ github.event_name == 'pull_request' && 'pr' || github.run_id }}
      cancel-in-progress: false
EOF
if verify_ci_host_serialization "$ci_host_fixture_dir/event-scoped.yml" 2>/dev/null; then
    echo "host serialization fixture: event-scoped group incorrectly accepted" >&2
    exit 1
fi
cat >"$ci_host_fixture_dir/shared-everything.yml" <<'EOF'
    concurrency:
      group: hfoc-verify-${{ matrix.phase }}
      cancel-in-progress: false
EOF
if verify_ci_host_serialization "$ci_host_fixture_dir/shared-everything.yml" 2>/dev/null; then
    echo "host serialization fixture: main push not isolated incorrectly accepted" >&2
    exit 1
fi
cat >"$ci_host_fixture_dir/no-record.yml" <<'EOF'
    concurrency:
      group: hfoc-verify-${{ matrix.phase }}-${{ github.event_name == 'push' && 'main' || 'pr' }}
      cancel-in-progress: false
EOF
if verify_ci_host_serialization "$ci_host_fixture_dir/no-record.yml" 2>/dev/null; then
    echo "host serialization fixture: missing overlap record incorrectly accepted" >&2
    exit 1
fi
rm -rf "$ci_host_fixture_dir"
trap - EXIT
# ITEM-370A: every heavy CI phase runs its whole heavy body under the
# host-wide heavy lock in shared mode, so the two phases stay parallel while a
# local heavy build (exclusive mode) waits instead of overlapping them. A
# heavy command that escapes the locked step — an unlocked `make check` next
# to QEMU guests — is exactly the contention class this item removes.
hfoc_ci_step_block() {
    local ci_file=$1
    local step_name=$2
    awk -v needle="      - name: $step_name" '
        index($0, "      - name: ") == 1 { inside = (index($0, needle) == 1); next }
        inside { print }
    ' "$ci_file"
}
verify_ci_heavy_lock() {
    local ci_file=$1
    local static_body=$2
    local acceptance_body=$3
    local phase
    local block
    local lock_command
    local required

    for phase in static-host acceptance-qemu; do
        block=$(hfoc_ci_step_block "$ci_file" "HFOC heavy phase $phase")
        if [[ -z "$block" ]]; then
            echo "CI не содержит locked heavy step фазы $phase в $ci_file" >&2
            return 1
        fi
        lock_command="scripts/hfoc-heavy-lock.sh ci-shared $phase -- scripts/ci-$phase-phase.sh"
        if ! grep -Fq "$lock_command" <<<"$block"; then
            echo "heavy step фазы $phase не берёт host-wide lock в $ci_file" >&2
            return 1
        fi
        if [[ $(grep -Fc "scripts/ci-$phase-phase.sh" <<<"$block") -ne 1 ]]; then
            echo "heavy step фазы $phase запускает body вне lock в $ci_file" >&2
            return 1
        fi
    done

    # ITEM-425: the post-merge phase takes the lock exclusively, so it cannot
    # starve a live pull-request phase - the measurement is in the workflow
    # comment and in docs/ci-self-hosted-runner.md.
    postmerge_block=$(hfoc_ci_step_block "$ci_file" "HFOC heavy phase postmerge")
    if [[ -z "$postmerge_block" ]]; then
        echo "CI не содержит locked heavy step пост-merge фазы в $ci_file" >&2
        return 1
    fi
    if ! grep -Fq 'scripts/hfoc-heavy-lock.sh ci-exclusive postmerge -- scripts/ci-static-host-phase.sh' \
        <<<"$postmerge_block"; then
        echo "пост-merge фаза не берёт lock эксклюзивно в $ci_file" >&2
        return 1
    fi
    if ! grep -Fq 'github.event_name == '"'"'push'"'"'' <<<"$postmerge_block"; then
        echo "пост-merge фаза не ограничена push-событием в $ci_file" >&2
        return 1
    fi
    shared_static_block=$(hfoc_ci_step_block "$ci_file" \
        "HFOC heavy phase static-host")
    if ! grep -Fq "github.event_name != 'push'" <<<"$shared_static_block"; then
        echo "shared static-host фаза не исключает push-событие в $ci_file" >&2
        return 1
    fi

    # A heavy command left in the workflow itself would bypass the lock.
    # shellcheck disable=SC2016
    for required in \
        'make check' \
        'make test-host-suite' \
        'make test-qemu-parallel-after-acceptance' \
        'make test-production-end-to-end-acceptance' \
        'shellcheck "$script"' \
        'shellcheck scripts/'; do
        if grep -v '^[[:space:]]*#' "$ci_file" | grep -Fq "$required"; then
            echo "heavy команда вне locked phase в $ci_file: $required" >&2
            return 1
        fi
    done

    # The locked bodies must keep every gate of the steps they replace.
    for required in 'shellcheck ' 'make check' 'make test-host-suite'; do
        if ! grep -v '^[[:space:]]*#' "$static_body" | grep -Fq "$required"; then
            echo "static-host body потерял команду: $required" >&2
            return 1
        fi
    done
    for required in \
        'make test-release-appliance-v1-acceptance' \
        'make test-https-vertical-slice-acceptance' \
        'make test-hfoc-appliance-platform-release-acceptance' \
        'make test-hfoc-integrated-installer-dual-boot-management-acceptance' \
        'make test-nvme-production-acceptance' \
        'make test-hfoc-release-documentation' \
        'make test-qemu-parallel-after-acceptance' \
        'make test-production-end-to-end-acceptance'; do
        if ! grep -v '^[[:space:]]*#' "$acceptance_body" | grep -Fq "$required"; then
            echo "acceptance-qemu body потерял команду: $required" >&2
            return 1
        fi
    done
    # ShellCheck (~9 GiB) and the kernel build must stay out of the QEMU
    # phase, otherwise the acceptance phase duplicates the static half.
    for required in 'shellcheck ' 'make check' 'make test-host-suite'; do
        if grep -v '^[[:space:]]*#' "$acceptance_body" | grep -Fq "$required"; then
            echo "acceptance-qemu body дублирует static-host работу: $required" >&2
            return 1
        fi
    done
}
verify_ci_heavy_lock .github/workflows/ci.yml \
    scripts/ci-static-host-phase.sh scripts/ci-acceptance-qemu-phase.sh || exit 1
hfoc_heavy_fixture_dir=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-ci-heavy.XXXXXX")
trap 'rm -rf "$hfoc_heavy_fixture_dir"' EXIT
cat >"$hfoc_heavy_fixture_dir/unlocked.yml" <<'EOF'
      - name: HFOC heavy phase static-host
        if: matrix.phase == 'static-host'
        run: scripts/ci-static-host-phase.sh
      - name: HFOC heavy phase acceptance-qemu
        if: matrix.phase == 'acceptance-qemu' && (github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch')
        run: scripts/hfoc-heavy-lock.sh ci-shared acceptance-qemu -- scripts/ci-acceptance-qemu-phase.sh
EOF
if verify_ci_heavy_lock "$hfoc_heavy_fixture_dir/unlocked.yml" \
    scripts/ci-static-host-phase.sh scripts/ci-acceptance-qemu-phase.sh 2>/dev/null; then
    echo "heavy lock fixture: unlocked phase body incorrectly accepted" >&2
    exit 1
fi
cat >"$hfoc_heavy_fixture_dir/escaped.yml" <<'EOF'
      - name: HFOC heavy phase static-host
        if: matrix.phase == 'static-host'
        run: scripts/hfoc-heavy-lock.sh ci-shared static-host -- scripts/ci-static-host-phase.sh
      - name: HFOC heavy phase acceptance-qemu
        if: matrix.phase == 'acceptance-qemu' && (github.event_name == 'pull_request' || github.event_name == 'workflow_dispatch')
        run: scripts/hfoc-heavy-lock.sh ci-shared acceptance-qemu -- scripts/ci-acceptance-qemu-phase.sh
      - name: Escaped heavy work
        run: make check
EOF
if verify_ci_heavy_lock "$hfoc_heavy_fixture_dir/escaped.yml" \
    scripts/ci-static-host-phase.sh scripts/ci-acceptance-qemu-phase.sh 2>/dev/null; then
    echo "heavy lock fixture: escaped heavy command incorrectly accepted" >&2
    exit 1
fi
cat >"$hfoc_heavy_fixture_dir/acceptance-body.sh" <<'EOF'
make test-release-appliance-v1-acceptance
EOF
if verify_ci_heavy_lock .github/workflows/ci.yml \
    scripts/ci-static-host-phase.sh "$hfoc_heavy_fixture_dir/acceptance-body.sh" 2>/dev/null; then
    echo "heavy lock fixture: acceptance body with missing gates incorrectly accepted" >&2
    exit 1
fi
rm -rf "$hfoc_heavy_fixture_dir"
trap - EXIT
# ShellCheck and make check must stay in the static-host phase only, otherwise
# the acceptance-qemu phase duplicates a ~9 GiB shellcheck and a second kernel
# build next to the QEMU guests. Since ITEM-370A the static half is one locked
# phase step whose body scripts/check.sh asserts above.
# ITEM-425: the static half is two steps now - the shared one for pull
# requests and dispatches, the exclusive one for a post-merge push - and both
# are the locked static phase, so the count is two.
ci_static_only_steps=$(grep -cE "^[[:space:]]*if: matrix\.phase == 'static-host'( &&|$)" \
    .github/workflows/ci.yml)
if [[ "$ci_static_only_steps" -ne 2 ]]; then
    echo "Ожидались 2 static-host шага (shared и post-merge locked фазы), найдено: $ci_static_only_steps" >&2
    exit 1
fi
# The post-restart echo keeps its retries but must stay inside a latency
# budget, so a listener that only answers after minutes still fails. The
# measured latency is written to a summary file that CI prints on every run.
for ci_restart_budget_contract in \
    'HFOC_NET_RESOURCE_RESTART_ECHO_BUDGET' \
    'restart_echo_latency > restart_echo_budget'; do
    grep -Fq "$ci_restart_budget_contract" scripts/test-qemu-network-resource.sh || {
        echo "network-resource proof не содержит latency budget contract: $ci_restart_budget_contract" >&2
        exit 1
    }
done
grep -Fq 'network-resource-restart-echo-latency.txt' \
    scripts/test-qemu-network-resource.sh .github/workflows/ci.yml || {
    echo "latency summary не публикуется в CI" >&2
    exit 1
}
for canonical_qemu_target in \
    test-pic-irq0-masking-fallback \
    test-supportability-qemu \
    test-console-history; do
    if ! awk -v target="\$(MAKE) $canonical_qemu_target" '
        /^test-serial:/ { in_recipe = 1; next }
        in_recipe && /^[^[:space:]#]/ { exit }
        in_recipe && index($0, target) { found = 1 }
        END { exit found ? 0 : 1 }
    ' Makefile; then
        echo "test-serial не содержит $canonical_qemu_target" >&2
        exit 1
    fi
done

# Resilience matrix contracts.
grep -Fq 'test-resilience-matrix-host' Makefile || {
    echo "Makefile не содержит test-resilience-matrix-host" >&2
    exit 1
}
grep -Fq 'test-resilience-matrix-qemu' Makefile || {
    echo "Makefile не содержит test-resilience-matrix-qemu" >&2
    exit 1
}
grep -Fq 'scripts/test-resilience-matrix.sh' Makefile || {
    echo "Makefile не вызывает resilience matrix script" >&2
    exit 1
}
grep -Fq 'HFOC_QEMU_SUITE_ARTIFACT_DIR' scripts/run-qemu-suite.sh || {
    echo "run-qemu-suite.sh не поддерживает artifact capture" >&2
    exit 1
}
grep -Fq 'Resilience matrix: OK' scripts/test-resilience-matrix.sh || {
    echo "test-resilience-matrix.sh не содержит success marker" >&2
    exit 1
}
grep -Fq 'test-power-cycle-continuity-drill-qemu' \
    scripts/test-resilience-matrix.sh || {
    echo "resilience matrix не содержит power-cycle continuity target" >&2
    exit 1
}
if ! grep -Fq 'test-resilience-matrix-qemu' Makefile; then
    echo "Makefile не содержит resilience matrix QEMU target" >&2
    exit 1
fi
if ! awk '
    /^QEMU_PARALLEL_PHASE_1 :=/ { in_phase = 1; next }
    in_phase && /^[^[:space:]#]/ { exit }
    in_phase && /test-resilience-matrix-qemu/ { found = 1 }
    END { if (!found) exit 1 }
' Makefile; then
    echo "QEMU_PARALLEL_PHASE_1 не содержит resilience matrix target" >&2
    exit 1
fi
if ! grep -Fq 'resilience-matrix' .github/workflows/ci.yml; then
    echo "CI pgrep guard не содержит resilience-matrix script" >&2
    exit 1
fi
for resilience_doc in README.md docs/ARCHITECTURE.md docs/ROADMAP.md; do
    if ! grep -Eiq 'resilience matrix|resilience-matrix' "$resilience_doc"; then
        echo "$resilience_doc не документирует resilience matrix" >&2
        exit 1
    fi
done
grep -Fq -- '- [x] **190. Resilience matrix QEMU proof' \
    docs/superpowers/plans/2026-08-17-appliance-v1-next-100-pr-queue-execution.md || {
    echo "appliance queue plan не помечает item 190 завершённым" >&2
    exit 1
}
bash -n scripts/test-resilience-matrix.sh || {
    echo "Синтаксис scripts/test-resilience-matrix.sh некорректен" >&2
    exit 1
}
bash -n tests/test-resilience-matrix-host.sh || {
    echo "Синтаксис tests/test-resilience-matrix-host.sh некорректен" >&2
    exit 1
}
bash tests/test-resilience-matrix-host.sh
bash tests/test-qemu-suite-host.sh
test -x tests/test-qemu-duplicate-policy.sh || {
    echo 'tests/test-qemu-duplicate-policy.sh is not executable' >&2
    exit 1
}
bash -n tests/test-qemu-duplicate-policy.sh || {
    echo "Синтаксис tests/test-qemu-duplicate-policy.sh некорректен" >&2
    exit 1
}
bash tests/test-qemu-duplicate-policy.sh

# ITEM-431: the post-merge gate waits longer than the phase it waits for, and
# the merge rule is a tool instead of a memory.
#
# The post-merge static phase asks for the heavy lock exclusively (ITEM-425),
# so it waits for a whole live pull-request phase - measured at 15-30.5 minutes
# - while its wait bound was the shared one, 1200 s.  On 2026-09-16 it timed
# out with EX_TEMPFAIL=75 waiting for a live phase and took `main` red with it,
# twice.  The exclusive bound is now longer than any phase and still inside the
# workflow's 90-minute job timeout (ITEM-584).
# shellcheck disable=SC2016  # the single-quoted text is what the file says
for heavy_lock_budget in \
    'DEFAULT_CI_WAIT=1200' \
    'DEFAULT_CI_EXCLUSIVE_WAIT=4500' \
    'HFOC_HEAVY_CI_EXCLUSIVE_WAIT'; do
    grep -Fq -- "$heavy_lock_budget" scripts/hfoc-heavy-lock.sh || {
        echo "ITEM-431: бюджет ожидания heavy-lock потерян:" \
            "$heavy_lock_budget" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the single-quoted text is what the file says
grep -Fq \
    'wait_seconds=${HFOC_HEAVY_CI_EXCLUSIVE_WAIT:-${HFOC_HEAVY_CI_WAIT:-$DEFAULT_CI_EXCLUSIVE_WAIT}}' \
    scripts/hfoc-heavy-lock.sh || {
    echo "ITEM-431: эксклюзивное ожидание не берёт свой бюджет" >&2
    exit 1
}
# The job must still outlive its own wait: 75 minutes of waiting plus the
# static phase's own work inside the job.  ITEM-513 raised the bound from 60 to
# 75 minutes when the acceptance phase, with one more bounded QEMU boot,
# finished its work at 58m43s and was killed by the 60-minute clock during
# evidence collection (run 38027017135).  ITEM-584 raised the wait to 75
# minutes and the job timeout to 90 minutes after the 2026-11-01 post-merge
# run timed out behind a 47-minute acceptance phase.
grep -Fq 'timeout-minutes: 90' .github/workflows/ci.yml || {
    echo "ITEM-431: таймаут job'а не покрывает бюджет ожидания" >&2
    exit 1
}
for heavy_lock_drill in \
    'the exclusive CI wait is not the post-merge budget' \
    'the shared CI wait changed with the exclusive one' \
    'the exclusive CI wait override was not honoured'; do
    grep -Fq -- "$heavy_lock_drill" tests/test-heavy-lock-host.sh || {
        echo "ITEM-431: host-проба heavy-lock не содержит: " \
            "$heavy_lock_drill" >&2
        exit 1
    }
done

# The merge rule: a pull request whose own run is still live must not be
# merged.  The guard is the tool the worker runs before every merge, its
# refusals are distinct exit codes, and its host proof drives every one of
# them against a fake `gh`.
test -x scripts/hfoc-merge-guard.sh || {
    echo "ITEM-431: scripts/hfoc-merge-guard.sh отсутствует или не исполняем" >&2
    exit 1
}
bash -n scripts/hfoc-merge-guard.sh || {
    echo "ITEM-431: синтаксис merge guard некорректен" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the file says
for merge_guard_code in \
    'EX_LIVE_RUN=11' \
    'EX_NO_GREEN_RUN=12' \
    'EX_NOT_CLEAN=13' \
    'EX_CLOSED=10' \
    'EX_GH_UNREADABLE=65' \
    'EX_MERGE_REFUSED=66' \
    'reason=GH_UNREADABLE' \
    'reason=MERGE_REFUSED' \
    'HFOC-MERGE-GUARD pr=%s head=%s state=OK run=%s' \
    'HFOC-MERGE-GUARD pr=%s head=%s state=MERGED commit=%s' \
    'pr merge "$pr" --squash' \
    '"$EX_LIVE_RUN" LIVE_RUN' \
    '"$EX_NO_GREEN_RUN" NO_GREEN_RUN' \
    '"$EX_NOT_CLEAN" NOT_CLEAN' \
    '"$EX_CLOSED" PR_NOT_OPEN' \
    'HFOC_MERGE_GUARD_GH'; do
    grep -Fq -- "$merge_guard_code" scripts/hfoc-merge-guard.sh || {
        echo "ITEM-431: merge guard не содержит: $merge_guard_code" >&2
        exit 1
    }
done
# shellcheck disable=SC2016  # the single-quoted text is what the file says
for merge_guard_drill in \
    'a live run is refused with exit 11' \
    'an older green run does not excuse a live one' \
    'a head with no green run is refused with exit 12' \
    'a pull request that is not open is refused with exit 10' \
    'a head GitHub does not call clean is refused with exit 13' \
    'a gh read failure is refused with exit 65' \
    'a $rare_status run is refused with exit 11' \
    'the guard asks for the runs of the head commit' \
    'a green head merges with --merge' \
    'a live run refuses --merge before merging' \
    'the live-run refusal never reaches the merge' \
    'a refused merge is its own code' \
    'the refusal never claims the merge happened'; do
    grep -Fq -- "$merge_guard_drill" tests/test-merge-guard-host.sh || {
        echo "ITEM-431: host-проба merge guard не содержит: " \
            "$merge_guard_drill" >&2
        exit 1
    }
done
grep -Fq 'test-merge-guard-host:' Makefile || {
    echo "ITEM-431: Makefile не содержит test-merge-guard-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-merge-guard-host' Makefile || {
    echo "ITEM-431: host suite не вызывает test-merge-guard-host" >&2
    exit 1
}
grep -Fq 'test-merge-guard-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-431: манифест host suite не содержит test-merge-guard-host" >&2
    exit 1
}
for merge_guard_doc in \
    'hfoc-merge-guard.sh' \
    'LIVE_RUN'; do
    grep -Fq -- "$merge_guard_doc" docs/ci-self-hosted-runner.md || {
        echo "ITEM-431: docs/ci-self-hosted-runner.md не фиксирует: " \
            "$merge_guard_doc" >&2
        exit 1
    }
done
grep -Fq 'ITEM-431' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-431" >&2
    exit 1
}
bash tests/test-merge-guard-host.sh

# ITEM-432: our own code compiles warning-free, and the phase that compiles it
# is the gate.
#
# The production build carried exactly two warnings for several items - a dead
# declaration in platform_reset.c and a function declared unable to return that
# the compiler could not prove - and nothing failed on them, so they were read
# past.  The acceptance phase captures its build and refuses a `kernel/`
# warning, publishes the count, and the two causes are fixed where they were.
grep -Fq 'static _Noreturn void platform_reset_triple_fault(void) {' \
    kernel/src/platform_reset.c || {
    echo "ITEM-432: triple fault снова не объявлен как noreturn" >&2
    exit 1
}
# The phase captures the build that compiles the kernel and hands it to the
# gate; the gate itself is a script, so the host suite can drive every shape of
# its reading on captured logs instead of a copy of its regex.
# shellcheck disable=SC2016  # the single-quoted text is what the file says
grep -Fq "make kernel-rebuild 2>&1 | tee -a '\$build_log'" \
    scripts/ci-acceptance-qemu-phase.sh || {
    echo "ITEM-432: фаза приёмки не пересобирает production-ядро под гейт" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the file says
grep -Fq "make qemu-test-images 2>&1 | tee -a '\$build_log'" \
    scripts/ci-acceptance-qemu-phase.sh || {
    echo "ITEM-432: фаза приёмки не захватывает сборку ядра" >&2
    exit 1
}
grep -Fq 'kernel-rebuild:' Makefile || {
    echo "ITEM-432: Makefile не содержит kernel-rebuild" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) -B $(KERNEL)' Makefile || {
    echo "ITEM-432: kernel-rebuild не форсирует компиляцию ядра" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the file says
grep -Fq 'bash scripts/hfoc-build-warning-gate.sh "$build_log"' \
    scripts/ci-acceptance-qemu-phase.sh || {
    echo "ITEM-432: фаза приёмки не читает сборку гейтом" >&2
    exit 1
}
test -x scripts/hfoc-build-warning-gate.sh || {
    echo "ITEM-432: scripts/hfoc-build-warning-gate.sh отсутствует или не исполняем" >&2
    exit 1
}
for build_warning_gate in \
    'HFOC-BUILD-WARNINGS state=CLEAN kernel=0' \
    'HFOC-BUILD-WARNINGS state=FAIL kernel=' \
    'HFOC-BUILD-WARNINGS state=FAIL reason=LOG_UNREADABLE' \
    'HFOC-BUILD-WARNINGS state=FAIL reason=LOG_EMPTY' \
    "'^kernel/[^:]+:[0-9]+(:[0-9]+)?: [Ww]arning:'"; do
    grep -Fq -- "$build_warning_gate" scripts/hfoc-build-warning-gate.sh || {
        echo "ITEM-432: гейт не содержит: $build_warning_gate" >&2
        exit 1
    }
done
for build_warning_drill in \
    'a kernel warning' \
    'an assembler warning' \
    'two kernel warnings' \
    'third-party warnings only' \
    'missing log' \
    'empty log' \
    'unreadable log'; do
    grep -Fq -- "$build_warning_drill" tests/test-build-warning-gate-host.sh || {
        echo "ITEM-432: host-проба гейта не содержит: $build_warning_drill" >&2
        exit 1
    }
done
grep -Fq 'test-build-warning-gate-host:' Makefile || {
    echo "ITEM-432: Makefile не содержит test-build-warning-gate-host" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the Makefile says
grep -Fq '$(MAKE) test-build-warning-gate-host' Makefile || {
    echo "ITEM-432: host suite не вызывает test-build-warning-gate-host" >&2
    exit 1
}
grep -Fq 'test-build-warning-gate-host' tests/test-host-suite-manifest.sh || {
    echo "ITEM-432: манифест host suite не содержит test-build-warning-gate-host" >&2
    exit 1
}
grep -Fq 'ITEM-432' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-432" >&2
    exit 1
}
