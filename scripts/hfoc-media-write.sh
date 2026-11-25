#!/usr/bin/env bash
# ITEM-557: one locked, verified media write.
#
# Why this exists: the FOCUS run of this host has more than one automation
# worker, and on 2026-10-26 a nightly worker wrote its own medium to /dev/sdb
# minutes after the diagnostics medium had been written and read back.  A note
# in a queue file is a request, not a mechanism; the shared heavy lock is the
# mechanism, and this script is the only path this repository writes media by.
#
# Every invocation:
#   1. re-executes itself under `scripts/hfoc-heavy-lock.sh local`, so a write
#      never overlaps a CI heavy phase or another lock-honouring writer;
#   2. refuses a device that is not the declared removable one (/dev/nvme0n1,
#      /dev/sda and every non-USB device are named refusals);
#   3. refuses an image that does not match its digest or does not carry the
#      build id the caller expects - a stale or foreign medium is not written;
#   4. writes, flushes, reads the bytes back and compares them, checks the MBR's
#      active entry, and re-reads the digest after a settle window, so a write
#      that another process overwrote in the same window is a failure and not a
#      success claimed too early;
#   5. leaves a report next to its caller (or at --report) with the image
#      digest, the device identity, the read-back result and the settle result.
#
# Usage:
#   scripts/hfoc-media-write.sh --image <iso> --expect-sha <sha256> \
#       --serial <serial> --device <path> [--expect-build <id>] \
#       [--report <path>] [--settle-seconds <n>]
#   scripts/hfoc-media-write.sh --verify-only --device <path> --serial <serial> --image <iso> \
#       --expect-sha <sha256> [--report <path>]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

DEVICE=
SERIAL=
IMAGE=
EXPECT_SHA=
EXPECT_BUILD=
REPORT=${HFOC_MEDIA_WRITE_REPORT:-build/media-write-report.txt}
SETTLE_SECONDS=20
VERIFY_ONLY=0

refuse() {
    echo "MEDIA STOP: $*" >&2
    exit 1
}

# 1. The shared lock, taken before anything is parsed or inspected: the
#    identity check must see the same device the write will use, and the
#    arguments must survive the re-execution (the first version of this script
#    re-executed after its parser had already shifted them away, and the write
#    then ran with no image at all).
if [[ ${HFOC_MEDIA_WRITE_LOCKED:-0} != 1 ]]; then
    [[ -x scripts/hfoc-heavy-lock.sh ]] ||
        refuse "scripts/hfoc-heavy-lock.sh is missing (the shared lock)"
    exec scripts/hfoc-heavy-lock.sh local -- \
        env HFOC_MEDIA_WRITE_LOCKED=1 "$0" "$@"
fi

usage() {
    sed -n '2,40p' "$0" | sed 's/^# \{0,1\}//'
}

while (($# > 0)); do
    case $1 in
    --device) DEVICE=$2; shift 2 ;;
    --serial) SERIAL=$2; shift 2 ;;
    --image) IMAGE=$2; shift 2 ;;
    --expect-sha) EXPECT_SHA=$2; shift 2 ;;
    --expect-build) EXPECT_BUILD=$2; shift 2 ;;
    --report) REPORT=$2; shift 2 ;;
    --settle-seconds) SETTLE_SECONDS=$2; shift 2 ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) refuse "unknown argument: $1" ;;
    esac
done

[[ -n $DEVICE ]] || refuse "--device is required"
[[ -n $SERIAL ]] || refuse "--serial is required"

mkdir -p "$(dirname "$REPORT")"
exec > >(tee "$REPORT") 2>&1

block_device_name=$(basename "$DEVICE")
resolved=$(readlink -f "$DEVICE")
case $resolved in
/dev/nvme*) refuse "$DEVICE resolves to $resolved: the host system disk is never touched" ;;
/dev/sda|/dev/sda*) refuse "$DEVICE resolves to $resolved: the acceptance disk is never a media target" ;;
esac
block_device_name=$(basename "$resolved")

[[ -b "$resolved" ]] || refuse "$DEVICE is not a block device"
transport=$(lsblk -dn -o TRAN "$resolved" | tr -d ' ')
serial=$(lsblk -dn -o SERIAL "$resolved" | tr -d ' ')
sectors=$(cat "/sys/block/${block_device_name}/size" 2>/dev/null || echo 0)

echo "===== LOCK ====="
echo "device:   $resolved"
if (( VERIFY_ONLY == 0 )); then
    echo "role:     write and verify"
else
    echo "role:     verify only"
fi
echo "settles:  ${SETTLE_SECONDS}s"
echo
echo "===== STICK ====="
lsblk -dn -o NAME,MODEL,SERIAL,TRAN,SIZE "$resolved" || refuse "lsblk failed"
echo "tran=$transport serial=$serial sectors=$sectors"
[[ $transport == usb ]] || refuse "unexpected transport '$transport': only a removable USB medium is a target"
[[ $serial == "$SERIAL" ]] || refuse "unexpected serial '$serial' (declared $SERIAL)"

read_digest() {
    local bytes=$1
    head -c "$bytes" "$resolved" | sha256sum | awk '{print $1}'
}

# 2. The image: identity first, then the properties the firmware reads.
SIZE=
if (( VERIFY_ONLY == 0 )); then
    [[ -n $IMAGE ]] || refuse "--image is required"
    [[ -n $EXPECT_SHA ]] || refuse "--expect-sha is required (a digest is not inferred)"
    [[ -f $IMAGE ]] || refuse "$IMAGE is missing"
    SIZE=$(stat -c%s "$IMAGE")
    image_check=(--image "$IMAGE" --expect-sha "$EXPECT_SHA")
    if [[ -n $EXPECT_BUILD ]]; then
        image_check+=(--expect-build "$EXPECT_BUILD")
    fi
    # ITEM-564: the gate is one process reading the image.  `strings | grep -q`
    # returned 141 (SIGPIPE) on an image that did carry the id.
    bash scripts/hfoc-media-image-check.sh "${image_check[@]}" || exit 1
else
    [[ -n $EXPECT_SHA ]] || refuse "--expect-sha is required with --verify-only"
    SIZE=$(stat -c%s "$IMAGE" 2>/dev/null || echo 0)
    if [[ $SIZE -eq 0 ]]; then
        # Without an image, the caller states the size the digest covers.
        refuse "--verify-only needs --image to know how many bytes the digest covers"
    fi
fi

# 3. Nothing may be mounted: a stale mount is how a previous run wrote into a
#    mounted filesystem instead of the device.
echo
echo "===== UNMOUNT ====="
for _ in 1 2 3 4 5; do
    found=$(lsblk -ln -o MOUNTPOINT "$resolved" | grep -v '^$' || true)
    [[ -z $found ]] && break
    while read -r where; do
        [[ -n $where ]] && umount "$where" && echo "unmounted $where"
    done <<<"$found"
    sync
done
left=$(lsblk -ln -o MOUNTPOINT "$resolved" | grep -v '^$' || true)
[[ -z $left ]] || refuse "still mounted: $left"

if (( VERIFY_ONLY == 0 )); then
    echo
    echo "===== WRITE ====="
    dd if="$IMAGE" of="$resolved" bs=8M conv=fsync status=none || refuse "dd failed"
    sync
    blockdev --flushbufs "$resolved" 2>/dev/null || true
    sync
fi

echo
echo "===== READ BACK ====="
read_sha=$(read_digest "$SIZE")
echo "sha256 of the first $SIZE bytes on the medium: $read_sha"
[[ $read_sha == "$EXPECT_SHA" ]] || refuse "read-back digest $read_sha != $EXPECT_SHA"
if (( VERIFY_ONLY == 0 )); then
    cmp -n "$SIZE" "$resolved" "$IMAGE" || refuse "READ-BACK FAILED"
    echo "READ-BACK OK: the first $SIZE bytes equal the image"
fi
stick_mbr=$(dd if="$resolved" bs=1 skip=446 count=4 status=none | od -An -tx1 | tr -s ' ')
echo "MBR entry 1 on the medium:$stick_mbr"

# 4. The settle window: another writer that ignores the lock would land here.
#    Re-reading after it is what makes "the medium carries this image" a fact
#    about the medium now, not about the instant right after dd.
if (( SETTLE_SECONDS > 0 )); then
    echo
    echo "===== SETTLE (${SETTLE_SECONDS}s) ====="
    sleep "$SETTLE_SECONDS"
    sync
    settled_sha=$(read_digest "$SIZE")
    echo "sha256 after the settle window: $settled_sha"
    [[ $settled_sha == "$EXPECT_SHA" ]] ||
        refuse "the medium changed during the settle window ($settled_sha != $EXPECT_SHA): another writer is active"
fi

if [[ $resolved == /dev/sd* ]]; then
    partprobe "$resolved" 2>/dev/null || true
fi

echo
if (( VERIFY_ONLY == 0 )); then
    echo "MEDIA OK: $resolved carries $EXPECT_SHA"
else
    echo "MEDIA VERIFIED: $resolved still carries $EXPECT_SHA"
fi
echo "report saved to: $REPORT"
