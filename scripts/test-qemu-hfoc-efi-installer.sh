#!/usr/bin/env bash
# OVMF proof for the bounded pre-kernel HFOC UEFI installer helper.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in cmp cp dd mcopy mdel mformat mkdir python3 \
    qemu-system-x86_64 timeout truncate; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
IMAGE=${IMAGE:-build/efi/hfoc-efi-installer.iso}
PAYLOAD=${PAYLOAD:-deps/limine/BOOTX64.EFI}

for path in "$OVMF_CODE" "$OVMF_VARS" "$IMAGE" "$PAYLOAD"; do
    [[ -f "$path" ]] || {
        echo "Missing required file: $path" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d)
cleanup() {
    local status=$?
    trap - EXIT INT TERM
    if (( status != 0 )) && [[ "${HFOC_KEEP_TEST_DIR:-0}" == 1 ]]; then
        echo "OVMF proof artifacts: $TEST_DIR" >&2
    else
        rm -rf "$TEST_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

DISK=$TEST_DIR/target.img
VARS=$TEST_DIR/vars.fd
SEED_LOG=$TEST_DIR/seed.log
HELPER_LOG=$TEST_DIR/helper.log
STARTUP=$TEST_DIR/startup.nsh
README_SOURCE=$TEST_DIR/README.TXT
UBUNTU_SOURCE=$TEST_DIR/SHIMX64.EFI
ESP_FIRST=2048
ESP_LAST=133119
# ITEM-405: the length of the ESP partition the GPT declares.  The volume the
# fixture formats has to describe *its partition*, not the whole image: the ESP
# bind refuses a volume whose boot sector claims sectors the partition does not
# have, because that is the stale-volume shape a replaced partition table
# leaves behind (the acceptance run's 33 MiB role held a boot sector describing
# the 100 MiB volume the previous table had).
ESP_SECTORS=$(( ESP_LAST - ESP_FIRST + 1 ))
ESP_OFFSET=$((ESP_FIRST * 512))

printf 'HFOC_EFI_HELPER_UNRELATED_README\n' >"$README_SOURCE"
printf 'HFOC_EFI_HELPER_UNRELATED_UBUNTU\n' >"$UBUNTU_SOURCE"

truncate -s 96M "$DISK"
python3 - "$DISK" "$ESP_FIRST" "$ESP_LAST" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
total_sectors = 196608
esp_first = int(sys.argv[2])
esp_last = int(sys.argv[3])
hfoc_first = 133120
hfoc_last = 196573
entries = [(esp_first, esp_last), (hfoc_first, hfoc_last)]
entry_types = [
    bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b"),
    bytes.fromhex("e144014c0d6e7a4bb49d2210333d4131"),
]

buf = bytearray(total_sectors * 512)
buf[446 + 4] = 0xEE
struct.pack_into("<I", buf, 446 + 8, 1)
struct.pack_into("<I", buf, 446 + 12, total_sectors - 1)
buf[510:512] = b"\x55\xaa"

array = bytearray(512)
for index, ((first, last), type_guid) in enumerate(zip(entries, entry_types)):
    entry = bytearray(128)
    entry[0:16] = type_guid
    entry[16:32] = bytes((index + 1,)) * 16
    struct.pack_into("<QQ", entry, 32, first, last)
    name = ("HFOC ESP" if index == 0 else "HFOC PERSIST").encode("utf-16le")
    entry[56:56 + len(name)] = name
    array[index * 128:(index + 1) * 128] = entry


def header(current_lba, backup_lba, entries_crc):
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
    struct.pack_into("<I", result, 88, entries_crc)
    result_crc = zlib.crc32(
        bytes(result[:16] + b"\0" * 4 + result[20:92])
    ) & 0xFFFFFFFF
    struct.pack_into("<I", result, 16, result_crc)
    return result


entries_crc = zlib.crc32(bytes(array)) & 0xFFFFFFFF
buf[512:1024] = header(1, total_sectors - 1, entries_crc)
buf[1024:1536] = array
buf[(total_sectors - 2) * 512:(total_sectors - 1) * 512] = array
buf[(total_sectors - 1) * 512:] = header(
    total_sectors - 1, 1, entries_crc
)
with open(path, "wb") as output:
    output.write(buf)
PY

# ITEM-405: -T sizes the filesystem to the ESP partition.  Without it mformat
# sizes the volume to the end of the *image*, so the boot sector declared
# 194560 sectors inside a 131072-sector partition - a volume that does not fit
# the partition it lives in, which the ESP bind now refuses.
mformat -i "$DISK"@@$ESP_OFFSET -F -T "$ESP_SECTORS" ::

# The secondary GPT metadata is rewritten after formatting, so the table on the
# disk is canonical whatever the formatter touched.
python3 - "$DISK" <<'PY'
import struct
import sys
import zlib

path = sys.argv[1]
total_sectors = 196608
with open(path, "r+b") as output:
    data = bytearray(output.read())
    array = bytes(data[1024:1536])
    entries_crc = zlib.crc32(array) & 0xFFFFFFFF

    header = bytearray(512)
    header[0:8] = b"EFI PART"
    struct.pack_into("<I", header, 8, 0x00010000)
    struct.pack_into("<I", header, 12, 92)
    struct.pack_into("<Q", header, 24, total_sectors - 1)
    struct.pack_into("<Q", header, 32, 1)
    struct.pack_into("<Q", header, 40, 34)
    struct.pack_into("<Q", header, 48, total_sectors - 34)
    header[56:72] = bytes.fromhex("0102030405060708090a0b0c0d0e0f10")
    struct.pack_into("<Q", header, 72, 2)
    struct.pack_into("<I", header, 80, 4)
    struct.pack_into("<I", header, 84, 128)
    struct.pack_into("<I", header, 88, entries_crc)
    result_crc = zlib.crc32(
        bytes(header[:16] + b"\0" * 4 + header[20:92])
    ) & 0xFFFFFFFF
    struct.pack_into("<I", header, 16, result_crc)
    data[(total_sectors - 2) * 512:(total_sectors - 1) * 512] = array
    data[(total_sectors - 1) * 512:] = header
    output.seek(0)
    output.write(data)
PY

mmd -i "$DISK"@@$ESP_OFFSET ::EFI
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/BOOT
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/UBUNTU
mmd -i "$DISK"@@$ESP_OFFSET ::EFI/HFOC
mcopy -i "$DISK"@@$ESP_OFFSET "$README_SOURCE" ::README.TXT
mcopy -i "$DISK"@@$ESP_OFFSET "$UBUNTU_SOURCE" ::EFI/UBUNTU/SHIMX64.EFI

cat >"$STARTUP" <<'EOF'
@echo -off
echo HFOC_EFI_HELPER_SEED_BEGIN
bcfg boot add 1 fs0:\EFI\UBUNTU\SHIMX64.EFI "Existing Ubuntu"
bcfg boot add 2 fs0:\EFI\UBUNTU\SHIMX64.EFI "Existing Debian"
echo HFOC_EFI_HELPER_SEED_DONE
EOF
mcopy -i "$DISK"@@$ESP_OFFSET "$STARTUP" ::startup.nsh

cp "$OVMF_VARS" "$VARS"
if ! timeout 12s qemu-system-x86_64 \
    -machine q35 -m 256M -display none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$DISK",format=raw,if=ide,index=0,media=disk \
    -serial stdio -no-reboot -no-shutdown \
    >"$SEED_LOG" 2>&1; then
    :
fi
grep -Fq "HFOC_EFI_HELPER_SEED_DONE" "$SEED_LOG" || {
    echo "OVMF proof: failed to seed unrelated boot entries" >&2
    cat "$SEED_LOG" >&2
    exit 1
}
mdel -i "$DISK"@@$ESP_OFFSET ::startup.nsh

cat >"$STARTUP" <<'EOF'
@echo -off
fs1:\EFI\BOOT\BOOTX64.EFI
echo HFOC_EFI_HELPER_LIMINE_RETURNED_1
echo HFOC_EFI_HELPER_BOOT_DUMP_1_BEGIN
bcfg boot dump -v
echo HFOC_EFI_HELPER_BOOT_DUMP_1_END
fs1:\EFI\BOOT\BOOTX64.EFI
echo HFOC_EFI_HELPER_LIMINE_RETURNED_2
echo HFOC_EFI_HELPER_BOOT_DUMP_2_BEGIN
bcfg boot dump -v
echo HFOC_EFI_HELPER_BOOT_DUMP_2_END
EOF
mcopy -i "$DISK"@@$ESP_OFFSET "$STARTUP" ::startup.nsh

if ! timeout 45s qemu-system-x86_64 \
    -machine q35 -m 256M -display none \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$DISK",format=raw,if=ide,index=0,media=disk \
    -cdrom "$IMAGE" \
    -serial stdio -no-reboot -no-shutdown \
    >"$HELPER_LOG" 2>&1; then
    :
fi
grep -Fq "[HFOC-EFI] begin" "$HELPER_LOG" || {
    echo "OVMF proof: helper did not start" >&2
    tail -n 200 "$HELPER_LOG" >&2
    exit 1
}
SELECTION_COUNT=$(grep -Fc \
    "selection=HFOC_EFI_INSTALLER_OK" "$HELPER_LOG" || true)
if (( SELECTION_COUNT != 2 )); then
    echo "OVMF proof: expected two successful helper selections" >&2
    tail -n 200 "$HELPER_LOG" >&2
    exit 1
fi

mapfile -t STATUS_LINES < <(grep -F \
    "status=HFOC_EFI_INSTALLER_OK boot_order_entries=" \
    "$HELPER_LOG")
if (( ${#STATUS_LINES[@]} != 2 )); then
    echo "OVMF proof: expected two persisted helper status lines" >&2
    tail -n 200 "$HELPER_LOG" >&2
    exit 1
fi

FIRST_ORDER_COUNT=
FIRST_ENTRY_COUNT=
SECOND_ORDER_COUNT=
SECOND_ENTRY_COUNT=
for index in 0 1; do
    if [[ ! "${STATUS_LINES[$index]}" =~ \
          boot_order_entries=([0-9]+)[[:space:]]+boot_entries=([0-9]+) ]]; then
        echo "OVMF proof: helper status omitted boot-variable counts" >&2
        tail -n 200 "$HELPER_LOG" >&2
        exit 1
    fi
    if (( BASH_REMATCH[1] != BASH_REMATCH[2] ||
          BASH_REMATCH[1] < 5 )); then
        echo "OVMF proof: helper boot-variable counts are inconsistent" >&2
        tail -n 200 "$HELPER_LOG" >&2
        exit 1
    fi
    if (( index == 0 )); then
        FIRST_ORDER_COUNT=${BASH_REMATCH[1]}
        FIRST_ENTRY_COUNT=${BASH_REMATCH[2]}
    else
        SECOND_ORDER_COUNT=${BASH_REMATCH[1]}
        SECOND_ENTRY_COUNT=${BASH_REMATCH[2]}
    fi
done
if (( FIRST_ORDER_COUNT != SECOND_ORDER_COUNT ||
      FIRST_ENTRY_COUNT != SECOND_ENTRY_COUNT )); then
    echo "OVMF proof: repeated helper launch changed boot-variable counts" >&2
    tail -n 200 "$HELPER_LOG" >&2
    exit 1
fi

for launch in 1 2; do
    grep -Fq "HFOC_EFI_HELPER_LIMINE_RETURNED_$launch" \
        "$HELPER_LOG" || {
        echo "OVMF proof: Limine launch $launch did not return" >&2
        tail -n 200 "$HELPER_LOG" >&2
        exit 1
    }
done

awk '
    /HFOC_EFI_HELPER_BOOT_DUMP_2_BEGIN/ {
        in_dump = 1
        next
    }
    /HFOC_EFI_HELPER_BOOT_DUMP_2_END/ {
        in_dump = 0
    }
    in_dump {
        print
    }
' "$HELPER_LOG" >"$TEST_DIR/final-boot-dump.txt"
grep -Fq "HFOC_EFI_HELPER_BOOT_DUMP_2_END" "$HELPER_LOG" || {
    echo "OVMF proof: final boot-variable dump is missing" >&2
    tail -n 200 "$HELPER_LOG" >&2
    exit 1
}
if [[ $(grep -Fc "HFOC OS" "$TEST_DIR/final-boot-dump.txt" || true) -ne 1 ||
      $(grep -Fc "Existing Ubuntu" "$TEST_DIR/final-boot-dump.txt" || true) -ne 1 ||
      $(grep -Fc "Existing Debian" "$TEST_DIR/final-boot-dump.txt" || true) -ne 1 ]]; then
    echo "OVMF proof: boot entries are duplicated or missing" >&2
    cat "$TEST_DIR/final-boot-dump.txt" >&2
    exit 1
fi

mcopy -i "$DISK"@@$ESP_OFFSET \
    ::EFI/HFOC/BOOTX64.EFI "$TEST_DIR/hfoc.extracted"
mcopy -i "$DISK"@@$ESP_OFFSET \
    ::README.TXT "$TEST_DIR/readme.extracted"
mcopy -i "$DISK"@@$ESP_OFFSET \
    ::EFI/UBUNTU/SHIMX64.EFI "$TEST_DIR/ubuntu.extracted"
cmp "$PAYLOAD" "$TEST_DIR/hfoc.extracted"
cmp "$README_SOURCE" "$TEST_DIR/readme.extracted"
cmp "$UBUNTU_SOURCE" "$TEST_DIR/ubuntu.extracted"

echo "HFOC EFI installer OVMF QEMU proof: ALL OK"
