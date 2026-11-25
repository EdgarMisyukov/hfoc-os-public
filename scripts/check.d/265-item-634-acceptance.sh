#!/usr/bin/env bash
# ITEM-634: production shell acceptance and delivery evidence.
#           Sourced in pinned order by scripts/check.sh; lint it on its own.
# shellcheck disable=SC2016  # the grep patterns are literal shell syntax

[[ -f docs/hfoc-shell-acceptance-634.md ]] || {
    echo "ITEM-634: отсутствует shell acceptance документ" >&2
    exit 1
}
for row in 'file create/read/list/copy/rename/delete' \
           'pipeline and redirection' 'exit status' 'background job' \
           'terminal signals' 'second shell session' \
           'ITEM-635' 'host-seed'; do
    grep -Fq -- "$row" docs/hfoc-shell-acceptance-634.md || {
        echo "ITEM-634: acceptance не называет:" "$row" >&2
        exit 1
    }
done

[[ -x scripts/hfoc-shell-acceptance.sh ]] || {
    echo "ITEM-634: нет исполняемого shell acceptance script" >&2
    exit 1
}
grep -Fq 'test-appliance-ssh-qemu' scripts/hfoc-shell-acceptance.sh || {
    echo "ITEM-634: acceptance script не запускает SSH proof" >&2
    exit 1
}
grep -Fq 'verdict: PASS' scripts/hfoc-shell-acceptance.sh || {
    echo "ITEM-634: acceptance script не пишет вердикт" >&2
    exit 1
}
grep -Fq 'workspace binding ITEM-635' scripts/hfoc-shell-acceptance.sh || {
    echo "ITEM-634: acceptance script скрывает workspace status" >&2
    exit 1
}

for marker in 'cp note copy' 'mv copy renamed' 'false; echo status=$?' \
              'renamed' 'status=1' 'status=0'; do
    grep -Fq -- "$marker" scripts/test-ssh-jobs.py || {
        echo "ITEM-634: shell proof не проверяет:" "$marker" >&2
        exit 1
    }
done
grep -Fq 'second-session-ok' scripts/test-qemu-ssh.sh || {
    echo "ITEM-634: нет второго shell session" >&2
    exit 1
}
grep -Fq 'the second owner shell session failed' scripts/test-qemu-ssh.sh || {
    echo "ITEM-634: отказ второго shell не проверяется" >&2
    exit 1
}
grep -Fq 'test-production-installer-update-qemu' docs/hfoc-shell-acceptance-634.md || {
    echo "ITEM-634: installed update preservation не назван" >&2
    exit 1
}
