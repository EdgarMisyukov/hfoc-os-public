#!/usr/bin/env bash
# ITEM-524 (pins ITEM-523): contracted slice of the HFOC OS check suite, sourced
#           in pinned order by scripts/check.sh and lintable on its own with
#           the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item524_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item524_root" || exit 1

# ITEM-524: ITEM-523's awaited paint samples stay awaited.
#
# ITEM-523 replaced the storage boot proof's fixed 12-second window for the
# `[LOOP-PHASES]` paint samples with a wait on the record: six samples, a
# 20-second stall bound, a 60-second cap.  It shipped without a slice of its
# own (the queue note `item-523-note.md` records that gap), so nothing in the
# suite stopped the wait from sliding back to a wall clock - and a wall clock is
# exactly what run `38264136090` lost this proof to on 2026-10-12, when the
# guest's own clock lagged the host's by roughly 7x under the acceptance
# phase's parallel load.  This slice pins:
#   * the wait's shape: the progress loop, the stall bound, the cap, the two
#     bounds checked against the record's own arrival time, and the absence of
#     the fixed window it replaced;
#   * the assertion ITEM-523 left untouched: three samples at least, at most
#     one interval whose painted-line count grew by more than two, and the same
#     qualifying filter (`turns >= 5`, `wake_hz >= 50`);
#   * the give-up: still the canonical `HFOC-QEMU-MARKER-TIMEOUT` record the
#     phase's one retry reads, still naming the bound that expired, and now
#     naming the count it did see;
#   * that the change stayed host-side: the kernel carries no such bound.
# The behavioural proof lives in the queue's `item-523-drill.py` (RED/GREEN
# against the block itself) and in the QEMU proof this slice pins.

for item524_file in \
    scripts/test-qemu-production-storage-boot.sh \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item524_file ]] || {
        echo "ITEM-524: отсутствует $item524_file" >&2
        exit 1
    }
done

item524_proof=scripts/test-qemu-production-storage-boot.sh

# 1. The wait is on the record, not on the clock.
for item524_wait in \
    'paint_stall = 20.0' \
    'paint_cap = 60.0' \
    'paint_started = time.monotonic()' \
    'paint_last = paint_started' \
    'paint_last = time.monotonic()' \
    'while len(paint_samples) < 6:' \
    'if paint_now - paint_last >= paint_stall:' \
    'if paint_now - paint_started >= paint_cap:'; do
    grep -Fq -- "$item524_wait" "$item524_proof" || {
        echo "ITEM-523: ожидание образцов больше не идёт по записи:" \
            "$item524_wait" >&2
        exit 1
    }
done
for item524_gone in \
    'paint_deadline = time.monotonic() + 12.0' \
    'while time.monotonic() < paint_deadline'; do
    if grep -Fq -- "$item524_gone" "$item524_proof"; then
        echo "ITEM-523: фиксированное окно ожидания вернулось:" \
            "$item524_gone" >&2
        exit 1
    fi
done

# 2. The assertion ITEM-523 left alone is still the one ITEM-477 wrote.
for item524_assert in \
    'if len(paint_samples) < 3:' \
    'if paint_jumps > 1:' \
    'the periodic records keep painting the screen ' \
    'int(candidate["turns"]) < 5 or \' \
    'int(candidate["wake_hz"]) < 50:'; do
    grep -Fq -- "$item524_assert" "$item524_proof" || {
        echo "ITEM-523: утверждение пробы ослаблено или снято:" \
            "$item524_assert" >&2
        exit 1
    }
done

# 3. The give-up is still the canonical timing record.
for item524_record in \
    "'HFOC-QEMU-MARKER-TIMEOUT marker=\"[LOOP-PHASES] with '" \
    "'paint_lines\" deadline=%ds\\n' % int(paint_deadline))" \
    'paint_deadline = paint_stall if paint_expired == "stall" \' \
    'paint_expired = "cap"' \
    'the phase record never published the paint samples ' \
    'qualifying records in '; do
    grep -Fq -- "$item524_record" "$item524_proof" || {
        echo "ITEM-523: отказ не называет класс и предел:" \
            "$item524_record" >&2
        exit 1
    }
done

# 4. The bound is the proof's, not the product's: the kernel carries no
#    `paint_stall`/`paint_cap`, so nothing about this wait can reach the build.
if grep -Rq -- 'paint_stall\|paint_cap' kernel/ 2>/dev/null; then
    echo "ITEM-523: предел ожидания просочился в ядро" >&2
    exit 1
fi

# 5. The runner document owns the flake history this item closed.
for item524_doc in \
    'ITEM-523' \
    'paint_stall' \
    'paint_cap' \
    'the paint samples are awaited, not timed'; do
    grep -Fq -- "$item524_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-523:" \
            "$item524_doc" >&2
        exit 1
    }
done
