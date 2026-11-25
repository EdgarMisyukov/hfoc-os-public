#!/usr/bin/env bash
# ITEM-413: contracted slice of the HFOC OS check suite. Sourced in pinned
#           order by scripts/check.sh; lint it on its own with shellcheck.

# Installing twice onto the same disk.  The acceptance host was installed onto
# twice: the first run left our own ESP on the disk and the next medium
# installed over it.  The format reset the boot sector, both FATs and the root
# directory, but not the volume's *data area*, which still held the previous
# install's directory clusters - and a directory this writer created kept those
# bytes, so the writer's own lookup adopted a stale entry, took the cluster it
# named and left it unallocated in the freshly formatted FAT.  The file
# allocator handed that cluster out for file data, the staged set no longer
# matched the sealed digest, and the flow refused with
# `PRODUCTION_INSTALLER_VERIFICATION_FAILED` at `stage=esp-payload` - no
# reboot, no boot log, machine left on the stick.
#
# The invariant the item adds is one line of the writer's contract: a directory
# this writer creates starts empty.  This slice pins the mechanism (one helper,
# used by both directory-creation paths) and both proofs that the invariant
# holds: the host test that poisons the volume's free data area first, and the
# QEMU case that installs the declared medium twice onto the same disk.
for reinstall_evidence in \
    'efi_fat32_directory_clear' \
    'every cluster of a directory this writer creates starts as zeroed' \
    'ITEM-413'; do
    grep -Fq "$reinstall_evidence" kernel/src/efi_fat32_esp.c || {
        echo "писатель ESP не объявляет инвариант ITEM-413: $reinstall_evidence" >&2
        exit 1
    }
done
# Both directory-creation paths must clear the cluster they allocated.  The
# helper itself carries the definition, so the call sites are what is counted.
reinstall_calls=$(grep -c 'status = efi_fat32_directory_clear(' \
    kernel/src/efi_fat32_esp.c)
[[ "$reinstall_calls" -ge 2 ]] || {
    echo "создание каталога не зануляет кластер: вызовов $reinstall_calls" >&2
    exit 1
}
reinstall_checks=$(grep -c 'carries none of the ' \
    tests/efi-fat32-esp-host.c)
[[ "$reinstall_checks" -eq 2 ]] || {
    echo "хостовое доказательство проверяет созданные каталоги не дважды: $reinstall_checks" >&2
    exit 1
}
# shellcheck disable=SC2016  # the single-quoted text is what the proof says
for reinstall_proof in \
    're-installing (ITEM-413)' \
    '[PRODUCTION-INSTALL] failed stage='; do
    grep -Fq "$reinstall_proof" \
        scripts/test-qemu-production-install-flow.sh || {
        echo "product-flow proof не называет дефект переустановки: $reinstall_proof" >&2
        exit 1
    }
done
reinstall_runs=$(grep -c '^run_boot "reinstall" ' \
    scripts/test-qemu-production-install-flow.sh)
[[ "$reinstall_runs" -eq 2 ]] || {
    echo "product-flow proof ставит систему не дважды, а $reinstall_runs раз" >&2
    exit 1
}
