#!/usr/bin/env bash
# ITEM-527: contracted slice of the HFOC OS check suite, sourced in pinned order
#           by scripts/check.sh and lintable on its own with the shellcheck tool.
#
# ITEM-525 fixed one boot, ITEM-526 the https proof's ten boots, the
# sni-selection proof and the four acme proofs.  The same raw-grep shape - a
# shell assertion over the raw serial file while an interrupt-context
# diagnostic can land inside a record the console prints one character at a
# time - stayed in the ten remaining proofs that write a QEMU serial file.
# This slice pins their conversion:
#   * every one of them ships the shared serial_view helper (the repository
#     renderer scripts/hfoc-serial-view.py, the same rule the embedded python
#     helpers carry);
#   * every assertion of theirs reads the view; only stderr failure diagnostics
#     may print the raw stream;
#   * the witness covers them, including storage-boot's per-label logs whose
#     names do not say they are serial logs;
#   * the runner document records the extension.
#
# The pins below are literal source text, so `$(...)` must not expand and a
# trailing backslash may end a quoted pin.
# shellcheck disable=SC2016,SC1003

# The slice is sourced by scripts/check.sh (which defines ROOT and cd's there);
# standalone it locates the tree itself, so a shellcheck-only or manual run
# works the same way.
item527_root=${ROOT:-$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)}
cd "$item527_root" || exit 1

item527_proofs=(
    scripts/test-qemu-platform-power.sh
    scripts/test-qemu-irqs.sh
    scripts/test-qemu-production-first-boot.sh
    scripts/test-qemu-production-forward.sh
    scripts/test-qemu-production-recovery-reset.sh
    scripts/test-qemu-production-storage-boot.sh
    scripts/test-qemu-usb-mouse-menu.sh
    scripts/test-qemu-usb-two-controllers.sh
    scripts/test-qemu-installed-boot.sh
)

for item527_file in "${item527_proofs[@]}" \
    tests/serial-view-normalised-host.py \
    Makefile \
    docs/ci-self-hosted-runner.md; do
    [[ -f $item527_file ]] || {
        echo "ITEM-527: отсутствует $item527_file" >&2
        exit 1
    }
done

# 1. Every covered proof ships the shared helper and renders through the one
#    repository renderer.
for item527_proof in "${item527_proofs[@]}"; do
    for item527_helper in \
        'serial_view() {' \
        'python3 "$ROOT/scripts/hfoc-serial-view.py" "$1" >"$view"' \
        'printf '\''%s\n'\'' "$view"'; do
        grep -Fq -- "$item527_helper" "$item527_proof" || {
            echo "ITEM-527: $item527_proof не читает нормализованный поток:" \
                "$item527_helper" >&2
            exit 1
        }
    done
done

# 2. The assertions read the view: each proof keeps at least the number of view
#    reads it was converted to (a removal fails), and the raw forms the
#    conversion removed do not come back.  The `>&2` diagnostics stay raw.
item527_expectations=(
    'scripts/test-qemu-platform-power.sh 4'
    'scripts/test-qemu-irqs.sh 1'
    'scripts/test-qemu-production-first-boot.sh 3'
    'scripts/test-qemu-production-forward.sh 1'
    'scripts/test-qemu-production-recovery-reset.sh 5'
    'scripts/test-qemu-production-storage-boot.sh 28'
    'scripts/test-qemu-usb-mouse-menu.sh 4'
    'scripts/test-qemu-usb-two-controllers.sh 12'
    'scripts/test-qemu-installed-boot.sh 1'
)
for item527_expectation in "${item527_expectations[@]}"; do
    item527_proof=${item527_expectation% *}
    item527_minimum=${item527_expectation#* }
    item527_views=$(grep -Fo '$(serial_view "$' "$item527_proof" | wc -l)
    [[ $item527_views -ge $item527_minimum ]] || {
        echo "ITEM-527: в $item527_proof $item527_views чтений вида," \
            "ожидалось не меньше $item527_minimum" >&2
        exit 1
    }
done
for item527_gone in \
    '"$serial"; then' \
    '"$serial_log"; then' \
    '"$SERIAL_LOG"; then' \
    '"$SERIAL_A"; then' \
    '"$SERIAL_B"; then' \
    '"$usb_loss_log"; then' \
    '"$usb_second_log"; then'; do
    if grep -Fq -- "$item527_gone" "${item527_proofs[@]}"; then
        echo "ITEM-527: утверждение вернулось к сырому файлу:" \
            "$item527_gone" >&2
        exit 1
    fi
done

# 3. The witness covers the ten proofs, including storage-boot's per-label
#    logs, is registered with every proof as a dependency, and keeps the
#    no-late-helper control ITEM-526 added.
for item527_witness in \
    'scripts/test-qemu-platform-power.sh' \
    'scripts/test-qemu-usb-two-controllers.sh' \
    'EXTRA_SERIAL_NAMES' \
    'usb_loss_log' \
    'serial_view needs no late helper file'; do
    grep -Fq -- "$item527_witness" tests/serial-view-normalised-host.py || {
        echo "ITEM-527: свидетель не покрывает:" "$item527_witness" >&2
        exit 1
    }
done
for item527_dependency in \
    'scripts/test-qemu-production-storage-boot.sh \' \
    'scripts/test-qemu-usb-two-controllers.sh \' \
    'scripts/test-qemu-platform-power.sh \'; do
    grep -Fq -- "$item527_dependency" Makefile || {
        echo "ITEM-527: цель свидетеля не зависит от:" \
            "$item527_dependency" >&2
        exit 1
    }
done

# 4. The runner document records the extension.
for item527_doc in \
    'ITEM-527' \
    'storage-boot' \
    'usb-two-controllers'; do
    grep -Fq -- "$item527_doc" docs/ci-self-hosted-runner.md || {
        echo "docs/ci-self-hosted-runner.md не фиксирует ITEM-527:" \
            "$item527_doc" >&2
        exit 1
    }
done
