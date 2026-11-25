#!/usr/bin/env python3
"""Wait for one QEMU proof marker in the production serial stream.

The QEMU proofs used to carry their own bounded marker loops.  The update proof
(``scripts/test-qemu-production-install-flow-update.sh``) is the one this helper
was extracted from: its positive phase waits for ``[PRODUCTION-INSTALL]
applied``, and a production refusal is a terminal answer, not a slow success.

The loop is kept in one file so the proof and the host regression
(``tests/qemu-marker-wait-host.sh``) exercise the same code instead of two
copies of the decision.

Usage:
    hfoc-qemu-marker-wait.py <serial-log> <marker> <timeout-seconds> \
        <hfoc-serial-view.py> [--stop-pid PID]

``--stop-pid`` freezes that process (normally the QEMU emulator) with SIGSTOP
before returning from a successful wait.  A negative install scenario needs it:
after a terminal refusal the guest continues into the installed appliance,
whose own boot writes runtime state; freezing the vCPU at the refusal keeps
those later writes out of the "the refusal wrote nothing" proof.

Exit codes:
    0  the marker appeared
    1  the bounded wait expired without the marker
    2  the guest printed a kernel panic
    3  the guest printed a terminal production refusal
    4  the marker appeared but --stop-pid could not be signalled
"""
import importlib.util
import os
import pathlib
import signal
import sys
import time

PANIC = "*** HFOC KERNEL PANIC ***"
# The production install renderers finish every refusal with ` writes=0`
# (kernel/src/production_install_media.c: production_install_media_render_refusal
# and ..._render_controller_refusal).  Requiring the suffix keeps a line that
# is still being printed - `refused reason=CONT` on its way to `CONTROLLER` -
# from being mistaken for the terminal answer the negative scenario awaits.
TERMINAL_REFUSAL_PREFIX = "[PRODUCTION-INSTALL] refused reason="
TERMINAL_REFUSAL_SUFFIX = " writes=0"


def load_serial_view(path):
    """Load the shared serial normaliser from the proof's own tree."""
    spec = importlib.util.spec_from_file_location("hfoc_serial_view", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_text(path):
    try:
        return pathlib.Path(path).read_bytes().decode("ascii", errors="replace")
    except FileNotFoundError:
        return ""


def terminal_refusal(text):
    """The first complete production refusal line, or None.

    A refusal is terminal for the positive phase: the installer has already
    printed `writes=0`, so the `applied` marker will never follow.  Returning
    the line lets the phase fail with the production reason - the coordinator's
    TARGET_NOT_FOUND named its Toshiba target and the fixture's QM00001 disk -
    instead of waiting out the marker deadline.
    """
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith(TERMINAL_REFUSAL_PREFIX) and \
                stripped.endswith(TERMINAL_REFUSAL_SUFFIX):
            return stripped
    return None


def freeze(pid):
    """Stop ``pid`` before it can run the guest's next boot.

    The process may have exited between the marker and this call; that is not
    an error for the proof.  Any other failure is reported so a mistyped pid
    cannot turn the negative scenario into a silently unfrozen run.
    """
    if pid is None:
        return 0
    try:
        os.kill(pid, signal.SIGSTOP)
    except ProcessLookupError:
        return 0
    except OSError as error:
        sys.stderr.write("cannot stop pid %d: %s\n" % (pid, error))
        return 4
    return 0


def parse_args(argv):
    """Return (positionals, stop-pid), accepting --stop-pid after the four.

    The option is removed before the positional count is checked so the old
    four-argument callers keep working unchanged.
    """
    args = list(argv[1:])
    stop_pid = None
    if "--stop-pid" in args:
        index = args.index("--stop-pid")
        if index + 1 >= len(args):
            return None, None
        try:
            stop_pid = int(args[index + 1], 10)
        except ValueError:
            return None, None
        # Zero and negative pids address whole process groups; this option is
        # only for one emulator, so refuse them instead of signalling the
        # proof's own shell or every process it can reach.
        if stop_pid <= 0:
            return None, None
        del args[index:index + 2]
    if len(args) != 4:
        return None, None
    return args, stop_pid


def main(argv):
    args, stop_pid = parse_args(argv)
    if args is None:
        sys.stderr.write(
            "usage: hfoc-qemu-marker-wait.py <serial-log> <marker> "
            "<timeout-seconds> <hfoc-serial-view.py> [--stop-pid PID]\n")
        return 2
    path, marker, timeout, view_path = args
    serial_view = load_serial_view(view_path)
    deadline = time.monotonic() + float(timeout)
    while time.monotonic() < deadline:
        text = serial_view.normalise(read_text(path))
        if marker in text:
            return freeze(stop_pid)
        if PANIC in text:
            sys.stderr.write("kernel panic while waiting for: %s\n" % marker)
            return 2
        refusal = terminal_refusal(text)
        if refusal is not None:
            sys.stderr.write(
                'HFOC-QEMU-TERMINAL-REFUSAL marker="%s" refusal="%s"\n'
                % (marker, refusal))
            return 3
        # A negative phase must freeze the vCPU before the guest's next boot.
        # The old 200 ms poll left a whole appliance boot inside the window.
        time.sleep(0.01)
    sys.stderr.write(
        'HFOC-QEMU-MARKER-TIMEOUT marker="%s" deadline=%ds\n'
        % (marker, int(float(timeout))))
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
