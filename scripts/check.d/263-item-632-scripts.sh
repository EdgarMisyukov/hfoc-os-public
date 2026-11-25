#!/usr/bin/env bash
# ITEM-632: script language, bounded AST and documented sh compatibility.
#           Sourced in pinned order by scripts/check.sh; lint it on its own.
# shellcheck disable=SC2016  # the grep patterns are literal shell syntax

[[ -f docs/hfoc-sh-syntax-632.md ]] || {
    echo "ITEM-632: отсутствует матрица поддержанного синтаксиса" >&2
    exit 1
}
for matrix_row in \
    '`if` / `elif` / `else` / `fi`' \
    '`while` / `until`' \
    '`$(list)` command substitution' \
    '`trap COMMAND EXIT`' \
    '`case`, `select`, `time`' \
    'not a POSIX certification'; do
    grep -Fq -- "$matrix_row" docs/hfoc-sh-syntax-632.md || {
        echo "ITEM-632: матрица не называет:" "$matrix_row" >&2
        exit 1
    }
done

for shell_part in \
    'sh_parse_script' \
    'sh_parse_list' \
    'sh_exec_node' \
    'sh_expand_word_range' \
    'sh_call_function' \
    'sh_capture_shell' \
    'sh_test_command' \
    'sh_run_exit_trap' \
    'maximum call depth exceeded' \
    'command substitution output exceeds 4096 bytes'; do
    grep -Fq -- "$shell_part" user/bin/sh.c || {
        echo "ITEM-632: shell не несёт:" "$shell_part" >&2
        exit 1
    }
done

# Whole-script parsing must reject bad syntax before any simple command runs.
grep -Fq 'sh_parse_incomplete' user/bin/sh.c || {
    echo "ITEM-632: нет различения неполного ввода и ошибки" >&2
    exit 1
}
grep -Fq 'unterminated quote' user/bin/sh.c || {
    echo "ITEM-632: незакрытая кавычка не отвергается до исполнения" >&2
    exit 1
}

[[ -f tests/fixtures/sh-632-red.sh ]] || {
    echo "ITEM-632: отсутствует fixture скриптового языка" >&2
    exit 1
}
for script_part in 'greet()' 'for name in "$@"' 'if [ "$x" = outer ]' \
                   'while false' 'until true' '$(echo inner)' \
                   'trap '; do
    grep -Fq -- "$script_part" tests/fixtures/sh-632-red.sh || {
        echo "ITEM-632: fixture не содержит:" "$script_part" >&2
        exit 1
    }
done
grep -Fq 'cp tests/fixtures/sh-632-red.sh' Makefile || {
    echo "ITEM-632: fixture не попадает в test rootfs" >&2
    exit 1
}

# Command substitution reads a pipe whose writer exits before the parent
# reaps it, so the exit path must close descriptors, not defer to reap.
grep -Fq 'process_release_descriptor_table(process);' kernel/src/process.c || {
    echo "ITEM-632: exit не закрывает дескрипторы до reap" >&2
    exit 1
}
grep -Fq 'a process closes its descriptors when it exits' kernel/src/process.c || {
    echo "ITEM-632: причина закрытия дескрипторов не задокументирована" >&2
    exit 1
}

# The production QEMU proof compares the exact bytes and the real status.
grep -Fq 'user_util_file_equals("/tmp/sh-632-out"' kernel/src/user_init_qemu.c || {
    echo "ITEM-632: QEMU proof не читает вывод скриптов" >&2
    exit 1
}
grep -Fq '"sh-632-syntax", "/bin/sh", syntax632_argv, 2U' \
    kernel/src/user_init_qemu.c || {
    echo "ITEM-632: QEMU proof не требует код 2 для плохого синтаксиса" >&2
    exit 1
}
for shell_marker in \
    '"[USER-SH] scripts OK"' \
    '"[USER-SH] syntax OK"'; do
    grep -Fq -- "$shell_marker" scripts/test-qemu-user-init.sh || {
        echo "ITEM-632: маркер не проверяется:" "$shell_marker" >&2
        exit 1
    }
done
