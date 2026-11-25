#!/usr/bin/env bash
# ITEM-412: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The QEMU acceptance phase must not fail on a load-induced marker timeout, and
# it must never retry a real failure.
#
# A proof that stopped because it did not observe an expected marker inside its
# own deadline is a different event from a proof whose assertion failed: the
# guest may be perfectly healthy and merely starved, so the phase retries that
# target once, visibly, and takes its verdict from the retry. Everything else -
# a failed expectation, an in-guest `[TAG] failed:` record, a kernel panic, a
# fail-closed guard, or a nonzero exit that carries no record at all - fails the
# phase immediately, exactly as it did before, and is never retried.
#
# The class is therefore not inferred from English text: a proof declares it by
# writing one strict record
#
#     HFOC-QEMU-MARKER-TIMEOUT marker="<marker>" deadline=<seconds>s
#
# and scripts/hfoc-qemu-failure-class.sh refuses the class for anything else,
# including a bare `timed out waiting for ...` line, a malformed record, a log
# that also carries a fail-closed signature and an unreadable log.
for marker_timeout_file in \
    scripts/hfoc-qemu-failure-class.sh \
    scripts/run-qemu-suite.sh \
    tests/test-qemu-suite-host.sh \
    docs/ci-self-hosted-runner.md; do
    [[ -f $marker_timeout_file ]] || {
        echo "ITEM-412: отсутствует $marker_timeout_file" >&2
        exit 1
    }
done
grep -Fq 'hfoc-qemu-failure-class.sh' scripts/run-qemu-suite.sh || {
    echo "run-qemu-suite.sh не использует классификатор класса отказа" >&2
    exit 1
}
grep -Fq "HFOC_QEMU_MARKER_TIMEOUT_RECORD='HFOC-QEMU-MARKER-TIMEOUT'" \
    scripts/hfoc-qemu-failure-class.sh || {
    echo "каноническая запись marker-timeout не зафиксирована" >&2
    exit 1
}
for fail_closed_signature in \
    "'*** HFOC KERNEL PANIC ***'" \
    "'Kernel panic '" \
    "'reported failure'" \
    "' failed: '"; do
    grep -Fq "$fail_closed_signature" scripts/hfoc-qemu-failure-class.sh || {
        echo "fail-closed признак не зафиксирован: $fail_closed_signature" >&2
        exit 1
    }
done
for suite_retry_contract in \
    'QEMU suite: FAIL-CLASS %s class=%s reason=%s seconds=%s' \
    'QEMU suite: RETRY %s reason=marker-timeout attempt=2' \
    'QEMU suite: NO-RETRY %s class=%s reason=%s' \
    'HFOC_QEMU_TIMING_RETRY:-1' \
    'HFOC_QEMU_RETRY_LIMIT:-3' \
    'GITHUB_STEP_SUMMARY' \
    'retries.txt' \
    'may_retry'; do
    grep -Fq -- "$suite_retry_contract" scripts/run-qemu-suite.sh || {
        echo "run-qemu-suite.sh не содержит контракт повтора: $suite_retry_contract" >&2
        exit 1
    }
done
# A single retry per target: the target is re-queued only while it is not in the
# retried set, and a second failure of the same target ends the phase.
# shellcheck disable=SC2016  # the compared text is literal
grep -Fq 'retried_target[$reaped_target]=1' scripts/run-qemu-suite.sh || {
    echo "повтор не ограничен одним разом на цель" >&2
    exit 1
}
grep -Fq "queue+=(\"\$reaped_target\")" scripts/run-qemu-suite.sh || {
    echo "повтор не возвращает ту же цель с теми же ожиданиями" >&2
    exit 1
}

# The proofs that flaked declare the class themselves, so the suite can see it.
for marker_timeout_proof in \
    scripts/test-qemu-local-control.sh \
    scripts/test-qemu-network-resource.sh \
    scripts/test-qemu-https-restart-soak.sh \
    scripts/test-qemu-production-dhcp-dns.sh; do
    grep -Fq 'def marker_timeout(marker' "$marker_timeout_proof" || {
        echo "$marker_timeout_proof не объявляет класс таймаута маркера" >&2
        exit 1
    }
    grep -Fq 'HFOC-QEMU-MARKER-TIMEOUT marker=' "$marker_timeout_proof" || {
        echo "$marker_timeout_proof не пишет каноническую запись" >&2
        exit 1
    }
done
# ITEM-434/ITEM-506: the production service/HTTPS proof sets the account on
# the local console and the enrollment record follows a moment later; the
# records it used to assert are awaited now, and a record that really does not
# arrive declares the class with the same canonical line.
for lagging_record_proof in \
    'wait_for_record' \
    'HFOC-QEMU-MARKER-TIMEOUT marker=' \
    "wait_for_record '[ADMIN-ENROLLMENT] credentials changed' 20"; do
    grep -Fq -- "$lagging_record_proof" \
        scripts/test-qemu-production-service-https.sh || {
        echo "ITEM-434: доказательство сервиса не ждёт отстающую запись:" \
            "$lagging_record_proof" >&2
        exit 1
    }
done
grep -Fq 'ITEM-434' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-434" >&2
    exit 1
}

# The classification is proven, not asserted: the same fixture logs the suite
# sees must land in the intended class, and every fail-closed shape must stay
# out of the retryable class.
marker_timeout_fixture=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-marker-timeout.XXXXXX")
trap 'rm -rf "$marker_timeout_fixture"' EXIT
printf '%s\n' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    'timed out waiting for [FAKE] proof OK' >"$marker_timeout_fixture/timing.log"
printf '%s\n' \
    'timed out waiting for [FAKE] proof OK' >"$marker_timeout_fixture/legacy.log"
printf '%s\n' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    '*** HFOC KERNEL PANIC *** double fault' >"$marker_timeout_fixture/panic.log"
printf '%s\n' \
    '[FAKE] failed: read-only appliance status proof' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    >"$marker_timeout_fixture/in-guest.log"
printf '%s\n' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK"' \
    >"$marker_timeout_fixture/malformed.log"
: >"$marker_timeout_fixture/empty.log"
# ITEM-463: a healthy guest prints `Kernel panic: none` as its own all-clear,
# and the unanchored `Kernel panic` signature swallowed it, so the timing class
# was refused and a starved-but-healthy guest was never retried.  The all-clear
# is not a panic; a proof's own panic word still is.
printf '%s\n' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    'Kernel panic: none' >"$marker_timeout_fixture/all-clear.log"
printf '%s\n' \
    'HFOC-QEMU-MARKER-TIMEOUT marker="[FAKE] proof OK" deadline=7s' \
    'Kernel panic detected' >"$marker_timeout_fixture/panic-word.log"
classify_marker_timeout_log() {
    bash -c '
        set -Eeuo pipefail
        . scripts/hfoc-qemu-failure-class.sh
        hfoc_qemu_failure_classify "$1"
        printf "%s|%s|%s" "$hfoc_qemu_failure_class" \
            "$hfoc_qemu_failure_marker" "$hfoc_qemu_failure_deadline"
    ' _ "$1"
}
[[ $(classify_marker_timeout_log "$marker_timeout_fixture/timing.log") == \
    'marker-timeout|[FAKE] proof OK|7s' ]] || {
    echo "классификатор не распознал каноническую запись таймаута" >&2
    exit 1
}
# ITEM-463: the all-clear keeps the retryable class.
[[ $(classify_marker_timeout_log "$marker_timeout_fixture/all-clear.log") == \
    'marker-timeout|[FAKE] proof OK|7s' ]] || {
    echo "all-clear здорового гостя попал в assertion" >&2
    exit 1
}
for marker_timeout_negative in \
    legacy.log panic.log panic-word.log in-guest.log malformed.log empty.log; do
    classified=$(classify_marker_timeout_log "$marker_timeout_fixture/$marker_timeout_negative")
    [[ ${classified%%|*} == assertion ]] || {
        echo "fail-closed лог попал в повторяемый класс: $marker_timeout_negative ($classified)" >&2
        exit 1
    }
done
rm -rf "$marker_timeout_fixture"
trap - EXIT

# The mechanism is proven both ways in the harness's own test: a timing failure
# is retried once and then fails the phase, an assertion failure is never run
# twice, and a wrong echo, a panic and an in-guest failure keep their class.
for marker_timeout_case in \
    'timing-once' \
    'timing-always' \
    'assert-fail' \
    'guard-fail' \
    'in-guest-fail' \
    'QEMU suite: RETRY timing-once reason=marker-timeout attempt=2' \
    'QEMU suite: PASS timing-once attempt=2' \
    'NO-RETRY timing-always class=marker-timeout reason=retry-exhausted'; do
    grep -Fq -- "$marker_timeout_case" tests/test-qemu-suite-host.sh || {
        echo "host-тест не проверяет случай повтора: $marker_timeout_case" >&2
        exit 1
    }
done

# The rule is documented where a maintainer looks for it.
for marker_timeout_doc_contract in \
    'HFOC-QEMU-MARKER-TIMEOUT' \
    'QEMU suite: RETRY' \
    'ITEM-412'; do
    grep -Fq -- "$marker_timeout_doc_contract" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует правило повтора: $marker_timeout_doc_contract" >&2
        exit 1
    }
done
grep -Fq 'ITEM-412' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-412" >&2
    exit 1
}

# ITEM-421: the rule belongs to every runner that can stop a phase on a timing
# stop, not only to the QEMU suite runner.
#
# The acceptance gate runs its own target list through
# `scripts/run-acceptance-suite.sh`, and on 2026-09-15 it failed a phase on
# `test-https-restart-soak-qemu` whose log carried the class record - the gate
# had no rule to read it.  The gate applies the same rule now, a console-driven
# proof that can stop on timing declares the class (`test-qemu-lapic-timer-oneshot`
# failed the same way with no record), and the runner's own host drill proves
# both the retry and the refusal to retry an assertion.
[[ -f scripts/run-acceptance-suite.sh ]] || {
    echo "нет приёмочного раннера" >&2
    exit 1
}
for marker_timeout_gate in \
    'hfoc_qemu_failure_classify' \
    'ACCEPTANCE suite: RETRY' \
    'RETRY-PASS'; do
    grep -Fq -- "$marker_timeout_gate" scripts/run-acceptance-suite.sh || {
        echo "приёмочный раннер не применяет правило повтора: $marker_timeout_gate" >&2
        exit 1
    }
done
grep -Fq 'HFOC-QEMU-MARKER-TIMEOUT' \
    scripts/test-qemu-lapic-timer-oneshot.sh || {
    echo "proof таймера не объявляет класс таймаута маркера" >&2
    exit 1
}
for marker_timeout_gate_case in \
    'RETRY-PASS timing-once reason=marker-timeout attempt=2' \
    'acceptance runner retried an assertion'; do
    grep -Fq -- "$marker_timeout_gate_case" \
        tests/test-acceptance-suite-runner.sh || {
        echo "host-тест приёмочного раннера не проверяет: $marker_timeout_gate_case" >&2
        exit 1
    }
done

# ITEM-422: the declaration is required of every proof the acceptance phase
# runs, not only of the two that have flaked.
#
# On 2026-09-15 the gate failed on `test-pic-disable`, whose stop was a marker
# timeout with no record: the phase had the retry rule (ITEM-421) and could not
# use it, because the proof stayed silent.  A rule that only covers the proofs
# that already failed once is a rule that fails again on the next proof, so the
# reader walks the phase's own target list out of the Makefile and requires the
# record at every give-up on a guest marker.
[[ -f scripts/hfoc-proof-marker-class.sh ]] || {
    echo "нет читателя класса маркеров доказательств" >&2
    exit 1
}
# The list is the phase's, read from the Makefile: a target joins the rule by
# being run, not by being remembered here.
for marker_timeout_target in \
    'QEMU_SERIAL_TARGETS' \
    'QEMU_PARALLEL_PHASE_1'; do
    grep -Fq -- "$marker_timeout_target" scripts/hfoc-proof-marker-class.sh || {
        echo "читатель не берёт список целей фазы: $marker_timeout_target" >&2
        exit 1
    }
done
marker_timeout_pairs=$(bash scripts/hfoc-proof-marker-class.sh list)
marker_timeout_count=$(printf '%s\n' "$marker_timeout_pairs" | grep -c .)
(( marker_timeout_count >= 40 )) || {
    echo "читатель нашёл только $marker_timeout_count целей фазы" >&2
    exit 1
}
# The failure of 2026-09-15 is the drill: the same proof, with its record taken
# away, has to be reported.  The record is what the classifier reads, so a
# reader that does not miss it is the reader that would have retried that run.
marker_timeout_drill=$(mktemp)
trap 'rm -f "$marker_timeout_drill"' EXIT
cat >"$marker_timeout_drill" <<'DRILL'
        else:
            raise SystemExit(
                "timed out waiting for boot prompt, runtime diagnostic, "
                "and PIC disable diagnostic")
DRILL
if bash scripts/hfoc-proof-marker-class.sh check "$marker_timeout_drill" \
    >/dev/null 2>&1; then
    echo "доказательство без записи класса не отвергнуто" >&2
    exit 1
fi
cat >"$marker_timeout_drill" <<'DRILL'
        else:
            # ITEM-422: a proof that gives up on a marker says so.
            sys.stderr.write(
                'HFOC-QEMU-MARKER-TIMEOUT marker="boot prompt, runtime diagnostic" '
                'deadline=50s\n')
            sys.stderr.flush()
            raise SystemExit(
                "timed out waiting for boot prompt, runtime diagnostic, "
                "and PIC disable diagnostic")
DRILL
bash scripts/hfoc-proof-marker-class.sh check "$marker_timeout_drill" || {
    echo "доказательство с записью класса отвергнуто" >&2
    exit 1
}
# The harness is not the guest: a wait for a monitor or serial socket is not a
# marker timeout and must not be required to declare one.
cat >"$marker_timeout_drill" <<'DRILL'
    while not socket_path.exists():
        if time.monotonic() >= deadline:
            raise SystemExit("timed out waiting for QEMU monitor socket")
DRILL
bash scripts/hfoc-proof-marker-class.sh check "$marker_timeout_drill" || {
    echo "ожидание сокета монитора принято за маркер гостя" >&2
    exit 1
}
bash scripts/hfoc-proof-marker-class.sh check || {
    echo "не каждое доказательство фазы объявляет класс маркера" >&2
    exit 1
}
for marker_timeout_doc_422 in \
    'ITEM-422' \
    'HFOC-QEMU-MARKER-TIMEOUT'; do
    grep -Fq -- "$marker_timeout_doc_422" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-422" >&2
        exit 1
    }
done
grep -Fq 'ITEM-422' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "очередь не содержит ITEM-422" >&2
    exit 1
}

# ITEM-456: the installed-boot proof declares its own timing class, and a failed
# proof does not leave its guest running.
#
# Two load-induced failures on 2026-09-19 (176 s and 195 s, both on the ITEM-447
# menu phase of `test-production-installer-qemu`) exited with a bare
# `timed out waiting for ...` line.  That is not the strict record this slice
# defines, so the suite classified both as assertions, never retried them, and
# the phase failed on a healthy but starved guest.  The same failures left the
# QEMU process alive - the proof's own failure path returned before killing it -
# so the suite's leak check failed beside them.
for install_flow_class in \
    scripts/test-qemu-production-install-flow.sh \
    scripts/hfoc-qemu-failure-class.sh; do
    [[ -f $install_flow_class ]] || {
        echo "ITEM-456: отсутствует $install_flow_class" >&2
        exit 1
    }
done
grep -Fq -- "HFOC-QEMU-MARKER-TIMEOUT marker=" \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-456: проба установки не объявляет класс marker-timeout" >&2
    exit 1
}
grep -Fq -- 'def declare_marker_timeout(marker, timeout):' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-456: у пробы установки нет объявления класса таймаута" >&2
    exit 1
}
declared_waits=$(grep -c -F 'declare_marker_timeout(' \
    scripts/test-qemu-production-install-flow.sh || true)
[[ $declared_waits -ge 5 ]] || {
    echo "ITEM-456: класс таймаута объявляют не все ожидания ($declared_waits)" >&2
    exit 1
}
grep -Fq -- 'MENU_WAIT_SECONDS = 90.0' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-456: окно интерактивной фазы меню не расширено под нагрузку" >&2
    exit 1
}
grep -Fq -- 'wait_for_text(expected, MENU_WAIT_SECONDS)' \
    scripts/test-qemu-production-install-flow.sh || {
    echo "ITEM-456: ожидание записи о коммите адреса не использует окно" >&2
    exit 1
}
# The failure path kills the guest it started, and waits for it.
# shellcheck disable=SC2016  # the pattern is literal source text
grep -A 8 -F '# ITEM-456: a proof that failed must not leave its guest running.' \
    scripts/test-qemu-production-install-flow.sh | \
    grep -Fq 'kill -TERM "$qemu_pid" 2>/dev/null || true' || {
    echo "ITEM-456: упавшая проба не убивает своего гостя" >&2
    exit 1
}
# shellcheck disable=SC2016  # the pattern is literal source text
grep -A 9 -F '# ITEM-456: a proof that failed must not leave its guest running.' \
    scripts/test-qemu-production-install-flow.sh | \
    grep -Fq 'wait "$qemu_pid" 2>/dev/null || true' || {
    echo "ITEM-456: упавшая проба не дожидается убитого гостя" >&2
    exit 1
}
