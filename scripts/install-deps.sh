#!/usr/bin/env bash
set -Eeuo pipefail

if [[ ${EUID} -ne 0 ]]; then
    echo "Запустите: sudo ./scripts/install-deps.sh" >&2
    exit 1
fi

apt update
apt install -y --no-install-recommends \
    build-essential gcc binutils make curl ca-certificates \
    xorriso qemu-system-x86 ovmf gdb file

echo "Зависимости HFOC OS установлены."
