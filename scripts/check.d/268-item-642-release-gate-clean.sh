#!/usr/bin/env bash
# ITEM-642: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The product release gate compares two kernel/ISO builds.  It must build the
# first artifact from a clean tree too: the old `test-product-release-gate: iso`
# prerequisite ran an incremental `iso` before the recipe's first capture, so
# the first build could carry stale objects and the comparison was not a clean
# reproducibility proof.  The recipe now owns both `make clean` steps.
# shellcheck disable=SC2016

item642_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item642_root" || exit 1

# 1. No `iso` prerequisite: the recipe must not let make build the first image
#    before it has cleaned.
item642_target=$(awk '/^test-product-release-gate:/ { print; exit }' Makefile)
[[ $item642_target == 'test-product-release-gate:' ]] || {
    echo "ITEM-642: test-product-release-gate имеет prerequisite: $item642_target" >&2
    exit 1
}

# 2. The recipe body, from the target line to the next non-tab line.
item642_recipe=$(awk '
    /^test-product-release-gate:/ { inside = 1; next }
    inside && /^[^\t]/ { exit }
    inside { print }
' Makefile)

mapfile -t item642_clean_lines < <(
    printf '%s\n' "$item642_recipe" | grep -n 'make clean' | cut -d: -f1
)
mapfile -t item642_capture_lines < <(
    printf '%s\n' "$item642_recipe" |
        grep -n 'capture-reproducible-image-digests.sh' | cut -d: -f1
)
(( ${#item642_clean_lines[@]} >= 2 )) || {
    echo "ITEM-642: рецепт не делает make clean перед обоими build" >&2
    exit 1
}
(( ${#item642_capture_lines[@]} >= 2 )) || {
    echo "ITEM-642: рецепт не снимает оба digest-манифеста" >&2
    exit 1
}
(( item642_clean_lines[0] < item642_capture_lines[0] )) || {
    echo "ITEM-642: первый capture идёт до первого make clean" >&2
    exit 1
}
(( item642_clean_lines[1] < item642_capture_lines[1] )) || {
    echo "ITEM-642: второй capture идёт до второго make clean" >&2
    exit 1
}
(( item642_clean_lines[0] < item642_clean_lines[1] )) || {
    echo "ITEM-642: два make clean идут не по порядку" >&2
    exit 1
}
printf '%s\n' "$item642_recipe" |
    grep -Fq 'verify-product-release-gate.sh build/release' || {
    echo "ITEM-642: рецепт потерял финальный product release gate" >&2
    exit 1
}

# 3. The item is recorded in the queue and the roadmap without taking over the
#    moving `## Current item` heading (slice 52 owns it).
item642_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '**ITEM-642 — the release gate starts from a clean build.**' \
    "$item642_queue" || {
    echo "ITEM-642: очередь не называет текущий пункт" >&2
    exit 1
}
grep -A 4 '^## Item ITEM-641$' "$item642_queue" | grep -Fq 'ITEM-641' || {
    echo "ITEM-642: очередь потеряла исторический раздел ITEM-641" >&2
    exit 1
}
grep -Fq 'ITEM-642' docs/ROADMAP.md || {
    echo "ITEM-642: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
