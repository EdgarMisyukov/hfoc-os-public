#!/usr/bin/env python3
"""ITEM-427: bounded retype for console commands typed through the HMP monitor.

A proof that drives the guest console with HMP `sendkey` types one keystroke per
monitor round trip, and every keystroke travels through the guest's local input
path: the kernel keeps a bounded queue and `local_input_push()` drops the newest
event while the console has not drained it (`kernel/src/local_input.c`).  Under a
loaded host the guest may not be scheduled between two `sendkey` commands, the
queue overflows, and a command the proof typed never reaches the console.  The
proof then waits for the result of a command the guest never saw, its deadline
expires, and the phase retries a healthy machine - measured on 2026-09-16 as
192/193/197 seconds on `test-lapic-timer-oneshot`, `test-pic-disable` and
`test-console-history`.

The console echoes every accepted keystroke: `redraw_input_line()` writes
`\\r` + `hfoc> ` + the current line to the same serial stream the proof reads,
so a line that never appeared in the serial log was never delivered.  These
helpers use that echo as the delivery witness:

  * `type_line()` types a line and returns the serial offset of the attempt
    whose echo confirmed it.  A line that was never echoed is retyped, bounded,
    from a cleared line; backspace on an empty console line is a silent no-op,
    so clearing costs nothing when the lost attempt left nothing behind.
  * `submit()` presses Enter until the command's result probe succeeds.
    Repeating Enter is safe: the first Enter that reached the console submitted
    the echo-confirmed line, and a later one submits an empty line.
  * `press_key()` sends one editing key and waits for the redraw that key owns,
    so a lost `up`/`down` is sent again instead of corrupting the history state.

`type_line()` injects the characters and the clearing backspaces by name, so it
takes a `send_key(key)` callable.  `submit()` and `press_key()` always resend
the same key, so they take a no-argument sender instead; that keeps a caller's
key sequence spelled out at its own call site.

Nothing here weakens a proof.  A line is never retyped while the console is
showing it, so a slow guest cannot receive a duplicated command; the caller's
exactness checks still run against the output of the attempt that succeeded; the
retype is bounded by `ATTEMPTS` and `RETYPE_WINDOW`; and a budget that runs out
raises `LostKeystrokes`, which the proof reports as the canonical marker-timeout
record the phase already retries once.  A kernel panic remains fail-closed: it
raises `SystemExit` at once and is never retried.
"""

import re
import time

# One line is typed at most this many times, and the console is given this long
# to echo a line or answer a submitted command.  The window is intentionally
# small: a loss is visible as a missing echo almost immediately, while a guest
# that is genuinely starved falls through to the phase's own retry rule.
ATTEMPTS = 3
RETYPE_WINDOW = 15.0
# A lost attempt may have left a partial line behind.  Backspace on an empty
# line writes nothing, so clearing is harmless when there is nothing to clear.
CLEAR_KEYS = 16
CLEAR_KEY = "backspace"
PROMPT_ECHO = "\rhfoc> "
PANIC_MARKER = "*** HFOC KERNEL PANIC ***"
# ITEM-482: the fast path paints one cell per character and serializes that one
# character, so the typed line itself is the delivery witness.  A complete
# `[NAME] ...` record may land between two characters, so the witness view drops
# whole record lines before the contiguous search; a dropped character still
# makes the run impossible.
_RECORD_LINE = re.compile(r"\[[A-Z][A-Z0-9 _-]*\][^\r\n]*\r?\n")

_PENDING = object()


class LostKeystrokes(Exception):
    """The console never confirmed a typed line or an editing key."""


def echo(line):
    """The exact bytes the console's redraw writes for the current line."""
    return PROMPT_ECHO + line


def delivery_view(text):
    """The typed-echo view: record lines removed, characters kept in order."""
    return _RECORD_LINE.sub("", text)


def _suffix(serial_bytes, offset):
    data = serial_bytes()
    if offset >= len(data):
        return ""
    return data[offset:].decode("ascii", errors="replace")


def _wait(predicate, serial_bytes, offset, deadline, window):
    """Poll `predicate` for at most `window`, and never past `deadline`.

    `predicate` returns None while the condition is not there yet.  A panic in
    the observed suffix raises straight away: it belongs to the fail-closed
    class and is never retried.
    """
    attempt_deadline = min(deadline, time.monotonic() + window)
    while time.monotonic() < attempt_deadline:
        if PANIC_MARKER in _suffix(serial_bytes, offset):
            raise SystemExit("Kernel panic during console input")
        value = predicate()
        if value is not None:
            return value
        time.sleep(0.05)
    return _PENDING


def type_line(send_key, serial_bytes, line, deadline, *,
              attempts=ATTEMPTS, window=RETYPE_WINDOW, witness=None):
    """Type `line` and return the offset of the attempt its echo confirmed.

    Raises LostKeystrokes when no attempt inside the budget was echoed.

    ITEM-511: `witness` is an optional transform applied to the expected line
    and to the observed suffix before the echo is compared.  A proof whose wake
    leaves the guest's Shift modifier set - the modifier-only `sendkey shift`
    reports ITEM-452 types through the walk - sees the console echo the line in
    upper case, while the console's own command vocabulary is case-insensitive
    (`console_command_is`).  Such a proof passes `witness=str.lower`, and the
    delivery witness stays exact in every other respect: the whole line still
    has to arrive contiguously, and a missing character still fails the attempt.
    """
    confirm = witness if witness is not None else (lambda text: text)

    for attempt in range(1, attempts + 1):
        if time.monotonic() >= deadline:
            break
        if attempt > 1:
            for _ in range(CLEAR_KEYS):
                send_key(CLEAR_KEY)
        offset = len(serial_bytes())
        for character in line:
            send_key(character)
        # ITEM-482: a redraw (`echo`) or the fast path's own characters
        # (`delivery_view`) both witness the line; a broken contiguous run in
        # either view means a character never arrived, and the attempt retries.
        wanted_echo = confirm(echo(line))
        wanted_line = confirm(line)
        confirmed = _wait(
            lambda: True
            if wanted_echo in confirm(_suffix(serial_bytes, offset)) or
            wanted_line in confirm(delivery_view(_suffix(serial_bytes, offset)))
            else None,
            serial_bytes, offset, deadline, window)
        if confirmed is not _PENDING:
            return offset
    raise LostKeystrokes(f"the console never echoed the typed line {line!r}")


def submit(press, serial_bytes, probe, deadline, *,
           attempts=ATTEMPTS, window=RETYPE_WINDOW):
    """Press Enter until `probe(suffix)` returns a value.

    `press` presses Enter once.  `probe` receives the serial suffix written
    after the Enter it belongs to, returns None while the result is not there
    yet, and may raise SystemExit on a definite mismatch (that is an assertion,
    and it fails at once).  The value of the probe that succeeded is returned.
    """
    for _ in range(attempts):
        if time.monotonic() >= deadline:
            break
        offset = len(serial_bytes())
        press()
        value = _wait(lambda: probe(_suffix(serial_bytes, offset)),
                      serial_bytes, offset, deadline, window)
        if value is not _PENDING:
            return value
    raise LostKeystrokes("the console never answered the submitted command")


def press_key(press, serial_bytes, expected_line, deadline, *,
              attempts=ATTEMPTS, window=RETYPE_WINDOW):
    """Press one key until its redraw shows `expected_line`.

    `press` presses the key once.  Returns the offset of the attempt whose
    redraw confirmed the transition, and raises LostKeystrokes when the redraw
    never appeared inside the budget.
    """
    for _ in range(attempts):
        if time.monotonic() >= deadline:
            break
        offset = len(serial_bytes())
        press()
        confirmed = _wait(
            lambda: True if echo(expected_line) in _suffix(serial_bytes, offset)
            else None,
            serial_bytes, offset, deadline, window)
        if confirmed is not _PENDING:
            return offset
    raise LostKeystrokes(f"the console never redrew {expected_line!r}")
