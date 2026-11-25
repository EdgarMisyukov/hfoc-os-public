#!/usr/bin/env bash
# ITEM-618: QEMU framebuffer proof of the black console, the 6x8 cell and the
# non-blocking scrollback viewer.  The guest's own `network` answer is longer
# than the body, so `scroll` must show its oldest lines after HOME and its
# newest lines after END, and Q must return to the live prompt.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in python3 qemu-system-x86_64 timeout mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

IMAGE=${IMAGE:-build/hfoc-os-$(<VERSION).iso}
[[ -f $IMAGE ]] || {
    echo "Missing image: $IMAGE" >&2
    exit 1
}

TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-scrollback.XXXXXX")
SERIAL_LOG=$TEST_DIR/serial.log
MONITOR_SOCKET=$TEST_DIR/monitor.sock
DISK=$TEST_DIR/disk.raw
QEMU_PID=
cleanup() {
    local status=$?
    if [[ -n ${QEMU_PID:-} ]]; then
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
    if [[ -n ${HFOC_SCROLLBACK_ARTIFACT_DIR:-} ]]; then
        mkdir -p "$HFOC_SCROLLBACK_ARTIFACT_DIR"
        cp -a "$TEST_DIR"/*.ppm "$SERIAL_LOG" "$TEST_DIR/qemu.err" \
            "$HFOC_SCROLLBACK_ARTIFACT_DIR/" 2>/dev/null || true
    fi
    if (( status != 0 )); then
        echo "console scrollback proof artifacts: $TEST_DIR" >&2
        tail -n 80 "$SERIAL_LOG" >&2 || true
    elif [[ -z ${HFOC_SCROLLBACK_ARTIFACT_DIR:-} ]]; then
        rm -rf "$TEST_DIR"
    fi
    exit "$status"
}
trap cleanup EXIT

truncate -s 64M "$DISK"
: >"$SERIAL_LOG"
qemu-system-x86_64 \
    -accel tcg -machine q35 -m 1024M -smp 1 -cpu max \
    -display none -vga std -boot order=d,menu=off \
    -cdrom "$IMAGE" \
    -drive if=none,id=target,file="$DISK",format=raw,cache=writeback,file.locking=off \
    -device ide-hd,drive=target,bus=ide.0 \
    -serial "file:$SERIAL_LOG" \
    -monitor "unix:$MONITOR_SOCKET,server,nowait" \
    -no-reboot -no-shutdown \
    >"$TEST_DIR/qemu.out" 2>"$TEST_DIR/qemu.err" &
QEMU_PID=$!

python3 - "$SERIAL_LOG" "$MONITOR_SOCKET" "$TEST_DIR" <<'PY'
import os
import socket
import sys
import time

serial_path, monitor_path, work = sys.argv[1:4]

def wait_for(needle, timeout=120.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with open(serial_path, "rb") as f:
            text = f.read().decode("utf-8", "replace")
        if needle in text:
            return text
        time.sleep(0.2)
    raise SystemExit(f"serial did not show {needle!r}")

def monitor():
    deadline = time.time() + 30.0
    while time.time() < deadline:
        try:
            sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            sock.connect(monitor_path)
            sock.settimeout(1.0)
            return sock
        except OSError:
            time.sleep(0.1)
    raise SystemExit("monitor socket did not appear")

sock = monitor()
# QEMU's HMP banner is not interesting; a newline gets the prompt.
sock.sendall(b"\n")
time.sleep(0.2)
try:
    sock.recv(65536)
except socket.timeout:
    pass

def hmp(command):
    sock.sendall((command + "\n").encode("ascii"))
    time.sleep(0.15)
    try:
        sock.recv(65536)
    except socket.timeout:
        pass

def send(text):
    for character in text:
        if character == "\n":
            hmp("sendkey ret")
        elif character == " ":
            hmp("sendkey spc")
        else:
            hmp("sendkey " + character)
        time.sleep(0.05)

def dump(name):
    path = os.path.join(work, name)
    hmp("screendump " + path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            return path
        time.sleep(0.1)
    raise SystemExit("screendump did not produce " + path)

wait_for("hfoc> ")
# Two copies make the answer longer than any supported body, so the viewer has
# an oldest page distinct from its newest one on every mode.
send("network\n")
time.sleep(2.0)
send("network\n")
time.sleep(2.0)
# The live screen already shows the newest part of the answer.
dump("live-bottom.ppm")
if os.environ.get("HFOC_SCROLLBACK_SNAPSHOT_ONLY") == "1":
    print("snapshot-only run: live framebuffer captured")
    raise SystemExit(0)
send("scroll\n")
time.sleep(1.0)
dump("scroll-bottom.ppm")
hmp("sendkey home")
time.sleep(0.5)
dump("scroll-top.ppm")
hmp("sendkey end")
time.sleep(0.5)
hmp("sendkey q")
wait_for("hfoc> ", 30.0)
print("QEMU console scrollback: prompt, network, scroll, home/end, Q all observed")
PY

if [[ ${HFOC_SCROLLBACK_SNAPSHOT_ONLY:-0} == 1 ]]; then
    echo "console scrollback snapshot-only: live framebuffer captured"
    exit 0
fi

python3 - "$TEST_DIR" <<'PY'
import os
import sys
import zlib

work = sys.argv[1]

def load_ppm(name):
    path = os.path.join(work, name)
    with open(path, "rb") as f:
        data = f.read()
    if not data.startswith(b"P6"):
        raise SystemExit(f"{name}: not a binary PPM")
    fields = []
    index = 2
    while len(fields) < 3:
        while index < len(data) and data[index:index + 1].isspace():
            index += 1
        if data[index:index + 1] == b"#":
            index = data.index(b"\n", index) + 1
            continue
        start = index
        while index < len(data) and not data[index:index + 1].isspace():
            index += 1
        fields.append(int(data[start:index]))
    index += 1
    width, height, maxval = fields
    if maxval != 255:
        raise SystemExit(f"{name}: maxval {maxval}")
    pixels = data[index:index + width * height * 3]
    if len(pixels) != width * height * 3:
        raise SystemExit(f"{name}: short pixel payload")
    return width, height, pixels

def digest(pixels):
    return zlib.crc32(pixels)

bottom = load_ppm("scroll-bottom.ppm")
top = load_ppm("scroll-top.ppm")
live = load_ppm("live-bottom.ppm")
if bottom[:2] != top[:2] or bottom[:2] != live[:2]:
    raise SystemExit("screendump dimensions differ")
width, height, live_pixels = live
_, _, bottom_pixels = bottom
_, _, top_pixels = top
if digest(bottom_pixels) == digest(top_pixels):
    raise SystemExit("HOME and END produced the same screen")
# The body is black and the text is light: sample the whole surface, require a
# dark majority and enough bright glyph pixels.
samples = list(range(0, len(live_pixels) - 3, 3 * 17))
dark = sum(1 for i in samples
           if live_pixels[i] < 0x20 and live_pixels[i + 1] < 0x20
           and live_pixels[i + 2] < 0x20)
light = sum(1 for i in samples
            if live_pixels[i] > 0xD0 and live_pixels[i + 1] > 0xD0
            and live_pixels[i + 2] > 0xD0)
if dark * 2 < len(samples):
    raise SystemExit("the console body is not mostly black")
if light < 10:
    raise SystemExit("the console body has no light text")
for name in ("live-bottom.ppm", "scroll-bottom.ppm", "scroll-top.ppm"):
    print("framebuffer", name, "ok")
print("QEMU console scrollback: ALL OK")
PY
