#!/usr/bin/env bash
# ITEM-564: the media writer's image gate on synthetic images.
#
# The gate must accept an image that carries its build id, refuse a wrong
# digest, an inactive MBR entry and a missing id, and it must do so on the same
# shape that exposed the old producer/consumer pipeline on the real medium: the
# id near the beginning of a large binary image.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
GATE=$ROOT/scripts/hfoc-media-image-check.sh
WORK=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-media-gate.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

pass() { echo "ok: $*"; }
fail() { echo "FAIL: $*" >&2; exit 1; }

make_image() { # <path> <build-id> [<tail-bytes>]
    local path=$1 build=$2 tail_bytes=${3:-4096}
    {
        head -c 512 /dev/zero
        printf '%s\n' "$build"
        head -c "$tail_bytes" /dev/zero | tr '\0' 'A'
    } >"$path"
    # MBR partition entry 1, boot indicator.
    printf '\200' | dd of="$path" bs=1 seek=446 conv=notrunc status=none
}

digest_of() { sha256sum "$1" | awk '{print $1}'; }

IMAGE=$WORK/image.iso
BUILD=ITEM-564-HOST-BUILD
make_image "$IMAGE" "$BUILD"
DIGEST=$(digest_of "$IMAGE")

# 1. The correct image passes and names the build id.
out=$(bash "$GATE" --image "$IMAGE" --expect-sha "$DIGEST" --expect-build "$BUILD")
grep -Fq "build:  $BUILD (present in the image)" <<<"$out" ||
    fail "the accepted image did not report its build id"
pass "the gate accepts an image that carries its build id"

# 2. A wrong digest is refused before any other property is trusted.
if out=$(bash "$GATE" --image "$IMAGE" --expect-sha "$(printf '0%.0s' {1..64})" 2>&1); then
    fail "the gate accepted a wrong digest"
fi
grep -Fq "the image does not match its expected digest" <<<"$out" ||
    fail "the wrong-digest refusal does not name the digest"
pass "the gate refuses a wrong digest"

# 3. An inactive MBR entry is refused.
inactive=$WORK/inactive.iso
make_image "$inactive" "$BUILD"
printf '\000' | dd of="$inactive" bs=1 seek=446 conv=notrunc status=none
if out=$(bash "$GATE" --image "$inactive" --expect-sha "$(digest_of "$inactive")" 2>&1); then
    fail "the gate accepted an inactive MBR entry"
fi
grep -Fq "MBR entry 1 is not active (0x80)" <<<"$out" ||
    fail "the inactive-MBR refusal does not name the flag"
pass "the gate refuses an inactive MBR entry"

# 4. A missing build id is refused, and the refusal names the id.
if out=$(bash "$GATE" --image "$IMAGE" --expect-sha "$DIGEST" --expect-build ABSENT-ID 2>&1); then
    fail "the gate accepted an image without the build id"
fi
grep -Fq "the image does not carry build id ABSENT-ID" <<<"$out" ||
    fail "the missing-build refusal does not name the id"
pass "the gate refuses an image without the build id"

# 5. The shape that exposed the old pipeline on the real medium: a large binary
#    image whose id is at the beginning.  The gate reads the bytes itself and
#    must not depend on a producer's exit status; no assertion is made about
#    what `strings | grep -q` does on a particular host.
LARGE=$WORK/large.iso
make_image "$LARGE" "$BUILD" $((64 * 1024 * 1024))
LARGE_DIGEST=$(digest_of "$LARGE")
bash "$GATE" --image "$LARGE" --expect-sha "$LARGE_DIGEST" --expect-build "$BUILD" >/dev/null ||
    fail "the gate refused a large image that carries its build id"
pass "the large binary image is accepted"

# 6. Required arguments are still required.
if bash "$GATE" --image "$IMAGE" >/dev/null 2>&1; then
    fail "the gate accepted a call without a digest"
fi
pass "the gate still requires the expected digest"

# ITEM-593: run the export proof's actual preflight on the same large shape.
# A deliberately invalid journal stops after the identity gate and before QEMU.
mkdir -p "$WORK/bin"
cat >"$WORK/bin/qemu-system-x86_64" <<'QEMU_STUB'
#!/usr/bin/env bash
printf 'unexpected QEMU invocation\n' >"$HFOC_GATE_TEST_QEMU_CALLED"
exit 99
QEMU_STUB
chmod +x "$WORK/bin/qemu-system-x86_64"
if out=$(PATH="$WORK/bin:$PATH" HFOC_GATE_TEST_QEMU_CALLED="$WORK/qemu.called" \
    IMAGE="$IMAGE" EXPECTED_KERNEL="$LARGE" EXPECTED_BUILD_ID="$BUILD" \
    bash "$ROOT/scripts/test-qemu-diag-export.sh" 2>&1); then
    fail "export preflight accepted a deliberately invalid journal"
fi
grep -Fq 'has no readable reserved journal' <<<"$out" ||
    fail "export build-id gate refused a large kernel carrying the expected id"
[[ ! -e $WORK/qemu.called ]] || fail "preflight launched QEMU before validating the journal"
pass "export proof accepts a large kernel id before refusing its invalid journal"
if out=$(PATH="$WORK/bin:$PATH" HFOC_GATE_TEST_QEMU_CALLED="$WORK/qemu.called" \
    IMAGE="$IMAGE" EXPECTED_KERNEL="$LARGE" EXPECTED_BUILD_ID=ABSENT-ID \
    bash "$ROOT/scripts/test-qemu-diag-export.sh" 2>&1); then
    fail "export preflight accepted a missing kernel build id"
fi
grep -Fq 'does not carry build id ABSENT-ID' <<<"$out" ||
    fail "export build-id refusal does not name the absent id"
[[ ! -e $WORK/qemu.called ]] || fail "missing build id reached QEMU"
pass "export proof still refuses a kernel without the expected build id"

echo "media image gate: ALL OK"
