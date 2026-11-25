#!/usr/bin/env bash
# ITEM-646: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh; lint it on its own with shellcheck.
#
# The 2026-11-22 CI attempts 2 and 3 of run 44997656837 failed the production
# update proof with `the oversized-declaration refusal wrote to the disk`.  The
# refusal itself is terminal (`writes=0`); the machine then continues into the
# installed appliance, whose boot writes runtime state.  The proof sampled the
# disk after a SIGTERM-only stop, so that unrelated boot raced the comparison.
# This slice pins the freeze at the refusal boundary and the stronger
# whole-file fingerprint, plus the host regression for the frozen process.
# shellcheck disable=SC2016

item646_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item646_root" || exit 1

for item646_file in \
    scripts/hfoc-qemu-marker-wait.py \
    scripts/test-qemu-production-install-flow-update.sh \
    tests/qemu-marker-wait-host.sh \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item646_file ]] || {
        echo "ITEM-646: отсутствует $item646_file" >&2
        exit 1
    }
done

# 1. The waiter accepts --stop-pid and freezes the process with SIGSTOP.
grep -Fq -- '[--stop-pid PID]' scripts/hfoc-qemu-marker-wait.py || {
    echo "ITEM-646: waiter не объявляет --stop-pid" >&2
    exit 1
}
grep -Fq 'os.kill(pid, signal.SIGSTOP)' scripts/hfoc-qemu-marker-wait.py || {
    echo "ITEM-646: waiter не посылает SIGSTOP" >&2
    exit 1
}
grep -Fq 'return freeze(stop_pid)' scripts/hfoc-qemu-marker-wait.py || {
    echo "ITEM-646: найденный маркер не замораживает процесс" >&2
    exit 1
}
grep -Fq 'time.sleep(0.01)' scripts/hfoc-qemu-marker-wait.py || {
    echo "ITEM-646: опрос маркера не ужат до 10 мс" >&2
    exit 1
}
grep -Fq 'if stop_pid <= 0:' scripts/hfoc-qemu-marker-wait.py || {
    echo "ITEM-646: waiter принимает pid группы процессов" >&2
    exit 1
}

# 2. The negative update phase passes the emulator pid, and stop_qemu resumes a
#    stopped process so QEMU can flush and exit instead of hanging.
grep -Fq '"[PRODUCTION-INSTALL] refused reason=$refusal" 240 "$QEMU_PID"' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-646: refused-фаза не передаёт pid эмулятора" >&2
    exit 1
}
grep -Fq 'kill -CONT "$QEMU_PID"' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-646: stop_qemu не возобновляет остановленный QEMU" >&2
    exit 1
}
grep -Fq 'stop_args=(--stop-pid "$stop_pid")' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-646: wait_marker не прокидывает --stop-pid" >&2
    exit 1
}

# 3. The oversized proof requires that the installed appliance never booted
#    after the refusal, and fingerprints the whole file rather than a prefix
#    plus filesystem allocation metadata.
grep -Fq "grep -aEq '^HFOC OS .* INITIALISED\\r?\$'" \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-646: нет запрета на следующий boot после отказа" >&2
    exit 1
}
grep -Fq 'sha256sum "$path"' \
    scripts/test-qemu-production-install-flow-update.sh || {
    echo "ITEM-646: fingerprint не хеширует весь файл" >&2
    exit 1
}
if grep -Fq "stat -c '%s %b'" \
    scripts/test-qemu-production-install-flow-update.sh; then
    echo "ITEM-646: fingerprint всё ещё содержит счётчик блоков ФС" >&2
    exit 1
fi
if grep -Fq 'head -c 4194304' \
    scripts/test-qemu-production-install-flow-update.sh; then
    echo "ITEM-646: fingerprint всё ещё читает только префикс" >&2
    exit 1
fi

# 4. The host regression drives the freeze with a real stopped process.
grep -Fq -- '--stop-pid "$victim"' tests/qemu-marker-wait-host.sh || {
    echo "ITEM-646: host-проба не проверяет --stop-pid" >&2
    exit 1
}
grep -Fq '[[ $victim_state == T* ]]' tests/qemu-marker-wait-host.sh || {
    echo "ITEM-646: host-проба не требует остановленного состояния" >&2
    exit 1
}
grep -Fq -- '--stop-pid 0' tests/qemu-marker-wait-host.sh || {
    echo "ITEM-646: host-проба не отвергает pid группы" >&2
    exit 1
}

exit 0
