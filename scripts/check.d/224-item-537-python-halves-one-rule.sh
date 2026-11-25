#!/usr/bin/env bash
# ITEM-537: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-529…536 converted the python halves of the proofs the witness covers and
# of the panic/fault, input/console, https-matrix, installer, https-client/drill,
# install-flow/physical-storage and heap/pmm/vmm groups.  But the witness's
# python-half control accepted any block whose source merely mentioned an
# interrupt prefix, so five already-covered proofs kept ITEM-526's wide rule
# (`\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n`) in their python halves:
# `test-qemu-platform-power.sh` (block 1), `test-qemu-production-storage-boot.sh`
# (block 3), `test-qemu-usb-mouse-menu.sh` (block 1),
# `test-qemu-usb-two-controllers.sh` (blocks 1, 4 and 7) and
# `test-qemu-installed-boot.sh` (block 1).  The wide rule removes console
# records the narrow rule keeps, so those halves could disagree with their own
# shell halves, which read through the shared renderer (ITEM-528).
#
# ITEM-537 makes the control honest: a python block that reads a declared serial
# log is normalised only when it receives `$(serial_view …)` or carries the one
# `load_serial_view` loader.  That turned the five drifted proofs and the
# interrupt-controller group (`test-qemu-acpi.sh`, `test-qemu-ioapic-irq3-7.sh`,
# `test-qemu-ioapic-irq8-15.sh`, `test-qemu-ioapic-probe.sh`) red - fourteen
# readers in nine proofs - and all fourteen now normalise every text read
# through the shared renderer while the byte offsets stay raw (ITEM-527).
#
# This slice pins the conversion and the control:
#   * every proof hands the shared renderer to each converted python block and
#     reads text only through the same `load_serial_view`;
#   * the wide ITEM-526 literal does not come back to the converted proofs;
#   * the raw byte offsets and the suffix rule of the ioapic-uart proofs stay;
#   * the assertions the readers make are unchanged;
#   * the witness carries the strict predicate, the proof list and the class
#     control, and the runner document records the item.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item537_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item537_root" || exit 1

item537_proofs=(
    scripts/test-qemu-acpi.sh
    scripts/test-qemu-ioapic-irq3-7.sh
    scripts/test-qemu-ioapic-irq8-15.sh
    scripts/test-qemu-ioapic-probe.sh
    scripts/test-qemu-platform-power.sh
    scripts/test-qemu-production-storage-boot.sh
    scripts/test-qemu-usb-mouse-menu.sh
    scripts/test-qemu-usb-two-controllers.sh
    scripts/test-qemu-installed-boot.sh
)
for item537_file in "${item537_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item537_file ]] || {
        echo "ITEM-537: отсутствует $item537_file" >&2
        exit 1
    }
done

# 1. The loaders, the renderer arguments and the normalised reads, counted per
#    proof (usb-two-controllers carries seven loaders: three converted here and
#    four that ITEM-529 converted).
item537_expectations=(
    'scripts/test-qemu-acpi.sh 1'
    'scripts/test-qemu-ioapic-irq3-7.sh 2'
    'scripts/test-qemu-ioapic-irq8-15.sh 2'
    'scripts/test-qemu-ioapic-probe.sh 1'
    'scripts/test-qemu-platform-power.sh 1'
    'scripts/test-qemu-production-storage-boot.sh 1'
    'scripts/test-qemu-usb-mouse-menu.sh 1'
    'scripts/test-qemu-usb-two-controllers.sh 7'
    'scripts/test-qemu-installed-boot.sh 1'
)
for item537_expectation in "${item537_expectations[@]}"; do
    item537_proof=${item537_expectation% *}
    item537_minimum=${item537_expectation#* }
    for item537_pin in \
        'def load_serial_view(path):' \
        'SERIAL_VIEW = load_serial_view(sys.argv[-1])' \
        '"$ROOT/scripts/hfoc-serial-view.py"' \
        'SERIAL_VIEW.normalise('; do
        item537_count=$(grep -Fc -- "$item537_pin" "$item537_proof" || true)
        [[ $item537_count -ge $item537_minimum ]] || {
            echo "ITEM-537: в $item537_proof $item537_count вхождений" \
                "'$item537_pin', ожидалось не меньше $item537_minimum" >&2
            exit 1
        }
    done
done

# 2. The wide ITEM-526 literal does not return to the converted proofs, and the
#    private copies of the rule are gone.
for item537_proof in "${item537_proofs[@]}"; do
    if grep -Fq -- '\[(?:IOAPIC|LAPIC|PIC|PIT|IRQ)[^\[\r\n]*\r?\n' \
        "$item537_proof"; then
        echo "ITEM-537: $item537_proof вернул широкий литерал ITEM-526" >&2
        exit 1
    fi
done
for item537_private in \
    'scripts/test-qemu-platform-power.sh:periodic_diagnostics' \
    'scripts/test-qemu-installed-boot.sh:periodic_diagnostics'; do
    item537_proof=${item537_private%%:*}
    item537_text=${item537_private#*:}
    if grep -Fq -- "$item537_text" "$item537_proof"; then
        echo "ITEM-537: $item537_proof сохранил частную копию правила:" \
            "$item537_text" >&2
        exit 1
    fi
done

# 3. The ioapic-uart proofs keep their raw byte offsets and cut the suffix by
#    raw bytes before the text is normalised.
for item537_offset in \
    'scripts/test-qemu-ioapic-irq3-7.sh:def suffix(data, offset):' \
    'scripts/test-qemu-ioapic-irq3-7.sh:str(len(serial_path.read_bytes()))' \
    'scripts/test-qemu-ioapic-irq3-7.sh:serial_suffix_text()' \
    'scripts/test-qemu-ioapic-irq8-15.sh:def suffix(data, offset):' \
    'scripts/test-qemu-ioapic-irq8-15.sh:str(len(serial_path.read_bytes()))' \
    'scripts/test-qemu-ioapic-irq8-15.sh:serial_suffix_text()'; do
    item537_proof=${item537_offset%%:*}
    item537_text=${item537_offset#*:}
    grep -Fq -- "$item537_text" "$item537_proof" || {
        echo "ITEM-537: $item537_proof потерял сырой счёт байтов:" \
            "$item537_text" >&2
        exit 1
    }
done

# 4. The assertions the readers make are unchanged, and storage-boot keeps the
#    one extra removal ITEM-463 requires: the scroll record can split the echo
#    this proof waits for, so it is still dropped after the shared normalise.
for item537_marker in \
    'scripts/test-qemu-production-storage-boot.sh:\[CONSOLE\] scroll[^\[\r\n]*\r?\n' \
    'scripts/test-qemu-acpi.sh:[ACPI] Parsed successfully; legacy PIC retained' \
    'scripts/test-qemu-acpi.sh:HFOC-QEMU-MARKER-TIMEOUT marker="ACPI success markers"' \
    'scripts/test-qemu-ioapic-irq3-7.sh:[IOAPIC] route failed: irq=' \
    'scripts/test-qemu-ioapic-irq3-7.sh:[PIC ] disabled; APIC runtime active.' \
    'scripts/test-qemu-ioapic-irq8-15.sh:[IOAPIC] route failed: irq=' \
    'scripts/test-qemu-ioapic-irq8-15.sh:[PIC ] disabled; APIC runtime active.' \
    'scripts/test-qemu-ioapic-probe.sh:master mask: 0x00000000000000ff' \
    'scripts/test-qemu-platform-power.sh:[PLATFORM-POWER] result=' \
    'scripts/test-qemu-platform-power.sh:HFOC-QEMU-MARKER-TIMEOUT' \
    'scripts/test-qemu-production-storage-boot.sh:[CONSOLE] boot paint lines=' \
    'scripts/test-qemu-usb-mouse-menu.sh:[MOUSE] state=READY reason=OK' \
    'scripts/test-qemu-usb-two-controllers.sh:USB=READY' \
    'scripts/test-qemu-installed-boot.sh:[BOOT-CHAIN] verdict=ATTESTED'; do
    item537_proof=${item537_marker%%:*}
    item537_text=${item537_marker#*:}
    grep -Fq -- "$item537_text" "$item537_proof" || {
        echo "ITEM-537: $item537_proof потерял утверждение:" \
            "$item537_text" >&2
        exit 1
    }
done

# 5. The witness carries the strict predicate, the new proofs and the class
#    control, and the loose interrupt-prefix mention test is gone.
item537_host=tests/serial-view-normalised-host.py
for item537_control in \
    'normalised = ("serial_view" in invocation or LOADER in source)' \
    '"scripts/test-qemu-acpi.sh",' \
    '"scripts/test-qemu-ioapic-irq3-7.sh",' \
    '"scripts/test-qemu-ioapic-irq8-15.sh",' \
    '"scripts/test-qemu-ioapic-probe.sh",' \
    'PYTHON_HALF_PROOFS = [' \
    'PROOFS + PYTHON_HALF_PROOFS' \
    'python halves read the normalised stream'; do
    grep -Fq -- "$item537_control" "$item537_host" || {
        echo "ITEM-537: свидетель потерял контроль:" \
            "$item537_control" >&2
        exit 1
    }
done
if grep -Fq -- 're.search(r"IOAPIC|LAPIC-TIMER", source)' "$item537_host"; then
    echo "ITEM-537: свидетель вернул нестрогую проверку упоминания префикса" >&2
    exit 1
fi
for item537_witness in \
    'test-serial-view-normalised-host:' \
    'python3 tests/serial-view-normalised-host.py' \
    '$(MAKE) test-serial-view-normalised-host'; do
    grep -Fq -- "$item537_witness" Makefile || {
        echo "ITEM-537: свидетель не зарегистрирован:" \
            "$item537_witness" >&2
        exit 1
    }
done

# 6. The runner document owns the batch.
for item537_doc in \
    'ITEM-537' \
    'one rule' \
    'interrupt-controller group'; do
    grep -Fq -- "$item537_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-537:" \
            "$item537_doc" >&2
        exit 1
    }
done
