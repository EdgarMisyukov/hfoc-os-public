#!/usr/bin/env bash
set -Eeuo pipefail

ROOT=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd -P)
VERSION=$(<"$ROOT/VERSION")
ISO="$ROOT/build/hfoc-os-${VERSION}.iso"

[[ -f "$ISO" ]] || { echo "Сначала выполните: make iso" >&2; exit 1; }

OVMF=""
for candidate in \
    /usr/share/OVMF/OVMF_CODE_4M.fd \
    /usr/share/OVMF/OVMF_CODE.fd \
    /usr/share/ovmf/OVMF.fd; do
    if [[ -f "$candidate" ]]; then
        OVMF="$candidate"
        break
    fi
done
[[ -n "$OVMF" ]] || { echo "Не найден OVMF. Установите пакет ovmf." >&2; exit 1; }

exec qemu-system-x86_64 \
    -machine pc \
    -m 512M \
    -cpu max,-apic \
    -display gtk,grab-on-hover=on \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -cdrom "$ISO" \
    -serial stdio \
    -no-reboot
