#!/usr/bin/env bash
# ITEM-427: host drill for the bounded retype of console commands typed through
# the QEMU HMP monitor.
#
# The drill replaces the guest with a fake console that models what the kernel
# actually does: a keystroke either reaches the line (and is echoed as
# `\r` + `hfoc> ` + the current line) or is dropped by the bounded input queue,
# and a submitted line produces its result now or after a delay.  It proves the
# three properties the fix rests on:
#
#   * a lost keystroke is retyped, bounded, and the command executes exactly
#     once when the retype succeeds;
#   * a line that was echoed is never retyped, so a slow guest cannot receive a
#     duplicated command (and the caller's exactness check stays meaningful);
#   * a budget that runs out raises LostKeystrokes instead of hanging, which is
#     the marker-timeout class the phase retries once.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(git rev-parse --show-toplevel)
cd "$ROOT"

python3 - <<'PY'
import pathlib
import sys
import time

sys.path.insert(0, "scripts")
import hfoc_console_command as retype

HELP = ("COMMANDS: HELP CLEAR VERSION UPTIME TICKS IRQ KBD MEM PMM HEAP "
        "ABOUT APPLIANCE APPLIANCE-BOOT CREDENTIALS LOGIN LANGUAGE "
        "REBOOT SHUTDOWN PANIC DIVZERO PAGEFAULT DOUBLEFAULT VMMMAP")
VERSION = "HFOC OS 1.0.0 X86-64"


class FakeConsole:
    """The kernel's console, reduced to what the retype helpers observe."""

    def __init__(self, drop=(), echo=True, result_delay=0.0,
                 key_lines=None, panic_after=None, shift=False):
        self.serial = bytearray()
        self.line = ""
        self.sent = 0
        self.drop = set(drop)
        self.echo = echo
        self.result_delay = result_delay
        self.key_lines = dict(key_lines or {})
        self.panic_after = panic_after
        # ITEM-511: the guest's Shift modifier stuck after the walk's
        # modifier-only wake: every letter arrives upper case, and the console
        # matches commands case-insensitively.
        self.shift = shift
        self.commands = []
        self._pending = []

    def send_key(self, key):
        index = self.sent
        self.sent += 1
        if self.panic_after is not None and index >= self.panic_after:
            self.serial += b"*** HFOC KERNEL PANIC ***\n"
            return
        if index in self.drop:
            return
        if key == "ret":
            self.serial += b"\n"
            self._pending.append((time.monotonic() + self.result_delay,
                                  self.line))
            self.line = ""
            self.serial += b"hfoc> "
        elif key == "backspace":
            if self.line:
                self.line = self.line[:-1]
                self._redraw()
        else:
            if key in self.key_lines:
                self.line = self.key_lines[key]
            else:
                self.line += key.upper() if self.shift else key
            self._redraw()

    def _redraw(self):
        if self.echo:
            self.serial += ("\rhfoc> " + self.line).encode("ascii")

    def _write_result(self, line):
        if not line:
            return
        self.commands.append(line)
        # The console's command vocabulary is case-insensitive; the submitted
        # line is recorded as it arrived, upper case when Shift was stuck.
        if line.lower() == "help":
            result = HELP + "\n"
        elif line.lower() == "version":
            result = VERSION + "\n"
        else:
            result = "UNKNOWN COMMAND: " + line + ". TYPE HELP.\n"
        self.serial += result.encode("ascii")

    def serial_bytes(self):
        now = time.monotonic()
        ready = [item for item in self._pending if item[0] <= now]
        self._pending = [item for item in self._pending if item[0] > now]
        for _, line in ready:
            self._write_result(line)
        return bytes(self.serial)


def expect(condition, message):
    if not condition:
        raise SystemExit("console retype host: " + message)


def one_marker(marker):
    def probe(suffix):
        count = suffix.count(marker)
        if count > 1:
            raise SystemExit(
                f"console retype host: expected one {marker!r}, got {count}")
        return True if count == 1 else None
    return probe


# The budgets are bounded: an unbounded retype would hide a real failure.
expect(0 < retype.ATTEMPTS <= 4, f"attempts are unbounded: {retype.ATTEMPTS}")
expect(0 < retype.RETYPE_WINDOW <= 60.0,
       f"retype window is not bounded: {retype.RETYPE_WINDOW}")
expect(retype.CLEAR_KEYS >= 4,
       "the retype must clear a partial line before it types again")

deadline = time.monotonic() + 30.0

# 1. The bounded queue dropped the first two keystrokes of the line.  The
#    helper retypes it; the command runs once and only after it was submitted.
console = FakeConsole(drop=(0, 1))
offset = retype.type_line(console.send_key, console.serial_bytes, "help",
                          deadline, attempts=3, window=0.3)
expect(console.commands == [],
       "a command ran while the line was still being typed")
expect("\rhfoc> help" in console.serial_bytes()[offset:].decode("ascii"),
       "the retyped line was not echoed")
retype.submit(lambda: console.send_key("ret"), console.serial_bytes,
              one_marker(HELP), deadline, attempts=3, window=0.3)
expect(console.commands == ["help"],
       f"the retyped command did not run exactly once: {console.commands}")
expect(console.sent <= 64,
       f"the retype was not bounded: {console.sent} keys sent")

# 2. The line was delivered but its result is slower than one window: the proof
#    must wait, and the repeated Enter must submit an empty line, never the
#    command a second time.
console = FakeConsole(result_delay=0.5)
retype.type_line(console.send_key, console.serial_bytes, "help", deadline,
                 attempts=3, window=0.3)
value = retype.submit(lambda: console.send_key("ret"), console.serial_bytes,
                      one_marker(HELP), deadline, attempts=3, window=0.3)
expect(value is True, "the delayed command result was not observed")
expect(console.commands == ["help"],
       f"a slow result duplicated the command: {console.commands}")

# 3. Every keystroke is lost: the helper gives up with LostKeystrokes inside its
#    budget - that is the class the phase retries - instead of hanging forever.
console = FakeConsole(drop=range(0, 1000))
try:
    retype.type_line(console.send_key, console.serial_bytes, "help", deadline,
                     attempts=3, window=0.2)
except retype.LostKeystrokes:
    pass
else:
    raise SystemExit("console retype host: a lost line was not reported")
expect(console.commands == [], "a lost line executed a command")
expect(console.sent <= 64, f"the give-up was not bounded: {console.sent}")

# 4. A navigation key is lost: the redraw it owns never appears, the key is
#    sent again, and the state transition happens exactly once.
console = FakeConsole(drop=(0,), key_lines={"up": "version"})
offset = retype.press_key(lambda: console.send_key("up"),
                          console.serial_bytes, "version", deadline,
                          attempts=3, window=0.3)
expect("\rhfoc> version" in console.serial_bytes()[offset:].decode("ascii"),
       "the retried navigation key did not redraw the expected line")
expect(console.sent == 2,
       f"the navigation key was not retried exactly once: {console.sent}")

# 5. A panic during the retype is fail-closed: it is never retried.
console = FakeConsole(panic_after=2)
try:
    retype.type_line(console.send_key, console.serial_bytes, "help", deadline,
                     attempts=3, window=0.3)
except SystemExit as exc:
    expect("Kernel panic" in str(exc), f"wrong panic report: {exc}")
else:
    raise SystemExit("console retype host: a panic was not reported")

# 6. ITEM-511: the walk's modifier-only wake can leave Shift set, so the console
#    echoes the line upper case while its vocabulary matches case-insensitively.
#    The witness option confirms the upper-case echo without retyping, and the
#    command still runs exactly once; without the witness the same console is
#    not confirmed, which is the failure the two-controller proof hit.
console = FakeConsole(shift=True)
offset = retype.type_line(console.send_key, console.serial_bytes, "help",
                          deadline, attempts=3, window=0.2,
                          witness=str.lower)
expect(console.sent == 4,
       f"the upper-case echo was retyped without a loss: {console.sent} keys")
expect(console.commands == [], "the upper-case line ran before Enter")
value = retype.submit(lambda: console.send_key("ret"), console.serial_bytes,
                      one_marker(HELP), deadline, attempts=3, window=0.3)
expect(value is True, "the upper-case command's result was not observed")
expect(console.commands == ["HELP"],
       f"the upper-case command ran as {console.commands}")
console = FakeConsole(shift=True)
try:
    retype.type_line(console.send_key, console.serial_bytes, "help", deadline,
                     attempts=3, window=0.2)
except retype.LostKeystrokes:
    pass
else:
    raise SystemExit(
        "console retype host: the upper-case echo was accepted without the "
        "witness")

print("console retype host: lost keystroke retyped (1)")
print("console retype host: echoed line never duplicated (1)")
print("console retype host: exhausted budget is LostKeystrokes (1)")
print("console retype host: lost navigation key retried (1)")
print("console retype host: panic is fail-closed (1)")
print("console retype host: upper-case echo confirmed with the witness (1)")
print("console retype host: OK")
PY
