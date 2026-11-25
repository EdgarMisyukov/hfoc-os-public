#!/usr/bin/env bash
# ITEM-509 increment 26: contracted slice of the HFOC OS check suite. Sourced in
#           pinned order by scripts/check.sh; lint it on its own with shellcheck.
#
# The pins below are literal source and document text, so `$(...)` must not
# expand and a trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item509_inc26_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item509_inc26_root" || exit 1

# ITEM-509 increment 26 (509.7d-3b): the gate that keeps the operator surface
# translated.  `scripts/check-untranslated-strings.py` extracts every string
# literal of the ten declared surface files (comments and preprocessor lines
# dropped, decoded, deduplicated per file, sorted) and this slice requires the
# stream to match `scripts/check-untranslated-strings.baseline.txt` byte for
# byte.  A new, removed or changed literal is red until the baseline is updated
# in the same reviewed commit, so a new English string cannot reach the console,
# the panel or the installer unnoticed.  The negative control proves the gate
# can fail: one synthetic literal in a copy of the tree must change the stream.
for item509_inc26_file in \
    scripts/check-untranslated-strings.py \
    scripts/check-untranslated-strings.baseline.txt \
    docs/hfoc-language-surfaces.md \
    docs/ROADMAP.md \
    docs/superpowers/plans/2026-09-11-appliance-productization-queue.md; do
    [[ -f $item509_inc26_file ]] || {
        echo "ITEM-509 inc26: отсутствует $item509_inc26_file" >&2
        exit 1
    }
done
command -v python3 >/dev/null || {
    echo "ITEM-509 inc26: нет python3 для извлекателя" >&2
    exit 1
}

# 1. The extractor declares exactly the ten surface files, drops comments and
#    preprocessor lines, and names the frozen baseline it feeds.
for item509_inc26_surface in \
    'kernel/src/console.c' \
    'kernel/src/appliance_management_web_ui.c' \
    'kernel/src/installer_ui.c' \
    'kernel/src/installer_storage_screen.c' \
    'kernel/src/installer_credential_screen.c' \
    'kernel/src/installer_notice.c' \
    'kernel/src/local_menu.c' \
    'kernel/src/appliance_admin_credentials_prompt.c' \
    'kernel/src/appliance_boot_console.c' \
    'kernel/src/appliance_production_panel_view.c'; do
    grep -Fq "\"$item509_inc26_surface\"," \
        scripts/check-untranslated-strings.py || {
        echo "ITEM-509 inc26: извлекатель не знает поверхность: $item509_inc26_surface" >&2
        exit 1
    }
done
grep -Fq 'def strip_comments_and_directives(text: str) -> str:' \
    scripts/check-untranslated-strings.py || {
    echo "ITEM-509 inc26: извлекатель не отбрасывает комментарии" >&2
    exit 1
}
grep -Fq 'line.lstrip().startswith("#")' \
    scripts/check-untranslated-strings.py || {
    echo "ITEM-509 inc26: извлекатель не отбрасывает директивы" >&2
    exit 1
}
grep -Fq 'check-untranslated-strings.baseline.txt' \
    scripts/check-untranslated-strings.py || {
    echo "ITEM-509 inc26: извлекатель не называет baseline" >&2
    exit 1
}

# 2. The tree's stream matches the frozen baseline exactly.
item509_inc26_stream=$(mktemp "${TMPDIR:-/tmp}/hfoc-untranslated.XXXXXX")
trap 'rm -f "$item509_inc26_stream"' EXIT
python3 scripts/check-untranslated-strings.py . >"$item509_inc26_stream" || {
    echo "ITEM-509 inc26: извлекатель не отработал" >&2
    exit 1
}
if ! cmp -s scripts/check-untranslated-strings.baseline.txt \
    "$item509_inc26_stream"; then
    diff -u scripts/check-untranslated-strings.baseline.txt \
        "$item509_inc26_stream" | head -n 40 >&2 || true
    echo "ITEM-509 inc26: литералы поверхности разошлись с baseline;" \
        "переведите строку или обновите baseline тем же коммитом" >&2
    exit 1
fi
rm -f "$item509_inc26_stream"
trap - EXIT

# 3. The negative control: a copy of the ten files plus one synthetic English
#    literal must change the stream; a gate that does not react is not a gate.
item509_inc26_control=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-untranslated-control.XXXXXX")
trap 'rm -rf "$item509_inc26_control"' EXIT
for item509_inc26_surface in \
    kernel/src/console.c \
    kernel/src/appliance_management_web_ui.c \
    kernel/src/installer_ui.c \
    kernel/src/installer_storage_screen.c \
    kernel/src/installer_credential_screen.c \
    kernel/src/installer_notice.c \
    kernel/src/local_menu.c \
    kernel/src/appliance_admin_credentials_prompt.c \
    kernel/src/appliance_boot_console.c \
    kernel/src/appliance_production_panel_view.c; do
    mkdir -p "$item509_inc26_control/${item509_inc26_surface%/*}"
    cp "$item509_inc26_root/$item509_inc26_surface" \
        "$item509_inc26_control/$item509_inc26_surface"
done
printf '\nstatic const char *hfoc_slice_probe = "TOTALLY NEW SENTENCE";\n' \
    >>"$item509_inc26_control/kernel/src/console.c"
python3 scripts/check-untranslated-strings.py "$item509_inc26_control" \
    >"$item509_inc26_control/stream.txt" || {
    echo "ITEM-509 inc26: извлекатель не отработал на контроле" >&2
    exit 1
}
if cmp -s scripts/check-untranslated-strings.baseline.txt \
    "$item509_inc26_control/stream.txt"; then
    echo "ITEM-509 inc26: негативный контроль не изменил поток" >&2
    exit 1
fi
grep -Fq 'TOTALLY NEW SENTENCE' "$item509_inc26_control/stream.txt" || {
    echo "ITEM-509 inc26: контроль не назвал фиктивную строку" >&2
    exit 1
}
rm -rf "$item509_inc26_control"
trap - EXIT

# 4. The increment is recorded where the next window reads it.
grep -Fq 'ITEM-509 increment 26' docs/hfoc-language-surfaces.md || {
    echo "ITEM-509 inc26: поверхности языка не называют инкремент" >&2
    exit 1
}
grep -Fq '509.7d-3b' docs/superpowers/plans/2026-09-11-appliance-productization-queue.md || {
    echo "ITEM-509 inc26: очередь не называет кусок 509.7d" >&2
    exit 1
}
