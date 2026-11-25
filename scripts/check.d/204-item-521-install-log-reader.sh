#!/usr/bin/env bash
# ITEM-521 (requirement 4): contracted slice of the HFOC OS check suite,
#           sourced in pinned order by scripts/check.sh and lintable on its own
#           with the shellcheck tool.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item521r_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item521r_root" || exit 1

# ITEM-521, the reading half of requirement 4: the owner brings the medium back
# and one command prints the journal the install run left on it.  This slice
# holds that contract and nothing about where the journal is placed - the
# ITEM-520 fork (FAT12 file, preallocated file or raw extent) is still the
# owner's, and the reader accepts all three forms:
#   * `--disk /dev/sdb` (and the positional spelling) work with no mount, and
#     a block device is sized with BLKGETSIZE64, not fstat;
#   * the three exit codes are 0 read, 1 malformed (named on stderr), 2 absent;
#   * the placement is autodetected: MBR type 0xEF -> BPB FAT12 -> the file's
#     12-bit chain, otherwise the raw extent at blocks - 34 - 2048;
#   * a torn or foreign record is never printed as text, and the commit point
#     of both a growing and a preallocated file is honored;
#   * the host proof builds every form from the documented frame and drives
#     the reader as an operator would; it is registered in the host suite, and
#     it drives the machine-readable `--json` half too (a FAT12 file, a raw
#     extent, a mounted file, a torn record, a medium with no journal) and the
#     `--offset` spellings (a volume inside a whole disk, a bare volume dump, a
#     bogus offset that must be malformed, not silently "no journal"), so a
#     script on the acceptance host rests on a checked branch.

# 1. The reader, its proof and the target exist where the contract says.
for item521r_file in \
    scripts/hfoc-install-log-read.py \
    tests/install-log-reader-host.py \
    Makefile; do
    [[ -f $item521r_file ]] || {
        echo "ITEM-521: отсутствует $item521r_file" >&2
        exit 1
    }
done

# 2. One command, no mount, read-only, and a block device is sized with
#    BLKGETSIZE64 because fstat answers zero for a device.
grep -Fq 'hfoc-install-log-read.py --disk /dev/sdb' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: читалка не называет команду для устройства" >&2
    exit 1
}
grep -Fq 'BLKGETSIZE64 = 0x80081272' scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: размер блочного устройства не читается через BLKGETSIZE64" >&2
    exit 1
}
grep -Fq 'fcntl.ioctl(self.f.fileno(), BLKGETSIZE64, buf, True)' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: ioctl размера устройства пропал" >&2
    exit 1
}
grep -Fq 'self.f = open(path, "rb")' scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: читалка открывает носитель не только для чтения" >&2
    exit 1
}

# 3. The exit contract: 0 read, 1 malformed and named, 2 absent and named.
for item521r_exit in \
    'EXIT_OK = 0' \
    'EXIT_MALFORMED = 1' \
    'EXIT_ABSENT = 2' \
    'print(f"install-log: malformed: {error}", file=sys.stderr)' \
    'print("install-log: no journal on this medium", file=sys.stderr)'; do
    grep -Fq "$item521r_exit" scripts/hfoc-install-log-read.py || {
        echo "ITEM-521: код возврата читалки не назван: $item521r_exit" >&2
        exit 1
    }
done

# 4. Placement is autodetected and no form is chosen here: the MBR's 0xEF
#    partition, the FAT12 file (growing and preallocated commit points) and
#    the raw extent are all reachable.  An explicit `--offset` names the
#    volume's first byte directly, so a partition dump or an image with no MBR
#    reads the same way.  ITEM-582 added the diagnostic stick's FAT12
#    partition to the same scan; the reader still walks the table and names
#    the first partition that can carry the journal.
for item521r_placement in \
    'for wanted in ((0xEF,), (0x01, 0x04, 0x06, 0x0B, 0x0C)):' \
    'placement="fat12-file" if size < capacity_bytes else "fat12-preallocated",' \
    'placement="extent",' \
    'first = blocks - 34 - 2048' \
    'if journal is not None:' \
    'return read_extent_journal(image)' \
    'ap.add_argument("--offset", type=lambda x: int(x, 0),' \
    'if offset is not None:' \
    'return offset'; do
    grep -Fq "$item521r_placement" scripts/hfoc-install-log-read.py || {
        echo "ITEM-521: читалка не разбирает размещение: $item521r_placement" >&2
        exit 1
    }
done

# 5. Both extent header layouts are accepted - the prototype the QEMU proof
#    wrote and the placement-independent core frame - so evidence gathered
#    before the reconciliation still reads.
grep -Fq 'if le64(header, 25) == first and crc32(header, 0, 41) == le32(header, 41):' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: заголовок extent-прототипа не читается" >&2
    exit 1
}
grep -Fq 'elif crc32(header, 0, 36) == le32(header, 36):' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: заголовок ядра в extent-области не читается" >&2
    exit 1
}

# 6. A torn, foreign or non-printable record is named and never printed as
#    text, and the commit point of a file whose size is capacity is the
#    header's write_slot, not the zero tail.
grep -Fq 'warnings.append(f"slot {index} is torn (record CRC mismatch)")' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: рваная запись не названа" >&2
    exit 1
}
grep -Fq 'warnings.append(f"slot {index} carries non-printable text")' \
    scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: нечитаемый текст не отсечён" >&2
    exit 1
}
grep -Fq 'if 1 <= write_slot < by_size:' scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: точка коммита предвыделенного файла не учтена" >&2
    exit 1
}

# 7. The host proof drives every form and the named failures, and the target
#    is registered in the compiled host suite (the manifest check re-derives
#    the count from the serial recipe).
for item521r_proof_pin in \
    'case: FAT12 file (option a)' \
    'case: preallocated FAT12 file (option c, header write_slot commits)' \
    'case: raw extent, prototype header layout (option b)' \
    'case: raw extent, core header layout' \
    'case: mounted file (--file)' \
    'case: --offset names the FAT12 volume inside a whole disk' \
    'case: --offset 0 reads a bare volume dump' \
    'case: a bogus --offset is malformed, not silent' \
    'check("not called absent", "no journal" not in err, err)' \
    'case: torn committed record is warned, not printed' \
    'case: broken header CRC is malformed, not '"'"'no journal'"'"'' \
    'case: FAT chain loop is malformed' \
    'case: medium without a journal' \
    'check("torn text not printed", "state=MATCH" not in out, out)' \
    'check("exit 2", code == 2, f"exit={code} err={err!r}")'; do
    grep -Fq "$item521r_proof_pin" tests/install-log-reader-host.py || {
        echo "ITEM-521: host-проба читалки не проверяет: $item521r_proof_pin" >&2
        exit 1
    }
done

# 9. The machine-readable half is pinned as well: `--json` is the branch a
#    script on the acceptance host would use, so the witness drives it for a
#    FAT12 file, a raw extent, a mounted file, a torn record and a medium with
#    no journal, and the reader still names the same journal in one document.
grep -Fq 'print(json.dumps(journal, indent=2))' scripts/hfoc-install-log-read.py || {
    echo "ITEM-521: читалка не печатает журнал одним JSON-документом" >&2
    exit 1
}
for item521r_json_pin in \
    'case: --json names the same journal, machine-readably' \
    'case: --json names the raw extent with its lba' \
    'case: --json on a medium without a journal keeps stdout empty' \
    'case: --json names the torn slot and withholds its text' \
    'case: --json with --offset names the same FAT12 journal' \
    'case: --file --json names the mounted-file placement' \
    'check("stdout is one JSON document", True)' \
    'document["placement"] == "fat12-file"' \
    'document["placement"] == "file"' \
    'document["capacity_slots"] == 64' \
    'document["first_lba"] == 8228' \
    'document["slots"] == 3' \
    'check("stdout stays empty", out == "", out)' \
    'check("no warnings", document["warnings"] == [], document["warnings"])' \
    'any("torn" in warning for warning in document["warnings"])'; do
    grep -Fq "$item521r_json_pin" tests/install-log-reader-host.py || {
        echo "ITEM-521: host-проба не закрепляет JSON-ветку: $item521r_json_pin" >&2
        exit 1
    }
done
grep -Fq 'test-install-log-read-host:' Makefile || {
    echo "ITEM-521: цель host-пробы читалки не определена" >&2
    exit 1
}
grep -Fq '$(MAKE) test-install-log-read-host' Makefile || {
    echo "ITEM-521: host-проба читалки не зарегистрирована в host-suite" >&2
    exit 1
}

# 8. The queue records this item (ITEM-526 moved the `## Current item` heading on
#    to its own item, so this pin reads ITEM-521's own historical section), and
#    the roadmap records the increment.
grep -A 3 '^## Item ITEM-521$' \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md |
    grep -Fq 'ITEM-521' || {
    echo "ITEM-521: раздел очереди не называет ITEM-521" >&2
    exit 1
}
grep -Fq 'ITEM-521' docs/ROADMAP.md || {
    echo "ITEM-521: docs/ROADMAP.md не называет пункт" >&2
    exit 1
}
