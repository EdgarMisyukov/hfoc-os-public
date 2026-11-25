#!/usr/bin/env bash
# ITEM-658: keep the native editor wired to the production SSH PTY proof.

for editor_source in \
    '#define SH_LINE_MAX 4096U' \
    '#define SH_HISTORY_MAX 8U' \
    'sh_read_line(line, sizeof(line), prompt)' \
    'sh_history_move(&edit, arrow)'; do
    grep -Fq -- "$editor_source" user/bin/sh.c || {
        echo "ITEM-658: отсутствует контракт редактора: $editor_source" >&2
        exit 1
    }
done

for editor_proof in \
    '("editor-utf8_é_мир", 1)' \
    '("editor-history", 2)' \
    '("editor-draft", 1)' \
    '("editor-boundary-aXéz", 1)' \
    '("editor-delete-ab", 1)' \
    '("editor-overflow-status-2", 1)' \
    'line editor display wrapped in the 20-column PTY' \
    'line editor prompt overwrote partial command output' \
    'lines.count(marker) != count'; do
    grep -Fq -- "$editor_proof" scripts/test-ssh-jobs.py || {
        echo "ITEM-658: отсутствует проверка редактора: $editor_proof" >&2
        exit 1
    }
done

grep -Fq 'full pty mode string is acknowledged' tests/ssh-shell-host.c || {
    echo 'ITEM-658: отсутствует проверка полного списка режимов PTY' >&2
    exit 1
}
