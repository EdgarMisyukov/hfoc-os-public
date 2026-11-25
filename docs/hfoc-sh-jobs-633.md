# Native shell jobs and terminal signals (ITEM-633)

This is the job-control layer of the native HFOC shell.  It is not a POSIX
certification and does not add user-installed signal handlers.

## What is supported

- `cmd &` on a simple and-or list starts a background job.  The shell runs the
  list in a real `/bin/sh -c` child in a new process group; the parent records
  the job and returns to the prompt.  Compound statements followed directly by
  `&` are not claimed in this item.
- `jobs` prints `[n] Running|Stopped|Done <command>` for up to 8 jobs.
- `fg [%n]` makes the job's process group the terminal foreground group,
  continues it if it was stopped, waits for it, and returns its status.
- `bg [%n]` sends `SIGCONT` to a stopped job and leaves it in the background.
- `wait [%n]` waits for one job, or for every live job when no argument is
  given, and returns the last status.
- `Ctrl-C`, `Ctrl-\` and `Ctrl-Z` on the owner TTY are converted to `SIGINT`,
  `SIGQUIT` and `SIGTSTP` for the foreground process group while a job owns the
  terminal.  At the prompt (no foreground job) they stay ordinary bytes, so the
  line editor keeps its 629 behaviour.
- The SSH `signal` request is applied to the foreground group; `TERM`/`HUP`
  without a foreground job end the session.  Hangup signals the whole session
  (`SIGHUP`) before the shell is cancelled, so background jobs do not outlive
  the owner session.

## Kernel surface

- `process_control_block` carries `pgid`, `session_id`, `pending_signals`,
  `stop_signal`, `wait_stopped` and `wait_continued`; a live PID registry makes
  a bounded 64-PID lookup possible.
- Appended native calls: `KILL=21`, `WAIT2=22`, `SETPGID=23`,
  `TTY_FOREGROUND=24`, `SLEEP=25`.  `WRITE=0`/`YIELD=1`/`EXIT=2` and all
  earlier numbers keep their published meaning.
- `WAIT2` returns an encoded status: `(exit << 8)`, the signal number for a
  signalled child, `(stop_signal << 8) | 0x7f` for a stopped child, or
  `0xffff` for continued.  Flags are `NOHANG`, `UNTRACED` and `CONTINUED`.
- Signals have default actions only: HUP/INT/QUIT/TERM cancel the target with
  128+signal; TSTP suspends a non-current task off every scheduler queue; CONT
  makes it runnable again; CHLD is a no-op because the parent polls children.
  `kill` to self is refused; a group signal is confined to the caller's
  session.
- Only the session leader may set the terminal foreground group; a background
  group reading the TTY gets `EBADF` instead of consuming a foreground byte.
- The PIT/IOAPIC IRQ0 tick expires the scheduler timeout queue without
  preempting from the IRQ handler.  Blocking `WAIT`/`WAIT2` poll with
  interrupts enabled so a sleeping child can still be woken by the PIT.

## Bounds

8 jobs, 8 PIDs per job, 128-byte job text, 64 PIDs total, 2 TTYs, 16 KiB TTY
output, 4 KiB TTY input.  A stopped foreground pipeline is recorded once; a
full job table is a named error.  No unbounded heap growth and no new IRQ
routing, APIC/IOAPIC ownership or timer ownership.

## Production proof

`scripts/test-qemu-ssh.sh` drives one real stock-OpenSSH pty session with
`scripts/test-ssh-jobs.py`: the 629 shell checks, then `sleep 2 &` + `jobs` +
`wait`, then a foreground `sleep 30` stopped with Ctrl-Z, listed as Stopped,
continued with `bg`, returned with `fg`, interrupted with Ctrl-C, followed by a
new prompt and `echo job-done`.  The test requires the shell to survive every
step and the session to exit cleanly.  QEMU is not a Toshiba hardware proof.

## Known limits

- No `kill`/`trap` signal builtins beyond the 632 `trap ... EXIT`; the native
  kill call is available to programs.
- A background job is a child shell, so parent-shell functions and unexported
  variables are not inherited; exported variables, cwd and descriptors are.
- `wait` on a stopped job blocks until it is continued or terminated.
- The process group/session model is flat: one owner session, no login
  sessions, no controlling-terminal handoff to a new session leader.
