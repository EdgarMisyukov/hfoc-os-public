#!/usr/bin/env bash
# ITEM-628: the userland shell language over the native ABI.
#           Sourced in pinned order by scripts/check.sh; lint it on its own.

# SPAWN and WAIT append after the published numbers; the first 19 never move.
for shell_abi in \
    'SYSCALL_PIPE = 17,' \
    'SYSCALL_DUP2 = 18,' \
    'SYSCALL_SPAWN = 19,' \
    'SYSCALL_WAIT = 20,' \
    '#define SYSCALL_COUNT 26U'; do
    grep -Fq -- "$shell_abi" kernel/include/syscall.h || {
        echo "ITEM-628: syscall ABI не закреплён:" "$shell_abi" >&2
        exit 1
    }
done

# The shell is a real target program, not a kernel command table.
grep -Fq 'sh' Makefile || {
    echo "ITEM-628: /bin/sh не собран в Makefile" >&2
    exit 1
}
[[ -f user/bin/sh.c ]] || {
    echo "ITEM-628: отсутствует user/bin/sh.c" >&2
    exit 1
}
for shell_source in \
    'HFOC_SPAWN_OPEN' \
    'HFOC_SPAWN_ADOPT' \
    'HFOC_SPAWN_DUP' \
    'HFOC_SPAWN_ACTION_MAX' \
    'sh_run_script' \
    'sh_execute_andor' \
    'sh_parse_pipeline' \
    'unsupported syntax in this shell'; do
    grep -Fq -- "$shell_source" user/bin/sh.c || {
        echo "ITEM-628: shell не несёт:" "$shell_source" >&2
        exit 1
    }
done

# The production proof executes /bin/sh from the target rootfs and reads the
# resulting files back, so output text alone cannot pass.
grep -Fq 'user_sh_proof();' kernel/src/user_init_qemu.c || {
    echo "ITEM-628: QEMU proof не запускает shell" >&2
    exit 1
}
grep -Fq 'user_util_file_equals("/tmp/sh-out"' kernel/src/user_init_qemu.c || {
    echo "ITEM-628: QEMU proof не читает результат shell" >&2
    exit 1
}
for shell_marker in \
    '"[USER-SH] start"' \
    '"[USER-SH] script OK"' \
    '"[USER-SH] inline OK"' \
    '"[USER-SH] proof OK"'; do
    grep -Fq -- "$shell_marker" scripts/test-qemu-user-init.sh || {
        echo "ITEM-628: маркер shell не проверяется:" "$shell_marker" >&2
        exit 1
    }
done
grep -Fq 'if "[USER-SH] failed" in text' scripts/test-qemu-user-init.sh || {
    echo "ITEM-628: отказ shell не проверяется" >&2
    exit 1
}
grep -Fq 'cp tests/fixtures/sh-test.sh' Makefile || {
    echo "ITEM-628: script fixture не попадает в test rootfs" >&2
    exit 1
}

# ITEM-656: diagnostics belong to descriptor 2, so a plain `2>` captures them
# without merging stdout.  The production proof separates the two files.
grep -Fq 'int hfoc_eprintf(const char *format, ...);' user/libhfoc/hfoc.h || {
    echo "ITEM-656: libhfoc не объявляет hfoc_eprintf" >&2
    exit 1
}
grep -Fq 'hfoc_eputs(sh_program_name);' user/bin/sh.c || {
    echo "ITEM-656: sh_error не пишет в fd 2" >&2
    exit 1
}
grep -Fq 'cat /missing > /tmp/sh-out-only 2>/tmp/sh-err-sep' \
    tests/fixtures/sh-test.sh || {
    echo "ITEM-656: fixture не разделяет stdout и stderr" >&2
    exit 1
}
grep -Fq '"/tmp/sh-err-sep"' kernel/src/user_init_qemu.c || {
    echo "ITEM-656: QEMU proof не читает отдельный stderr" >&2
    exit 1
}
