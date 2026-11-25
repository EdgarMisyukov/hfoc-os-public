#!/usr/bin/env python3
"""CI-SPEEDUP-3 host witness: the expected refusal is answered, not timed out.

The speedup measurement (PR #761, run 41738411880) put 187 s of the main HTTPS
proof into `account-durability/console-policy`, and 121 s of that belonged to
no typed answer: the proof's expected-refusal call drove the whole sign-in
conversation and then waited out the 120-second `ADMIN SESSION` marker for a
session that a no-longer-current password can never be issued.  The refusal the
call was looking for had already been printed.

This witness extracts the shipped helper from
`scripts/test-qemu-production-service-https.sh` - it cannot drift from the code
under test - and drives the shipped `refuse` mode against a fake QEMU monitor
that speaks the same HMP protocol, appending the console's answers to a serial
file the way the guest would.  The claims:

  * a fresh full refusal line makes the mode succeed and print `refused`;
  * the same line already in the serial *before* the attempt does not satisfy
    it - the stale marker times out, so the recorded serial position is real;
  * a session issued for the retired password fails the check by name;
  * a refusal for another reason (`THROTTLED`) fails and names the reason;
  * a refusal the log sees while it is still being printed is waited out: a
    prefix of the expected line is not another reason (run 42075493165 failed
    exactly that way - the console printed the whole line, the read saw 31
    characters of it, and the proof called it a different refusal);
  * a console that answers nothing fails on the bounded deadline;
  * a monitor that hangs up fails instead of being retried;
  * the proof calls the mode exactly once, keeps the standard `login-gate`
    after it, and no longer asserts the refusal as "the login mode failed".
"""
import pathlib
import socket
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]
PROOF = ROOT / "scripts" / "test-qemu-production-service-https.sh"

REFUSAL = "[ADMIN-ENROLLMENT] login refused: " \
          "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED"
THROTTLED = "[ADMIN-ENROLLMENT] login refused: " \
            "APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_THROTTLED"
SESSION = "ADMIN SESSION: Authorization: Bearer " + "A" * 32
LOGIN = "admin2"
SECRET = "oldsecret1"

checks = 0
failures = 0


def check(name, condition, detail=""):
    global checks, failures
    checks += 1
    if condition:
        print(f"  ok   {name}", flush=True)
    else:
        failures += 1
        print(f"  FAIL {name}" + (f" -- {detail}" if detail else ""),
              flush=True)


def extract_heredoc(lines):
    """The embedded admin-enroll.py, between its heredoc markers."""
    start = None
    for index, line in enumerate(lines):
        if line.strip() == "cat >\"$TEST_DIR/admin-enroll.py\" <<'ENROLLEOF'":
            start = index + 1
            break
    if start is None:
        return ""
    for end in range(start, len(lines)):
        if lines[end].strip() == "ENROLLEOF":
            return "\n".join(lines[start:end]) + "\n"
    return ""


class Monitor:
    """A QEMU HMP stand-in: one prompt per command, one guest answer per ENTER.

    `answers[n]` is what the console prints after the n-th typed line, exactly
    like a console that asks its next question once ENTER is received.
    """

    def __init__(self, path, serial, answers, hangup_after=None,
                 followup=None):
        self.path = path
        self.serial = serial
        self.answers = answers
        self.hangup_after = hangup_after
        # A console answer the log sees in pieces: `(delay, payload)` printed
        # after the last typed line, with no further ENTER in between.
        self.followup = followup
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.bind(str(path))
        self.sock.listen(1)
        self.thread = threading.Thread(target=self._serve, daemon=True)

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *unused):
        self.sock.close()
        self.thread.join(timeout=5)

    def _reply(self, connection, payload):
        if payload:
            with self.serial.open("a", encoding="ascii") as handle:
                handle.write(payload)
        try:
            connection.sendall(b"(qemu) ")
        except OSError:
            # The mode already finished (the follow-up of a failed case): the
            # console it was talking to is gone, which is not the witness's
            # business.
            return

    def _serve(self):
        connection, _ = self.sock.accept()
        with connection:
            connection.sendall(b"(qemu) ")
            buffer = b""
            entered = 0
            while True:
                chunk = connection.recv(4096)
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    text = line.strip()
                    if not text:
                        continue
                    if text == b"sendkey ret":
                        entered += 1
                        if (self.hangup_after is not None and
                                entered >= self.hangup_after):
                            return
                        answer = ""
                        if entered <= len(self.answers):
                            answer = self.answers[entered - 1]
                        self._reply(connection, answer)
                        if (self.followup is not None and
                                entered == len(self.answers)):
                            delay, payload = self.followup
                            time.sleep(delay)
                            self._reply(connection, payload)
                    else:
                        connection.sendall(b"(qemu) ")


def run_mode(work, name, answers, seed="", hangup_after=None,
             timeout="60", followup=None):
    """Run the shipped `refuse` mode once against a scripted console."""
    helper = work / "shipped-admin-enroll.py"
    helper.write_text(work.joinpath("helper-source.py").read_text())
    serial = work / f"{name}.log"
    serial.write_text(
        "[PRODUCTION-FIRST-BOOT] state=COMMITTED\nhfoc> " + seed,
        encoding="ascii")
    monitor_path = work / f"{name}.monitor"
    with Monitor(monitor_path, serial, answers, hangup_after, followup):
        try:
            completed = subprocess.run(
                [sys.executable, str(helper), "refuse", str(monitor_path),
                 str(serial), SECRET, LOGIN],
                capture_output=True, text=True,
                env={"PATH": "/usr/bin:/bin", "LC_ALL": "C",
                     "HFOC_ADMIN_REFUSE_TIMEOUT": timeout},
                timeout=45)
        except subprocess.TimeoutExpired as expired:
            # A mode that never answers is a failure of the mode, not of the
            # witness: report it the way a timeout on the deadline would read.
            completed = subprocess.CompletedProcess(
                expired.cmd, 124, b"", b"witness: the mode did not finish")
            if expired.stdout:
                completed.stdout = expired.stdout.decode(errors="replace")
            if expired.stderr:
                completed.stderr = expired.stderr.decode(errors="replace")
    return completed, serial.read_text(encoding="ascii")


def main():
    lines = PROOF.read_text().splitlines()
    text = PROOF.read_text()
    helper_source = extract_heredoc(lines)
    check("the proof ships its embedded python helper", bool(helper_source))

    check("the proof asks for the expected refusal by mode",
          'admin-enroll.py" refuse' in text)
    check("the retired password is still typed for the new login",
          '"$ADMIN_SECRET" "$ACCOUNT_NEW_LOGIN"' in text)
    check("the old timeout-shaped expectation is gone",
          "The old password still signs in on the local console" not in text)
    branch_at = helper_source.find('elif mode == "refuse":')
    branch_end = helper_source.find("\n    else:\n", branch_at)
    branch = helper_source[branch_at:branch_end]
    check("the refusal branch is in the shipped helper", branch_at >= 0)
    check("the refusal is the denied line, spelled out in the helper",
          '"[ADMIN-ENROLLMENT] login refused: "' in branch and
          '"APPLIANCE_PRODUCTION_ADMIN_ENROLLMENT_DENIED"' in branch,
          branch[:200])
    check("the mode fixes the serial position before the password",
          "attempt_offset = len(serial_text())" in helper_source)
    check("the bounded deadline is overridable for this witness",
          "HFOC_ADMIN_REFUSE_TIMEOUT" in helper_source)
    refuse_at = text.find('admin-enroll.py" refuse')
    gate_at = text.find('admin-enroll.py" login-gate')
    check("the standard login-gate still follows the refusal check",
          0 <= refuse_at < gate_at)

    with tempfile.TemporaryDirectory(prefix="hfoc-refusal-") as tmp:
        work = pathlib.Path(tmp)
        work.joinpath("helper-source.py").write_text(helper_source)

        fresh, fresh_serial = run_mode(
            work, "fresh",
            ["\nLogin: ", "\nPassword: ", "\n" + REFUSAL + "\n"])
        check("a fresh refusal line makes the mode succeed",
              fresh.returncode == 0,
              f"exit={fresh.returncode} stderr={fresh.stderr.strip()!r}")
        check("the mode reports the refusal it accepted",
              "refused" in fresh.stdout.split(), fresh.stdout.strip())
        check("the refusal reaches the serial the way the console prints it",
              REFUSAL in fresh_serial)

        stale, _ = run_mode(
            work, "stale",
            ["\nLogin: ", "\nPassword: ", ""],
            seed="\n" + REFUSAL + "\nhfoc> ", timeout="2")
        check("a refusal that predates the attempt does not satisfy it",
              stale.returncode != 0,
              f"exit={stale.returncode} stdout={stale.stdout.strip()!r}")
        check("the stale case fails for the missing answer",
              "answered nothing" in stale.stderr, stale.stderr.strip())

        session, _ = run_mode(
            work, "session",
            ["\nLogin: ", "\nPassword: ", "\n" + SESSION + "\n"])
        check("a session issued for the retired password fails",
              session.returncode != 0,
              f"exit={session.returncode}")
        check("the unexpected session is named",
              "signs in" in session.stderr, session.stderr.strip())

        throttled, _ = run_mode(
            work, "throttled",
            ["\nLogin: ", "\nPassword: ", "\n" + THROTTLED + "\n"])
        check("a refusal for another reason fails",
              throttled.returncode != 0, f"exit={throttled.returncode}")
        check("the other reason is named",
              "THROTTLED" in throttled.stderr, throttled.stderr.strip())

        torn, torn_serial = run_mode(
            work, "torn",
            ["\nLogin: ", "\nPassword: ", "\n" + REFUSAL[:40]],
            followup=(0.4, REFUSAL[40:] + "\n"))
        check("a refusal read while it is still being printed is waited out",
              torn.returncode == 0,
              f"exit={torn.returncode} stderr={torn.stderr.strip()!r}")
        check("the torn refusal is accepted once its terminator arrives",
              "refused" in torn.stdout.split(), torn.stdout.strip())
        check("the whole denial line reaches the serial",
              REFUSAL in torn_serial)

        silent, _ = run_mode(
            work, "silent",
            ["\nLogin: ", "\nPassword: ", ""], timeout="2")
        check("a console that answers nothing fails on the deadline",
              silent.returncode != 0, f"exit={silent.returncode}")
        check("the missing answer is reported", "answered nothing" in
              silent.stderr, silent.stderr.strip())

        hung, _ = run_mode(
            work, "hangup",
            ["\nLogin: ", "\nPassword: ", "\n" + REFUSAL + "\n"],
            hangup_after=1)
        check("a monitor that hangs up fails the check",
              hung.returncode != 0, f"exit={hung.returncode}")

    print(f"\n{checks} checks, {failures} failures")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
