#!/usr/bin/env bash
# ITEM-564: the image gate of the media writer, exercised without a device.
#
# The writer used to ask whether an image carried its build id with
#
#   strings -a <image> | grep -Fq <id>
#
# under `set -Eeuo pipefail`.  `grep -q` exits at the first match, `strings`
# then writes into a closed pipe and is killed by SIGPIPE, so the pipeline
# reports 141 even though the id is present.  On 2026-10-28 the gate refused the
# freshly built 0b0e04d9 medium for exactly this reason, before anything was
# written; the operator then wrote it without the optional `--expect-build`.
#
# This script is the gate itself: no lock, no device, no write.  It reads the
# image as binary text in one process (`grep -a`), so there is no producer to
# kill and no pipeline status to misread, and the host proof can drive it on a
# synthetic image.
#
# Usage:
#   scripts/hfoc-media-image-check.sh --image <iso> --expect-sha <sha256> \
#       [--expect-build <id>]
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

IMAGE=
EXPECT_SHA=
EXPECT_BUILD=

usage() {
    sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'
}

refuse() {
    echo "MEDIA STOP: $*" >&2
    exit 1
}

while (($# > 0)); do
    case $1 in
    --image) IMAGE=$2; shift 2 ;;
    --expect-sha) EXPECT_SHA=$2; shift 2 ;;
    --expect-build) EXPECT_BUILD=$2; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) refuse "unknown argument: $1" ;;
    esac
done

[[ -n $IMAGE ]] || refuse "--image is required"
[[ -n $EXPECT_SHA ]] || refuse "--expect-sha is required (a digest is not inferred)"
[[ -f $IMAGE ]] || refuse "$IMAGE is missing"

SIZE=$(stat -c%s "$IMAGE")
echo
echo "===== IMAGE ====="
echo "path:   $IMAGE"
echo "size:   $SIZE"
got=$(sha256sum "$IMAGE" | awk '{print $1}')
echo "sha256: $got"
echo "expect: $EXPECT_SHA"
[[ $got == "$EXPECT_SHA" ]] || refuse "the image does not match its expected digest"

mbr_entry=$(dd if="$IMAGE" bs=1 skip=446 count=4 status=none | od -An -tx1 | tr -s ' ')
echo "MBR entry 1:$mbr_entry"
case $mbr_entry in
*" 80"*) : ;;
*) refuse "MBR entry 1 is not active (0x80)" ;;
esac

if [[ -n $EXPECT_BUILD ]]; then
    # One process reads the image; nothing writes into a pipe that may close.
    grep -aFq -- "$EXPECT_BUILD" "$IMAGE" ||
        refuse "the image does not carry build id $EXPECT_BUILD"
    echo "build:  $EXPECT_BUILD (present in the image)"
fi
