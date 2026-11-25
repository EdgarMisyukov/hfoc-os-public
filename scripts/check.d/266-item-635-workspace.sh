#!/usr/bin/env bash
# ITEM-635: the persistent workspace starts after the host-seed slots, the
#           production binding and commit hook are on, and the installed proof
#           names the corrected RCA.
# shellcheck disable=SC2016  # the grep patterns are literal shell syntax

[[ -f docs/hfocfs-workspace-635.md ]] || {
    echo "ITEM-635: отсутствует workspace binding документ" >&2
    exit 1
}
for row in 'host-seed' 'block 514' 'REFUSED' 'Пароль:' 'QEMU'; do
    grep -Fq -- "$row" docs/hfocfs-workspace-635.md || {
        echo "ITEM-635: документ не называет:" "$row" >&2
        exit 1
    }
done
grep -Fq 'APPLIANCE_HOST_SEED_STATE_SLOT_COUNT' kernel/include/hfocfs_workspace.h || {
    echo "ITEM-635: workspace offset не выводится из host-seed slots" >&2
    exit 1
}
grep -Fq 'APPLIANCE_STATE_LAYOUT_DEFAULT_CAPACITY_BLOCKS' \
    kernel/include/hfocfs_workspace.h || {
    echo "ITEM-635: workspace offset не выводится из state layout" >&2
    exit 1
}
grep -Fq '_Static_assert' kernel/src/hfocfs_workspace.c || {
    echo "ITEM-635: нет compile-time layout guard" >&2
    exit 1
}
grep -Fq 'kernel_workspace_boot_init' kernel/src/kernel.c || {
    echo "ITEM-635: production boot binding отсутствует" >&2
    exit 1
}
grep -Fq 'hfocfs_workspace_commit' kernel/src/syscall_file.c || {
    echo "ITEM-635: file syscall commit hook отсутствует" >&2
    exit 1
}
grep -Fq 'host_seed_intact' tests/hfocfs-workspace-host.c || {
    echo "ITEM-635: host proof не хранит host-seed sentinels" >&2
    exit 1
}
grep -Fq 'base = 514 * 512' scripts/test-qemu-hfocfs-workspace.sh || {
    echo "ITEM-635: QEMU proof не проверяет block 514" >&2
    exit 1
}
grep -Fq '[WORKSPACE] state=OK range=514+32768' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-635: installed proof не проверяет workspace record" >&2
    exit 1
}
grep -Fq 'host-seed slots' scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-635: installed proof не проверяет host-seed sentinel" >&2
    exit 1
}
grep -Fq 'Пароль:' scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-635: installed proof не запрещает false login prompt" >&2
    exit 1
}
grep -Fq 'Пароль:' docs/hfoc-shell-acceptance-634.md || {
    echo "ITEM-634: installed proof не называет false login prompt" >&2
    exit 1
}
