#!/usr/bin/env bash
# OVMF QEMU proof for the ITEM-350 ESP and UEFI boot-variable seam.
#
# The proof mounts a GPT ESP under the OVMF UEFI Shell, installs and updates
# \EFI\HFOC\BOOTX64.EFI, adds HFOC to BootOrder without deleting unrelated
# entries, and leaves deterministic host-visible markers and files behind.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in cp dd mcopy mformat mmd python3 qemu-system-x86_64 timeout \
    truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
[[ -f "$OVMF_CODE" ]] || {
    echo "Missing OVMF code image: $OVMF_CODE" >&2
    exit 1
}
[[ -f "$OVMF_VARS" ]] || {
    echo "Missing OVMF vars image: $OVMF_VARS" >&2
    exit 1
}

TEST_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

SERIAL_LOG=$TEST_DIR/serial.log
VARS=$TEST_DIR/vars.fd
DISK=$TEST_DIR/esp.img
READONLY_MARKER=$TEST_DIR/readonly.marker
UBUNTU_MARKER=$TEST_DIR/ubuntu.marker
PAYLOAD_V1=$TEST_DIR/payload-v1.efi
PAYLOAD_V2=$TEST_DIR/payload-v2.bin
STARTUP=$TEST_DIR/startup.nsh
ESP_OFFSET=$((2048 * 512))

if [[ -f deps/limine/BOOTX64.EFI ]]; then
    cp deps/limine/BOOTX64.EFI "$PAYLOAD_V1"
else
    printf 'HFOC_PAYLOAD_V1\n' >"$PAYLOAD_V1"
fi
printf 'HFOC_PAYLOAD_V2\n' >"$PAYLOAD_V2"
printf 'UNRELATED_ESP_README\n' >"$READONLY_MARKER"
printf 'UNRELATED_UBUNTU_PAYLOAD\n' >"$UBUNTU_MARKER"

# Build a small GPT disk. The ESP starts at LBA 2048 and is FAT32-formatted
# with mtools. mformat sizes the volume to the end of the image, so GPT backup
# metadata is rewritten after formatting.
truncate -s 64M "$DISK"
python3 - "$DISK" <<'PY'
import os
import struct
import sys
import zlib

path = sys.argv[1]
total_sectors = 131072
first_lba = 2048
last_lba = total_sectors - 34 - 1
buf = bytearray(total_sectors * 512)

buf[446 + 4] = 0xEE
struct.pack_into("<I", buf, 446 + 8, 1)
struct.pack_into("<I", buf, 446 + 12, total_sectors - 1)
buf[510:512] = b"\x55\xaa"

entry = bytearray(128)
entry[0:16] = bytes.fromhex("282a73c11ff8d211ba4b00a0c93ec93b")
entry[16:32] = bytes.fromhex("d4c3b2a1f6e51807")
struct.pack_into("<QQ", entry, 32, first_lba, last_lba)
name = "EFI SYSTEM".encode("utf-16le")
entry[56:56 + len(name)] = name

array = bytearray(512)
array[0:128] = entry
buf[2 * 512:3 * 512] = array


def header(current_lba, backup_lba, crc):
    result = bytearray(512)
    result[0:8] = b"EFI PART"
    struct.pack_into("<I", result, 8, 0x00010000)
    struct.pack_into("<I", result, 12, 92)
    struct.pack_into("<Q", result, 24, current_lba)
    struct.pack_into("<Q", result, 32, backup_lba)
    struct.pack_into("<Q", result, 40, 34)
    struct.pack_into("<Q", result, 48, total_sectors - 34)
    result[56:72] = bytes.fromhex("0102030405060708090a0b0c0d0e0f10")
    struct.pack_into("<Q", result, 72, 2)
    struct.pack_into("<I", result, 80, 4)
    struct.pack_into("<I", result, 84, 128)
    struct.pack_into("<I", result, 88, crc)
    header_crc = zlib.crc32(
        bytes(result[:16] + b"\0" * 4 + result[20:92])) & 0xFFFFFFFF
    struct.pack_into("<I", result, 16, header_crc)
    return result


array_crc = zlib.crc32(bytes(array)) & 0xFFFFFFFF
buf[512:1024] = header(1, total_sectors - 1, array_crc)
buf[(total_sectors - 2) * 512:(total_sectors - 1) * 512] = array
buf[(total_sectors - 1) * 512:] = header(
    total_sectors - 1, 1, array_crc)
with open(path, "wb") as output:
    output.write(buf)
PY

mformat -i "$DISK"@@$ESP_OFFSET -F ::
python3 - "$DISK" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
total_sectors = 131072
with open(path, "r+b") as f:
    data = bytearray(f.read())
    array = data[2 * 512:3 * 512]
    crc = zlib.crc32(bytes(array)) & 0xFFFFFFFF

    def rebuild_header(current_lba, backup_lba):
        result = bytearray(512)
        result[0:8] = b"EFI PART"
        struct.pack_into("<I", result, 8, 0x00010000)
        struct.pack_into("<I", result, 12, 92)
        struct.pack_into("<Q", result, 24, current_lba)
        struct.pack_into("<Q", result, 32, backup_lba)
        struct.pack_into("<Q", result, 40, 34)
        struct.pack_into("<Q", result, 48, total_sectors - 34)
        result[56:72] = bytes.fromhex("0102030405060708090a0b0c0d0e0f10")
        struct.pack_into("<Q", result, 72, 2)
        struct.pack_into("<I", result, 80, 4)
        struct.pack_into("<I", result, 84, 128)
        struct.pack_into("<I", result, 88, crc)
        result_crc = zlib.crc32(
            bytes(result[:16] + b"\0" * 4 + result[20:92])) & 0xFFFFFFFF
        struct.pack_into("<I", result, 16, result_crc)
        return result

    data[(total_sectors - 2) * 512:(total_sectors - 1) * 512] = array
    data[(total_sectors - 1) * 512:] = rebuild_header(
        total_sectors - 1, 1)
    f.seek(0)
    f.write(data)
PY

mmd -i "$DISK"@@$ESP_OFFSET ::EFI
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/BOOT
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/UBUNTU
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/STAGING
mcopy -i "$DISK"@@$ESP_OFFSET "$PAYLOAD_V1" ::EFI/BOOT/BOOTX64.EFI
mcopy -i "$DISK"@@$ESP_OFFSET "$UBUNTU_MARKER" ::EFI/UBUNTU/SHIMX64.EFI
mcopy -i "$DISK"@@$ESP_OFFSET "$PAYLOAD_V2" ::EFI/STAGING/HFOC_PAYLOAD.V2
mcopy -i "$DISK"@@$ESP_OFFSET "$READONLY_MARKER" ::README.TXT

cat >"$STARTUP" <<'EOF'
@echo -off
echo OVMF_EFI_PROOF_INSTALL_BEGIN
bcfg boot add 1 fs0:\EFI\UBUNTU\SHIMX64.EFI "Existing Ubuntu"
bcfg boot add 2 fs0:\EFI\BOOT\BOOTX64.EFI "Existing Debian"
echo OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_BEGIN
bcfg boot dump -v
echo OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_END
mkdir fs0:\EFI\HFOC
cp fs0:\EFI\BOOT\BOOTX64.EFI fs0:\EFI\HFOC\BOOTX64.EFI
echo OVMF_EFI_PROOF_INSTALL_DONE
cp fs0:\EFI\STAGING\HFOC_PAYLOAD.V2 fs0:\EFI\HFOC\BOOTX64.EFI
echo OVMF_EFI_PROOF_UPDATE_DONE
bcfg boot add 0 fs0:\EFI\HFOC\BOOTX64.EFI "HFOC OS"
echo OVMF_EFI_PROOF_BOOT_DUMP_AFTER_BEGIN
bcfg boot dump -v
echo OVMF_EFI_PROOF_BOOT_DUMP_AFTER_END
echo OVMF_EFI_PROOF_DONE
EOF
mcopy -i "$DISK"@@$ESP_OFFSET "$STARTUP" ::startup.nsh

# First boot without a disk persists the Shell as Boot0001 in BootOrder.
cp "$OVMF_VARS" "$VARS"
if ! timeout 12s qemu-system-x86_64 \
    -machine q35 -m 256M -display none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -serial stdio -no-reboot -no-shutdown \
    >"$TEST_DIR/seed.log" 2>&1; then
    :
fi
grep -Fq "UEFI Interactive Shell" "$TEST_DIR/seed.log" || {
    echo "OVMF proof: did not reach UEFI Shell during boot-order seed" >&2
    cat "$TEST_DIR/seed.log" >&2
    exit 1
}

# Second boot mounts the GPT ESP and runs startup.nsh from the Shell.
if ! timeout 45s qemu-system-x86_64 \
    -machine q35 -m 256M -display none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$DISK",format=raw,if=ide,index=0,media=disk \
    -serial stdio -no-reboot -no-shutdown \
    >"$SERIAL_LOG" 2>&1; then
    :
fi

for marker in \
    OVMF_EFI_PROOF_INSTALL_BEGIN \
    OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_BEGIN \
    OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_END \
    OVMF_EFI_PROOF_INSTALL_DONE \
    OVMF_EFI_PROOF_UPDATE_DONE \
    OVMF_EFI_PROOF_BOOT_DUMP_AFTER_BEGIN \
    OVMF_EFI_PROOF_BOOT_DUMP_AFTER_END \
    OVMF_EFI_PROOF_DONE; do
    grep -Fq "$marker" "$SERIAL_LOG" || {
        echo "OVMF proof: missing marker $marker" >&2
        tail -n 200 "$SERIAL_LOG" >&2
        exit 1
    }
done

python3 - "$SERIAL_LOG" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="ascii", errors="replace")
text = re.sub(r"\x1b\[[0-9;?]*[A-Za-z]", "", text)


def order_block(start, end):
    block = text.split(start, 1)[1].split(end, 1)[0]
    names = []
    for name in ("HFOC OS", "Existing Ubuntu", "Existing Debian"):
        if name in block:
            names.append(name)
    return names


before = order_block(
    "OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_BEGIN",
    "OVMF_EFI_PROOF_BOOT_DUMP_BEFORE_END")
after = order_block(
    "OVMF_EFI_PROOF_BOOT_DUMP_AFTER_BEGIN",
    "OVMF_EFI_PROOF_BOOT_DUMP_AFTER_END")

if before.index("Existing Ubuntu") >= before.index("Existing Debian"):
    raise SystemExit("unrelated BootOrder was reordered before HFOC apply")
if "HFOC OS" not in after:
    raise SystemExit("HFOC boot entry was not added")
if after.index("HFOC OS") >= after.index("Existing Ubuntu"):
    raise SystemExit("HFOC boot entry was not placed before unrelated entries")
if after.index("Existing Ubuntu") >= after.index("Existing Debian"):
    raise SystemExit("unrelated BootOrder changed after HFOC apply")
PY

mcopy -i "$DISK"@@$ESP_OFFSET ::README.TXT "$TEST_DIR/readme.extracted"
mcopy -i "$DISK"@@$ESP_OFFSET \
    ::EFI/UBUNTU/SHIMX64.EFI "$TEST_DIR/ubuntu.extracted"
mcopy -i "$DISK"@@$ESP_OFFSET \
    ::EFI/HFOC/BOOTX64.EFI "$TEST_DIR/hfoc.extracted"
cmp "$READONLY_MARKER" "$TEST_DIR/readme.extracted"
cmp "$UBUNTU_MARKER" "$TEST_DIR/ubuntu.extracted"
cmp "$PAYLOAD_V2" "$TEST_DIR/hfoc.extracted"

echo "OVMF EFI filesystem and UEFI NVRAM QEMU proof: ALL OK"
