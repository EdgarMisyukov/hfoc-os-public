#!/usr/bin/env python3
"""ITEM-509 509.7d-3b: the frozen literal stream of the operator surface.

Every string literal of the declared operator-surface source files, decoded,
deduplicated per file and sorted.  `scripts/check.d/189-item-509-no-untranslated-
strings.sh` compares this stream byte for byte with
`scripts/check-untranslated-strings.baseline.txt`, so any new, removed or changed
literal in those files is red until the baseline is updated in the same reviewed
commit: a new English string cannot appear on an operator surface unnoticed.

Usage: check-untranslated-strings.py <repo-root>
"""
import re
import sys
from pathlib import Path

# The declared operator surface: the sources whose literals the item promises
# are translated, machine text, or reviewed exceptions.  Pinned by slice 189.
SURFACE_FILES = (
    "kernel/src/console.c",
    "kernel/src/appliance_management_web_ui.c",
    "kernel/src/installer_ui.c",
    "kernel/src/installer_storage_screen.c",
    "kernel/src/installer_credential_screen.c",
    "kernel/src/installer_notice.c",
    "kernel/src/local_menu.c",
    "kernel/src/appliance_admin_credentials_prompt.c",
    "kernel/src/appliance_boot_console.c",
    "kernel/src/appliance_production_panel_view.c",
)

LITERAL = re.compile(r'"((?:[^"\\]|\\.)*)"')


def c_unescape(raw: str) -> str:
    out = []
    i = 0
    while i < len(raw):
        ch = raw[i]
        if ch != "\\":
            out.append(ch)
            i += 1
            continue
        i += 1
        if i >= len(raw):
            break
        esc = raw[i]
        mapping = {"n": "\n", "t": "\t", "r": "\r", "\\": "\\", '"': '"',
                   "0": "\0", "a": "\a", "b": "\b", "f": "\f", "v": "\v"}
        if esc in mapping:
            out.append(mapping[esc])
            i += 1
        elif esc == "x":
            j = i + 1
            while j < len(raw) and raw[j] in "0123456789abcdefABCDEF":
                j += 1
            out.append(chr(int(raw[i + 1:j], 16)))
            i = j
        elif esc in "01234567":
            j = i
            while j < len(raw) and j < i + 3 and raw[j] in "01234567":
                j += 1
            out.append(chr(int(raw[i:j], 8)))
            i = j
        else:
            out.append(esc)
            i += 1
    return "".join(out)


def strip_comments_and_directives(text: str) -> str:
    """Drop /* */ and // comments and preprocessor lines, keeping the rest."""
    lines = []
    for line in text.splitlines():
        if line.lstrip().startswith("#"):
            lines.append("")
        else:
            lines.append(line)
    text = "\n".join(lines)
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = n if j < 0 else j + 2
            continue
        if text.startswith("//", i):
            j = text.find("\n", i + 2)
            i = n if j < 0 else j
            continue
        if text[i] == '"':
            j = i + 1
            while j < n and text[j] != '"':
                j += 2 if text[j] == "\\" else 1
            out.append(text[i:j + 1])
            i = j + 1
            continue
        out.append(text[i])
        i += 1
    return "".join(out)


def escape(value: str) -> str:
    out = []
    for ch in value:
        if ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\t":
            out.append("\\t")
        elif ch == "\r":
            out.append("\\r")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\x%02x" % ord(ch))
        else:
            out.append(ch)
    return "".join(out)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check-untranslated-strings.py <repo-root>", file=sys.stderr)
        return 2
    root = Path(sys.argv[1])
    stream = []
    for name in SURFACE_FILES:
        path = root / name
        if not path.is_file():
            print("check-untranslated-strings: missing " + name, file=sys.stderr)
            return 1
        text = strip_comments_and_directives(
            path.read_text(encoding="utf-8"))
        seen = set()
        for match in LITERAL.finditer(text):
            value = c_unescape(match.group(1))
            if value in seen:
                continue
            seen.add(value)
            stream.append(name + "\t" + escape(value))
    stream.sort()
    sys.stdout.write("\n".join(stream) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
