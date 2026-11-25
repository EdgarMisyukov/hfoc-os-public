#!/usr/bin/env bash
# ITEM-598: the provisioning host creates a private bounded record from
# os.getrandom(), verifies its target/build binding, and refuses repository
# output without printing the seed or the complete record digest.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

work=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-host-seed-provision.XXXXXX")
trap 'rm -rf "$work"' EXIT

tool=scripts/hfoc-host-seed-provision.py
record=$work/host-seed.bin
wrong=$work/wrong-target.bin
corrupt=$work/corrupt.bin
inside=$ROOT/.host-seed-provision-must-refuse.bin

output=$(python3 "$tool" --output "$record" \
    --target TESTNVME000001 --build-id 7)
[[ $output == HFOC\ host\ seed:\ OK\ target=TESTNVME000001\ build=7\ \
record_bytes=120\ header_sha256=* ]] || {
    echo "unexpected provisioning record: $output" >&2
    exit 1
}
[[ $(stat -c %a "$record") == 600 ]] || {
    echo "host seed record is not private" >&2
    exit 1
}
[[ $(stat -c %s "$record") == 120 ]] || {
    echo "host seed record has the wrong length" >&2
    exit 1
}

python3 "$tool" --verify "$record" \
    --target TESTNVME000001 --build-id 7
if python3 "$tool" --verify "$record" \
    --target TESTNVME000002 --build-id 7 >/dev/null 2>&1; then
    echo "wrong target was accepted" >&2
    exit 1
fi
if python3 "$tool" --verify "$record" \
    --target TESTNVME000001 --build-id 8 >/dev/null 2>&1; then
    echo "wrong build id was accepted" >&2
    exit 1
fi

cp "$record" "$corrupt"
printf '\001' | dd of="$corrupt" bs=1 seek=119 conv=notrunc status=none
if python3 "$tool" --verify "$corrupt" \
    --target TESTNVME000001 --build-id 7 >/dev/null 2>&1; then
    echo "corrupt record was accepted" >&2
    exit 1
fi

if python3 "$tool" --output "$inside" \
    --target TESTNVME000001 --build-id 7 >/dev/null 2>&1; then
    echo "repository output was accepted" >&2
    exit 1
fi
[[ ! -e $inside ]] || {
    echo "repository refusal left a seed record behind" >&2
    exit 1
}

if python3 "$tool" --output "$wrong" \
    --target 'bad target' --build-id 7 >/dev/null 2>&1; then
    echo "invalid target was accepted" >&2
    exit 1
fi

echo "host seed provisioning test: OK"
