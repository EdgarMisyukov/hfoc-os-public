#!/usr/bin/env bash
# ITEM-398 QEMU proof: the installed appliance boots next to the systems that
# are already on the machine.
#
# The product install flow used to hardcode the destructive whole-disk layout
# mode, so a run on a machine that already carried a system destroyed it.  This
# proof drives that exact run and the fixed one through the real flow - an
# authorised medium booted on UEFI firmware, an AHCI target disk that carries a
# synthetic foreign GPT and a foreign ESP - and compares the disk, the records
# and the boot the installed system reaches.
#
#   RED    the flow as it was, reproduced through the flow: the medium declares
#          `HFOC-INSTALL-LAYOUT=whole-disk`, which is the hardcoded mode the fixed
#          flow made an explicit operator choice.  Every foreign GPT entry is
#          replaced by the appliance's own four roles, the protective MBR is
#          rewritten, and the ESP role is *created* by this run: ITEM-405 gives
#          the role to the install that declares the destructive layout, so it is
#          formatted and then bound, and the machine's partition table, its ESP
#          assignment, its filesystem and its boot path are gone.  The record
#          says `existing_loader=FREE existing_config=FREE` because there was no
#          foreign path left to preserve when the set was staged, and the
#          before/after bytes of every foreign slot are recorded verbatim.  The
#          old volume is replaced, so the bytes an operator could still find in
#          sectors the new volume has not allocated are not evidence of anything:
#          a whole-disk install gives no guarantee about them.  The GREEN run
#          below is where the other system's bytes are the proof, because there
#          the ESP belongs to that system and is never formatted.
#   GREEN  the same medium with no layout declaration at all.  The target disk
#          carries a foreign system, so the flow preserves it: every foreign GPT
#          entry, the protective MBR and the foreign ESP files are byte for
#          byte what they were, the appliance's three roles land in the lowest
#          free slots, exactly one installed boot payload set is staged, the
#          preservation and menu records are published before the first write -
#          and the disk the flow installed boots on its own, with no installer
#          medium attached, to the ordinary appliance readiness record.
#
#   RELEASE  ITEM-501: the same foreign disk, installed from the operator medium
#          by a person.  The operator names the disk, accepts the disk warning,
#          then chooses one existing partition on the partition screen and
#          accepts the warning page drawn for it.  ITEM-506 increment 4 puts the
#          account screen after that accepted warning, so the person typed here
#          sets the login and the password too, and the run is required to read
#          the account record back before it promises `admin=SET`.  The run
#          removes exactly that GPT entry, leaves every other entry, the
#          protective MBR and every foreign ESP file byte for byte, and creates
#          the appliance's three roles in the space the given-up partition
#          occupied - and the disk still boots the installed appliance alone.
#
# The medium also carries the two paths a shared ESP usually already has
# (`\EFI\BOOT\BOOTX64.EFI` and `\EFI\BOOT\limine.conf`), so the GREEN run proves
# the occupied-path handling on the real flow: the occupant's bytes stay
# recoverable under the documented `.PRE` name and the record says where.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp truncate sha256sum head stat; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

if [[ "${HFOC_INSTALL_PRESERVE_TEST_INNER:-0}" != 1 ]]; then
    exec timeout --kill-after=5s --signal=TERM \
        "${HFOC_INSTALL_PRESERVE_TEST_TIMEOUT:-900}s" \
        env HFOC_INSTALL_PRESERVE_TEST_INNER=1 "$0" "$@"
fi

QEMU_MACHINE=${QEMU_MACHINE:-q35}
QEMU_SMP=${QEMU_SMP:-1}
PRESERVE_IMAGE=${PRESERVE_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-authorised.iso}
WHOLE_DISK_IMAGE=${WHOLE_DISK_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-whole-disk.iso}
# ITEM-501: the operator medium, whose first entry opens the storage screen.
CONSOLE_IMAGE=${CONSOLE_IMAGE:-build/production-installer/hfoc-os-$(<VERSION)-production-installer-console.iso}
AUTHORISED_TARGET=${HFOC_INSTALL_MEDIA_TARGET:-QM00001}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
# The installer policy requires at least 2 GiB of usable memory.
QEMU_MEMORY=${QEMU_MEMORY:-3072M}

test "${QEMU_MACHINE:-}" = q35 || {
    echo "Preserving install-flow test must set QEMU_MACHINE=q35" >&2
    exit 1
}
test "${QEMU_SMP:-}" = 1 || {
    echo "Preserving install-flow test must set QEMU_SMP=1" >&2
    exit 1
}
for path in "$PRESERVE_IMAGE" "$WHOLE_DISK_IMAGE" "$CONSOLE_IMAGE" "$OVMF_CODE" "$OVMF_VARS"; do
    [[ -f "$path" ]] || {
        echo "Missing required file: $path" >&2
        exit 1
    }
done

# The declared acceptance disk class: the same 234375000-sector media the
# ITEM-391 contract proof and the ITEM-396 fixture publish.
DISK_BYTES=$((234375000 * 512))
ESP_FIRST=2048
ESP_LENGTH=131072
# ITEM-405: the role the *whole-disk* layout declares at the same LBA.  The RED
# run replaces the 131072-sector foreign partition with this 67584-sector role,
# formats it, and binds that volume - so the formatted geometry is the role's,
# not the foreign partition's.
ROLE_ESP_FIRST=2048
ROLE_ESP_LENGTH=67584
# The foreign ESP geometry the synthetic volume is built with: 512-byte sectors,
# one sector per cluster, two 1024-sector FAT copies after 32 reserved sectors.
ESP_RESERVED=32
ESP_FAT_COUNT=2
ESP_SECTORS_PER_FAT=1024
ESP_ROOT_CLUSTER=2
ESP_EFI_CLUSTER=3
ESP_DEBIAN_CLUSTER=4
ESP_GRUB_CLUSTER=5
ESP_GRUB_CFG_CLUSTER=7
ESP_BOOT_CLUSTER=8
ESP_FALLBACK_CLUSTER=9
ESP_MENU_CLUSTER=11
ESP_GRUB_LENGTH=700
ESP_GRUB_CFG_LENGTH=11
ESP_MENU_LENGTH=15
# The foreign partitions: Debian's root, its data volume and one above 4 GiB.
ROOT_FIRST=$((ESP_FIRST + ESP_LENGTH))
ROOT_LENGTH=2097152
DATA_FIRST=$((ROOT_FIRST + ROOT_LENGTH))
DATA_LENGTH=1048576
HIGH_FIRST=8388608
HIGH_LENGTH=1000000

TEST_DIR=$(mktemp -d)
QEMU_LOG=$TEST_DIR/qemu.log
FAILURE_MESSAGE=

cleanup() {
    local status=$?
    trap - EXIT INT TERM
    set +e
    if (( status != 0 )); then
        local artifact_dir
        artifact_dir="build/test-artifacts/production-install-flow-preserve-$(date -u +%Y%m%dT%H%M%SZ)-$$"
        mkdir -p "$artifact_dir"
        cp -a "$TEST_DIR"/. "$artifact_dir/" 2>/dev/null || true
        printf '%s\n' "${FAILURE_MESSAGE:-Preserving install-flow test failed}" \
            >&2
        echo "Preserving-install artifacts: $artifact_dir" >&2
    fi
    rm -rf "$TEST_DIR"
    exit "$status"
}
trap cleanup EXIT
trap 'exit 124' TERM
trap 'exit 130' INT

# --------------------------------------------------------------- fixtures ---
# The foreign disk: a protective MBR with boot code, a primary and a backup GPT
# over 128 entries, an EFI System Partition that carries Debian's loader and its
# configuration plus the two paths a shared ESP usually has, and three Debian
# partitions.  The bytes are the machine's, and the proof compares them.
build_foreign_disk() {
    local disk=$1
    local state_dir=$2

    truncate -s "$DISK_BYTES" "$disk"
    python3 - "$disk" "$state_dir" \
        "$ESP_FIRST" "$ESP_LENGTH" "$ESP_RESERVED" "$ESP_FAT_COUNT" \
        "$ESP_SECTORS_PER_FAT" "$ESP_ROOT_CLUSTER" "$ESP_EFI_CLUSTER" \
        "$ESP_DEBIAN_CLUSTER" "$ESP_GRUB_CLUSTER" "$ESP_GRUB_CFG_CLUSTER" \
        "$ESP_BOOT_CLUSTER" "$ESP_FALLBACK_CLUSTER" "$ESP_MENU_CLUSTER" \
        "$ESP_GRUB_LENGTH" "$ESP_GRUB_CFG_LENGTH" "$ESP_MENU_LENGTH" \
        "$ROOT_FIRST" "$ROOT_LENGTH" "$DATA_FIRST" "$DATA_LENGTH" \
        "$HIGH_FIRST" "$HIGH_LENGTH" "$((DISK_BYTES / 512))" <<'PY'
import binascii
import json
import pathlib
import struct
import sys

(disk_path, state_dir, esp_first, esp_length, esp_reserved, esp_fat_count,
 esp_sectors_per_fat, esp_root, esp_efi, esp_debian, esp_grub, esp_grub_cfg,
 esp_boot, esp_fallback, esp_menu, esp_grub_length, esp_grub_cfg_length,
 esp_menu_length, root_first, root_length, data_first, data_length,
 high_first, high_length, media_lba) = (
    sys.argv[1], sys.argv[2],
    *[int(value) for value in sys.argv[3:]])

SECTOR = 512
ESP_TYPE = bytes.fromhex("28732ac11ff8d211ba4b00a0c93ec93b")
LINUX_TYPE = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
ENTRY_COUNT = 128
ENTRY_SIZE = 128
ARRAY_SECTORS = ENTRY_COUNT * ENTRY_SIZE // SECTOR
esp_data_start = esp_reserved + esp_fat_count * esp_sectors_per_fat


def crc32(data):
    return binascii.crc32(data) & 0xFFFFFFFF


def guid_text(raw):
    return "%08x-%04x-%04x-%s-%s" % (
        struct.unpack_from("<I", raw, 0)[0],
        struct.unpack_from("<H", raw, 4)[0],
        struct.unpack_from("<H", raw, 6)[0],
        raw[8:10].hex(), raw[10:16].hex())


def entry(slot, type_guid, first, last, name):
    out = bytearray(ENTRY_SIZE)
    out[0:16] = type_guid
    for index in range(16):
        out[16 + index] = 0x70 + slot
    struct.pack_into("<Q", out, 32, first)
    struct.pack_into("<Q", out, 40, last)
    for index, character in enumerate(name[:35]):
        out[56 + index * 2] = ord(character)
    return bytes(out)


def header(my_lba, alternate_lba, entry_lba, array, signature):
    out = bytearray(SECTOR)
    out[0:8] = b"EFI PART"
    out[10] = 0x01
    struct.pack_into("<I", out, 12, 92)
    struct.pack_into("<Q", out, 24, my_lba)
    struct.pack_into("<Q", out, 32, alternate_lba)
    struct.pack_into("<Q", out, 40, 34)
    struct.pack_into("<Q", out, 48, media_lba - 34)
    out[56:72] = bytes((0x50 + index) & 0xFF for index in range(16))
    struct.pack_into("<Q", out, 72, entry_lba)
    struct.pack_into("<I", out, 80, ENTRY_COUNT)
    struct.pack_into("<I", out, 84, ENTRY_SIZE)
    struct.pack_into("<I", out, 88, crc32(array))
    struct.pack_into("<I", out, 16, crc32(bytes(out[0:92])))
    return bytes(out)


def fat_name(name, extension):
    out = bytearray(b" " * 11)
    for index, character in enumerate(name[:8]):
        out[index] = ord(character)
    for index, character in enumerate(extension[:3]):
        out[8 + index] = ord(character)
    return bytes(out)


def dir_entry(name, extension, attributes, first_cluster, size):
    out = bytearray(32)
    out[0:11] = fat_name(name, extension)
    out[11] = attributes
    struct.pack_into("<H", out, 20, (first_cluster >> 16) & 0xFFFF)
    struct.pack_into("<H", out, 26, first_cluster & 0xFFFF)
    struct.pack_into("<I", out, 28, size)
    return bytes(out)


def short_checksum(short_name):
    total = 0
    for byte in short_name:
        total = (((total & 1) << 7) + (total >> 1) + byte) & 0xFF
    return total


def lfn_entry(name, short_name):
    """One VFAT long-name entry (the ordinal 0x41 form of a single part).

    `limine.conf` has a four-character extension, so it does not fit 8.3 and a
    real volume stores the long name plus the derived short entry.  The kernel's
    reader requires exactly this encoding: one entry whose ordinal carries the
    "last" bit, the checksum of the short name, and the name in UTF-16 with a
    0x0000 terminator."""
    out = bytearray(b"\xff" * 32)
    out[0] = 0x41
    out[11] = 0x0F
    out[12] = 0x00
    out[13] = short_checksum(short_name)
    out[26] = 0x00
    out[27] = 0x00
    for index in range(13):
        if index < 5:
            position = 1 + index * 2
        elif index < 11:
            position = 14 + (index - 5) * 2
        else:
            position = 28 + (index - 11) * 2
        if index < len(name):
            value = ord(name[index])
        elif index == len(name):
            value = 0x0000
        else:
            value = 0xFFFF
        out[position] = value & 0xFF
        out[position + 1] = (value >> 8) & 0xFF
    return bytes(out)


array = bytearray(ARRAY_SECTORS * SECTOR)
foreign = [
    (ESP_TYPE, esp_first, esp_first + esp_length - 1, "EFI System Partition"),
    (LINUX_TYPE, root_first, root_first + root_length - 1, "Debian GNU/Linux"),
    (LINUX_TYPE, data_first, data_first + data_length - 1, "Debian home"),
    (LINUX_TYPE, high_first, high_first + high_length - 1,
     "Debian above 4 GiB"),
]
for slot, (type_guid, first, last, name) in enumerate(foreign):
    array[slot * ENTRY_SIZE:(slot + 1) * ENTRY_SIZE] = entry(
        slot, type_guid, first, last, name)
array = bytes(array)

mbr = bytearray(SECTOR)
for index in range(446):
    mbr[index] = 0x40 + (index % 5)
mbr[440] = 0x5A
mbr[441] = 0x5A
mbr[446] = 0x00
mbr[447] = 0x00
mbr[448] = 0x02
mbr[449] = 0x00
mbr[450] = 0xEE
mbr[451] = 0xFF
mbr[452] = 0xFF
mbr[453] = 0xFF
struct.pack_into("<I", mbr, 454, 1)
struct.pack_into("<I", mbr, 458, 0xFFFFFFFF)
mbr[510] = 0x55
mbr[511] = 0xAA

# The EFI System Partition: a FAT32 volume with Debian's loader, Debian's
# configuration, and the two paths the installed boot set uses.
boot_sector = bytearray(SECTOR)
boot_sector[11:13] = struct.pack("<H", SECTOR)
boot_sector[13] = 1
boot_sector[14:16] = struct.pack("<H", esp_reserved)
boot_sector[16] = esp_fat_count
boot_sector[17:19] = struct.pack("<H", 0)
boot_sector[19:21] = struct.pack("<H", 0)
boot_sector[21] = 0xF8
boot_sector[22:24] = struct.pack("<H", 0)
boot_sector[24:26] = struct.pack("<H", 63)
boot_sector[26:28] = struct.pack("<H", 255)
boot_sector[28:32] = struct.pack("<I", esp_first)
boot_sector[32:36] = struct.pack("<I", esp_length)
struct.pack_into("<I", boot_sector, 36, esp_sectors_per_fat)
boot_sector[44:48] = struct.pack("<I", esp_root)
# No FSInfo sector and no backup boot sector: 0xFFFF is the documented "none",
# so no driver reads a sector that holds something else.
struct.pack_into("<H", boot_sector, 48, 0xFFFF)
struct.pack_into("<H", boot_sector, 50, 0xFFFF)
boot_sector[64] = 0x80
boot_sector[66] = 0x29
struct.pack_into("<I", boot_sector, 67, 0x12345678)
boot_sector[71:82] = b"NO NAME    "
boot_sector[82:90] = b"FAT32   "
boot_sector[510] = 0x55
boot_sector[511] = 0xAA

fat = bytearray(esp_sectors_per_fat * SECTOR)
fat[0:4] = struct.pack("<I", 0x0FFFFFF8)
fat[4:8] = struct.pack("<I", 0x0FFFFFFF)
for cluster, next_cluster in (
        (esp_root, 0x0FFFFFFF),
        (esp_efi, 0x0FFFFFFF),
        (esp_debian, 0x0FFFFFFF),
        (esp_grub, esp_grub + 1),
        (esp_grub + 1, 0x0FFFFFFF),
        (esp_grub_cfg, 0x0FFFFFFF),
        (esp_boot, 0x0FFFFFFF),
        (esp_fallback, esp_fallback + 1),
        (esp_fallback + 1, 0x0FFFFFFF),
        (esp_menu, 0x0FFFFFFF)):
    struct.pack_into("<I", fat, cluster * 4, next_cluster)

root_dir = bytearray(SECTOR)
root_dir[0:32] = dir_entry("EFI", "", 0x10, esp_efi, 0)
efi_dir = bytearray(SECTOR)
efi_dir[0:32] = dir_entry("DEBIAN", "", 0x10, esp_debian, 0)
efi_dir[32:64] = dir_entry("BOOT", "", 0x10, esp_boot, 0)
debian_dir = bytearray(SECTOR)
debian_dir[0:32] = dir_entry("GRUBX64", "EFI", 0x20, esp_grub, esp_grub_length)
debian_dir[32:64] = dir_entry("GRUB", "CFG", 0x20, esp_grub_cfg,
                              esp_grub_cfg_length)
boot_dir = bytearray(SECTOR)
boot_dir[0:32] = lfn_entry("limine.conf", fat_name("LIMINE~1", "CON"))
boot_dir[32:64] = dir_entry("LIMINE~1", "CON", 0x20, esp_menu,
                            esp_menu_length)
boot_dir[64:96] = dir_entry("BOOTX64", "EFI", 0x20, esp_fallback,
                            esp_grub_length)


def payload(length, seed):
    return bytes((seed + (index % 13)) & 0xFF for index in range(length))


def esp_chain_sectors(first_cluster, length):
    """The absolute LBAs one file's cluster chain occupies.

    The fixture writes through `put()`, which addresses the ESP from its first
    LBA, so the proof that reads the disk back has to add that first LBA: the
    numbers recorded here are the ones `disk[lba * SECTOR:...]` must use.
    """
    return [esp_first + esp_data_start + (first_cluster - 2) +
            index // SECTOR
            for index in range(0, length, SECTOR)]


def esp_chain_bytes(data):
    """The bytes the chain holds: `put_chain` pads every sector, so the stored
    reference is the padded content, not the file length."""
    return b"".join(data[index:index + SECTOR].ljust(SECTOR, b"\x00")
                    for index in range(0, len(data), SECTOR))


grub_bytes = payload(esp_grub_length, 0x90)
grub_cfg_bytes = b"grub config" + b"\x00" * (esp_grub_cfg_length - 11)
fallback_bytes = payload(esp_grub_length, 0xB0)
menu_bytes = b"foreign limine!" + b"\x00" * (esp_menu_length - 15)

with open(disk_path, "r+b") as handle:
    def put(relative_lba, data):
        handle.seek((esp_first + relative_lba) * SECTOR)
        handle.write(data)

    def put_chain(first_cluster, data):
        for index in range(0, len(data), SECTOR):
            put(esp_data_start + (first_cluster - 2) + index // SECTOR,
                data[index:index + SECTOR].ljust(SECTOR, b"\x00"))

    handle.seek(0)
    handle.write(mbr)
    handle.write(header(1, media_lba - 1, 2, array, 0x50))
    handle.write(array)
    handle.seek((media_lba - 1 - ARRAY_SECTORS) * SECTOR)
    handle.write(array)
    handle.seek((media_lba - 1) * SECTOR)
    handle.write(header(media_lba - 1, 1, media_lba - 1 - ARRAY_SECTORS,
                        array, 0x60))

    put(0, bytes(boot_sector))
    for index in range(esp_sectors_per_fat):
        put(esp_reserved + index, bytes(fat[index * SECTOR:(index + 1) * SECTOR]))
        put(esp_reserved + esp_sectors_per_fat + index,
            bytes(fat[index * SECTOR:(index + 1) * SECTOR]))
    put(esp_data_start + (esp_root - 2), bytes(root_dir))
    put(esp_data_start + (esp_efi - 2), bytes(efi_dir))
    put(esp_data_start + (esp_debian - 2), bytes(debian_dir))
    put(esp_data_start + (esp_boot - 2), bytes(boot_dir))
    put_chain(esp_grub, grub_bytes)
    put_chain(esp_grub_cfg, grub_cfg_bytes)
    put_chain(esp_fallback, fallback_bytes)
    put_chain(esp_menu, menu_bytes)

state = {
    # The bytes of every foreign GPT entry, verbatim, so both runs can record
    # the before and the after of each slot.
    "entry_bytes": [array[slot * ENTRY_SIZE:(slot + 1) * ENTRY_SIZE].hex()
                    for slot in range(len(foreign))],
    "entries": [[first, last, name]
                for (_type, first, last, name) in foreign],
    "spans": {
        "esp": [esp_first, esp_length],
        "root": [root_first, root_length],
        "data": [data_first, data_length],
        "high": [high_first, high_length],
    },
    "esp": {
        "first": esp_first,
        "length": esp_length,
        "data_start": esp_data_start,
        "clusters": {
            "root": esp_root,
            "efi": esp_efi,
            "debian": esp_debian,
            "grub": esp_grub,
            "grub_cfg": esp_grub_cfg,
            "boot": esp_boot,
            "fallback": esp_fallback,
            "menu": esp_menu,
        },
    },
    "esp_file_sectors": {
        "grub": esp_chain_sectors(esp_grub, len(grub_bytes)),
        "grub_cfg": esp_chain_sectors(esp_grub_cfg, len(grub_cfg_bytes)),
        "fallback": esp_chain_sectors(esp_fallback, len(fallback_bytes)),
        "menu": esp_chain_sectors(esp_menu, len(menu_bytes)),
        "debian_dir": esp_chain_sectors(esp_debian, SECTOR),
        "boot_dir": esp_chain_sectors(esp_boot, SECTOR),
    },
    "esp_file_bytes": {
        "grub": esp_chain_bytes(grub_bytes).hex(),
        "grub_cfg": esp_chain_bytes(grub_cfg_bytes).hex(),
        "fallback": esp_chain_bytes(fallback_bytes).hex(),
        "menu": esp_chain_bytes(menu_bytes).hex(),
    },
    "esp_dir_bytes": {
        "debian_dir": bytes(debian_dir).hex(),
        "boot_dir": bytes(boot_dir).hex(),
    },
    "mbr": bytes(mbr).hex(),
    "guid": guid_text(array[0:16]),
}
pathlib.Path(state_dir, "foreign.json").write_text(json.dumps(state))
print("[PRESERVE-TEST] foreign disk prepared")
PY
}

# snapshot_before DISK STATE_DIR LABEL
snapshot_before() {
    local disk=$1
    local state_dir=$2
    local label=$3

    python3 - "$disk" "$state_dir/foreign.json" "$label" <<'PY'
import hashlib
import json
import mmap
import pathlib
import sys

disk_path, state_path, label = sys.argv[1:4]
state = json.loads(pathlib.Path(state_path).read_text())
with open(disk_path, "rb") as handle:
    # The acceptance image is a 114 GiB sparse file, so it is never read whole:
    # the mapping faults only the pages this proof compares - the protective MBR,
    # the entry array and the ESP clusters - and every comparison below is the
    # same byte-for-byte comparison it would be on a materialised copy.
    disk = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)

SECTOR = 512
ENTRY_SIZE = 128


print(f"[PRESERVE-TEST] before {label} foreign slots:")
for index, entry in enumerate(state["entry_bytes"]):
    first, last, name = state["entries"][index]
    print(f"[PRESERVE-TEST]   slot{index} first_lba={first} last_lba={last} "
          f"name={name!r} bytes={entry}")
print("[PRESERVE-TEST] before mbr=" + disk[0:SECTOR].hex())
for name in ("grub", "grub_cfg", "fallback", "menu"):
    sectors = state["esp_file_sectors"][name]
    data = b"".join(disk[lba * SECTOR:(lba + 1) * SECTOR] for lba in sectors)
    digest = hashlib.sha256(data).hexdigest()
    print(f"[PRESERVE-TEST] before esp {name} sectors={sectors} "
          f"sha256={digest}")
PY
}

# --------------------------------------------------------------- qemu boot ---
# run_boot LABEL IMAGE DISK EXPECT OUTPUT_FILE
run_boot() {
    local label=$1
    local image=$2
    local disk=$3
    local expect=$4
    local serial_log=$TEST_DIR/$label.log
    local vars=$TEST_DIR/$label-vars.fd
    local qemu_pid output

    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none -cdrom "$image" \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=target,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=target,bus=ide.0,serial=$AUTHORISED_TARGET" \
        -serial "file:$serial_log" -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$expect" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import signal
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
expect = sys.argv[3]
deadline = time.monotonic() + 300.0


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


while time.monotonic() < deadline:
    text = serial_text()
    if expect in text:
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("Kernel panic before: " + expect)
    try:
        os.kill(qemu_pid, 0)
    except OSError:
        raise SystemExit("QEMU exited before: " + expect +
                         "\n" + serial_text()[-3000:])
    time.sleep(0.05)
else:
    raise SystemExit("timed out waiting for: " + expect +
                     "\n" + serial_text()[-3000:])

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    pass
PY
    ); then
        FAILURE_MESSAGE="$label: $output"
        printf '%s\n' "$output" >&2
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"
    return 0
}


# ITEM-501: run_release LABEL IMAGE DISK
#
# The operator entry, driven through the emulated keyboard: the person names the
# disk, accepts the disk warning, chooses an existing partition on the partition
# screen, accepts the warning page drawn for it and sets the account on the
# screen that warning opens.  The proof waits for each page before it presses
# the key that answers it, so a page that never appeared fails the run instead
# of being skipped.
run_release() {
    local label=$1
    local image=$2
    local disk=$3
    local serial_log=$TEST_DIR/$label.log
    local socket=$TEST_DIR/$label.monitor
    local vars=$TEST_DIR/$label-vars.fd
    local qemu_pid output

    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none \
        -drive "file=$image,if=none,id=release-cd,format=raw,media=cdrom,readonly=on" \
        -device "ide-cd,drive=release-cd,bus=ide.2,unit=0" \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=release-target,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=release-target,bus=ide.0,serial=$AUTHORISED_TARGET" \
        -serial "file:$serial_log" \
        -monitor "unix:$socket,server,nowait" \
        -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$socket" "$qemu_pid" 2>&1 "$ROOT/scripts/hfoc-serial-view.py" <<'PYDRIVER'
import os
import pathlib
import signal
import socket
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
socket_path = sys.argv[2]
qemu_pid = int(sys.argv[3])


def body():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


def wait_for(marker, seconds, what):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        text = body()
        if "*** HFOC KERNEL PANIC ***" in text:
            raise SystemExit("kernel panic waiting for " + what)
        if marker in text:
            return
        try:
            os.kill(qemu_pid, 0)
        except OSError:
            raise SystemExit("QEMU exited waiting for " + what + "\n" +
                             text[-3000:])
        time.sleep(0.1)
    raise SystemExit("timed out waiting for " + what + "\n" + body()[-3000:])


def open_monitor():
    end = time.monotonic() + 120.0
    while time.monotonic() < end:
        if pathlib.Path(socket_path).exists():
            try:
                monitor = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
                monitor.settimeout(5.0)
                monitor.connect(socket_path)
                received = b""
                while b"(qemu)" not in received:
                    chunk = monitor.recv(4096)
                    if not chunk:
                        raise ConnectionError("the QEMU monitor closed")
                    received += chunk
                return monitor
            except OSError:
                time.sleep(0.1)
                continue
        time.sleep(0.1)
    raise SystemExit("the QEMU monitor never answered")


def press(monitor, name):
    monitor.sendall(("sendkey " + name + "\n").encode("ascii"))
    received = b""
    while b"(qemu)" not in received:
        chunk = monitor.recv(4096)
        if not chunk:
            raise SystemExit("the QEMU monitor closed unexpectedly")
        received += chunk
    time.sleep(0.08)


def type_text(monitor, text):
    """Type a login or password line one key at a time (lower case, digits)."""
    for character in text:
        press(monitor, character)


wait_for("Select installation disk", 300.0, "the storage screen")
monitor = open_monitor()
press(monitor, "ret")
# ITEM-514: naming the disk opens the scheme screen, not a warning.  The disk
# carries a system, so the default is "alongside"; the operator wants the
# manual release, which is the fourth row (three DOWNs from the default).
wait_for("Choose the installation scheme", 120.0, "the scheme screen")
for _ in range(3):
    press(monitor, "down")
press(monitor, "ret")
wait_for("Give up one existing partition", 180.0, "the partition screen")
press(monitor, "ret")
wait_for("WARNING: THE DATA ON THIS PARTITION WILL BE DELETED", 120.0,
         "the partition warning page")
press(monitor, "ret")
# ITEM-514: the accepted partition opens the summary - the plan the writer will
# execute, with the released partition and the reused ESP on it - and ENTER
# there is the confirmation that used to be the disk warning's.
wait_for("Confirm the installation plan", 120.0, "the summary page")
wait_for("Will be deleted: partition 2", 120.0,
         "the summary naming the released partition")
press(monitor, "ret")
# ITEM-506 increment 4: the accepted plan opens the account screen, so the
# operator install sets the one account here.  The login and the password are
# typed on the real screen (the password is never echoed), and the run must
# read the record back before it promises `admin=SET` - so the release layout
# is proven to persist the account, and the password may not reach the log.
account_login = "installer1"
account_password = "hfocinstallpass"
wait_for("New login:", 120.0, "the installer account screen")
type_text(monitor, account_login)
press(monitor, "ret")
wait_for("New password:", 60.0, "the installer account password")
type_text(monitor, account_password)
press(monitor, "ret")
wait_for("Repeat new password:", 60.0, "the installer account confirmation")
type_text(monitor, account_password)
press(monitor, "ret")
wait_for("[PRODUCTION-INSTALL] admin=SET login=" + account_login +
         " persisted=VERIFIED", 180.0, "the committed operator account")
if account_password in body():
    raise SystemExit("the hidden account password reached the serial log")
print("[PRESERVE-TEST] RELEASE: the operator set the account on the screen")
wait_for("[PRODUCTION-INSTALL] scheme declared=none chosen=manual "
         "source=operator writes=0", 180.0, "the operator's scheme record")
wait_for("[PRODUCTION-INSTALL] layout mode=release reason=operator-release "
         "declared=0 released_index=2 chosen=manual", 180.0,
         "the release decision record")
wait_for("[PRODUCTION-INSTALL] applied", 600.0, "the completed install")
print("[PRESERVE-TEST] RELEASE: the operator gave up partition 2 on the "
      "partition warning page")
try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    pass
PYDRIVER
    ); then
        FAILURE_MESSAGE="$label: $output"
        printf '%s\n' "$output" >&2
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"
    return 0
}

# ------------------------------------------------------------ assertions ----
# assert_red DISK STATE_DIR
assert_red() {
    local disk=$1
    local state_dir=$2

    python3 - "$disk" "$state_dir/foreign.json" "$TEST_DIR/red.log" \
        "$ROLE_ESP_FIRST" "$ROLE_ESP_LENGTH" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import hashlib
import json
import mmap
import pathlib
import struct
import sys

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

disk_path, state_path, log_path, role_first, role_length = sys.argv[1:6]
role_first = int(role_first)
role_length = int(role_length)
state = json.loads(pathlib.Path(state_path).read_text())
text = SERIAL_VIEW.normalise(
    pathlib.Path(log_path).read_bytes().decode("ascii", errors="replace"))
with open(disk_path, "rb") as handle:
    # The acceptance image is a 114 GiB sparse file, so it is never read whole:
    # the mapping faults only the pages this proof compares - the protective MBR,
    # the entry array and the ESP clusters - and every comparison below is the
    # same byte-for-byte comparison it would be on a materialised copy.
    disk = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)

SECTOR = 512
ENTRY_SIZE = 128
entry_lba = struct.unpack_from("<Q", disk, SECTOR + 72)[0]
entry_count = struct.unpack_from("<I", disk, SECTOR + 80)[0]
entry_size = struct.unpack_from("<I", disk, SECTOR + 84)[0]


def require(condition, message):
    if not condition:
        raise SystemExit("RED: " + message)


def slot_bytes(index):
    start = entry_lba * SECTOR + index * ENTRY_SIZE
    return disk[start:start + ENTRY_SIZE]


print("[PRESERVE-TEST] RED layout decision: " + [
    line for line in text.splitlines()
    if "layout mode=" in line][0])
require("layout mode=whole-disk reason=declared-whole-disk declared=1" in text,
        "the declared whole-disk run did not publish its decision")

# The destructor really did destroy: not one foreign slot survives, and the
# before/after bytes of each one are recorded verbatim.  The foreign ESP happens
# to sit at LBA 2048, which is where the clean plan puts this appliance's own
# ESP, so slot 0 is *replaced* rather than zeroed - the byte comparison is
# against the foreign entry, not against zeroes.
print("[PRESERVE-TEST] RED foreign slots before -> after:")
destroyed = 0
for index in range(len(state["entries"])):
    before = state["entry_bytes"][index]
    after = slot_bytes(index).hex()
    print(f"[PRESERVE-TEST]   slot{index} before={before}")
    print(f"[PRESERVE-TEST]   slot{index} after ={after}")
    if after != before:
        destroyed += 1
require(destroyed == 4,
        f"the destructive run left {4 - destroyed} foreign entries intact")
new_first = set()
for index in range(entry_count):
    entry = slot_bytes(index)
    if entry[0:16] == b"\x00" * 16:
        continue
    first = struct.unpack_from("<Q", entry, 32)[0]
    last = struct.unpack_from("<Q", entry, 40)[0]
    new_first.add((first, last + 1 - first))
require((2048, 67584) in new_first and (69632, 131072) in new_first and
        (200704, 65536) in new_first and (266240, 65536) in new_first,
        f"the destructive run did not write the whole-disk layout: {sorted(new_first)}")
require(len(new_first) == 4,
        f"the destructive run left {len(new_first)} partitions, expected 4")
# The ESP role is created by this run, so ITEM-405 formats it: the volume on the
# disk is this installer's, it declares the role it lives in (and not the
# foreign volume's 131072 sectors), and `\EFI\BOOT` carries this run's loader at
# the documented fallback path.  The boot-entry record is where the operator
# reads that: there was no foreign path to move, so both paths are published as
# FREE - which is exactly what the fixed rule promises for a role the install
# owns.  A shared ESP is the other case and the GREEN run below is its proof.
print("[PRESERVE-TEST] RED mbr before=" + state["mbr"])
print("[PRESERVE-TEST] RED mbr after =" + disk[0:SECTOR].hex())
require(disk[0:SECTOR].hex() != state["mbr"],
        "the protective MBR survived the destructive run unchanged")
require("existing_loader=FREE" in text and "existing_config=FREE" in text,
        "the destructive run does not publish where the machine's own paths went")
require("payload_set=7" in text,
        "the destructive run did not stage the menu it built over the machine's "
        "ESP")
require("menu=\\EFI\\BOOT\\limine.conf entries=1 timeout=10s "
        "source=GENERATED" in text,
        "the destructive run did not install the generated configuration")
require("menu_reason=" not in text,
        "the destructive run published the sealed fallback for a generated menu")
# ITEM-409: the machine's only other system was on the disk this run erases, so
# the menu offers this appliance alone - and the record names that stage instead
# of leaving a single-entry menu unexplained.
red_menus = [line for line in text.splitlines()
             if line.startswith("[PRODUCTION-INSTALL] boot-menu ")]
require(len(red_menus) == 1,
        f"expected exactly one boot-menu record, got {len(red_menus)}")
require("state=GENERATED reason=appliance-only stage=target-erased" in
        red_menus[0],
        f"the RED run does not name the stage that decided: {red_menus[0]}")
# The target disk carries an ESP; the whole-disk install drops it on purpose, so
# the record names that disk's device state and reports no unreadable disk
# besides it.
require("d1=GPT/esp1/NO_DEVICE/0" in red_menus[0] and
        "disks=1" in red_menus[0] and "unreadable=0" in red_menus[0],
        f"the RED run misreports the machine it is installing to: "
        f"{red_menus[0]}")
print("[PRESERVE-TEST] RED boot-menu record: " + red_menus[0])

esp_first = role_first
role_boot = bytes(disk[esp_first * SECTOR:(esp_first + 1) * SECTOR])
role_total = struct.unpack_from("<I", role_boot, 32)[0]
print(f"[PRESERVE-TEST] RED esp role OEM={role_boot[3:11]!r} "
      f"total_sectors={role_total} role_length={role_length}")
require(role_boot[3:11] == b"HFOC    ",
        f"the ESP role was not formatted by the destructive run: "
        f"OEM {role_boot[3:11]!r}")
require(role_total == role_length,
        f"the formatted role declares {role_total} sectors, the role is "
        f"{role_length}")


# The tree is walked in the geometry the formatted role declares, not in the
# foreign volume's: the foreign clusters survive as unreferenced bytes, and a
# walk through the old geometry would be reading a volume that is no longer
# there.
role_reserved = struct.unpack_from("<H", role_boot, 14)[0]
role_spc = role_boot[13]
role_fat_count = role_boot[16]
role_spf = struct.unpack_from("<I", role_boot, 36)[0]
role_root = struct.unpack_from("<I", role_boot, 44)[0]
role_data_start = role_reserved + role_fat_count * role_spf


def directory_entries(cluster):
    first = esp_first + role_data_start + (cluster - role_root) * role_spc
    data = b"".join(disk[(first + index) * SECTOR:(first + index + 1) * SECTOR]
                    for index in range(role_spc))
    entries = {}
    for index in range(len(data) // 32):
        entry = data[index * 32:(index + 1) * 32]
        if entry[0] == 0x00:
            break
        if entry[0] == 0xE5 or entry[11] == 0x0F:
            continue
        short = entry[0:8].decode("ascii", "replace").rstrip()
        extension = entry[8:11].decode("ascii", "replace").rstrip()
        name = short + "." + extension if extension else short
        entries[name] = (struct.unpack_from("<I", entry, 28)[0],
                         struct.unpack_from("<H", entry, 26)[0] |
                         (struct.unpack_from("<H", entry, 20)[0] << 16))
    return entries


efi_directory = directory_entries(role_root).get("EFI")
require(efi_directory is not None,
        "the formatted role carries no \\EFI directory")
boot_directory = directory_entries(efi_directory[1]).get("BOOT")
require(boot_directory is not None,
        "the formatted role carries no \\EFI\\BOOT directory")
boot_entries = directory_entries(boot_directory[1])
print("[PRESERVE-TEST] RED \\EFI\\BOOT entries after=" + repr(boot_entries))
require(boot_entries.get("BOOTX64.EFI", (0, 0))[0] == 360448,
        "the documented fallback path does not name this run's loader")
# ITEM-409: the documented path now carries *this run's* configuration - the
# generated menu is the installed configuration for every install - while the
# machine's own bytes are gone with the volume this run formatted (the record
# says `existing_config=FREE`, because there was no path left to preserve).  The
# menu itself is read back from the finished image by `assert_red_menu` below.
require("LIMINE~1.CON" in boot_entries,
        "the run did not install its own configuration at the documented path")
for name in ("grub", "fallback", "menu"):
    sectors = state["esp_file_sectors"][name]
    data = b"".join(disk[lba * SECTOR:(lba + 1) * SECTOR] for lba in sectors)
    digest = hashlib.sha256(data).hexdigest()
    expected = hashlib.sha256(
        bytes.fromhex(state["esp_file_bytes"][name])).hexdigest()
    print(f"[PRESERVE-TEST] RED esp {name} sha256={digest} "
          f"(foreign {expected}, the role now carries this install's volume, "
          f"so those bytes are not the machine's any more)")
print("[PRESERVE-TEST] RED: the whole-disk mode destroyed the foreign machine; "
      "every foreign slot, the MBR and the formatted ESP role are recorded "
      "above")
PY
}

# assert_red_menu DISK
#
# ITEM-409: the destructive run installs a menu too.  It owns the disk, so the
# only system it could offer is the one it erases - and it offers none of it -
# but the appliance is still reached through a menu with a readable timeout
# rather than through a configuration with no menu at all.  Read out of the
# finished image, not from a record.
assert_red_menu() {
    local disk=$1
    local menu=$TEST_DIR/red-menu.conf

    python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'EFI\BOOT\limine.conf' >"$menu" || {
        echo "cannot read the generated menu from $disk" >&2
        return 1
    }
    python3 - "$menu" <<'PY'
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text()
lines = text.splitlines()


def require(condition, message):
    if not condition:
        raise SystemExit("RED menu: " + message)


require("timeout: 10" in lines,
        "the destructive run installed no readable timeout")
require("/HFOC OS" in lines,
        "the destructive run installed no appliance entry")
require("path: boot():/boot/hfoc-kernel.elf" in text,
        "the appliance entry does not name the staged kernel")
require("cmdline: hfoc.management=local-only" in text,
        "the appliance entry does not declare the medium's management scope")
require("protocol: efi" not in text,
        "the destructive run offered a system it is about to erase")
offered = [line for line in lines if line.startswith("/")]
require(len(offered) == 1,
        f"the destructive menu offers {len(offered)} entries: {offered}")
print("[PRESERVE-TEST] RED: the destructive run installed its own menu "
      "(one entry, timeout 10s) instead of a configuration with no menu")
PY
}

# assert_green DISK STATE_DIR
assert_green() {
    local disk=$1
    local state_dir=$2

    python3 - "$disk" "$state_dir/foreign.json" "$TEST_DIR/green.log" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import hashlib
import json
import mmap
import pathlib
import re
import struct
import sys

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

disk_path, state_path, log_path = sys.argv[1:4]
state = json.loads(pathlib.Path(state_path).read_text())
text = SERIAL_VIEW.normalise(
    pathlib.Path(log_path).read_bytes().decode("ascii", errors="replace"))
with open(disk_path, "rb") as handle:
    # The acceptance image is a 114 GiB sparse file, so it is never read whole:
    # the mapping faults only the pages this proof compares - the protective MBR,
    # the entry array and the ESP clusters - and every comparison below is the
    # same byte-for-byte comparison it would be on a materialised copy.
    disk = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)

SECTOR = 512
ENTRY_SIZE = 128


def require(condition, message):
    if not condition:
        raise SystemExit("GREEN: " + message)


def slot_bytes(index):
    start = 2 * SECTOR + index * ENTRY_SIZE
    return disk[start:start + ENTRY_SIZE]


# The decision, the preservation record and the menu are published before the
# layout record - which is read back from the GPT the writer produced.
require("layout mode=preserve reason=existing-system declared=0" in text,
        "the run did not take the preserving path by itself")
require("[INSTALLER-PRESERVE] plan " in text,
        "the preservation record was not published")
require("[PRODUCTION-INSTALL] state erase stores=" in text and
        "verify=CLEAN" in text,
        "the preserving install did not publish its state erase step")
require("preserved=4" in text and "added=3" in text and "esp_reused=1" in text,
        "the preservation record does not state what is kept and added")
require("[INSTALLER-BOOT-MENU] menu " in text,
        "the boot menu record was not published")
for first, second in (
        ("[PRODUCTION-INSTALL] layout mode=", "[INSTALLER-PRESERVE] plan "),
        ("[INSTALLER-PRESERVE] plan ", "[INSTALLER-LAYOUT] plan "),
        ("[INSTALLER-BOOT-MENU] menu ", "[INSTALLER-LAYOUT] plan ")):
    require(text.index(first) < text.index(second),
            f"{first!r} is not published before {second!r}")

# Every foreign GPT entry is what it was, byte for byte.
for index, entry in enumerate(state["entry_bytes"]):
    after = slot_bytes(index).hex()
    print(f"[PRESERVE-TEST] GREEN slot{index} before={entry}")
    print(f"[PRESERVE-TEST] GREEN slot{index} after ={after}")
    require(after == entry,
            f"foreign GPT entry {index} is not byte-identical")
print("[PRESERVE-TEST] GREEN: all four foreign GPT entries are byte-identical")
print("[PRESERVE-TEST] GREEN mbr after=" + disk[0:SECTOR].hex())
require(disk[0:SECTOR].hex() == state["mbr"],
        "the protective MBR was modified")

# The foreign ESP files are byte for byte what they were.
for name in ("grub", "grub_cfg", "fallback", "menu"):
    sectors = state["esp_file_sectors"][name]
    data = b"".join(disk[lba * SECTOR:(lba + 1) * SECTOR] for lba in sectors)
    digest = hashlib.sha256(data).hexdigest()
    expected = hashlib.sha256(
        bytes.fromhex(state["esp_file_bytes"][name])).hexdigest()
    print(f"[PRESERVE-TEST] GREEN esp {name} sha256={digest}")
    require(digest == expected,
            f"the foreign ESP file {name} was not preserved byte for byte")
require(disk[state["esp_file_sectors"]["debian_dir"][0] * SECTOR:
             (state["esp_file_sectors"]["debian_dir"][0] + 1) * SECTOR].hex() ==
        state["esp_dir_bytes"]["debian_dir"],
        "the foreign EFI\\DEBIAN directory was modified")

# The appliance's three roles landed in the lowest free slots.
added = []
for index in range(128):
    entry = slot_bytes(index)
    if entry[0:16] == b"\x00" * 16 or index < 4:
        continue
    added.append(index)
require(added[:3] == [4, 5, 6],
        f"the appliance roles are not in the lowest free slots: {added[:4]}")
require(len(added) == 3,
        f"the preserving run wrote {len(added)} partitions, expected 3")

# Exactly one installed boot payload set, and the record says what it took over.
records = [line for line in text.splitlines()
           if line.startswith("[PRODUCTION-INSTALL] boot-entry ")]
require(len(records) == 1,
        f"expected exactly one boot-entry record, got {len(records)}")
record = records[0]
for fragment in (
        "state=AVAILABLE",
        "mechanism=REMOVABLE_MEDIA_PATH",
        "path=\\EFI\\BOOT\\BOOTX64.EFI",
        "menu=\\EFI\\BOOT\\limine.conf entries=",
        " timeout=10s source=GENERATED",
        "payload_set=7",
        "existing_loader=PRESERVED",
        "existing_loader_bytes=700",
        "existing_loader_kept_at=\\EFI\\HFOC\\BOOTX64.PRE",
        "existing_config=PRESERVED",
        "existing_config_kept_at=\\EFI\\HFOC\\LIMINE.PRE",
        "verify=SEALED_MEDIA",
        "boot_entry=NONE",
):
    require(fragment in record, f"the boot-entry record is missing {fragment}: {record}")
require(record.count("payload_set=") == 1,
        "the run staged more than one payload set")
require("menu_reason=" not in record,
        "the record claims the sealed fallback for a generated menu")
_menu_entries = re.search(r"menu=\\EFI\\BOOT\\limine.conf entries=(\d+) "
                           r"timeout=10s source=GENERATED", record)
require(_menu_entries is not None,
        f"the record does not state the entries and timeout: {record}")
green_menus = [line for line in text.splitlines()
               if line.startswith("[PRODUCTION-INSTALL] boot-menu ")]
require(len(green_menus) == 1,
        f"expected exactly one boot-menu record, got {len(green_menus)}")
require("state=GENERATED reason=chainload-entry stage=chainload-entry" in
        green_menus[0],
        f"the GREEN run does not publish the chainload entry: {green_menus[0]}")
print("[PRESERVE-TEST] GREEN boot-menu record: " + green_menus[0])
print("[PRESERVE-TEST] GREEN menu entries: " + _menu_entries.group(1))
print("[PRESERVE-TEST] GREEN boot-entry record: " + record)
require(text.count("[PRODUCTION-INSTALL] esp payload=VERIFIED") == 1,
        "the run staged the ESP payload set more than once")
require("mode=preserve" in [
    line for line in text.splitlines()
    if "[PRODUCTION-INSTALL] esp payload=VERIFIED" in line][0],
    "the esp record does not name the preserving mode")
PY
}

# assert_installed_boot [LOG]
#
# ITEM-399: the disk the preserving run installed boots the *installed* kernel
# the medium carried - the product appliance build - with the management scope
# the medium declared, so no installer-build record and no QEMU-fixture record
# may appear here.
assert_installed_boot() {
    python3 - "${1:-$TEST_DIR/installed.log}" "$ROOT/scripts/hfoc-serial-view.py" <<'PY'
import pathlib
import re
import sys

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

text = SERIAL_VIEW.normalise(
    pathlib.Path(sys.argv[1]).read_bytes().decode("ascii", errors="replace"))
readiness = re.compile(
    r"\[APPLIANCE-BOOT\] runtime=READY identity=OK config=[A-Z_]+ "
    r"local_management=READY storage=READY "
    r"local_input=(?:AVAILABLE|UNAVAILABLE)")
required = [
    r"\[BOOT\] Bootloader: Limine ",
    r"\[BOOT-CHAIN\] verdict=ATTESTED",
    r"\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK "
    r"discovered=1 valid=1 selected=A[HC]CI storage=READY",
    readiness.pattern,
]
for pattern in required:
    if not re.search(pattern, text):
        raise SystemExit("the installed disk did not report " + pattern +
                         "\n" + text[-3000:])
if len(re.findall(readiness.pattern, text)) != 1:
    raise SystemExit("expected exactly one appliance readiness record")
if "*** HFOC KERNEL PANIC ***" in text:
    raise SystemExit("kernel panic booting the installed disk")
if "[PRODUCTION-INSTALL] " in text:
    raise SystemExit("the install flow ran again in the installed boot")
# ITEM-399: the installed kernel is the product build, never the installer.
installer_records = [line for line in text.splitlines()
                     if "[PRODUCTION-INSTALLER]" in line]
if installer_records:
    raise SystemExit(
        "the installed disk booted an installer build; it published: " +
        "; ".join(installer_records[:4]))
# ITEM-399: the installed configuration declares its management scope, so the
# bind record names the boot declaration and not the kernel's default.
binds = [line for line in text.splitlines()
         if line.startswith("[PRODUCTION-MGMT-BIND] ")]
if len(binds) != 1:
    raise SystemExit(
        f"expected exactly one management bind record, got {len(binds)}")
expected = ("[PRODUCTION-MGMT-BIND] scope=LOCAL_ONLY "
            "source=boot-declaration")
if not binds[0].startswith(expected):
    raise SystemExit(
        f"the installed boot does not publish {expected!r}: {binds[0]}")
print("[PRESERVE-TEST] GREEN: the installed disk booted alone to the ordinary "
      "appliance record with the sealed chain attested, the product kernel and "
      "the declared management scope")
print("[PRESERVE-TEST] GREEN: " + binds[0])
PY
}

# assert_installed_menu DISK ENTRIES
#
# ITEM-399: a preserving install has a system to chainload, so the generated
# menu is the configuration the loader reads - and that menu is where the
# installed appliance's command line has to be, otherwise the scope would be the
# default.  The menu is read out of the finished image, not from a record.
#
# ITEM-409: the same read pins the owner's rule for this case - the menu is the
# installed configuration, our system is the first entry (Limine's default) and
# therefore what a machine nobody touches boots, the timeout is one a person can
# read, and the system that was already on the machine is offered through the
# path of the loader that is really on the shared ESP.  ENTRIES is the count the
# boot-entry record published, and the menu has to match it.
assert_installed_menu() {
    local disk=$1
    local entries=$2
    local menu=$TEST_DIR/green-menu.conf

    python3 scripts/hfoc-esp-read.py --disk "$disk" \
        --path 'EFI\BOOT\limine.conf' >"$menu" || {
        echo "cannot read the generated menu from $disk" >&2
        return 1
    }
    python3 - "$menu" "$entries" <<'PY'
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text()
entries = int(sys.argv[2])
lines = text.splitlines()


def require(condition, message):
    if not condition:
        raise SystemExit("installed menu: " + message)


require("/HFOC OS\r\n    protocol: limine\r\n" in text or
        "/HFOC OS\n    protocol: limine\n" in text,
        "the generated menu has no appliance entry")
require("path: boot():/boot/hfoc-kernel.elf" in text,
        "the appliance entry does not name the staged kernel")
require("cmdline: hfoc.management=local-only" in text,
        "the appliance entry does not declare the medium's management scope")
require(text.count("cmdline:") == 1,
        "the generated menu declares more than one command line")
require("module_path: boot():/boot/hfoc-boot.man" in text and
        "module_path: boot():/boot/limine/limine.conf" in text,
        "the appliance entry does not carry the sealed chain's modules")
require("protocol: efi" in text,
        "the generated menu does not offer the system on the machine")
# ITEM-409: the system that was already there, through the loader that is on
# the shared ESP, and behind our own entry.
require("path: boot():/EFI/DEBIAN/GRUBX64.EFI" in text,
        "the menu does not offer the existing system's own loader")
require("timeout: 10" in lines,
        "the menu does not declare the documented ten-second timeout")
require(lines.index("/HFOC OS") ==
        min(index for index, line in enumerate(lines)
            if line.startswith("/")),
        "the appliance entry is not the first entry (Limine's default)")
offered = [line for line in lines if line.startswith("/")]
require(len(offered) == entries,
        f"the menu offers {len(offered)} entries, the record published "
        f"{entries}: {offered}")
print("[PRESERVE-TEST] GREEN: the installed configuration is the generated "
      "menu, with the declared management scope and the existing system's "
      "entry")
print("[PRESERVE-TEST] GREEN: menu entries " + "; ".join(offered) +
      " (timeout 10s, appliance first)")
PY
}

boot_installed_disk() {
    local disk=$1
    local serial_log=$TEST_DIR/installed.log
    local vars=$TEST_DIR/installed-vars.fd
    local qemu_pid output

    cp "$OVMF_VARS" "$vars"
    : >"$serial_log"
    qemu-system-x86_64 \
        -machine "$QEMU_MACHINE" -m "$QEMU_MEMORY" -smp "$QEMU_SMP" \
        -display none \
        -drive "if=pflash,format=raw,readonly=on,file=$OVMF_CODE" \
        -drive "if=pflash,format=raw,file=$vars" \
        -drive "if=none,id=installed,file=$disk,format=raw,cache=writeback,file.locking=off" \
        -device "ide-hd,drive=installed,bus=ide.0" \
        -serial "file:$serial_log" -no-reboot -no-shutdown \
        >"$QEMU_LOG" 2>&1 &
    qemu_pid=$!

    if ! output=$(python3 - "$serial_log" "$qemu_pid" "$ROOT/scripts/hfoc-serial-view.py" 2>&1 <<'PY'
import os
import pathlib
import re
import signal
import sys
import time

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

serial_path = pathlib.Path(sys.argv[1])
qemu_pid = int(sys.argv[2])
deadline = time.monotonic() + 300.0
readiness = re.compile(
    r"\[APPLIANCE-BOOT\] runtime=READY identity=OK config=[A-Z_]+ "
    r"local_management=READY storage=READY "
    r"local_input=(?:AVAILABLE|UNAVAILABLE)")
required = [
    r"\[BOOT-CHAIN\] verdict=ATTESTED",
    r"\[PRODUCTION-BOOT\] status=APPLIANCE_PRODUCTION_BOOT_OK "
    r"discovered=1 valid=1 selected=A[HC]CI storage=READY",
    readiness.pattern,
]


def serial_text():
    try:
        return SERIAL_VIEW.normalise(
            serial_path.read_bytes().decode("ascii", errors="replace"))
    except FileNotFoundError:
        return ""


while time.monotonic() < deadline:
    text = serial_text()
    if all(re.search(pattern, text) for pattern in required):
        break
    if "*** HFOC KERNEL PANIC ***" in text:
        raise SystemExit("kernel panic booting the installed disk")
    time.sleep(0.1)
else:
    missing = [pattern for pattern in required
               if not re.search(pattern, serial_text())]
    raise SystemExit("the installed disk did not boot; missing: " +
                     "; ".join(missing) + "\n" + serial_text()[-3000:])

try:
    os.kill(qemu_pid, signal.SIGTERM)
except ProcessLookupError:
    raise SystemExit("QEMU exited before the installed boot completed")
print("[PRESERVE-TEST] installed disk booted")
PY
    ); then
        FAILURE_MESSAGE="installed-disk: $output"
        printf '%s\n' "$output" >&2
        return 1
    fi
    wait "$qemu_pid" 2>/dev/null || true
    printf '%s\n' "$output"
    return 0
}


# ITEM-501: assert_release DISK STATE_DIR
#
# The operator gave up exactly one partition.  The proof compares the GPT entry
# array, the protective MBR and the foreign ESP bytes with the snapshot taken
# before the run, so "every other partition is untouched" is a byte comparison
# and not a record's claim - and it requires the three appliance roles to begin
# where the given-up partition began.
assert_release() {
    local disk=$1
    local state_dir=$2

    python3 - "$disk" "$state_dir/foreign.json" "$TEST_DIR/release.log" "$ROOT/scripts/hfoc-serial-view.py" <<'PYASSERT'
import hashlib
import json
import mmap
import pathlib
import sys

def load_serial_view(path):
    import importlib.util
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


SERIAL_VIEW = load_serial_view(sys.argv[-1])

disk_path, state_path, log_path = sys.argv[1:4]
state = json.loads(pathlib.Path(state_path).read_text())
text = SERIAL_VIEW.normalise(
    pathlib.Path(log_path).read_bytes().decode("ascii", errors="replace"))
with open(disk_path, "rb") as handle:
    # The image is sparse and 114 GiB, so the mapping faults only the pages this
    # proof compares - the MBR, the entry array and the ESP clusters.
    disk = mmap.mmap(handle.fileno(), 0, access=mmap.ACCESS_READ)

SECTOR = 512
ENTRY_SIZE = 128
RELEASED_SLOT = 1


def require(condition, message):
    if not condition:
        raise SystemExit("RELEASE: " + message)


def slot_bytes(index):
    start = 2 * SECTOR + index * ENTRY_SIZE
    return disk[start:start + ENTRY_SIZE]


require("layout mode=release reason=operator-release declared=0 "
        "released_index=2 chosen=manual" in text,
        "the run did not take the release path the operator chose")
require("[PRODUCTION-INSTALL] scheme declared=none chosen=manual "
        "source=operator writes=0" in text,
        "the scheme record does not name the operator's choice")
require("[INSTALLER-PRESERVE] plan mode=release" in text and
        " released_index=1" in text,
        "the preservation record does not name the released slot")
require("released_index=2 released_first_lba=133120 released_lba=2097152" in
        text,
        "the layout record does not name the released partition")
require("available_mib=1024 required_mib=128" in text,
        "the layout record does not carry the free and required numbers")
require("[PRODUCTION-INSTALL] state erase stores=" in text and
        "verify=CLEAN" in text,
        "the release install did not publish its state erase step")

require(slot_bytes(RELEASED_SLOT) == b"\x00" * ENTRY_SIZE,
        "the given-up GPT entry is not empty")
for index, entry in enumerate(state["entry_bytes"]):
    if index == RELEASED_SLOT:
        continue
    require(slot_bytes(index).hex() == entry,
            f"foreign GPT entry {index} is not byte-identical")
print("[PRESERVE-TEST] RELEASE: only slot 1 (the given-up partition) changed")
require(disk[0:SECTOR].hex() == state["mbr"],
        "the protective MBR was modified")
for name in ("grub", "grub_cfg", "fallback", "menu"):
    sectors = state["esp_file_sectors"][name]
    data = b"".join(disk[lba * SECTOR:(lba + 1) * SECTOR] for lba in sectors)
    expected = hashlib.sha256(
        bytes.fromhex(state["esp_file_bytes"][name])).hexdigest()
    require(hashlib.sha256(data).hexdigest() == expected,
            f"the foreign ESP file {name} was not preserved")
require(disk[state["esp_file_sectors"]["debian_dir"][0] * SECTOR:
             (state["esp_file_sectors"]["debian_dir"][0] + 1) * SECTOR].hex() ==
        state["esp_dir_bytes"]["debian_dir"],
        "the foreign EFI\\DEBIAN directory was modified")

added = []
for index in range(128):
    if index == RELEASED_SLOT:
        continue
    entry = slot_bytes(index)
    if entry[0:16] == b"\x00" * 16 or index < 4:
        continue
    added.append(index)
require(added[:3] == [4, 5, 6],
        f"the appliance roles are not in the expected free slots: {added[:4]}")
require(len(added) == 3,
        f"the release run wrote {len(added)} partitions, expected 3")
first = int.from_bytes(slot_bytes(added[0])[32:40], "little")
require(first == 133120,
        f"the first role does not start where the given-up partition did: "
        f"{first}")

records = [line for line in text.splitlines()
           if line.startswith("[PRODUCTION-INSTALL] boot-entry ")]
require(len(records) == 1,
        f"expected exactly one boot-entry record, got {len(records)}")
for fragment in ("state=AVAILABLE", "mechanism=REMOVABLE_MEDIA_PATH",
                 "payload_set=7", "existing_loader=PRESERVED",
                 "existing_config=PRESERVED", "verify=SEALED_MEDIA"):
    require(fragment in records[0],
            f"the boot-entry record is missing {fragment}: {records[0]}")
print("[PRESERVE-TEST] RELEASE: the released extent carries the three "
      "appliance roles and the rest of the machine is byte-identical")
PYASSERT
}

# ------------------------------------------------------------------- runs ----
STATE_DIR=$TEST_DIR/state
mkdir -p "$STATE_DIR"
build_foreign_disk "$TEST_DIR/red.raw" "$STATE_DIR"
cp "$TEST_DIR/red.raw" "$TEST_DIR/green.raw"
snapshot_before "$TEST_DIR/red.raw" "$STATE_DIR" "red"

run_boot "red" "$WHOLE_DISK_IMAGE" "$TEST_DIR/red.raw" \
    "[PRODUCTION-INSTALL] applied" || exit 1
assert_red "$TEST_DIR/red.raw" "$STATE_DIR" || exit 1
assert_red_menu "$TEST_DIR/red.raw" || exit 1
printf '%s\n' \
    "[PRESERVE-TEST] RED reproduced through the flow: the hardcoded whole-disk" \
    "mode destroyed the foreign machine on the target disk"

run_boot "green" "$PRESERVE_IMAGE" "$TEST_DIR/green.raw" \
    "[PRODUCTION-INSTALL] applied" || exit 1
assert_green "$TEST_DIR/green.raw" "$STATE_DIR" || exit 1
printf '%s\n' \
    "[PRESERVE-TEST] GREEN: the preserving run kept every foreign byte and" \
    "staged exactly one installed boot payload set"

GREEN_MENU_ENTRIES=$(sed -n 's/.*menu=\\EFI\\BOOT\\limine.conf entries=\([0-9][0-9]*\) .*/\1/p' \
    "$TEST_DIR/green.log" | head -1)
printf '%s\n' "[PRESERVE-TEST] GREEN menu entries published by the record: ${GREEN_MENU_ENTRIES:-MISSING}"
[[ -n "$GREEN_MENU_ENTRIES" ]] || {
    echo "the GREEN record did not publish the menu entry count" >&2
    exit 1
}
boot_installed_disk "$TEST_DIR/green.raw" || exit 1
assert_installed_boot "$TEST_DIR/installed.log" || exit 1
assert_installed_menu "$TEST_DIR/green.raw" "$GREEN_MENU_ENTRIES" || exit 1

# ITEM-501: the release run on a fresh copy of the same foreign machine.
build_foreign_disk "$TEST_DIR/release.raw" "$STATE_DIR"
snapshot_before "$TEST_DIR/release.raw" "$STATE_DIR" "release"
run_release "release" "$CONSOLE_IMAGE" "$TEST_DIR/release.raw" || exit 1
assert_release "$TEST_DIR/release.raw" "$STATE_DIR" || exit 1
RELEASE_MENU_ENTRIES=$(sed -n 's/.*menu=\\EFI\\BOOT\\limine.conf entries=\([0-9][0-9]*\) .*/\1/p' \
    "$TEST_DIR/release.log" | head -1)
printf '%s\n' "[PRESERVE-TEST] RELEASE menu entries published by the record: ${RELEASE_MENU_ENTRIES:-MISSING}"
[[ -n "$RELEASE_MENU_ENTRIES" ]] || {
    echo "the RELEASE record did not publish the menu entry count" >&2
    exit 1
}
boot_installed_disk "$TEST_DIR/release.raw" || exit 1
assert_installed_boot "$TEST_DIR/installed.log" || exit 1
assert_installed_menu "$TEST_DIR/release.raw" "$RELEASE_MENU_ENTRIES" || exit 1
printf '%s\n' \
    "[PRESERVE-TEST] RELEASE: the disk with one given-up partition boots the" \
    "installed appliance with the other system still offered"

echo "QEMU production install preserve-flow test: OK"
