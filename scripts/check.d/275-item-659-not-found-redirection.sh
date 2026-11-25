#!/usr/bin/env bash
# ITEM-659: a command that is not found still performs its own redirections
# and reports the shell's not-found status, so `nosuch 2>file` creates the
# file and puts the diagnostic there.
grep -Fq 'static int sh_not_found(struct sh_command *command)' user/bin/sh.c || {
    echo "ITEM-659: нет пути not-found с собственными redirections" >&2
    exit 1
}
grep -Fq 'return pid == SH_SPAWN_NOT_FOUND ? 127 : 1;' user/bin/sh.c || {
    echo "ITEM-659: not-found не публикует статус 127" >&2
    exit 1
}
grep -Fq 'nosuchcommand > /tmp/sh-out-only 2>/tmp/sh-nf-err' \
    tests/fixtures/sh-test.sh || {
    echo "ITEM-659: fixture не проверяет 2> у not-found" >&2
    exit 1
}
grep -Fq '"/tmp/sh-nf-err"' kernel/src/user_init_qemu.c || {
    echo "ITEM-659: QEMU proof не читает stderr not-found" >&2
    exit 1
}
grep -Fq '"/tmp/sh-nf-err", "command not found"' kernel/src/user_init_qemu.c || {
    echo "ITEM-659: QEMU proof не требует текст not-found" >&2
    exit 1
}
