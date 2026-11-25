#!/usr/bin/env bash
# ITEM-441: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# The queue has exactly one current item, and it is the last section.
#
# The document grew one `## Current item` heading per item, so fourteen
# headings claimed to be the current one at the same time and the reader - the
# human and the worker that takes the next item's number from that section
# alike - could not tell the current item from its history.  Every earlier
# section is now titled `## Item ITEM-NNN` after the item it records, the
# heading is unique and last, and the sections themselves (the evidence and
# reasoning other slices pin by item number) are untouched.
#
# ITEM-565: the current item must also be ahead of the history and must not
# repeat a historical number, so a queue parked on a merged item fails instead
# of silently naming it again.  And an item slice must pin its own historical
# section, never this moving heading: the ITEM-564 slice pinned it, which made
# a historical check depend on whatever the queue was on and kept the queue
# parked on an item that had already been merged.
QUEUE_DOC=${HFOC_QUEUE_DOC:-docs/superpowers/plans/2026-09-11-appliance-productization-queue.md}
QUEUE_CHECK_DIR=${HFOC_QUEUE_CHECK_DIR:-scripts/check.d}
QUEUE_CHECK_SELF=52-queue-current-item.sh
QUEUE_CHECK_FIXTURE=238-item-565-queue-current-pin.sh

[[ -f $QUEUE_DOC ]] || {
    echo "ITEM-441: отсутствует $QUEUE_DOC" >&2
    exit 1
}
current_headings=$(grep -c '^## Current item$' "$QUEUE_DOC" || true)
[[ $current_headings == 1 ]] || {
    echo "ITEM-441: в очереди $current_headings текущих разделов, а не один" >&2
    exit 1
}
# The one current heading is the document's last `## ` heading: everything
# after it is that item's own subsections, never another section.
last_section=$(grep -n '^## ' "$QUEUE_DOC" | tail -1 | cut -d: -f1)
current_line=$(grep -n '^## Current item$' "$QUEUE_DOC" | cut -d: -f1)
[[ $last_section == "$current_line" ]] || {
    echo "ITEM-441: текущий раздел не последний в очереди" >&2
    exit 1
}
# And it names the item it is about, so the next number is readable from it.
current_item=$(awk '
    /^## Current item$/ { inside = 1; next }
    inside && /^## / { exit }
    inside { print }
' "$QUEUE_DOC" | grep -m1 -Eo 'ITEM-[0-9]+' || true)
[[ -n $current_item ]] || {
    echo "ITEM-441: текущий раздел не называет номер item'а" >&2
    exit 1
}
# The history is intact and titled, so a rewrite cannot drop a section the
# other slices refer to.
history_headings=$(grep -c '^## Item ITEM-' "$QUEUE_DOC" || true)
[[ $history_headings -ge 10 ]] || {
    echo "ITEM-441: история очереди потеряла разделы ($history_headings)" >&2
    exit 1
}
# ITEM-565: the current item is ahead of every historical item and is not one
# of them, so a queue that still names a merged item fails.
history_numbers=$(grep '^## Item ITEM-' "$QUEUE_DOC" |
    grep -Eo 'ITEM-[0-9]+' | sed 's/^ITEM-//' || true)
[[ -n $history_numbers ]] || {
    echo "ITEM-565: история очереди не называет ни одного номера" >&2
    exit 1
}
current_number=${current_item#ITEM-}
max_history=$(printf '%s\n' "$history_numbers" | sort -n | tail -1)
if printf '%s\n' "$history_numbers" | grep -Fxq "$current_number"; then
    echo "ITEM-565: текущий пункт ITEM-$current_number уже есть в истории" >&2
    exit 1
fi
(( current_number > max_history )) || {
    echo "ITEM-565: текущий пункт ITEM-$current_number не впереди истории (ITEM-$max_history)" >&2
    exit 1
}
# ITEM-565: only this slice may read the moving heading.  Every other slice
# pins its own `## Item ITEM-NNN` section, so advancing the queue cannot break
# a historical check.
for module in "$QUEUE_CHECK_DIR"/*.sh; do
    [[ ${module##*/} == "$QUEUE_CHECK_SELF" ]] && continue
    [[ ${module##*/} == "$QUEUE_CHECK_FIXTURE" ]] && continue
    if grep -Fq "'^## Current item\$'" "$module"; then
        echo "ITEM-565: ${module##*/} привязан к движущемуся разделу '## Current item'" >&2
        exit 1
    fi
done
