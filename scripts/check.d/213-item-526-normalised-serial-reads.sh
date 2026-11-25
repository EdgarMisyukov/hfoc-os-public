#!/usr/bin/env bash
# ITEM-526: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-525 fixed one boot of the https proof; the same defect - a shell
# assertion grepping the raw serial file while an interrupt-context diagnostic
# can land inside a record the console prints one character at a time - stayed
# in the proof's other nine guest boots, in its own first boot (whose serial is
# `-serial "file:$SERIAL_LOG"`, so the name does not end in `_SERIAL`), and in
# the five proofs that had no normaliser at all.  This slice pins the class:
#   * scripts/hfoc-serial-view.py is the one normaliser, carrying the same rule
#     as the embedded python helpers of the https, forward and dhcp-dns proofs;
#   * every covered proof ships a serial_view helper that renders through a
#     normaliser into a file and prints that file (no reader can SIGPIPE the
#     renderer, ITEM-437);
#   * every assertion of those proofs reads the view; only stderr failure
#     diagnostics may print the raw stream;
#   * the behavioural witness and its registration exist in the canonical host
#     suite, and the runner document records the defect and its mechanism.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item526_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item526_root" || exit 1

for item526_file in \
    scripts/hfoc-serial-view.py \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-sni-selection.sh \
    scripts/test-qemu-acme-order.sh \
    scripts/test-qemu-acme-boot-order.sh \
    scripts/test-qemu-acme-http01.sh \
    scripts/test-qemu-acme-directory.sh \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item526_file ]] || {
        echo "ITEM-526: отсутствует $item526_file" >&2
        exit 1
    }
done

# 1. The shared renderer is the one rule: the same literal the embedded python
#    helpers carry, one normalise function and one command-line entry point.
#    ITEM-528 narrowed the literal (the broad form deleted records the proofs
#    require); this slice keeps pinning that one literal, and slice 215 pins
#    what the narrowed rule must remove and must keep.
for item526_renderer in \
    'DIAGNOSTIC_LINE = re.compile(' \
    'r"\[(?:IOAPIC|LAPIC-TIMER)\] irq[0-9]*"' \
    'r"(?: vector=0x[0-9A-Fa-f]+| lapic_eoi)[^\[\r\n]*\r?\n")' \
    'def normalise(text):' \
    'def serial_text(path):' \
    'sys.stdout.write(serial_text(pathlib.Path(argv[1])))'; do
    grep -Fq -- "$item526_renderer" scripts/hfoc-serial-view.py || {
        echo "ITEM-526: общий нормализатор потерял правило:" \
            "$item526_renderer" >&2
        exit 1
    }
done

# 2. Every covered proof ships the helper, renders through a normaliser and
#    prints the rendered file rather than the raw stream.
for item526_proof in \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-sni-selection.sh \
    scripts/test-qemu-acme-order.sh \
    scripts/test-qemu-acme-boot-order.sh \
    scripts/test-qemu-acme-http01.sh \
    scripts/test-qemu-acme-directory.sh; do
    for item526_helper in \
        'serial_view() {' \
        'local view=$1.view' \
        '>"$view"' \
        'printf '\''%s\n'\'' "$view"'; do
        grep -Fq -- "$item526_helper" "$item526_proof" || {
            echo "ITEM-526: $item526_proof больше не читает вид:" \
                "$item526_helper" >&2
            exit 1
        }
    done
done
item526_shared_proofs=$(grep -Fl -- \
    'python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"' \
    scripts/test-qemu-production-service-https.sh \
    scripts/test-qemu-production-sni-selection.sh \
    scripts/test-qemu-acme-order.sh \
    scripts/test-qemu-acme-boot-order.sh \
    scripts/test-qemu-acme-http01.sh \
    scripts/test-qemu-acme-directory.sh | wc -l)
[[ $item526_shared_proofs -eq 6 ]] || {
    echo "ITEM-526: общий вид используют $item526_shared_proofs проб из 6" >&2
    exit 1
}
# The helper must not depend on a file the proof writes later: the https proof's
# first boot runs before `$TEST_DIR/admin-enroll.py` exists, and rendering
# through that file left the view empty there (CI run 38408474798).
if grep -Fq 'admin-enroll.py" normalise' \
        scripts/test-qemu-production-service-https.sh; then
    echo "ITEM-526: вид https-пробы снова зависит от позднего файла" >&2
    exit 1
fi

# 3. The assertions read the view: every converted proof keeps at least the
#    number of view reads it was converted to (a removal fails), and the raw
#    assertion forms the conversion removed do not come back.  The `>&2`
#    diagnostics are the deliberate exception and stay raw.  (ITEM-544 moved
#    the https proof's policy-boot wait into the shared `wait_for_record`
#    helper: its two inline view reads became one call to the helper, so the
#    baseline moved from 117 to 115 and the bound still fails on any further
#    removal.  ITEM-586 moved the no-RNG/operator-seed scenario out of the
#    https proof: its eleven view reads now live in the extracted script, so
#    the https bound is 110 and the new script is pinned separately.)
item526_views_https=$(grep -Fo '$(serial_view "$' \
    scripts/test-qemu-production-service-https.sh | wc -l)
[[ $item526_views_https -ge 110 ]] || {
    echo "ITEM-526: в https-пробе $item526_views_https чтений вида, ожидалось 110" >&2
    exit 1
}
item526_expectations=(
    'scripts/test-qemu-production-service-https-noentropy.sh 11'
    'scripts/test-qemu-production-sni-selection.sh 14'
    'scripts/test-qemu-acme-order.sh 13'
    'scripts/test-qemu-acme-boot-order.sh 7'
    'scripts/test-qemu-acme-http01.sh 7'
    'scripts/test-qemu-acme-directory.sh 6'
)
for item526_expectation in "${item526_expectations[@]}"; do
    item526_proof=${item526_expectation% *}
    item526_minimum=${item526_expectation#* }
    item526_views=$(grep -Fo '$(serial_view "$' "$item526_proof" | wc -l)
    [[ $item526_views -ge $item526_minimum ]] || {
        echo "ITEM-526: в $item526_proof $item526_views чтений вида," \
            "ожидалось не меньше $item526_minimum" >&2
        exit 1
    }
done
for item526_gone in \
    '"$SERIAL_LOG"; then' \
    '"$AUTH_SERIAL"; then' \
    '"$ROUTING_SERIAL"; then' \
    '"$NETWORK_APPLY_SERIAL"; then' \
    '"$ACCOUNT_CHANGE_SERIAL"; then' \
    '"$POLICY_SERIAL"; then' \
    '"$SELECTION_SERIAL"; then' \
    '"$DURABILITY_SERIAL"; then' \
    '"$SERIAL"; then' \
    '"$SERIAL" | grep -Fq' \
    '"$serial"; then'; do
    if grep -Fq -- "$item526_gone" \
            scripts/test-qemu-production-service-https.sh \
            scripts/test-qemu-production-sni-selection.sh \
            scripts/test-qemu-acme-order.sh \
            scripts/test-qemu-acme-boot-order.sh \
            scripts/test-qemu-acme-http01.sh \
            scripts/test-qemu-acme-directory.sh; then
        echo "ITEM-526: утверждение вернулось к сырому файлу:" \
            "$item526_gone" >&2
        exit 1
    fi
done
for item526_raw_diagnostic in \
    'grep -F -e '\''MANAGEMENT GATE'\'' -e '\''REFUSED:'\'' "$serial" >&2' \
    'grep -F '\''PRODUCTION-SECURITY'\'' "$SERIAL_LOG" >&2' \
    'grep -F '\''[PRODUCTION-TLS-SNI]'\'' "$SELECTION_SERIAL" >&2' \
    'grep -F '\''[ACME-ORDER]'\'' "$SERIAL" | tail -3 >&2'; do
    grep -Fq -- "$item526_raw_diagnostic" \
        scripts/test-qemu-production-service-https.sh \
        scripts/test-qemu-production-sni-selection.sh \
        scripts/test-qemu-acme-order.sh || {
        echo "ITEM-526: сырая диагностика отказа исчезла:" \
            "$item526_raw_diagnostic" >&2
        exit 1
    }
done

# 4. The witness is registered in the canonical host suite and keeps its
#    negative control: the recorded bytes must still bite a raw reader.
for item526_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item526_witness" Makefile || {
        echo "ITEM-526: свидетель не зарегистрирован:" \
            "$item526_witness" >&2
        exit 1
    }
done
item526_host=tests/serial-view-normalised-host.py
for item526_control in \
    'PRODUCTION-SECURITY] gate=ENABLED source=LOCAL_SECRET' \
    'b"[[IOAPIC] irq1 vector=0x41\r\n"' \
    'the raw recorded bytes still hide the record' \
    'the embedded python and the shared renderer agree byte for byte' \
    'has no raw serial assertion' \
    'names the serial logs it asserts on' \
    'SERIAL_TARGET = re.compile' \
    'serial_view needs no late helper file' \
    'serial_view recovers the record'; do
    grep -Fq -- "$item526_control" "$item526_host" || {
        echo "ITEM-526: свидетель потерял контроль или улику:" \
            "$item526_control" >&2
        exit 1
    }
done

# 5. The runner document owns the defect, the class and the shared renderer.
for item526_doc in \
    'ITEM-526' \
    'hfoc-serial-view.py' \
    'has no raw serial assertion' \
    'serial_view'; do
    grep -Fq -- "$item526_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-526:" \
            "$item526_doc" >&2
        exit 1
    }
done
