#!/usr/bin/env bash
# ITEM-525: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# Branch run 38264136090 (2026-10-12) lost the credential-durability gate proof:
# the console prints one character at a time, an interrupt-context diagnostic
# landed inside `[PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET`, the
# proof's embedded python stripped the diagnostic and saw the record, and the
# shell greps read the raw file and reported the record missing.  This slice
# pins the fix and keeps it from sliding back:
#   * the embedded python keeps a `normalise` mode that writes the diagnostics-
#     stripped stream, and the monitor modes keep their full argument list;
#   * the shell helper renders the serial through that mode, so both halves of
#     the proof read the same stream;
#   * the durability boot's assertions - the listener, the three gate records
#     and their order, the local session, the operator warning, the
#     certificate-name record, the secret-leak check and the panic check - read
#     the normalised view, and no assertion in that boot reads the raw file;
#   * the behavioural witness and its registration exist;
#   * the runner document records the defect, its mechanism and the run.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item525_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item525_root" || exit 1

for item525_file in \
    scripts/test-qemu-production-service-https.sh \
    tests/https-gate-serial-view-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item525_file ]] || {
        echo "ITEM-525: отсутствует $item525_file" >&2
        exit 1
    }
done

item525_proof=scripts/test-qemu-production-service-https.sh

# 1. The normaliser the shell half reads is the python half's own: the mode
#    takes only the serial path, writes the stripped stream and exits, while
#    every monitor mode keeps the full argument list it had.
for item525_python in \
    'if mode == "normalise":' \
    'serial_path = pathlib.Path(sys.argv[2])' \
    'sys.stdout.write(serial_text())' \
    'raise SystemExit(0)' \
    'serial_path = pathlib.Path(sys.argv[3])'; do
    grep -Fq -- "$item525_python" "$item525_proof" || {
        echo "ITEM-525: нормализатор python не назван:" \
            "$item525_python" >&2
        exit 1
    }
done

# 2. The shell helper renders a serial file through the shared renderer whose
#    rule the helper's own python carries (ITEM-526 moved it off the embedded
#    helper: that file is written after the proof's first boot, so rendering
#    through it left the first boot's view empty), into a file the assertions
#    grep, so a reader that stops early cannot SIGPIPE the renderer.
for item525_helper in \
    'serial_view() {' \
    'python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"' \
    'printf '\''%s\n'\'' "$view"'; do
    grep -Fq -- "$item525_helper" "$item525_proof" || {
        echo "ITEM-525: оболочка больше не читает нормализованный поток:" \
            "$item525_helper" >&2
        exit 1
    }
done

# 3. The durability boot's assertions read the normalised view.
item525_views=$(grep -Fc '$(serial_view "$ACCOUNT_DURABILITY_SERIAL")' \
    "$item525_proof")
[[ $item525_views -ge 11 ]] || {
    echo "ITEM-525: утверждения опять читают сырой файл:" \
        "serial_view использован $item525_views раз" >&2
    exit 1
}
for item525_assert in \
    'grep -Eq '\''\[PRODUCTION-SERVING\] .*listener=live'\'' \' \
    'grep -Fq '\''*** HFOC KERNEL PANIC ***'\'' \' \
    'grep -Fq "$GATE_ENABLED_RECORD" \' \
    'grep -Fq '\''ADMIN SESSION: Authorization: Bearer '\'' \' \
    'grep -Fq '\''The machine will NO LONGER REQUIRE PHYSICAL PRESENCE'\'' \' \
    'grep -Fq "$CERTIFICATE_NAME_RECORD" \' \
    'grep -Fq "$ACCOUNT_NEW_SECRET" \' \
    'gate_closed_line=$(grep -n -F -m1 "$GATE_CLOSED_RECORD" \' \
    'gate_pending_line=$(grep -n -F -m1 "$GATE_PENDING_RECORD" \' \
    'gate_enabled_line=$(grep -n -F -m1 "$GATE_ENABLED_RECORD" \'; do
    grep -Fq -- "$item525_assert" "$item525_proof" || {
        echo "ITEM-525: утверждение читает не тот поток:" \
            "$item525_assert" >&2
        exit 1
    }
done

# 4. No assertion of the credential-durability boot reads the raw file.  The
#    assertion forms end the raw file with `; then` or feed it to `cut`; the
#    two failure diagnostics print it with `>&2` and are not assertions.
item525_boot=$(awk '
    /^ACCOUNT_DURABILITY_SERIAL=\$TEST_DIR\// { inside = 1 }
    inside { print }
    inside && /^wait "\$QEMU_PID"/ { exit }
' "$item525_proof")
[[ -n $item525_boot ]] || {
    echo "ITEM-525: прогон durability не найден" >&2
    exit 1
}
for item525_gone in \
    '"$ACCOUNT_DURABILITY_SERIAL"; then' \
    '"$ACCOUNT_DURABILITY_SERIAL" | cut -d: -f1)'; do
    if grep -Fq -- "$item525_gone" <<<"$item525_boot"; then
        echo "ITEM-525: утверждение вернулось к сырому файлу:" \
            "$item525_gone" >&2
        exit 1
    fi
done

# 5. The witness is registered in the canonical host suite and keeps its
#    negative control: the recorded bytes must still bite a raw reader.
for item525_witness in \
    'test-https-gate-serial-view-host:' \
    'python3 tests/https-gate-serial-view-host.py' \
    '$(MAKE) test-https-gate-serial-view-host'; do
    grep -Fq -- "$item525_witness" Makefile || {
        echo "ITEM-525: свидетель не зарегистрирован:" \
            "$item525_witness" >&2
        exit 1
    }
done
item525_host=tests/https-gate-serial-view-host.py
for item525_control in \
    'REPORTED_FAILURE = "The console enable gesture published no gate record"' \
    'b"[[IOAPIC] irq1 vector=0x41\r\n"' \
    'b"PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET\n"' \
    'RAW_HELPER' \
    'serial_view() {' \
    'GATE_CLOSED_RECORD="[PRODUCTION-SECURITY] gate=CLOSED'; do
    grep -Fq -- "$item525_control" "$item525_host" || {
        echo "ITEM-525: свидетель потерял контроль или улику:" \
            "$item525_control" >&2
        exit 1
    }
done

# 6. The runner document owns the defect, the mechanism and the run.
for item525_doc in \
    'ITEM-525' \
    '38264136090' \
    'The console enable gesture published no gate record' \
    'PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET' \
    'serial_view'; do
    grep -Fq -- "$item525_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-525:" \
            "$item525_doc" >&2
        exit 1
    }
done
