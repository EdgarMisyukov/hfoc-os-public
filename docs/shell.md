# Native shell reference

[Home](../README.md) · [First boot and management](operate.md) · English · [Русский](shell.ru.md)

This reference covers the native `/bin/sh` shipped in HFOC OS 1.0.0. The shell runs HFOC ELF64 programs. Its language implements the subset described below. See the [1.0.0 release notes](release-1.0.0.md) for the release scope.

## Connect and use the prompt

Enroll the owner's public key and verify the SSH host key as described in [First boot and management](operate.md). Run this command on the SSH client. Substitute the management address for `HFOC_ADDRESS` and use the paths to your private key and the file containing the verified host key.

```sh
ssh -tt -i ~/.ssh/hfoc_owner -o IdentitiesOnly=yes \
  -o UserKnownHostsFile="$HOME/.ssh/hfoc_known_hosts" \
  -o StrictHostKeyChecking=yes hfoc@HFOC_ADDRESS
```

- Enter commands at `hfoc$`. The `>` prompt requests another line for an incomplete compound command. Ctrl+C at the prompt clears the current input, including any continuation, and sets status 130. Ctrl+D on an empty input line or `exit` closes the shell.
- Type UTF-8 directly in arguments and file names. Left and Right move by one Unicode code point. Backspace removes the preceding code point, including when the client sends the DEL byte for Backspace. A separate forward-Delete key sequence has no editing action.
- Up and Down recall up to eight recent input lines within a shared 4096-byte budget. Down past the newest entry restores your draft and cursor. History lasts for the session.
- Long input scrolls horizontally on one terminal row. The editor starts the prompt on a fresh row after partial output. SSH accepts window-size changes, and the editor uses the width supplied when the shell starts. Reconnect to apply a new width to the editor.

Use `/home/owner` for persistent files. `/tmp` and `/run` are stored in memory. `/bin` and `/etc` come from the system image. An SSH session sets `HOME` to `/tmp`. Use `cd /home/owner` to open the persistent workspace and `pwd` to check the current directory.

## Commands, words and status

The native tools in `/bin` include `sh`, `ls`, `cat`, `cp`, `mv`, `rm`, `mkdir`, `echo`, `pwd`, `grep`, `wc`, `sleep`, `true`, `false` and `hfoc`. They have their own option sets. A command name containing a slash is used as a path. For other external commands, the shell searches the nonempty entries in the colon-separated `PATH`. If `PATH` is unset or empty, the search uses `/bin`.

| Syntax | Behavior |
| --- | --- |
| `'text'` | A literal word. |
| `"text"` | One word, with dollar expansions enabled. Inside double quotes, backslash escapes `"`, `\` and `$`. |
| `\` outside quotes | Quotes the following byte. |
| `# comment` | Starts a comment at a word boundary. |
| `$NAME`, `${NAME}` | Expand a variable. Unset variables expand to empty text. |
| `$?` | Expands to the most recent command or list status. Zero indicates success. |
| `;`, newline | Separate commands and lists. |
| `&&`, `\|\|` | Run the next pipeline if the preceding status is zero or nonzero, respectively. |
| `\|` | Connects stdout of one stage to stdin of the next. Use external programs for pipeline stages. |

Expansion normally stays in the same word. An exact `$@` or `"$@"` is the exception described below. The shell does no field splitting or wildcard filename expansion. Quote paths containing spaces and spell out the filenames you want. A completed pipeline reports the last stage’s status without job control. With job control, it reports the status of the last process the shell reaps for the pipeline.

A standalone `NAME=value` persists in the shell and reports status 0, including when the value uses command substitution. `NAME=value command` sets a value for one simple command and restores the previous value afterward. Use `export NAME=value` to set a value explicitly for child processes. In this release the shell also passes standalone assignments to its child processes. The shell expands command arguments before applying assignment prefixes.

```sh
MESSAGE='hello world'
echo "${MESSAGE}"
MESSAGE=temporary /bin/sh -c 'echo "$MESSAGE"'
echo "$MESSAGE"
false; echo "status=$?"
true && echo success
false || echo fallback
```

## Redirection and files

| Syntax | Behavior |
| --- | --- |
| `< file` | Reads stdin from a file. |
| `> file` | Creates or truncates a file for stdout. |
| `>> file` | Appends stdout from a native program to a file, creating it if needed. |
| `2> file` | Creates or truncates a file for stderr. |
| `2>&1` | Sends stderr to the current stdout destination. |

Order matters. `> file 2>&1` sends both streams to the file. Native program diagnostics use stderr, and normal output uses stdout. When a command name is missing from the search path, the shell applies its redirections, writes `command not found` to stderr and reports status 127. A stdout file in such a command is created empty unless stderr is redirected into it.

Create a fresh scratch directory for the file examples. Continue with the file examples only after the command succeeds. If the path already exists, choose an unused path and use it in both commands.

```sh
mkdir /tmp/hfoc-shell-demo && cd /tmp/hfoc-shell-demo
```

```sh
echo 'hello HFOC' > 'sample мир.txt'
echo 'second line' >> 'sample мир.txt'
cat < 'sample мир.txt' | wc -l
hfoc_no_such_command > stdout.txt 2> stderr.txt
echo "status=$?"
cat stderr.txt
hfoc_no_such_command > combined.txt 2>&1
cat combined.txt
```

The pipeline counts two lines. The first missing command reports 127, with an empty `stdout.txt` and the diagnostic in `stderr.txt`. The second writes the diagnostic to `combined.txt`.

## Scripts and positional parameters

Run `/bin/sh path/to/script arg ...` to read a script file. Run `/bin/sh -c 'commands' label arg ...` to run a command string. In a script file `$0` is the script path. With `-c`, an optional `label` sets `$0` and the following arguments become positional parameters.

- Use `$1` through `$9` to access individual arguments. `$#` is the parameter count.
- A word consisting exactly of `$@` or `"$@"` expands to one word per parameter, preserving empty arguments. With no parameters it yields no words. Embedded `$@` and `$*` join the parameters with spaces into one word.
- `shift [count]` removes positional arguments from the start, one by default.

From the scratch directory created above, write and run a script. It prints `first argument` and `second` on separate lines.

```sh
echo 'for item; do echo "$item"; done' > show-args.sh
/bin/sh ./show-args.sh 'first argument' second
/bin/sh -c 'echo "$0: $# arguments"; echo "$1"' demo 'first argument' second
```

## Conditions, loops and functions

| Form | Behavior |
| --- | --- |
| `if list; then list; elif list; then list; else list; fi` | Runs the branch whose condition returns status 0. `elif` and `else` are optional. |
| `for name in words; do list; done` | Iterates over the expanded words. |
| `for name; do list; done` | Iterates over the positional parameters. |
| `while list; do list; done` | Repeats while the condition returns 0. |
| `until list; do list; done` | Repeats until the condition returns 0. |
| `name() { list; }`, `function name { list; }` | Defines a function. Its arguments temporarily replace the positional parameters. |
| `{ list; }` | Groups a list in the current shell. |
| `( list )` | Runs a list with a saved shell state. Changes to variables, the working directory and positional parameters inside the group stay local to it. File changes remain visible. |

At the interactive prompt a function definition lasts for one input buffer. Define and call it on the same input line or place both in a script. `return [status]` leaves a function. `break [1]` leaves the innermost loop, and `continue [1]` starts its next iteration.

Use `$(list)` to capture a child `/bin/sh` process's stdout. The shell removes trailing newlines and does not split the resulting text into words. Stderr stays on its own stream. Paired backticks do not expand in the shipped shell.

```sh
greet() { echo "hello $1"; }; greet owner
if [ -d /home/owner ]; then echo workspace; else echo no-workspace; fi
for word in alpha 'beta gamma'; do echo "$word"; done
while true; do echo once; break; done
VALUE=before
( VALUE=inside; cd /tmp; pwd; echo "$VALUE" )
echo "$VALUE"
{ VALUE=after; echo "$VALUE"; }
echo "captured=$(echo native)"
/bin/sh -c 'trap "echo leaving" EXIT; echo running'
```

## Built-in commands

| Command | Behavior |
| --- | --- |
| `cd [directory]` | Changes the current directory. With no argument it uses `HOME`, falling back to `/` if unset or empty. |
| `export NAME=value`, `unset NAME` | Set an environment value or remove a variable. |
| `exit [status]` | Exits the shell. The preceding status is used if omitted. |
| `test expression`, `[ expression ]` | Return 0 for true, 1 for false or 2 for an invalid expression. Keep `[` and `]` as separate words. |
| `shift [count]`, `return [status]` | Shift positional parameters or leave a function. `return` uses the preceding status if omitted. |
| `break [1]`, `continue [1]` | Control the innermost loop. |
| `trap 'command' EXIT`, `trap - EXIT` | Set or clear one command to run when the shell exits normally. `EXIT` is the only event supported by `trap`. |
| `jobs`, `fg [%n]`, `bg [%n]`, `wait [%n]` | Inspect or control jobs in the current shell. |

For `test` and `[`, use a single string (true when nonempty), `-n string`, `-z string` or `! string`. File checks are `-e path`, `-f path`, `-d path`, `-r path` and `-w path`. `-r` checks whether the path can be looked up, and `-w` also checks the read-only flag. String comparisons use `=` and `!=`. Integer comparisons use `-eq`, `-ne`, `-lt`, `-le`, `-gt` and `-ge`. Combine separate tests with shell-level `&&` and `||`.

## Jobs and terminal signals

End a simple command, pipeline or and-or list with `&` to run it in a background child shell with its own process group. A compound statement cannot take `&` directly. The child inherits the working directory, standard streams and shell environment. Function definitions in the parent are local to its input buffer. A background group cannot read from the owner terminal.

- `jobs` lists jobs as `[n] Running|Stopped|Done command`.
- `fg [%n]` brings a job to the foreground, resumes it if stopped, waits and returns the job status.
- `bg [%n]` resumes a stopped job in the background. With no job number, `bg` and `fg` pick the running or stopped job with the highest number.
- `wait [%n]` waits for the specified job or all live jobs and returns the last status collected. For a job already stopped, resume it before waiting if you want it to finish.

Start a short background job and wait for it.

```sh
sleep 5 &
jobs
wait
echo "status=$?"
```

To try job control, enter `sleep 30` and press Ctrl+Z while it runs. Enter `jobs` to see the stopped job, `bg` to resume it and `fg` to bring it back. Press Ctrl+C to interrupt it and return to the prompt.

While a foreground job owns the terminal, Ctrl+C sends `SIGINT`, Ctrl+\ sends `SIGQUIT` and Ctrl+Z sends `SIGTSTP` to its process group. SSH signal requests `INT`, `QUIT`, `TSTP`, `CONT`, `TERM` and `HUP` also target the foreground group. With no foreground job, SSH `TERM` or `HUP` ends the session. Closing the SSH session sends `SIGHUP` to its jobs. Signal handling uses the default actions.

## Bounds and unsupported syntax

| Resource | Limit |
| --- | --- |
| Script or `-c` command string | 10 KiB buffer, including the terminating byte. |
| Structural parsing | 1024 token slots, 128 syntax-tree nodes, 512 list entries across the tree, 64 statements per list, six levels of compound nesting. |
| Simple command input | 4096-byte buffer, including the terminating byte. |
| Arguments and redirections | 64 argument slots including the command name, eight redirections per command, eight leading assignments for a single command. |
| Pipelines and conditional chains | Eight stages per pipeline, four pipelines in an `&&`/`\|\|` chain. |
| Shell variables | 12 variables including inherited entries. Names up to 63 bytes and values up to 255 bytes. |
| Functions and subshells | 32 functions, six active function calls, two nested parenthesized subshells. |
| Loops and substitution | 32 words per `for ... in` header, 10,000 iterations per `while` or `until` loop, 4096 bytes of captured stdout per substitution. |
| Positional parameters | Ten stored arguments with up to 255 bytes each. Individual indices cover `$1` through `$9`. |
| Jobs | Eight jobs and eight tracked processes per job. Job descriptions have a 128-byte buffer. |

The shell checks the structure of the input before executing it. A malformed compound command in a script exits with status 2. Limits on individual commands and expansions can also produce errors when those commands are reached. A substitution that exceeds its output bound fails with a diagnostic.

The language subset excludes `case`, `select`, `time`, arithmetic expansion, `set -e`, arrays, process substitution, here-documents, shell-level signal handlers and the `kill` builtin. Group syntax is for running lists. Use an external program for each pipeline stage and use the redirections listed above.
