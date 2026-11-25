#!/usr/bin/env bash
# ITEM-437: the boot-stack budget proof must not race its own reader.
#
# `tests/drill-stack-budget-host.sh` finds the frame instruction early in a
# proof's disassembly and stops reading there.  When that read was a pipe into
# objdump, the writer was still producing tens of kilobytes of disassembly:
# it took SIGPIPE, `set -o pipefail` made that the status of the assignment,
# and `set -e` ended the measurement.  The check therefore passed under CI
# load (a slow reader lets the writer finish) and failed on an idle host -
# backwards for a budget check that is supposed to be deterministic.
#
# This drill makes the race a fact instead of a story: a fake objdump emits the
# entry symbol the budget drill looks for and then a tail far larger than a
# pipe buffer, so a reader that closes the pipe early kills it with SIGPIPE and
# a reader that consumes the disassembly from a file cannot.  The drill then
# runs the real budget proof against those fakes and requires it to finish, and
# it proves its own negative control - the same fake writer really does die
# when it is piped into an early-exiting reader - so a regression to the piped
# shape fails here instead of on a developer's idle machine.
set -Eeuo pipefail
export LC_ALL=C
export LANG=C

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
cd "$ROOT"

for command in awk mktemp; do
    command -v "$command" >/dev/null || {
        echo "Missing command: $command" >&2
        exit 1
    }
done

TEST_DIR=$(mktemp -d "${TMPDIR:-/tmp}/hfoc-drill-race.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT

# A compiler whose only job is to leave an object file behind.
cat >"$TEST_DIR/cc" <<'FAKE_CC'
#!/usr/bin/env bash
set -Eeuo pipefail
out=
while (($#)); do
    if [[ $1 == -o ]]; then
        out=$2
        shift 2
    else
        shift
    fi
done
[[ -n $out ]] || exit 2
printf 'fake object\n' >"$out"
FAKE_CC

# A disassembler that answers with the entry symbol the budget proof looks for
# and then keeps writing.  The tail (20,000 lines, about 160 KiB) is several
# times a pipe buffer, so the write cannot all land before an early reader
# closes the pipe.
cat >"$TEST_DIR/objdump" <<'FAKE_OBJDUMP'
#!/usr/bin/env bash
set -Eeuo pipefail
printf '0000000000000000 <storage_interruption_qemu_test_init>:\n'
printf '   0:\tsub    $0x30,%%rsp\n'
for ((index = 0; index < 20000; ++index)); do
    printf '   1:\tnop\n'
done
FAKE_OBJDUMP

chmod +x "$TEST_DIR/cc" "$TEST_DIR/objdump"

# The negative control first: the fake writer must really be able to lose the
# race, or the run below would pass for the wrong reason.  What is required is
# that the writer cannot finish - `pipefail` reports the last non-zero status of
# the pipeline, and a writer killed mid-write answers 141 when the last write
# was a process and 1 when the shell itself reported the failed builtin write.
# Both are the race; a writer that exits 0 would mean this drill cannot see it.
set +e
set -o pipefail
{
    "$TEST_DIR/objdump" |
        awk '{ if ($0 ~ /sub[[:space:]]+\$0x[0-9a-f]+,%rsp/) { exit } }' \
            >/dev/null
} 2>/dev/null
early_reader_status=$?
set +o pipefail
set -e
if [[ $early_reader_status -eq 0 ]]; then
    echo "the fake writer survived an early reader (status" \
        "$early_reader_status); this drill cannot detect the race" >&2
    exit 1
fi

# The budget proof runs against the fakes.  It measures every proof in its
# list, so finishing at all is the property: with the piped shape it dies on
# the first one.
if ! PATH="$TEST_DIR:$PATH" bash tests/drill-stack-budget-host.sh \
    cc -Ikernel/include 4096 >"$TEST_DIR/budget.txt" 2>&1; then
    echo "the stack budget proof failed against a writer that cannot be" \
        "piped into an early reader" >&2
    cat "$TEST_DIR/budget.txt" >&2
    exit 1
fi
grep -Fq 'PASS: storage_interruption_qemu_test_init' "$TEST_DIR/budget.txt" || {
    echo "the budget proof did not measure the fake disassembly" >&2
    cat "$TEST_DIR/budget.txt" >&2
    exit 1
}
grep -Fq 'Boot-path proof stack budget: OK' "$TEST_DIR/budget.txt" || {
    echo "the budget proof did not finish its own verdict" >&2
    cat "$TEST_DIR/budget.txt" >&2
    exit 1
}

echo "Boot-path stack budget race host test: OK"
