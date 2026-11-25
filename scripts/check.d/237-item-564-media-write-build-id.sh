#!/usr/bin/env bash
# ITEM-564: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The media writer's build-id gate must read the image in one process.  The
# old `strings -a "$IMAGE" | grep -Fq "$EXPECT_BUILD"` pipeline reported 141
# (SIGPIPE) under `set -Eeuo pipefail` on the 0b0e04d9 medium, whose bytes did
# carry the id: `grep -q` exits at the first match and `strings` is killed while
# it is still writing.  The gate now lives in its own testable script and uses
# `grep -aFq` directly, so the host proof can drive it without a block device.
item564_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item564_root" || exit 1

for item564_file in \
    scripts/hfoc-media-write.sh \
    scripts/hfoc-media-image-check.sh \
    tests/media-write-image-check-host.sh \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item564_file ]] || {
        echo "ITEM-564: отсутствует $item564_file" >&2
        exit 1
    }
done

# 1. The gate is one fixed-string read of the image, not a producer/consumer
#    pipeline whose producer can be killed after a match.
grep -Fq 'grep -aFq -- "$EXPECT_BUILD" "$IMAGE"' \
    scripts/hfoc-media-image-check.sh || {
    echo "ITEM-564: гейт build id не читает образ одной командой grep" >&2
    exit 1
}
if grep -Fq 'strings -a "$IMAGE" | grep' scripts/hfoc-media-write.sh ||
    grep -Fq 'strings -a "$IMAGE" | grep' scripts/hfoc-media-image-check.sh; then
    echo "ITEM-564: конвейер strings|grep, дававший 141, вернулся" >&2
    exit 1
fi

# 2. The writer still takes the lock and its destructive checks; only the image
#    gate moved.  The helper is invoked before any write.
grep -Fq 'bash scripts/hfoc-media-image-check.sh "${image_check[@]}"' \
    scripts/hfoc-media-write.sh || {
    echo "ITEM-564: писатель не вызывает проверку образа" >&2
    exit 1
}
grep -Fq 'HFOC_MEDIA_WRITE_LOCKED' scripts/hfoc-media-write.sh || {
    echo "ITEM-564: писатель потерял общий lock" >&2
    exit 1
}

# 3. The host proof drives the production gate: accept, wrong digest, inactive
#    MBR, missing id, and the large-image shape that used to return 141.
bash tests/media-write-image-check-host.sh || {
    echo "ITEM-564: host-проба гейта образа не прошла" >&2
    exit 1
}

# 4. This item's own section is history now, and ITEM-547 stays history too;
#    the roadmap records the item.  The pin is its `## Item ITEM-564` section,
#    never the moving `## Current item` heading (ITEM-565).
item564_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -A 5 '^## Item ITEM-564$' "$item564_queue" | grep -Fq 'ITEM-564' || {
    echo "ITEM-564: очередь потеряла исторический раздел ITEM-564" >&2
    exit 1
}
grep -A 3 '^## Item ITEM-547$' "$item564_queue" | grep -Fq 'ITEM-547' || {
    echo "ITEM-564: очередь потеряла исторический раздел ITEM-547" >&2
    exit 1
}
grep -Fq '## ITEM-564' docs/ROADMAP.md || {
    echo "ITEM-564: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
