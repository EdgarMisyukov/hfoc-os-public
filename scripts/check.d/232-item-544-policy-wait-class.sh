#!/usr/bin/env bash
# ITEM-544: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# The third occurrence of `test-production-service-https-qemu` failing as
# `class=assertion reason=no-marker-timeout-record` (run 38903525304,
# 2026-10-16 13:28:31 UTC, job `verify (acceptance-qemu)`) had a concrete guest
# fact behind it: the after-boot policy proof read
#
#   [PRODUCTION-BOOT] status=APPLIANCE_PRODUCTION_BOOT_STATE_LOAD_FAILED
#       discovered=1 valid=1 selected=AHCI storage=UNAVAILABLE mode=FAIL_CLOSED
#
# so the stored policy never reached the gate and the record the proof waited
# for (`[PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY`) was never
# published.  Eight other boots of the same probe loaded state normally.
#
# The proof waited for that record in an inline loop that prints no canonical
# timeout record, and the suite infers the timing class from that record alone
# (it deliberately refuses to infer it from English text).  The failure was
# therefore read as an assertion and got no retry.
#
# This slice pins the fix:
#   * the policy-boot wait goes through `wait_for_record`, which prints the
#     canonical `HFOC-QEMU-MARKER-TIMEOUT marker="..." deadline=...s` when its
#     deadline expires, so a record that did not arrive in time takes the
#     documented one retry as the timing class it is;
#   * nothing is weakened: the record is still required, the failure still
#     names it, the kernel-panic message is still the fail-closed one and the
#     `[PRODUCTION-SECURITY]` dump is still printed for the operator;
#   * the canonical record is written in exactly one place, so the proof cannot
#     grow a second, private spelling of the class.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item544_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item544_root" || exit 1

item544_files=(
    scripts/test-qemu-production-service-https.sh
    scripts/hfoc-qemu-failure-class.sh
    scripts/run-qemu-suite.sh
    tests/test-qemu-suite-host.sh
    docs/ROADMAP.md
)
for item544_file in "${item544_files[@]}"; do
    [[ -f $item544_file ]] || {
        echo "ITEM-544: отсутствует $item544_file" >&2
        exit 1
    }
done

# 1. The policy-boot wait is the canonical bounded wait for its record, and it
#    names the log that boot writes (the phase-1 log is not this one).
item544_probe=scripts/test-qemu-production-service-https.sh
for item544_pin in \
    'POLICY_PUBLISHED_RECORD="[PRODUCTION-SECURITY] gate=PUBLISHED source=POLICY"' \
    'if ! wait_for_record "$POLICY_PUBLISHED_RECORD" 180 \' \
    'local log=${3:-$SERIAL_LOG}' \
    'FAILURE_MESSAGE="The policy boot published no gate=PUBLISHED source=POLICY record"' \
    'FAILURE_MESSAGE="Kernel panic during the after-boot policy proof"' \
    "grep -F 'PRODUCTION-SECURITY' \"\$POLICY_SERIAL\" >&2 || true"; do
    grep -Fq -- "$item544_pin" "$item544_probe" || {
        echo "ITEM-544: проба не несёт '$item544_pin'" >&2
        exit 1
    }
done
# The wait hands the helper the log this boot writes; the two lines are one
# call, so they are checked together.
if ! grep -A 1 -F -- 'if ! wait_for_record "$POLICY_PUBLISHED_RECORD" 180 \' \
        "$item544_probe" | grep -Fq -- '"$POLICY_SERIAL"'; then
    echo "ITEM-544: ожидание policy boot не называет свой журнал" >&2
    exit 1
fi

# 2. The inline policy loop is gone, and the class has exactly one spelling:
#    the helper's own canonical record.
if grep -Fq -- 'policy_deadline=' "$item544_probe"; then
    echo "ITEM-544: у policy boot остался свой инлайновый дедлайн" >&2
    exit 1
fi
# The bash half writes the canonical record in exactly one place.  (The probe's
# python halves write it for their own host-side waits; those are not this
# site's spelling and are pinned by their own slices.)
item544_canonical=$(grep -Fc -- \
    "printf 'HFOC-QEMU-MARKER-TIMEOUT marker=\"%s\" deadline=%ss\\n'" \
    "$item544_probe")
[[ $item544_canonical == 1 ]] || {
    echo "ITEM-544: bash-половина пишет каноническую запись" \
        "$item544_canonical раз, а не один" >&2
    exit 1
}
grep -Fq -- 'printf '"'"'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ss\n'"'"' \' \
    "$item544_probe" || {
    echo "ITEM-544: wait_for_record больше не пишет каноническую запись" >&2
    exit 1
}

# 3. The suite still classifies only that record as the timing class.
for item544_classifier in \
    "HFOC_QEMU_MARKER_TIMEOUT_RECORD='HFOC-QEMU-MARKER-TIMEOUT'" \
    'hfoc_qemu_failure_reason=no-marker-timeout-record' \
    "hfoc_qemu_failure_class='marker-timeout'"; do
    grep -Fq -- "$item544_classifier" scripts/hfoc-qemu-failure-class.sh || {
        echo "ITEM-544: классификатор потерял '$item544_classifier'" >&2
        exit 1
    }
done
grep -Fq 'QEMU suite: RETRY %s reason=marker-timeout attempt=2' \
    scripts/run-qemu-suite.sh || {
    echo "ITEM-544: повтор класса marker-timeout не зафиксирован" >&2
    exit 1
}

# 4. The queue keeps this item as history, the roadmap records it.  (ITEM-547
#    now owns the historical section; this slice's own ITEM-544 record is the
#    history section above and the roadmap entry below.)
item544_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 3 '^## Item ITEM-547$' "$item544_queue" | grep -Fq 'ITEM-547' || {
    echo "ITEM-544: очередь потеряла исторический раздел ITEM-547" >&2
    exit 1
}
grep -Fq '## Item ITEM-544' "$item544_queue" || {
    echo "ITEM-544: очередь потеряла исторический раздел ITEM-544" >&2
    exit 1
}
grep -Fq 'ITEM-544' docs/ROADMAP.md || {
    echo "ITEM-544: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
