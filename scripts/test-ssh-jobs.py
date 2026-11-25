#!/usr/bin/env python3
"""Drive stock OpenSSH through a real pty and prove the shell.

One session covers the shell language, the ITEM-658 line editor, and job
control (background + wait, Ctrl-Z stop, jobs/bg/fg and Ctrl-C interrupt).
The output is also written to SSH_SHELL_OUT for the caller's marker checks.
"""
import fcntl
import os
import pty
import re
import struct
import subprocess
import sys
import termios
import threading
import time
import tty

PORT = os.environ["PORT"]
KEY = os.environ["SSH_KEY"]
KNOWN_HOSTS = os.environ["SSH_KNOWN_HOSTS"]
OUT = os.environ.get("SSH_SHELL_OUT", "")
COLUMNS = 20

CMD = [
    "ssh", "-F", "/dev/null", "-p", PORT, "-i", KEY,
    "-o", "IdentitiesOnly=yes",
    "-o", "UserKnownHostsFile=" + KNOWN_HOSTS,
    "-o", "StrictHostKeyChecking=yes",
    "-o", "BatchMode=yes",
    "-o", "KexAlgorithms=ecdh-sha2-nistp256",
    "-o", "Ciphers=aes128-gcm@openssh.com",
    "-o", "MACs=hmac-sha2-256",
    "-tt", "hfoc@127.0.0.1",
]

output = []


def _read(stream):
    for chunk in iter(lambda: stream.read(1), ""):
        output.append(chunk)


def send(input_stream, text, delay=0.8):
    time.sleep(delay)
    input_stream.write(text.encode("utf-8"))
    input_stream.flush()


def main():
    master, slave = pty.openpty()
    tty.setraw(slave)
    fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 4, COLUMNS, 0, 0))
    process = subprocess.Popen(
        CMD, stdin=slave, stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, text=True, encoding="utf-8", bufsize=1)
    process.stdout.reconfigure(newline="")
    os.close(slave)
    input_stream = os.fdopen(master, "wb")
    readers = [
        threading.Thread(target=_read, args=(process.stdout,), daemon=True),
        threading.Thread(target=_read, args=(process.stderr,), daemon=True),
    ]
    for thread in readers:
        thread.start()
    timed_out = False
    broken = False
    try:
        send(input_stream, "echo shell-ok\n")
        send(input_stream, "/bin/echo pipeline | /bin/wc -l\n", 0.5)
        send(input_stream, "mkdir /tmp/shell-dir\n", 0.4)
        send(input_stream, "cd /tmp/shell-dir\n", 0.4)
        send(input_stream, "pwd\n", 0.4)
        send(input_stream, "echo file > note\n", 0.4)
        send(input_stream, "cat note\n", 0.4)
        send(input_stream, "cp note copy\n", 0.4)
        send(input_stream, "mv copy renamed\n", 0.4)
        send(input_stream, "ls\n", 0.4)
        send(input_stream, "rm renamed\n", 0.4)
        send(input_stream, "cat note\n", 0.4)
        send(input_stream, "false; echo status=$?\n", 0.4)
        send(input_stream, "true; echo status=$?\n", 0.4)
        send(input_stream, "echo 'echo script-ok' > /tmp/sh-script\n", 0.4)
        send(input_stream, "sh /tmp/sh-script\n", 0.8)
        # ITEM-658: these bytes must go through the actual SSH TTY and /bin/sh.
        send(input_stream, "echo editor-utf8_é_мир > editor-имя\n", 0.4)
        send(input_stream, "cat editor-имя\n", 0.4)
        send(input_stream, "ls\n", 0.4)
        send(input_stream, "echo editor-history\n", 0.4)
        send(input_stream, "\x1b[A\n", 0.4)
        send(input_stream, "echo editor-drat\x1b[D\x1b[A\x1b[Bf\n", 0.4)
        send(input_stream, "echo editor-cursor-ac\x1b[Db\x1b[Cd\n", 0.4)
        send(input_stream, "echo editor-boundary-aéz\x1bOD\x1bODX\n", 0.4)
        send(input_stream, "echo editor-wide-a界z\x1b[D\x1b[D\x1b[CX\n", 0.4)
        send(input_stream, "echo editor-delete-aé\x7fb\n", 0.4)
        send(input_stream, "echo editor-escape-\x1b[15~kept\n", 0.4)
        send(input_stream, "echo view-" + "x" * 26 + "\x1b[Dy\n", 0.4)
        send(input_stream, "echo -n no-eol\n", 0.4)
        send(input_stream, "echo editor-output-ac\x1b[Db\n", 0.4)
        for index in range(9):
            send(input_stream, f"echo editor-ring-{index}\n", 0.15)
        send(input_stream, "\x1b[A" * 12 + "\n", 0.4)
        send(input_stream, "echo editor-cancel-fired\x03", 0.4)
        send(input_stream, "echo editor-cancel-status-$?\x04\n", 0.4)
        overlong = "echo editor-overflow-fired".ljust(4096)
        for start in range(0, len(overlong), 256):
            send(input_stream, overlong[start:start + 256], 0.08)
        send(input_stream, "\necho editor-overflow-status-$?\n", 0.4)
        # ITEM-633 job control in the same session.
        send(input_stream, "sleep 2 &\n", 0.5)
        send(input_stream, "jobs\n", 0.5)
        send(input_stream, "wait\n", 0.5)
        send(input_stream, "echo bg-done\n", 3.0)
        send(input_stream, "sleep 30\n", 1.0)
        send(input_stream, "\x1a", 1.5)          # Ctrl-Z stops the foreground job
        send(input_stream, "jobs\n", 1.0)
        send(input_stream, "bg\n", 1.0)
        send(input_stream, "jobs\n", 1.5)
        send(input_stream, "fg\n", 1.0)
        send(input_stream, "\x03", 1.0)          # Ctrl-C interrupts the foreground job
        send(input_stream, "jobs\n", 1.5)
        send(input_stream, "echo job-done\n", 1.0)
        send(input_stream, "exit\n", 1.5)
    except BrokenPipeError:
        broken = True
    try:
        returncode = process.wait(timeout=15)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=5)
        returncode = -1
        timed_out = True
    finally:
        input_stream.close()
    for thread in readers:
        thread.join()
    text = "".join(output)
    if OUT:
        with open(OUT, "w", encoding="utf-8") as handle:
            handle.write(text)
    failures = []
    for marker in ("hfoc$", "shell-ok", "/tmp/shell-dir", "file",
                   "renamed", "status=1", "status=0", "script-ok",
                   "bg-done", "Stopped", "Running", "job-done"):
        if marker not in text:
            failures.append("missing output: " + marker)
    # A command's echo contains the marker too. Count complete output lines.
    lines = text.splitlines()
    for marker, count in (
        ("editor-utf8_é_мир", 1), ("editor-имя", 1), ("editor-history", 2),
        ("editor-draft", 1), ("editor-cursor-abcd", 1),
        ("editor-boundary-aXéz", 1), ("editor-wide-a界Xz", 1),
        ("editor-delete-ab", 1),
        ("editor-escape-kept", 1), ("view-" + "x" * 25 + "yx", 1),
        ("editor-output-abc", 1),
        ("editor-ring-0", 1), ("editor-ring-1", 2),
        ("editor-cancel-fired", 0), ("editor-cancel-status-130", 1),
        ("editor-overflow-fired", 0), ("editor-overflow-status-2", 1),
    ):
        if lines.count(marker) != count:
            failures.append(f"line editor output count for {marker!r}: "
                            f"expected {count}, got {lines.count(marker)}")
    # The PTY has 20 columns. With a six-cell prompt, no ASCII redraw may
    # reach column 20, where a following byte would wrap and may scroll.
    start = text.find("echo view-")
    end = text.find("\r\n", start)
    trace = re.sub(r"\x1b(?:[78]|\[[JK])", "", text[start:end])
    redraws = trace.split("\r")
    budget = COLUMNS - len("hfoc$ ") - 1
    if (start < 0 or end < 0 or len(redraws) < 2 or len(redraws[0]) > budget
            or any(len(chunk) >= COLUMNS for chunk in redraws[1:])):
        failures.append("line editor display wrapped in the 20-column PTY")
    # A command without a final newline leaves the cursor partway across
    # its row. The next editable prompt must start on a fresh row.
    _, partial, after = text.partition("\r\nno-eol")
    gap, prompt, _ = after.partition("hfoc$ ")
    pads_through_margin = (gap.endswith("\r") and not gap[:-1].strip(" ")
                           and len(gap) - 1 > COLUMNS - len("no-eol"))
    if not partial or not prompt or ("\r\n" not in gap and not pads_through_margin):
        failures.append("line editor prompt overwrote partial command output")
    if "cannot start" in text or "panic" in text.lower():
        failures.append("shell reported a start/panic failure")
    if timed_out:
        failures.append("the shell session did not exit (timed out)")
    if broken:
        failures.append("the shell closed the session before the script ended")
    if returncode != 0:
        failures.append("ssh exited with %d" % returncode)
    if failures:
        sys.stderr.write("\n".join(failures) + "\n")
        sys.stderr.write("---- shell output ----\n")
        sys.stderr.write(text)
        return 1
    print("SSH shell/job-control proof: OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
