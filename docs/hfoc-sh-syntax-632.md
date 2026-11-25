# Native shell syntax matrix (ITEM-632)

The shell is a small native Unix-like language over the HFOC ABI, not Bash and
not a POSIX certification.  This matrix names the delivered scope.  Anything
not marked supported is rejected by name with a nonzero status; it is not
silently accepted.

| Construct | Status |
|---|---|
| words, single/double quotes, backslash | supported |
| `$VAR`, `${VAR}`, `$?` | supported |
| `$0`, `$1..$9`, `$#`, `$@`, `$*` | supported, positional frame of 9 parameters |
| `;`, newline, `&&`, `\|\|` | supported |
| pipelines up to 8 stages | supported |
| `<`, `>`, `>>`, `2>`, `2>&1` | supported |
| `cd`, `export`, `unset`, `exit`, `shift` builtins | supported |
| `test` / `[` builtin: `-e -f -d -r -w -n -z = != -eq -ne -lt -le -gt -ge !` | supported |
| `name=value` standalone assignment | supported, persists in the shell |
| `name=value command` assignment prefix | supported for a single command; restored afterwards |
| `if` / `elif` / `else` / `fi` | supported |
| `for name in words; do ...; done` | supported, up to 32 words |
| `for name; do ...; done` (positional parameters) | supported |
| `while` / `until` | supported, bounded to 10000 iterations |
| `name() { ...; }` and `function name { ...; }` | supported in scripts; call depth bounded to 6 |
| `return`, `break`, `continue` (level 1 only) | supported inside the matching construct |
| `( list )` subshell | supported: variables, cwd, positional frame, functions and last status restored |
| `{ list; }` group | supported |
| `$(list)` command substitution | supported, output capped at 4096 bytes |
| backtick command substitution | supported, output capped at 4096 bytes |
| `trap COMMAND EXIT` | supported; one EXIT trap |
| `case`, `select`, `time`, arithmetic `$(( ))`, `set -e` | rejected by name |
| arrays, process substitution, here-documents, globbing, field splitting | not supported |
| `cmd &`, `jobs`, `fg [%n]`, `bg [%n]`, `wait [%n]` | supported: up to 8 jobs, 8 PIDs per job, simple and-or lists; a compound statement directly followed by `&` is not claimed |

Job-control bounds and the terminal-signal model are documented in
`docs/hfoc-sh-jobs-633.md`.

Rules that hold for every row:

- Scripts are read whole into a 10 KiB buffer and parsed to a bounded static AST
  (128 nodes, 512 list slots, 1024 tokens, 4 chained `&&`/`||` lists) before the
  first command runs.  A syntax error therefore never executes half of a
  compound command.  A script that exceeds a bound is rejected with a named
  error before execution.
- One simple command line is at most 4096 bytes, 1024 words, 64 arguments, 8
  redirections and 8 pipeline stages.  There are 12 shell variables (name 63
  bytes, value 255 bytes each), 32 functions, 9 positional parameters and at
  most 2 nested subshells.  A value that does not fit is an error, never a
  truncated value.
- The shell image keeps its own memory below the kernel's 64-page user mapping
  limit, so the 4-page user stack is never traded for static shell state.
- Interactive input supports continuation lines for an incomplete compound
  command.  Function definitions live for one interactive buffer only; put
  functions in a script or on the same input line as the call.
- Over an SSH PTY, type UTF-8 directly in arguments and file names. Left,
  Right and Backspace move or delete one UTF-8 code point. Up and Down
  recall up to eight recent lines within a 4096-byte budget. Returning past
  the newest entry restores the draft and its cursor. History lives only in
  that shell session. The VT editor scrolls a single row horizontally using
  the width supplied when the SSH session opens. The prompt starts on a
  fresh row after partial command output. Reconnect after resizing to use
  the new width.
- `$@` and `"$@"` expand to separate words only when the token is exactly
  `$@`/`"$@"`; embedded forms join with spaces.  There is no unquoted word
  splitting or glob expansion.
- Command substitution and pipelines run real `/bin/sh` and real target ELF
  programs; the shell does not fake their output.  A command substitution that
  produces more than 4096 bytes is a named error, never a silent truncation.
- `break`/`continue` accept only level 1.  `trap` accepts only `EXIT`.
- A standalone assignment reports status 0; it does not propagate the status of
  a command substitution used as its value.  `$?` after a normal command is
  that command's real exit status.
- A command name that does not resolve still performs its own redirections
  before the shell reports it: `nosuch 2>file` creates the file and writes
  `command not found` there, `nosuch >file 2>&1` captures it on stdout, and
  an unused `>file` is created empty.  The status is 127, the shell's
  missing-name status.

The production proof is `tests/fixtures/sh-632-red.sh`: the QEMU user-init
stage runs it through the target `/bin/sh`, redirects its output to
`/tmp/sh-632-out`, appends the real `$?`, and the kernel compares the exact
bytes and status.  A second `-c` run with an unterminated `if` must exit 2,
which proves that wrong syntax is rejected before execution.
