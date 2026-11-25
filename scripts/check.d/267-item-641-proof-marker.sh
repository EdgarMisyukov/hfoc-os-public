#!/usr/bin/env bash
# ITEM-641: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# Kernel marker checks must read the file in one process.  Under
# `set -Eeuo pipefail`, `strings -a "$kernel" | grep -Fq "$marker"` returns 141
# when grep exits at the first match and strings is still writing: a present
# build id looked absent, and `if strings ... | grep -Fq forbidden; then
# refuse; fi` never refused a kernel that really carried the marker.
# shellcheck disable=SC2016

item641_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item641_root" || exit 1

for item641_file in \
    scripts/hfoc-file-marker.sh \
    scripts/test-qemu-diag-failure.sh \
    scripts/test-qemu-diag-export.sh \
    scripts/test-qemu-production-install-flow.sh \
    tests/media-write-image-check-host.sh \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item641_file ]] || {
        echo "ITEM-641: отсутствует $item641_file" >&2
        exit 1
    }
done

# 1. The shared gate is one fixed-string read, not a producer/consumer pipeline.
grep -Fq 'grep -aFq -- "$marker" "$file"' scripts/hfoc-file-marker.sh || {
    echo "ITEM-641: общий гейт маркера не читает файл одной командой grep" >&2
    exit 1
}
for item641_proof in \
    scripts/test-qemu-diag-failure.sh \
    scripts/test-qemu-diag-export.sh \
    scripts/test-qemu-production-install-flow.sh; do
    grep -Fq 'scripts/hfoc-file-marker.sh' "$item641_proof" || {
        echo "ITEM-641: $item641_proof не использует общий гейт маркера" >&2
        exit 1
    }
done

# 2. No executable kernel-marker producer/consumer pipeline may remain in the
#    check suite, and the proof preflights may not grow one either.  Quoted
#    Makefile pins in historical slices are source text, not commands.
if grep -RInE \
        '^[^#]*strings[[:space:]]+build/hfoc-kernel\.elf[^|]*\|[[:space:]]*grep[[:space:]]+-Fq' \
        scripts/check.d 2>/dev/null |
    grep -v '^scripts/check.d/267-item-641-proof-marker.sh:' |
    grep -v '^scripts/check.d/237-item-564-media-write-build-id.sh:'; then
    echo "ITEM-641: конвейер strings|grep, дававший 141, вернулся" >&2
    exit 1
fi
# The pipeline may also put the `|` at the end of the strings line and start
# the grep on the next one; both shapes end 141 under pipefail.
if grep -RInA1 'strings build/hfoc-kernel\.elf |$' scripts/check.d 2>/dev/null |
    grep -F 'grep -Fq' |
    grep -v '^scripts/check.d/267-item-641-proof-marker.sh:' |
    grep -v '^scripts/check.d/237-item-564-media-write-build-id.sh:'; then
    echo "ITEM-641: многострочный конвейер strings|grep, дававший 141, вернулся" >&2
    exit 1
fi
if grep -nE '^[^#]*strings[^|]*\|[[:space:]]*grep[[:space:]]+-Fq' \
        scripts/test-qemu-diag-failure.sh \
        scripts/test-qemu-diag-export.sh \
        scripts/test-qemu-production-install-flow.sh 2>/dev/null; then
    echo "ITEM-641: proof preflight снова читает маркер конвейером" >&2
    exit 1
fi
for item641_image in \
    scripts/check.d/04-https-peer-failure-throttle.sh \
    scripts/check.d/16-stage-task-irq1-route.sh \
    scripts/check.d/19-controlled-configuration-command.sh \
    scripts/check.d/22-entropy-diagnostics-privacy.sh; do
    grep -Fq 'scripts/hfoc-file-marker.sh' "$item641_image" || {
        echo "ITEM-641: $item641_image не использует общий гейт маркера" >&2
        exit 1
    }
done

# 3. The gate answers both directions on a large file, including a marker at
#    the very start and at the very end.  This is the production helper.
item641_work=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-marker.XXXXXX")
head -c 512 /dev/zero >"$item641_work/start.bin"
printf '%s\n' 'ITEM-641-MARKER' >>"$item641_work/start.bin"
head -c $((4 * 1024 * 1024)) /dev/zero | tr '\0' 'A' >>"$item641_work/start.bin"
head -c $((4 * 1024 * 1024)) /dev/zero | tr '\0' 'B' >"$item641_work/end.bin"
printf '%s\n' 'ITEM-641-END-MARKER' >>"$item641_work/end.bin"

bash scripts/hfoc-file-marker.sh "$item641_work/start.bin" 'ITEM-641-MARKER' || {
    echo "ITEM-641: гейт не нашёл маркер в начале большого файла" >&2
    rm -rf "$item641_work"
    exit 1
}
bash scripts/hfoc-file-marker.sh "$item641_work/end.bin" 'ITEM-641-END-MARKER' || {
    echo "ITEM-641: гейт не нашёл маркер в конце большого файла" >&2
    rm -rf "$item641_work"
    exit 1
}
if bash scripts/hfoc-file-marker.sh "$item641_work/start.bin" 'ITEM-641-ABSENT'; then
    echo "ITEM-641: гейт принял отсутствующий маркер" >&2
    rm -rf "$item641_work"
    exit 1
fi
rm -rf "$item641_work"

# 4. The item is recorded in the queue and the roadmap, and the historical
#    ITEM-640 section is still present.  Slice 52 owns the moving
#    `## Current item` heading; this slice pins only its own text and history.
item641_queue=docs/superpowers/plans/2026-09-11-appliance-productization-queue.md
grep -Fq '**ITEM-641 — a proof reads a kernel marker in one process.**' \
    "$item641_queue" || {
    echo "ITEM-641: очередь не называет текущий пункт" >&2
    exit 1
}
grep -A 4 '^## Item ITEM-640$' "$item641_queue" | grep -Fq 'ITEM-640' || {
    echo "ITEM-641: очередь потеряла исторический раздел ITEM-640" >&2
    exit 1
}
grep -Fq 'ITEM-641' docs/ROADMAP.md || {
    echo "ITEM-641: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
